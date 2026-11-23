#include "audiobookshelfqueue.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "abslist.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/popover.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/gui/nowplaying/player.h"
#include "src/system/core/lang.h"
#include "src/system/streaming/abs_download.h"

// ---------------------------------------------------------------------------
// The queue screen (spec §8.1, §8.5 line 579).
//
// One screen, built once: a clear-finished action row, a storage-paused note
// and the pooled download rows. State flows one way, on one cadence: the
// screen's lv_timer copies the engine's coalesced snapshot into one
// screen-lifetime buffer (allocated when the screen loads, freed when it
// unloads) and repaints only when the snapshot's generation, job set, per-job
// progress bytes or the storage pause actually changed — so a 32 KiB file
// read costs nothing in the GUI and a long transfer repaints at the poll rate,
// never faster (§8.1: "polls a coalesced snapshot at 4 Hz or slower; the
// worker does not post an LVGL event for every read").
//
// Rows are the shared abslist pool: twelve row widgets bound to the snapshot
// by data index, so a queue of any length (≤16 jobs, §8.1) costs the same
// twelve-row tree, and scrolling rebinds instead of allocating.
//
// Commands run straight on the GUI thread through abs_download_*() — each is
// a transactional, mutex-brief engine call returning an English error, which
// lands in a toast. There is no HTTP, no SQLite and no worker logic in this
// file; the screen only mirrors and commands the queue (§8.5 line 579).
// ---------------------------------------------------------------------------

// §8.1: "4 Hz or slower". One constant, asserted structurally by the queue
// smoke (tools/audiobookshelf-queueui-smoke.sh) so the cadence cannot drift.
#define ABS_QUEUE_POLL_MS 250

typedef enum {
	QUEUE_ACT_PAUSE = 0,
	QUEUE_ACT_RESUME,
	QUEUE_ACT_CANCEL,
	QUEUE_ACT_REMOVE,
	QUEUE_ACT_RETRY,
	QUEUE_ACT_COUNT
} queue_action_t;

static const char *const action_keys[QUEUE_ACT_COUNT] = {
	[QUEUE_ACT_PAUSE] = "audiobookshelf_queue_pause",
	[QUEUE_ACT_RESUME] = "audiobookshelf_queue_resume",
	[QUEUE_ACT_CANCEL] = "audiobookshelf_queue_cancel",
	[QUEUE_ACT_REMOVE] = "audiobookshelf_queue_remove",
	[QUEUE_ACT_RETRY] = "audiobookshelf_queue_retry",
};

static const char *const state_keys[ABS_DL_STATE_COUNT] = {
	[ABS_DL_QUEUED] = "abs_queue_queued",
	[ABS_DL_FETCHING] = "abs_queue_fetching",
	[ABS_DL_DOWNLOADING] = "abs_queue_downloading",
	[ABS_DL_PAUSING] = "abs_queue_stopping",
	[ABS_DL_PAUSED] = "abs_queue_paused",
	[ABS_DL_WAITING_NETWORK] = "abs_queue_waiting_network",
	[ABS_DL_FINALIZING] = "abs_queue_finalizing",
	[ABS_DL_DONE] = "abs_queue_done",
	[ABS_DL_FAILED] = "abs_queue_failed",
	[ABS_DL_CANCELLED] = "abs_queue_cancelled",
};

// Per-row menu. PAUSING and FINALIZING are worker-owned transitional states
// with no permitted action (§8.1); every other state offers at most two.
#define QUEUE_ROW_ACTIONS_MAX 2

// Popover action closures. One popover exists at a time, so a small static
// pool carries the job id + action from the menu build to the invocation —
// popover_show() copies the items but must keep pointing at live cells.
typedef struct {
	long long job_id;
	int action;
} queue_action_cell_t;
#define QUEUE_ACTION_CELLS QUEUE_ROW_ACTIONS_MAX

static lv_obj_t *queue_screen;
static lv_obj_t *content;
static abs_list_t *list;
static lv_obj_t *clear_row;
static lv_obj_t *storage_note;
static lv_timer_t *poll_timer;

// The screen-lifetime snapshot: one heap copy the poll refreshes in place.
// Alive only while the screen is loaded (§8.6 line 605: the UI owns snapshot
// memory, so it is not held while the user is elsewhere). Design note in
// .specs/forge-m3-gui-report.md: the engine's snapshot struct is fixed at
// 16 whole jobs (37.5 KiB); the line's <24 KiB target assumes paged rows, and
// the engine exposes no paging surface in 0.6.0 — bounded one-copy reuse, not
// per-tick copies, is the posture this wave takes.
static abs_download_snapshot_t *snap;

