#ifndef AUDIOBOOKSHELF_H
#define AUDIOBOOKSHELF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The download queue's media surface (§8.2/§8.5) is declared against the
// queue's types: the ops receive jobs, file rows and plans, never tokens.
#include "abs_download.h"

#define ABS_ID_MAX 96
#define ABS_NAME_MAX 256
#define ABS_URL_MAX 1024
#define ABS_PAGE_LIMIT 40
#define ABS_TRACK_MAX 256

// §10.1 separate limits. A book list always takes a server page of 40 items;
// these are independent because the GUI's row pool (ABS_PAGE_LIMIT) could
// later differ from the server contract. Search asks the server for 101
// matches but keeps 100: the 101st usable match is `has_more` evidence, never
// a displayed row.
#define ABS_BROWSE_PAGE_LIMIT 40
#define ABS_SEARCH_REQUEST_LIMIT 101
#define ABS_SEARCH_DISPLAY_LIMIT 100

// §10.2 group bounds. The GUI shows one filterdata group as client-side pages
// of ABS_GROUP_UI_PAGE_LIMIT rows; the projection behind it is hard-bounded
// BEFORE allocation at ABS_FILTER_GROUP_LIMIT entries and
// ABS_FILTER_GROUP_BYTES_MAX bytes, so a multi-megabyte filterdata can never
// expand into an unbounded array -- past either bound the first bounded,
// pageable set is retained and the caller is told (TRUNCATED).
#define ABS_GROUP_UI_PAGE_LIMIT 40
#define ABS_FILTER_GROUP_LIMIT 512
#define ABS_FILTER_GROUP_BYTES_MAX (192 * 1024)

// Verified server sort mapping for book item queries (§10.1). The strings are
// what the reviewed Audiobookshelf route accepts; `sequence` is meaningful
// only under a series filter.
typedef enum {
	ABS_SORT_TITLE = 0,       // media.metadata.title
	ABS_SORT_AUTHOR,          // media.metadata.authorNameLF (last-name-first)
	ABS_SORT_ADDED_AT,        // addedAt ("recently added")
	ABS_SORT_DURATION,        // media.duration
	ABS_SORT_SERIES_POSITION, // sequence (series-filter default only)
} abs_browse_sort_t;

// One active group filter at a time (§10.1): ABS's item query accepts a
// single `filter=`, so 0.6.0 supports exactly one, combined with one sort.
typedef enum {
	ABS_FILTER_NONE = 0,
	ABS_FILTER_AUTHOR, // value: author object id
	ABS_FILTER_SERIES, // value: series object id
	ABS_FILTER_GENRE,  // value: the genre string itself (never an id, §10.1)
	ABS_FILTER_TAG,    // value: the tag string itself (never an id, §10.1)
} abs_browse_filter_t;

// Bounded browse query (§10.3): fixed-size fields, no allocation; pass by
// address. `page` is zero-based (§10.1); changing sort, direction, filter,
// library or search resets it, which is the caller's (GUI's) ownership.
typedef struct {
	char library_id[ABS_ID_MAX];
	abs_browse_sort_t sort;
	bool descending;
	abs_browse_filter_t filter;
	char filter_value[ABS_NAME_MAX]; // author/series id, or genre/tag string
	int page;
} abs_browse_query_t;

typedef struct {
	char id[ABS_ID_MAX];
	char name[ABS_NAME_MAX];
	char media_type[24];
	// Filter-group entries only (§10.2): a raw value that exceeds the
	// 256-byte filter query contract is never silently truncated into a
	// working-looking but wrong filter. The head of the value is kept for
	// display, `oversized` marks it unusable, and groups_ex() counts these
	// for the visible warning. Library listings always carry false.
	bool oversized;
} audiobookshelf_library_t;

typedef struct {
	char id[ABS_ID_MAX];
	char title[ABS_NAME_MAX];
	char author[ABS_NAME_MAX];
	char series[ABS_NAME_MAX];
	char cover[ABS_URL_MAX];
	double duration;
	double progress;
	bool finished;
	int tracks;
} audiobookshelf_item_t;

typedef struct {
	int index;
	char title[ABS_NAME_MAX];
	char filename[ABS_NAME_MAX];
	char content_url[ABS_URL_MAX];
	char mime[64];
	double start_offset;
	double duration;
	// The server's exact file size when it states one, else -1 (§8.2:
	// DOWNLOAD_FILE.expected_size). Content-Length is the transfer's own
	// check; this is what recovery compares a pre-existing file against.
	long long size;
} audiobookshelf_track_t;

