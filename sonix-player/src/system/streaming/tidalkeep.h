#ifndef TIDALKEEP_H
#define TIDALKEEP_H

#include <stdbool.h>

#include "src/system/streaming/tidal.h"

// ---------------------------------------------------------------------------
// The Tidal download queue: the "download" behind the per-track and
// per-album menu entries.
//
// tidalcache downloads while it plays; this queue downloads without playing,
// one track at a time on its own thread, and marks every finished track
// "kept" so the cache's clearing rules leave it alone (see tidalcache.h).
//
// The queue lives in memory: what is already finished is on the card and
// survives a restart, while an interrupted batch has to be asked for again
// (the album entry is still there to tap). Failed items stay in a bounded
// failed list that "retry failed downloads" can put back on the queue.
// ---------------------------------------------------------------------------

#define TIDALKEEP_MAX 128

typedef struct {
	int pending;  // queued, not yet attempted
	int done;     // finished and kept in this session
	int failed;   // gave up after the retry budget
	long current; // the track downloading right now, 0 when none
	bool active;  // a batch is running (pending > 0 or a download is in flight)
} tidalkeep_state_t;

typedef enum {
	TIDALKEEP_REJECTED, // no queue capacity or worker unavailable
	TIDALKEEP_SKIPPED,  // already kept, in flight, queued, or not streamable
	TIDALKEEP_ADDED,
} tidalkeep_enqueue_result_t;

tidalkeep_enqueue_result_t tidalkeep_enqueue(const tidal_track_t *track);

// A snapshot of the queue, safe to call from the GUI thread.
tidalkeep_state_t tidalkeep_state(void);

// Puts every failed item back on the queue.
void tidalkeep_retry_failed(void);

// Runs on the worker thread when the queue drains: nothing pending, no
// download in flight. Register from the GUI; bounce to the GUI thread with
// gui_post() before touching widgets.
typedef void (*tidalkeep_done_cb_t)(void);
void tidalkeep_set_done_cb(tidalkeep_done_cb_t cb);

// Test seam: replaces the per-track fetch. Set before starting the worker.
// Never call in production code.
typedef bool (*tidalkeep_fetch_t)(const tidal_track_t *track, int quality, char *path_out, size_t size, bool *complete_transfer);
void tidalkeep_set_executor(tidalkeep_fetch_t fn);

int tidalkeep_quality(void);
void tidalkeep_set_quality(int value);

#endif /* TIDALKEEP_H */
