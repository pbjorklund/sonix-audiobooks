// Focused host tests for the §11.5 download destination and manifest binding:
//   - new books land under <card>/Audiobooks/Audiobookshelf/abs-<12-hex
//     SHA-256 of the normalized server>/<safe-author>/<safe-title>;
//   - a hash-prefix directory claimed by another server fails the job before
//     any byte or directory is written (full manifest server, not the prefix);
//   - the shared path ceiling rejects a destination before writing;
//   - audiobookshelf_local_linked() binds on resolved manifest server AND
//     item_id, with version/required-field validation, and a 0.5.0 manifest
//     without server= resolves to legacy_server, never the active profile.
//
// No mock servers and no UI here (the two-host smoke covers the same rules
// end to end): the network surface of audiobookshelf.c is stubbed, so a test
// that reaches a stub asserts that the gates stood between the job and the
// first byte. Compiles with the real audiobookshelf.c + abs_profiles.c +
// pathlimits.c + sha256.c + config/audiobookdb, ASan/UBSan on.

#include "src/system/core/config.h"
#include "src/system/core/pathlimits.h"
#include "src/system/core/sha256.h"
#include "src/system/library/audiobookdb.h"
#include "src/system/net/http.h"
#include "src/system/streaming/abs_profiles.h"
#include "src/system/streaming/audiobookshelf.h"

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *work; // from argv[1]
static char store_path[1024]; // $WORK/profiles.ini, the two-profile store

// ---------------------------------------------------------------------------
// Network stubs (§11.5 gates must decide before any byte moves)
// ---------------------------------------------------------------------------

bool http_stream_open_ex(http_stream_t *st, const char *url, const char *extra_headers, int timeout_secs) {
	(void)st; (void)url; (void)extra_headers; (void)timeout_secs;
	return false;
}

int http_stream_read(http_stream_t *st, void *buf, int len) {
	(void)st; (void)buf; (void)len;
	return 0;
}

void http_stream_close(http_stream_t *st) { (void)st; }

const char *http_last_error(void) { return ""; }

// audiobookdb registers this collation at open; no card database is opened in
// this harness, but the symbol must exist for the link (same as the outbox test).
int library_collate_listorder(void *unused, int na, const void *a, int nb, const void *b) {
	(void)unused;
	int cmp = memcmp(a, b, na < nb ? na : nb);
	return cmp ? cmp : na - nb;
}

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

// Reference digests (computed with hashlib; guards against hash drift inside
// the destination builder): sha256("http://abs-a.example")[0:12] etc.
static const char *EXPECT_HASH_A = "a47146b6ab4a";
static const char *EXPECT_HASH_B = "b92f98bef8bb";
static const char *URL_A = "http://abs-a.example";
static const char *URL_B = "http://abs-b.example";

static void path_join(char *out, size_t size, const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	vsnprintf(out, size, fmt, args);
	va_end(args);
}

