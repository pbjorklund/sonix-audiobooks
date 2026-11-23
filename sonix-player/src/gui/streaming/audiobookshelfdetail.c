#include "audiobookshelfdetail.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "abslist.h"
#include "audiobookshelfqueue.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/library/imageview.h"
#include "src/gui/nowplaying/coverloader.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/spinner.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/lang.h"

// ---------------------------------------------------------------------------
// The §9.2 detail screen, wave 2: everything, including the cover.
//
// Presentation only. The expanded model is parsed by the engine
// (audiobookshelf_detail(), §9.3) on the browse page's worker, which also
// runs the local-catalog scan there and fetches the authenticated cover into
// the abs-cover-cache (§9.1/§9.3); this screen holds the results between an
// open (identity copied as values) and the unload that releases them.
//
// The cover (wave 2, §9.1/§9.3/§9.4): the accepted completion carries a
// local cache-file path -- no token in it, by the cache's construction. The
// decode runs off-thread on coverloader slot 108 into a 450x640-shape
// contain box; a tap releases the detail's own pixels FIRST and then opens
// the full-screen viewer (imageview) on the same local file under the book's
// title. Decode and viewer decodes therefore never overlap with retained
// pixels: exactly one remote cover image is alive at any moment, and every
// pixel/encoded buffer dies with the unload (release_state) or the stale
// generation.
//
// Only Summary or Chapters is populated at once (§9.2): the toggle repaints
// the content container, so the chapter pool exists only while the chapter
// view does. The description stays inside the engine model the whole time
// (≤ 8 KiB) and LVGL holds one wrapped copy of it; under the measured
// envelope (§9.5: ≤ 1 MiB screen-lifetime growth) releasing it per view
// would only force a refresh repaint, so it is kept.
//
// No raw HTML ever reaches a label: the description string came out of the
// shared description helper, and every other row is engine-parsed text.
// ---------------------------------------------------------------------------

static lv_obj_t *detail_screen, *content;
static audiobookshelfdetail_delegate_t actions;
static audiobookshelfdetail_refill_t refill;

// The open item's identity: values copied at open time (§9.2), never an
// index into the browse page's mutable 40-item array.
static char open_profile[ABS_PROFILE_ID_MAX];
static char open_library[ABS_ID_MAX];
static char open_item[ABS_ID_MAX];
static char open_title[ABS_NAME_MAX];

// The accepted state. All of it dies with the screen unload (§9.5: nothing
// persists after the screen is gone).
static audiobookshelf_detail_t *model;
static abs_detail_local_t local;
static char local_path[1280];
static char failure[256];
static bool screen_loaded;
static bool show_chapters;

// The cover (wave 2, §9.3): the cached file, the state of its decode, and
// the ONE retained cover image (§9.5). `cover_file` names a card file only
// -- the hashed cache name carries no credential.
static char cover_file[512];
static cover_image_t cover_pixels;
static bool cover_requested;   // decode queued on slot 108, result not collected yet
static bool cover_unavailable; // the decode finished and could not
static lv_obj_t *cover_widget, *cover_busy, *cover_note;
static int cover_box_w, cover_box_h;
static lv_timer_t *cover_timer;

static void draw(void);

// Lets go of the whole cover state: the decode slot (a queued, running or
// decoded result there is stale/free in every path that gets here), the one
// retained image, and the file name. The widget pointers ride the next
// draw()'s rebuild.
static void cover_release(void) {
	cover_requested = false;
	cover_unavailable = false;
	coverloader_release(COVERLOADER_ABS_DETAIL);
	cover_free(&cover_pixels); // NULL-safe: cover_free zeroes the struct
	cover_file[0] = '\0';
	cover_widget = cover_busy = cover_note = NULL;
}

// A new model's cover. Stale slot results and stale pixels die here first
// (§9.3); an empty path is the §9.2 text-only detail and requests nothing.
static void cover_accept(const char *path) {
	cover_release();
	snprintf(cover_file, sizeof(cover_file), "%s", path ? path : "");
	if (!cover_file[0]) return;
	coverloader_request_image(COVERLOADER_ABS_DETAIL, cover_file, cover_box_w, cover_box_h);
	cover_requested = true;
}

