#include "abs_episode_progress.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define KEY "server=? AND owner=? AND show=? AND episode=?"
#define VALUES "seconds,duration,finished,updated,revision,dirty"

bool abs_episode_progress_schema(sqlite3 *db) {
	return db && sqlite3_exec(db,
		"CREATE TABLE IF NOT EXISTS ABS_EPISODE_PROGRESS ("
		"server TEXT NOT NULL,owner TEXT NOT NULL,show TEXT NOT NULL,episode TEXT NOT NULL,"
		"seconds REAL NOT NULL,duration REAL NOT NULL,finished INTEGER NOT NULL,updated INTEGER NOT NULL,"
		"revision INTEGER NOT NULL,dirty INTEGER NOT NULL,attempted INTEGER NOT NULL DEFAULT 0,"
		"PRIMARY KEY(server,owner,show,episode));"
		"CREATE INDEX IF NOT EXISTS ABS_EPISODE_PENDING ON ABS_EPISODE_PROGRESS(server,owner,attempted);",
		NULL, NULL, NULL) == SQLITE_OK;
}

static void bind_string(sqlite3_stmt *q, int i, const char *s) { sqlite3_bind_text(q, i, s, -1, SQLITE_TRANSIENT); }

static void bind_key(sqlite3_stmt *q, int i, const abs_episode_manifest_t *id) {
	bind_string(q, i, id->server);
	bind_string(q, i + 1, id->owner);
	bind_string(q, i + 2, id->show);
	bind_string(q, i + 3, id->episode);
}

static bool valid(const abs_episode_checkpoint_t *s) {
	return s && s->identity.server[0] && s->identity.owner[0] && s->identity.show[0] && s->identity.episode[0] &&
		isfinite(s->seconds) && s->seconds >= 0 && isfinite(s->duration) && s->duration >= 0 && s->updated_ms >= 0;
}

static void read_value(sqlite3_stmt *q, abs_episode_checkpoint_t *out) {
	out->seconds = sqlite3_column_double(q, 0);
	out->duration = sqlite3_column_double(q, 1);
	out->finished = sqlite3_column_int(q, 2) != 0;
	out->updated_ms = sqlite3_column_int64(q, 3);
	out->revision = sqlite3_column_int64(q, 4);
	out->dirty = sqlite3_column_int(q, 5) != 0;
}

bool abs_episode_progress_get(sqlite3 *db, const abs_episode_manifest_t *id, abs_episode_checkpoint_t *out) {
	if (!db || !id || !out) return false;
	sqlite3_stmt *q = NULL;
	if (sqlite3_prepare_v2(db, "SELECT " VALUES " FROM ABS_EPISODE_PROGRESS WHERE " KEY, -1, &q, NULL) != SQLITE_OK)
		return false;
	bind_key(q, 1, id);
	bool ok = sqlite3_step(q) == SQLITE_ROW;
	if (ok) { out->identity = *id; read_value(q, out); }
	sqlite3_finalize(q);
	return ok;
}

static void bind_value(sqlite3_stmt *q, int i, const abs_episode_checkpoint_t *s) {
	sqlite3_bind_double(q, i, s->seconds);
	sqlite3_bind_double(q, i + 1, s->duration);
	sqlite3_bind_int(q, i + 2, s->finished);
	sqlite3_bind_int64(q, i + 3, s->updated_ms);
}

bool abs_episode_progress_save(sqlite3 *db, const abs_episode_checkpoint_t *s) {
	if (!db || !valid(s)) return false;
	sqlite3_stmt *q = NULL;
	const char *sql = "INSERT INTO ABS_EPISODE_PROGRESS(server,owner,show,episode," VALUES ") VALUES(?,?,?,?,?,?,?,?,1,1)"
		" ON CONFLICT(server,owner,show,episode) DO UPDATE SET seconds=excluded.seconds,duration=excluded.duration,"
		"finished=excluded.finished,updated=excluded.updated,revision=revision+1,dirty=1,attempted=0"
		" WHERE seconds<>excluded.seconds OR duration<>excluded.duration OR finished<>excluded.finished";
	if (sqlite3_prepare_v2(db, sql, -1, &q, NULL) != SQLITE_OK) return false;
	bind_key(q, 1, &s->identity);
	bind_value(q, 5, s);
	bool ok = sqlite3_step(q) == SQLITE_DONE;
	sqlite3_finalize(q);
	return ok;
}

