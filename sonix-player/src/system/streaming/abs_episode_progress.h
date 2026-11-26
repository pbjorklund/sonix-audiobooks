#ifndef ABS_EPISODE_PROGRESS_H
#define ABS_EPISODE_PROGRESS_H

#include "abs_episode_card.h"
#include "src/system/db/sqlite3.h"

typedef struct {
	abs_episode_manifest_t identity;
	double seconds, duration;
	long long updated_ms, revision;
	bool finished, dirty;
} abs_episode_checkpoint_t;

bool abs_episode_progress_schema(sqlite3 *db);
bool abs_episode_progress_get(sqlite3 *db, const abs_episode_manifest_t *id, abs_episode_checkpoint_t *out);
bool abs_episode_progress_save(sqlite3 *db, const abs_episode_checkpoint_t *value);
bool abs_episode_progress_apply(sqlite3 *db, const abs_episode_checkpoint_t *base, const abs_episode_checkpoint_t *next);
bool abs_episode_progress_next(sqlite3 *db, const char *server, const char *owner,
	long long before, abs_episode_checkpoint_t *out);
bool abs_episode_progress_attempt(sqlite3 *db, const abs_episode_checkpoint_t *base, long long now);

bool audiobookshelf_episode_reconcile(const abs_auth_snapshot_t *auth, const abs_episode_manifest_t *identity, http_op_t *op);
void audiobookshelf_episode_refresh_position(const abs_auth_snapshot_t *auth, const abs_episode_manifest_t *identity,
	http_op_t *op);
void audiobookshelf_episode_progress_wake(void);

#endif
