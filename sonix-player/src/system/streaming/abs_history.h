#ifndef SONIX_ABS_HISTORY_H
#define SONIX_ABS_HISTORY_H

#include <stdbool.h>
#include "src/system/db/sqlite3.h"

// One immutable upload. Retrying uses the same ID and listening time.
typedef struct {
	char id[37];
	char scope[65];
	char item[128];
	long long started_ms;
	long long ended_ms;
	long long played_ms;
	double position;
	double duration;
} abs_history_session_t;

typedef struct {
	abs_history_session_t session;
	unsigned generation;
	double track_offset;
	double submitted;
	double rendered;
} abs_history_capture_t;

void audiobookshelf_history_begin(abs_history_capture_t *capture, const char *file);
void audiobookshelf_history_finish(abs_history_capture_t *capture, double position);

bool abs_history_schema(sqlite3 *db);
bool abs_history_enqueue(sqlite3 *db, const abs_history_session_t *session);
bool abs_history_next(sqlite3 *db, const char *scope, abs_history_session_t *session);
bool abs_history_ack(sqlite3 *db, const char *scope, const char *id);

#endif
