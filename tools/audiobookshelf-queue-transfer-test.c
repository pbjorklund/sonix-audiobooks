// Focused §8.2/§8.5 tests for the ABS download queue's real transfer adapter
// (Bob spec §8.2 steps 1–4). The fake-op queue tests in
// tools/audiobookshelf-queue-test.c are untouched; this harness drives the
// REAL media surface from audiobookshelf.c (audiobookshelf_queue_media_ops()).
//
// Two shapes, built from one source:
//
//   * Offline (default): the buffered API surface is a stubbed http_request()
//     answering canned expanded-JSON, so the metadata gates are pinned without
//     HTTP: the explicit >256-track refusal (§8.2 step 2, never silent
//     truncation), cross-origin contentUrl refusal (§11.3), zero-track
//     refusal, destination path overflow / basename-column refusals before any
//     card write, and auth negatives against the in-memory profile store. The
//     stream functions are stubs that poison any probe that touches them —
//     there are no fake transfer callbacks anywhere: every transfer-shaped
//     assertion in the offline binary is a refusal that must decide BEFORE any
//     connection is opened.
//
//   * Live (-DABS_QUEUE_TRANSFER_LIVE): the real HTTP stack (http.c + tls.c,
//     no stubs at all) against tools/audiobookshelf_mock.py, once per
//     checked-in fixture alias. Phases per alias:
//       1. Direct driver: real fetch with the server-hash destination verified
//          against an independently computed SHA-256; a deterministically
//          cancelled transfer (the flag is preset, so the loop's first poll
//          stops with a retained empty .part); the restart-from-zero
//          re-transfer; byte-exact read-backs of both tracks.
//       2. Engine run: a fresh enqueue through abs_download_pump() — real auth
//          step, real fetch, real plan ingestion, real per-file transfer under
//          the queue's own commit gates — where every completed file is
//          RETAINED without one media request, ending at the honest
//          not-wired-yet finalize refusal; Retry redoes finalize only (§8.1).
//       3. Fresh engine transfer under a §8.1 mid-transfer pause: a watcher
//          thread pauses from the RAM byte counter — deterministic, because
//          the counter is monotonic per file and a poll interval of 50 µs can
//          never cover more than one 32 KiB read unit of the 16 MiB track —
//          the PAUSING ack retains the partial .part, and Resume re-downloads
//          it from byte zero to a byte-exact file.
//       4. Negatives: the episode refusal, an unresolvable profile (the auth
//          stage fails before any request), and token-leak scans over the
//          queue database, every card file and the mock's request log.
//
// Queue schema, state-machine and recovery tests remain in the fake-op
// harness; nothing here duplicates them.

#include "src/system/core/sha256.h"
#include "src/system/net/http.h"
#include "src/system/streaming/abs_download.h"
#include "src/system/streaming/abs_profiles.h"
#include "src/system/streaming/audiobookshelf.h"

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(cond, ...)                                                    \
	do {                                                                    \
		if (!(cond)) {                                                      \
			fprintf(stderr, "transfer test: CHECK failed %s:%d: %s\n",      \
					__FILE__, __LINE__, #cond);                             \
			fprintf(stderr, __VA_ARGS__);                                   \
			exit(1);                                                        \
		}                                                                   \
	} while (0)

// The sandbox root. Offline keeps everything under argv[1]; each live alias
// run points at <root>/<alias>/, prepared by
// tools/audiobookshelf-queue-transfer-smoke.sh together with the profile-store
// environment (SONIX_AUDIOBOOKSHELF_PROFILES) and the fixture mock.
static char work[900];

#define STUB_ID "stub-book"
#define STUB_SERVER "http://stub.invalid"
#define STUB_TOKEN "transfer-token-AAAA-1"
#define OTHER_TOKEN "transfer-token-BBBB-2"

// ---------------------------------------------------------------------------
// Sandbox helpers
// ---------------------------------------------------------------------------

static char card[900];

static void make_card(void) {
	snprintf(card, sizeof(card), "%.700s/card", work);
	if (mkdir(card, 0777) != 0 && errno != EEXIST) {
		CHECK(0, "cannot create %s: %s\n", card, strerror(errno));
	}
}

static void write_bytes_abs(const char *path, const char *bytes, size_t len) {
	FILE *f = fopen(path, "wb");
	CHECK(f != NULL, "cannot write %s: %s\n", path, strerror(errno));
	if (len > 0) {
		CHECK(fwrite(bytes, 1, len, f) == len, "short write %s\n", path);
	}
	CHECK(fclose(f) == 0, "close %s: %s\n", path, strerror(errno));
}

static void make_file(const char *path, size_t size, char fill) {
	char *buf = malloc(size);
	CHECK(buf != NULL, "oom for %s\n", path);
	memset(buf, fill, size);
	write_bytes_abs(path, buf, size);
	free(buf);
}

static void mkpath_abs(const char *abs) {
	char walk[1400];
	snprintf(walk, sizeof(walk), "%s", abs);
	for (char *p = walk + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			mkdir(walk, 0777);
			*p = '/';
		}
	}
	mkdir(walk, 0777);
}

static bool read_bytes_abs(const char *path, char **out, size_t *len_out) {
	*out = NULL;
	*len_out = 0;
	FILE *f = fopen(path, "rb");
	if (!f) return false;
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return false;
	}
	long size = ftell(f);
	if (size < 0) {
		fclose(f);
		return false;
	}
	rewind(f);
	char *buf = malloc((size_t)size + 1);
	CHECK(buf != NULL, "oom for %s\n", path);
	CHECK(fread(buf, 1, (size_t)size, f) == (size_t)size, "short read %s\n", path);
	fclose(f);
	buf[size] = '\0';
	*out = buf;
	*len_out = (size_t)size;
	return true;
}