typedef struct {
	audiobookshelf_item_t item;
	char description[8192];
	char series_part[32];
	long long progress_updated_ms;
	audiobookshelf_track_t tracks[ABS_TRACK_MAX];
	int track_count;
} audiobookshelf_book_t;

// ---------------------------------------------------------------------------
// The book-detail display model (§9.3): a small projection of one expanded
// item, deliberately separate from audiobookshelf_book_t. Display and
// download have different lifetimes and memory needs (ADR-3): the detail
// never carries the 256 inline download tracks -- the queue refetches
// media.tracks under its own auth snapshot -- and its chapters are exact
// heap allocations shown with an explicit overflow row past 400.
//
//   narrator           media.metadata.narratorName (the v2.3x `narrator`
//                      wire name the checked-in fixtures record is read as a
//                      fallback); the row hides when empty (§9.1)
//   series_part        the first series' `sequence` VERBATIM (§9.1: "1.5"
//                      survives; never an integer cast)
//   description        descriptionPlain preferred, else the raw description
//                      cleaned through the shared description helper: no
//                      LVGL markup, no raw publisher HTML, 8 KiB cap
//   chapters           media.chapters[] only (§9.1: never inferred from
//                      tracks); sorted by (start,id) for display -- this is
//                      the display projection and stays clear of the local
//                      playback chapter parsing
//   chapters_overflow  true when the server's chapter list was truncated at
//                      the cap, or its numChapters counts more than it sent
// ---------------------------------------------------------------------------
#define ABS_DETAIL_DESCRIPTION_MAX 8192
#define ABS_DETAIL_CHAPTER_MAX 400

typedef struct {
	int64_t id;
	double start, end;
	char title[192];
} abs_detail_chapter_t;

typedef struct {
	audiobookshelf_item_t item; // no inline tracks in this wrapper (§9.3)
	char narrator[ABS_NAME_MAX];
	char series_part[32];
	char description[ABS_DETAIL_DESCRIPTION_MAX + 1];
	abs_detail_chapter_t *chapters; // exact heap allocation, <= 400
	int chapter_count;
	bool chapters_overflow;
	// §9.5: the detail's JSON exceeded the shared 3 MiB body cap and arrived
	// truncated (http_last_truncated()). The model then carries the list-row
	// identity and actions with a visible "Details too large" note instead of
	// refusing the screen; the cap is never raised.
	bool details_too_large;
} audiobookshelf_detail_t;

// Fetches one item's expanded detail (GET /api/items/{id}?expanded=1&include=
// progress,authors -- the same request audiobookshelf_book() sends) into a
// CALLER-OWNED heap object; the worker owns it until an immutable GUI
// generation accepts it, and cancelled/stale completions free it (§9.3).
// NULL on any failure with audiobookshelf_last_error() set. Like
// audiobookshelf_book(), this buffers a large JSON body and belongs on a
// worker thread; it holds abs_catalog_lock for the duration (§8.3). A detail
// body cut at the 3 MiB cap is not a failure: it returns the model with
// details_too_large set, for §9.5's note-and-actions presentation.
audiobookshelf_detail_t *audiobookshelf_detail(const char *item_id);

// Releases the chapters and the object. NULL is allowed.
void audiobookshelf_detail_free(audiobookshelf_detail_t *detail);

// ---------------------------------------------------------------------------
// §9.1 wave 2, the authenticated cover. The detail cover is fetched by the
// same worker job that parsed the expanded model, under a credential snapshot
// resolved HERE first:
//
//   audiobookshelf_browse_credentials()
//       the browsing identity as a one-operation snapshot (§11.3): the exact
//       credential pair api_request() resolves -- the active profile's
//       server+token, or the 0.5.0 pair in the migration window. Tokens exist
//       only inside snapshots; wipe it (abs_profiles_snapshot_wipe) after the
//       one operation.
//
//   abs_cover_fetch()                       -- abs_cover.h
//       streams GET /api/items/{itemId}/cover?format=jpeg&width=900&height=1280
//       from the snapshot's own server into <card>/.local/abs-cover-cache/
//       <server-hash>-<item-hash>.jpg (same-origin by construction, 2 MiB
//       encoded cap, .part + checked close + atomic rename), then applies the
//       §9.3 LRU ceilings of eight files / 16 MiB.
//
// Any cover failure is the §9.2 text-only detail; it never fails the jobs
// whose metadata already succeeded.
// ---------------------------------------------------------------------------
bool audiobookshelf_browse_credentials(abs_auth_snapshot_t *out);

// Reads the saved server address and bearer token. Call after config_init().
void audiobookshelf_init(void);

bool audiobookshelf_configured(void);
const char *audiobookshelf_server(void);
void audiobookshelf_set_credentials(const char *server, const char *token);
void audiobookshelf_clear_credentials(void);

