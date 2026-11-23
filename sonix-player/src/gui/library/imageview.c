#include "imageview.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/nowplaying/cover.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/spinner.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/lang.h"

lv_obj_t *imageview_screen;

static lv_obj_t *title_label;
static lv_obj_t *picture;
static lv_obj_t *busy;
static lv_obj_t *failed; // the file is not a picture this player can decode
static int box_w, box_h;

static cover_image_t shown; // what `picture` draws

// ---------------------------------------------------------------------------
// The decode thread.
//
// Decoding a photograph takes fractions of a second to seconds, so it never
// happens on the interface thread -- but until 0.6.0 every open spawned a
// detached thread of its own, so two quick opens had two decodes running at
// once, each paying the full read-and-decode cost of the §9.5 budget. The
// screen that cares (Audiobookshelf: detail, then full-screen of the same
// cover, then back) now shares ONE worker thread instead:
//
//   * a request while a decode runs only replaces the queued path -- the
//     worker never starts a second decode before the one in flight is
//     finished, so image-view decodes are serial by construction;
//   * `generation` is bumped by every open and every unload, and a decode
//     that finishes for a picture nobody waits for any more has its pixels
//     freed on the spot rather than shown;
//   * when the Audiobookshelf detail opens its full-screen view it has
//     already freed its own (slot's) decoded pixels, so at most one
//     retained cover image is alive at a time (§9.3).
//
// No URL networking on this page: the callers hand over a local path, and
// whatever bytes the file holds were already fetched and size-capped where
// they came from.
// ---------------------------------------------------------------------------

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
static bool worker_started;

// The queued request: newest wins, nothing starts before the previous decode
// ends. Guarded by `lock`.
static char next_path[1024];
static bool have_next;

static unsigned generation; // bumped by every open and every unload

typedef struct {
	unsigned generation;
	char path[1024];
	int w, h;
	cover_image_t image;
	bool ok;
} decode_job_t;

static void drop_picture(void) {
	lv_image_set_src(picture, NULL);
	cover_free(&shown);
}

static void decoded(void *user) {
	decode_job_t *job = user;
	if (job->generation != generation) {
		// The page moved on while the decode ran: nobody wants the pixels.
		cover_free(&job->image);
		free(job);
		return;
	}
	lv_obj_set_hidden(busy, true);
	if (!job->ok) {
		lv_obj_set_hidden(failed, false);
		free(job);
		return;
	}
	drop_picture();
	shown = job->image;
	lv_image_set_src(picture, &shown.dsc);
	lv_obj_set_hidden(picture, false);
	free(job);
}

// One persistent worker. It takes the queued path only when idle, so the
// "prior decode finished" half of the §9.4 quiescence rule holds without the
// interface thread ever waiting for a decode.
static void *decode_worker(void *arg) {
	(void)arg;
	for (;;) {
		pthread_mutex_lock(&lock);
		while (!have_next) {
			pthread_cond_wait(&wake, &lock);
		}
		decode_job_t *job = calloc(1, sizeof(*job));
		if (!job) {
			// The request cannot be served; drop it rather than spinning on
			// it. The page keeps its spinner until the next open.
			have_next = false;
			pthread_mutex_unlock(&lock);
			continue;
		}
		memcpy(job->path, next_path, sizeof(job->path));
		job->w = box_w;
		job->h = box_h;
		job->generation = generation;
		have_next = false;
		pthread_mutex_unlock(&lock);

		job->ok = cover_load_image_file(job->path, job->w, job->h, COVER_FIT_CONTAIN, &job->image);

		if (!gui_post(decoded, job)) {
			// The interface is gone: the pixels die with the post failure.
			cover_free(&job->image);
			free(job);
		}
	}
	return NULL;
}

static bool worker_start(void) {
	pthread_mutex_lock(&lock);
	if (worker_started) {
		pthread_mutex_unlock(&lock);
		return true;
	}
	worker_started = true;
	pthread_mutex_unlock(&lock);

	pthread_t thread;
	if (pthread_create(&thread, NULL, decode_worker, NULL) != 0) {
		pthread_mutex_lock(&lock);
		worker_started = false;
		pthread_mutex_unlock(&lock);
		return false;
	}
	pthread_detach(thread);
	return true;
}

void imageview_open_named(const char *path, const char *title) {
	if (!path || !path[0]) return;

	// §9.4 quiescence, the "newer wins" half: whatever was queued is
	// replaced, and the generation bump frees the result of any decode the
	// previous open still has in flight -- and the single worker never starts
	// a second decode before the one in flight is finished, so this page's
	// decodes are serial by construction.
	bool started = worker_start();
	pthread_mutex_lock(&lock);
	generation++;
	snprintf(next_path, sizeof(next_path), "%s", path);
	have_next = started;
	pthread_cond_signal(&wake);
	pthread_mutex_unlock(&lock);

	drop_picture();
	lv_obj_set_hidden(picture, true);
	lv_obj_set_hidden(failed, true);
	// The page is up with the spinner until the decode lands in decoded();
	// without a worker thread the decode cannot even start (the "unreadable"
	// note takes its place).
	lv_obj_set_hidden(busy, !started);

	if (title && title[0]) {
		lv_label_set_text(title_label, title);
	} else {
		const char *slash = strrchr(path, '/');
		lv_label_set_text(title_label, slash ? slash + 1 : path);
	}
	switch_screen(imageview_screen);
}

void imageview_open(const char *path) {
	imageview_open_named(path, NULL);
}

// Leaving the page lets the picture go: a photo the size of the screen is the
// better part of a megabyte, and nothing else needs it. An in-flight decode
// is invalidated through the generation bump -- its pixels are freed when it
// posts. A queued-but-not-started decode is dropped here, so leaving the page
// cannot leave a decode running only to free it moments later.
static void unloaded_cb(lv_event_t *e) {
	(void)e;
	pthread_mutex_lock(&lock);
	generation++;
	have_next = false;
	pthread_mutex_unlock(&lock);
	drop_picture();
}

void imageview_init(gui_config_t *cfg) {
	lv_obj_add_style(imageview_screen, &theme_style_screen, 0);
	title_label = settingsrow_title(imageview_screen, cfg, "");
	settingsrow_title_corner_slots(title_label, cfg, 0);

	int content_top = settingsrow_content_top(cfg);
	box_w = cfg->screen_width - 2 * cfg->padding;
	box_h = cfg->screen_height - content_top - 2 * cfg->padding;

	lv_obj_t *area = lv_obj_create(imageview_screen);
	lv_obj_remove_style_all(area);
	lv_obj_set_size(area, cfg->screen_width, cfg->screen_height - content_top);
	lv_obj_align(area, LV_ALIGN_TOP_LEFT, 0, content_top);
	lv_obj_set_scrollable(area, false);
	lv_obj_set_event_bubble(area, true);

	picture = lv_image_create(area);
	lv_obj_center(picture);
	lv_obj_set_event_bubble(picture, true);
	lv_obj_set_hidden(picture, true);

	busy = spinner_create(area, &icon_loader_big);
	lv_obj_center(busy);
	lv_obj_set_hidden(busy, true);

	failed = lv_label_create(area);
	lv_label_set_text(failed, tr("files_image_unreadable"));
	lv_label_set_long_mode(failed, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(failed, box_w);
	lv_obj_set_style_text_align(failed, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(failed, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(failed, &font_ui_22, 0);
	lv_obj_center(failed);
	lv_obj_set_hidden(failed, true);

	lv_obj_add_event_cb(imageview_screen, unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
	switcher_attach_back_gesture(imageview_screen);
}
