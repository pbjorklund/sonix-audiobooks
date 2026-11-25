#include "src/system/streaming/abs_episode_progress.h"
#include "src/system/streaming/abs_episode_playback.h"
#include "src/system/streaming/abs_history.h"
#include "src/system/streaming/abs_show_reader.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char card[512];
static unsigned card_generation = 1;
static bool download_blocked;
static sqlite3 *progress_db;
unsigned abs_download_card_generation(void) { return card_generation; }
bool abs_download_card_path(const char *relative, char *out, size_t cap) {
	int n = snprintf(out, cap, "%s/%s", card, relative);
	return n > 0 && (size_t)n < cap;
}
bool abs_download_sync_blocked(const char *destination) { (void)destination; return download_blocked; }
unsigned audiobookdb_history_generation(void) { return card_generation; }
void audiobookshelf_episode_progress_wake(void) {}
bool audiobookdb_episode_save(unsigned generation, const abs_episode_checkpoint_t *value) {
	return generation == card_generation && abs_episode_progress_save(progress_db, value);
}
bool audiobookdb_episode_get(const abs_episode_manifest_t *id, abs_episode_checkpoint_t *out, unsigned *generation) {
	(void)generation;
	return abs_episode_progress_get(progress_db, id, out);
}

static void fixture_episode(abs_episode_manifest_t *id, char *audio, size_t cap) {
	char rel[ABS_DL_DEST_MAX], dir[SONIX_LINKED_PATH_MAX];
	assert(abs_episode_destination(id->server, id->owner, id->show, id->episode, rel, sizeof(rel)));
	assert(abs_episode_card_dir(rel, dir, sizeof(dir)));
	for (char *p = dir + strlen(card) + 1; *p; p++) if (*p == '/') {
		*p = '\0'; (void)mkdir(dir, 0700); *p = '/';
	}
	assert(mkdir(dir, 0700) == 0);
	assert((size_t)snprintf(audio, cap, "%s/%s", dir, id->audio) < cap);
	FILE *f = fopen(audio, "w");
	assert(f && fputc('a', f) != EOF && fclose(f) == 0);
	assert(abs_episode_manifest_commit(dir, id));
}

static void download_groups(sqlite3 *db, const char *work) {
	progress_db = db;
	assert(strlen(work) < sizeof(card));
	strcpy(card, work);
	char *slash = strrchr(card, '/');
	assert(slash); *slash = '\0';
	abs_episode_manifest_t id = {.server = "https://abs.test", .owner = "user-a", .show = "show-a",
		.episode = "finished", .file = "file", .audio = "audio-0123456789abcdef.mp3", .title = "First",
		.show_title = "Show A", .published_ms = 1500000000000LL, .bytes = 1, .duration = 100};
	char a[1400], b[1400], other[1400], book[1400], late[1400];
	fixture_episode(&id, a, sizeof(a));
	abs_episode_manifest_t verified;
	assert(abs_episode_card_identity(a, &verified) && !strcmp(verified.episode, id.episode));
	download_blocked = true;
	assert(!abs_episode_card_identity(a, &verified));
	download_blocked = false;
	const char *stream = "/fixture/.local/abs-episode-stream-0.mp3";
	char local[1400];
	abs_episode_playback_register(stream, &id);
	assert(abs_episode_playback_local(stream, local, sizeof(local)) && !strcmp(local, a));
	assert(!abs_episode_playback_switched(stream));
	abs_episode_playback_did_switch(stream, local);
	assert(abs_episode_playback_switched(stream) && abs_episode_playback_local_in_use(stream, local, sizeof(local)));
	assert(!abs_episode_playback_switched("/different.mp3"));
	card_generation++;
	assert(!abs_episode_playback_local(stream, local, sizeof(local)) && !abs_episode_playback_switched(stream));
	card_generation--;
	abs_episode_manifest_t newer = id;
	strcpy(newer.audio, "audio-0123456789aaaaaa.mp3");
	abs_episode_playback_register(stream, &newer);
	assert(!abs_episode_playback_local(stream, local, sizeof(local)) && local[0] == '\0');
	abs_episode_checkpoint_t done = {.identity = id, .duration = 100, .seconds = 100, .finished = true, .updated_ms = 123};
	assert(abs_episode_progress_save(db, &done));
	strcpy(id.episode, "unfinished");
	fixture_episode(&id, b, sizeof(b));
	strcpy(id.show, "show-b"); strcpy(id.show_title, "Show B");
	fixture_episode(&id, other, sizeof(other));
	assert((size_t)snprintf(book, sizeof(book), "%s/book.mp3", card) < sizeof(book));
	FILE *f = fopen(book, "w"); assert(f && fclose(f) == 0);
	abs_episode_row_t rows[2];
	bool more;
	assert(abs_episode_download_list(id.server, id.owner, NULL, 0, rows, 2, &more) == 2 && !more);
	assert(!strcmp(rows[0].title, "Show A") && !strcmp(rows[1].title, "Show B"));
	assert(abs_episode_download_list(id.server, id.owner, "show-a", 0, rows, 2, &more) == 2);
	assert(rows[0].published_ms == id.published_ms && rows[0].downloaded);
	abs_episode_removal_t plan = {.server = "https://abs.test", .owner = "user-a", .show = "show-a", .finished_only = true};
	assert(abs_episode_removal_plan(&plan, a) && plan.count == 0);
	assert(abs_episode_removal_plan(&plan, NULL) && plan.count == 1);
	assert(abs_episode_removal_apply(&plan, NULL) == 1 && access(a, F_OK) != 0 && access(b, F_OK) == 0);
	free(plan.episodes); plan.episodes = NULL; plan.finished_only = false;
	assert(abs_episode_removal_plan(&plan, NULL) && plan.count == 1);
	strcpy(id.show, "show-a"); strcpy(id.episode, "late");
	fixture_episode(&id, late, sizeof(late));
	card_generation++;
	assert(abs_episode_removal_apply(&plan, NULL) == -1 && access(b, F_OK) == 0);
	card_generation--;
	assert(abs_episode_removal_apply(&plan, NULL) == 1);
	assert(access(b, F_OK) != 0 && access(late, F_OK) == 0 && access(other, F_OK) == 0 && access(book, F_OK) == 0);
	abs_episode_checkpoint_t saved;
	assert(abs_episode_progress_get(db, &done.identity, &saved) && saved.finished && saved.seconds == 100);
	free(plan.episodes);
}

