#include "tidalsaved.h"

#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>

#include "src/system/db/sqlite3.h"

static sqlite3 *open_db(const char *root, bool write) {
	char path[512];
	struct stat st;
	if (!root || !*root || stat(root, &st) != 0 || !S_ISDIR(st.st_mode) ||
		snprintf(path, sizeof(path), "%s/.local", root) >= (int)sizeof(path)) {
		return NULL;
	}
	if (write && mkdir(path, 0755) != 0 && errno != EEXIST) {
		return NULL;
	}
	if (snprintf(path, sizeof(path), "%s/.local/tidal-saved.db", root) >= (int)sizeof(path)) {
		return NULL;
	}
	sqlite3 *db = NULL;
	int flags = write ? SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE : SQLITE_OPEN_READONLY;
	if (sqlite3_open_v2(path, &db, flags, NULL) != SQLITE_OK) {
		sqlite3_close(db);
		return NULL;
	}
	sqlite3_busy_timeout(db, 500);
	if (write && sqlite3_exec(db, "CREATE TABLE IF NOT EXISTS saved("
		"id TEXT UNIQUE NOT NULL,title TEXT,artist TEXT,cover TEXT,released TEXT,tracks INTEGER)",
		NULL, NULL, NULL) != SQLITE_OK) {
		sqlite3_close(db);
		return NULL;
	}
	return db;
}

bool tidalsaved_has(const char *root, const char *id) {
	sqlite3 *db = open_db(root, false);
	if (!db) {
		return false;
	}
	sqlite3_stmt *stmt = NULL;
	bool found = sqlite3_prepare_v2(db, "SELECT 1 FROM saved WHERE id=?1", -1, &stmt, NULL) == SQLITE_OK &&
		sqlite3_bind_text(stmt, 1, id, -1, SQLITE_TRANSIENT) == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW;
	sqlite3_finalize(stmt);
	sqlite3_close(db);
	return found;
}

bool tidalsaved_set(const char *root, const tidal_album_t *album, bool saved) {
	if (!album || !album->id_text[0]) {
		return false;
	}
	sqlite3 *db = open_db(root, true);
	if (!db) {
		return false;
	}
	sqlite3_stmt *stmt = NULL;
	const char *sql = saved ? "INSERT OR IGNORE INTO saved(id,title,artist,cover,released,tracks) VALUES(?,?,?,?,?,?)"
		: "DELETE FROM saved WHERE id=?1";
	bool ok = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK &&
		sqlite3_bind_text(stmt, 1, album->id_text, -1, SQLITE_TRANSIENT) == SQLITE_OK;
	if (ok && saved) {
		ok = sqlite3_bind_text(stmt, 2, album->title, -1, SQLITE_TRANSIENT) == SQLITE_OK &&
			sqlite3_bind_text(stmt, 3, album->artist, -1, SQLITE_TRANSIENT) == SQLITE_OK &&
			sqlite3_bind_text(stmt, 4, album->cover, -1, SQLITE_TRANSIENT) == SQLITE_OK &&
			sqlite3_bind_text(stmt, 5, album->released, -1, SQLITE_TRANSIENT) == SQLITE_OK &&
			sqlite3_bind_int(stmt, 6, album->track_count) == SQLITE_OK;
	}
	ok = ok && sqlite3_step(stmt) == SQLITE_DONE;
	sqlite3_finalize(stmt);
	sqlite3_close(db);
	return ok;
}

static const char *column_text(sqlite3_stmt *stmt, int index) {
	const unsigned char *value = sqlite3_column_text(stmt, index);
	return value ? (const char *)value : "";
}

int tidalsaved_list(const char *root, int offset, tidal_album_t *out, int max) {
	if (!out || max <= 0 || offset < 0) return -1;
	char path[512];
	struct stat st;
	if (!root || !root[0] || stat(root, &st) != 0 || !S_ISDIR(st.st_mode) ||
		snprintf(path, sizeof(path), "%s/.local/tidal-saved.db", root) >= (int)sizeof(path)) return -1;
	if (stat(path, &st) != 0) return errno == ENOENT ? 0 : -1;
	if (!S_ISREG(st.st_mode)) return -1;
	sqlite3 *db = open_db(root, false);
	if (!db) return -1;
	sqlite3_stmt *stmt = NULL;
	int count = 0, rc = SQLITE_ERROR;
	if (sqlite3_prepare_v2(db, "SELECT id,title,artist,cover,released,tracks FROM saved "
			"ORDER BY rowid DESC LIMIT ?1 OFFSET ?2", -1, &stmt, NULL) == SQLITE_OK &&
		sqlite3_bind_int(stmt, 1, max) == SQLITE_OK && sqlite3_bind_int(stmt, 2, offset) == SQLITE_OK) {
		while (count < max && (rc = sqlite3_step(stmt)) == SQLITE_ROW) {
			tidal_album_t *album = &out[count++];
			*album = (tidal_album_t){0};
			snprintf(album->id_text, sizeof(album->id_text), "%s", column_text(stmt, 0));
			snprintf(album->title, sizeof(album->title), "%s", column_text(stmt, 1));
			snprintf(album->artist, sizeof(album->artist), "%s", column_text(stmt, 2));
			snprintf(album->cover, sizeof(album->cover), "%s", column_text(stmt, 3));
			snprintf(album->released, sizeof(album->released), "%s", column_text(stmt, 4));
			album->track_count = sqlite3_column_int(stmt, 5);
		}
	}
	sqlite3_finalize(stmt);
	sqlite3_close(db);
	return rc == SQLITE_DONE || (rc == SQLITE_ROW && count == max) ? count : -1;
}

