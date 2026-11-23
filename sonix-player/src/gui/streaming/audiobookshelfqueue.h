#ifndef AUDIOBOOKSHELFQUEUE_H
#define AUDIOBOOKSHELFQUEUE_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// ---------------------------------------------------------------------------
// Workstream 2 — the download-queue screen (spec §8.1, §8.5 line 579).
//
// Renders the card's download queue: one pooled `abslist` row per job —
// scrolling rebinds rows and never adds LVGL objects — with the title, the
// state and the per-file progress, a per-item action menu (pause, resume,
// cancel, remove, retry, per the §8.1 allowed-actions table) and the
// clear-finished action.
//
// Knowledge boundaries: this file holds no HTTP and no SQLite. Every state
// read is `abs_download_snapshot()` — one copy per 4 Hz timer tick, taken into
// one screen-lifetime buffer and repainted only when it actually changed
// (coalesced, generation-tagged; the worker never posts an LVGL event for
// reads). Job rows are engine mirrors; nothing here retains a book or show
// model (§8.1 line 685/693: item identity only).
// ---------------------------------------------------------------------------

void audiobookshelfqueue_init(gui_config_t *cfg);

// The section's screen, for pages that embed links to it.
lv_obj_t *audiobookshelfqueue_screen(void);

// Opens the queue (the download confirm on the browse page, the "Downloads"
// row on the libraries view). The screen rebinds its rows from a fresh
// snapshot on load.
void audiobookshelfqueue_open(void);

#endif /* AUDIOBOOKSHELFQUEUE_H */