static int test_read(void *arg, void *buf, int cap) {
	const char **left = arg;
	size_t n = strnlen(*left, cap < 5 ? (size_t)cap : 5);
	memcpy(buf, *left, n);
	*left += n;
	return (int)n;
}

static void show_projection(void) {
	const char *show = "{\"media\":{\"episodes\":["
		"{\"libraryItemId\":\"show\",\"id\":\"older\",\"title\":\"First\",\"publishedAt\":1500000000000},"
		"{\"libraryItemId\":\"show\",\"id\":\"newer\",\"title\":\"Second \\u2014 sample\","
		"\"pubDate\":\"Tue, 14 Nov 2017 01:00:00 +0100\",\"description\":\"<p>ignored</p>\"}]},"
		"\"mediaType\":\"podcast\",\"id\":\"show\"}";
	abs_episode_row_t *rows;
	const char *p = show;
	assert(abs_show_read(test_read, &p, "show", &rows) == 2);
	assert(!strcmp(rows[0].episode_id, "newer") && rows[0].published_ms == 1510617600000LL);
	assert(!strcmp(rows[0].title, "Second — sample"));
	rows[0].finished = true;
	int slots[2] = {-1, -1};
	rows[1].in_progress = true;
	assert(abs_episode_project(rows, 2, false, ABS_EP_ALL, 0, slots, 2) == 2 && slots[0] == 0 && slots[1] == 1);
	assert(abs_episode_project(rows, 2, true, ABS_EP_ALL, 0, slots, 2) == 2 && slots[0] == 1 && slots[1] == 0);
	assert(abs_episode_project(rows, 2, true, ABS_EP_FINISHED, 0, slots, 1) == 1 && slots[0] == 0);
	assert(abs_episode_project(rows, 2, false, ABS_EP_STARTED, 0, slots, 1) == 1 && slots[0] == 1);
	assert(abs_episode_project(rows, 2, false, ABS_EP_UNPLAYED, 0, slots, 2) == 0);
	assert(abs_episode_project(rows, 2, false, 0, 0, slots, 2) == 0);
	assert(abs_episode_project(rows, 2, true, ABS_EP_STARTED | ABS_EP_FINISHED, 1, slots, 1) == 2 && slots[0] == 0);
	abs_episode_sort(rows, 2, ABS_EPISODE_TITLE);
	assert(!strcmp(rows[0].episode_id, "older"));
	rows[0].added_ms = 1;
	rows[1].added_ms = 2;
	abs_episode_sort(rows, 2, ABS_EPISODE_ADDED);
	assert(!strcmp(rows[0].episode_id, "newer"));
	free(rows);
	const abs_episode_row_t choices[] = {
		{.episode_id = "middle", .added_ms = 200},
		{.episode_id = "heard", .published_ms = 400, .finished = true},
		{.episode_id = "oldest", .published_ms = 100},
		{.episode_id = "newest", .published_ms = 300},
		{.episode_id = "undated"},
	};
	assert(abs_episode_next_pair(choices, 5, true, slots) == 2 && slots[0] == 2 && slots[1] == 0);
	assert(abs_episode_next_pair(choices, 5, false, slots) == 2 && slots[0] == 3 && slots[1] == 0);
	assert(abs_episode_next_pair(choices + 1, 1, false, slots) == 0);
	assert(abs_episode_next_pair(choices, 1, true, slots) == 1 && slots[0] == 0 && slots[1] == -1);
	char title[ABS_NAME_MAX];
	p = "{\"media\":{\"episodes\":[{\"title\":\"Not the show\"}],\"metadata\":{\"title\":\"Actual Show\"}},\"id\":\"show\",\"mediaType\":\"podcast\"}";
	assert(abs_show_read_title(test_read, &p, "show", title) && !strcmp(title, "Actual Show"));
	p = "{\"id\":\"other\",\"mediaType\":\"podcast\",\"media\":{\"metadata\":{\"title\":\"Other\"}}}";
	assert(!abs_show_read_title(test_read, &p, "show", title) && !title[0]);
	p = show;
	assert(abs_show_read(test_read, &p, "wrong-parent", &rows) == -1 && !rows);
	p = "{\"id\":\"show\",\"mediaType\":\"podcast\",\"media\":{\"episodes\":[{";
	assert(abs_show_read(test_read, &p, "show", &rows) == -1 && !rows);
	p = "{\"id\":\"show\",\"mediaType\":\"podcast\",\"media\":{\"episodes\":[]}} junk";
	assert(abs_show_read(test_read, &p, "show", &rows) == -1 && !rows);
}

