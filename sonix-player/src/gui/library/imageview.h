#ifndef IMAGEVIEW_H
#define IMAGEVIEW_H

#include <stdbool.h>

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// A picture on a page of its own: the file manager opens JPEG, PNG and GIF
// files here (the first frame of a GIF). The picture is fitted whole into the
// page below the heading, never enlarged, and decoded on a worker thread of
// its own so a large photo does not hold up the interface; the page is up at
// once, with a spinner until the picture is ready.
//
// That worker is one persistent thread, not a thread per open: a second open
// while a decode runs only queues the newer path -- this page's decodes
// never overlap, and the generation each open bumps frees any result that
// landed for a picture nobody waits for (§9.4: the Audiobookshelf detail and
// full-screen views of one cover must not run two decodes, or keep two sets
// of pixels).
//
// Locally held files only: nothing here fetches from a URL. Cover bytes that
// arrived from a streaming server were fetched, size-capped and cached
// elsewhere (abs-cover-cache), then named by their local path.

extern lv_obj_t *imageview_screen;

void imageview_init(gui_config_t *cfg);

// Switches to the page and starts decoding `path`. When the file turns out
// not to be a picture this player can decode, the page says so instead.
void imageview_open(const char *path);

// The same, with the heading showing `title` instead of the file's name: the
// Audiobookshelf detail opens its cached cover this way (§9.2), under the
// book's own title rather than a hashed cache-file name.
void imageview_open_named(const char *path, const char *title);

#endif /* IMAGEVIEW_H */
