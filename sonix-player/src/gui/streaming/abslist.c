#include "abslist.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/theme.h"

// ---------------------------------------------------------------------------
// Row pool geometry.
//
// The canvas is a plain (non-flex) child of the page's scrolling container
// with the full data height, so the page scrolls normally. Every pool row is
// a card pinned to its data slot (index * pitch) inside the canvas: scrolling
// moves whole slots under the viewport exactly like a real list, and the
// scroll event re-binds the pool's texts one slot before a row fully leaves
// the viewport. The row widgets themselves never move while scrolling, and
// the tree never grows past the pool -- a 100-result search page costs the
// same twelve rows as a 40-item book page.
//
// One note label rides on the canvas for the loading/empty/error states.
// The renderer knows nothing about HTTP or ids: the owner refills rows by
// data index.
// ---------------------------------------------------------------------------

#define ABS_LIST_NOTE_SLOT ABS_LIST_ROW_POOL
#define ABS_LIST_BIND_BUF 256

struct abs_list {
	lv_obj_t *canvas;
	lv_obj_t *rows[ABS_LIST_ROW_POOL];
	int row_index[ABS_LIST_ROW_POOL]; // the data row each pool row is showing
	int count;
	int row_height;
	int pitch;
	bool noted;
	bool two_line;
	abs_list_bind_cb bind;
	abs_list_click_cb click;
	void *user;
};

static void abs_list_rebind(abs_list_t *list);

// Recomputes the visible window from the parent's scroll position and
// refills the pool from `first`: rows past the end of the data hide, every
// other row is re-bound in place at its slot.
static void abs_list_rebind(abs_list_t *list) {
	int scroll_y = lv_obj_get_scroll_y(lv_obj_get_parent(list->canvas));
	int top = scroll_y > 0 ? scroll_y / list->pitch : 0;
	// One row above the viewport top stays bound, so coverage never lags the
	// scroll by more than the row that is already leaving.
	int first = top > 0 ? top - 1 : 0;
	int window = ABS_LIST_ROW_POOL;
	if (first + window > list->count) window = list->count - first;
	if (window < 0) window = 0;

	for (int i = 0; i < ABS_LIST_ROW_POOL; i++) {
		lv_obj_t *row = list->rows[i];
		if (list->noted || i >= window) {
			lv_obj_set_hidden(row, true);
			continue;
		}
		int index = first + i;
		list->row_index[i] = index;
		lv_obj_set_y(row, index * list->pitch);
		lv_obj_set_hidden(row, false);
		if (list->bind) {
			char title[ABS_LIST_BIND_BUF], subtitle[ABS_LIST_BIND_BUF];
			list->bind(list->user, index, title, sizeof(title), subtitle, sizeof(subtitle));
			lv_label_set_text(lv_obj_get_child(row, 0), title);
			lv_obj_t *subtitle_label = lv_obj_get_child(row, 1);
			if (subtitle[0]) {
				lv_label_set_text(subtitle_label, subtitle);
				lv_obj_set_hidden(subtitle_label, false);
			} else {
				lv_label_set_text(subtitle_label, "");
				lv_obj_set_hidden(subtitle_label, true);
			}
		}
	}
	// The note shares the canvas and the scroll: on top of the rows when
	// shown, out of the way (and hidden) when not.
	lv_obj_t *note = lv_obj_get_child(list->canvas, ABS_LIST_NOTE_SLOT);
	if (note) {
		lv_obj_set_y(note, 24);
		if (list->noted) lv_obj_set_hidden(note, false);
		else lv_obj_set_hidden(note, true);
	}
}

static void scroll_cb(lv_event_t *e) {
	abs_list_rebind((abs_list_t *)lv_event_get_user_data(e));
}

static void row_clicked_cb(lv_event_t *e) {
	abs_list_t *list = (abs_list_t *)lv_event_get_user_data(e);
	lv_obj_t *row = lv_event_get_current_target(e);
	if (!list || list->noted) return;
	// The tap belongs to the row that carries the event; its bound data index
	// is current by construction (rebinding is synchronous with scrolling).
	for (int i = 0; i < ABS_LIST_ROW_POOL; i++) {
		if (list->rows[i] != row) continue;
		int index = list->row_index[i];
		if (index < 0 || index >= list->count) return;
		if (list->click) list->click(list->user, index);
		return;
	}
}