// All network calls block and belong on a worker thread.
int audiobookshelf_libraries(audiobookshelf_library_t *out, int max);
int audiobookshelf_items(const char *library_id, int page, audiobookshelf_item_t *out, int max, int *total_out);

// ---------------------------------------------------------------------------
// Bounded book browsing (§10.1, §10.3). The three building blocks:
//
//   abs_browse_sort_field()       the verified `sort=` string for one sort;
//                                 NULL for an out-of-range input
//   abs_browse_query_default_desc() the §10.1 default direction column: only
//                                 "recently added" opens newest-first
//   abs_browse_query_init()       validates bounds and fills the query
//                                 (series-position sort is refused without a
//                                 series filter); negative pages clamp to 0
//
// and one request:
//
//   audiobookshelf_browse()       GET /api/libraries/{id}/items?page={page}
//                                 &limit=40&sort={verified}&desc={0|1}
//                                 &minified=0&collapseseries=0&filter={encoded}
//                                 Uses the returned total for page controls.
//
// The filter is built with the same rules the request needs: standard padded
// Base64 ("+" and "/" are the standard alphabet, NOT Base64URL) that is then
// URL-encoded, so "+", "/" and "=" survive as %2B %2F %3D.
// ---------------------------------------------------------------------------
const char *abs_browse_sort_field(abs_browse_sort_t sort);
bool abs_browse_query_default_desc(abs_browse_sort_t sort);
bool abs_browse_query_init(abs_browse_query_t *query, const char *library_id,
						   abs_browse_sort_t sort, abs_browse_filter_t group,
						   const char *group_value, int page, bool descending);
// URL-encoded `<prefix>.<Base64(value)>` for the filter= parameter. False when
// the group needs a non-empty value that does not fit.
bool abs_browse_query_build_filter(abs_browse_filter_t group, const char *value,
								   char *out, size_t size);
int audiobookshelf_browse(const abs_browse_query_t *query, audiobookshelf_item_t *out,
						  int max, int *total_out);

// Library-scoped search (§10.1). The route has no offset/page/total and no
// sort/filter contract, so this always asks for ABS_SEARCH_REQUEST_LIMIT
// matches, retains at most min(max, ABS_SEARCH_DISPLAY_LIMIT) of them in
// server order, and reports `has_more` only when a usable 101st match exists.
// A count of exactly 100 without has_more means "showing up to 100; refine the
// search" -- never a "more matches found" claim.
int audiobookshelf_search_ex(const char *library_id, const char *query,
							 audiobookshelf_item_t *out, int max, bool *has_more);

// Legacy search entry point: identical wire behavior to 0.6.0 M1 (request
// limit = min(max, ABS_PAGE_LIMIT), no has_more). Superseded by
// audiobookshelf_search_ex() -- the GUI now uses the §10.1 entry points -- and
// kept as the M1 wire pin for the browse tests. Library-scoped (§10.1),
// whatever the old "server-wide" wording said; at most max matches (no server
// search pagination).
int audiobookshelf_search(const char *library_id, const char *query, audiobookshelf_item_t *out, int max);
// Caller owns the allocated, alphabetically sorted filter list.
int audiobookshelf_groups(const char *library_id, bool series, audiobookshelf_library_t **out);
// filterdata for one group (§10.1). Authors/series carry {id,name}; for
// genres and tags the returned `name` holds the string itself (reviewed
// servers send bare strings, the checked-in fixtures {id,name} objects -- the
// name is the filter value either way) and `id` stays empty: the value a
// genre/tag filter encodes is the name, never an object id. The returned list
// is alphabetically sorted and caller-owned (free()).
//
// §10.2 projection bounds: the response is bounded to the first
// ABS_FILTER_GROUP_LIMIT entries / ABS_FILTER_GROUP_BYTES_MAX of projection
// before anything is allocated; above either bound `*truncated_out` reports
// TRUNCATED and only the bounded first set arrives (in received order, then
// sorted). Entries whose raw filter value does not fit the 256-byte query
// contract are marked `oversized` and counted in `*oversized_out` for the
// caller's visible warning -- they are never silently truncated into a
// working-looking but wrong filter. Both outputs are optional (NULL skips).
int audiobookshelf_groups_ex(const char *library_id, abs_browse_filter_t group,
							 audiobookshelf_library_t **out, bool *truncated_out,
							 int *oversized_out);
int audiobookshelf_filtered_items(const char *library_id, int page, const char *filter,
	 audiobookshelf_item_t *out, int max, int *total_out);
