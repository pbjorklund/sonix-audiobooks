#include "src/system/streaming/abs_history.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
	assert(argc == 2);
	sqlite3 *db;
	assert(sqlite3_open(argv[1], &db) == SQLITE_OK);
	assert(abs_history_schema(db));
	abs_history_session_t session = {
		.id = "15de94a5-1034-49ca-9385-529605d800c0", .item = "book-fixture",
		.started_ms = 1000, .ended_ms = 5700, .played_ms = 4200,
		.position = 600, .duration = 800
	};
	memset(session.scope, 'a', 64);
	assert(abs_history_enqueue(db, &session));
	abs_history_session_t retry = session;
	retry.played_ms = 9000;
	assert(abs_history_enqueue(db, &retry));
	assert(sqlite3_close(db) == SQLITE_OK);
	assert(sqlite3_open(argv[1], &db) == SQLITE_OK);
	char another[65] = {0};
	memset(another, 'b', 64);
	assert(!abs_history_next(db, another, &retry));
	assert(!abs_history_ack(db, another, session.id));
	assert(abs_history_next(db, session.scope, &retry));
	assert(!strcmp(session.id, retry.id) && retry.played_ms == 4200);
	assert(retry.position == 600 && retry.duration == 800);
	assert(abs_history_ack(db, session.scope, session.id));
	assert(!abs_history_next(db, session.scope, &retry));
	assert(sqlite3_close(db) == SQLITE_OK);
	puts("History outbox: durable retry, credential isolation and exact acknowledgement passed.");
}