// The decode poll: coverloader finishes off-thread, this collects. A finished
// decode without pixels is the "Cover unavailable" state (§9.7's text-only
// fallback shape); a found one attaches the pixels and un-hides the widget.
static void cover_poll_cb(lv_timer_t *timer) {
	(void)timer;
	if (!screen_loaded || !cover_requested) return;

	cover_image_t image;
	bool finished = false;
	bool found = coverloader_take(COVERLOADER_ABS_DETAIL, &image, &finished);
	if (!finished) return; // still decoding; the poll is a locked no-op

	cover_requested = false;
	if (found) {
		// The pixels moved in: the slot keeps nothing (take's contract),
		// this screen keeps the ONE retained image.
		cover_pixels = image;
		cover_pixels.dsc.data = cover_pixels.pixels;
	} else {
		cover_unavailable = true;
	}
	if (cover_busy) lv_obj_set_hidden(cover_busy, true);
	if (found && cover_widget) {
		lv_image_set_src(cover_widget, &cover_pixels.dsc);
		lv_obj_set_hidden(cover_widget, false);
	} else if (!found && cover_note) {
		lv_obj_set_hidden(cover_note, false);
	}
}

// The tap on the cover: full-screen (§9.2 "tap for screen-fit view") of the
// SAME local cache file. §9.3's ownership rule runs here in order: the
// viewer's path and title are copied FIRST, the detail's pixels leave the
// screen (source off), the slot is released and the retained image freed --
// and only then does the viewer open, starting its own decode. One retained
// remote cover image across the whole pair, and, because the viewer's worker
// serializes its decodes and the detail's decode finished before the tap
// could even aim at a picture, the two never overlap.
static void cover_tap_cb(lv_event_t *e) {
	(void)e;
	if (!screen_loaded || !cover_file[0] || !cover_pixels.pixels) return;

	char file[sizeof(cover_file)];
	snprintf(file, sizeof(file), "%s", cover_file);
	char title[ABS_NAME_MAX];
	snprintf(title, sizeof(title), "%s", model && model->item.title[0] ? model->item.title : open_title);

	if (cover_widget) lv_image_set_src(cover_widget, NULL); // no dangling src while we free
	coverloader_release(COVERLOADER_ABS_DETAIL);
	cover_free(&cover_pixels);
	imageview_open_named(file, title);
	// The screen switch above unloads this screen (and so releases
	// everything else); nothing else may run after the viewer opens.
}

static void release_state(void) {
	cover_release();
	audiobookshelf_detail_free(model);
	model = NULL;
	local = ABS_DETAIL_LOCAL_NONE;
	local_path[0] = '\0';
	failure[0] = '\0';
	show_chapters = false;
}

// The two one-off note labels of the streaming pages (a centred dim line).
static void note(const char *key) {
	lv_obj_t *label = lv_label_create(content);
	lv_label_set_text(label, tr(key));
	lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(label, lv_pct(100));
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(label, &font_ui_22, 0);
	lv_obj_set_style_pad_ver(label, 22, 0);
}

static void note_text(const char *text) {
	lv_obj_t *label = lv_label_create(content);
	lv_label_set_text(label, text);
	lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(label, lv_pct(100));
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(label, &font_ui_22, 0);
	lv_obj_set_style_pad_ver(label, 22, 0);
}

// The item name, above everything else in every state.
static void title_label(const char *text) {
	lv_obj_t *label = lv_label_create(content);
	lv_label_set_text(label, text && text[0] ? text : tr("audiobookshelf_queue_untitled"));
	lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(label, lv_pct(100));
	lv_obj_add_style(label, &theme_style_text, 0);
	lv_obj_set_style_text_font(label, &font_ui_24, 0);
	lv_obj_set_style_pad_ver(label, 8, 0);
}

// One "label … value" card in the settings row shape. Rows with a value
// carry no chevron (settingsrow_add draws one only for value-less rows) and
// a NULL callback is a no-op click.
static void meta_row(const char *key, const char *value) {
	lv_obj_t *value_label = NULL;
	settingsrow_add(content, key, &value_label, NULL, NULL);
	if (value_label) {
		lv_label_set_text(value_label, value);
		lv_label_set_long_mode(value_label, LV_LABEL_LONG_DOT);
		lv_obj_set_width(value_label, lv_pct(55));
		lv_obj_set_height(value_label, lv_font_get_line_height(&font_ui_20));
		lv_obj_set_style_text_font(value_label, &font_ui_20, 0);
	}
}