int main(int argc, char **argv) {
	assert(argc == 2);
	show_projection();
	sqlite3 *db;
	assert(sqlite3_open(argv[1], &db) == SQLITE_OK);
	assert(abs_episode_progress_schema(db));
	abs_episode_checkpoint_t p = {.identity = {.server = "https://abs.test", .owner = "user-a", .show = "show", .episode = "ep"},
		.seconds = 14.25, .duration = 100, .updated_ms = 1500000000123LL};
	assert(abs_episode_progress_save(db, &p));
	abs_episode_checkpoint_t snapshot;
	assert(abs_episode_progress_get(db, &p.identity, &snapshot) && snapshot.dirty && snapshot.revision == 1);
	p.seconds = 9;
	assert(abs_episode_progress_save(db, &p));
	assert(!abs_episode_progress_apply(db, &snapshot, &snapshot));
	abs_episode_manifest_t isolated = p.identity;
	strcpy(isolated.owner, "user-b");
	assert(!abs_episode_progress_get(db, &isolated, &snapshot));
	strcpy(isolated.owner, "user-a");
	strcpy(isolated.episode, "sibling");
	assert(!abs_episode_progress_get(db, &isolated, &snapshot));
	strcpy(isolated.episode, "ep");
	strcpy(isolated.server, "https://other.test");
	assert(!abs_episode_progress_get(db, &isolated, &snapshot));
	assert(sqlite3_close(db) == SQLITE_OK);
	assert(sqlite3_open(argv[1], &db) == SQLITE_OK);
	assert(abs_episode_progress_get(db, &p.identity, &snapshot) && snapshot.seconds == 9 && snapshot.dirty);
	assert(abs_episode_progress_attempt(db, &snapshot, 67000));
	abs_episode_checkpoint_t candidate;
	assert(!abs_episode_progress_next(db, p.identity.server, p.identity.owner, 66999, &candidate));
	assert(abs_episode_progress_next(db, p.identity.server, p.identity.owner, 67000, &candidate));
	assert(abs_episode_progress_apply(db, &candidate, &candidate));
	assert(abs_episode_progress_get(db, &p.identity, &snapshot) && !snapshot.dirty && snapshot.seconds == 9);

	// Start with the pre-podcast schema and an existing book row.
	assert(sqlite3_exec(db, "CREATE TABLE ABS_HISTORY (id TEXT PRIMARY KEY, scope TEXT NOT NULL, item TEXT NOT NULL, started INTEGER NOT NULL, ended INTEGER NOT NULL, played INTEGER NOT NULL, position REAL NOT NULL, duration REAL NOT NULL);"
		"INSERT INTO ABS_HISTORY VALUES('book-id','books','book',10,20,10,7,100);", NULL, NULL, NULL) == SQLITE_OK);
	assert(abs_history_schema(db));
	abs_history_session_t saved;
	assert(abs_history_next(db, "books", &saved) && !saved.episode[0] && !saved.wire_ms && saved.played_ms == 10);
	abs_history_session_t listen = {.id = "15de94a5-1034-49ca-9385-529605d800c1", .item = "show", .episode = "ep",
		.started_ms = 70000, .ended_ms = 73000, .played_ms = 2600, .position = 9, .duration = 100};
	memset(listen.scope, 'p', 64);
	assert(abs_history_enqueue(db, &listen));
	assert(abs_history_set_wire(db, listen.scope, listen.id, 72999));
	assert(!abs_history_set_wire(db, listen.scope, listen.id, 73000));
	assert(!abs_history_ack(db, "books", listen.id));
	assert(sqlite3_close(db) == SQLITE_OK);
	assert(sqlite3_open(argv[1], &db) == SQLITE_OK);
	assert(abs_history_next(db, listen.scope, &saved) && !strcmp(saved.episode, "ep") && saved.wire_ms == 72999);
	assert(abs_history_ack(db, listen.scope, saved.id));
	assert(abs_history_next(db, "books", &saved) && !strcmp(saved.item, "book"));
	download_groups(db, argv[1]);
	assert(abs_history_next(db, "books", &saved) && !strcmp(saved.item, "book"));
	assert(sqlite3_close(db) == SQLITE_OK);
	puts("ABS episodes: bounded show parsing, progress restart/CAS/account isolation, and history migration passed.");
}
