#include "morepage.h"

#include "lvgl/lvgl.h"

#include "src/gui/audio/dacpage.h"
#include "src/gui/audio/metronomepage.h"
#include "src/gui/ebook/ebookpage.h"
#include "src/gui/flappybird/flappybird.h"
#include "src/gui/library/audiobooks.h"
#include "src/gui/library/filespage.h"
#include "src/gui/gearboy/gearboypage.h"
#include "src/gui/shell/gridpage.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/wireless/wireless.h"

lv_obj_t *morepage_screen;

static void more_loaded_cb(lv_event_t *e) {
	gridpage_hint_scroll(lv_event_get_user_data(e));
}

void morepage_init(gui_config_t *cfg) {
	const grid_entry_t entries[] = {
		{"wireless", &icon_menu_wireless, &wireless_screen, NULL},
		{"audiobooks", &icon_menu_audiobooks, &audiobooks_screen, NULL},
		{"dac", &icon_menu_dac, &dacpage_screen, NULL},
#if GB_CORE
		// The Game Boy emulator. Present only when the binary was built with
		// GB=1 (see the Makefile).
		{"gearboy", &icon_menu_gearboy, &gearboypage_screen, NULL},
#endif
		// An action rather than a screen: the shelf reads the Ebook folder again
		// every time it is opened, so a book copied onto the card turns up
		// without a restart.
		{"books", &icon_menu_books, NULL, ebookpage_open},
		// An action for the same reason the shelf is one: the listing is read
		// when the page is entered, so a card that changed under the player
		// shows what is on it now.
		{"file_explorer", &icon_menu_file_explorer, NULL, filespage_open},
		// An action: the artwork is read from the resource tree on the way in.
		{"flappy_bird", &icon_menu_flappy_bird, NULL, flappybird_open},
		{"metronome", &icon_menu_metronome, &metronomepage_screen, NULL},
	};

	// Keep the usual tile size and scroll to the remaining tools.
	lv_obj_t *grid = gridpage_build(morepage_screen, cfg, entries, (int)(sizeof(entries) / sizeof(entries[0])), 2, 3, true);
	gridpage_set_scrolling(grid);
	lv_obj_add_event_cb(morepage_screen, more_loaded_cb, LV_EVENT_SCREEN_LOADED, grid);

	settingsrow_title(morepage_screen, cfg, "more");
}