// One pooled row: the streaming pages' card row, title over a dim subtitle.
// Two-line shape for 112-high item cards; single centred line for the
// settingsrow-height group rows.
static lv_obj_t *make_row(abs_list_t *list) {
	lv_obj_t *row = lv_btn_create(list->canvas);
	lv_obj_set_size(row, lv_pct(100), list->row_height);
	lv_obj_add_style(row, &theme_style_card, 0);
	lv_obj_add_style(row, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_radius(row, 12, 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_obj_set_style_shadow_width(row, 0, 0);
	lv_obj_set_style_pad_hor(row, 20, 0);
	lv_obj_set_style_pad_ver(row, 10, 0);
	lv_obj_set_style_pad_row(row, 2, 0);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
	lv_obj_set_event_bubble(row, true);
	lv_obj_set_hidden(row, true);
	lv_obj_add_event_cb(row, row_clicked_cb, LV_EVENT_CLICKED, list);

	lv_obj_t *title = lv_label_create(row);
	lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
	lv_obj_set_width(title, lv_pct(100));
	lv_obj_add_style(title, &theme_style_text, 0);
	if (list->two_line) {
		lv_obj_set_style_text_font(title, &font_ui_22, 0);
		lv_obj_set_style_max_height(title, 2 * lv_font_get_line_height(&font_ui_22), 0);
	} else {
		lv_obj_set_style_text_font(title, &font_ui_20, 0);
	}
	lv_obj_set_event_bubble(title, true);

	lv_obj_t *subtitle = lv_label_create(row);
	lv_label_set_long_mode(subtitle, LV_LABEL_LONG_DOT);
	lv_obj_set_width(subtitle, lv_pct(100));
	lv_obj_add_style(subtitle, &theme_style_text_dim, 0);
	if (list->two_line) {
		lv_obj_set_height(subtitle, lv_font_get_line_height(&font_ui_18));
		lv_obj_set_style_text_font(subtitle, &font_ui_18, 0);
	} else {
		lv_obj_set_height(subtitle, 0); // collapses; the line stays centred
		lv_obj_set_style_text_font(subtitle, &font_ui_20, 0);
	}
	lv_obj_set_event_bubble(subtitle, true);
	lv_obj_set_hidden(subtitle, true);
	return row;
}

// The page's content container is cleaned on every redraw; the canvas delete
// event is where the instance memory and the parent scroll binding both go.
static void canvas_delete_cb(lv_event_t *e) {
	abs_list_t *list = (abs_list_t *)lv_event_get_user_data(e);
	if (!list) return;
	lv_obj_remove_event_cb_with_user_data(lv_obj_get_parent(list->canvas), scroll_cb, list);
	free(list);
}

abs_list_t *abs_list_create(const abs_list_config_t *cfg) {
	if (!cfg || !cfg->parent || cfg->row_height <= 0) return NULL;
	abs_list_t *list = calloc(1, sizeof(*list));
	if (!list) return NULL;
	list->row_height = cfg->row_height;
	list->pitch = list->row_height + (cfg->row_gap > 0 ? cfg->row_gap : 0);
	list->two_line = list->row_height >= 112;
	list->bind = cfg->bind;
	list->click = cfg->click;
	list->user = cfg->user;

	list->canvas = lv_obj_create(cfg->parent);
	// A plain spacer, not a themed card: every default style goes first so the
	// explicit sizes below are not stripped again.
	lv_obj_remove_style_all(list->canvas);
	lv_obj_set_width(list->canvas, lv_pct(100));
	lv_obj_set_height(list->canvas, 0); // sized by abs_list_set_count()
	lv_obj_set_style_pad_all(list->canvas, 0, 0);
	lv_obj_set_scrollable(list->canvas, false);
	lv_obj_set_event_bubble(list->canvas, true);

	for (int i = 0; i < ABS_LIST_ROW_POOL; i++) list->rows[i] = make_row(list);
	lv_obj_t *note = lv_label_create(list->canvas);
	lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(note, lv_pct(100));
	lv_label_set_text(note, "");
	lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(note, &font_ui_22, 0);
	lv_obj_set_style_pad_ver(note, 22, 0);
	lv_obj_set_style_pad_hor(note, 8, 0);
	lv_obj_set_y(note, 24);
	lv_obj_set_hidden(note, true);
	lv_obj_move_to_index(note, ABS_LIST_NOTE_SLOT);

	// Unload cleanup without a leak: when the host page's content container
	// is cleaned (or abs_list_destroy runs), the canvas dies and frees the
	// instance after detaching the scroll binding.
	lv_obj_add_event_cb(list->canvas, canvas_delete_cb, LV_EVENT_DELETE, list);
	lv_obj_add_event_cb(cfg->parent, scroll_cb, LV_EVENT_SCROLL, list);
	return list;
}

void abs_list_destroy(abs_list_t *list) {
	if (!list) return;
	// Removing the canvas also runs the DELETE callback, which detaches the
	// scroll binding and frees the instance.
	lv_obj_delete(list->canvas);
}

void abs_list_set_count(abs_list_t *list, int count) {
	if (!list || count < 0) return;
	list->count = count;
	// The canvas is the full data height: the last slot's bottom edge is
	// (count - 1) * pitch + row_height.
	int height = count > 0 ? (count - 1) * list->pitch + list->row_height : 0;
	lv_obj_set_height(list->canvas, height);
	if (!list->noted) abs_list_rebind(list);
}

void abs_list_note(abs_list_t *list, const char *text) {
	if (!list) return;
	list->noted = text && text[0];
	lv_obj_t *note = lv_obj_get_child(list->canvas, ABS_LIST_NOTE_SLOT);
	if (note) lv_label_set_text(note, list->noted ? text : "");
	abs_list_rebind(list);
}

void abs_list_scroll_top(abs_list_t *list) {
	if (!list) return;
	lv_obj_scroll_to_y(lv_obj_get_parent(list->canvas), 0, LV_ANIM_OFF);
	abs_list_rebind(list);
}