// What the rows were last painted from; the coalescing test between polls.
typedef struct {
	unsigned generation;
	int count;
	bool storage_paused;
	struct {
		long long id;
		unsigned generation;
		int state;
		int file_index;
		int file_count;
		long long bytes_done;
		long long bytes_total;
	} job[ABS_DL_JOB_CAP];
} queue_cache_t;
static queue_cache_t cache;

static queue_action_cell_t action_cells[QUEUE_ACTION_CELLS];

// Telemetry the queue smoke reads from the log to prove the poll coalesces
// (4 Hz ticks, repaints only on change).
static unsigned poll_count;
static unsigned repaint_count;

static void poll_once(void);
static void queue_screen_loaded_cb(lv_event_t *e);
static void queue_screen_unloaded_cb(lv_event_t *e);
static void queue_screen_delete_cb(lv_event_t *e);
static void clear_finished_cb(lv_event_t *e);
static void queue_action_cb(void *user);

lv_obj_t *audiobookshelfqueue_screen(void) {
	return queue_screen;
}

void audiobookshelfqueue_open(void) {
	if (!queue_screen) return;
	switch_screen(queue_screen);
}

// The §8.1 allowed-actions table, as the row menu offers it. PAUSING and
// FINALIZING rows open nothing: both are worker-owned transitional states
// ("Stop requested" waits for the ack; the atomic finalize cannot be safely
// torn) and per the table offer no action.
static void queue_row_click(void *user, int index, lv_obj_t *row) {
	(void)user;
	if (!snap || !row || index < 0 || index >= snap->job_count) return;
	const abs_download_job_t *job = &snap->jobs[index];

	int actions[QUEUE_ROW_ACTIONS_MAX];
	int offers = 0;
	switch (job->state) {
	case ABS_DL_QUEUED:
	case ABS_DL_FETCHING:
	case ABS_DL_DOWNLOADING:
	case ABS_DL_WAITING_NETWORK:
		actions[offers++] = QUEUE_ACT_PAUSE;
		actions[offers++] = QUEUE_ACT_CANCEL;
		break;
	case ABS_DL_PAUSED:
		actions[offers++] = QUEUE_ACT_RESUME;
		actions[offers++] = QUEUE_ACT_REMOVE;
		break;
	case ABS_DL_DONE:
		actions[offers++] = QUEUE_ACT_REMOVE;
		break;
	case ABS_DL_FAILED:
	case ABS_DL_CANCELLED:
		actions[offers++] = QUEUE_ACT_RETRY;
		actions[offers++] = QUEUE_ACT_REMOVE;
		break;
	default:
		return;
	}

	popover_item_t items[QUEUE_ROW_ACTIONS_MAX];
	for (int i = 0; i < offers; i++) {
		action_cells[i] = (queue_action_cell_t){.job_id = job->id, .action = actions[i]};
		items[i] = (popover_item_t){.label = tr(action_keys[actions[i]]),
									.action = queue_action_cb,
									.user = &action_cells[i],
									.checked = false};
	}
	popover_show(row, items, offers);
}

static void queue_action_cb(void *user) {
	queue_action_cell_t *cell = user;
	if (!cell) return;
	char error[ABS_DL_ERROR_MAX];
	bool ok;
	switch (cell->action) {
	case QUEUE_ACT_PAUSE:
		ok = abs_download_pause(cell->job_id, error, sizeof(error));
		break;
	case QUEUE_ACT_RESUME:
		ok = abs_download_resume(cell->job_id, error, sizeof(error));
		break;
	case QUEUE_ACT_CANCEL:
		ok = abs_download_cancel(cell->job_id, error, sizeof(error));
		break;
	case QUEUE_ACT_REMOVE:
		ok = abs_download_remove(cell->job_id, error, sizeof(error));
		break;
	case QUEUE_ACT_RETRY:
		ok = abs_download_retry(cell->job_id, error, sizeof(error));
		break;
	default:
		return;
	}
	// Failures are engine-shaped English errors (a busy job refuses, a card
	// swap disarms, FINALIZING defers); the row keeps its state either way and
	// the next poll repaints whatever the queue actually decided.
	if (!ok) toast_error(error[0] ? error : tr("audiobookshelf_request_failed"));
}

static void clear_finished_cb(lv_event_t *e) {
	(void)e;
	char error[ABS_DL_ERROR_MAX];
	if (!abs_download_clear_finished(error, sizeof(error)))
		toast_error(error[0] ? error : tr("audiobookshelf_request_failed"));
}

