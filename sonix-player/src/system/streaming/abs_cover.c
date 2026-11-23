#include "abs_cover.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h> // AT_FDCWD
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "abs_download.h" // abs_download_op_cancelled() polling helper (§8.5)
#include "src/system/core/sha256.h"
#include "src/system/net/http.h"

// ---------------------------------------------------------------------------
// Authenticated cover download + LRU cache (§9.1/§9.3).
//
// One fetch runs at a time in practice (the detail job is the only caller and
// it runs on the browse page's worker), but the cache bookkeeping is still
// mutex-guarded: pruning reads the card directory, and set_root()/quiesce()
// arrive from other threads (boot, card removal).
//
// The cache file name is `<server-hash>-<item-hash>.jpg` -- twelve lowercase
// hex characters of the SHA-256 over the normalized server (the identity the
// download-directory prefix derives, §11.5) and over the item id. A token can
// therefore appear in no name whatever a caller hands in, and two servers
// carrying the same item id never share a file.
// ---------------------------------------------------------------------------

#define ABS_COVER_HASH_LEN 12
#define ABS_COVER_NAME_MAX 64
#define ABS_COVER_ROOT_MAX 512
#define ABS_COVER_FILE_PATH_MAX (ABS_COVER_ROOT_MAX + ABS_COVER_NAME_MAX + 2)
#define ABS_COVER_URL_MAX 1200
#define ABS_COVER_ENTRY_MAX 512 // directory scan cap; a larger cache is still pruned
#define ABS_COVER_CHUNK 16384   // stream read unit; the 2 MiB cap is 128 units

static pthread_mutex_t cover_lock = PTHREAD_MUTEX_INITIALIZER;
static char cover_root[ABS_COVER_ROOT_MAX] = "";
static bool cover_quiesced;

// Twelve hex characters of SHA-256 over `data` — what audiobookshelf.c's
// abs_book_hash_prefix() derives for the download directory too, so the cover
// cache and the media tree spell one server one way.
static void cover_hash_prefix(const char *data, char *out, size_t size) {
	char hex[SHA256_HEX_LEN];
	sha256_ctx ctx;
	sha256_init(&ctx);
	sha256_update(&ctx, data, strlen(data));
	sha256_final_hex(&ctx, hex);
	snprintf(out, size, "%.*s", ABS_COVER_HASH_LEN, hex);
}

// Marks `path` used right now, explicitly: the SD mount may be carried with
// noatime or relatime, so the LRU cannot lean on kernel read accounting.
// Both fetch completion and a cache hit touch their file, which is what makes
// the eviction order follow real usage.
static void cover_touch(const char *path) {
	struct timespec both[2] = {{0, UTIME_NOW}, {0, UTIME_NOW}};
	utimensat(AT_FDCWD, path, both, 0);
}

// <card>/.local exists before the cache's own directory; a plain mkdir would
// fail on a card that never hosted a cache yet.
static bool cover_make_dirs(const char *root) {
	char path[ABS_COVER_FILE_PATH_MAX];
	const size_t root_len = strlen(root);
	if (root_len == 0 || root_len + sizeof(ABS_COVER_CACHE_DIR) + 1 >= sizeof(path)) return false;

	memcpy(path, root, root_len);
	path[root_len] = '\0';
	size_t cut = root_len;
	for (const char *rest = ABS_COVER_CACHE_DIR;;) {
		path[cut++] = '/';
		const char *slash = strchr(rest, '/');
		size_t part = slash ? (size_t)(slash - rest) : strlen(rest);
		if (cut + part + 1 >= sizeof(path)) return false;
		memcpy(path + cut, rest, part);
		cut += part;
		path[cut] = '\0';
		if (mkdir(path, 0777) != 0 && errno != EEXIST) {
			fprintf(stderr, "abs-cover: cannot create %s: %s\n", path, strerror(errno));
			return false;
		}
		if (!slash) return true;
		rest = slash + 1;
	}
}

// ---------------------------------------------------------------------------
// Measuring and pruning: the §9.3 ceilings, eight files / 16 MiB.
// ---------------------------------------------------------------------------

typedef struct {
	char name[ABS_COVER_NAME_MAX];
	long long size;
	long long atime; // the cover_touch() stamp, in nanoseconds
} cover_entry_t;

