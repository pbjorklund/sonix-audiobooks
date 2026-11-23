#ifndef AUDIOBOOKSHELF_H
#define AUDIOBOOKSHELF_H

#include <stdbool.h>
#include <stddef.h>

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
} audiobookshelf_track_t;

typedef struct {
	audiobookshelf_item_t item;
	char description[8192];
	char series_part[32];
	long long progress_updated_ms;
	audiobookshelf_track_t tracks[ABS_TRACK_MAX];
	int track_count;
} audiobookshelf_book_t;

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
// audiobookshelf_search_ex(); kept only until the GUI migrates (§10.3).
// Library-scoped (§10.1), whatever the old "server-wide" wording said; at most
// max matches (no server search pagination).
int audiobookshelf_search(const char *library_id, const char *query, audiobookshelf_item_t *out, int max);
// Caller owns the allocated, alphabetically sorted filter list.
int audiobookshelf_groups(const char *library_id, bool series, audiobookshelf_library_t **out);
// filterdata for one group (§10.1). Authors/series carry {id,name}; for
// genres and tags the returned `name` holds the string itself (reviewed
// servers send bare strings, the checked-in fixtures {id,name} objects -- the
// name is the filter value either way) and `id` stays empty: the value a
// genre/tag filter encodes is the name, never an object id. The returned list
// is alphabetically sorted and caller-owned (free()).
int audiobookshelf_groups_ex(const char *library_id, abs_browse_filter_t group,
							 audiobookshelf_library_t **out);
int audiobookshelf_filtered_items(const char *library_id, int page, const char *filter,
	 audiobookshelf_item_t *out, int max, int *total_out);
// Raw (unencoded) `authors.|series.<Base64(id)>` filter value for the legacy
// M1 GUI flow, which URL-encodes the whole string later. The general building
// block for one already-encoded filter is abs_browse_query_build_filter().
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

// Why the most recent call on this thread failed.
const char *audiobookshelf_last_error(void);

#endif /* AUDIOBOOKSHELF_H */