static bool file_exists_abs(const char *path) {
	struct stat st;
	return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static long long file_bytes_abs(const char *path) {
	struct stat st;
	if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return -1;
	return (long long)st.st_size;
}

// Reads a work-root file (argv[1]-relative), trimmed of line endings.
static void read_root_file(const char *name, char *out, size_t size) {
	char path[1000];
	snprintf(path, sizeof(path), "%.700s/%.200s", work, name);
	FILE *f = fopen(path, "r");
	CHECK(f != NULL, "%s is missing\n", name);
	char line[ABS_SERVER_MAX + 8];
	CHECK(fgets(line, sizeof(line), f) != NULL, "%s is empty\n", name);
	fclose(f);
	size_t len = strcspn(line, "\r\n");
	CHECK(len > 0 && len < size, "%s unusable\n", name);
	memcpy(out, line, len);
	out[len] = '\0';
}

// Recursively scans every regular file under `root` for `needle`.
static bool tree_contains(const char *root, const char *needle, char *hit, size_t hit_size) {
	DIR *dir = opendir(root);
	if (!dir) return false;
	struct dirent *entry;
	bool found = false;
	while ((entry = readdir(dir)) != NULL && !found) {
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
		char path[1100];
		if ((size_t)snprintf(path, sizeof(path), "%s/%s", root, entry->d_name) >= sizeof(path)) continue;
		struct stat st;
		if (stat(path, &st) != 0) continue;
		if (S_ISDIR(st.st_mode)) {
			found = tree_contains(path, needle, hit, hit_size);
			continue;
		}
		if (!S_ISREG(st.st_mode)) continue;
		char *bytes = NULL;
		size_t len = 0;
		if (!read_bytes_abs(path, &bytes, &len)) continue;
		if (len >= strlen(needle) && memmem(bytes, len, needle, strlen(needle))) {
			found = true;
			if (hit) snprintf(hit, hit_size, "%s", path);
		}
		free(bytes);
	}
	closedir(dir);
	return found;
}

// The §11.5 destination the fetch must produce, hashed independently in this
// harness (the production hash helper is never reused here).
static void expected_destination(const char *server, const char *author, const char *title,
								 char *out_card_relative, size_t size) {
	char hex[SHA256_HEX_LEN];
	sha256_ctx ctx;
	sha256_init(&ctx);
	sha256_update(&ctx, server, strlen(server));
	sha256_final_hex(&ctx, hex);
	snprintf(out_card_relative, size, "Audiobooks/Audiobookshelf/abs-%.12s/%s/%s", hex, author, title);
}

// The absolute card path of "<destination>/<name>".
static void media_path_for(const char *card_root, const char *destination, const char *name,
						   char *out, size_t size) {
	snprintf(out, size, "%.660s/%.500s/%.260s", card_root, destination, name);
}

// ---------------------------------------------------------------------------
// Queue + ops handles shared by both shapes
// ---------------------------------------------------------------------------

static abs_download_ops_t ops_storage;
static abs_download_ops_t *ops;
static char live_server[ABS_SERVER_MAX];
static char live_profile[ABS_PROFILE_ID_MAX];

static void queue_init(void) {
	ops_storage = *audiobookshelf_queue_media_ops();
	ops = &ops_storage;
	abs_download_config_t cfg = {.sd_root = card, .ops = ops, .enable_worker_thread = false};
	CHECK(abs_download_init(&cfg), "queue init failed\n");
}

static void queue_stop(void) {
	abs_download_quiesce_storage();
}

static abs_download_job_t snapshot_job(long long id) {
	abs_download_snapshot_t snap;
	CHECK(abs_download_snapshot(&snap), "snapshot unavailable\n");
	for (int i = 0; i < snap.job_count; i++) {
		if (snap.jobs[i].id == id) return snap.jobs[i];
	}
	CHECK(0, "job %lld vanished from the snapshot\n", id);
	exit(1);
}

// Drives the engine until the job reaches a terminal state for the phase
// (paused counts as terminal for the pause phase) or the loop bound trips.
static abs_download_state_t pump_until_terminal(long long job_id) {
	for (int steps = 0; steps < 4096; steps++) {
		if (!abs_download_pump()) break;
		abs_download_job_t j = snapshot_job(job_id);
		switch (j.state) {
		case ABS_DL_DONE:
		case ABS_DL_FAILED:
		case ABS_DL_CANCELLED:
		case ABS_DL_PAUSED:
		case ABS_DL_WAITING_NETWORK:
			return j.state;
		default:
			continue;
		}
	}
	CHECK(0, "job %lld never reached a terminal state\n", job_id);
	exit(1);
}

static long long enqueue_book(const char *profile, const char *server, const char *item,
							  const char *title) {
	abs_download_enqueue_t req;
	memset(&req, 0, sizeof(req));
	req.kind = ABS_DL_KIND_BOOK;
	req.profile_id = profile;
	req.server = server;
	req.library_id = "";
	req.item_id = item;
	req.episode_id = "";
	req.title = title;
	req.destination = "";
	req.remote_progress = -1;
	long long id = 0;
	bool deduped = false;
	char error[256] = {0};
	CHECK(abs_download_enqueue_book(&req, &id, &deduped, error, sizeof(error)),
		  "enqueue failed: %s\n", error);
	CHECK(!deduped, "unexpected dedup for %s\n", item);
	return id;
}

static long long enqueue_episode(const char *profile, const char *server, const char *item,
								 const char *title, const char *destination) {
	abs_download_enqueue_t req;
	memset(&req, 0, sizeof(req));
	req.kind = ABS_DL_KIND_EPISODE;
	req.profile_id = profile;
	req.server = server;
	req.library_id = "";
	req.item_id = item;
	req.episode_id = "ep-1";
	req.title = title;
	req.destination = destination;
	req.remote_progress = -1;
	long long id = 0;
	bool deduped = false;
	char error[256] = {0};
	CHECK(abs_download_enqueue_episode(&req, &id, &deduped, error, sizeof(error)),
		  "episode enqueue failed: %s\n", error);
	return id;
}

// ---------------------------------------------------------------------------
// Offline shape
// ---------------------------------------------------------------------------

#ifndef ABS_QUEUE_TRANSFER_LIVE

// Canned expanded JSON the stub answers.
static char stub_body[262144];
static size_t stub_len;
static char stub_url[2048];
static bool stream_was_reached; // stream stubs must never run offline

static void stub_reset(void) {
	stub_len = 0;
	stub_body[0] = '\0';
	stub_url[0] = '\0';
	stream_was_reached = false;
}

static void stub_append(const char *text) {
	size_t len = strlen(text);
	CHECK(stub_len + len + 1 <= sizeof(stub_body), "stub body overflow\n");
	memcpy(stub_body + stub_len, text, len + 1);
	stub_len += len;
}

// Builds one expanded item. `sizes[i] >= 0` emits a metadata size; -1 keeps
// the field absent (the queue must treat that as unknown). `absolute_urls`
// makes every contentUrl cross-origin. `name_pad` pads the filename with 'N'
// bytes for the basename-column probe; 0 uses "Part <n>.mp3".
static void build_expanded(const char *id, const char *title, const char *author, int tracks,
						   const long long *sizes, bool absolute_urls, bool with_progress,
						   int name_pad) {
	stub_reset();
	char piece[700];
	snprintf(piece, sizeof(piece),
			 "{\"id\":\"%s\",\"media\":{\"duration\":200.0,\"coverPath\":\"/api/items/%s/cover\","
			 "\"metadata\":{\"title\":\"%s\",\"authorName\":\"%s\"},\"tracks\":[",
			 id, id, title, author);
	stub_append(piece);
	for (int i = 0; i < tracks; i++) {
		if (i) stub_append(",");
		snprintf(piece, sizeof(piece),
				 "{\"index\":%d,\"startOffset\":%d.0,\"duration\":10.0,\"title\":\"Part %d\","
				 "\"contentUrl\":\"%s/s/%s/file/%d\",\"mimeType\":\"audio/mpeg\","
				 "\"metadata\":{\"filename\":\"",
				 i + 1, i * 10, i + 1, absolute_urls ? "http://evil.invalid" : "", id, i + 1);
		stub_append(piece);
		if (name_pad > 0) {
			for (int k = 0; k < name_pad; k++) stub_append("N");
			stub_append(".mp3");
		} else {
			snprintf(piece, sizeof(piece), "Part %d.mp3", i + 1);
			stub_append(piece);
		}
		stub_append("\"");
		if (sizes && sizes[i] >= 0) {
			snprintf(piece, sizeof(piece), ",\"size\":%lld", sizes[i]);
			stub_append(piece);
		}
		stub_append("}");
		stub_append("}");
	}
	stub_append("]}"); // tracks array + media object
	if (with_progress) {
		stub_append(",\"userMediaProgress\":{\"currentTime\":13.0,\"isFinished\":false,"
					"\"lastUpdate\":1700000000000}");
	}
	stub_append("}");
}

bool http_request(const char *url, const http_req_t *req, char **out, size_t *out_len,
				  size_t limit, int timeout_secs, int *status_out) {
	(void)req;
	(void)limit;
	(void)timeout_secs;
	snprintf(stub_url, sizeof(stub_url), "%s", url ? url : "");
	if (status_out) *status_out = 200;
	char *body = malloc(stub_len + 1);
	CHECK(body != NULL, "oom for stub body\n");
	memcpy(body, stub_body, stub_len + 1);
	*out = body;
	if (out_len) *out_len = stub_len;
	return true;
}

// Every offline transfer probe must decide before any connection; a touch of
// the stream surface marks the probe broken.
bool http_stream_open_ex(http_stream_t *st, const char *url, const char *extra_headers,
						 int timeout_secs) {
	(void)st;
	(void)url;
	(void)extra_headers;
	(void)timeout_secs;
	stream_was_reached = true;
	fprintf(stderr, "transfer test: offline probe touched http_stream_open_ex\n");
	return false;
}

int http_stream_read(http_stream_t *st, void *buf, int len) {
	(void)st;
	(void)buf;
	(void)len;
	stream_was_reached = true;
	return -1;
}

void http_stream_close(http_stream_t *st) {
	(void)st;
}

const char *http_last_error(void) {
	return "offline stub: the transfer path must not open streams";
}

static abs_auth_snapshot_t held;
static volatile int offline_cancel;
static abs_download_plan_t offline_plan_out;
static char offline_error[512];
static const char *offline_stage;

// auth_acquire + fetch_identity, exactly the queue engine's calling shape.
static bool offline_fetch(void) {
	memset(&offline_plan_out, 0, sizeof(offline_plan_out));
	offline_stage = "(none)";
	offline_error[0] = '\0';
	if (!ops->auth_acquire(NULL, "p1", STUB_SERVER, &held)) {
		offline_stage = "auth";
		snprintf(offline_error, sizeof(offline_error), "auth acquire refused");
		return false;
	}
	abs_download_op_ctx_t ctx = {.job_id = 1, .state = ABS_DL_FETCHING, .cancel = &offline_cancel,
								 .user = NULL};
	abs_download_job_t job;
	memset(&job, 0, sizeof(job));
	job.id = 1;
	snprintf(job.kind, sizeof(job.kind), "%s", ABS_DL_KIND_BOOK);
	snprintf(job.profile_id, sizeof(job.profile_id), "p1");
	snprintf(job.server, sizeof(job.server), "%s", STUB_SERVER);
	snprintf(job.item_id, sizeof(job.item_id), "%s", STUB_ID);
	job.state = ABS_DL_FETCHING;
	return ops->fetch_identity(NULL, &ctx, &job, &offline_plan_out, &offline_stage, offline_error,
							   sizeof(offline_error));
}

// file_download alone: the caller owns the attempt's auth (the engine runs
// one auth per attempt, then fetch, then the file ops).
static bool offline_transfer(long long job_id, const abs_download_file_t *row, long long *size_out) {
	offline_stage = "(none)";
	offline_error[0] = '\0';
	abs_download_op_ctx_t ctx = {.job_id = job_id, .state = ABS_DL_DOWNLOADING,
								 .cancel = &offline_cancel, .user = NULL};
	return ops->file_download(NULL, &ctx, row, size_out, &offline_stage, offline_error,
							  sizeof(offline_error));
}

static void offline_run(void) {
	abs_profiles_init();
	char id_out[ABS_PROFILE_ID_MAX];
	CHECK(abs_profiles_add("Transfer A", STUB_SERVER, STUB_TOKEN, id_out, sizeof(id_out)),
		  "cannot add profile A\n");
	char id_b[ABS_PROFILE_ID_MAX];
	CHECK(abs_profiles_add("Transfer B", "http://other.invalid", OTHER_TOKEN, id_b, sizeof(id_b)),
		  "cannot add profile B\n");
	CHECK(strcmp(id_b, "p2") == 0, "unexpected second profile id %s\n", id_b);

	make_card();
	queue_init();

	// --- auth negatives: another profile's token never resolves across
	// --- servers; a refused attempt leaves zero token bytes behind.
	abs_auth_snapshot_t probe;
	memset(&probe, 0, sizeof(probe));
	char resolve_error[256];
	CHECK(!audiobookshelf_queue_auth_resolve("p2", STUB_SERVER, &probe, resolve_error,
											 sizeof(resolve_error)),
		  "p2's token must not resolve for the stub server\n");
	CHECK(probe.token[0] == '\0', "the refused snapshot leaked token bytes\n");
	CHECK(resolve_error[0] != '\0', "the refusal carries no reason\n");
	CHECK(!audiobookshelf_queue_auth_resolve("p9", STUB_SERVER, &probe, resolve_error,
											 sizeof(resolve_error)),
		  "an unknown profile must not resolve\n");
	CHECK(probe.token[0] == '\0', "the unknown-profile refusal leaked bytes\n");

	// --- explicit >256-track refusal (§8.2 step 2) ---------------------------
	long long sizes[258];
	for (int i = 0; i < 258; i++) sizes[i] = 1000;
	build_expanded(STUB_ID, "Stub Book", "Stub Author", 257, sizes, false, true, 0);
	CHECK(!offline_fetch(), "a 257-track item must be refused\n");
	CHECK(offline_stage && strcmp(offline_stage, "metadata") == 0, "stage %s\n", offline_stage);
	CHECK(strstr(offline_error, "more than 256") != NULL, "unsupported error missing: %s\n",
		  offline_error);
	CHECK(strstr(stub_url, "/api/items/stub-book?expanded=1&include=progress,authors") != NULL,
		  "the expanded identity request is missing: %s\n", stub_url);
	{
		char audiobooks[1000];
		snprintf(audiobooks, sizeof(audiobooks), "%.700s/Audiobooks", card);
		struct stat st;
		CHECK(stat(audiobooks, &st) != 0, "a refused 257-track fetch wrote to the card\n");
	}
	ops->auth_release(NULL, &held);

	// --- cross-origin contentUrl refusal (§11.3) ------------------------------
	build_expanded(STUB_ID, "Stub Book", "Stub Author", 2, NULL, true, true, 0);
	CHECK(!offline_fetch(), "cross-origin media must be refused\n");
	CHECK(strcmp(offline_stage, "metadata") == 0, "stage %s\n", offline_stage);
	CHECK(strstr(offline_error, "hosted by another server") != NULL, "origin refusal missing: %s\n",
		  offline_error);
	ops->auth_release(NULL, &held);

	// --- zero-track refusal ---------------------------------------------------
	build_expanded(STUB_ID, "Stub Book", "Stub Author", 0, NULL, false, true, 0);
	CHECK(!offline_fetch(), "a trackless item must be refused\n");
	CHECK(strstr(offline_error, "no downloadable audio tracks") != NULL,
		  "trackless refusal missing: %s\n", offline_error);
	ops->auth_release(NULL, &held);

	// --- destination path overflow before any card write (§8.2 step 3) --------
	// A safe-component author/title plus a padded basename push the per-track
	// absolute path over the shared 512-byte consumer ceiling while the
	// basename itself stays inside its column: this isolates the §11.5 path
	// gate from the column gate probed next.
	char big_author[140];
	memset(big_author, 'L', sizeof(big_author) - 1);
	big_author[sizeof(big_author) - 1] = '\0';
	char big_title[200];
	memset(big_title, 'T', sizeof(big_title) - 1);
	big_title[sizeof(big_title) - 1] = '\0';
	build_expanded(STUB_ID, big_title, big_author, 2, NULL, false, true, 240);
	CHECK(!offline_fetch(), "an overflowing destination must be refused\n");
	CHECK(strstr(offline_error, "too long") != NULL, "overflow refusal missing: %s\n", offline_error);
	ops->auth_release(NULL, &held);

	// --- oversized basename: the legacy naming clamp keeps parity -----------
	// local_track_name() bounds its component at 239 bytes ("001 - " + one
	// component + NUL can never pass the 256-byte DOWNLOAD_FILE column), so a
	// server filename longer than that is clipped exactly the way the legacy
	// 0.5.0 download named its files. The fetch must accept it with the
	// identical, byte-stable name.
	build_expanded(STUB_ID, "Stub Book", "Stub Author", 2, NULL, false, true, 250);
	CHECK(offline_fetch(), "an oversized server filename must still fetch\n");
	{
		char clamped[280] = "001 - ";
		memset(clamped + 6, 'N', 239);
		clamped[6 + 239] = '\0';
		CHECK(offline_plan_out.files[0].final_name &&
				  strcmp(offline_plan_out.files[0].final_name, clamped) == 0,
			  "clamp mismatch: %.290s\n", offline_plan_out.files[0].final_name);
	}
	ops->auth_release(NULL, &held);
	{
		char audiobooks[1000];
		snprintf(audiobooks, sizeof(audiobooks), "%.700s/Audiobooks", card);
		struct stat st;
		CHECK(stat(audiobooks, &st) != 0, "the overflow probes left card residue\n");
	}
	ops->auth_release(NULL, &held);

	// --- positive fetch: plan shape, server-hash destination ------------------
	long long pair_sizes[2] = {1200, 1200};
	build_expanded(STUB_ID, "Stub Book", "Stub Author", 2, pair_sizes, false, true, 0);
	CHECK(offline_fetch(), "the plain fetch failed: %s (%s)\n", offline_error, offline_stage);
	abs_download_plan_t *plan = &offline_plan_out;
	CHECK(plan->file_count == 2, "plan file count %d\n", plan->file_count);
	char want_dest[600];
	expected_destination(STUB_SERVER, "Stub Author", "Stub Book", want_dest, sizeof(want_dest));
	CHECK(plan->destination && strcmp(plan->destination, want_dest) == 0,
		  "destination mismatch:\n got %s\n want %s\n", plan->destination, want_dest);
	CHECK(strcmp(plan->files[0].final_name, "001 - Part 1.mp3") == 0, "name0 %s\n",
		  plan->files[0].final_name);
	CHECK(strcmp(plan->files[1].final_name, "002 - Part 2.mp3") == 0, "name1 %s\n",
		  plan->files[1].final_name);
	char url1[600];
	snprintf(url1, sizeof(url1), "%s/s/stub-book/file/1", STUB_SERVER);
	char url2[600];
	snprintf(url2, sizeof(url2), "%s/s/stub-book/file/2", STUB_SERVER);
	CHECK(strcmp(plan->files[0].source_url, url1) == 0, "url0 %s\n", plan->files[0].source_url);
	CHECK(strcmp(plan->files[1].source_url, url2) == 0, "url1 %s\n", plan->files[1].source_url);
	CHECK(plan->files[0].expected_size == 1200 && plan->files[1].expected_size == 1200,
		  "expected sizes %lld/%lld\n", plan->files[0].expected_size, plan->files[1].expected_size);
	CHECK(plan->files[0].start_offset == 0.0 && plan->files[1].start_offset == 10.0, "offsets\n");
	CHECK(plan->remote_progress == 13.0, "remote progress %f\n", plan->remote_progress);
	CHECK(!plan->remote_finished && plan->remote_updated_ms == 1700000000000, "remote stamp\n");
	{
		char audiobooks[1000];
		snprintf(audiobooks, sizeof(audiobooks), "%.700s/Audiobooks", card);
		struct stat st;
		CHECK(stat(audiobooks, &st) != 0, "the fetch touched the card before its transfer\n");
	}

	// --- transfer gates in the fetch's own attempt (job id 1) ----------------
	// The stream stubs poison any probe that ever reaches a connection: each
	// of these refusals and the retention path must decide before that.
	abs_download_file_t row1;
	memset(&row1, 0, sizeof(row1));
	row1.job_id = 1;
	row1.ordinal = 1;
	snprintf(row1.source_url, sizeof(row1.source_url), "%s", url1);
	snprintf(row1.final_name, sizeof(row1.final_name), "001 - Part 1.mp3");
	row1.expected_size = 1200;

	abs_download_file_t rowx = row1;
	snprintf(rowx.source_url, sizeof(rowx.source_url), "http://evil.invalid/x.mp3");
	long long size_out = -1;
	CHECK(!offline_transfer(row1.job_id, &rowx, &size_out),
		  "a cross-origin row must be refused by the transfer\n");
	CHECK(stream_was_reached == false, "the origin gate must decide before the stream\n");
	CHECK(strstr(offline_error, "another server") != NULL, "origin message missing: %s\n",
		  offline_error);

	// An existing final file of the exact size is retained without a stream.
	char dest_abs[1400];
	snprintf(dest_abs, sizeof(dest_abs), "%.700s/%.600s", card, want_dest);
	mkpath_abs(dest_abs);
	char final1[1400];
	snprintf(final1, sizeof(final1), "%.1100s/001 - Part 1.mp3", dest_abs);
	make_file(final1, 1200, 'k');
	size_out = -1;
	CHECK(offline_transfer(row1.job_id, &row1, &size_out),
		  "an exact existing final must be retained: %s\n", offline_error);
	CHECK(stream_was_reached == false, "retention must not open a stream\n");
	CHECK(size_out == 1200, "retained size %lld\n", size_out);

	// A differently sized existing file is a conflict, not a skip.
	char final2[1400];
	snprintf(final2, sizeof(final2), "%.1100s/002 - Part 2.mp3", dest_abs);
	make_file(final2, 601, 'j');
	abs_download_file_t row2 = row1;
	row2.ordinal = 2;
	snprintf(row2.final_name, sizeof(row2.final_name), "002 - Part 2.mp3");
	size_out = -1;
	CHECK(!offline_transfer(row1.job_id, &row2, &size_out),
		  "a wrong-size existing file must be refused\n");
	CHECK(strstr(offline_error, "differently sized") != NULL, "conflict message missing: %s\n",
		  offline_error);
	ops->auth_release(NULL, &held);

	// --- forget semantics: the attempt dies with the release ------------------
	// A fetch always re-acquires its own snapshot; a stray transfer op cannot
	// ride on a released attempt and must refuse at the auth stage.
	build_expanded(STUB_ID, "Stub Book", "Stub Author", 1, NULL, false, false, 0);
	size_out = -1;
	CHECK(!offline_transfer(row1.job_id, &row1, &size_out),
		  "a transfer after auth_release must refuse\n");
	CHECK(offline_stage && strcmp(offline_stage, "auth") == 0, "stage %s\n", offline_stage);

	queue_stop();
	printf("transfer test: offline gates passed\n");
}


// Offline main, inside the same conditional as its shape.
int main(int argc, char **argv) {
	if (argc != 2 || !argv[1] || !argv[1][0]) {
		fprintf(stderr, "usage: audiobookshelf-queue-transfer-test <workdir>\n");
		return 2;
	}
	snprintf(work, sizeof(work), "%.880s", argv[1]);
	offline_run();
	return 0;
}

#else // ABS_QUEUE_TRANSFER_LIVE
// ---------------------------------------------------------------------------
// Live shape: real HTTP against the fixture mock (one alias per run).
// The runner prepares <work>/server.txt, <work>/token.txt, <work>/media/
// (part1.mp3 16 MiB, part2.mp3 4 KiB), the request-log path, an empty card/,
// and points SONIX_AUDIOBOOKSHELF_PROFILES at a fresh profile file.
// ---------------------------------------------------------------------------

static char media_dir[1000];
static char request_log[1000];
static char mock_token[256];

// Lines of the mock's request log that contain `needle`. A missing log counts
// as zero; the first request creates it.
static long long count_log_lines_with(const char *needle) {
	FILE *f = fopen(request_log, "r");
	if (!f) return 0;
	char line[4096];
	long long hits = 0;
	while (fgets(line, sizeof(line), f)) {
		if (strstr(line, needle)) hits++;
	}
	fclose(f);
	return hits;
}

typedef struct {
	pthread_t thread;
	long long job_id;
	long long threshold;
	bool fired;
} pause_watcher_t;

// §8.1 mid-transfer pause: fires abs_download_pause() once the queue's RAM
// counter shows the active file past `threshold`. Deterministic on a 16 MiB
// track: the counter is monotonic per file, one 50 µs poll can never miss it,
// and the remaining ~15.9 MiB keeps the transfer in flight well past the
// pause call.
static void *pause_watcher_main(void *arg) {
	pause_watcher_t *w = arg;
	for (int i = 0; i < 400000 && !w->fired; i++) {
		abs_download_snapshot_t snap;
		if (abs_download_snapshot(&snap)) {
			for (int j = 0; j < snap.job_count && !w->fired; j++) {
				if (snap.jobs[j].id != w->job_id || snap.jobs[j].state != ABS_DL_DOWNLOADING) continue;
				if (snap.jobs[j].bytes_done < w->threshold) continue;
				char err[128] = {0};
				if (abs_download_pause(w->job_id, err, sizeof(err))) {
					w->fired = true;
					return NULL;
				}
				fprintf(stderr, "pause retry: %s\n", err);
			}
		}
		usleep(50);
	}
	return NULL;
}

static void live_run(void) {
	read_root_file("server.txt", live_server, sizeof(live_server));
	read_root_file("token.txt", mock_token, sizeof(mock_token));
	snprintf(media_dir, sizeof(media_dir), "%.860s/media", work);
	snprintf(request_log, sizeof(request_log), "%.700s/progress.requests", work);

	// The profile store is the runner's fresh file; the real auth surface is
	// exercised through it exactly like a device session.
	CHECK(abs_profiles_init() >= 0, "profiles init failed\n");
	CHECK(abs_profiles_add("Transfer", live_server, mock_token, live_profile,
						   sizeof(live_profile)),
		  "cannot add the live profile\n");
	CHECK(strcmp(live_profile, "p1") == 0, "unexpected profile id %s\n", live_profile);

	make_card();
	queue_init();

	// Media reference bytes (the same files the mock serves).
	char ref_path[1300];
	snprintf(ref_path, sizeof(ref_path), "%.900s/part1.mp3", media_dir);
	char *part1 = NULL;
	size_t part1_len = 0;
	CHECK(read_bytes_abs(ref_path, &part1, &part1_len), "part1.mp3 missing\n");
	snprintf(ref_path, sizeof(ref_path), "%.900s/part2.mp3", media_dir);
	char *part2 = NULL;
	size_t part2_len = 0;
	CHECK(read_bytes_abs(ref_path, &part2, &part2_len), "part2.mp3 missing\n");
	CHECK(part1_len > (1 << 20) && part2_len > 0 && part2_len < 65536,
		  "the runner must make part1 16 MiB and part2 small\n");

	char want_dest[600];
	expected_destination(live_server, "Test Author", "The Mock Journey", want_dest, sizeof(want_dest));
	char url1[ABS_DL_URL_MAX];
	snprintf(url1, sizeof(url1), "%s/api/items/mock-book/file/1", live_server);
	char url2[ABS_DL_URL_MAX];
	snprintf(url2, sizeof(url2), "%s/api/items/mock-book/file/2", live_server);
	char dest_abs[1300];
	snprintf(dest_abs, sizeof(dest_abs), "%.700s/%.500s", card, want_dest);
	char final1[1400], final2[1400], part1_file[1420], part2_file[1420];
	media_path_for(card, want_dest, "001 - Part One.mp3", final1, sizeof(final1));
	media_path_for(card, want_dest, "002 - Part Two.mp3", final2, sizeof(final2));
	snprintf(part1_file, sizeof(part1_file), "%.1300s.part", final1);
	snprintf(part2_file, sizeof(part2_file), "%.1300s.part", final2);

	// ------------------------------------------------------------------
	// Phase 1 — direct driver (§8.2 steps 1–4, engine-not-involved)
	// ------------------------------------------------------------------
	long long direct_job = enqueue_book(live_profile, live_server, "mock-book", "The Mock Journey");

	abs_auth_snapshot_t held;
	memset(&held, 0, sizeof(held));
	CHECK(ops->auth_acquire(NULL, live_profile, live_server, &held), "live auth failed\n");
	CHECK(strcmp(held.token, mock_token) == 0, "live snapshot token mismatch\n");

	abs_download_plan_t plan;
	memset(&plan, 0, sizeof(plan));
	const char *stage = "";
	char error[512];
	abs_download_job_t jobrow = snapshot_job(direct_job);
	volatile int direct_cancel = 0;
	abs_download_op_ctx_t ctx = {.job_id = direct_job, .state = ABS_DL_QUEUED,
								 .cancel = &direct_cancel, .user = NULL};
	CHECK(ops->fetch_identity(NULL, &ctx, &jobrow, &plan, &stage, error, sizeof(error)),
		  "live fetch failed: %s (%s)\n", error, stage);

	CHECK(plan.destination && strcmp(plan.destination, want_dest) == 0,
		  "live destination mismatch:\n got %s\n want %s\n", plan.destination, want_dest);
	CHECK(plan.file_count == 2, "live plan file count %d\n", plan.file_count);
	CHECK(strcmp(plan.files[0].source_url, url1) == 0, "live url0 %s\n", plan.files[0].source_url);
	CHECK(strcmp(plan.files[1].source_url, url2) == 0, "live url1 %s\n", plan.files[1].source_url);
	CHECK(strcmp(plan.files[0].final_name, "001 - Part One.mp3") == 0, "live name0 %s\n",
		  plan.files[0].final_name);
	CHECK(strcmp(plan.files[1].final_name, "002 - Part Two.mp3") == 0, "live name1 %s\n",
		  plan.files[1].final_name);
	// The fixture states no sizes for mock-book; the plan must stay unknown
	// and let Content-Length govern the exact-length check.
	CHECK(plan.files[0].expected_size == -1 && plan.files[1].expected_size == -1,
		  "expected sizes %lld/%lld must stay unknown\n", plan.files[0].expected_size,
		  plan.files[1].expected_size);
	CHECK(plan.files[0].start_offset == 0.0 && plan.files[1].start_offset == 10.0, "offsets\n");
	CHECK(plan.files[0].duration == 10.0 && plan.files[1].duration == 10.0, "durations\n");
	CHECK(plan.remote_progress > 0.0, "remote progress %f\n", plan.remote_progress);
	CHECK(!plan.remote_finished, "remote finished\n");

	abs_download_file_t row1;
	memset(&row1, 0, sizeof(row1));
	row1.job_id = direct_job;
	row1.ordinal = 1;
	snprintf(row1.source_url, sizeof(row1.source_url), "%s", plan.files[0].source_url);
	snprintf(row1.final_name, sizeof(row1.final_name), "%s", plan.files[0].final_name);
	row1.expected_size = plan.files[0].expected_size;
	abs_download_file_t row2 = row1;
	row2.ordinal = 2;
	snprintf(row2.source_url, sizeof(row2.source_url), "%s", plan.files[1].source_url);
	snprintf(row2.final_name, sizeof(row2.final_name), "%s", plan.files[1].final_name);
	row2.expected_size = plan.files[1].expected_size;

	// A preset flag stops the loop at its first poll unit: the op must honour
	// an externally set cancel between units, retain the .part, and commit
	// nothing.
	direct_cancel = 1;
	long long size_out = -1;
	CHECK(!ops->file_download(NULL, &ctx, &row1, &size_out, &stage, error, sizeof(error)),
		  "the preset cancel must stop the transfer\n");
	CHECK(file_exists_abs(part1_file), "the stopped transfer must retain its .part (§8.1 pause)\n");
	CHECK(file_bytes_abs(part1_file) == 0, "a first-unit stop leaves an empty .part: %lld\n",
		  file_bytes_abs(part1_file));
	CHECK(!file_exists_abs(final1), "no final file may appear for a cancelled transfer\n");
	direct_cancel = 0;

	// The next attempt truncates the orphaned .part from byte zero (§8.2) and
	// completes for real.
	CHECK(ops->file_download(NULL, &ctx, &row1, &size_out, &stage, error, sizeof(error)),
		  "the restarted transfer failed: %s (%s)\n", error, stage);
	CHECK(size_out == (long long)part1_len, "final size %lld wants %zu\n", size_out, part1_len);
	CHECK(!file_exists_abs(part1_file), "the completed .part must be gone\n");
	{
		char *got = NULL;
		size_t got_len = 0;
		CHECK(read_bytes_abs(final1, &got, &got_len), "cannot read back %s\n", final1);
		CHECK(got_len == part1_len && memcmp(got, part1, got_len) == 0, "part1 bytes differ\n");
		free(got);
	}

	CHECK(ops->file_download(NULL, &ctx, &row2, &size_out, &stage, error, sizeof(error)),
		  "the second transfer failed: %s (%s)\n", error, stage);
	{
		char *got = NULL;
		size_t got_len = 0;
		CHECK(read_bytes_abs(final2, &got, &got_len), "cannot read back %s\n", final2);
		CHECK(got_len == part2_len && memcmp(got, part2, got_len) == 0, "part2 bytes differ\n");
		free(got);
	}

	// Cross-origin refusal before ANY connection: localhost is a different
	// origin from 127.0.0.1 even though the mock would answer it.
	{
		const char *host = strstr(live_server, "127.0.0.1");
		CHECK(host != NULL, "the runner must address the mock as 127.0.0.1\n");
		char cross_origin[ABS_DL_URL_MAX];
		snprintf(cross_origin, sizeof(cross_origin), "http://localhost:%s", host + strlen("127.0.0.1"));
		size_t path_start = strlen(cross_origin);
		snprintf(cross_origin + path_start, sizeof(cross_origin) - path_start,
				 "/api/items/mock-book/file/1");
		abs_download_file_t rowx = row1;
		snprintf(rowx.source_url, sizeof(rowx.source_url), "%s", cross_origin);
		long long media1_before = count_log_lines_with("/api/items/mock-book/file/1");
		CHECK(!ops->file_download(NULL, &ctx, &rowx, &size_out, &stage, error, sizeof(error)),
			  "a cross-origin transfer must be refused\n");
		CHECK(strstr(error, "another server") != NULL, "origin message missing: %s\n", error);
		CHECK(count_log_lines_with("/api/items/mock-book/file/1") == media1_before,
			  "the origin refusal must not touch the socket\n");
	}
	ops->auth_release(NULL, &held);

	// After the release the attempt is gone: the next fetch refuses at auth.
	{
		abs_download_plan_t stale_plan;
		memset(&stale_plan, 0, sizeof(stale_plan));
		const char *stale_stage = "";
		CHECK(!ops->fetch_identity(NULL, &ctx, &jobrow, &stale_plan, &stale_stage, error,
								   sizeof(error)),
			  "a fetch without a held snapshot must refuse\n");
		CHECK(stale_stage && strcmp(stale_stage, "auth") == 0, "stage %s\n", stale_stage);
	}
	CHECK(abs_download_remove(direct_job, error, sizeof(error)),
		  "cannot remove the direct-flow row: %s\n", error);

	// ------------------------------------------------------------------
	// Phase 2 — engine run: retention, honest finalize, finalize-only retry
	// ------------------------------------------------------------------
	long long items_probe = count_log_lines_with("expanded=1&include=progress,authors");
	long long media1_probe = count_log_lines_with("/api/items/mock-book/file/1");
	long long media2_probe = count_log_lines_with("/api/items/mock-book/file/2");
	CHECK(items_probe == 1 && media1_probe == 2 && media2_probe == 1,
		  "unexpected baseline: items %lld media1 %lld media2 %lld\n", items_probe, media1_probe,
		  media2_probe);

	long long job2 = enqueue_book(live_profile, live_server, "mock-book", "The Mock Journey");
	abs_download_state_t state2 = pump_until_terminal(job2);
	CHECK(state2 == ABS_DL_FAILED, "phase 2 state %s\n", abs_download_state_name(state2));
	abs_download_job_t seen2 = snapshot_job(job2);
	CHECK(strcmp(seen2.error_stage, "manifest") == 0, "stage %s\n", seen2.error_stage);
	CHECK(strstr(seen2.error, "later wave") != NULL, "not-wired finalize message: %s\n", seen2.error);
	CHECK(seen2.file_count == 2, "file count %d\n", seen2.file_count);

	{
		char *got = NULL;
		size_t got_len = 0;
		CHECK(read_bytes_abs(final1, &got, &got_len), "cannot read back %s\n", final1);
		CHECK(got_len == part1_len && memcmp(got, part1, got_len) == 0, "part1 bytes differ\n");
		free(got);
		got = NULL;
		CHECK(read_bytes_abs(final2, &got, &got_len), "cannot read back %s\n", final2);
		CHECK(got_len == part2_len && memcmp(got, part2, got_len) == 0, "part2 bytes differ\n");
		free(got);
	}
	{
		abs_download_file_t rows[8];
		int n = abs_download_job_files(job2, rows, 8);
		CHECK(n == 2, "rows %d\n", n);
		CHECK(strcmp(rows[0].state, "complete") == 0 && strcmp(rows[1].state, "complete") == 0,
			  "row states %s/%s\n", rows[0].state, rows[1].state);
		CHECK(rows[0].final_size == (long long)part1_len && rows[1].final_size == (long long)part2_len,
			  "committed sizes %lld/%lld\n", rows[0].final_size, rows[1].final_size);
		CHECK(strcmp(rows[0].source_url, url1) == 0 && strcmp(rows[1].source_url, url2) == 0,
			  "row urls %s / %s\n", rows[0].source_url, rows[1].source_url);
	}
	CHECK(count_log_lines_with("/api/items/mock-book/file/1") == media1_probe,
		  "retention must not re-fetch file 1\n");
	CHECK(count_log_lines_with("/api/items/mock-book/file/2") == media2_probe,
		  "retention must not re-fetch file 2\n");
	CHECK(count_log_lines_with("/api/items/mock-book?expanded") == items_probe + 1,
		  "the engine fetch is one items request, got %lld\n",
		  count_log_lines_with("/api/items/mock-book?expanded"));

	// Retry of a fully committed job redoes finalize only (§8.1): one more
	// identity fetch, zero media requests, the same honest refusal.
	char retry_error[256] = {0};
	CHECK(abs_download_retry(job2, retry_error, sizeof(retry_error)), "retry failed: %s\n",
		  retry_error);
	CHECK(pump_until_terminal(job2) == ABS_DL_FAILED, "retry state\n");
	CHECK(count_log_lines_with("/api/items/mock-book/file/1") == media1_probe,
		  "retry re-fetched file 1\n");
	CHECK(count_log_lines_with("/api/items/mock-book/file/2") == media2_probe,
		  "retry re-fetched file 2\n");
	CHECK(count_log_lines_with("/api/items/mock-book?expanded") == items_probe + 2,
		  "retry fetch missing, got %lld\n", count_log_lines_with("/api/items/mock-book?expanded"));

	// ------------------------------------------------------------------
	// Phase 3 — fresh engine transfer under a mid-transfer §8.1 pause
	// ------------------------------------------------------------------
	CHECK(unlink(final1) == 0 && unlink(final2) == 0, "cannot clear the completed files\n");
	long long job3 = enqueue_book(live_profile, live_server, "mock-book", "The Mock Journey");
	pause_watcher_t watcher = {.job_id = job3, .threshold = 128 * 1024, .fired = false};
	CHECK(pthread_create(&watcher.thread, NULL, pause_watcher_main, &watcher) == 0, "watcher\n");
	abs_download_state_t state3 = pump_until_terminal(job3);
	CHECK(watcher.fired, "the watcher must fire while the 16 MiB track transfers\n");
	CHECK(state3 == ABS_DL_PAUSED, "phase 3 state %s\n", abs_download_state_name(state3));
	pthread_join(watcher.thread, NULL);
	abs_download_job_t seen3 = snapshot_job(job3);
	CHECK(seen3.file_index == 0, "the pause must be on file 1, index %d\n", seen3.file_index);
	CHECK(file_exists_abs(part1_file), "the paused .part must be retained\n");
	long long paused_part = file_bytes_abs(part1_file);
	CHECK(paused_part > 0 && paused_part < (long long)part1_len, "partial .part %lld\n",
		  paused_part);
	CHECK(!file_exists_abs(final1), "no final file for a paused first file\n");

	retry_error[0] = '\0';
	CHECK(abs_download_resume(job3, retry_error, sizeof(retry_error)), "resume failed: %s\n",
		  retry_error);
	CHECK(pump_until_terminal(job3) == ABS_DL_FAILED, "resumed state\n");
	{
		char *got = NULL;
		size_t got_len = 0;
		CHECK(read_bytes_abs(final1, &got, &got_len), "cannot read back %s\n", final1);
		CHECK(got_len == part1_len && memcmp(got, part1, got_len) == 0,
			  "the redownloaded part1 differs (restart-from-zero)\n");
		free(got);
		got = NULL;
		CHECK(read_bytes_abs(final2, &got, &got_len), "cannot read back %s\n", final2);
		CHECK(got_len == part2_len && memcmp(got, part2, got_len) == 0, "part2 bytes differ\n");
		free(got);
	}
	CHECK(!file_exists_abs(part1_file), "the completed .part must be gone\n");
	// Wait: the final .part policy — file 2 completed cleanly too.
	CHECK(count_log_lines_with("/api/items/mock-book/file/1") == media1_probe + 2,
		  "file 1 must be transferred twice here (interrupted + restart), got %lld\n",
		  count_log_lines_with("/api/items/mock-book/file/1"));
	CHECK(count_log_lines_with("/api/items/mock-book/file/2") == media2_probe + 1,
		  "file 2 once, got %lld\n", count_log_lines_with("/api/items/mock-book/file/2"));
	CHECK(count_log_lines_with("/api/items/mock-book?expanded") == items_probe + 4,
		  "identity fetches: job3 + resume, got %lld\n",
		  count_log_lines_with("/api/items/mock-book?expanded"));

	// ------------------------------------------------------------------
	// Phase 4 — negatives
	// ------------------------------------------------------------------
	// An episode job is refused by the adapter until the podcast wave.
	long long ep_job = enqueue_episode(live_profile, live_server, "pod-41",
									   "Fixture Forty-One Podcast", "");
	CHECK(pump_until_terminal(ep_job) == ABS_DL_FAILED, "episode state\n");
	abs_download_job_t seen_ep = snapshot_job(ep_job);
	CHECK(strcmp(seen_ep.error_stage, "metadata") == 0, "stage %s\n", seen_ep.error_stage);
	CHECK(strstr(seen_ep.error, "not wired yet") != NULL, "episode refusal: %s\n", seen_ep.error);
	CHECK(count_log_lines_with("expanded=1&include=progress,authors") == items_probe + 4,
		  "the episode refusal must not fetch anything, got %lld\n",
		  count_log_lines_with("expanded=1&include=progress,authors"));

	// An unresolvable profile fails at the auth stage before any request.
	long long auth_job = enqueue_book("p9", live_server, "pod-41", "Fixture Forty-One Podcast");
	CHECK(pump_until_terminal(auth_job) == ABS_DL_FAILED, "auth-failure state\n");
	abs_download_job_t seen_auth = snapshot_job(auth_job);
	CHECK(strcmp(seen_auth.error_stage, "auth") == 0, "stage %s\n", seen_auth.error_stage);
	CHECK(count_log_lines_with("expanded=1&include=progress,authors") == items_probe + 4,
		  "a refused auth must not fetch, got %lld\n",
		  count_log_lines_with("expanded=1&include=progress,authors"));

	// ------------------------------------------------------------------
	// Token-leak scans: card tree, queue database, mock request log
	// ------------------------------------------------------------------
	{
		char hit[500];
		CHECK(!tree_contains(card, mock_token, hit, sizeof(hit)),
			  "the token leaked into the card: %s\n", hit);
		CHECK(!tree_contains(card, "Bearer ", hit, sizeof(hit)),
			  "a Bearer fragment leaked into the card: %s\n", hit);
		CHECK(!tree_contains(card, "token=", hit, sizeof(hit)),
			  "a credential-bearing URL leaked: %s\n", hit);
		char *log_bytes = NULL;
		size_t log_len = 0;
		CHECK(read_bytes_abs(request_log, &log_bytes, &log_len), "request log missing\n");
		CHECK(!memmem(log_bytes, log_len, mock_token, strlen(mock_token)),
			  "the token leaked into the request log\n");
		CHECK(!memmem(log_bytes, log_len, "Authorization", 13),
			  "an Authorization header leaked into the request log\n");
		free(log_bytes);
	}

	free(part1);
	free(part2);
	queue_stop();
	printf("transfer test: live alias passed\n");
}

int main(int argc, char **argv) {
	if (argc != 2 || !argv[1] || !argv[1][0]) {
		fprintf(stderr, "usage: audiobookshelf-queue-transfer-test <workdir>\n");
		return 2;
	}
	snprintf(work, sizeof(work), "%.880s", argv[1]);
	live_run();
	return 0;
}
#endif // ABS_QUEUE_TRANSFER_LIVE
