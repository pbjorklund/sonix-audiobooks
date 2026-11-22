#include "audiobookshelfprofiles.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "src/gui/shell/icons.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/gui/fonts/fonts.h"
#include "src/system/core/lang.h"
#include "src/system/streaming/abs_profiles.h"

// The green of the language page's chosen-language tick: the "this is the one
// in use" colour, so a marked profile row reads like a language row.
#define PICKER_CHECK_GREEN lv_color_make(46, 194, 126)

static lv_obj_t *picker_screen;
static lv_obj_t *picker_list;
static audiobookshelfprofiles_delegate_t module_delegate;

// What a row needs after the store read: identity for selecting and display
// text. Deliberately not abs_profile_t -- the store's token bytes are dropped
// on the way through and never retained anywhere in this module (§11.6).
typedef struct {
	char id[ABS_PROFILE_ID_MAX];
	char name[ABS_PROFILE_NAME_MAX];
	char server[ABS_SERVER_MAX];
} picker_entry_t;

static picker_entry_t entries[ABS_PROFILE_MAX];
static int entry_count;
static lv_obj_t *marks[ABS_PROFILE_MAX];

static void select_cb(lv_event_t *e);
static void open_setup_cb(lv_event_t *e);

// The marks are painted from the store's active id: the id is the durable
// identity, the row order is only this screen's display order.
static void paint_checks(void) {
	char active_id[ABS_PROFILE_ID_MAX];
	abs_profiles_active(active_id, sizeof(active_id));
	for (int i = 0; i < entry_count; i++) {
		if (marks[i]) lv_obj_set_hidden(marks[i], strcmp(entries[i].id, active_id) != 0);
	}
}

// Redraws the whole list. Runs on init and on every entry; a selection repaints
// only the checks, so a switch does not flash the list.
static void draw(void) {
	lv_obj_clean(picker_list);

	abs_profile_t profiles[ABS_PROFILE_MAX];
	int count = abs_profiles_list(profiles, ABS_PROFILE_MAX);
	entry_count = 0;
	for (int i = 0; i < count && entry_count < ABS_PROFILE_MAX; i++) {
		picker_entry_t *entry = &entries[entry_count++];
		// Same-size fixed fields, copied directly: an snprintf("%s") from one
		// fixed buffer into another of the same size is correct but makes GCC
		// suspect truncation (-Wformat-truncation) in this very same-size case.
		memcpy(entry->id, profiles[i].id, sizeof(entry->id));
		memcpy(entry->name, profiles[i].name, sizeof(entry->name));
		memcpy(entry->server, profiles[i].server, sizeof(entry->server));
		// The token bytes served their one pass through this copy; drop them.
		memset(profiles[i].token, 0, sizeof(profiles[i].token));
	}

	for (int i = 0; i < entry_count; i++) {
		lv_obj_t *row = settingsrow_action(picker_list, entries[i].name[0] ? entries[i].name
																		   : entries[i].server,
										   select_cb, (void *)(intptr_t)i);
		// A profile set up late may carry a long URL where its name would be;
		// wrap like a search result rather than stepping the font down.
		settingsrow_name_lines(row, 2);
		lv_obj_t *mark = lv_image_create(row);
		lv_image_set_src(mark, &icon_check);
		lv_obj_set_style_image_recolor(mark, PICKER_CHECK_GREEN, 0);
		lv_obj_set_style_image_recolor_opa(mark, LV_OPA_COVER, 0);
		lv_obj_align(mark, LV_ALIGN_RIGHT_MID, 0, 0);
		lv_obj_set_hidden(mark, true);
		marks[i] = mark;
	}

	if (module_delegate.on_setup)
		settingsrow_action(picker_list, "audiobookshelf_profile_add", open_setup_cb, NULL);

	lv_obj_t *note = lv_label_create(picker_list);
	lv_label_set_text(note, tr("audiobookshelf_profile_note"));
	lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(note, lv_pct(100));
	lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(note, &font_ui_22, 0);
	lv_obj_set_style_pad_ver(note, 22, 0);

	paint_checks();
}

static void picker_loaded_cb(lv_event_t *e) {
	(void)e;
	draw();
}

static void select_cb(lv_event_t *e) {
	int index = (int)(intptr_t)lv_event_get_user_data(e);
	if (index < 0 || index >= entry_count) return;

	char active_id[ABS_PROFILE_ID_MAX];
	abs_profiles_active(active_id, sizeof(active_id));
	if (strcmp(entries[index].id, active_id) == 0) return;

	// The store persists before the page is told: the active id is what every
	// browse request will read, so the switch must be on file first (§11.4).
	// A failing save leaves the previous id in place, and the store lock covers
	// the memory flip and the file write together.
	if (!abs_profiles_select(entries[index].id)) {
		toast_error("audiobookshelf_profile_select_failed");
		return;
	}
	paint_checks();
	if (module_delegate.on_selected) module_delegate.on_selected(module_delegate.user);
}

static void open_setup_cb(lv_event_t *e) {
	(void)e;
	if (module_delegate.on_setup) module_delegate.on_setup(module_delegate.user);
}

lv_obj_t *audiobookshelfprofiles_screen(void) { return picker_screen; }

lv_obj_t *audiobookshelfprofiles_init(gui_config_t *cfg,
									  const audiobookshelfprofiles_delegate_t *delegate) {
	if (delegate) module_delegate = *delegate;
	picker_screen = lv_obj_create(NULL);
	picker_list = settingsrow_page(picker_screen, cfg, "audiobookshelf_profiles");
	// settingsrow_page() only gestures on the container; row taps land on the
	// screen's empty margins, and the setup page wires the screen for the same
	// reason.
	switcher_attach_back_gesture(picker_screen);
	lv_obj_add_event_cb(picker_screen, picker_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	draw();
	return picker_screen;
}
