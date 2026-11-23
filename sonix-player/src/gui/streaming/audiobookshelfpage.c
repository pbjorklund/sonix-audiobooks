#include "audiobookshelfpage.h"
#include "audiobookshelfprofiles.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "abslist.h"
#include "audiobookshelfqueue.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/library/audiobooks.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/confirm.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/shell/popover.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/core/utils.h"
#include "src/system/device/system.h"
#include "src/system/library/audiobookdb.h"
#include "src/system/net/wifi.h"
#include "src/system/playback/audiobook.h"
#include "src/system/streaming/abs_download.h"
#include "src/system/streaming/abs_profiles.h"
#include "src/system/streaming/audiobookshelf.h"

#include <ctype.h>

#define ABS_LIBRARY_MAX 16
#define LOCAL_MATCH_WINDOW 8

lv_obj_t *audiobookshelf_screen;

static gui_config_t *config;
static lv_obj_t *content;
static lv_obj_t *page_title;
static lv_obj_t *search_screen, *search_field;
static keyboard_t *search_keyboard;
static char search_query[ABS_NAME_MAX];
static bool returning_from_search;

// The view chain: libraries -> books (browse or search) -> filter kinds ->
// one group's values. Back walks the same chain in reverse: a filtered book
// list returns to its group list, a group list to the filter kinds, the
// filter kinds to the books they came from, and books to the libraries.
typedef enum { VIEW_LIBRARIES = 0, VIEW_ITEMS, VIEW_FILTERS, VIEW_GROUPS } view_t;
static view_t view;
static char library_id[ABS_ID_MAX];
static char library_name[ABS_NAME_MAX];
static char selected_id[ABS_ID_MAX];
static char selected_title[ABS_NAME_MAX];
static char selected_local_book[1280];
// The §8.1 line-693 identity seed: what the download dedupes/queues with and
// what the row's first remote-progress hint is. Values only — a full book
// model is never retained for a queued download.
static double selected_progress;
static bool selected_finished;
static double selected_duration;
static char filter_title[ABS_NAME_MAX]; // display name of the active filter value

// ---------------------------------------------------------------------------
// §10.2 query state and result ownership.
//
// The browse query is an immutable value: every UI change (sort, direction,
// filter, library, search, page) constructs a fresh abs_browse_query_t with
// the page reset that change requires, and the job carries its own copy --
// nothing shares a mutable query string with the worker. browse_query is what
// was last requested on the GUI thread; the displayed page is that request's
// generation.
//
// The results are one generation-owned heap page. submit() releases the
// visible page before the worker allocates its replacement, so a browse page
// and a search response are never both alive (§10.2); the worker publishes
// its buffer only when its generation still matches and frees its own buffer
// otherwise. GUI reads of the published page happen on the GUI thread after
// the job_done_async publish ordering, and later taps are gated by `busy` --
// the same §11.4 discipline the static arrays before it used, with the heap
// page living only for the page it was loaded for.
// ---------------------------------------------------------------------------
static abs_browse_query_t browse_query;
static audiobookshelf_item_t *page_items;
static int page_count, page_total;
static bool search_overflow; // a usable 101st match exists (§10.1)

static audiobookshelf_library_t libraries[ABS_LIBRARY_MAX];
static int library_count;
static audiobookshelf_library_t *groups; // one §10.2 projection, swapped under job_lock
static int group_count, group_page;
static abs_browse_filter_t group_kind;
static bool group_truncated;
static int group_oversized;

static abs_list_t *books_list; // the pooled rows currently on screen (null once cleaned)

static void draw_libraries(void);
static void draw_books(void);
static void draw_filters(void);
static void draw_groups(void);
static void book_row_bind(void *user, int index, char *title, size_t title_size,
						  char *subtitle, size_t subtitle_size);
static void group_row_bind(void *user, int index, char *title, size_t title_size,
						   char *subtitle, size_t subtitle_size);
static void item_click_cb(void *user, int index, lv_obj_t *row);
static void group_click_cb(void *user, int index, lv_obj_t *row);
static void group_page_cb(lv_event_t *e);
static void open_search_cb(lv_event_t *e);
static void search_accept_cb(lv_event_t *e);
static void sort_menu_cb(lv_event_t *e);
static void filter_menu_cb(lv_event_t *e);
static void all_books_cb(lv_event_t *e);
static void open_filter_kind_cb(lv_event_t *e);
static void request_group_kind(abs_browse_filter_t kind);
static void library_clicked_cb(lv_event_t *e);

typedef enum { JOB_NONE = 0, JOB_TEST, JOB_LIBRARIES, JOB_BROWSE, JOB_SEARCH, JOB_GROUPS, JOB_LINK } job_kind_t;
typedef struct {
	job_kind_t kind;
	uint32_t generation; // browse generation at submit time (§11.4)
	abs_browse_query_t query; // JOB_BROWSE: the complete, immutable query (§10.2)
	char id[ABS_ID_MAX];
	char text[ABS_NAME_MAX];   // JOB_SEARCH text
	abs_browse_filter_t group; // JOB_GROUPS kind
	char path[1280];
} job_t;
static void run_loading(const job_t *job);

// Job queue and browse generation, all under job_lock (§11.4). Every submitted
// job AND every profile switch advances the generation: a completion that lands
// after something newer was requested is stale and must not repaint this page
// with another profile's data (the spec's own discard mechanism). The queue
// holds one slot -- the newest browse intent replaces an older queued one, so a
// switch during an in-flight request queues its reload instead of dropping it.
static pthread_mutex_t job_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t job_cond = PTHREAD_COND_INITIALIZER;
static job_t pending;
static bool have_pending;
static bool worker_started;
static bool busy; // a request is in flight or queued (GUI-thread view)
static uint32_t browse_generation = 0;

// The last completed job. The worker publishes it under job_lock and only
// job_done_async on the GUI thread reads it -- that pair, not the fields
// themselves, is the synchronization (§11.4: no unsynchronized shared writes).
typedef struct {
	job_kind_t kind;
	bool ok;
	uint32_t generation;
	char error[256];
} job_result_t;
static job_result_t result;

// ---------------------------------------------------------------------------
// §8.5 line 587: the follow-up scan on per-job completion. A low-rate watcher
// on the queue's busy bit re-counts the "Downloads" badge at every busy/idle
// crossing and at any enqueue this page makes, and — when a DONE row still
// has its resume import pending — starts the Audiobooks scan whose completion
// notification drives the import coordinator (§8.3 line 556). The watcher
// owns no queue state: the heap snapshot it takes at a crossing is dropped
// immediately, so the page's steady cost is one bool and an int.
// ---------------------------------------------------------------------------
static lv_timer_t *queue_watch_timer;
static bool queue_watch_busy;
static int downloads_badge;
static bool downloads_badge_dirty;

static bool wifi_connected(void) {
#ifdef HOST_BUILD
	return true;
#else
	// Association snapshots and address ioctls both flap on some R1 builds
	// while the link is already carrying traffic. Only block a request when the
	// user has actually switched Wi-Fi off; the HTTP result is authoritative.
	return wifi_get_enabled();
#endif
}

static void show_note(const char *message) {
	lv_obj_t *note = lv_label_create(content);
	lv_label_set_text(note, tr(message));
	lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(note, lv_pct(100));
	lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(note, &font_ui_22, 0);
	lv_obj_set_style_pad_ver(note, 22, 0);
}

// The same, for text that is data (a search string, a formatted count) rather
// than a translation key.
static void show_note_text(const char *text) {
	lv_obj_t *note = lv_label_create(content);
	lv_label_set_text(note, text);
	lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(note, lv_pct(100));
	lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(note, &font_ui_22, 0);
	lv_obj_set_style_pad_ver(note, 22, 0);
}

