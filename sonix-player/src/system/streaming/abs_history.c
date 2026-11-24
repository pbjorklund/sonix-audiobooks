#include "abs_history.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

bool abs_history_schema(sqlite3 *db) {
	return db && sqlite3_exec(db,
		"CREATE TABLE IF NOT EXISTS ABS_HISTORY ("
		"id TEXT PRIMARY KEY, scope TEXT NOT NULL, item TEXT NOT NULL, "
		"started INTEGER NOT NULL, ended INTEGER NOT NULL, played INTEGER NOT NULL, "
		"position REAL NOT NULL, duration REAL NOT NULL);"
		"CREATE INDEX IF NOT EXISTS ABS_HISTORY_SCOPE ON ABS_HISTORY(scope, ended DESC);",
		NULL, NULL, NULL) == SQLITE_OK;
}

bool abs_history_enqueue(sqlite3 *db, const abs_history_session_t *s) {
	if (!db || !s || strnlen(s->id, sizeof(s->id)) != 36 ||
		strnlen(s->scope, sizeof(s->scope)) != 64 || !s->item[0] ||
		strnlen(s->item, sizeof(s->item)) == sizeof(s->item) ||
		s->started_ms <= 0 || s->ended_ms < s->started_ms || s->played_ms <= 0 ||
		!isfinite(s->position) || !isfinite(s->duration) || s->position < 0 ||
		s->duration <= 0 || s->position > s->duration)
		return false;
	sqlite3_stmt *q = NULL;
	if (sqlite3_prepare_v2(db, "INSERT OR IGNORE INTO ABS_HISTORY VALUES(?,?,?,?,?,?,?,?)", -1, &q, NULL) != SQLITE_OK)
		return false;
	sqlite3_bind_text(q, 1, s->id, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(q, 2, s->scope, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(q, 3, s->item, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(q, 4, s->started_ms);
	sqlite3_bind_int64(q, 5, s->ended_ms);
	sqlite3_bind_int64(q, 6, s->played_ms);
	sqlite3_bind_double(q, 7, s->position);
	sqlite3_bind_double(q, 8, s->duration);
	bool ok = sqlite3_step(q) == SQLITE_DONE;
	sqlite3_finalize(q);
	return ok;
}

bool abs_history_next(sqlite3 *db, const char *scope, abs_history_session_t *s) {
	if (!db || !scope || !s) return false;
	sqlite3_stmt *q = NULL;
	if (sqlite3_prepare_v2(db, "SELECT id,item,started,ended,played,position,duration FROM ABS_HISTORY "
		"WHERE scope=? ORDER BY ended DESC,id LIMIT 1", -1, &q, NULL) != SQLITE_OK)
		return false;
	sqlite3_bind_text(q, 1, scope, -1, SQLITE_TRANSIENT);
	bool ok = sqlite3_step(q) == SQLITE_ROW;
	if (ok) {
		memset(s, 0, sizeof(*s));
		snprintf(s->id, sizeof(s->id), "%s", sqlite3_column_text(q, 0));
		snprintf(s->scope, sizeof(s->scope), "%s", scope);
		snprintf(s->item, sizeof(s->item), "%s", sqlite3_column_text(q, 1));
		s->started_ms = sqlite3_column_int64(q, 2);
		s->ended_ms = sqlite3_column_int64(q, 3);
		s->played_ms = sqlite3_column_int64(q, 4);
		s->position = sqlite3_column_double(q, 5);
		s->duration = sqlite3_column_double(q, 6);
	}
	sqlite3_finalize(q);
	return ok;
}

bool abs_history_ack(sqlite3 *db, const char *scope, const char *id) {
	if (!db || !scope || !id) return false;
	sqlite3_stmt *q = NULL;
	if (sqlite3_prepare_v2(db, "DELETE FROM ABS_HISTORY WHERE scope=? AND id=?", -1, &q, NULL) != SQLITE_OK)
		return false;
	sqlite3_bind_text(q, 1, scope, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(q, 2, id, -1, SQLITE_TRANSIENT);
	bool ok = sqlite3_step(q) == SQLITE_DONE && sqlite3_changes(db) == 1;
	sqlite3_finalize(q);
	return ok;
}
