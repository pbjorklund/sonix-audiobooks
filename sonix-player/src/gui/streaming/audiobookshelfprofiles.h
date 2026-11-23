#ifndef AUDIOBOOKSHELF_PROFILES_H
#define AUDIOBOOKSHELF_PROFILES_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// ---------------------------------------------------------------------------
// The Audiobookshelf profile picker and editor (spec §11.4, §11.6).
//
// One screen listing the configured profiles (up to four), the active one
// marked, a tap on a row selecting it, an "Add a server" row, and per-row
// edit/remove buttons. Selecting persists the active id through
// abs_profiles_select() on the spot; adding, editing and removing go through
// the store's own calls, so every change is on file before any delegate is
// told about it. No backend change rides along here: browse requests already
// resolve the active profile per request.
//
// The focused editor (§11.6) lives in this module too: one screen for Add and
// Edit with name, server (immutable after creation) and token fields plus the
// shared keyboard. Tokens are never read back into a field -- a blank token on
// save means "keep" -- and the store read is reduced to id/name/server copies
// before any of it is retained (§11.6).
// ---------------------------------------------------------------------------

typedef struct {
	// A selection was persisted as the new active profile, or a newly added one
	// became active: clear remote list/detail state and load its libraries (the
	// load doubles as validation; a failure pops over the picker).
	void (*on_selected)(void *user);
	// An edit saved against the ACTIVE profile -- possibly with a new token:
	// validate it in place via GET /api/libraries while the editor stays up. On
	// success step back to the picker and toast; on failure the error popup
	// lands over the editor so the value can be corrected and re-saved.
	void (*on_validate)(void *user);
	// The active profile was removed (other profiles may remain): clear remote
	// state and show the unconfigured view. Selecting a remaining profile is a
	// deliberate tap, never an implicit switch (§11.4).
	void (*on_active_lost)(void *user);
	void *user; // passed to every callback; may be NULL
} audiobookshelfprofiles_delegate_t;

// Builds the picker screen, copies the delegate, and builds the editor. The
// struct can be a local of the caller; the function pointers in it must stay
// valid for the run, as page statics always do.
lv_obj_t *audiobookshelfprofiles_init(gui_config_t *cfg,
									  const audiobookshelfprofiles_delegate_t *delegate);

// The picker screen, for switch_screen(). Its rows rebuild from the profile
// store every time it becomes visible, so store changes made elsewhere show up
// on the next visit.
lv_obj_t *audiobookshelfprofiles_screen(void);

// The editor screen: the page's test-job completion path needs to know whether
// a validation succeeded while it was up (then step back) or failed over it.
lv_obj_t *audiobookshelfprofiles_editor_screen(void);

#endif /* AUDIOBOOKSHELF_PROFILES_H */