// The shared half of every "the browsing session changed" path (§11.4): a
// profile switch, a newly added active profile, or the last one removed. The
// generation bump happens before the clear so an in-flight result from the old
// profile is stale by the time it lands: job_done_async discards it instead of
// repainting this page with another server's data (and a tap on a stale row
// could never send that item id to the active server). Queue jobs and manifests
// are untouched -- they resolve their own credentials.
static void clear_browse_state(void) {
	pthread_mutex_lock(&job_lock);
	browse_generation++; // invalidate every in-flight or queued old-profile result
	view = VIEW_LIBRARIES;
	library_count = 0;
	// §10.2 result ownership: the visible page is released with the session.
	free(page_items);
	page_items = NULL;
	page_count = page_total = 0;
	search_overflow = false;
	free(groups);
	groups = NULL;
	group_count = group_page = 0;
	group_kind = ABS_FILTER_NONE;
	group_truncated = false;
	group_oversized = 0;
	memset(&browse_query, 0, sizeof(browse_query));
	library_id[0] = library_name[0] = search_query[0] = '\0';
	filter_title[0] = '\0';
	selected_id[0] = selected_title[0] = '\0';
	selected_local_book[0] = '\0';
	pthread_mutex_unlock(&job_lock);
}

// A profile was selected in the picker and the store has already persisted it:
// clear remote state and load the newly active profile's libraries. The reload
// queues behind any in-flight job rather than being dropped while busy.
static void profile_selected_cb(void *user) {
	(void)user;
	clear_browse_state();
	draw_libraries();
	if (!audiobookshelf_configured() || !wifi_connected()) return;
	job_t job = {.kind = JOB_LIBRARIES};
	run_loading(&job);
}

// The editor saved an edit to the ACTIVE profile -- possibly with a new token:
// check it against GET /api/libraries while the editor stays up (§11.4). On
// success job_done_async steps back from the editor and toasts; on failure the
// error popup lands over the fields so the value can be corrected in place, as
// the setup screen used to allow.
static void validate_profile_cb(void *user) {
	(void)user;
	job_t job = {.kind = JOB_TEST};
	run_loading(&job);
}

// The active profile was removed (other profiles may remain): clear remote
// state and show the unconfigured view, as a fresh store would. Selecting one
// of the remaining profiles is a deliberate tap from the picker.
static void active_lost_cb(void *user) {
	(void)user;
	clear_browse_state();
	draw_libraries();
}

static void open_profiles_cb(lv_event_t *e) {
	(void)e;
	// No busy guard, as with setup: switching profile during an in-flight
	// download is safe by design (§11.4 -- a switch does not touch queue jobs,
	// and every request resolves its own credentials).
	switch_screen(audiobookshelfprofiles_screen());
}

// Active profile display for the ABS page's top row: name, falling back to
// the normalized server for a nameless profile. Tokens are dropped as the
// list copies come out of the store.
static void top_row_label(char *out, size_t size) {
	char active_id[ABS_PROFILE_ID_MAX];
	if (!abs_profiles_active(active_id, sizeof(active_id))) {
		// No active profile: the legacy (0.5.0 / migration-window) display.
		const char *server = audiobookshelf_server();
		snprintf(out, size, "%s", server[0] ? server : tr("audiobookshelf_not_configured"));
		return;
	}
	abs_profile_t profiles[ABS_PROFILE_MAX];
	int count = abs_profiles_list(profiles, ABS_PROFILE_MAX);
	for (int i = 0; i < count; i++) {
		if (strcmp(profiles[i].id, active_id) != 0) continue;
		memset(profiles[i].token, 0, sizeof(profiles[i].token));
		snprintf(out, size, "%s", profiles[i].name[0] ? profiles[i].name : profiles[i].server);
		return;
	}
	snprintf(out, size, "%s", audiobookshelf_server());
}

// Queues the next job, stamping it with a fresh generation. Never drops: when
// one is already queued the newest browse intent replaces it (a second switch
// supersedes the first), and an in-flight job simply runs to completion -- its
// stale result is discarded on return (§11.4).
//
// §10.2: a new QUERY releases the visible page here, before the worker
// allocates its replacement, so a browse page and a search response are never
// both alive. Download/link/validation jobs do not change the query, so the
// rows their dialog was confirmed for stay bound through the job -- the
// linked-copy resume path reads the page again after the link completes.
// Taps during any job stay gated by `busy`; LVGL labels already hold their
// own copies of the text that was on screen.
static void submit(const job_t *job) {
	job_t queued = *job;
	bool releases_page = job->kind == JOB_BROWSE || job->kind == JOB_SEARCH ||
						 job->kind == JOB_GROUPS;
	pthread_mutex_lock(&job_lock);
	queued.generation = ++browse_generation;
	if (releases_page) {
		free(page_items);
		page_items = NULL;
		page_count = page_total = 0;
		search_overflow = false;
	}
	pending = queued;
	have_pending = true;
	busy = true;
	pthread_cond_signal(&job_cond);
	pthread_mutex_unlock(&job_lock);
}

static void run_loading(const job_t *job) {
	pthread_mutex_lock(&job_lock);
	bool first = !busy; // the busy toast stays up while anything is queued
	pthread_mutex_unlock(&job_lock);
	if (first) toast_busy("loading");
	submit(job);
}

// One watcher observation: re-count the badge, and when a DONE row still owes
// its resume import, start the Audiobooks scan whose completion notification
// runs the queue's per-row import coordinator (§8.3 line 556). A job that
// finished while a scan was already running imports at that scan's
// completion; a scan that started before the files existed re-covers it via
// the coordinator's retry at the next completed scan. The snapshot is a
// transient heap copy — taken, read, dropped.
static void queue_watch_observe(void) {
	downloads_badge_dirty = false;
	abs_download_snapshot_t *snap = malloc(sizeof(*snap));
	if (!snap) return;
	if (!abs_download_snapshot(snap)) {
		// Queue disarmed (card absent / quiesced): the badge has nothing to
		// count. A re-init makes the next observation see the rows again.
		free(snap);
		downloads_badge = 0;
		return;
	}
	int pending = 0;
	bool done_needs_import = false;
	for (int i = 0; i < snap->job_count; i++) {
		const abs_download_job_t *job = &snap->jobs[i];
		if (job->state != ABS_DL_DONE && job->state != ABS_DL_FAILED &&
			job->state != ABS_DL_CANCELLED)
			pending++;
		if (job->state == ABS_DL_DONE && job->index_pending) done_needs_import = true;
	}
	downloads_badge = pending;
	free(snap);

	if (!done_needs_import || audiobookdb_scan_running()) return;
	const char *root = config->sd_root_path;
	char scan_root[1024];
	if (root && root[0] &&
		(size_t)snprintf(scan_root, sizeof(scan_root), "%s/%s", root, AUDIOBOOKDB_FOLDER) <
			sizeof(scan_root)) {
		// §8.5 line 587: the per-job completion's follow-up scan. Failures
		// keep every row importable: the coordinator runs again at the next
		// completed scan, whichever path starts it.
		audiobookdb_scan_start(scan_root);
	}
}

static void queue_watch_cb(lv_timer_t *timer) {
	(void)timer;
	// One mutex-brief engine bit per tick; the snapshot observation only
	// happens at a busy/idle crossing or right after this page enqueued.
	bool busy_now = abs_download_busy();
	if (busy_now != queue_watch_busy || downloads_badge_dirty) {
		queue_watch_busy = busy_now;
		queue_watch_observe();
	}
}

// The "Downloads" row: the queue is card-local durable intent (§8.1), not a
// remote view — it opens whatever the browse job state is.
static void open_downloads_cb(lv_event_t *e) {
	(void)e;
	audiobookshelfqueue_open();
}