// Legacy M1 items entry point, kept as the M1 wire pin for the browse tests;
// the GUI browses through audiobookshelf_browse() (§10.1). Raw (unencoded)
// `authors.|series.<Base64(id)>` filter value for that M1 flow, which
// URL-encodes the whole string later. The general building block for one
// already-encoded filter is abs_browse_query_build_filter().
void audiobookshelf_group_filter(bool series, const char *id, char *out, size_t size);
bool audiobookshelf_book(const char *item_id, audiobookshelf_book_t *out);

// Downloads one complete book into the server-qualified directory (§11.5)
//   <sd>/Audiobooks/Audiobookshelf/abs-<sha256(normalized-server)[0:12]>/<author>/<title>/
// and writes metadata sidecars understood by the local audiobook scanner.
// Existing 0.5.0-era books under the un-hashed layout are never moved or
// rewritten. Existing complete files are kept; temporary files end in .part
// and are not visible to the scanner. The destination is refused -- before any
// byte or directory is written -- when the hash-prefix directory already binds
// a different Audiobookshelf server, or when any downstream consumer (durable
// checkpoint row, progress watch slot) could not represent the path.
typedef void (*audiobookshelf_progress_cb)(int track, int tracks, long long done, long long total, void *user);
bool audiobookshelf_download(const audiobookshelf_book_t *book, const char *sd_root,
								 audiobookshelf_progress_cb cb, void *user, char *book_path, size_t path_size);

// ---------------------------------------------------------------------------
// The ABS download queue's real media surface (§8.2 steps 1–4, §8.5 split).
//
// One at a time: the queue worker owns exactly one active job (§8.1), so the
// resolved auth snapshot, the expanded book and the computed destination are
// module state valid for the active job's attempt — auth_resolve fills them,
// auth_forget wipes them (§8.2 step 8: freed before the next job). Every
// callback is safe to call only from the queue worker (or a single-threaded
// host driver); cancellable work polls ctx->cancel between units.
//
// The finalize op (§8.2 steps 5–7) covers both attempt shapes: the normal
// window after auth+fetch+files reuses the legacy writers (write_sidecars(),
// write_description(), local_track_name(), download_file() for the cover,
// audiobookshelf_resume_target()) and commits the manifest LAST atomically;
// a FINALIZING job adopted after a crash reconciles from the card instead —
// a valid manifest completes offline, an incomplete one retries finalization
// only. The acknowledgement reset (§8.2 step 8) resets book sync state
// (audiobookdb_abs_reset); episodes are the podcast wave's refusal.
// ---------------------------------------------------------------------------
const abs_download_ops_t *audiobookshelf_queue_media_ops(void);

// §8.3 line 556: registers the audiobookscan-completion listener that routes
// every completed Audiobooks scan into the queue's per-row import coordinator
// (abs_download_import_pending() over the index_pending DONE rows). Idempotent
// and process-lifetime, so one call at the queue's init boundary (§8.5, the
// ABS/storage lifecycle) covers boot and every card-attach re-init; the
// coordinator itself refuses while the queue is disarmed. No scan is started
// here and no page imports directly.
void audiobookshelf_queue_import_install(void);

// §8.2 step 1 / §8.1: the job's own credentials. Copies `profile_id`'s latest
// token under the profile lock (an inactive profile owns its own token) — or,
// only when the row carries no profile id, the active profile's — and rejects
// anything whose normalized server does not match `server`. A snapshot whose
// server drifted is never adopted: wipe, refuse. Tokens never leave here
// un-wiped; `error` receives a short sanitized English reason, never a URL
// or token.
bool audiobookshelf_queue_auth_resolve(const char *profile_id, const char *server,
									   abs_auth_snapshot_t *out, char *error, size_t error_size);

// Zeroes the snapshot bytes and the adapter's copy of the same state (the
// active job's expanded book and destination die with it, §8.2 step 8).
void audiobookshelf_queue_auth_forget(abs_auth_snapshot_t *snapshot);

// Maps Audiobookshelf's whole-book progress to the downloaded local track and
// its track-relative offset. Call after a successful download while `book` is
// still available; the scanner can then index the file before the caller saves
// the returned position in audiobookdb.
bool audiobookshelf_resume_target(const audiobookshelf_book_t *book, const char *book_path,
									 char *file_out, size_t file_size, double *seconds_out,
									 bool *finished_out);

// Links an already-indexed SD-card book to its Audiobookshelf item without
// copying audio. The local and server copies must have the same number of
// parts and nearly the same total duration. The generated manifest uses the
// local files' measured boundaries, so multipart progress maps directly.
bool audiobookshelf_link_local(const audiobookshelf_book_t *book, const char *local_book,
								 char *file_out, size_t file_size, double *seconds_out,
								 bool *finished_out);

