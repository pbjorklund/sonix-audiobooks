#ifndef AUDIOBOOKSHELFDETAIL_H
#define AUDIOBOOKSHELFDETAIL_H

#include <stdbool.h>
#include <stddef.h>

#include "src/gui/shell/gui.h"
#include "src/system/streaming/audiobookshelf.h"

// ---------------------------------------------------------------------------
// Workstream 3 — the book-detail screen, wave 1, no cover (spec §9.2–§9.4).
//
// One pre-download page reached from the browse list: the §9.1 fields, a
// wrapped clean description, the Summary|Chapters toggle over a shared row
// pool, one primary local action and a Downloads shortcut.
//
// Ownership (§9.2/§9.3): `audiobookshelfdetail_open()` copies the tapped
// item's identity as VALUES -- profile id, library id, item id, display
// title -- and never holds an index into the browse page's mutable 40-item
// array. The expanded model the page's worker parses arrives through
// audiobookshelfdetail_receive(), which accepts it only while this screen is
// loaded: leaving (or a generation-stale completion, or a profile switch)
// frees it. Nothing here fetches anything: the page owns the JOB_DETAIL
// job, the local catalog scan and every action.
// ---------------------------------------------------------------------------

// What the worker's offline catalog scan decides about the tapped item
// (§9.2). PLAY only for a unique local copy whose manifest binds THIS
// server's item; a different-server manifest with the same item id offers
// download only -- never a link, never a play.
typedef enum {
	ABS_DETAIL_LOCAL_NONE = 0,       // no unique local match: download only
	ABS_DETAIL_LOCAL_PLAY,           // unique, linked to this server item
	ABS_DETAIL_LOCAL_LINK,           // unique, unbound or re-bindable here
	ABS_DETAIL_LOCAL_OTHER_SERVER,   // unique, owned by another server
} abs_detail_local_t;

// The actions belong to the browse page (which keeps the tapped identity and
// owns every job). Each runs on the LVGL thread.
typedef struct {
	void (*on_play)(void *user);  // local Play for a linked copy
	void (*on_link)(void *user);  // link the matched local copy
	void (*on_queue)(void *user); // queue the download (identity only)
	void *user;
} audiobookshelfdetail_delegate_t;

// Builds the screen and adopts the delegate. One-time, like the queue screen.
void audiobookshelfdetail_init(gui_config_t *cfg, const audiobookshelfdetail_delegate_t *delegate);

// The job refill belongs to the browse page: the screen's LOADED event calls
// exactly this hook when the open item still has no model (the single
// JOB_DETAIL submission path -- first open, and every re-entry after the
// user left, which freed the model, or landed a failure).
typedef struct {
	void (*request)(void *user);
	void *user;
} audiobookshelfdetail_refill_t;

void audiobookshelfdetail_set_refill(const audiobookshelfdetail_refill_t *refill);

// §9.2: the browse page hands over the tapped item's identity (values, not a
// page index) and this opens the detail screen in its loading state. The
// page submits the JOB_DETAIL fetch through the refill callback it
// registered at init -- the LOADED event is the single submission path, so a
// first open and a re-entry after leaving both fetch exactly once.
void audiobookshelfdetail_open(const char *profile_id, const char *library_id,
							   const char *item_id, const char *title);

// The page's refill hook reads back the open identity for its job: values
// copied out, never an index into the browse page. True when an item id is
// open (profile/library may legitimately be empty in the §11.1 migration
// window -- the request itself needs the item id). False when nothing is
// open (the page then simply does not fetch).
bool audiobookshelfdetail_identity(char *profile_id, size_t profile_size,
								   char *library_id, size_t library_size,
								   char *item_id, size_t item_size,
								   char *title, size_t title_size);

// The worker's completion, consumed once by the GUI thread: the parsed
// §9.3 model (ownership moves in), the sanitized failure text when the fetch
// failed, and the offline scan outcome. The model is accepted only while
// this screen is loaded; a completion that arrives after the user left is
// freed on the spot (a return re-fetches). Returns true when a model was
// accepted -- the caller then records the local match for its actions.
//
// `cover_path`, wave 2 (§9.1/§9.3): the cached authenticated cover file, a
// local hashed-name path with no credential in it. Empty means the §9.2
// text-only detail (no cover on the server, or the fetch failed); present
// means the screen decodes it off-thread (coverloader slot 108) into its
// 450x640-shape view, and a tap opens the full-screen viewer on the same
// file. Only ONE remote cover image stays retained across either of those,
// and every pixel dies with the screen unload or the generation it went
// stale in.
typedef struct {
	audiobookshelf_detail_t *detail; // NULL with `error` on failure
	const char *error;               // sanitized English, never a URL/token
	abs_detail_local_t local;
	const char *local_path;          // unique local copy, when one matched
	const char *cover_path;          // cached cover file, NULL/empty = text-only
} audiobookshelfdetail_receive_t;

bool audiobookshelfdetail_receive(const audiobookshelfdetail_receive_t *in);

// Clears everything the detail view holds (identity, model, scan outcome).
// The browse page calls it when the browsing session ends (profile switch /
// store clear, §11.4): the detail belongs to that session.
void audiobookshelfdetail_forget(void);

#endif /* AUDIOBOOKSHELFDETAIL_H */
