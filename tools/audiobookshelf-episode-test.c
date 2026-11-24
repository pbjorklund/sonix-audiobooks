#include "src/system/streaming/abs_episode_progress.h"
#include "src/system/streaming/abs_history.h"
#include "src/system/streaming/abs_show_reader.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
	free(rows);
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
	assert(sqlite3_close(db) == SQLITE_OK);
	puts("ABS episodes: bounded show parsing, progress restart/CAS/account isolation, and history migration passed.");
}