// True when the local book's manifest binds THIS server item: a valid
// version=1 manifest whose resolved server and item_id both match (§11.5).
// Matching item_id alone would let server B's copy of one book open server A's
// local files. A manifest without `server=` (pre-0.6.0 vintage) resolves to
// the persisted legacy_server; with no reliable binding, false -- an explicit
// re-link is required and local progress stays pending.
// This lets the server library open a previously linked copy directly instead
// of offering to link the same book again.
bool audiobookshelf_local_linked(const char *local_book, const char *item_id);

// UI decision for a unique local match that is NOT linked to this server item
// (§11.5): true when the local copy's manifest -- a valid version=1 binding --
// resolves to a server OTHER than the active browsing identity. Such a copy
// belongs to another server: it must never be re-linked (rewriting its
// manifest would move the other server's binding), and the UI offers a
// download from this server instead, which lands in this server's own
// abs-<hash> directory and touches nothing of the other copy. With no
// reliable binding (§11.5), or a binding to this very server with a different
// item id, false -- the existing explicit re-link question still applies.
bool audiobookshelf_local_owned_elsewhere(const char *local_book);

typedef enum {
	ABS_SYNC_CHECKING, ABS_SYNC_LOCAL, ABS_SYNC_SETUP, ABS_SYNC_LINKED,
	ABS_SYNC_PENDING, ABS_SYNC_OFFLINE, ABS_SYNC_SENDING, ABS_SYNC_SYNCED,
	ABS_SYNC_RETRY, ABS_SYNC_OTHER_SERVER, ABS_SYNC_SYNCED_OFFLINE
} audiobookshelf_sync_status_t;

// Cached snapshot only: no file/database/network I/O on the UI thread.
audiobookshelf_sync_status_t audiobookshelf_sync_status(const char *file);
// Notification only; never enables Wi-Fi or waits for an upload.
void audiobookshelf_network_changed(void);

// Wakes the worker after a local checkpoint. The persisted audiobook position
// remains pending until the server acknowledges that exact snapshot, even
// across offline listening, multiple books and player restarts.
void audiobookshelf_queue_progress(const char *book_path, const char *file, double seconds,
								   bool finished, bool immediate);

// ---------------------------------------------------------------------------
// Per-profile sync diagnostics (§12.4 line 1201): in-memory only; never
// persisted, never containing tokens or credentials — the bounded English
// observability surface for "why has this profile not synced".
//
// One entry appears with the first observed server interaction for a profile
// id (or the literal "legacy" while the §11.1 migration window is all there
// is): the last successful libraries response, the last successful progress
// pass, the class of the last failure ("auth", "network", "server", "data"),
// how many interactions failed since the last success, and the reconnect
// generation that was current at the last interaction — compared against the
// generation a caller gets from audiobookshelf_profile_reconnect_generation(),
// it says whether this profile has been retried since the network returned.
// Any profile replacement (add/edit/remove/factory reset through the profile
// store) clears every entry: the history belonged to the old credentials.
// ---------------------------------------------------------------------------

#define ABS_PROFILE_DIAG_LEGACY "legacy"
#define ABS_PROFILE_DIAG_CLASS_MAX 16

typedef struct {
	char profile_id[ABS_PROFILE_ID_MAX];
	long long last_libraries_ms;         // last successful /api/libraries; 0 = none
	long long last_progress_ms;          // last successful progress GET/PATCH; 0 = none
	char last_error_class[ABS_PROFILE_DIAG_CLASS_MAX]; // "" when the last try succeeded
	int consecutive_failures;            // failed server interactions since the last success
	unsigned last_reconnect_generation;  // reconnect counter at the last interaction
} abs_profile_diag_t;

// Copies the live diagnostics into `out` (bounded by `max`), entries oldest
// first, at most ABS_PROFILE_MAX + 1. Returns how many were filled.
int audiobookshelf_profile_diagnostics(abs_profile_diag_t *out, int max);

// The monotonically rising reconnect counter the diagnostics stamp. Every
// connectivity notification (wifi enable, a fresh association mid-session)
// bumps it exactly once.
unsigned audiobookshelf_profile_reconnect_generation(void);

// Hooks the clear-on-replacement behavior (§12.4) to the profile store. Boot
// calls this once from audiobookshelf_init(); the host tests call it directly.
// Idempotent.
void audiobookshelf_profile_diagnostics_install(void);

// Why the most recent call on this thread failed.
const char *audiobookshelf_last_error(void);

#endif /* AUDIOBOOKSHELF_H */