// The libraries view, the root of the chain: the profile row on top, one
// Downloads row, then one row per book library.
static void draw_libraries(void) {
	view = VIEW_LIBRARIES;
	books_list = NULL;
	lv_label_set_text(page_title, tr("audiobookshelf"));
	lv_obj_clean(content);

	// §11.4: the current profile is the page's top row and opens the picker --
	// which also holds the Add row, so a store with no profiles yet leads there
	// too (through "Add a server") instead of the retired setup screen. While
	// no profile is active (0.5.0 store, migration window) the value keeps its
	// old meaning: the legacy server display.
	char label[ABS_SERVER_MAX];
	bool have_active = abs_profiles_active(NULL, 0);
	lv_obj_t *server_value = NULL;
	settingsrow_add(content, have_active ? "audiobookshelf_profile" : "audiobookshelf_server",
					&server_value, open_profiles_cb, NULL);
	top_row_label(label, sizeof(label));
	if (server_value) {
		lv_label_set_text(server_value, label);
		lv_label_set_long_mode(server_value, LV_LABEL_LONG_DOT);
		lv_obj_set_width(server_value, lv_pct(52));
		lv_obj_set_height(server_value, lv_font_get_line_height(&font_ui_20));
		lv_obj_set_style_text_font(server_value, &font_ui_20, 0);
	}

	// §8.5 line 594: one "Downloads" entry — a row on the library view, not a
	// new top-level streaming tile. The value is the watcher's last count of
	// unfinished jobs. The queue itself is card-local durable intent: the row
	// opens it regardless of configuration or Wi-Fi.
	lv_obj_t *badge_value = NULL;
	settingsrow_add(content, "audiobookshelf_downloads", &badge_value, open_downloads_cb, NULL);
	if (badge_value) {
		lv_label_set_long_mode(badge_value, LV_LABEL_LONG_DOT);
		lv_obj_set_width(badge_value, lv_pct(16));
		lv_obj_set_height(badge_value, lv_font_get_line_height(&font_ui_20));
		lv_obj_set_style_text_font(badge_value, &font_ui_20, 0);
		if (downloads_badge > 0) {
			char badge[16];
			snprintf(badge, sizeof(badge), "%d", downloads_badge);
			lv_label_set_text(badge_value, badge);
		} else {
			lv_obj_set_hidden(badge_value, true);
		}
	}

	if (!audiobookshelf_configured()) {
		show_note("audiobookshelf_setup_note");
		return;
	}
	if (!wifi_connected()) {
		show_note("enable_wi_fi_first");
		return;
	}
	if (library_count == 0) {
		show_note("audiobookshelf_no_libraries");
		return;
	}
	for (int i = 0; i < library_count; i++) {
		lv_obj_t *row = settingsrow_add(content, libraries[i].name, NULL, library_clicked_cb, (void *)(intptr_t)i);
		settingsrow_name_lines(row, 2);
	}
}

// Fills a fresh browse query: title order in its §10.1 default direction,
// page zero, no filter. Every query-changing UI path starts from the shape
// this builds and applies its own change, which keeps the page-reset rules
// uniform (§10.2).
static bool browse_query_reset(void) {
	return abs_browse_query_init(&browse_query, library_id, ABS_SORT_TITLE,
								 ABS_FILTER_NONE, NULL, 0,
								 abs_browse_query_default_desc(ABS_SORT_TITLE));
}

// The browse job for the current query: the query is copied into the job by
// value, so the worker sees an immutable snapshot (§10.2).
static job_t browse_job(void) {
	job_t job = {.kind = JOB_BROWSE, .query = browse_query};
	return job;
}

static void library_clicked_cb(lv_event_t *e) {
	int index = (int)(intptr_t)lv_event_get_user_data(e);
	if (busy || index < 0 || index >= library_count) return;
	snprintf(library_id, sizeof(library_id), "%s", libraries[index].id);
	snprintf(library_name, sizeof(library_name), "%s", libraries[index].name);
	// A library change resets the whole query (§10.2), search mode included.
	if (!browse_query_reset()) return;
	filter_title[0] = '\0';
	job_t job = browse_job();
	run_loading(&job);
	// search_query follows the query's completion: the browse publish clears
	// it under job_lock, so the GUI never races the worker on that string.
}

static void page_nav_cb(lv_event_t *e) {
	int page = (int)(intptr_t)lv_event_get_user_data(e);
	if (page < 0 || busy) return;
	// Paging keeps the exact query otherwise; only the page moves. The
	// next/previous rows are built from the response's real total, never from
	// a hard-coded page count (§10.2).
	abs_browse_query_t query = browse_query;
	query.page = page;
	browse_query = query;
	job_t job = {.kind = JOB_BROWSE, .query = query};
	run_loading(&job);
}