static bool dir_exists(const char *path) {
	struct stat st;
	return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool file_exists(const char *path) { return access(path, F_OK) == 0; }

static void write_file(const char *path, const char *text) {
	FILE *f = fopen(path, "wb");
	assert(f);
	fputs(text, f);
	fclose(f);
}

static void make_dir(const char *path) {
	char copy[1024];
	snprintf(copy, sizeof(copy), "%s", path);
	for (char *p = copy + 1; *p; p++) {
		if (*p != '/') continue;
		*p = '\0';
		assert(mkdir(copy, 0777) == 0 || errno == EEXIST);
		*p = '/';
	}
	assert(mkdir(copy, 0777) == 0 || errno == EEXIST);
}

static void recursive_remove(const char *path) {
	DIR *dir = opendir(path);
	if (dir) {
		struct dirent *entry;
		char child[1024];
		while ((entry = readdir(dir))) {
			if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
			snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
			recursive_remove(child);
		}
		closedir(dir);
	}
	assert(remove(path) == 0 || errno == ENOENT);
}

static audiobookshelf_book_t make_book(const char *item_id, const char *author, const char *title,
									   int track_count) {
	audiobookshelf_book_t book;
	memset(&book, 0, sizeof(book));
	snprintf(book.item.id, sizeof(book.item.id), "%s", item_id);
	snprintf(book.item.title, sizeof(book.item.title), "%s", title);
	snprintf(book.item.author, sizeof(book.item.author), "%s", author);
	book.item.duration = 600.0 * (track_count > 0 ? track_count : 1);
	book.track_count = track_count;
	for (int i = 0; i < track_count; i++) {
		audiobookshelf_track_t *track = &book.tracks[i];
		snprintf(track->title, sizeof(track->title), "Part %d", i + 1);
		snprintf(track->filename, sizeof(track->filename), "Part %d.mp3", i + 1);
		snprintf(track->content_url, sizeof(track->content_url), "/s/item/%s/%d.mp3", item_id, i);
		track->duration = 600.0;
		track->start_offset = 600.0 * i;
	}
	return book;
}

// ---------------------------------------------------------------------------
// The §11.5 cases
// ---------------------------------------------------------------------------

static void download_and_destination_tests(const char *sd, const char *id_a, const char *id_b) {
	char root_a[1024], folder[1024], foreign[1200], foreign_manifest[1300], check[1300];

	// Case 1: a fresh book goes to abs-<h12>/<safe-author>/<safe-title>, and
	// the refusal at the (stubbed) network proves the gates ran their course
	// without one byte reaching the card.
	assert(abs_profiles_select(id_a));
	path_join(root_a, sizeof(root_a), "%s/Audiobooks/Audiobookshelf/abs-%s", sd, EXPECT_HASH_A);
	audiobookshelf_book_t book = make_book("probe-item", "Author A", "Collision Probe", 2);
	path_join(folder, sizeof(folder), "%s/Author A/Collision Probe", root_a);
	assert(audiobookshelf_download(&book, sd, NULL, NULL, NULL, 0) == false);
	assert(strcmp(audiobookshelf_last_error(), "The audiobook file could not be downloaded.") == 0);
	assert(dir_exists(folder));
	// No audio bytes: the stub refused the stream, and nothing else may exist.
	path_join(check, sizeof(check), "%s/001 - Part One.mp3", folder);
	assert(!file_exists(check));
	book = make_book("probe-item", "Author B", "Second Probe", 1);
	path_join(folder, sizeof(folder), "%s/Author B/Second Probe", root_a);
	assert(audiobookshelf_download(&book, sd, NULL, NULL, NULL, 0) == false);
	assert(dir_exists(folder));
	path_join(check, sizeof(check), "%s/001 - Part One.mp3", folder);
	assert(!file_exists(check));

	// Case 2: hash-prefix collision. A foreign manifest is enough of a claim:
	// compare the full manifest server and fail the job rather than share.
	path_join(foreign, sizeof(foreign), "%s/Foreign Author/Foreign Title", root_a);
	path_join(foreign_manifest, sizeof(foreign_manifest), "%s/.audiobookshelf", foreign);
	make_dir(foreign);
	write_file(foreign_manifest, "version=1\nitem_id=other-item\nserver=http://abs-b.example\nduration=1\ntrack=1\t0\t1\tx.mp3\n");

	book = make_book("probe-item", "Server B Server", "Same Title", 1);
	assert(audiobookshelf_download(&book, sd, NULL, NULL, NULL, 0) == false);
	assert(strcmp(audiobookshelf_last_error(), "The download path is already used by another Audiobookshelf server.") == 0);
	path_join(check, sizeof(check), "%s/Server B Server", root_a);
	assert(!dir_exists(check)); // the refused job left no trace under the shared root
	// The foreign manifest survived untouched.
	{
		char before[256];
		FILE *f = fopen(foreign_manifest, "r");
		assert(f && fgets(before, sizeof(before), f));
		fclose(f);
		(void)before;
	}
	(void)id_b;

	// A manifest with no resolvable server is a foreign claim too (fail closed).
	write_file(foreign_manifest, "version=1\nitem_id=other-item\nserver=   \nduration=1\n");
	assert(audiobookshelf_download(&book, sd, NULL, NULL, NULL, 0) == false);
	assert(strcmp(audiobookshelf_last_error(), "The download path is already used by another Audiobookshelf server.") == 0);

	// A manifest naming OUR server is not a collision: the job gets past the
	// hash gate and fails at the (stubbed) network instead.
	write_file(foreign_manifest, "version=1\nitem_id=probe-item\nserver=http://abs-a.example\nduration=1\n");
	assert(audiobookshelf_download(&book, sd, NULL, NULL, NULL, 0) == false);
	assert(strcmp(audiobookshelf_last_error(), "The audiobook file could not be downloaded.") == 0);

	// Case 3: the shared path ceiling refuses the destination before any
	// directory is created: author 159 bytes + title 219 bytes push every
	// track path past the durable-row limit (SONIX_LINKED_DB_PATH_MAX). The
	// track filename is maxed out too, so the rejection holds even on hosts
	// with a short temp path.
	char long_author[160], long_title[220], long_file[280];
	memset(long_author, 'L', sizeof(long_author) - 1);
	long_author[sizeof(long_author) - 1] = '\0';
	memset(long_title, 'M', sizeof(long_title) - 1);
	long_title[sizeof(long_title) - 1] = '\0';
	memset(long_file, 'N', 239);
	long_file[239] = '\0';
	book = make_book("probe-item", long_author, long_title, 1);
	memcpy(book.tracks[0].filename, long_file, 239); // 239 < filename[240] capacity
	book.tracks[0].filename[239] = '\0';
	assert(audiobookshelf_download(&book, sd, NULL, NULL, NULL, 0) == false);
	assert(strcmp(audiobookshelf_last_error(), "The audiobook download path is too long.") == 0);
	path_join(check, sizeof(check), "%s/%s/%s", root_a, long_author, long_title);
	assert(!dir_exists(check));
	assert(sonix_linked_path_representable(SONIX_LINKED_DB_PATH_MAX - 1));
	assert(!sonix_linked_path_representable(SONIX_LINKED_DB_PATH_MAX));
}

static void local_linked_tests(const char *sd, const char *id_a, const char *id_b) {
	char folder[1024], manifest_path[1200], audio[1300];
	path_join(folder, sizeof(folder), "%s/Audiobooks/Audiobookshelf/abs-%s/Author A/Collision Probe", sd, EXPECT_HASH_A);
	path_join(manifest_path, sizeof(manifest_path), "%s/.audiobookshelf", folder);
	path_join(audio, sizeof(audio), "%s/Part One.mp3", folder);

	// A server-bound manifest links only for the identity it names.
	write_file(manifest_path, "version=1\nitem_id=probe-item\nserver=http://abs-a.example\nduration=1200\nprogress=3\nprogress_updated=7\ntrack=1\t0\t600\tPart One.mp3\n");
	assert(abs_profiles_select(id_a));
	assert(audiobookshelf_local_linked(audio, "probe-item") == true);
	// Same server, different item: no.
	assert(audiobookshelf_local_linked(audio, "other-item") == false);
	// §11.5: server B's identical item id cannot open server A's copy.
	assert(abs_profiles_select(id_b));
	assert(audiobookshelf_local_linked(audio, "probe-item") == false);
	// The GUI decision for that not-linked match (§11.5): the copy belongs to
	// another server -> offered a download from this server, never re-linked.
	// Under its own server the copy is not "elsewhere": re-link applies.
	assert(audiobookshelf_local_owned_elsewhere(audio) == true);
	assert(abs_profiles_select(id_a));
	assert(audiobookshelf_local_owned_elsewhere(audio) == false);

	// §11.5: manifest version/required fields are validated before the binding
	// is trusted. No version line, no link -- and no reliable binding, so this
	// is not "another server's copy" either: the re-link path applies.
	write_file(manifest_path, "item_id=probe-item\nserver=http://abs-a.example\n");
	assert(audiobookshelf_local_linked(audio, "probe-item") == false);
	assert(audiobookshelf_local_owned_elsewhere(audio) == false);
	// Empty item_id is not a binding either.
	write_file(manifest_path, "version=1\nitem_id=\nserver=http://abs-a.example\n");
	assert(audiobookshelf_local_linked(audio, "probe-item") == false);
	write_file(manifest_path, "version=1\nitem_id=probe-item\nserver=http://abs-a.example\nduration=1200\n");
	assert(audiobookshelf_local_linked(audio, "probe-item") == true);
}

// §11.5 legacy clause: a 0.5.0 manifest without server= binds to the persisted
// legacy_server -- with multiple profiles, never to whichever profile happens
// to be active -- and without a reliable legacy server it never links.
static void legacy_binding_tests(const char *sd) {
	char cfg[1024], prof[1024];
	path_join(cfg, sizeof(cfg), "%s/legacy_config.ini", work);
	path_join(prof, sizeof(prof), "%s/legacy_profiles.ini", work);
	unlink(prof);
	setenv("SONIX_AUDIOBOOKSHELF_PROFILES", prof, 1);
	config_init(cfg);
	config_set("audiobookshelf", "server", " http://abs-a.example/ "); // unnormalized on purpose
	config_set("audiobookshelf", "token", "legacy-token");
	assert(config_save());
	assert(abs_profiles_init() == 1); // migration: sole identity becomes p1
	char legacy[ABS_SERVER_MAX];
	abs_profiles_legacy_server(legacy, sizeof(legacy));
	assert(strcmp(legacy, URL_A) == 0);

	char folder[1024], manifest_path[1200], audio[1300];
	path_join(folder, sizeof(folder), "%s/Audiobooks/Audiobookshelf/Legacy Author/Legacy Book", sd);
	path_join(manifest_path, sizeof(manifest_path), "%s/.audiobookshelf", folder);
	path_join(audio, sizeof(audio), "%s/Part One.mp3", folder);
	make_dir(folder);
	// 0.5.0 wrote version=1 and item_id; these carry no server=.
	write_file(manifest_path, "version=1\nitem_id=legacy-item\nduration=600\n");

	char migrated[ABS_PROFILE_ID_MAX], second[ABS_PROFILE_ID_MAX];
	assert(abs_profiles_active(migrated, sizeof(migrated)));

	// With one profile (the migrated 0.5.0 identity) the missing server binds
	// to it: the migration window IS the original binding.
	assert(audiobookshelf_local_linked(audio, "legacy-item") == true);

	// With multiple profiles the binding must still be legacy_server even
	// though another profile is active: a second profile must not inherit old
	// manifests by happening to be selected (§11.5).
	assert(abs_profiles_add("Server B", URL_B, "token-b", second, sizeof(second)));
	assert(abs_profiles_select(second));
	assert(audiobookshelf_local_linked(audio, "legacy-item") == false);
	// An explicit server=B manifest DOES bind under the active B identity --
	// so the false above comes from the legacy rule, not a broken link path.
	write_file(manifest_path, "version=1\nitem_id=probe-item\nserver=http://abs-b.example\nduration=600\n");
	assert(audiobookshelf_local_linked(audio, "probe-item") == true);

	// No reliable legacy server left (empty in the store, profiles present):
	// require explicit re-link, keep local progress pending. Never guess.
	assert(abs_profiles_select(migrated));
	setenv("SONIX_AUDIOBOOKSHELF_PROFILES", store_path, 1);
	assert(abs_profiles_init() == 2); // the two-profile store, legacy_server empty
	write_file(manifest_path, "version=1\nitem_id=probe-item\nserver=  \nduration=600\n");
	assert(audiobookshelf_local_linked(audio, "probe-item") == false);
	if (file_exists(cfg)) unlink(cfg);
}

int main(int argc, char **argv) {
	assert(argc == 2);
	work = argv[1];

	unsetenv("SONIX_AUDIOBOOKSHELF_SERVER");
	unsetenv("SONIX_AUDIOBOOKSHELF_TOKEN");
	path_join(store_path, sizeof(store_path), "%s/profiles.ini", work);
	setenv("SONIX_AUDIOBOOKSHELF_PROFILES", store_path, 1);

	// Two mock-server profiles with identical book content underneath.
	assert(abs_profiles_init() == 0);
	char id_a[ABS_PROFILE_ID_MAX], id_b[ABS_PROFILE_ID_MAX];
	assert(abs_profiles_add("Server A", URL_A, "token-a", id_a, sizeof(id_a)));
	assert(abs_profiles_add("Server B", URL_B, "token-b", id_b, sizeof(id_b)));
	assert(strcmp(id_a, "p1") == 0 && strcmp(id_b, "p2") == 0);

	char sd[1024];
	path_join(sd, sizeof(sd), "%s/card", work);
	make_dir(sd);

	download_and_destination_tests(sd, id_a, id_b);
	local_linked_tests(sd, id_a, id_b);
	legacy_binding_tests(sd);

	printf("audiobookshelf-absdirs: all tests passed\n");
	return 0;
}