bool tidalsaved_playlist_set(const char *root, const char *id, const char *name, const long *tracks, int count) {
	if (!id || !*id || !name || !tracks || count <= 0) return false;
	sqlite3 *db = open_db(root, true);
	if (!db) return false;
	bool ok = sqlite3_exec(db, "CREATE TABLE IF NOT EXISTS offline_playlist(id TEXT PRIMARY KEY,name TEXT);"
		"CREATE TABLE IF NOT EXISTS offline_track(id TEXT,pos INTEGER,track INTEGER,PRIMARY KEY(id,pos));BEGIN",
		NULL, NULL, NULL) == SQLITE_OK;
	sqlite3_stmt *stmt = NULL;
	ok = ok && sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO offline_playlist VALUES(?,?)", -1, &stmt, NULL) == SQLITE_OK &&
		sqlite3_bind_text(stmt, 1, id, -1, SQLITE_TRANSIENT) == SQLITE_OK &&
		sqlite3_bind_text(stmt, 2, name, -1, SQLITE_TRANSIENT) == SQLITE_OK && sqlite3_step(stmt) == SQLITE_DONE;
	sqlite3_finalize(stmt);
	stmt = NULL;
	ok = ok && sqlite3_prepare_v2(db, "DELETE FROM offline_track WHERE id=?", -1, &stmt, NULL) == SQLITE_OK &&
		sqlite3_bind_text(stmt, 1, id, -1, SQLITE_TRANSIENT) == SQLITE_OK && sqlite3_step(stmt) == SQLITE_DONE;
	sqlite3_finalize(stmt);
	stmt = NULL;
	ok = ok && sqlite3_prepare_v2(db, "INSERT INTO offline_track VALUES(?,?,?)", -1, &stmt, NULL) == SQLITE_OK &&
		sqlite3_bind_text(stmt, 1, id, -1, SQLITE_TRANSIENT) == SQLITE_OK;
	for (int i = 0; ok && i < count; i++) {
		ok = sqlite3_bind_int(stmt, 2, i) == SQLITE_OK && sqlite3_bind_int64(stmt, 3, tracks[i]) == SQLITE_OK &&
			sqlite3_step(stmt) == SQLITE_DONE && sqlite3_reset(stmt) == SQLITE_OK;
	}
	sqlite3_finalize(stmt);
	bool committed = sqlite3_exec(db, ok ? "COMMIT" : "ROLLBACK", NULL, NULL, NULL) == SQLITE_OK;
	sqlite3_close(db);
	return ok && committed;
}

int tidalsaved_playlists(const char *root, int offset, tidal_playlist_t *out, int max) {
	if (!out || offset < 0 || max <= 0) return 0;
	sqlite3 *db = open_db(root, false);
	if (!db) return 0;
	sqlite3_stmt *stmt = NULL;
	int count = 0;
	if (sqlite3_prepare_v2(db, "SELECT id,name FROM offline_playlist ORDER BY name COLLATE NOCASE,id LIMIT ? OFFSET ?",
			-1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_int(stmt, 1, max);
		sqlite3_bind_int(stmt, 2, offset);
		while (count < max && sqlite3_step(stmt) == SQLITE_ROW) {
			tidal_playlist_t *p = &out[count++];
			*p = (tidal_playlist_t){0};
			snprintf(p->id, sizeof(p->id), "%s", column_text(stmt, 0));
			snprintf(p->name, sizeof(p->name), "%s", column_text(stmt, 1));
		}
	}
	sqlite3_finalize(stmt);
	sqlite3_close(db);
	return count;
}

int tidalsaved_playlist_tracks(const char *root, const char *id, int offset, long *out, int max) {
	if (!out || !id || offset < 0 || max <= 0) return 0;
	sqlite3 *db = open_db(root, false);
	if (!db) return 0;
	sqlite3_stmt *stmt = NULL;
	int count = 0;
	if (sqlite3_prepare_v2(db, "SELECT track FROM offline_track WHERE id=? ORDER BY pos LIMIT ? OFFSET ?",
			-1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, id, -1, SQLITE_TRANSIENT);
		sqlite3_bind_int(stmt, 2, max);
		sqlite3_bind_int(stmt, 3, offset);
		while (count < max && sqlite3_step(stmt) == SQLITE_ROW) out[count++] = (long)sqlite3_column_int64(stmt, 0);
	}
	sqlite3_finalize(stmt);
	sqlite3_close(db);
	return count;
}