static void normalized_name(const char *input, char *out, size_t size) {
	size_t written = 0;
	for (const unsigned char *p = (const unsigned char *)input; p && *p && written + 1 < size; p++) {
		if (*p >= 'A' && *p <= 'Z') out[written++] = (char)(*p - 'A' + 'a');
		else if ((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9')) out[written++] = (char)*p;
	}
	out[written] = '\0';
}

static void normalized_book_title(const char *input, char *out, size_t size) {
	char title[ABS_NAME_MAX];
	snprintf(title, sizeof(title), "%s", input ? input : "");
	size_t len = strlen(title);
	while (len && isspace((unsigned char)title[len - 1])) title[--len] = '\0';

	const char *extensions[] = {".mp3", ".m4b", ".m4a", ".flac", ".ogg", ".opus", ".aac", ".wav"};
	for (size_t i = 0; i < sizeof(extensions) / sizeof(extensions[0]); i++) {
		size_t ext_len = strlen(extensions[i]);
		if (len > ext_len && strcasecmp(title + len - ext_len, extensions[i]) == 0) {
			title[len -= ext_len] = '\0';
			break;
		}
	}

	// Plex-style audiobook names commonly append a series marker and year to
	// the album title. They are useful in filenames but are not part of the
	// Audiobookshelf title used for matching.
	for (;;) {
		while (len && isspace((unsigned char)title[len - 1])) title[--len] = '\0';
		if (len >= 2 && title[len - 1] == ']') {
			char *open = strrchr(title, '[');
			if (open) {
				len = (size_t)(open - title);
				title[len] = '\0';
				continue;
			}
		}
		if (len >= 6 && title[len - 1] == ')') {
			char *open = strrchr(title, '(');
			if (open && strlen(open) == 6 && isdigit((unsigned char)open[1]) &&
				isdigit((unsigned char)open[2]) && isdigit((unsigned char)open[3]) &&
				isdigit((unsigned char)open[4])) {
				len = (size_t)(open - title);
				title[len] = '\0';
				continue;
			}
		}
		break;
	}
	normalized_name(title, out, size);
}

typedef struct {
	const audiobookshelf_item_t *remote;
	char path[1280];
	int matches;
} local_match_t;

typedef struct {
	char paths[LOCAL_MATCH_WINDOW][1280];
	int count;
} local_window_t;

static bool count_local_part(const char *path, const char *title, void *user);

static bool collect_local_row(const char *name, const char *path, void *user) {
	(void)name;
	local_window_t *window = user;
	if (window->count >= LOCAL_MATCH_WINDOW) return false;
	snprintf(window->paths[window->count++], sizeof(window->paths[0]), "%s", path);
	return true;
}

static void match_local_path(local_match_t *match, const char *path) {
	char title[ABS_NAME_MAX], author[ABS_NAME_MAX];
	char local_title[ABS_NAME_MAX], remote_title[ABS_NAME_MAX];
	char local_author[ABS_NAME_MAX], remote_author[ABS_NAME_MAX];
	if (!audiobookdb_book_info(path, title, sizeof(title), author, sizeof(author), NULL))
		return;
	normalized_book_title(title, local_title, sizeof(local_title));
	normalized_book_title(match->remote->title, remote_title, sizeof(remote_title));
	normalized_name(author, local_author, sizeof(local_author));
	normalized_name(match->remote->author, remote_author, sizeof(remote_author));
	bool title_matches = strcasecmp(title, match->remote->title) == 0 ||
		(strlen(local_title) >= 8 && strcmp(local_title, remote_title) == 0);
	if (!title_matches) return;
	if (author[0] && match->remote->author[0]) {
		bool author_matches = strcasecmp(author, match->remote->author) == 0 ||
			(strlen(local_author) >= 4 && (strcmp(local_author, remote_author) == 0 ||
			 strstr(remote_author, local_author) != NULL || strstr(local_author, remote_author) != NULL));
		if (!author_matches) return;
	}
	int part_count = 0;
	int parts = audiobookdb_is_folder_book(path)
		? audiobookdb_parts_for_each(path, count_local_part, &part_count)
		: 1;
	if (match->remote->tracks > 0 && parts != match->remote->tracks) return;
	match->matches++;
	if (match->matches == 1) snprintf(match->path, sizeof(match->path), "%s", path);
	else match->path[0] = '\0';
}

static bool count_local_part(const char *path, const char *title, void *user) {
	(void)path;
	(void)title;
	int *count = user;
	(*count)++;
	return true;
}

static bool unique_local_match(const audiobookshelf_item_t *remote, char *path, size_t size) {
	local_match_t match = {.remote = remote};
	audiobookdb_index_t *index = audiobookdb_index_open(AUDIOBOOK_LIST_ALL, "", AUDIOBOOK_ORDER_NAME, false);
	if (!index) return false;
	int count = audiobookdb_index_count(index);
	for (int offset = 0; offset < count; offset += LOCAL_MATCH_WINDOW) {
		local_window_t window = {0};
		audiobookdb_index_window(index, offset,
								 count - offset > LOCAL_MATCH_WINDOW ? LOCAL_MATCH_WINDOW : count - offset,
								 collect_local_row, &window);
		for (int i = 0; i < window.count; i++) match_local_path(&match, window.paths[i]);
	}
	audiobookdb_index_close(index);
	if (match.matches != 1 || !match.path[0]) return false;
	snprintf(path, size, "%s", match.path);
	return true;
}

// §8.5 line 587: download = queue. The confirm's OK enqueues durable intent
// (§8.1) and opens the queue view — a single one-item queue is exactly the old
// single-download behavior (same formats: files, sidecars, manifest), just
// durable, resumable and cancellable. Enqueue deduplicates unfinished jobs by
// identity, so a repeated tap on a queued book reopens its row instead of
// double-queueing it. No modal: back stays free at every moment (line 468).
static void begin_download(void *user) {
	(void)user;
	if (!selected_id[0] || busy) return;

	char active_id[ABS_PROFILE_ID_MAX];
	abs_profile_t profiles[ABS_PROFILE_MAX];
	if (!abs_profiles_active(active_id, sizeof(active_id)) || abs_profiles_list(profiles, ABS_PROFILE_MAX) < 1) {
		gui_notify_popup(tr("audiobookshelf_not_configured"));
		return;
	}
	const abs_profile_t *profile = NULL;
	for (int i = 0; i < ABS_PROFILE_MAX; i++) {
		if (strcmp(profiles[i].id, active_id) == 0) {
			profile = &profiles[i];
			break;
		}
	}
	if (!profile || !profile->server[0]) {
		gui_notify_popup(tr("audiobookshelf_not_configured"));
		return;
	}

	// Item identity only (§9.2 line 693): the server refetches the expanded
	// book under its own auth snapshot when the worker runs the job. The
	// browse view's progress seeds the row's remote progress hint; the fetch
	// reconciles against the live server values anyway.
	double seeded_progress = -1;
	if (selected_finished && selected_duration > 0) seeded_progress = selected_duration;
	else if (selected_progress > 0) seeded_progress = selected_progress;
	abs_download_enqueue_t req = {
		.kind = ABS_DL_KIND_BOOK,
		.profile_id = profile->id,
		.server = profile->server, // the store's normalized URL, the sync identity
		.library_id = library_id,
		.item_id = selected_id,
		.episode_id = "",          // books have no episode id (§8.1)
		.title = selected_title,
		.destination = "",         // books: the identity fetch computes the §11.5 hash directory
		.remote_progress = seeded_progress,
		.remote_finished = selected_finished ? 1 : 0,
		.remote_updated_ms = 0,
	};
	long long job_id = 0;
	bool deduped = false;
	char error[ABS_DL_ERROR_MAX];
	if (!abs_download_enqueue_book(&req, &job_id, &deduped, error, sizeof(error))) {
		// Capacity, disarmed card, unusable database: all refusals happen
		// before any network activity (§8.1), with the engine's English
		// reason. The detail row the confirm came out of stays put.
		gui_notify_popup(error[0] ? error : tr("audiobookshelf_request_failed"));
		return;
	}
	// One-item queue or dedup: either way the queue view shows the book's
	// row — the old modal's start-of-download moment, now persistent.
	downloads_badge_dirty = true;
	audiobookshelfqueue_open();
}

static void begin_link(void *user) {
	(void)user;
	if (!selected_id[0] || !selected_local_book[0] || busy) return;
	job_t job = {.kind = JOB_LINK};
	snprintf(job.id, sizeof(job.id), "%s", selected_id);
	snprintf(job.path, sizeof(job.path), "%s", selected_local_book);
	run_loading(&job);
}

// A row of the book list was tapped. The decision tree covers every state a
// unique local match can be in (§11.5): already linked to this server item ->
// resume the local copy at its checkpoint; bound to ANOTHER server -> the
// explicit "download anyway from this server" choice, which never rewrites
// the other server's manifest (its copy stays its own; the new download lands
// in this server's own abs-<hash> directory); unbound, or bound to this
// server with a different item id -> the explicit re-link question, as before.
static void item_click_cb(void *user, int index, lv_obj_t *row) {
	(void)user;
	(void)row;
	if (busy || !page_items || index < 0 || index >= page_count) return;
	snprintf(selected_id, sizeof(selected_id), "%s", page_items[index].id);
	snprintf(selected_title, sizeof(selected_title), "%s", page_items[index].title);
	selected_local_book[0] = '\0';
	// The identity seed for the confirm's download action (§8.1 line 693):
	// values copied out of the owned page, never an index into it.
	selected_progress = page_items[index].progress;
	selected_finished = page_items[index].finished;
	selected_duration = page_items[index].duration;
	char message[ABS_NAME_MAX + 128];
	if (unique_local_match(&page_items[index], selected_local_book, sizeof(selected_local_book))) {
		if (audiobookshelf_local_linked(selected_local_book, selected_id)) {
			// Resume the linked copy at its saved checkpoint, wherever that is:
			// audiobooks_play_at() writes the point through as where the book
			// stands (upstream's play_book() does the same through
			// audiobook_resume_point()).
			char resume_file[512];
			double resume_seconds = 0;
			if (audiobook_resume_point(selected_local_book, resume_file, sizeof(resume_file), &resume_seconds))
				audiobooks_play_at(selected_local_book, resume_file, resume_seconds);
			return;
		}
		if (audiobookshelf_local_owned_elsewhere(selected_local_book)) {
			snprintf(message, sizeof(message), tr("audiobookshelf_download_anyway_question"), selected_title);
			confirm_show("audiobookshelf_download", message, "download", begin_download, NULL);
			return;
		}
		snprintf(message, sizeof(message), tr("audiobookshelf_link_question"), selected_title);
		confirm_show("audiobookshelf_link", message, "link", begin_link, NULL);
		return;
	}
	snprintf(message, sizeof(message), tr("audiobookshelf_download_question"), selected_title);
	confirm_show("audiobookshelf_download", message, "download", begin_download, NULL);
}

// ---------------------------------------------------------------------------
// Sort / Filter controls (§10.2): the toolbar becomes Search / Sort / Filter.
// Authors and Series live inside Filter next to Genres and Tags, preserving
// their group-list navigation, so the 480-pixel screen keeps three buttons.
// ---------------------------------------------------------------------------

static void sort_picked(void *user) {
	abs_browse_sort_t sort = (abs_browse_sort_t)(intptr_t)user;
	if (busy || !library_id[0]) return;
	// A sort change resets the page (§10.2) and takes that sort's §10.1
	// default direction (only "recently added" opens newest-first).
	abs_browse_query_t query;
	if (!abs_browse_query_init(&query, library_id, sort, browse_query.filter,
							   browse_query.filter_value, 0,
							   abs_browse_query_default_desc(sort)))
		return; // out-of-range or series-position without its filter: refuse
	browse_query = query;
	job_t job = {.kind = JOB_BROWSE, .query = query};
	run_loading(&job);
}

static void sort_flip_cb(void *user) {
	(void)user;
	if (busy || !library_id[0]) return;
	// A direction change resets the page (§10.2) and changes nothing else.
	abs_browse_query_t query = browse_query;
	query.page = 0;
	query.descending = !query.descending;
	browse_query = query;
	job_t job = {.kind = JOB_BROWSE, .query = query};
	run_loading(&job);
}

// The sort popover: one entry per available sort field (series order exists
// only under its series filter, §10.1) plus the direction toggle. The checked
// row is the current field. In search mode the control is disabled with a
// clear note: server search has no sort contract, and locally sorting a
// sample must never claim to reorder the server's matches (§10.1).
static void sort_menu_cb(lv_event_t *e) {
	if (busy) return;
	if (search_query[0]) {
		toast_plain("audiobookshelf_search_sort_note");
		return;
	}
	if (!library_id[0]) return;
	popover_item_t items[8];
	int n = 0;
	items[n++] = (popover_item_t){tr("audiobookshelf_sort_title"), sort_picked,
								  (void *)(intptr_t)ABS_SORT_TITLE, browse_query.sort == ABS_SORT_TITLE};
	items[n++] = (popover_item_t){tr("audiobookshelf_sort_author"), sort_picked,
								  (void *)(intptr_t)ABS_SORT_AUTHOR, browse_query.sort == ABS_SORT_AUTHOR};
	items[n++] = (popover_item_t){tr("audiobookshelf_sort_added"), sort_picked,
								  (void *)(intptr_t)ABS_SORT_ADDED_AT, browse_query.sort == ABS_SORT_ADDED_AT};
	items[n++] = (popover_item_t){tr("audiobookshelf_sort_duration"), sort_picked,
								  (void *)(intptr_t)ABS_SORT_DURATION, browse_query.sort == ABS_SORT_DURATION};
	if (browse_query.filter == ABS_FILTER_SERIES)
		items[n++] = (popover_item_t){tr("audiobookshelf_sort_series_order"), sort_picked,
									  (void *)(intptr_t)ABS_SORT_SERIES_POSITION,
									  browse_query.sort == ABS_SORT_SERIES_POSITION};
	items[n++] = (popover_item_t){tr(browse_query.descending ? "audiobookshelf_sort_descending"
															 : "audiobookshelf_sort_ascending"),
								  sort_flip_cb, NULL, false};
	popover_show(lv_event_get_current_target(e), items, n);
}

// The Filter button: the four group kinds live on a settingsrow page view,
// keeping the flow to the group lists consistent. Same search-mode rule as
// sort: a filter on a search sample would lie about its scope.
static void filter_menu_cb(lv_event_t *e) {
	(void)e;
	if (busy) return;
	if (search_query[0]) {
		toast_plain("audiobookshelf_search_sort_note");
		return;
	}
	draw_filters();
}

// Applies one group choice as the active filter (§10.1: one filter at a
// time): author/series take the object id, genre/tag the UTF-8 string itself.
// A series selection opens with the series-position order (§10.1: its only
// meaningful default); any other group leaves a series-position sort behind.
// The page resets to zero, as every filter change does (§10.2).
static void apply_filter(abs_browse_filter_t kind, const char *value, const char *title) {
	if (busy || !library_id[0] || !value || !value[0]) return;
	abs_browse_sort_t sort = browse_query.sort;
	bool descending = browse_query.descending;
	if (kind == ABS_FILTER_SERIES) {
		sort = ABS_SORT_SERIES_POSITION;
		descending = abs_browse_query_default_desc(ABS_SORT_SERIES_POSITION);
	} else if (sort == ABS_SORT_SERIES_POSITION) {
		sort = ABS_SORT_TITLE;
		descending = abs_browse_query_default_desc(ABS_SORT_TITLE);
	}
	abs_browse_query_t query;
	if (!abs_browse_query_init(&query, library_id, sort, kind, value, 0, descending)) return;
	browse_query = query;
	snprintf(filter_title, sizeof(filter_title), "%s", title);
	job_t job = {.kind = JOB_BROWSE, .query = query};
	run_loading(&job);
}

// The groups list (authors, series, genres, tags) for one kind: the whole
// bounded projection is fetched once and shown in ABS_GROUP_UI_PAGE_LIMIT
// client pages (§10.2), so page flips never touch the network.
static void request_group_kind(abs_browse_filter_t kind) {
	job_t job = {.kind = JOB_GROUPS, .group = kind};
	snprintf(job.id, sizeof(job.id), "%s", library_id);
	run_loading(&job);
}

static void open_filter_kind_cb(lv_event_t *e) {
	abs_browse_filter_t kind = (abs_browse_filter_t)(intptr_t)lv_event_get_user_data(e);
	if (busy || !library_id[0]) return;
	request_group_kind(kind);
}

// "All books" -- the exit from both search mode and an active filter. The
// browse query keeps its sort (a series-position sort falls back to title
// without its series filter), the filter drops, and the page resets (§10.2).
static void all_books_cb(lv_event_t *e) {
	(void)e;
	if (busy || !library_id[0]) return;
	filter_title[0] = '\0';
	abs_browse_sort_t sort = browse_query.sort;
	if (sort == ABS_SORT_SERIES_POSITION) sort = ABS_SORT_TITLE;
	abs_browse_query_t query;
	if (!abs_browse_query_init(&query, library_id, sort, ABS_FILTER_NONE, NULL, 0,
							   browse_query.descending))
		return;
	browse_query = query;
	job_t job = {.kind = JOB_BROWSE, .query = query};
	run_loading(&job);
	// search_query follows the query's completion: the browse publish clears
	// it under job_lock, so the GUI never races the worker on that string.
}

// The values of one group (§10.2). Author/series filter by object id,
// genre/tag by the string itself; the display name is what the books view
// notes. The page resets (apply_filter), as every filter change does.
static void group_click_cb(void *user, int index, lv_obj_t *row) {
	(void)user;
	(void)row;
	if (busy || !groups || index < 0) return;
	int absolute = group_page * ABS_GROUP_UI_PAGE_LIMIT + index;
	if (absolute >= group_count) return;
	// §10.2: a value that does not fit the 256-byte query contract is never
	// silently truncated into a working-looking but wrong filter -- it stays
	// visible, refuses the tap, and the count above the list explains why.
	if (groups[absolute].oversized) {
		toast_plain("audiobookshelf_filter_oversized");
		return;
	}
	apply_filter(group_kind,
				 groups[absolute].id[0] ? groups[absolute].id : groups[absolute].name,
				 groups[absolute].name);
}

// Group paging moves within the one projection; no request is made (§10.2).
static void group_page_cb(lv_event_t *e) {
	if (busy) return;
	group_page = (int)(intptr_t)lv_event_get_user_data(e);
	if (group_page < 0) group_page = 0;
	draw_groups();
}

// Row refill for the group list: single-line name rows.
static void group_row_bind(void *user, int index, char *title, size_t title_size,
						   char *subtitle, size_t subtitle_size) {
	(void)user;
	if (subtitle_size) subtitle[0] = '\0';
	if (!groups || index < 0) {
		if (title_size) title[0] = '\0';
		return;
	}
	int absolute = group_page * ABS_GROUP_UI_PAGE_LIMIT + index;
	if (absolute >= group_count) {
		if (title_size) title[0] = '\0';
		return;
	}
	snprintf(title, title_size, "%s", groups[absolute].name);
}

// The group values view: the bounded projection in client pages, with the
// §10.2 truncation and oversized-value warnings when they apply.
static void draw_groups(void) {
	view = VIEW_GROUPS;
	static const char *const titles[] = {
		[ABS_FILTER_AUTHOR] = "audiobookshelf_filter_authors",
		[ABS_FILTER_SERIES] = "audiobookshelf_filter_series",
		[ABS_FILTER_GENRE] = "audiobookshelf_filter_genres",
		[ABS_FILTER_TAG] = "audiobookshelf_filter_tags",
	};
	lv_label_set_text(page_title, tr(group_kind >= ABS_FILTER_AUTHOR && group_kind <= ABS_FILTER_TAG
										? titles[group_kind] : "audiobookshelf"));
	books_list = NULL;
	lv_obj_clean(content);

	int first = group_page * ABS_GROUP_UI_PAGE_LIMIT;
	int window = groups && first < group_count ? group_count - first : 0;
	if (window > ABS_GROUP_UI_PAGE_LIMIT) window = ABS_GROUP_UI_PAGE_LIMIT;

	if (group_truncated)
		show_note("audiobookshelf_filter_truncated");
	if (group_oversized > 0) {
		char note[160];
		snprintf(note, sizeof(note), tr("audiobookshelf_filter_oversized_count"), group_oversized);
		show_note_text(note);
	}

	abs_list_config_t cfg = {
		.parent = content,
		.row_height = 100,
		.row_gap = 8,
		.bind = group_row_bind,
		.click = group_click_cb,
		.user = NULL,
	};
	books_list = abs_list_create(&cfg);
	if (!books_list) return;
	abs_list_set_count(books_list, window);
	if (window == 0 && !group_truncated && group_oversized == 0)
		abs_list_note(books_list, tr("audiobookshelf_group_none"));

	if (window > 0) {
		if (first > 0) settingsrow_action(content, "audiobookshelf_previous_page", group_page_cb,
										  (void *)(intptr_t)(group_page - 1));
		if (first + window < group_count)
			settingsrow_action(content, "audiobookshelf_next_page", group_page_cb,
							   (void *)(intptr_t)(group_page + 1));
	}
}

// The filter kinds view: Authors and Series preserved next to the genre and
// tag groups (§10.2), with a direct exit while a filter is active.
static void draw_filters(void) {
	view = VIEW_FILTERS;
	books_list = NULL;
	lv_obj_clean(content);
	if (browse_query.filter != ABS_FILTER_NONE)
		settingsrow_action(content, "audiobookshelf_all_books", all_books_cb, NULL);
	static const abs_browse_filter_t kinds[] = {
		ABS_FILTER_AUTHOR, ABS_FILTER_SERIES, ABS_FILTER_GENRE, ABS_FILTER_TAG,
	};
	static const char *const labels[] = {
		"audiobookshelf_filter_authors", "audiobookshelf_filter_series",
		"audiobookshelf_filter_genres", "audiobookshelf_filter_tags",
	};
	for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++)
		settingsrow_add(content, labels[i], NULL, open_filter_kind_cb, (void *)(intptr_t)kinds[i]);
}

// Row refill for the books list: title over the author (with a progress
// percentage when the server shows a part-way position). The pooled renderer
// only knows indexes.
static void book_row_bind(void *user, int index, char *title, size_t title_size,
						  char *subtitle, size_t subtitle_size) {
	(void)user;
	if (!page_items || index < 0 || index >= page_count) {
		if (title_size) title[0] = '\0';
		if (subtitle_size) subtitle[0] = '\0';
		return;
	}
	const audiobookshelf_item_t *item = &page_items[index];
	snprintf(title, title_size, "%s", item->title);
	if (item->progress > 0 && !item->finished && item->duration > 0)
		snprintf(subtitle, subtitle_size, "%s  %.0f%%", item->author,
				 item->progress * 100.0 / item->duration);
	else snprintf(subtitle, subtitle_size, "%s", item->author);
}

static void open_search_cb(lv_event_t *e) {
	(void)e;
	if (busy) return;
	lv_textarea_set_text(search_field, search_query);
	keyboard_reset(search_keyboard);
	returning_from_search = true;
	switch_screen(search_screen);
}

// Search results keep the server's own order (§10.1: ABS does not document a
// relevance contract, so the display never re-sorts them locally). The
// truthful copy distinguishes an honest 100-hit ceiling (no 101st probe) from
// a proven "more than 100 matches"; those notes are drawn in draw_books().
static void search_accept_cb(lv_event_t *e) {
	(void)e;
	if (busy) return;
	char text[ABS_NAME_MAX];
	snprintf(text, sizeof(text), "%s", lv_textarea_get_text(search_field));
	char *start = text;
	while (isspace((unsigned char)*start)) start++;
	memmove(text, start, strlen(start) + 1);
	size_t len = strlen(text);
	while (len && isspace((unsigned char)text[len - 1])) text[--len] = '\0';
	back_btn_cb(NULL); // leave the search screen whichever way the search went
	if (!len) {
		// §10.1: an empty search still returns to ordinary items, with the
		// page reset a search change carries (§10.2).
		all_books_cb(NULL);
		return;
	}
	job_t job = {.kind = JOB_SEARCH};
	snprintf(job.id, sizeof(job.id), "%s", library_id);
	snprintf(job.text, sizeof(job.text), "%s", text);
	run_loading(&job);
	// search_query flips only with real results (publish_page under job_lock),
	// so a failed search leaves the previous view standing; the browse query
	// keeps its sort and filter for the "All books" exit.
}

// The books view: ordinary browse pages carry the Search/Sort/Filter toolbar,
// server pagination and, with an active filter, its name and a direct exit.
// Search results name the search, carry no pagination and no sort/filter
// application -- the same toolbar refuses to act on a sample of the server's
// matches, instead pointing out that these controls belong to the library
// list (§10.1).
static void draw_books(void) {
	view = VIEW_ITEMS;
	lv_label_set_text(page_title, library_name[0] ? library_name : tr("audiobookshelf"));
	books_list = NULL;
	lv_obj_clean(content);

	lv_obj_t *toolbar = lv_obj_create(content);
	lv_obj_set_size(toolbar, lv_pct(100), 64);
	lv_obj_set_style_pad_all(toolbar, 0, 0);
	lv_obj_set_style_border_width(toolbar, 0, 0);
	lv_obj_set_style_bg_opa(toolbar, LV_OPA_TRANSP, 0);
	lv_obj_set_flex_flow(toolbar, LV_FLEX_FLOW_ROW);
	lv_obj_set_scrollable(toolbar, false);
	static const char *const toolbar_labels[] = {
		"search", "audiobookshelf_sort", "audiobookshelf_filter",
	};
	for (int i = 0; i < 3; i++) {
		lv_obj_t *button = lv_btn_create(toolbar);
		lv_obj_set_height(button, 60);
		lv_obj_set_flex_grow(button, 1);
		lv_obj_add_style(button, &theme_style_card, 0);
		lv_obj_add_style(button, &theme_style_card_pressed, LV_STATE_PRESSED);
		if (i == 0) lv_obj_add_event_cb(button, open_search_cb, LV_EVENT_CLICKED, NULL);
		else if (i == 1) lv_obj_add_event_cb(button, sort_menu_cb, LV_EVENT_CLICKED, NULL);
		else lv_obj_add_event_cb(button, filter_menu_cb, LV_EVENT_CLICKED, NULL);
		lv_obj_t *label = lv_label_create(button);
		lv_label_set_text(label, tr(toolbar_labels[i]));
		lv_obj_add_style(label, &theme_style_text, 0);
		lv_obj_set_style_text_font(label, &font_ui_18, 0);
		lv_obj_center(label);
	}

	bool searching = search_query[0] != '\0';
	if (searching) {
		show_note_text(search_query);
		if (search_overflow) show_note("audiobookshelf_search_overflow");
		else if (page_count >= ABS_SEARCH_DISPLAY_LIMIT) show_note("audiobookshelf_search_ceiling");
		settingsrow_action(content, "audiobookshelf_all_books", all_books_cb, NULL);
	} else if (browse_query.filter != ABS_FILTER_NONE && filter_title[0]) {
		show_note_text(filter_title);
		settingsrow_action(content, "audiobookshelf_all_books", all_books_cb, NULL);
	}

	// Pool rows bound to the published page: 40-item browse pages and 100-item
	// search results both render through the same twelve rows (§10.2).
	abs_list_config_t cfg = {
		.parent = content,
		.row_height = 112,
		.row_gap = 8,
		.bind = book_row_bind,
		.click = item_click_cb,
		.user = NULL,
	};
	books_list = abs_list_create(&cfg);
	if (!books_list) return;
	abs_list_set_count(books_list, page_count);
	if (page_count == 0) {
		// An empty page is a state of its own (§10.2), distinct from a failed
		// request -- a failed request pops, an empty one notes.
		abs_list_note(books_list, tr(searching ? "audiobookshelf_search_none"
											   : "audiobookshelf_no_books"));
	}

	if (!searching && page_count > 0) {
		if (browse_query.page > 0) settingsrow_action(content, "audiobookshelf_previous_page", page_nav_cb,
													  (void *)(intptr_t)(browse_query.page - 1));
		if ((browse_query.page + 1) * ABS_BROWSE_PAGE_LIMIT < (uint32_t)page_total)
			settingsrow_action(content, "audiobookshelf_next_page", page_nav_cb,
							   (void *)(intptr_t)(browse_query.page + 1));
	}
}

static bool back_guard(void) {
	// §8.1 line 468: back/navigation is always allowed — there is no download
	// back guard. The §8.1 busy-bit guards taps (an in-flight browse must not
	// build a second session on top of a half-drawn page), never navigation.
	if (view == VIEW_GROUPS) { draw_filters(); return true; }
	if (view == VIEW_FILTERS) { draw_books(); return true; }
	if (view == VIEW_ITEMS && search_query[0]) {
		// Search results are a transient view: back leaves them for the
		// libraries, as ever. The browse query keeps its sort and filter, so
		// re-entering the library restarts from the top of the chain.
		draw_libraries();
		return true;
	}
	if (view == VIEW_ITEMS && browse_query.filter != ABS_FILTER_NONE) {
		// Back from a filtered list reopens that filter's group list. When
		// another kind's projection is still held, refetch instead of showing
		// the wrong values under the filter's title.
		if (groups && group_kind == browse_query.filter) {
			group_page = 0;
			draw_groups();
		} else {
			request_group_kind(browse_query.filter);
		}
		return true;
	}
	if (view != VIEW_ITEMS) return false;
	draw_libraries();
	return true;
}

static void job_done_async(void *user) {
	(void)user;
	job_result_t done;
	bool more, stale;
	pthread_mutex_lock(&job_lock);
	done = result; // published by the worker under this lock (§11.4)
	more = have_pending; // a queued job follows: keep the busy state up for it
	busy = more;
	stale = done.generation != browse_generation;
	pthread_mutex_unlock(&job_lock);
	if (!more) toast_busy_end();

	// §11.4: a completion that landed after something newer was requested -- a
	// profile switch or a superseding request -- is stale. Its data belongs to
	// another profile's session and must not repaint this page (a tap on such a
	// row would send the old server's item id to the active one). The local
	// link is different: its confirm belonged to it, and the book it linked
	// belongs to its own server whatever is now active -- the completion toast
	// runs even when stale (its resume import happened in the worker).
	if (stale && done.kind != JOB_LINK) return;

	if (!done.ok) {
		gui_notify_popup(done.error[0] ? done.error : tr("audiobookshelf_request_failed"));
		return;
	}

	switch (done.kind) {
	case JOB_TEST:
		// A validation that succeeded while the editor was up steps back to
		// the picker; a failure already popped over it, where the value can be
		// corrected and re-saved.
		if (lv_screen_active() == audiobookshelfprofiles_editor_screen()) back_btn_cb(NULL);
		toast_success("audiobookshelf_connected");
		draw_libraries();
		break;
	case JOB_LIBRARIES:
		draw_libraries();
		break;
	case JOB_BROWSE:
	case JOB_SEARCH:
		draw_books();
		break;
	case JOB_GROUPS:
		draw_groups();
		break;
	case JOB_LINK:
		toast_success("audiobookshelf_linked");
		break;
	default:
		break;
	}
}

// Publishes a completed browse/search page under job_lock. §10.2 ownership:
// the GUI takes the heap page only when the job's generation still matches;
// a cancelled or superseded page is freed right here on the worker path. The
// previous published page is never touched from this side -- submit() already
// released it before this job began.
static void publish_page(audiobookshelf_item_t *buffer, int count, int total, bool overflow,
						 uint32_t generation, const char *search_text) {
	pthread_mutex_lock(&job_lock);
	if (generation != browse_generation) {
		pthread_mutex_unlock(&job_lock);
		free(buffer);
		return;
	}
	page_items = buffer;
	page_count = count;
	page_total = total;
	search_overflow = overflow;
	snprintf(search_query, sizeof(search_query), "%s", search_text ? search_text : "");
	pthread_mutex_unlock(&job_lock);
}

static void *worker_main(void *user) {
	(void)user;
	thread_be_background("audiobookshelf");
	for (;;) {
		pthread_mutex_lock(&job_lock);
		while (!have_pending) pthread_cond_wait(&job_cond, &job_lock);
		job_t job = pending;
		have_pending = false;
		pthread_mutex_unlock(&job_lock);

		// The network calls below run unlocked. Every GUI reader of the
		// published page either runs after this job's publish (ordered
		// through job_lock by job_done_async) or is gated on `busy`, which
		// stays set until then. What the lock still owns is what the
		// profile-switch clear touches -- counts, flags and the pointer swaps
		// -- so a switch can never interleave with them (§11.4: no
		// unsynchronized shared-field writes).
		job_result_t done = {.kind = job.kind, .ok = false, .generation = job.generation};
		char dl_resume_file[1280] = "";
		double dl_seconds = 0;
		bool dl_finished = false;
		long long dl_updated_ms = 0;
		char dl_server[512] = "";

		if (job.kind == JOB_TEST || job.kind == JOB_LIBRARIES) {
			int count = audiobookshelf_libraries(libraries, ABS_LIBRARY_MAX);
			if (count >= 0) {
				pthread_mutex_lock(&job_lock);
				library_count = count;
				pthread_mutex_unlock(&job_lock);
				done.ok = true;
			}
		} else if (job.kind == JOB_BROWSE) {
			// §10.2: a 40-item server page in one heap allocation owned by
			// this job. On a stale generation it never reaches the screen.
			audiobookshelf_item_t *buffer = calloc(ABS_BROWSE_PAGE_LIMIT, sizeof(*buffer));
			if (!buffer) snprintf(done.error, sizeof(done.error), "%s", tr("out_of_memory"));
			else {
				int total = 0;
				int count = audiobookshelf_browse(&job.query, buffer, ABS_BROWSE_PAGE_LIMIT, &total);
				if (count >= 0) {
					publish_page(buffer, count, total, false, job.generation, "");
					done.ok = true;
				} else free(buffer);
			}
		} else if (job.kind == JOB_SEARCH) {
			// §10.1/§10.2: the search contract is one request carrying the
			// 101-entry probe; at most the 100 kept matches are stored, and a
			// usable 101st becomes the honest "more than 100 matches" evidence.
			audiobookshelf_item_t *buffer = calloc(ABS_SEARCH_DISPLAY_LIMIT, sizeof(*buffer));
			if (!buffer) snprintf(done.error, sizeof(done.error), "%s", tr("out_of_memory"));
			else {
				bool more = false;
				int count = audiobookshelf_search_ex(job.id, job.text, buffer,
													 ABS_SEARCH_DISPLAY_LIMIT, &more);
				if (count >= 0) {
					publish_page(buffer, count, count, more, job.generation, job.text);
					done.ok = true;
				} else free(buffer);
			}
		} else if (job.kind == JOB_GROUPS) {
			audiobookshelf_library_t *next = NULL;
			bool truncated = false;
			int oversized = 0;
			int count = audiobookshelf_groups_ex(job.id, job.group, &next, &truncated, &oversized);
			if (count >= 0) {
				pthread_mutex_lock(&job_lock);
				if (job.generation != browse_generation) {
					pthread_mutex_unlock(&job_lock);
					free(next); // §10.2: the stale projection is never adopted
				} else {
					free(groups);
					groups = next;
					group_count = count;
					group_page = 0;
					group_kind = job.group;
					group_truncated = truncated;
					group_oversized = oversized;
					pthread_mutex_unlock(&job_lock);
					done.ok = true;
				}
			}
		} else if (job.kind == JOB_LINK) {
			audiobookshelf_book_t *book = calloc(1, sizeof(*book));
			if (!book) snprintf(done.error, sizeof(done.error), "%s", tr("out_of_memory"));
			else {
				snprintf(dl_server, sizeof(dl_server), "%s", audiobookshelf_server());
				done.ok = audiobookshelf_book(job.id, book) &&
					audiobookshelf_link_local(book, job.path, dl_resume_file,
									 sizeof(dl_resume_file), &dl_seconds, &dl_finished);
				if (done.ok) dl_updated_ms = book->progress_updated_ms;
				free(book);
				// §11.5: bind an existing local copy. When the local book has
				// its own position, that position is pushed to the server's
				// queue (next sync pass). When it has none, the link imports
				// the server's checkpoint onto the local copy — the old page
				// did this through its resume-import timer; the linked book is
				// already indexed (the tap matched it), so the import is
				// immediate here, in the worker, with no shared state.
				if (done.ok) {
					char local_file[1280];
					double local_seconds = 0;
					if (audiobookdb_get_position(job.path, local_file, sizeof(local_file), &local_seconds)) {
						audiobookshelf_queue_progress(job.path, local_file, local_seconds,
													audiobookdb_is_finished(job.path), true);
						dl_resume_file[0] = '\0'; // the local position is already pending
					} else if (dl_resume_file[0]) {
						char book_path[1280];
						if (audiobookdb_book_for_file(dl_resume_file, book_path, sizeof(book_path))) {
							audiobookdb_save_position(book_path, dl_resume_file, dl_seconds);
							if (dl_finished) audiobookdb_mark_finished(book_path);
							audiobookdb_abs_checkpoint_t checkpoint;
							if (dl_updated_ms > 0 && audiobookdb_abs_get(book_path, &checkpoint))
								audiobookdb_abs_import(&checkpoint, dl_server, dl_resume_file, dl_seconds,
													   dl_finished, dl_updated_ms);
							printf("audiobookshelf: linked copy imported resume %.1f s into %s\n",
								   dl_seconds, dl_resume_file);
						}
					}
				}
			}
		}
		if (!done.ok && !done.error[0])
			snprintf(done.error, sizeof(done.error), "%s", audiobookshelf_last_error());

		// One locked handoff to the GUI thread: the result. The 0.5.x
		// downloaded_* singleton handoff is gone: downloads are queue jobs
		// whose rows carry their own resume values (imported per-row by the
		// scan-completion coordinator), and the link's own import happened
		// above, in this worker.
		pthread_mutex_lock(&job_lock);
		result = done;
		pthread_mutex_unlock(&job_lock);
		gui_post(job_done_async, NULL);
	}
	return NULL;
}

static void start_worker(void) {
	if (worker_started) return;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 192 * 1024);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	pthread_t thread;
	if (pthread_create(&thread, &attr, worker_main, NULL) == 0) worker_started = true;
	pthread_attr_destroy(&attr);
}