// Row text for one pooled row: title over "<state> · <progress>". Progress is
// the worker's own coarse counters (file index / byte counters, §8.2), so the
// whole-book percentage is the same weighted shape the old modal showed —
// moved down from the modal's per-read callback to this 4 Hz poll.
static void queue_row_bind(void *user, int index, char *title, size_t title_size,
						   char *subtitle, size_t subtitle_size) {
	(void)user;
	if (title_size) title[0] = '\0';
	if (subtitle_size) subtitle[0] = '\0';
	if (!snap || index < 0 || index >= snap->job_count) return;
	const abs_download_job_t *job = &snap->jobs[index];

	snprintf(title, title_size, "%s",
			 job->title[0] ? job->title : tr("audiobookshelf_queue_untitled"));
	const char *state = tr(state_keys[job->state]);

	if (job->state == ABS_DL_DOWNLOADING) {
		// Whole-book percent: finished files count as full, the current file
		// by its byte counters. Without a server-provided length only the
		// file position is honest.
		int percent = -1;
		if (job->file_count > 0 && job->bytes_total > 0) {
			long long done = job->bytes_done;
			if (done < 0) done = 0;
			if (done > job->bytes_total) done = job->bytes_total;
			percent = (int)((job->file_index * 100 + done * 100 / job->bytes_total) /
							job->file_count);
			if (percent < 0) percent = 0;
			if (percent > 99) percent = 99; // 100% belongs to DONE, not DOWNLOADING
		}
		char file_of[64];
		if (job->file_count > 0) {
			snprintf(file_of, sizeof(file_of), tr("audiobookshelf_queue_file_of"),
					 job->file_index + 1, job->file_count);
		} else {
			file_of[0] = '\0';
		}
		if (percent >= 0 && file_of[0])
			snprintf(subtitle, subtitle_size, "%s · %d%% · %s", state, percent, file_of);
		else if (percent >= 0)
			snprintf(subtitle, subtitle_size, "%s · %d%%", state, percent);
		else if (file_of[0])
			snprintf(subtitle, subtitle_size, "%s · %s", state, file_of);
		else
			snprintf(subtitle, subtitle_size, "%s", state);
	} else if (job->state == ABS_DL_FAILED && job->error[0]) {
		// §8.4: the stored message is already sanitized English; the row never
		// invents stage-specific wording on top of it.
		snprintf(subtitle, subtitle_size, "%s — %s", state, job->error);
	} else {
		snprintf(subtitle, subtitle_size, "%s", state);
	}
}

// The coalescing test between two polls: the engine's transition generation,
// the job set, the per-job progress counters, or the storage pause changed.
// The refresh happens after the decision, so the caller paints a snapshot the
// cache then describes exactly.
static bool queue_snapshot_changed(const abs_download_snapshot_t *next) {
	bool changed = false;
	if (next->generation != cache.generation || next->job_count != cache.count ||
		next->storage_paused != cache.storage_paused)
		changed = true;
	for (int i = 0; i < next->job_count && i < ABS_DL_JOB_CAP; i++) {
		const abs_download_job_t *job = &next->jobs[i];
		if (job->id != cache.job[i].id || job->generation != cache.job[i].generation ||
			(int)job->state != cache.job[i].state ||
			job->file_index != cache.job[i].file_index ||
			job->file_count != cache.job[i].file_count ||
			job->bytes_done != cache.job[i].bytes_done ||
			job->bytes_total != cache.job[i].bytes_total)
			changed = true;
	}
	cache.generation = next->generation;
	cache.count = next->job_count;
	cache.storage_paused = next->storage_paused;
	memset(cache.job, 0, sizeof(cache.job));
	for (int i = 0; i < next->job_count && i < ABS_DL_JOB_CAP; i++) {
		const abs_download_job_t *job = &next->jobs[i];
		cache.job[i].id = job->id;
		cache.job[i].generation = job->generation;
		cache.job[i].state = (int)job->state;
		cache.job[i].file_index = job->file_index;
		cache.job[i].file_count = job->file_count;
		cache.job[i].bytes_done = job->bytes_done;
		cache.job[i].bytes_total = job->bytes_total;
	}
	return changed;
}

static bool queue_has_terminal(void) {
	if (!snap) return false;
	for (int i = 0; i < snap->job_count; i++) {
		if (snap->jobs[i].state == ABS_DL_DONE || snap->jobs[i].state == ABS_DL_FAILED ||
			snap->jobs[i].state == ABS_DL_CANCELLED)
			return true;
	}
	return false;
}