// "1 h 12 min" / "43 min", the catalog row's own rounding of the duration.
static void format_duration(double seconds, char *out, size_t size) {
	if (seconds <= 0) {
		out[0] = '\0';
		return;
	}
	int minutes = (int)((seconds + 30.0) / 60.0);
	if (minutes < 1) {
		minutes = 1;
	}
	if (minutes < 60) {
		snprintf(out, size, "%d min", minutes);
	} else {
		snprintf(out, size, "%d h %02d min", minutes / 60, minutes % 60);
	}
}

// "42% read", or Finished -- §9.1's existing progress row, hidden when the
// server says nothing.
static void format_progress(char *out, size_t size) {
	if (model->item.finished) {
		snprintf(out, size, "%s", tr("audiobookshelf_detail_finished"));
	} else if (model->item.progress > 0 && model->item.duration > 0) {
		int percent = (int)(model->item.progress * 100.0 / model->item.duration);
		if (percent < 1) percent = 1;
		if (percent > 99) percent = 99;
		snprintf(out, size, tr("audiobookshelf_detail_progress"), percent);
	} else {
		out[0] = '\0';
	}
}

// The primary action (§9.2: Play local / Link existing / Queue download,
// one at a time) and the Downloads shortcut. Which pages they open belongs
// to the browse page through the delegate -- this screen never enqueues,
// links or plays itself.
static void action_forward_cb(lv_event_t *e) {
	intptr_t which = (intptr_t)lv_event_get_user_data(e);
	switch (which) {
	case 0: if (actions.on_play) actions.on_play(actions.user); break;
	case 1: if (actions.on_link) actions.on_link(actions.user); break;
	case 2: if (actions.on_queue) actions.on_queue(actions.user); break;
	default: audiobookshelfqueue_open(); break; // the Downloads shortcut
	}
}

static void draw_actions(void) {
	switch (local) {
	case ABS_DETAIL_LOCAL_PLAY:
		settingsrow_action(content, "audiobookshelf_detail_play_local", action_forward_cb, (void *)(intptr_t)0);
		break;
	case ABS_DETAIL_LOCAL_LINK:
		// "Link existing copy" is the string the old confirm's link button
		// already had (§9.2 keeps that action, not the dialog).
		settingsrow_action(content, "audiobookshelf_link", action_forward_cb, (void *)(intptr_t)1);
		break;
	default:
		// No unique local match, or a copy owned by another server: the
		// download lands in THIS server's own abs-<hash> directory and
		// touches nothing of the other copy (§9.2, §11.5).
		settingsrow_action(content, "audiobookshelf_detail_queue", action_forward_cb, (void *)(intptr_t)2);
		break;
	}
	settingsrow_action(content, "audiobookshelf_downloads", action_forward_cb, (void *)(intptr_t)3);
}

// The Summary|Chapters toggle. The active view draws like a pressed card;
// the inactive one like an ordinary one.
static void pill_cb(lv_event_t *e) {
	bool want_chapters = (intptr_t)lv_event_get_user_data(e) != 0;
	if (!model || show_chapters == want_chapters) return;
	show_chapters = want_chapters;
	draw();
}

static void pills_row(void) {
	lv_obj_t *bar = lv_obj_create(content);
	lv_obj_set_size(bar, lv_pct(100), 64);
	lv_obj_set_style_pad_all(bar, 0, 0);
	lv_obj_set_style_border_width(bar, 0, 0);
	lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
	lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
	lv_obj_set_style_pad_column(bar, 8, 0);
	lv_obj_set_scrollable(bar, false);
	for (int i = 0; i < 2; i++) {
		bool active = show_chapters == (i == 1);
		lv_obj_t *pill = lv_btn_create(bar);
		lv_obj_set_height(pill, 60);
		lv_obj_set_flex_grow(pill, 1);
		lv_obj_add_style(pill, active ? &theme_style_card_pressed : &theme_style_card, 0);
		lv_obj_add_style(pill, &theme_style_card_pressed, LV_STATE_PRESSED);
		lv_obj_add_event_cb(pill, pill_cb, LV_EVENT_CLICKED, (void *)(intptr_t)(i == 1));
		lv_obj_t *label = lv_label_create(pill);
		lv_label_set_text(label, tr(i == 0 ? "audiobookshelf_detail_summary" : "audiobookshelf_detail_chapters"));
		lv_obj_add_style(label, &theme_style_text, 0);
		lv_obj_set_style_text_font(label, &font_ui_18, 0);
		lv_obj_center(label);
	}
}