static void page_loaded_cb(lv_event_t *e) {
	(void)e;
	if (returning_from_search) { returning_from_search = false; return; }
	if (!audiobookshelf_configured() || !wifi_connected()) {
		draw_libraries();
		return;
	}
	job_t job = {.kind = JOB_LIBRARIES};
	run_loading(&job);
}

// The search field's shape: one line, card fill, caret styled for the keyboard.
// No focus callback is attached here -- each screen wires its own, since a tap
// must point that screen's keyboard at the field, not another screen's.
static lv_obj_t *make_field(lv_obj_t *parent, const char *placeholder, int y) {
	lv_obj_t *field = lv_textarea_create(parent);
	lv_textarea_set_one_line(field, true);
	lv_textarea_set_max_length(field, 1023);
	lv_textarea_set_placeholder_text(field, tr(placeholder));
	lv_obj_set_size(field, config->screen_width - 2 * config->padding, 62);
	lv_obj_set_scrollbar_mode(field, LV_SCROLLBAR_MODE_OFF);
	lv_obj_align(field, LV_ALIGN_TOP_LEFT, config->padding, y);
	lv_obj_add_style(field, &theme_style_card, 0);
	lv_obj_set_style_radius(field, 12, 0);
	lv_obj_set_style_border_width(field, 0, 0);
	lv_obj_set_style_shadow_width(field, 0, 0);
	lv_obj_set_style_pad_all(field, 14, 0);
	lv_obj_set_style_text_font(field, &font_ui_22, 0);
	keyboard_style_caret(field);
	return field;
}

