#ifndef AUDIOBOOKSHELF_PROFILES_H
#define AUDIOBOOKSHELF_PROFILES_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// ---------------------------------------------------------------------------
// The Audiobookshelf profile picker (spec §11.4).
//
// One screen listing the configured profiles (up to four), the active one
// marked, and a tap on a row selects it. Selecting persists the active id
// through abs_profiles_select() on the spot, then reports it to the delegate
// so audiobookshelfpage.c can clear its browse state and load the newly
// active profile's libraries. No backend change rides along here: browse
// requests already resolve the active profile.
//
// §11.6 keeps tokens out of the picker: the store read is reduced to
// id/name/server copies before any of it is retained, and no editor snapshot
// exists in this piece. The row that leads to adding a server opens the page
// that owns the setup screen, via the delegate.
// ---------------------------------------------------------------------------

typedef struct {
	void (*on_selected)(void *user); // a selection was persisted as the new active profile
	void (*on_setup)(void *user);    // the "add a server" row was tapped
	void *user;                      // passed to both callbacks; may be NULL
} audiobookshelfprofiles_delegate_t;

// Builds the picker screen and copies the delegate, so the struct can be a
// local of the caller; the function pointers in it must stay valid for the
// run, as page statics always do.
lv_obj_t *audiobookshelfprofiles_init(gui_config_t *cfg,
									  const audiobookshelfprofiles_delegate_t *delegate);

// The screen, for switch_screen().
lv_obj_t *audiobookshelfprofiles_screen(void);

// The rows rebuild from the profile store every time the screen becomes
// visible, so store changes made elsewhere show up on the next visit.

#endif /* AUDIOBOOKSHELF_PROFILES_H */