// The clean description (§9.1): engine-normalized plain text, wrapped. An
// item without a description notes it rather than showing a silent gap.
static void draw_summary(void) {
	settingsrow_heading(content, "audiobookshelf_detail_about");
	if (model->description[0]) {
		lv_obj_t *about = lv_label_create(content);
		lv_label_set_text(about, model->description);
		lv_label_set_long_mode(about, LV_LABEL_LONG_WRAP);
		lv_obj_set_width(about, lv_pct(100));
		lv_obj_add_style(about, &theme_style_text, 0);
		lv_obj_set_style_text_font(about, &font_ui_20, 0);
		lv_obj_set_style_pad_ver(about, 4, 0);
	} else {
		note("audiobookshelf_detail_no_description");
	}
}

// "1:23" / "1:02:03" chapter stamps, the h:mm:ss shape once past the hour.
static void format_stamp(double seconds, char *out, size_t size) {
	long long total = (long long)(seconds < 0 ? 0 : seconds + 0.5);
	if (total >= 3600) snprintf(out, size, "%lld:%02lld:%02lld", total / 3600, (total / 60) % 60, total % 60);
	else snprintf(out, size, "%lld:%02lld", total / 60, total % 60);
}

static void chapter_row_bind(void *user, int index, char *title, size_t title_size,
							 char *subtitle, size_t subtitle_size) {
	(void)user;
	if (title_size) title[0] = '\0';
	if (subtitle_size) subtitle[0] = '\0';
	if (!model || !model->chapters) return;
	if (index < 0 || index >= model->chapter_count + (model->chapters_overflow ? 1 : 0)) return;
	// The overflow row is the +1 virtual row past the kept chapters (§9.5:
	// the first 400 with an explicit overflow row).
	if (model->chapters_overflow && index == model->chapter_count) {
		snprintf(title, title_size, "%s", tr("audiobookshelf_detail_chapters_overflow"));
		return;
	}
	const abs_detail_chapter_t *ch = &model->chapters[index];
	if (ch->title[0]) snprintf(title, title_size, "%s", ch->title);
	else snprintf(title, title_size, "%d", index + 1);
	// An invalid start/end pair arrived as an untimed title (§9.1: times
	// zeroed, title kept); a real 0–0 pair loses its stamps too -- harmless.
	if (ch->end > 0 || ch->start > 0) {
		char from[24], to[24];
		format_stamp(ch->start, from, sizeof(from));
		format_stamp(ch->end, to, sizeof(to));
		snprintf(subtitle, subtitle_size, "%s – %s", from, to);
	}
}

// Display-only rows (§9.1): a chapter row offers nothing this wave.
static void chapter_row_click(void *user, int index, lv_obj_t *row) {
	(void)user;
	(void)index;
	(void)row;
}

// The Chapters view over the shared pooled renderer: any chapter count
// costs the same twelve rows; scrolling rebinds, never allocates.
static void draw_chapters(void) {
	if (!model->chapters || model->chapter_count == 0) {
		note("audiobookshelf_detail_chapters_none");
		return;
	}
	abs_list_config_t cfg = {
		.parent = content,
		.row_height = 100,
		.row_gap = 8,
		.bind = chapter_row_bind,
		.click = chapter_row_click,
		.user = NULL,
	};
	abs_list_t *list = abs_list_create(&cfg);
	if (!list) return;
	abs_list_set_count(list, model->chapter_count + (model->chapters_overflow ? 1 : 0));
}