void audiobookshelfpage_init(gui_config_t *cfg) {
	config = cfg;
	// The queue screen is an ABS-family page: built with this one so every
	// link to it (the Downloads row, a download confirm) exists up front. Its
	// engine boundary is abs_download.h only — armed at boot in main.c, before
	// any page exists.
	audiobookshelfqueue_init(cfg);
	audiobookshelf_screen = lv_obj_create(NULL);
	content = settingsrow_page(audiobookshelf_screen, cfg, "audiobookshelf");
	page_title = settingsrow_page_title(audiobookshelf_screen);
	// The picker owns the profile editor too (§11.6): its delegate tells this
	// page when a selection or new active profile needs remote state cleared and
	// libraries loaded, when an edit to the active profile must be validated in
	// place, and when the last profile was removed.
	audiobookshelfprofiles_delegate_t profiles_delegate = {
		.on_selected = profile_selected_cb,
		.on_validate = validate_profile_cb,
		.on_active_lost = active_lost_cb,
		.user = NULL,
	};
	audiobookshelfprofiles_init(cfg, &profiles_delegate);
	search_screen = lv_obj_create(NULL);
	lv_obj_add_style(search_screen, &theme_style_screen, 0);
	settingsrow_title(search_screen, config, "search");
	search_field = make_field(search_screen, "search_placeholder", settingsrow_content_top(config));
	lv_textarea_set_max_length(search_field, ABS_NAME_MAX - 1);
	lv_obj_add_state(search_field, LV_STATE_FOCUSED);
	search_keyboard = keyboard_create(search_screen, config->screen_width, 316, search_field,
		&icon_search, NULL, search_accept_cb, NULL);
	switcher_attach_back_gesture(search_screen);
	start_worker();
	draw_libraries();
	lv_obj_add_event_cb(audiobookshelf_screen, page_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	switcher_set_back_guard(audiobookshelf_screen, back_guard);
	switcher_attach_back_gesture(audiobookshelf_screen);
	player_sheet_attach_drag(audiobookshelf_screen, true);
	player_sheet_attach_drag(content, true);
	// §8.5 line 587: the per-job completion watcher. Started with the page so
	// a download finishes (and its import scan fires) whichever screen the
	// user is on; crossings also re-count the Downloads badge. One cheap
	// engine-bit read per tick; observations only at crossings or enqueues.
	queue_watch_busy = abs_download_busy();
	downloads_badge_dirty = true; // count whatever the last session left behind
	queue_watch_timer = lv_timer_create(queue_watch_cb, 500, NULL);
}

// ---------------------------------------------------------------------------
// Search screen flow (§10.1): the field starts with the running search so it
// can be refined; an empty submission returns to ordinary items, with the
// page reset that a search change carries (§10.2).
// ---------------------------------------------------------------------------
