#ifndef ABSLIST_H
#define ABSLIST_H

#include <stdbool.h>
#include <stddef.h>

#include "lvgl/lvgl.h"

// Bounded row-pool renderer (§10.2/§10.3). A list of any length is shown with
// ABS_LIST_ROW_POOL row widgets: the pool rows are re-bound while the user
// scrolls, so scrolling changes row binding, never the number of LVGL
// objects. A 100-item search page costs the same tree as a 40-item book page.
//
// The renderer knows nothing about HTTP or Audiobookshelf ids: the owner
// hands it a row count and fills each visible row by data index.
//
// One instance owns one absolutely-positioned canvas whose height equals the
// full data height, so the host page scrolls normally; the pooled rows sit at
// their data slots inside it. A single note label covers the empty, loading
// and error states -- whatever the owner wants to say instead of rows.

// §10.2 pool size (10 rows would be the large-text fallback; 12 is the
// shipped value).
#define ABS_LIST_ROW_POOL 12

typedef struct abs_list abs_list_t;

// Fills one visible row. `index` is a data row. Write the title, and the
// subtitle or an empty string to hide the second line.
typedef void (*abs_list_bind_cb)(void *user, int index, char *title, size_t title_size,
								 char *subtitle, size_t subtitle_size);
// A row was tapped; carries the data index it was showing.
typedef void (*abs_list_click_cb)(void *user, int index);

typedef struct {
	lv_obj_t *parent;   // the page's scrolling container; the canvas joins it
	int row_height;     // one displayed row's height
	int row_gap;        // the gap between two displayed rows
	abs_list_bind_cb bind;
	abs_list_click_cb click;
	void *user;
} abs_list_config_t;

// Builds the canvas and the row pool. Row text and style match the streaming
// pages' card rows. Never returns rows beyond the pool.
abs_list_t *abs_list_create(const abs_list_config_t *cfg);

// Unload cleanup: removes the canvas (and every row with it) and detaches the
// scroll binding. Also runs when the host page's content container is cleaned.
void abs_list_destroy(abs_list_t *list);

// The number of data rows behind the canvas. Zero hides every row.
void abs_list_set_count(abs_list_t *list, int count);

// Replaces the rows with one centred dim message (loading, empty, error, "no
// matches"). NULL clears it and brings the rows back.
void abs_list_note(abs_list_t *list, const char *text);

// Returns to the first data window and rebinding.
void abs_list_scroll_top(abs_list_t *list);

#endif /* ABSLIST_H */