// One directory snapshot; `total` is the sum over every regular file in the
// directory (sampled or not), so the bytes ceiling holds even past the scan
// cap.
static int cover_scan(const char *dir, cover_entry_t *out, int max, long long *total_out) {
	long long total = 0;
	int count = 0;

	DIR *d = opendir(dir);
	if (!d) {
		if (total_out) *total_out = 0;
		return 0;
	}

	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		if (e->d_name[0] == '.') continue;
		struct stat st;
		if (fstatat(dirfd(d), e->d_name, &st, 0) != 0 || !S_ISREG(st.st_mode)) continue;
		total += st.st_size;
		if (out && count < max && strlen(e->d_name) < sizeof(out[0].name)) {
			// The length guard above keeps this copy exact; the precision
			// only repeats it where the compiler can see it.
			snprintf(out[count].name, sizeof(out[0].name), "%.63s", e->d_name);
			out[count].size = (long long)st.st_size;
			// Nanosecond stamp: fetches land inside one another's second, so
			// whole seconds alone would leave the eviction guessing.
			out[count].atime = (long long)st.st_atim.tv_sec * 1000000000ll + (long long)st.st_atim.tv_nsec;
			count++;
		}
	}
	closedir(d);
	if (total_out) *total_out = total;
	return count;
}

// Drops least-recently-used entries until both ceilings hold again: no more
// than ABS_COVER_FILES_MAX files and at most ABS_COVER_BYTES_MAX bytes.
// `fresh_name` is protected: the eviction never undid the fetch that is about
// to hand its path back. Incomplete ".part" remains from a crash count
// toward the ceilings too and leave by the same rule.
//
// One snapshot, evictions against it: removing from the card and re-scanning
// each round would spin forever against a directory that refuses a removal,
// while a compacted-out candidate costs at most a slightly conservative
// eviction until the next fetch's snapshot.
static void cover_prune(const char *dir, const char *fresh_name) {
	cover_entry_t *entries = malloc(sizeof(*entries) * ABS_COVER_ENTRY_MAX);
	if (!entries) return; // nothing deleted this pass; the next fetch retries

	long long total = 0;
	int count = cover_scan(dir, entries, ABS_COVER_ENTRY_MAX, &total);
	while (count > 0 && (total > (long long)ABS_COVER_BYTES_MAX || count > ABS_COVER_FILES_MAX)) {
		int oldest = -1;
		for (int k = 0; k < count; k++) {
			if (strcmp(entries[k].name, fresh_name) == 0) continue;
			if (oldest < 0 || entries[k].atime < entries[oldest].atime) oldest = k;
		}
		if (oldest < 0) break; // nothing but the fresh file is left

		char path[ABS_COVER_FILE_PATH_MAX];
		if ((size_t)snprintf(path, sizeof(path), "%s/%s", dir, entries[oldest].name) < sizeof(path) &&
			remove(path) == 0) {
			total -= entries[oldest].size;
			fprintf(stderr, "abs-cover: evicted %s (%lld KiB)\n", entries[oldest].name,
					entries[oldest].size / 1024);
		}
		// Compacted out either way: a removal the card refused is not retried
		// against the same snapshot, so the loop can only ever touch each
		// candidate once and then stops.
		entries[oldest] = entries[count - 1];
		count--;
	}
	free(entries);
}

// ---------------------------------------------------------------------------
// The fetch
// ---------------------------------------------------------------------------

void abs_cover_set_root(const char *card_root) {
	pthread_mutex_lock(&cover_lock);
	snprintf(cover_root, sizeof(cover_root), "%s", card_root ? card_root : "");
	cover_quiesced = false;
	pthread_mutex_unlock(&cover_lock);
}

void abs_cover_quiesce(void) {
	pthread_mutex_lock(&cover_lock);
	cover_quiesced = true;
	pthread_mutex_unlock(&cover_lock);
}