// One poll tick: copy the snapshot (§8.1) and repaint only on change. A
// refused snapshot (quiesced card / DB swap) parks the rows on the
// not-available note until a lifecycle re-init re-arms the queue.
static void poll_once(void) {
	poll_count++;
	if (!snap) return;
	bool ok = abs_download_snapshot(snap);

	bool changed;
	if (!ok) {
		changed = cache.generation != 0 || cache.count != 0 || cache.storage_paused;
		memset(&cache, 0, sizeof(cache));
		if (!changed) return;
		repaint_count++;
		abs_list_set_count(list, 0);
		abs_list_note(list, tr("audiobookshelf_queue_not_available"));
		lv_obj_set_hidden(clear_row, true);
		lv_obj_set_hidden(storage_note, true);
		return;
	}

	if (!queue_snapshot_changed(snap)) return; // the coalesced idle tick
	repaint_count++;

	lv_obj_set_hidden(storage_note, !snap->storage_paused);
	lv_obj_set_hidden(clear_row, !queue_has_terminal());

	// Note first, count second: the pooled renderer skips a rebind while the
	// note is up, so the ordering decides which view the rebind lands on.
	abs_list_note(list, snap->job_count == 0 ? tr("audiobookshelf_queue_none") : NULL);
	abs_list_set_count(list, snap->job_count);
}

static void poll_timer_cb(lv_timer_t *timer) {
	(void)timer;
	poll_once();
}

// The queue lives while the screen does: the snapshot buffer allocates on
// load, the poll timer only ticks on it, and a close parks both — an open
// queue screen costs its buffer, a closed one nothing.
static void queue_screen_loaded_cb(lv_event_t *e) {
	(void)e;
	if (!snap) snap = calloc(1, sizeof(*snap));
	memset(&cache, 0, sizeof(cache));
	if (poll_timer) lv_timer_resume(poll_timer);
	poll_once(); // paint the present state before the first tick
}

static void queue_screen_unloaded_cb(lv_event_t *e) {
	(void)e;
	if (poll_timer) lv_timer_pause(poll_timer);
	// One bounded line for the §8.1 coalescing shape: 4 Hz ticks, repaints
	// only on change (the queue smoke reads it to assert both).
	printf("audiobookshelf: queue screen closed after %u poll ticks (%u repaints)\n",
		   poll_count, repaint_count);
	if (snap) {
		free(snap);
		snap = NULL;
	}
}

// Process teardown deletes screens without unloading them first: the buffer
// and the poll timer must both go with it.
static void queue_screen_delete_cb(lv_event_t *e) {
	(void)e;
	if (poll_timer) {
		lv_timer_del(poll_timer);
		poll_timer = NULL;
	}
	if (snap) {
		free(snap);
		snap = NULL;
	}
}

void audiobookshelfqueue_init(gui_config_t *cfg) {
	queue_screen = lv_obj_create(NULL);
	content = settingsrow_page(queue_screen, cfg, "audiobookshelf_downloads");

	// The clear-finished action and the storage note above the rows, toggled
	// by the poll when the queue has something to say. Both just hide — the
	// row pool is untouched either way.
	clear_row = settingsrow_action(content, "audiobookshelf_queue_clear_finished",
								   clear_finished_cb, NULL);
	storage_note = lv_label_create(content);
	lv_label_set_long_mode(storage_note, LV_LABEL_LONG_WRAP);
	lv_label_set_text(storage_note, tr("audiobookshelf_queue_storage_paused"));
	lv_obj_set_width(storage_note, lv_pct(100));
	lv_obj_set_style_text_align(storage_note, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(storage_note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(storage_note, &font_ui_20, 0);
	lv_obj_set_style_pad_ver(storage_note, 12, 0);
	lv_obj_set_hidden(storage_note, true);

	abs_list_config_t cfg_list = {
		.parent = content,
		.row_height = 112, // two-line shape, the book-list card: title over progress
		.row_gap = 8,
		.bind = queue_row_bind,
		.click = queue_row_click,
		.user = NULL,
	};
	list = abs_list_create(&cfg_list);
	abs_list_note(list, tr("audiobookshelf_queue_none"));

	poll_timer = lv_timer_create(poll_timer_cb, ABS_QUEUE_POLL_MS, NULL);
	lv_timer_pause(poll_timer); // ticks only while the screen is loaded

	lv_obj_add_event_cb(queue_screen, queue_screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(queue_screen, queue_screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
	lv_obj_add_event_cb(queue_screen, queue_screen_delete_cb, LV_EVENT_DELETE, NULL);
	// Rows sit on the scrolling content, the swipe home gesture rides the
	// screen: both attached, like every streaming page.
	switcher_attach_back_gesture(queue_screen);
	player_sheet_attach_drag(queue_screen, true);
	player_sheet_attach_drag(content, true);
	(void)cfg;
}