// The cover block (§9.2, between the summary|chapters content and the
// actions): a card the shape of the §9.5 contain view, showing either the
// decoded picture (tappable), the spinner while the decode runs, or the
// "Cover unavailable" row. No block at all when the §9.2 detail is
// text-only: the cover was never fetched, failed to fetch, or the item has
// none on the server.
static void draw_cover_block(void) {
	cover_widget = cover_busy = cover_note = NULL;
	if (!cover_file[0]) return;

	lv_obj_t *box = lv_obj_create(content);
	lv_obj_set_size(box, cover_box_w, cover_box_h);
	lv_obj_set_style_pad_all(box, 0, 0);
	lv_obj_add_style(box, &theme_style_card, 0);
	lv_obj_set_style_border_width(box, 0, 0);
	lv_obj_set_style_shadow_width(box, 0, 0);
	lv_obj_set_style_radius(box, 12, 0);
	lv_obj_set_scrollable(box, false);
	// A tap aims at the whole card once a picture is in it; while the decode
	// runs the callback guards itself closed.
	lv_obj_add_event_cb(box, cover_tap_cb, LV_EVENT_CLICKED, NULL);

	if (cover_requested && !cover_pixels.pixels) {
		cover_busy = spinner_create(box, &icon_loader_big);
		lv_obj_center(cover_busy);
	} else if (cover_unavailable) {
		cover_note = lv_label_create(box);
		lv_label_set_text(cover_note, tr("audiobookshelf_detail_cover_unavailable"));
		lv_label_set_long_mode(cover_note, LV_LABEL_LONG_WRAP);
		lv_obj_set_width(cover_note, lv_pct(90));
		lv_obj_set_style_text_align(cover_note, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_add_style(cover_note, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(cover_note, &font_ui_20, 0);
		lv_obj_center(cover_note);
	}

	if (cover_pixels.pixels) {
		cover_widget = lv_image_create(box);
		lv_image_set_src(cover_widget, &cover_pixels.dsc);
		lv_obj_center(cover_widget);
	}
}

static void draw(void) {
	lv_obj_clean(content);

	if (!model) {
		// The loading/failure state. Back (the page chevron and the swipe)
		// is page chrome and stays available in every state (§9.2).
		title_label(open_title);
		if (failure[0]) note_text(failure);
		else note("audiobookshelf_detail_loading");
		return;
	}

	title_label(model->item.title);

	char value[ABS_NAME_MAX + 40];
	if (model->item.author[0]) meta_row("audiobookshelf_detail_author", model->item.author);
	if (model->narrator[0]) meta_row("audiobookshelf_detail_narrator", model->narrator);
	if (model->item.duration > 0) {
		format_duration(model->item.duration, value, sizeof(value));
		meta_row("audiobookshelf_detail_duration", value);
	}
	if (model->item.series[0]) {
		// The series position rides after the name verbatim ("1.5" is a
		// string on the wire, §9.1 -- never an integer cast).
		if (model->series_part[0])
			snprintf(value, sizeof(value), "%s · %s", model->item.series, model->series_part);
		else
			snprintf(value, sizeof(value), "%s", model->item.series);
		meta_row("audiobookshelf_detail_series", value);
	}
	if (model->item.progress > 0 || model->item.finished) {
		format_progress(value, sizeof(value));
		if (value[0]) meta_row("audiobookshelf_detail_existing_progress", value);
	}

	pills_row();
	if (show_chapters) draw_chapters();
	else draw_summary();
	draw_cover_block();
	draw_actions();
}

static void loaded_cb(lv_event_t *e) {
	(void)e;
	screen_loaded = true;
	draw();
	// §9.2: the LOADED event is the single JOB_DETAIL submission path. An
	// open item with no model (first open, a return after the unload freed
	// it, or a failed fetch) refills through the page's hook; skip it when
	// the identity is gone (nothing was ever opened).
	if (open_item[0] && (!model || failure[0]) && refill.request) refill.request(refill.user);
}

static void unloaded_cb(lv_event_t *e) {
	(void)e;
	screen_loaded = false;
	// §9.5: the model is not held while the user is elsewhere; a return
	// re-fetches through the page's refill hook (one extra request is the
	// accepted ADR-3 shape).
	release_state();
}

void audiobookshelfdetail_init(gui_config_t *cfg, const audiobookshelfdetail_delegate_t *delegate) {
	if (delegate) actions = *delegate;
	// The §9.5 contain shape: the screen's content width, at the 450:640
	// shape the cover view is fitted to, clamped to the screen the fit runs
	// in (a small display shows a smaller picture, not an oversized box).
	cover_box_w = cfg->screen_width - 2 * cfg->padding;
	cover_box_h = cover_box_w * 640 / 450;
	int cover_max_h = cfg->screen_height - settingsrow_content_top(cfg) - 2 * cfg->padding;
	if (cover_box_h > cover_max_h) cover_box_h = cover_max_h;
	cover_timer = lv_timer_create(cover_poll_cb, 100, NULL);
	detail_screen = lv_obj_create(NULL);
	lv_obj_add_style(detail_screen, &theme_style_screen, 0);
	content = settingsrow_page(detail_screen, cfg, "audiobookshelf_detail");
	lv_obj_add_event_cb(detail_screen, loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(detail_screen, unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
	lv_obj_add_event_cb(detail_screen, unloaded_cb, LV_EVENT_DELETE, NULL);
	// Rows sit on the scrolling content, the swipe-home gesture rides the
	// screen: both attached, like every streaming page.
	switcher_attach_back_gesture(detail_screen);
	player_sheet_attach_drag(detail_screen, true);
	player_sheet_attach_drag(content, true);
	draw();
}

void audiobookshelfdetail_set_refill(const audiobookshelfdetail_refill_t *in) {
	if (in) refill = *in;
	else refill = (audiobookshelfdetail_refill_t){0};
}

void audiobookshelfdetail_open(const char *profile_id, const char *library_id,
							   const char *item_id, const char *title) {
	// A second open never rides on the first one's model: only the identity
	// moves between taps (§9.2), and the fetch for it has not run yet.
	release_state();
	snprintf(open_profile, sizeof(open_profile), "%s", profile_id ? profile_id : "");
	snprintf(open_library, sizeof(open_library), "%s", library_id ? library_id : "");
	snprintf(open_item, sizeof(open_item), "%s", item_id ? item_id : "");
	snprintf(open_title, sizeof(open_title), "%s", title ? title : "");
	if (detail_screen) switch_screen(detail_screen);
}

bool audiobookshelfdetail_identity(char *profile_id, size_t profile_size,
								   char *library_id, size_t library_size,
								   char *item_id, size_t item_size,
								   char *title, size_t title_size) {
	if (!open_item[0]) {
		if (profile_size) profile_id[0] = '\0';
		if (library_size) library_id[0] = '\0';
		if (item_size) item_id[0] = '\0';
		if (title_size) title[0] = '\0';
		return false;
	}
	if (profile_id && profile_size) snprintf(profile_id, profile_size, "%s", open_profile);
	if (library_id && library_size) snprintf(library_id, library_size, "%s", open_library);
	if (item_id && item_size) snprintf(item_id, item_size, "%s", open_item);
	if (title && title_size) snprintf(title, title_size, "%s", open_title);
	// The request identity is the item id; profile/library ride along as the
	// §9.2 value copies (and may be empty in the §11.1 migration window).
	return open_item[0] != '\0';
}

bool audiobookshelfdetail_receive(const audiobookshelfdetail_receive_t *in) {
	if (!in) return false;
	if (!screen_loaded) {
		// The user left before the completion landed: the model is dropped
		// on the spot (§9.3), and returning re-fetches through the page.
		audiobookshelf_detail_free(in->detail);
		return false;
	}
	local = in->local;
	snprintf(local_path, sizeof(local_path), "%s", in->local_path ? in->local_path : "");
	if (!in->detail) {
		audiobookshelf_detail_free(in->detail); // NULL-safe by contract
		// §9.2: a fetch failure is the text-only detail; nothing from the
		// previous state (cover included) rides on under the error.
		cover_accept(NULL);
		snprintf(failure, sizeof(failure), "%s",
				 in->error && in->error[0] ? in->error : tr("audiobookshelf_detail_failed"));
		draw();
		return false;
	}
	model = in->detail;
	failure[0] = '\0';
	// §9.1 wave 2: accept the cached cover file and start its off-thread
	// decode (§9.4's slot 108). Stale pixels die inside cover_accept.
	cover_accept(in->cover_path);
	draw();
	return true;
}

void audiobookshelfdetail_forget(void) {
	release_state();
	open_profile[0] = '\0';
	open_library[0] = '\0';
	open_item[0] = '\0';
	open_title[0] = '\0';
	if (screen_loaded) draw();
}