bool abs_episode_progress_apply(sqlite3 *db, const abs_episode_checkpoint_t *base, const abs_episode_checkpoint_t *s) {
	if (!db || !base || !valid(s)) return false;
	sqlite3_stmt *q = NULL;
	const char *sql = base->revision ?
		"UPDATE ABS_EPISODE_PROGRESS SET seconds=?,duration=?,finished=?,updated=?,revision=revision+1,dirty=0 WHERE " KEY " AND revision=?" :
		"INSERT OR IGNORE INTO ABS_EPISODE_PROGRESS(seconds,duration,finished,updated,server,owner,show,episode,revision,dirty) VALUES(?,?,?,?,?,?,?,?,1,0)";
	if (sqlite3_prepare_v2(db, sql, -1, &q, NULL) != SQLITE_OK) return false;
	bind_value(q, 1, s);
	bind_key(q, 5, &base->identity);
	if (base->revision) sqlite3_bind_int64(q, 9, base->revision);
	bool ok = sqlite3_step(q) == SQLITE_DONE && sqlite3_changes(db) == 1;
	sqlite3_finalize(q);
	return ok;
}

bool abs_episode_progress_next(sqlite3 *db, const char *server, const char *owner, long long before,
	abs_episode_checkpoint_t *out) {
	if (!db || !server || !owner || !out) return false;
	sqlite3_stmt *q = NULL;
	const char *sql = "SELECT " VALUES ",show,episode FROM ABS_EPISODE_PROGRESS WHERE server=? AND owner=? AND "
		"(attempted<=? OR attempted>?) AND (dirty=1 OR attempted<=?3-240000 OR attempted>?4) ORDER BY attempted ASC LIMIT 1";
	if (sqlite3_prepare_v2(db, sql, -1, &q, NULL) != SQLITE_OK) return false;
	bind_string(q, 1, server);
	bind_string(q, 2, owner);
	sqlite3_bind_int64(q, 3, before);
	sqlite3_bind_int64(q, 4, before + 60000);
	bool ok = sqlite3_step(q) == SQLITE_ROW;
	if (ok) {
		memset(out, 0, sizeof(*out));
		snprintf(out->identity.server, sizeof(out->identity.server), "%s", server);
		snprintf(out->identity.owner, sizeof(out->identity.owner), "%s", owner);
		snprintf(out->identity.show, sizeof(out->identity.show), "%s", sqlite3_column_text(q, 6));
		snprintf(out->identity.episode, sizeof(out->identity.episode), "%s", sqlite3_column_text(q, 7));
		read_value(q, out);
		ok = valid(out);
	}
	sqlite3_finalize(q);
	return ok;
}

bool abs_episode_progress_attempt(sqlite3 *db, const abs_episode_checkpoint_t *base, long long now) {
	if (!db || !base || !base->revision) return false;
	sqlite3_stmt *q = NULL;
	if (sqlite3_prepare_v2(db, "UPDATE ABS_EPISODE_PROGRESS SET attempted=? WHERE " KEY " AND revision=?", -1, &q, NULL) != SQLITE_OK)
		return false;
	sqlite3_bind_int64(q, 1, now);
	bind_key(q, 2, &base->identity);
	sqlite3_bind_int64(q, 6, base->revision);
	bool ok = sqlite3_step(q) == SQLITE_DONE && sqlite3_changes(db) == 1;
	sqlite3_finalize(q);
	return ok;
}