bool abs_cover_fetch(const abs_auth_snapshot_t *auth, const char *item_id,
					 const volatile int *cancel, char *path_out, size_t path_size,
					 char *error, size_t error_size) {
	if (path_out && path_size) path_out[0] = '\0';
	if (error && error_size) error[0] = '\0';
#define COVER_FAIL(message)                                                  \
	do {                                                                     \
		if (error && error_size) snprintf(error, error_size, "%s", message); \
		return false;                                                        \
	} while (0)

	if (!auth || !item_id || !item_id[0] || !path_out || path_size < ABS_COVER_NAME_MAX)
		COVER_FAIL("The cover request was incomplete.");

	char root[ABS_COVER_ROOT_MAX];
	pthread_mutex_lock(&cover_lock);
	snprintf(root, sizeof(root), "%s", cover_root);
	bool quiesced = cover_quiesced || !cover_root[0];
	pthread_mutex_unlock(&cover_lock);
	if (quiesced)
		COVER_FAIL("The card is not available for the cover cache.");
	if (!auth->server[0] || !auth->token[0])
		COVER_FAIL("The cover has no login to use.");

	// The §11.5 identity: the download directory's hash and this cache's hash
	// spell the same normalized server, so one item on two servers shares
	// neither media directory nor cover file.
	char normalized[ABS_SERVER_MAX];
	abs_profiles_normalize_server(auth->server, normalized, sizeof(normalized));
	if (!normalized[0])
		COVER_FAIL("The cover's server address is unusable.");
	char server_hash[ABS_COVER_HASH_LEN + 1], item_hash[ABS_COVER_HASH_LEN + 1];
	cover_hash_prefix(normalized, server_hash, sizeof(server_hash));
	cover_hash_prefix(item_id, item_hash, sizeof(item_hash));
	char name[ABS_COVER_NAME_MAX];
	snprintf(name, sizeof(name), "%s-%s.jpg", server_hash, item_hash);

	char dir[ABS_COVER_FILE_PATH_MAX];
	char final_path[ABS_COVER_FILE_PATH_MAX];
	if (!cover_make_dirs(root) ||
		(size_t)snprintf(dir, sizeof(dir), "%s/" ABS_COVER_CACHE_DIR, root) >= sizeof(dir) ||
		(size_t)snprintf(final_path, sizeof(final_path), "%s/%s", dir, name) >= sizeof(final_path) ||
		strlen(final_path) >= ABS_COVER_PATH_MAX)
		COVER_FAIL("The cover cache path does not fit the card.");

	struct stat existing;
	if (stat(final_path, &existing) == 0 && S_ISREG(existing.st_mode) && existing.st_size > 0) {
		cover_touch(final_path); // LRU: a view is a use
		snprintf(path_out, path_size, "%s", final_path);
		return true;
	}

	// The request is built on the snapshot's own server, so it is same-origin
	// by construction; http.c drops the Authorization header across a
	// cross-origin redirect hop (§11.3: ABS may hop covers to object storage,
	// and the token never follows it there). Item ids follow the queue's
	// ABS_DL_ID_MAX bound; anything longer is a refusal, never a truncation.
	char id_encoded[ABS_DL_ID_MAX * 3];
	if (strlen(item_id) >= ABS_DL_ID_MAX)
		COVER_FAIL("The cover's item reference is too long.");
	http_url_encode(item_id, id_encoded, sizeof(id_encoded));
	char url[ABS_COVER_URL_MAX];
	if ((size_t)snprintf(url, sizeof(url), "%s/api/items/%s/cover?format=jpeg&width=900&height=1280",
						 normalized, id_encoded) >= sizeof(url))
		COVER_FAIL("The cover request address is too long.");

	char headers[ABS_TOKEN_MAX + 32];
	if ((size_t)snprintf(headers, sizeof(headers), "Authorization: Bearer %s\r\n", auth->token) >=
		sizeof(headers))
		COVER_FAIL("The cover's login does not fit the request.");

	http_stream_t stream;
	if (!http_stream_open_ex(&stream, url, headers, ABS_COVER_TIMEOUT_SECS))
		COVER_FAIL("The cover could not be downloaded.");
	if (stream.content_length > ABS_COVER_ENCODED_MAX) {
		// Refuse before a byte lands: a stated-oversize response never even
		// opens a .part file (§9.1's 2 MiB cap).
		http_stream_close(&stream);
		COVER_FAIL("The cover is too large to keep on this device.");
	}

	char part_path[ABS_COVER_FILE_PATH_MAX + 5];
	if ((size_t)snprintf(part_path, sizeof(part_path), "%s.part", final_path) >= sizeof(part_path)) {
		http_stream_close(&stream);
		COVER_FAIL("The cover cache path does not fit the card.");
	}
	FILE *f = fopen(part_path, "wb");
	if (!f) {
		http_stream_close(&stream);
		COVER_FAIL("The cover could not be written to the card.");
	}

	char buffer[ABS_COVER_CHUNK];
	long long done = 0;
	bool ok = true;
	bool cancelled = false;
	for (;;) {
		if (abs_download_op_cancelled(cancel)) {
			cancelled = true;
			ok = false;
		}
		if (!ok) break;
		int n = http_stream_read(&stream, buffer, sizeof(buffer));
		if (n == 0) break; // end of body
		if (n < 0) {
			ok = false;
			break;
		}
		if (done + n > ABS_COVER_ENCODED_MAX) {
			// An unstated length that grows past the cap stops here, with
			// nothing over the cap ever reaching the card.
			ok = false;
			break;
		}
		if (fwrite(buffer, 1, (size_t)n, f) != (size_t)n) {
			ok = false;
			break;
		}
		done += n;
	}
	if (fclose(f) != 0) ok = false;
	http_stream_close(&stream);
	if (ok && stream.content_length > 0 && done != stream.content_length) ok = false;
	if (ok && done == 0) ok = false; // an empty answer is not a cover
	if (ok && rename(part_path, final_path) != 0) ok = false;
	if (!ok) {
		remove(part_path);
		COVER_FAIL(cancelled ? "The cover download was cancelled."
							 : "The cover download was interrupted.");
	}

	cover_touch(final_path); // a fetch is a use
	cover_prune(dir, name);
	snprintf(path_out, path_size, "%s", final_path);
	return true;
#undef COVER_FAIL
}
