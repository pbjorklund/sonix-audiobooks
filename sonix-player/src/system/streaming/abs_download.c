// ---------------------------------------------------------------------------
// Workstream 2 — Audiobookshelf download queue: storage foundation (§8).
//
// See abs_download.h for the public contract. Implementation notes:
//
// Durability. The SQLite database on the card is the only durable state; the
// RAM mirror exists to serve snapshots and the worker without holding
// read-modify-write sessions open. Every state transition, file commit and
// removal runs inside one transaction and is followed by a mirror reload
// (16 rows, cheap), so the database can never drift from what the GUI saw.
//
// One worker, one active job. `abs_download_pump()` performs exactly one step:
// pausing acknowledgement, auth resolution, identity fetch, one file transfer,
// or finalize+acknowledge. The injected media ops run WITHOUT the queue mutex
// held -- they are slow network work, and pause/cancel must be able to run
// from the GUI thread while they are in flight. Cancellation reaches them
// two ways (§8.1 command semantics): through the `cancel` flag in the op
// context, which the ops poll between units, and through the op context's
// `http_op_t` wake slot (§8.5 line 1028), which the commands shut down so a
// read blocked INSIDE the network stack returns at once -- a paused download
// does not ride out its receive timeout. The worker alone records the
// resulting PAUSED/CANCELLED state after the op returns, and alone reads and
// closes its connections. The ops must not call back into abs_download_pump().
//
// Byte counters. RAM-backed between transitions; persisted at pause, cancel
// and every state transition only (§8.2). They describe the *current file*;
// the equal-weight per-track percentage a GUI computes from file_index and
// file_count stays correct when byte sizes are unknown (-1).
//
// Tokens. The database has no token column. DOWNLOAD_FILE.source_url rows are
// checked for same-origin against the job's normalized server and reject
// userinfo, fragments and credential query parameters; a job's auth snapshot
// lives in worker RAM for one attempt and is zeroed afterwards. Error strings
// are scrubbed of control bytes and credential-looking runs before being
// persisted (§8.4).
//
// Card generation guard. The database file's identity is recorded at init and
// re-checked on every command and pump step: if the card was pulled, replaced
// or the database was swapped underneath us, the queue marks its storage stale
// and refuses everything until quiesce + re-init (the card-reopened path).
// quiesce_storage() persists the active job in the restart-clean state, closes
// the database and disarms the module; every successful re-init runs the §8.2
// idempotent recovery and raises the card generation.
// ---------------------------------------------------------------------------

#include "abs_download.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "src/system/core/utils.h"
#include "src/system/db/sqlite3.h"

// Bounded per-job automatic backoff (§8.4): WAITING_NETWORK jobs are only
// reconsidered after a connectivity change AND after this interval since they
// entered the wait. A change inside the window must not cause an instant
// re-attempt storm. User actions (pause+resume, retry of a FAILED job) bypass
// it. The timer-driven half of this arrives with the WS5 bounded-wait work;
// see the report's missing entry points.
#define ABS_DL_WAIT_BACKOFF_MS (30 * 1000)

static const char *const STATE_NAMES[ABS_DL_STATE_COUNT] = {
	"QUEUED", "FETCHING", "DOWNLOADING", "PAUSING", "PAUSED",
	"WAITING_NETWORK", "FINALIZING", "DONE", "FAILED", "CANCELLED",
};

const char *abs_download_state_name(abs_download_state_t state) {
	if ((int)state < 0 || (int)state >= ABS_DL_STATE_COUNT) {
		return "?";
	}
	return STATE_NAMES[state];
}

// Unfinished states covered by the dedup uniqueness index and the FIFO pick.
// A macro so it concatenates directly into SQL string literals.
#define UNFINISHED_SQL \
	"('QUEUED','FETCHING','DOWNLOADING','PAUSING','PAUSED','WAITING_NETWORK','FINALIZING')"

// Schema version 1, verbatim from §8.2. The database belongs to the card and
// its destination files; tokens are never stored in it.
static const char *const SCHEMA_SQL =
	"CREATE TABLE IF NOT EXISTS DOWNLOAD_JOB("
	"  id INTEGER PRIMARY KEY,"
	"  kind TEXT NOT NULL,"
	"  profile_id TEXT NOT NULL DEFAULT '',"
	"  server TEXT NOT NULL,"
	"  library_id TEXT NOT NULL DEFAULT '',"
	"  item_id TEXT NOT NULL,"
	"  episode_id TEXT NOT NULL DEFAULT '',"
	"  title TEXT NOT NULL DEFAULT '',"
	"  destination TEXT NOT NULL DEFAULT '',"
	"  state TEXT NOT NULL,"
	"  seq INTEGER NOT NULL DEFAULT 0,"
	"  file_index INTEGER NOT NULL DEFAULT 0,"
	"  file_count INTEGER NOT NULL DEFAULT 0,"
	"  bytes_done INTEGER NOT NULL DEFAULT 0,"
	"  bytes_total INTEGER NOT NULL DEFAULT -1,"
	"  error_stage TEXT NOT NULL DEFAULT '',"
	"  error TEXT NOT NULL DEFAULT '',"
	"  index_pending INTEGER NOT NULL DEFAULT 0,"
	"  remote_progress REAL NOT NULL DEFAULT -1,"
	"  remote_finished INTEGER NOT NULL DEFAULT 0,"
	"  remote_updated_ms INTEGER NOT NULL DEFAULT 0,"
	"  resume_file TEXT NOT NULL DEFAULT '',"
	"  resume_seconds REAL NOT NULL DEFAULT -1,"
	"  ack_reset_done INTEGER NOT NULL DEFAULT 0,"
	"  created_ms INTEGER NOT NULL DEFAULT 0,"
	"  updated_ms INTEGER NOT NULL DEFAULT 0);"
	"CREATE TABLE IF NOT EXISTS DOWNLOAD_FILE("
	"  job_id INTEGER NOT NULL,"
	"  ordinal INTEGER NOT NULL,"
	"  source_url TEXT NOT NULL DEFAULT '',"
	"  final_name TEXT NOT NULL,"
	"  start_offset REAL NOT NULL DEFAULT 0,"
	"  duration REAL NOT NULL DEFAULT -1,"
	"  expected_size INTEGER NOT NULL DEFAULT -1,"
	"  final_size INTEGER NOT NULL DEFAULT -1,"
	"  state TEXT NOT NULL DEFAULT 'pending',"
	"  PRIMARY KEY(job_id, ordinal));"
	// §8.2: a partial unique index prevents duplicate unfinished identities
	// while allowing an explicit later enqueue after terminal removal.
	"CREATE UNIQUE INDEX IF NOT EXISTS download_job_unfinished"
	" ON DOWNLOAD_JOB(kind, server, item_id, episode_id)"
	" WHERE state IN " UNFINISHED_SQL ";";

// Stages recorded on terminal errors (§8.4). Anything else is coerced to
// "metadata" so a misbehaving op cannot smuggle arbitrary text into rows.
static const char *const STAGES[] = {"auth", "metadata", "network", "write", "sidecar", "manifest", "index"};

static const char *const FILE_PENDING = "pending";
static const char *const FILE_WRITING = "writing";
static const char *const FILE_COMPLETE = "complete";

// ---------------------------------------------------------------------------
// Module state. Everything below is guarded by `lock`.
// ---------------------------------------------------------------------------

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t work_cv = PTHREAD_COND_INITIALIZER;
// Serializes quiesce itself. §8.5 hands this wave three callers on three
// threads (usb.c export, system.c card detach, main.c shutdown), and quiesce
// deliberately releases `lock` across the worker join -- so `lock` cannot make
// two quiesces one-after-another, and a second one in the join window would
// see worker_alive still true and join the same thread a second time, which is
// undefined. quiesce_lock covers the whole body; nothing else ever takes it,
// so it cannot form a cycle with the join wait inside.
static pthread_mutex_t quiesce_lock = PTHREAD_MUTEX_INITIALIZER;

static abs_download_config_t cfg;       // string members deep-copied below
static char cfg_sd_root[512];
static char db_path[600];               // <sd_root>/.local/audiobookshelf-downloads.db

static bool armed;                      // between init() and quiesce_storage()
static bool stale;                      // card/database seen swapped while armed
static sqlite3 *db;                     // one shared connection while armed
static file_identity_t db_identity;     // recorded at init, re-checked per access

static unsigned card_generation;        // monotonic across inits, process-wide
static unsigned snapshot_generation;    // bumped on every committed visible change

static abs_download_job_t jobs[ABS_DL_JOB_CAP]; // mirror, FIFO order by seq
static int jobs_count;

static bool storage_paused;             // §8.4: ENOSPC/card write failure pauses the whole queue
static bool wait_blocked;               // a job hit the network; FIFO suspended until connectivity changes

static long long active_id;             // job the worker owns, 0 when none
static abs_download_state_t active_state; // mirror of jobs[active].state
static bool active_auth_done;           // auth snapshot resolved for this attempt
static abs_auth_snapshot_t active_auth; // worker RAM only, never on disk
static bool active_auth_held;
enum { STOP_NONE = 0, STOP_PAUSE = 1, STOP_CANCEL = 2 };
static int active_stop;                 // what a PAUSING row resolves to

static volatile int op_cancel;          // published into op contexts; stored atomically
static bool op_running;                 // an ops callback is executing (mutex released)
static bool import_running;             // the scan-completion import pass (§8.3) is executing

// Mutex-external readers (the media ops) acquire-load this flag, so its
// writers store release-evenly; queue-locked readers stay as they were.
static void op_cancel_store(int value) {
	__atomic_store_n(&op_cancel, value, __ATOMIC_RELEASE);
}

// The active attempt's http wake slot (§8.5 line 1028): published into every
// op context, woken by pause/cancel/quiesce, cleared at attempt boundaries.
// The media ops run their HTTP against THIS slot; it outlives any single op
// and carries no connection between attempts.
static http_op_t queue_http_op;

static bool stop_requested;             // worker thread: quiesce asked us to exit
static bool worker_alive;               // worker thread was created
static pthread_t worker_thread;

// The active job's row copy + its file rows, taken under the lock and used by
// the ops (which run unlocked). Static for the same reason the profile and
// sync stores are: no per-step allocation on a 1-core device.
static abs_download_job_t active_job;
static abs_download_file_t active_files[ABS_DL_FILE_CAP];
static int active_file_count;

static void cv_signal(void) { pthread_cond_broadcast(&work_cv); }

// Forward declaration: the card-generation guard is defined with the storage
// helpers, while abs_download_card_path() sits with the path builders.
static bool storage_stale_locked(void);

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

static long long now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	return (long long)ts.tv_sec * 1000 + (long long)ts.tv_nsec / 1000000LL;
}

// §8.4: bounded, sanitized English message. Control bytes become spaces; any
// credential-looking word (token/bearer/authorization/password) — and its
// value run, whether glued ("token=x") or spoken after it ("Bearer x") — is
// redacted before the string can reach a database row or the log. Callers
// already avoid logging URLs with credentials; this is the second line of
// defense, not the first.
static void sanitize_error(char *out, size_t size, const char *src) {
	if (!out || size == 0) {
		return;
	}
	out[0] = '\0';
	if (!src) {
		return;
	}
	static const char *const keys[] = {"token", "bearer", "authorization", "password"};
	char word[64];
	size_t o = 0;
	bool redact_next = false;
	const char *p = src;
	while (*p && o + 1 < size) {
		// One space-separated word; control bytes inside it become spaces.
		size_t w = 0;
		while (*p && (unsigned char)*p != 0x20 && w + 1 < sizeof(word)) {
			char c = (unsigned char)*p < 0x20 ? 0x20 : *p;
			word[w++] = (char)(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
			p++;
		}
		while (*p && (unsigned char)*p == 0x20) {
			p++; // keep splitting runs of separators into word boundaries
		}
		bool glue = w > 0 && (word[w - 1] == ':' || word[w - 1] == '=');
		// A glued value ("token=x") keeps the punctuation; the word after
		// "bearer"/"authorization:" is a value by position.
		bool hit = redact_next;
		for (size_t k = 0; k < sizeof(keys) / sizeof(keys[0]) && !hit; k++) {
			size_t l = strlen(keys[k]);
			if (w >= l && strncmp(word, keys[k], l) == 0) {
				hit = true;
				redact_next = redact_next || glue || w == l;
			}
		}
		if (!hit) {
			redact_next = false;
		}
		if (hit) {
			o += (size_t)snprintf(out + o, size - o, "[redacted] ");
		} else {
			o += (size_t)snprintf(out + o, size - o, "%.*s ", (int)w, word);
		}
	}
	if (o && out[o - 1] == ' ') {
		o--;
	}
	out[o] = '\0';
}

static bool stage_known(const char *stage) {
	if (!stage) {
		return false;
	}
	for (size_t i = 0; i < sizeof(STAGES) / sizeof(STAGES[0]); i++) {
		if (strcmp(stage, STAGES[i]) == 0) {
			return true;
		}
	}
	return false;
}

static void record_error(abs_download_job_t *j, const char *stage, const char *error) {
	snprintf(j->error_stage, sizeof(j->error_stage), "%s", stage_known(stage) ? stage : "metadata");
	sanitize_error(j->error, sizeof(j->error), error && error[0] ? error : "download failed");
}

// Lowercased scheme://authority of a URL, userinfo removed. False for an
// unparseable URL. The authority is the same-origin contract (§8.2).
static bool url_origin(const char *url, char *out, size_t size) {
	out[0] = '\0';
	if (!url || !url[0]) {
		return false;
	}
	for (const char *p = url; *p; p++) {
		if ((unsigned char)*p < 0x20) {
			return false;
		}
	}
	const char *scheme_end = strstr(url, "://");
	if (!scheme_end || scheme_end == url) {
		return false;
	}
	char scheme[16] = {0};
	if ((size_t)(scheme_end - url) >= sizeof(scheme)) {
		return false;
	}
	for (const char *p = url; p < scheme_end; p++) {
		scheme[p - url] = (char)(*p >= 'A' && *p <= 'Z' ? *p - 'A' + 'a' : *p);
	}
	if (strcmp(scheme, "http") != 0 && strcmp(scheme, "https") != 0) {
		return false;
	}
	const char *authority = scheme_end + 3;
	size_t authority_len = strcspn(authority, "/?#");
	if (authority_len == 0 || authority_len >= size || memchr(authority, '@', authority_len)) {
		return false; // user:pass@host is never a row we will store
	}
	snprintf(out, size, "%.*s", (int)authority_len, authority);
	for (char *q = out; *q; q++) {
		if (*q >= 'A' && *q <= 'Z') {
			*q = (char)(*q - 'A' + 'a');
		}
	}
	return true;
}

// A source URL must be same-origin with the job's normalized server, with no
// userinfo, no fragment and no credential query parameter. This is what keeps
// DOWNLOAD_FILE free of tokens without trusting every future caller (§8.2).
static bool source_url_ok(const char *url, const char *server) {
	char url_part[ABS_SERVER_MAX];
	char server_part[ABS_SERVER_MAX];
	if (!url_origin(url, url_part, sizeof(url_part)) ||
		!url_origin(server, server_part, sizeof(server_part))) {
		return false;
	}
	if (strcmp(url_part, server_part) != 0) {
		return false;
	}
	if (strchr(url, '#')) {
		return false;
	}
	static const char *const params[] = {"token", "password", "auth"};
	for (const char *q = strchr(url, '?'); q; q = strchr(q + 1, '&')) {
		for (size_t i = 0; i < sizeof(params) / sizeof(params[0]); i++) {
			size_t l = strlen(params[i]);
			if (strncasecmp(q + 1, params[i], l) == 0 && q[1 + l] == '=') {
				return false;
			}
		}
	}
	return true;
}

// final_name is a basename inside the job destination (§8.2: local basename).
static bool final_name_ok(const char *name) {
	if (!name || !name[0] || strlen(name) >= ABS_DL_NAME_MAX) {
		return false;
	}
	if (strchr(name, '/') || strchr(name, '\\')) {
		return false;
	}
	if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
		return false;
	}
	for (const char *p = name; *p; p++) {
		if ((unsigned char)*p < 0x20) {
			return false;
		}
	}
	return true;
}

// A card-relative destination directory: relative, traversal-free, printable,
// and representable by every downstream consumer (§11.5 via pathlimits).
static bool destination_ok(const char *dest) {
	if (!dest || !dest[0] || dest[0] == '/' || strstr(dest, "//")) {
		return false;
	}
	if (!sonix_linked_destination_ok(dest)) {
		return false;
	}
	const char *p = dest;
	while (*p) {
		const char *slash = strchr(p, '/');
		size_t len = slash ? (size_t)(slash - p) : strlen(p);
		if (len == 0 || len > 255) {
			return false;
		}
		if (len == 1 && p[0] == '.') {
			return false;
		}
		if (len == 2 && p[0] == '.' && p[1] == '.') {
			return false;
		}
		for (const char *q = p; q < p + len; q++) {
			if ((unsigned char)*q < 0x20) {
				return false;
			}
		}
		p = slash ? slash + 1 : p + len;
	}
	return true;
}

// The job's normalized server: http(s) URL with no credential anywhere.
static bool server_ok(const char *server) {
	char origin[ABS_SERVER_MAX];
	if (!url_origin(server, origin, sizeof(origin))) {
		return false;
	}
	if (strchr(server, '#')) {
		return false;
	}
	return true;
}

// Sizes without the 32-bit-long trap: stat runs with 64-bit off_t (the build
// defines _FILE_OFFSET_BITS=64) and everything here speaks long long (§8.4).
static bool file_size_ll(const char *path, long long *size_out) {
	struct stat st;
	if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
		return false;
	}
	*size_out = (long long)st.st_size;
	return true;
}

// "<card>/<card-relative path>", bounded. False on overflow, never truncated:
// a silently shortened path would name a different file on the card than the
// one every record (and §8.2 recovery) spells.
static bool card_path(const char *relative, char *out, size_t size) {
	if (!relative || !relative[0]) {
		return false;
	}
	size_t root_len = strlen(cfg_sd_root);
	if (root_len + strlen(relative) + 2 > size) {
		return false;
	}
	int n = snprintf(out, size, "%.480s/%s", cfg_sd_root, relative);
	return n > 0 && (size_t)n < size;
}

bool abs_download_card_path(const char *relative, char *out, size_t size) {
	if (!out || size == 0) {
		return false;
	}
	if (size) {
		out[0] = '\0';
	}
	bool ok = false;
	pthread_mutex_lock(&lock);
	// Same guard the commands use: a card the queue has declared swapped
	// cannot host new media either.
	if (armed && !storage_stale_locked() && cfg_sd_root[0]) {
		ok = card_path(relative, out, size);
	}
	pthread_mutex_unlock(&lock);
	return ok;
}

// "<card>/<destination>/<basename><suffix>" — the one path builder, so the
// .part policy, recovery and cancel all spell the same file the same way.
static bool job_file_path(const abs_download_job_t *j, const char *name, const char *suffix, char *out,
						  size_t size) {
	if (!final_name_ok(name)) {
		return false;
	}
	char dest[ABS_DL_DEST_MAX + ABS_DL_NAME_MAX + 2];
	int n = snprintf(dest, sizeof(dest), "%.511s/%.255s", j->destination, name);
	if (n < 0 || (size_t)n >= sizeof(dest)) {
		return false;
	}
	if (suffix) {
		if (strlen(dest) + strlen(suffix) + 1 > sizeof(dest)) {
			return false;
		}
		strcat(dest, suffix);
	}
	return card_path(dest, out, size);
}

// ---------------------------------------------------------------------------
// Database plumbing
// ---------------------------------------------------------------------------

static bool db_exec(const char *sql) {
	if (!db) {
		return false;
	}
	char *error = NULL;
	if (sqlite3_exec(db, sql, NULL, NULL, &error) != SQLITE_OK) {
		fprintf(stderr, "abs-download: %s: %s\n", sql, error ? error : "unknown error");
		sqlite3_free(error);
		return false;
	}
	return true;
}

static sqlite3 *open_db_locked(void) {
	if (sqlite3_open(db_path, &db) != SQLITE_OK) {
		fprintf(stderr, "abs-download: cannot open %s: %s\n", db_path,
				db ? sqlite3_errmsg(db) : "out of memory");
		if (db) {
			sqlite3_close(db);
			db = NULL;
		}
		return NULL;
	}
	// Same card-friendly choices as the library, audiobook and podcast
	// databases: a card that can be pulled at any moment, no WAL roll-forward
	// on slow flash, and the §8.2 64 KiB page cache.
	db_exec("PRAGMA synchronous=NORMAL");
	db_exec("PRAGMA journal_mode=TRUNCATE");
	db_exec("PRAGMA cache_size=-64");
	sqlite3_busy_timeout(db, 1000);
	return db;
}

static int schema_version_locked(void) {
	sqlite3_stmt *stmt = NULL;
	int version = -1;
	if (sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &stmt, NULL) == SQLITE_OK) {
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			version = sqlite3_column_int(stmt, 0);
		}
		sqlite3_finalize(stmt);
	}
	return version;
}

static bool ensure_schema_locked(void) {
	int version = schema_version_locked();
	// 0 = the database is being created now (or a foreign empty file). Anything
	// newer than 1 was written by a future player: refuse rather than guess.
	if (version < 0 || version > 1) {
		fprintf(stderr, "abs-download: refusing download database schema version %d\n", version);
		return false;
	}
	if (!db_exec(SCHEMA_SQL)) {
		return false;
	}
	if (version == 0 && !db_exec("PRAGMA user_version=1")) {
		return false;
	}
	return true;
}

static sqlite3_stmt *prepare_locked(const char *sql) {
	sqlite3_stmt *stmt = NULL;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
		fprintf(stderr, "abs-download: prepare failed: %s\n", sqlite3_errmsg(db));
		return NULL;
	}
	return stmt;
}

// The card-generation guard at runtime: one stat per access. If the database
// file was deleted or replaced (card pulled, swapped, or overwritten from a
// PC), the open handle answers from a file that is no longer on the card.
// Disarm the queue in that case — commands fail and the snapshot reports
// generation 0 until quiesce + re-init rearms it (§8.5 system.c/usb.c).
static bool storage_stale_locked(void) {
	if (stale || !armed) {
		return stale;
	}
	if (file_identity_changed(db_path, &db_identity)) {
		fprintf(stderr, "abs-download: the download database was replaced or removed; "
						"the queue is disarmed until the card is re-inited\n");
		sqlite3_close(db);
		db = NULL;
		stale = true;
	}
	return stale;
}

static const char *col_text(sqlite3_stmt *stmt, int index) {
	const char *text = (const char *)sqlite3_column_text(stmt, index);
	return text ? text : "";
}

// ---------------------------------------------------------------------------
// DOWNLOAD_JOB row mapping
// ---------------------------------------------------------------------------

static abs_download_state_t state_from_sql(const char *s) {
	for (int i = 0; i < ABS_DL_STATE_COUNT; i++) {
		if (s && strcmp(s, STATE_NAMES[i]) == 0) {
			return (abs_download_state_t)i;
		}
	}
	return ABS_DL_FAILED;
}

static void bind_text_or_empty(sqlite3_stmt *stmt, int index, const char *text) {
	sqlite3_bind_text(stmt, index, text ? text : "", -1, SQLITE_TRANSIENT);
}

static void job_from_row(sqlite3_stmt *stmt, abs_download_job_t *j) {
	memset(j, 0, sizeof(*j));
	j->id = sqlite3_column_int64(stmt, 0);
	snprintf(j->kind, sizeof(j->kind), "%s", col_text(stmt, 1));
	snprintf(j->profile_id, sizeof(j->profile_id), "%s", col_text(stmt, 2));
	snprintf(j->server, sizeof(j->server), "%s", col_text(stmt, 3));
	snprintf(j->library_id, sizeof(j->library_id), "%s", col_text(stmt, 4));
	snprintf(j->item_id, sizeof(j->item_id), "%s", col_text(stmt, 5));
	snprintf(j->episode_id, sizeof(j->episode_id), "%s", col_text(stmt, 6));
	snprintf(j->title, sizeof(j->title), "%s", col_text(stmt, 7));
	snprintf(j->destination, sizeof(j->destination), "%s", col_text(stmt, 8));
	j->state = state_from_sql(col_text(stmt, 9));
	j->seq = sqlite3_column_int64(stmt, 10);
	j->file_index = sqlite3_column_int(stmt, 11);
	j->file_count = sqlite3_column_int(stmt, 12);
	j->bytes_done = sqlite3_column_int64(stmt, 13);
	j->bytes_total = sqlite3_column_int64(stmt, 14);
	snprintf(j->error_stage, sizeof(j->error_stage), "%s", col_text(stmt, 15));
	snprintf(j->error, sizeof(j->error), "%s", col_text(stmt, 16));
	j->index_pending = sqlite3_column_int(stmt, 17);
	j->remote_progress = sqlite3_column_double(stmt, 18);
	j->remote_finished = sqlite3_column_int(stmt, 19);
	j->remote_updated_ms = sqlite3_column_int64(stmt, 20);
	snprintf(j->resume_file, sizeof(j->resume_file), "%s", col_text(stmt, 21));
	j->resume_seconds = sqlite3_column_double(stmt, 22);
	j->ack_reset_done = sqlite3_column_int(stmt, 23);
	j->created_ms = sqlite3_column_int64(stmt, 24);
	j->updated_ms = sqlite3_column_int64(stmt, 25);
	j->generation = snapshot_generation;
}

// One committed change = reload the mirror and tag the snapshot generation.
// The generation is the coalescing hint a >=4 Hz GUI poll compares (§8.1).
static void reload_locked(void) {
	jobs_count = 0;
	active_id = 0;
	active_state = ABS_DL_QUEUED;
	sqlite3_stmt *stmt = prepare_locked("SELECT * FROM DOWNLOAD_JOB ORDER BY seq");
	if (stmt) {
		while (jobs_count < ABS_DL_JOB_CAP && sqlite3_step(stmt) == SQLITE_ROW) {
			abs_download_job_t *j = &jobs[jobs_count++];
			job_from_row(stmt, j);
			if (j->state == ABS_DL_FETCHING || j->state == ABS_DL_DOWNLOADING ||
				j->state == ABS_DL_PAUSING || j->state == ABS_DL_FINALIZING) {
				if (!active_id) {
					active_id = j->id;
					active_state = j->state;
				}
			}
		}
		sqlite3_finalize(stmt);
	}
	snapshot_generation++;
}

// Persist the mutable work columns of one job row inside the caller's
// transaction. Identity columns (kind/server/item/episode/created) never
// change after enqueue. `destination` is identity in practice but assigned by
// the identity fetch for jobs that enqueued without one, so it travels with
// every write like the other work columns.
static bool persist_job_locked(const abs_download_job_t *j) {
	sqlite3_stmt *stmt = prepare_locked(
		"UPDATE DOWNLOAD_JOB SET state=?, file_index=?, file_count=?, bytes_done=?, bytes_total=?,"
		" error_stage=?, error=?, index_pending=?, remote_progress=?, remote_finished=?,"
		" remote_updated_ms=?, resume_file=?, resume_seconds=?, ack_reset_done=?, updated_ms=?,"
		" destination=? WHERE id=?");
	if (!stmt) {
		return false;
	}
	bind_text_or_empty(stmt, 1, STATE_NAMES[j->state]);
	sqlite3_bind_int(stmt, 2, j->file_index);
	sqlite3_bind_int(stmt, 3, j->file_count);
	sqlite3_bind_int64(stmt, 4, (sqlite3_int64)j->bytes_done);
	sqlite3_bind_int64(stmt, 5, (sqlite3_int64)j->bytes_total);
	bind_text_or_empty(stmt, 6, j->error_stage);
	bind_text_or_empty(stmt, 7, j->error);
	sqlite3_bind_int(stmt, 8, j->index_pending);
	sqlite3_bind_double(stmt, 9, j->remote_progress);
	sqlite3_bind_int(stmt, 10, j->remote_finished);
	sqlite3_bind_int64(stmt, 11, (sqlite3_int64)j->remote_updated_ms);
	bind_text_or_empty(stmt, 12, j->resume_file);
	sqlite3_bind_double(stmt, 13, j->resume_seconds);
	sqlite3_bind_int(stmt, 14, j->ack_reset_done);
	sqlite3_bind_int64(stmt, 15, (sqlite3_int64)now_ms());
	bind_text_or_empty(stmt, 16, j->destination);
	sqlite3_bind_int64(stmt, 17, (sqlite3_int64)j->id);
	bool ok = sqlite3_step(stmt) == SQLITE_DONE;
	sqlite3_finalize(stmt);
	if (!ok) {
		fprintf(stderr, "abs-download: cannot update job %lld: %s\n", j->id, sqlite3_errmsg(db));
	}
	return ok;
}

// Persist one job row in its own transaction and refresh the mirror: the only
// single-job write path outside multi-row transactions. Every committed state
// change is user-visible, so every apply tags a new snapshot generation.
static bool apply_job_locked(const abs_download_job_t *j) {
	db_exec("BEGIN");
	if (!persist_job_locked(j)) {
		db_exec("ROLLBACK");
		return false;
	}
	if (!db_exec("COMMIT")) {
		db_exec("ROLLBACK");
		return false;
	}
	reload_locked();
	return true;
}

static abs_download_job_t *job_by_id_locked(long long id) {
	for (int i = 0; i < jobs_count; i++) {
		if (jobs[i].id == id) {
			return &jobs[i];
		}
	}
	return NULL;
}

// ---------------------------------------------------------------------------
// DOWNLOAD_FILE rows
// ---------------------------------------------------------------------------

int abs_download_job_files(long long job_id, abs_download_file_t *out, int max) {
	if (!out || max <= 0) {
		return 0;
	}
	pthread_mutex_lock(&lock);
	int n = 0;
	if (armed && !storage_stale_locked() && db) {
		sqlite3_stmt *stmt = prepare_locked(
			"SELECT job_id, ordinal, source_url, final_name, start_offset, duration,"
			" expected_size, final_size, state FROM DOWNLOAD_FILE WHERE job_id=? ORDER BY ordinal");
		if (stmt) {
			sqlite3_bind_int64(stmt, 1, (sqlite3_int64)job_id);
			while (n < max && n < ABS_DL_FILE_CAP && sqlite3_step(stmt) == SQLITE_ROW) {
				abs_download_file_t *f = &out[n++];
				f->job_id = sqlite3_column_int64(stmt, 0);
				f->ordinal = sqlite3_column_int(stmt, 1);
				snprintf(f->source_url, sizeof(f->source_url), "%s", col_text(stmt, 2));
				snprintf(f->final_name, sizeof(f->final_name), "%s", col_text(stmt, 3));
				f->start_offset = sqlite3_column_double(stmt, 4);
				f->duration = sqlite3_column_double(stmt, 5);
				f->expected_size = sqlite3_column_int64(stmt, 6);
				f->final_size = sqlite3_column_int64(stmt, 7);
				snprintf(f->state, sizeof(f->state), "%s", col_text(stmt, 8));
			}
			sqlite3_finalize(stmt);
		}
	}
	pthread_mutex_unlock(&lock);
	return n;
}

// Delete every job-owned .part on the card. Only non-complete rows can own a
// .part (completed files were renamed); audio, sidecars, covers, manifests and
// resume state are never touched here (§8.1 Remove, §8.2 file rules).
static void delete_job_parts_locked(const abs_download_job_t *j) {
	if (!j->destination[0]) {
		return;
	}
	sqlite3_stmt *stmt = prepare_locked(
		"SELECT final_name FROM DOWNLOAD_FILE WHERE job_id=? AND state<>'complete'");
	if (!stmt) {
		return;
	}
	sqlite3_bind_int64(stmt, 1, (sqlite3_int64)j->id);
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		char part[SONIX_LINKED_SIDECAR_PATH_MAX];
		if (job_file_path(j, col_text(stmt, 0), ".part", part, sizeof(part))) {
			unlink(part);
		}
	}
	sqlite3_finalize(stmt);
}

// An in-flight file goes back to `pending` when the attempt stops: 0.6.0 has
// no HTTP Range resume, so the next attempt restarts that file at byte zero
// (§8.1); the retained .part may be truncated then.
static void reset_writing_file_locked(long long job_id) {
	sqlite3_stmt *stmt = prepare_locked(
		"UPDATE DOWNLOAD_FILE SET state='pending' WHERE job_id=? AND state='writing'");
	if (stmt) {
		sqlite3_bind_int64(stmt, 1, (sqlite3_int64)job_id);
		sqlite3_step(stmt);
		sqlite3_finalize(stmt);
	}
}

// ---------------------------------------------------------------------------
// Restart recovery (§8.2 recovery table). Idempotent; one transaction at init.
// ---------------------------------------------------------------------------

// One job's file/destination reconciliation:
// - a `writing` record whose final file exists is accepted only with a known,
//   exact expected size; any other final file is quarantined as a FAILED job
//   for user retry instead of being treated as valid content;
// - a file recorded complete (and still matching its committed size) is kept;
// - anything that cannot be trusted resets to `pending` for the next attempt.
static void reconcile_job_locked(abs_download_job_t *j) {
	if (!j->destination[0]) {
		return;
	}
	sqlite3_stmt *select = prepare_locked(
		"SELECT ordinal, final_name, state, expected_size, final_size FROM DOWNLOAD_FILE"
		" WHERE job_id=? ORDER BY ordinal");
	if (!select) {
		return;
	}
	sqlite3_bind_int64(select, 1, (sqlite3_int64)j->id);
	bool quarantined = false;
	while (!quarantined && sqlite3_step(select) == SQLITE_ROW) {
		int ordinal = sqlite3_column_int(select, 0);
		const char *name = col_text(select, 1);
		const char *row_state = col_text(select, 2);
		long long expected = sqlite3_column_int64(select, 3);
		long long committed = sqlite3_column_int64(select, 4);

		char final_path[SONIX_LINKED_SIDECAR_PATH_MAX];
		if (!job_file_path(j, name, "", final_path, sizeof(final_path))) {
			continue;
		}
		long long size = 0;
		bool exists = file_size_ll(final_path, &size);

		if (strcmp(row_state, FILE_COMPLETE) == 0) {
			if (!exists || (committed >= 0 && size != committed)) {
				// The record can no longer be trusted: redownload on the next
				// attempt. The stale bytes stay on the card untouched; the
				// retry writes through <final>.part and renames over them.
				sqlite3_stmt *reset = prepare_locked(
					"UPDATE DOWNLOAD_FILE SET state='pending', final_size=-1 WHERE job_id=? AND ordinal=?");
				if (reset) {
					sqlite3_bind_int64(reset, 1, (sqlite3_int64)j->id);
					sqlite3_bind_int(reset, 2, ordinal);
					sqlite3_step(reset);
					sqlite3_finalize(reset);
				}
			}
		} else if (strcmp(row_state, FILE_WRITING) == 0) {
			if (exists && expected >= 0 && size == expected) {
				// Renamed before the crash; the bytes are provably complete.
				sqlite3_stmt *accept = prepare_locked(
					"UPDATE DOWNLOAD_FILE SET state='complete', final_size=? WHERE job_id=? AND ordinal=?");
				if (accept) {
					sqlite3_bind_int64(accept, 1, (sqlite3_int64)expected);
					sqlite3_bind_int64(accept, 2, (sqlite3_int64)j->id);
					sqlite3_bind_int(accept, 3, ordinal);
					sqlite3_step(accept);
					sqlite3_finalize(accept);
				}
			} else if (exists) {
				// A `writing` record whose final file exists but does not
				// exactly match its expected size is never accepted (§8.2
				// file rules): fail for user retry rather than treating an
				// arbitrary nonempty file as valid.
				quarantined = true;
			} else {
				sqlite3_stmt *reset = prepare_locked(
					"UPDATE DOWNLOAD_FILE SET state='pending' WHERE job_id=? AND ordinal=?");
				if (reset) {
					sqlite3_bind_int64(reset, 1, (sqlite3_int64)j->id);
					sqlite3_bind_int(reset, 2, ordinal);
					sqlite3_step(reset);
					sqlite3_finalize(reset);
				}
			}
		}
		// `pending` rows need nothing.
	}
	sqlite3_finalize(select);
	if (quarantined) {
		j->state = ABS_DL_FAILED;
		snprintf(j->error_stage, sizeof(j->error_stage), "write");
		snprintf(j->error, sizeof(j->error),
				 "A file from the interrupted download has the wrong size; retry to redownload it");
		persist_job_locked(j);
	}
}

static bool recover_locked(void) {
	if (!db_exec("BEGIN")) {
		return false;
	}
	long long now = now_ms();

	// 1. Interrupted non-paused jobs return to QUEUED; an explicit user pause
	// (PAUSING) persists as PAUSED (§8.1, §8.2). Reconciliation runs first so
	// the next attempt starts from proven records.
	sqlite3_stmt *stmt = prepare_locked(
		"SELECT * FROM DOWNLOAD_JOB WHERE state IN ('FETCHING','DOWNLOADING','PAUSING','WAITING_NETWORK') ORDER BY seq");
	if (stmt) {
		while (sqlite3_step(stmt) == SQLITE_ROW) {
			abs_download_job_t j;
			job_from_row(stmt, &j);
			bool was_pausing = j.state == ABS_DL_PAUSING;
			j.bytes_done = 0;
			j.bytes_total = -1;
			j.file_index = 0;
			reconcile_job_locked(&j);
			if (j.state == ABS_DL_FAILED) {
				continue; // quarantined above; already persisted
			}
			j.state = was_pausing ? ABS_DL_PAUSED : ABS_DL_QUEUED;
			j.error_stage[0] = '\0';
			j.error[0] = '\0';
			j.updated_ms = now;
			persist_job_locked(&j);
		}
		sqlite3_finalize(stmt);
	}

	// 2. CANCELLED rows: delete any remaining job-owned .part; completed files
	// and the terminal state are retained (§8.2).
	stmt = prepare_locked("SELECT * FROM DOWNLOAD_JOB WHERE state='CANCELLED'");
	if (stmt) {
		while (sqlite3_step(stmt) == SQLITE_ROW) {
			abs_download_job_t j;
			job_from_row(stmt, &j);
			delete_job_parts_locked(&j);
		}
		sqlite3_finalize(stmt);
	}

	// 3. FINALIZING with the acknowledgement commit missing (a crash inside
	// the finalize window). §8.2 recovery table line 540: the sync barrier is
	// RE-ESTABLISHED by the persisted row itself — FINALIZING + ack_reset_done=0
	// stays exactly as the crash left it, reload_locked() makes it the active
	// job again, and the worker redoes the finalize step, whose media ops
	// reconcile from the card: a valid manifest completes the acknowledgement
	// (resume rebuild + reset + DONE transaction) offline; a missing one
	// retries finalization only (identity refetch, committed audio reused).
	// One active job exists per process, so at most one such row is expected;
	// extras in a foreign/corrupt database are normalized to a recoverable
	// FAILED job so no stale barrier blocks sync forever (§8.4 sanitizable
	// state, same defensive posture as the DONE normalization below).
	{
		int barrier_rows = 0;
		long long keep_id = 0;
		stmt = prepare_locked(
			"SELECT id FROM DOWNLOAD_JOB WHERE state='FINALIZING' AND ack_reset_done=0 ORDER BY seq");
		if (stmt) {
			while (sqlite3_step(stmt) == SQLITE_ROW) {
				long long id = sqlite3_column_int64(stmt, 0);
				if (!barrier_rows) {
					keep_id = id;
				}
				barrier_rows++;
			}
			sqlite3_finalize(stmt);
		}
		for (int extra = 1; extra < barrier_rows; extra++) {
			// Deterministic extra mapping: every row after the seq-first one.
			stmt = prepare_locked(
				"SELECT id FROM DOWNLOAD_JOB WHERE state='FINALIZING' AND ack_reset_done=0"
				" AND id<>? ORDER BY seq LIMIT 1");
			if (stmt) {
				sqlite3_bind_int64(stmt, 1, (sqlite3_int64)keep_id);
				long long id = sqlite3_step(stmt) == SQLITE_ROW ? sqlite3_column_int64(stmt, 0) : 0;
				sqlite3_finalize(stmt);
				if (id) {
					stmt = prepare_locked(
						"UPDATE DOWNLOAD_JOB SET state='FAILED', error_stage='manifest',"
						" error='Finalization was interrupted by a restart; retry to finish it',"
						" updated_ms=? WHERE id=?");
					if (stmt) {
						sqlite3_bind_int64(stmt, 1, (sqlite3_int64)now);
						sqlite3_bind_int64(stmt, 2, (sqlite3_int64)id);
						sqlite3_step(stmt);
						sqlite3_finalize(stmt);
					}
				}
			}
		}
	}

	// 4. A DONE row whose acknowledgement commit has not landed cannot result
	// from this module (DONE + ack_reset_done commit together, §8.2 step 8);
	// a foreign or corrupted row is normalized defensively: never re-reset,
	// release any stale barrier, keep the manifest (§8.2).
	if (!db_exec("UPDATE DOWNLOAD_JOB SET ack_reset_done=1, index_pending=1 WHERE state='DONE' AND ack_reset_done=0")) {
		db_exec("ROLLBACK");
		return false;
	}

	// 5. FAILED rows keep their state and error; completed files and .part
	// files are left exactly as the attempt left them (retry truncates from
	// byte zero; Remove cleans the part). PAUSED/QUEUED rows were reconciled
	// in step 1; DONE rows with ack_reset_done=1 need nothing.
	return db_exec("COMMIT");
}

// ---------------------------------------------------------------------------
// Enqueue (§8.1: durable intent, dedup, capacity before network)
// ---------------------------------------------------------------------------

static bool copy_request_string(char *out, size_t size, const char *value, bool required, const char *what,
								char *error, size_t error_size) {
	if (!value || (!value[0] && required)) {
		snprintf(error, error_size, "%s is required", what);
		return false;
	}
	if (value[0] && (int)strlen(value) >= (int)size) {
		snprintf(error, error_size, "%s is too long", what);
		return false;
	}
	snprintf(out, size, "%s", value);
	return true;
}

static bool run_enqueue(const abs_download_enqueue_t *req, bool is_book, long long *job_id_out,
						bool *deduped_out, char *error, size_t error_size) {
	if (error_size) {
		error[0] = '\0';
	}
	if (deduped_out) {
		*deduped_out = false;
	}
	if (job_id_out) {
		*job_id_out = 0;
	}
	if (!req || !error || !error_size) {
		return false;
	}

	char kind[ABS_DL_KIND_MAX] = {0};
	char profile_id[ABS_DL_PROFILE_ID_MAX] = {0};
	char server[ABS_DL_SERVER_MAX] = {0};
	char library_id[ABS_DL_ID_MAX] = {0};
	char item_id[ABS_DL_ID_MAX] = {0};
	char episode_id[ABS_DL_ID_MAX] = {0};
	char title[ABS_DL_TITLE_MAX] = {0};
	char destination[ABS_DL_DEST_MAX] = {0};

	snprintf(kind, sizeof(kind), "%s", is_book ? ABS_DL_KIND_BOOK : ABS_DL_KIND_EPISODE);
	if (!copy_request_string(profile_id, sizeof(profile_id), req->profile_id, true, "Profile", error, error_size) ||
		!copy_request_string(server, sizeof(server), req->server, true, "Server", error, error_size) ||
		!copy_request_string(item_id, sizeof(item_id), req->item_id, true, "Item", error, error_size) ||
		!copy_request_string(library_id, sizeof(library_id), req->library_id, false, "Library", error, error_size) ||
		!copy_request_string(title, sizeof(title), req->title, true, "Title", error, error_size) ||
		!copy_request_string(destination, sizeof(destination), req->destination, false, "Destination", error,
							 error_size)) {
		return false;
	}
	if (is_book) {
		if (req->episode_id && req->episode_id[0]) {
			snprintf(error, error_size, "A book enqueue cannot carry an episode id");
			return false;
		}
	} else if (!copy_request_string(episode_id, sizeof(episode_id), req->episode_id, true, "Episode", error,
									error_size)) {
		return false;
	}
	if (!server_ok(server)) {
		snprintf(error, error_size, "Server must be the normalized http(s) URL without credentials");
		return false;
	}
	// The identity fetch may still assign the destination; a caller-provided
	// one is validated here so a bad path is refused before any network
	// activity (§8.1, §8.2 step 3).
	if (destination[0] && !destination_ok(destination)) {
		snprintf(error, error_size, "Destination is not a representable card-relative directory");
		return false;
	}

	pthread_mutex_lock(&lock);
	if (!armed || storage_stale_locked()) {
		snprintf(error, error_size, "Download storage is not available");
		pthread_mutex_unlock(&lock);
		return false;
	}
	if (!db_exec("BEGIN IMMEDIATE")) {
		snprintf(error, error_size, "Cannot open a queue transaction");
		pthread_mutex_unlock(&lock);
		return false;
	}

	// §8.1 dedup: an unfinished identity opens its existing row. The partial
	// unique index is the transactional backstop for any racing writer.
	sqlite3_stmt *stmt = prepare_locked(
		"SELECT id FROM DOWNLOAD_JOB WHERE kind=? AND server=? AND item_id=? AND episode_id=?"
		" AND state IN " UNFINISHED_SQL);
	bool have_existing = false;
	bool inserted_now = false;
	long long existing = 0;
	if (stmt) {
		bind_text_or_empty(stmt, 1, kind);
		bind_text_or_empty(stmt, 2, server);
		bind_text_or_empty(stmt, 3, item_id);
		bind_text_or_empty(stmt, 4, episode_id);
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			existing = sqlite3_column_int64(stmt, 0);
			have_existing = true;
		}
		sqlite3_finalize(stmt);
	}
	if (!have_existing) {
		int count = 0;
		stmt = prepare_locked("SELECT COUNT(*) FROM DOWNLOAD_JOB");
		if (stmt) {
			if (sqlite3_step(stmt) == SQLITE_ROW) {
				count = sqlite3_column_int(stmt, 0);
			}
			sqlite3_finalize(stmt);
		}
		if (count >= ABS_DL_JOB_CAP) {
			// §8.1: the bound trips the enqueue before any network activity.
			db_exec("ROLLBACK");
			snprintf(error, error_size, "Queue is full (%d downloads); remove or clear finished ones",
					 ABS_DL_JOB_CAP);
			pthread_mutex_unlock(&lock);
			return false;
		}
		// seq is strictly increasing and never reused: removed rows leave no
		// gaps that re-enqueues could jump into.
		long long seq = -1;
		stmt = prepare_locked("SELECT COALESCE(MAX(seq), -1) FROM DOWNLOAD_JOB");
		if (stmt) {
			if (sqlite3_step(stmt) == SQLITE_ROW) {
				seq = sqlite3_column_int64(stmt, 0);
			}
			sqlite3_finalize(stmt);
		}
		stmt = prepare_locked(
			"INSERT INTO DOWNLOAD_JOB(kind, profile_id, server, library_id, item_id, episode_id, title,"
			" destination, state, seq, created_ms, updated_ms, remote_progress, remote_finished,"
			" remote_updated_ms, index_pending, ack_reset_done)"
			" VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
		if (stmt) {
			bind_text_or_empty(stmt, 1, kind);
			bind_text_or_empty(stmt, 2, profile_id);
			bind_text_or_empty(stmt, 3, server);
			bind_text_or_empty(stmt, 4, library_id);
			bind_text_or_empty(stmt, 5, item_id);
			bind_text_or_empty(stmt, 6, episode_id);
			bind_text_or_empty(stmt, 7, title);
			bind_text_or_empty(stmt, 8, destination);
			bind_text_or_empty(stmt, 9, "QUEUED");
			sqlite3_bind_int64(stmt, 10, (sqlite3_int64)(seq + 1));
			sqlite3_bind_int64(stmt, 11, (sqlite3_int64)now_ms());
			sqlite3_bind_int64(stmt, 12, (sqlite3_int64)now_ms());
			sqlite3_bind_double(stmt, 13, req->remote_progress >= 0 ? req->remote_progress : -1);
			// A seed from the browse view: remote state travels with the job
			// so the finalize step can build the resume mapping even before
			// the fetch plan lands (§8.2 step 5).
			sqlite3_bind_int(stmt, 14, req->remote_finished ? 1 : 0);
			sqlite3_bind_int64(stmt, 15, (sqlite3_int64)req->remote_updated_ms);
			sqlite3_bind_int(stmt, 16, 0); // index_pending
			sqlite3_bind_int(stmt, 17, 0); // ack_reset_done
			bool inserted = sqlite3_step(stmt) == SQLITE_DONE;
			long long new_id = inserted ? (long long)sqlite3_last_insert_rowid(db) : 0;
			sqlite3_finalize(stmt);
			if (inserted && new_id > 0) {
				existing = new_id;
				have_existing = true;
				inserted_now = true;
			}
		}
	}
	if (!have_existing) {
		// Only reachable via a writer outside this process; re-check the
		// identity so a duplicate becomes the friendly "opens existing row".
		db_exec("ROLLBACK");
		stmt = prepare_locked(
			"SELECT id FROM DOWNLOAD_JOB WHERE kind=? AND server=? AND item_id=? AND episode_id=?"
			" AND state IN " UNFINISHED_SQL);
		if (stmt) {
			bind_text_or_empty(stmt, 1, kind);
			bind_text_or_empty(stmt, 2, server);
			bind_text_or_empty(stmt, 3, item_id);
			bind_text_or_empty(stmt, 4, episode_id);
			if (sqlite3_step(stmt) == SQLITE_ROW) {
				existing = sqlite3_column_int64(stmt, 0);
				have_existing = true;
			}
			sqlite3_finalize(stmt);
		}
		if (!have_existing) {
			snprintf(error, error_size, "Cannot write the queue row");
			pthread_mutex_unlock(&lock);
			return false;
		}
	}
	if (!db_exec("COMMIT")) {
		db_exec("ROLLBACK");
		snprintf(error, error_size, "Cannot commit the queue row");
		pthread_mutex_unlock(&lock);
		return false;
	}
	if (job_id_out) {
		*job_id_out = existing;
	}
	if (deduped_out) {
		// True only when an unfinished identity existed before this call
		// (§8.1: selecting an already queued item opens its existing row).
		*deduped_out = !inserted_now;
	}
	reload_locked();
	cv_signal();
	pthread_mutex_unlock(&lock);
	return true;
}

bool abs_download_enqueue_book(const abs_download_enqueue_t *req, long long *job_id_out, bool *deduped_out,
							   char *error, size_t error_size) {
	return run_enqueue(req, true, job_id_out, deduped_out, error, error_size);
}

bool abs_download_enqueue_episode(const abs_download_enqueue_t *req, long long *job_id_out, bool *deduped_out,
								  char *error, size_t error_size) {
	return run_enqueue(req, false, job_id_out, deduped_out, error, error_size);
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

static bool command_ready_locked(char *error, size_t error_size) {
	if (!armed || storage_stale_locked()) {
		snprintf(error, error_size, "Download storage is not available");
		return false;
	}
	return true;
}

bool abs_download_pause(long long job_id, char *error, size_t error_size) {
	if (error_size) {
		error[0] = '\0';
	}
	bool ok = true;
	pthread_mutex_lock(&lock);
	if (!command_ready_locked(error, error_size)) {
		pthread_mutex_unlock(&lock);
		return false;
	}
	abs_download_job_t *j = job_by_id_locked(job_id);
	if (!j) {
		snprintf(error, error_size, "Download not found");
		pthread_mutex_unlock(&lock);
		return false;
	}
	switch (j->state) {
	case ABS_DL_QUEUED:
	case ABS_DL_WAITING_NETWORK:
		// §8.1: a queued pause is immediate and durable.
		j->state = ABS_DL_PAUSED;
		j->error_stage[0] = '\0';
		j->error[0] = '\0';
		ok = apply_job_locked(j);
		cv_signal();
		break;
	case ABS_DL_FETCHING:
	case ABS_DL_DOWNLOADING:
	case ABS_DL_FINALIZING:
		// Active: record PAUSING durably, publish the stop through the cancel
		// flag, and let only the worker acknowledge (§8.1). During FINALIZING
		// the pause is deferred to the end of the short atomic finalize; the
		// intent flag carries it there.
		j->state = ABS_DL_PAUSING;
		ok = apply_job_locked(j);
		if (ok && active_stop == STOP_NONE) {
			active_stop = STOP_PAUSE;
		}
		if (ok) {
			op_cancel_store(1);
			// §8.5 line 1028: the stop is published AND the in-flight
			// connection is shut down, so a pause does not wait for the
			// blocked read/connect to finish or time out. The PAUSING row is
			// already durable, so the snapshot said "Stopping" from now on.
			http_op_cancel(&queue_http_op);
			cv_signal();
		}
		break;
	case ABS_DL_PAUSING:
	case ABS_DL_PAUSED:
		break; // already stopped or stopping; idempotent
	default:
		snprintf(error, error_size, "Download already finished");
		ok = false;
		break;
	}
	pthread_mutex_unlock(&lock);
	return ok;
}

bool abs_download_resume(long long job_id, char *error, size_t error_size) {
	if (error_size) {
		error[0] = '\0';
	}
	bool ok = false;
	pthread_mutex_lock(&lock);
	if (!command_ready_locked(error, error_size)) {
		pthread_mutex_unlock(&lock);
		return false;
	}
	abs_download_job_t *j = job_by_id_locked(job_id);
	if (!j) {
		snprintf(error, error_size, "Download not found");
		pthread_mutex_unlock(&lock);
		return false;
	}
	// §8.1: resume returns a PAUSED job to QUEUED. Completed files recorded by
	// the queue are skipped; the interrupted current file restarts at byte
	// zero. User resume bypasses the WAITING_NETWORK backoff by design.
	if (j->state != ABS_DL_PAUSED) {
		snprintf(error, error_size, "Only a paused download can be resumed");
	} else {
		j->state = ABS_DL_QUEUED;
		j->error_stage[0] = '\0';
		j->error[0] = '\0';
		ok = apply_job_locked(j);
		if (ok) {
			// User action (§8.4): resuming past a card-full pause records the
			// user's space recovery, and telling us the road ahead is clear
			// lifts a network stall; a fresh failure re-pauses either.
			storage_paused = false;
			wait_blocked = false;
		}
		cv_signal();
	}
	pthread_mutex_unlock(&lock);
	return ok;
}

// Non-active cancel: immediate, deletes job-owned .part files, keeps every
// completed file (§8.1). Idempotent on CANCELLED rows.
static bool cancel_inactive_locked(abs_download_job_t *j) {
	if (j->state == ABS_DL_CANCELLED) {
		return true;
	}
	j->state = ABS_DL_CANCELLED;
	j->error_stage[0] = '\0';
	j->error[0] = '\0';
	db_exec("BEGIN");
	if (persist_job_locked(j)) {
		reset_writing_file_locked(j->id);
	}
	bool ok = db_exec("COMMIT");
	if (!ok) {
		db_exec("ROLLBACK");
	} else {
		delete_job_parts_locked(j);
		if (storage_paused) {
			storage_paused = false; // user action on the card-full pause (§8.4)
		}
		reload_locked();
		cv_signal();
	}
	return ok;
}

// Active cancel: PAUSING + the cancel flag + the wake; the worker records
// CANCELLED after the transfer acknowledges and cleans the job-owned .part
// (§8.1).
static bool cancel_active_locked(abs_download_job_t *j) {
	j->state = ABS_DL_PAUSING;
	bool ok = apply_job_locked(j);
	if (ok) {
		active_stop = STOP_CANCEL;
		op_cancel_store(1);
		http_op_cancel(&queue_http_op);
		cv_signal();
	}
	return ok;
}

bool abs_download_cancel(long long job_id, char *error, size_t error_size) {
	if (error_size) {
		error[0] = '\0';
	}
	bool ok;
	pthread_mutex_lock(&lock);
	if (!command_ready_locked(error, error_size)) {
		pthread_mutex_unlock(&lock);
		return false;
	}
	abs_download_job_t *j = job_by_id_locked(job_id);
	if (!j) {
		snprintf(error, error_size, "Download not found");
		pthread_mutex_unlock(&lock);
		return false;
	}
	bool active = j->state == ABS_DL_FETCHING || j->state == ABS_DL_DOWNLOADING ||
				  j->state == ABS_DL_PAUSING || j->state == ABS_DL_FINALIZING;
	if (j->state == ABS_DL_CANCELLED) {
		pthread_mutex_unlock(&lock);
		return true; // cancellation is idempotent (§8.1)
	}
	if (j->state == ABS_DL_DONE) {
		snprintf(error, error_size, "Download already finished; remove it instead");
		pthread_mutex_unlock(&lock);
		return false;
	}
	if (j->state == ABS_DL_FAILED) {
		snprintf(error, error_size, "Failed downloads are removed or retried, not cancelled");
		pthread_mutex_unlock(&lock);
		return false;
	}
	ok = active ? cancel_active_locked(j) : cancel_inactive_locked(j);
	if (!ok) {
		snprintf(error, error_size, "Cannot cancel the download now");
	}
	pthread_mutex_unlock(&lock);
	return ok;
}

bool abs_download_remove(long long job_id, char *error, size_t error_size) {
	if (error_size) {
		error[0] = '\0';
	}
	bool ok = false;
	pthread_mutex_lock(&lock);
	if (!command_ready_locked(error, error_size)) {
		pthread_mutex_unlock(&lock);
		return false;
	}
	abs_download_job_t *j = job_by_id_locked(job_id);
	if (!j) {
		snprintf(error, error_size, "Download not found");
		pthread_mutex_unlock(&lock);
		return false;
	}
	bool active = j->state == ABS_DL_FETCHING || j->state == ABS_DL_DOWNLOADING ||
				  j->state == ABS_DL_PAUSING || j->state == ABS_DL_FINALIZING;
	if (active) {
		// §8.1: active jobs must acknowledge cancellation before Remove.
		snprintf(error, error_size, "Stop the download before removing it");
		pthread_mutex_unlock(&lock);
		return false;
	}
	db_exec("BEGIN");
	sqlite3_stmt *del = prepare_locked("DELETE FROM DOWNLOAD_FILE WHERE job_id=?");
	if (del) {
		sqlite3_bind_int64(del, 1, (sqlite3_int64)job_id);
		sqlite3_step(del);
		sqlite3_finalize(del);
	}
	del = prepare_locked("DELETE FROM DOWNLOAD_JOB WHERE id=?");
	if (del) {
		sqlite3_bind_int64(del, 1, (sqlite3_int64)job_id);
		sqlite3_step(del);
		sqlite3_finalize(del);
	}
	ok = db_exec("COMMIT");
	if (!ok) {
		db_exec("ROLLBACK");
	} else {
		delete_job_parts_locked(j); // best effort: a leftover .part is invisible garbage
		if (storage_paused) {
			storage_paused = false;
		}
		reload_locked();
		cv_signal();
	}
	pthread_mutex_unlock(&lock);
	return ok;
}

bool abs_download_retry(long long job_id, char *error, size_t error_size) {
	if (error_size) {
		error[0] = '\0';
	}
	bool ok = false;
	pthread_mutex_lock(&lock);
	if (!command_ready_locked(error, error_size)) {
		pthread_mutex_unlock(&lock);
		return false;
	}
	abs_download_job_t *j = job_by_id_locked(job_id);
	if (!j) {
		snprintf(error, error_size, "Download not found");
		pthread_mutex_unlock(&lock);
		return false;
	}
	if (j->state != ABS_DL_FAILED && j->state != ABS_DL_CANCELLED) {
		snprintf(error, error_size, "Only failed or cancelled downloads can be retried");
		pthread_mutex_unlock(&lock);
		return false;
	}
	// §8.1: retry reuses the destination and the validated file records;
	// completed rows are not redownloaded, the rest restarts from byte zero.
	// A retry past a stalled FIFO is user action (§8.4).
	j->state = ABS_DL_QUEUED;
	j->error_stage[0] = '\0';
	j->error[0] = '\0';
	j->bytes_done = 0;
	j->bytes_total = -1;
	j->file_index = 0;
	ok = apply_job_locked(j);
	if (ok) {
		storage_paused = false;
		wait_blocked = false;
		cv_signal();
	}
	pthread_mutex_unlock(&lock);
	return ok;
}

bool abs_download_clear_finished(char *error, size_t error_size) {
	if (error_size) {
		error[0] = '\0';
	}
	bool ok = false;
	pthread_mutex_lock(&lock);
	if (!command_ready_locked(error, error_size)) {
		pthread_mutex_unlock(&lock);
		return false;
	}
	abs_download_job_t finished[ABS_DL_JOB_CAP];
	int finished_count = 0;
	for (int i = 0; i < jobs_count && finished_count < ABS_DL_JOB_CAP; i++) {
		if (jobs[i].state == ABS_DL_DONE || jobs[i].state == ABS_DL_FAILED ||
			jobs[i].state == ABS_DL_CANCELLED) {
			finished[finished_count++] = jobs[i];
		}
	}
	db_exec("BEGIN");
	sqlite3_stmt *del = prepare_locked(
		"DELETE FROM DOWNLOAD_FILE WHERE job_id IN"
		" (SELECT id FROM DOWNLOAD_JOB WHERE state IN ('DONE','FAILED','CANCELLED'))");
	if (del) {
		sqlite3_step(del);
		sqlite3_finalize(del);
	}
	del = prepare_locked("DELETE FROM DOWNLOAD_JOB WHERE state IN ('DONE','FAILED','CANCELLED')");
	if (del) {
		sqlite3_step(del);
		sqlite3_finalize(del);
	}
	ok = db_exec("COMMIT");
	if (!ok) {
		db_exec("ROLLBACK");
	} else {
		for (int i = 0; i < finished_count; i++) {
			delete_job_parts_locked(&finished[i]);
		}
		// The queue-wide card pause lifts only when its paused job left.
		if (storage_paused) {
			bool any_card_paused = false;
			for (int i = 0; i < jobs_count; i++) {
				if (jobs[i].state == ABS_DL_PAUSED && strcmp(jobs[i].error_stage, "write") == 0) {
					any_card_paused = true;
					break;
				}
			}
			if (!any_card_paused) {
				storage_paused = false;
			}
		}
		reload_locked();
		cv_signal();
	}
	pthread_mutex_unlock(&lock);
	return ok;
}

// ---------------------------------------------------------------------------
// Snapshot and status (§8.5)
// ---------------------------------------------------------------------------

bool abs_download_snapshot(abs_download_snapshot_t *out) {
	if (!out) {
		return false;
	}
	pthread_mutex_lock(&lock);
	storage_stale_locked(); // a 4 Hz poll is the first to notice a card swap
	bool ok = armed && !stale;
	if (ok) {
		out->generation = snapshot_generation;
		out->job_count = jobs_count;
		out->storage_paused = storage_paused;
		memcpy(out->jobs, jobs, sizeof(jobs[0]) * (size_t)jobs_count);
	} else {
		out->generation = 0;
		out->job_count = 0;
		out->storage_paused = false;
	}
	pthread_mutex_unlock(&lock);
	return ok;
}

bool abs_download_busy(void) {
	pthread_mutex_lock(&lock);
	bool busy = false;
	for (int i = 0; i < jobs_count; i++) {
		if (jobs[i].state == ABS_DL_FETCHING || jobs[i].state == ABS_DL_DOWNLOADING ||
			jobs[i].state == ABS_DL_PAUSING || jobs[i].state == ABS_DL_FINALIZING) {
			busy = true;
			break;
		}
	}
	pthread_mutex_unlock(&lock);
	return busy;
}

bool abs_download_sync_blocked(const char *destination) {
	if (!destination || !destination[0]) {
		return false;
	}
	// Accept card-relative and absolute forms: audiobookdb rows and manifest
	// bindings carry the card-relative path, callers may pass either.
	char relative[SONIX_LINKED_PATH_MAX];
	size_t root = strlen(cfg_sd_root);
	if (root && strncmp(destination, cfg_sd_root, root) == 0 && destination[root] == '/') {
		snprintf(relative, sizeof(relative), "%s", destination + root + 1);
	} else {
		snprintf(relative, sizeof(relative), "%s", destination);
	}

	pthread_mutex_lock(&lock);
	bool blocked = false;
	if (!storage_stale_locked()) {
		for (int i = 0; i < jobs_count; i++) {
			const abs_download_job_t *j = &jobs[i];
			if (!j->destination[0]) {
				continue;
			}
			// §8.2 steps 6–8: the barrier exists from the FINALIZING commit
			// until the DONE+ack_reset_done=1 transaction releases it.
			bool barrier = j->state == ABS_DL_FINALIZING ||
						   (j->state == ABS_DL_DONE && j->ack_reset_done == 0);
			if (!barrier) {
				continue;
			}
			size_t dest_len = strlen(j->destination);
			if (strncmp(relative, j->destination, dest_len) == 0 &&
				(relative[dest_len] == '\0' || relative[dest_len] == '/')) {
				blocked = true;
				break;
			}
		}
	}
	pthread_mutex_unlock(&lock);
	return blocked;
}

void abs_download_network_changed(void) {
	pthread_mutex_lock(&lock);
	if (armed && !storage_stale_locked()) {
		long long now = now_ms();
		// §8.4: automatic retry happens after a connectivity change, and never
		// inside the per-job window since the job entered the wait.
		db_exec("BEGIN");
		bool any = false;
		for (int i = 0; i < jobs_count; i++) {
			abs_download_job_t *j = &jobs[i];
			if (j->state != ABS_DL_WAITING_NETWORK || now - j->updated_ms < ABS_DL_WAIT_BACKOFF_MS) {
				continue;
			}
			j->state = ABS_DL_QUEUED;
			j->error_stage[0] = '\0';
			j->error[0] = '\0';
			if (persist_job_locked(j)) {
				any = true;
			}
		}
		if (any && db_exec("COMMIT")) {
			reload_locked();
		} else if (any) {
			db_exec("ROLLBACK");
		} else {
			db_exec("COMMIT");
		}
		wait_blocked = false;
	}
	cv_signal();
	pthread_mutex_unlock(&lock);
}

// ---------------------------------------------------------------------------
// Scan-completion import coordinator (§8.3 line 556)
//
// The one consumer of the DONE row's own values. Where the 0.5.0 page kept a
// single pending import (the `downloaded_*` handoff and its
// resume_import_cb() timer), the queue rows already carry per-row state, so
// the coordinator is a plain iteration: every DONE+index_pending row runs the
// injected compare-and-swap import with its own values, and only a successful
// one clears its index_pending. No shared "current download" cell exists to
// overwrite, several pending rows import independently, and a failed import
// changes nothing about the row beyond leaving the flag set — DONE is never
// downgraded, the audio stays linked, ack_reset (a FINALIZING-window step,
// §8.2 step 8) is never re-run, and ordinary background sync stays
// authoritative until the next completed scan retries.
//
// Concurrency: runs on the caller's thread (production: the audiobook scan
// thread via the §8.5 line-592 notification). Each op window releases the
// queue lock like every media surface. Single-flight: a second concurrent
// pass finds one already running and reports nothing done. The caller may
// quiesce mid-pass; then the remaining rows are skipped and a successful
// import that can no longer be committed stays index_pending=1 on the closed
// card — the next init's recovery preserves it and the next completed scan
// imports it.
// ---------------------------------------------------------------------------

int abs_download_import_pending(char *error, size_t error_size) {
	if (error_size) {
		error[0] = '\0';
	}
	pthread_mutex_lock(&lock);
	if (import_running) {
		// A pass is already running on another thread: report nothing done.
		pthread_mutex_unlock(&lock);
		return 0;
	}
	if (storage_stale_locked() || !armed) {
		snprintf(error, error_size, "The download queue is not available");
		pthread_mutex_unlock(&lock);
		return -1;
	}
	if (!cfg.ops || !cfg.ops->import_resume) {
		pthread_mutex_unlock(&lock);
		return 0;
	}
	// The candidates are copied once, under the lock: rows can disappear
	// (Remove/Clear finished) or the card can be pulled while the pass runs,
	// and each row is re-consulted durably right before its flag is cleared.
	abs_download_job_t pending[ABS_DL_JOB_CAP];
	int count = 0;
	for (int i = 0; i < jobs_count && count < ABS_DL_JOB_CAP; i++) {
		if (jobs[i].state == ABS_DL_DONE && jobs[i].index_pending) {
			pending[count++] = jobs[i];
		}
	}
	pthread_mutex_unlock(&lock);
	int imported = 0;
	char first_error[ABS_DL_ERROR_MAX] = {0};
	for (int i = 0; i < count; i++) {
		pthread_mutex_lock(&lock);
		if (!armed || stale || stop_requested) {
			pthread_mutex_unlock(&lock);
			break; // the card went away or storage is closing: drop the rest
		}
		import_running = true;
		pthread_mutex_unlock(&lock);

		bool ok = cfg.ops->import_resume(cfg.ops->user, &pending[i], first_error,
										 sizeof(first_error));

		// A worker op window may be open while this runs: the import touches
		// only DONE rows the worker never owns, and audiobookdb serializes
		// its own access behind its db lock.

		pthread_mutex_lock(&lock);
		import_running = false;
		if (ok && armed && !stale && db) {
			// Clear index_pending only after a successful import, durably,
			// against the row's live state (§8.3): it may have been removed
			// under us (Remove/Clear finished), or it may already be cleared.
			abs_download_job_t *live = job_by_id_locked(pending[i].id);
			if (live && live->state == ABS_DL_DONE && live->index_pending) {
				db_exec("BEGIN");
				abs_download_job_t cleared = *live;
				cleared.index_pending = 0;
				if (persist_job_locked(&cleared) && db_exec("COMMIT")) {
					reload_locked();
					imported++;
				} else {
					db_exec("ROLLBACK"); // the durable flag stays 1; retry next scan
				}
			}
		}
		// A quiesce that raced the pass (armed=false here or on the next row)
		// leaves the committed flag of the closed card untouched — recovery
		// preserves the row and the next completed scan after re-init imports.
		pthread_mutex_unlock(&lock);
		if (!ok && !first_error[0]) {
			// Defensive: a misbehaving op is treated like any other failure.
			snprintf(first_error, sizeof(first_error), "The resume import failed");
		}
	}
	if (first_error[0] && error_size > strlen(first_error)) {
		snprintf(error, error_size, "%s", first_error);
	}
	return imported;
}

// ---------------------------------------------------------------------------
// Worker engine (§8.1: one persistent worker, one active job, one active file)
// ---------------------------------------------------------------------------

void abs_download_op_progress(const abs_download_op_ctx_t *ctx, long long done, long long total) {
	if (!ctx || ctx->job_id <= 0) {
		return;
	}
	pthread_mutex_lock(&lock);
	abs_download_job_t *j = job_by_id_locked(ctx->job_id);
	// RAM-backed counters (§8.2): written to the mirror only, persisted at the
	// next transition. A stale context (different job or phase) is ignored.
	if (j && ctx->job_id == active_id && ctx->state == j->state) {
		j->bytes_done = done;
		j->bytes_total = total;
	}
	pthread_mutex_unlock(&lock);
}

static void op_ctx_fill(abs_download_op_ctx_t *ctx, abs_download_state_t state) {
	ctx->job_id = active_id;
	ctx->state = state;
	ctx->cancel = &op_cancel;
	ctx->op = &queue_http_op;
	ctx->user = cfg.ops ? cfg.ops->user : NULL;
}

static void auth_drop(void) {
	if (active_auth_held && cfg.ops && cfg.ops->auth_release) {
		cfg.ops->auth_release(cfg.ops->user, &active_auth);
		active_auth_held = false;
	}
	memset(&active_auth, 0, sizeof(active_auth));
}

// A terminal state or a stop resets the per-attempt context: the auth snapshot
// is destroyed (it never outlives one attempt, §8.1), the cancel flag is
// armed for the next attempt, and the wake slot is drained -- a stop aimed at
// the finished attempt must not wake the next job's network calls.
static void end_attempt_locked(void) {
	auth_drop();
	active_auth_done = false;
	active_stop = STOP_NONE;
	op_cancel_store(0);
	http_op_clear(&queue_http_op);
	snapshot_generation++;
	reload_locked();
}

// The active row + its file rows, copied for the unlocked ops window.
static bool active_take_locked(void) {
	abs_download_job_t *j = job_by_id_locked(active_id);
	if (!j) {
		return false;
	}
	active_job = *j;
	active_file_count = 0;
	sqlite3_stmt *stmt = prepare_locked(
		"SELECT job_id, ordinal, source_url, final_name, start_offset, duration,"
		" expected_size, final_size, state FROM DOWNLOAD_FILE WHERE job_id=? ORDER BY ordinal");
	if (stmt) {
		sqlite3_bind_int64(stmt, 1, (sqlite3_int64)active_id);
		while (active_file_count < ABS_DL_FILE_CAP && sqlite3_step(stmt) == SQLITE_ROW) {
			abs_download_file_t *f = &active_files[active_file_count++];
			f->job_id = sqlite3_column_int64(stmt, 0);
			f->ordinal = sqlite3_column_int(stmt, 1);
			snprintf(f->source_url, sizeof(f->source_url), "%s", col_text(stmt, 2));
			snprintf(f->final_name, sizeof(f->final_name), "%s", col_text(stmt, 3));
			f->start_offset = sqlite3_column_double(stmt, 4);
			f->duration = sqlite3_column_double(stmt, 5);
			f->expected_size = sqlite3_column_int64(stmt, 6);
			f->final_size = sqlite3_column_int64(stmt, 7);
			snprintf(f->state, sizeof(f->state), "%s", col_text(stmt, 8));
		}
		sqlite3_finalize(stmt);
	}
	return true;
}

static long long first_queued_locked(void) {
	sqlite3_stmt *stmt = prepare_locked("SELECT id FROM DOWNLOAD_JOB WHERE state='QUEUED' ORDER BY seq LIMIT 1");
	long long id = 0;
	if (stmt) {
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			id = sqlite3_column_int64(stmt, 0);
		}
		sqlite3_finalize(stmt);
	}
	return id;
}

// §8.1 pause acknowledgement: the stop intent becomes the durable terminal
// state. The in-flight file goes back to `pending` (restart from zero), the
// current .part is deleted for cancel / retained for pause (§8.1/§8.2).
static bool ack_pause_locked(void) {
	if (!active_take_locked()) {
		return false;
	}
	int intent = active_stop;
	bool cancelled = intent == STOP_CANCEL;

	db_exec("BEGIN");
	reset_writing_file_locked(active_job.id);
	active_job.state = cancelled ? ABS_DL_CANCELLED : ABS_DL_PAUSED;
	if (!persist_job_locked(&active_job)) {
		db_exec("ROLLBACK");
		return false;
	}
	if (!db_exec("COMMIT")) {
		db_exec("ROLLBACK");
		return false;
	}
	if (cancelled) {
		delete_job_parts_locked(&active_job);
	}
	end_attempt_locked();
	return true;
}

// QUEUED -> FETCHING: the FIFO head takes its turn. Auth resolution and the
// identity fetch are the next two pump steps, both under the same state.
static bool begin_next_locked(void) {
	long long id = first_queued_locked();
	if (!id || !job_by_id_locked(id)) {
		return false;
	}
	abs_download_job_t j = *job_by_id_locked(id);
	j.state = ABS_DL_FETCHING;
	j.error_stage[0] = '\0';
	j.error[0] = '\0';
	j.bytes_done = 0;
	j.bytes_total = -1;
	j.file_index = 0;
	if (!apply_job_locked(&j)) {
		return false;
	}
	active_auth_done = false;
	active_stop = STOP_NONE;
	op_cancel_store(0);
	// Fresh attempt, fresh slot: nothing from the previous attempt may wake
	// this one's network calls.
	http_op_clear(&queue_http_op);
	cv_signal();
	return true;
}

// §8.1: pending execution resolves the latest token for the profile and
// verifies that its URL still matches the job's server. The snapshot is held
// in RAM for this attempt only.
static bool step_auth_locked(void) {
	if (!active_take_locked()) {
		return false;
	}
	abs_auth_snapshot_t snapshot;
	memset(&snapshot, 0, sizeof(snapshot));
	abs_download_op_ctx_t ctx;
	op_ctx_fill(&ctx, ABS_DL_FETCHING);
	op_running = true;
	pthread_mutex_unlock(&lock);
	bool ok = cfg.ops && cfg.ops->auth_acquire &&
			  cfg.ops->auth_acquire(cfg.ops->user, active_job.profile_id, active_job.server, &snapshot);
	pthread_mutex_lock(&lock);
	op_running = false;
	if (stop_requested) {
		return true; // quiesce owns the cleanup after the join
	}
	if (active_state != ABS_DL_FETCHING || active_id != active_job.id) {
		return true; // a pause/cancel raced in; its ack step follows
	}
	if (!ok) {
		abs_download_job_t j = active_job;
		j.state = ABS_DL_FAILED;
		record_error(&j, "auth", "Could not resolve a login token for the server");
		end_attempt_locked();
		return apply_job_locked(&j);
	}
	active_auth = snapshot;
	active_auth_held = true;
	active_auth_done = true;
	return true;
}

// Ingest a fetch plan: validate everything, merge file records (completed
// records with the same basename+URL are reused on retry) and record
// DOWNLOADING — or FINALIZING when every file is already committed. The file
// rows and the job row move together in one transaction (§8.2 step 4).
// `active_files` (taken before the op ran) supplies the previously validated
// records, so the ops window cannot lose a completed file on a retry.
static bool plan_ingest_locked(abs_download_job_t *j, const abs_download_plan_t *plan) {
	// Destination: an explicit plan destination wins; otherwise keep the job's;
	// a job with neither cannot write a byte.
	const char *destination = j->destination;
	if (plan->destination && plan->destination[0]) {
		destination = plan->destination;
	}
	if (!destination_ok(destination)) {
		record_error(j, "metadata", "Download destination is missing or not representable on the card");
		j->state = ABS_DL_FAILED;
		return apply_job_locked(j);
	}
	snprintf(j->destination, sizeof(j->destination), "%s", destination);

	int count = plan->files ? plan->file_count : 0;
	if (count > ABS_DL_FILE_CAP) {
		// §8.2 step 2: more than 256 tracks is an explicit unsupported error,
		// never silent truncation.
		record_error(j, "metadata", "This item has more than 256 files; the queue does not support it yet");
		j->state = ABS_DL_FAILED;
		return apply_job_locked(j);
	}
	if (count <= 0) {
		record_error(j, "metadata", "The item has no downloadable files");
		j->state = ABS_DL_FAILED;
		return apply_job_locked(j);
	}
	for (int i = 0; i < count; i++) {
		const abs_download_file_req_t *req = &plan->files[i];
		if (!source_url_ok(req->source_url, j->server) || !final_name_ok(req->final_name)) {
			// §8.4: rows store same-origin, credential-free URLs only; the
			// offending URL itself is never copied into the error.
			record_error(j, "metadata", "A file of this item is not on the configured server");
			j->state = ABS_DL_FAILED;
			return apply_job_locked(j);
		}
	}

	db_exec("BEGIN");
	sqlite3_stmt *wipe = prepare_locked("DELETE FROM DOWNLOAD_FILE WHERE job_id=?");
	if (wipe) {
		sqlite3_bind_int64(wipe, 1, (sqlite3_int64)j->id);
		sqlite3_step(wipe);
		sqlite3_finalize(wipe);
	}
	int first_incomplete = -1;
	for (int i = 0; i < count; i++) {
		const abs_download_file_req_t *req = &plan->files[i];
		const abs_download_file_t *reuse = NULL;
		for (int k = 0; k < active_file_count; k++) {
			if (strcmp(active_files[k].final_name, req->final_name) == 0 &&
				strcmp(active_files[k].source_url, req->source_url) == 0 &&
				strcmp(active_files[k].state, FILE_COMPLETE) == 0) {
				reuse = &active_files[k];
				break;
			}
		}
		if (!reuse && first_incomplete < 0) {
			first_incomplete = i;
		}
		sqlite3_stmt *ins = prepare_locked(
			"INSERT INTO DOWNLOAD_FILE(job_id, ordinal, source_url, final_name, start_offset,"
			" duration, expected_size, final_size, state) VALUES(?,?,?,?,?,?,?,?,?)");
		if (!ins) {
			db_exec("ROLLBACK");
			record_error(j, "write", "Cannot write the file records");
			j->state = ABS_DL_FAILED;
			return apply_job_locked(j);
		}
		sqlite3_bind_int64(ins, 1, (sqlite3_int64)j->id);
		sqlite3_bind_int(ins, 2, i + 1);
		bind_text_or_empty(ins, 3, req->source_url);
		bind_text_or_empty(ins, 4, req->final_name);
		sqlite3_bind_double(ins, 5, req->start_offset);
		sqlite3_bind_double(ins, 6, req->duration);
		sqlite3_bind_int64(ins, 7, (sqlite3_int64)req->expected_size);
		if (reuse) {
			// §8.1: retry does not redownload a validated, committed file.
			sqlite3_bind_int64(ins, 8, (sqlite3_int64)reuse->final_size);
			bind_text_or_empty(ins, 9, FILE_COMPLETE);
		} else {
			sqlite3_bind_int64(ins, 8, -1);
			bind_text_or_empty(ins, 9, FILE_PENDING);
		}
		if (sqlite3_step(ins) != SQLITE_DONE) {
			fprintf(stderr, "abs-download: cannot insert file record: %s\n", sqlite3_errmsg(db));
		}
		sqlite3_finalize(ins);
	}

	j->file_count = count;
	j->bytes_done = 0;
	j->bytes_total = -1;
	j->remote_progress = plan->remote_progress >= 0 ? plan->remote_progress : j->remote_progress;
	j->remote_finished = plan->remote_finished;
	j->remote_updated_ms = plan->remote_updated_ms;
	if (first_incomplete < 0) {
		// A retry of a fully-downloaded job must not re-download a byte
		// (§8.1): go straight to the finalize window, which is also where
		// recovery leaves a crashed finalize.
		j->file_index = count;
		j->state = ABS_DL_FINALIZING;
	} else {
		j->file_index = first_incomplete;
		j->state = ABS_DL_DOWNLOADING;
	}
	if (!persist_job_locked(j)) {
		db_exec("ROLLBACK");
		return false;
	}
	if (!db_exec("COMMIT")) {
		db_exec("ROLLBACK");
		return false;
	}
	reload_locked();
	return true;
}

// FETCHING, second phase: resolve the expanded identity and the file plan.
// The plan (and its strings) must stay valid until the next ops callback for
// this job — the real implementation will keep the expanded book object alive
// for the active job (§8.2 step 2), the fakes use static storage.
static bool step_fetch_locked(void) {
	if (!active_take_locked()) {
		return false;
	}
	abs_download_op_ctx_t ctx;
	op_ctx_fill(&ctx, ABS_DL_FETCHING);
	abs_download_plan_t plan;
	memset(&plan, 0, sizeof(plan));
	char error[256] = {0};
	const char *stage = "metadata";
	op_running = true;
	pthread_mutex_unlock(&lock);
	bool ok = cfg.ops && cfg.ops->fetch_identity &&
			  cfg.ops->fetch_identity(cfg.ops->user, &ctx, &active_job, &plan, &stage, error, sizeof(error));
	pthread_mutex_lock(&lock);
	op_running = false;
	if (stop_requested) {
		return true;
	}
	if (active_state != ABS_DL_FETCHING || active_id != active_job.id) {
		return true; // paused/cancelled mid-fetch; nothing was written yet
	}
	if (!ok) {
		abs_download_job_t j = active_job;
		bool stall = stage_known(stage) && strcmp(stage, "network") == 0;
		if (stall) {
			j.state = ABS_DL_WAITING_NETWORK;
			wait_blocked = true;
		} else {
			j.state = ABS_DL_FAILED;
		}
		record_error(&j, stage, error);
		end_attempt_locked();
		return apply_job_locked(&j);
	}
	abs_download_job_t j = active_job;
	bool ingested = plan_ingest_locked(&j, &plan);
	return ingested;
}

// DOWNLOADING: the current file's transfer. The row goes to `writing` first
// (durable), then the op streams to <final>.part; the record is committed
// complete only after the op's atomic rename (§8.2 step 4).
static bool step_file_locked(void) {
	if (!active_take_locked()) {
		return false;
	}
	abs_download_file_t *current = NULL;
	for (int i = 0; i < active_file_count; i++) {
		if (strcmp(active_files[i].state, FILE_COMPLETE) != 0) {
			current = &active_files[i];
			break;
		}
	}
	if (!current) {
		// Every file committed: enter the finalize window (§8.2 steps 6–8).
		abs_download_job_t j = active_job;
		j.state = ABS_DL_FINALIZING;
		j.ack_reset_done = 0; // the sync barrier opens with this commit
		j.bytes_done = 0;
		j.bytes_total = -1;
		return apply_job_locked(&j);
	}

	// Durably mark the attempt before the first byte moves.
	db_exec("BEGIN");
	{
		sqlite3_stmt *stmt = prepare_locked(
			"UPDATE DOWNLOAD_FILE SET state='writing' WHERE job_id=? AND ordinal=?");
		if (stmt) {
			sqlite3_bind_int64(stmt, 1, (sqlite3_int64)active_job.id);
			sqlite3_bind_int(stmt, 2, current->ordinal);
			sqlite3_step(stmt);
			sqlite3_finalize(stmt);
		}
	}
	abs_download_job_t j = active_job;
	j.file_index = current->ordinal - 1;
	j.bytes_done = 0;
	j.bytes_total = current->expected_size >= 0 ? current->expected_size : -1;
	if (!persist_job_locked(&j) || !db_exec("COMMIT")) {
		db_exec("ROLLBACK");
		return false;
	}
	reload_locked();
	if (!active_take_locked()) {
		return false;
	}

	abs_download_op_ctx_t ctx;
	op_ctx_fill(&ctx, ABS_DL_DOWNLOADING);
	long long final_size = -1;
	char error[256] = {0};
	const char *stage = "write";
	int ordinal = current->ordinal;
	char final_name[ABS_DL_NAME_MAX];
	snprintf(final_name, sizeof(final_name), "%s", current->final_name);
	op_running = true;
	pthread_mutex_unlock(&lock);
	bool ok = cfg.ops && cfg.ops->file_download &&
			  cfg.ops->file_download(cfg.ops->user, &ctx, current, &final_size, &stage, error, sizeof(error));
	pthread_mutex_lock(&lock);
	op_running = false;
	if (stop_requested) {
		return true;
	}

	// Re-derive the row freshly: commands and progress may have touched it.
	bool paused_or_cancelled = active_state != ABS_DL_DOWNLOADING || active_id != active_job.id;
	if (ok) {
		// Defensive commit gate: the rename must show a real file of exactly
		// the promised size (checked Content-Length, §8.2 step 4 / §8.4).
		char final_path[SONIX_LINKED_SIDECAR_PATH_MAX];
		long long on_disk = -1;
		bool size_ok = job_file_path(&active_job, final_name, "", final_path, sizeof(final_path)) &&
					   file_size_ll(final_path, &on_disk) && (final_size < 0 || on_disk == final_size);
		if (current->expected_size >= 0 && on_disk != current->expected_size) {
			size_ok = false;
		}
		db_exec("BEGIN");
		{
			sqlite3_stmt *stmt = prepare_locked(
				"UPDATE DOWNLOAD_FILE SET state=?, final_size=? WHERE job_id=? AND ordinal=?");
			if (stmt) {
				bind_text_or_empty(stmt, 1, size_ok ? FILE_COMPLETE : FILE_PENDING);
				sqlite3_bind_int64(stmt, 2, (sqlite3_int64)(size_ok ? on_disk : -1));
				sqlite3_bind_int64(stmt, 3, (sqlite3_int64)active_job.id);
				sqlite3_bind_int(stmt, 4, ordinal);
				sqlite3_step(stmt);
				sqlite3_finalize(stmt);
			}
		}
		if (!size_ok) {
			// The transfer lied about the commit; quarantine for user retry.
			abs_download_job_t jj = active_job;
			jj.state = ABS_DL_FAILED;
			jj.bytes_done = on_disk > 0 ? on_disk : 0;
			jj.bytes_total = current->expected_size;
			record_error(&jj, "write", "The downloaded file has the wrong size; it will be redownloaded");
			if (persist_job_locked(&jj) && db_exec("COMMIT")) {
				end_attempt_locked();
				return true;
			}
			db_exec("ROLLBACK");
			return false;
		}
		// File record committed (§8.2 step 4: only after the rename).
		if (!db_exec("COMMIT")) {
			db_exec("ROLLBACK");
			return false;
		}
		reload_locked();
		if (paused_or_cancelled) {
			return true; // the PAUSING ack step finishes the stop; file retained
		}
		// Advance to the next incomplete file, or into finalize when done.
		abs_download_job_t advance = active_job;
		if (!active_take_locked()) {
			return false;
		}
		int next = -1;
		for (int i = 0; i < active_file_count; i++) {
			if (strcmp(active_files[i].state, FILE_COMPLETE) != 0) {
				next = active_files[i].ordinal - 1;
				break;
			}
		}
		if (next < 0) {
			advance.state = ABS_DL_FINALIZING;
			advance.ack_reset_done = 0;
			advance.file_count = active_file_count;
			advance.file_index = active_file_count;
			advance.bytes_done = 0;
			advance.bytes_total = -1;
		} else {
			advance.file_index = next;
			advance.bytes_done = 0;
			advance.bytes_total = -1;
		}
		return apply_job_locked(&advance);
	}

	// Failure paths. The in-flight row returns to `pending` in every one.
	// The counters come from the mirror, which the progress callback kept
	// current during the unlocked op window.
	db_exec("BEGIN");
	reset_writing_file_locked(active_job.id);
	abs_download_job_t fail = active_job;
	if (job_by_id_locked(active_job.id)) {
		fail = *job_by_id_locked(active_job.id);
	}
	fail.bytes_done = fail.bytes_done > 0 ? fail.bytes_done : 0;
	bool stall = stage_known(stage) && strcmp(stage, "network") == 0;
	bool card_full = stage_known(stage) && strcmp(stage, "write") == 0;
	if (paused_or_cancelled) {
		// The op returned because of the stop request, not an error: the stop
		// ack step records the terminal state; keep the counters it may want.
		if (persist_job_locked(&fail)) {
			db_exec("COMMIT");
			reload_locked();
		} else {
			db_exec("ROLLBACK");
		}
		return true;
	}
	if (stall) {
		fail.state = ABS_DL_WAITING_NETWORK;
		record_error(&fail, stage, error);
		if (persist_job_locked(&fail) && db_exec("COMMIT")) {
			wait_blocked = true;
			end_attempt_locked();
			return true;
		}
	} else if (card_full) {
		// §8.4: a card write failure pauses the queue as a whole, retains
		// completed files, and needs user action after space recovery.
		fail.state = ABS_DL_PAUSED;
		record_error(&fail, stage, error);
		if (persist_job_locked(&fail) && db_exec("COMMIT")) {
			storage_paused = true;
			end_attempt_locked();
			return true;
		}
	} else {
		fail.state = ABS_DL_FAILED;
		record_error(&fail, stage, error);
		if (persist_job_locked(&fail) && db_exec("COMMIT")) {
			end_attempt_locked();
			return true;
		}
	}
	db_exec("ROLLBACK");
	return false;
}

// §8.2 steps 6–8: FINALIZING — sidecars/manifest and the resume mapping, then
// the acknowledgement reset, then the DONE transaction that releases the sync
// barrier. The barrier is visible to abs_download_sync_blocked the whole time.
static bool step_finalize_locked(void) {
	if (!active_take_locked()) {
		return false;
	}
	abs_download_op_ctx_t ctx;
	op_ctx_fill(&ctx, ABS_DL_FINALIZING);
	abs_download_resume_t resume;
	memset(&resume, 0, sizeof(resume));
	resume.resume_seconds = -1;
	char error[256] = {0};
	const char *stage = "manifest";
	op_running = true;
	pthread_mutex_unlock(&lock);
	bool ok = cfg.ops && cfg.ops->finalize &&
			  cfg.ops->finalize(cfg.ops->user, &ctx, &active_job, active_files, active_file_count, &resume,
								&stage, error, sizeof(error));
	pthread_mutex_lock(&lock);
	op_running = false;
	if (stop_requested) {
		return true;
	}
	if (ok) {
		// Persist the resume mapping while the barrier is still held (§8.2
		// step 5: before completion).
		db_exec("BEGIN");
		abs_download_job_t j = active_job;
		snprintf(j.resume_file, sizeof(j.resume_file), "%.255s", resume.resume_file);
		j.resume_seconds = resume.resume_seconds;
		if (!persist_job_locked(&j)) {
			db_exec("ROLLBACK");
			return false;
		}
		if (!db_exec("COMMIT")) {
			db_exec("ROLLBACK");
			return false;
		}
		reload_locked();
		if (!active_take_locked()) {
			return false;
		}
	}

	bool stopped_mid_finalize = active_state != ABS_DL_FINALIZING || active_id != active_job.id;
	if (stopped_mid_finalize && !ok) {
		// Pause/cancel deferred to the end of finalize: apply the intent now;
		// a failed finalize resolves to the requested stop, not an error.
		return ack_pause_locked();
	}

	if (!ok) {
		abs_download_job_t j = active_job;
		// §8.2 step 7: manifest failure means the job is not DONE; leaving the
		// FINALIZING state releases the sync barrier into a recoverable state.
		if (stage_known(stage) && strcmp(stage, "network") == 0) {
			j.state = ABS_DL_WAITING_NETWORK;
			wait_blocked = true;
		} else {
			j.state = ABS_DL_FAILED;
		}
		record_error(&j, stage, error);
		end_attempt_locked();
		return apply_job_locked(&j);
	}

	// The acknowledgement reset (audiobookdb_abs_reset() for books, the
	// episode equivalent for episodes). It runs while the barrier is held, so
	// a progress reconciliation can never observe the new manifest with an old
	// acknowledgement (§8.2 step 8).
	{
		char ack_error[256] = {0};
		op_running = true;
		pthread_mutex_unlock(&lock);
		bool ack_ok = cfg.ops && cfg.ops->ack_reset &&
					  cfg.ops->ack_reset(cfg.ops->user, &active_job, ack_error, sizeof(ack_error));
		pthread_mutex_lock(&lock);
		op_running = false;
		if (stop_requested) {
			return true;
		}
		if (!ack_ok) {
			abs_download_job_t j = active_job;
			j.state = ABS_DL_FAILED;
			record_error(&j, "index", ack_error[0] ? ack_error : "Could not reset the progress acknowledgement");
			end_attempt_locked();
			return apply_job_locked(&j);
		}
		// §8.2 step 8: DONE + ack_reset_done=1 + index_pending=1 in exactly one
		// transaction; releasing it releases the barrier.
		db_exec("BEGIN");
		abs_download_job_t done = active_job;
		done.state = ABS_DL_DONE;
		done.ack_reset_done = 1;
		done.index_pending = 1;
		done.error_stage[0] = '\0';
		done.error[0] = '\0';
		if (!persist_job_locked(&done)) {
			db_exec("ROLLBACK");
			return false;
		}
		if (!db_exec("COMMIT")) {
			db_exec("ROLLBACK");
			return false;
		}
		end_attempt_locked();
		cv_signal();
		return true;
	}
}

// One non-blocking worker step. True when it did something and more may be
// pending; the worker thread calls it until it returns false, then sleeps.
bool abs_download_pump(void) {
	pthread_mutex_lock(&lock);
	if (!armed || storage_stale_locked() || !cfg.ops || op_running || stop_requested) {
		pthread_mutex_unlock(&lock);
		return false;
	}
	if (active_state == ABS_DL_PAUSING) {
		bool did = ack_pause_locked();
		pthread_mutex_unlock(&lock);
		return did;
	}
	if (!active_id) {
		if (storage_paused || wait_blocked) {
			pthread_mutex_unlock(&lock);
			return false;
		}
		bool began = begin_next_locked();
		pthread_mutex_unlock(&lock);
		return began;
	}
	bool did = false;
	switch (active_state) {
	case ABS_DL_FETCHING:
		did = active_auth_done ? step_fetch_locked() : step_auth_locked();
		break;
	case ABS_DL_DOWNLOADING:
		did = step_file_locked();
		break;
	case ABS_DL_FINALIZING:
		did = step_finalize_locked();
		break;
	default:
		// Active-but-unknown state: something touched the database directly;
		// refresh and let the next call re-derive.
		reload_locked();
		did = true;
		break;
	}
	pthread_mutex_unlock(&lock);
	return did;
}

static bool pump_has_work_locked(void) {
	if (!armed || stale || stop_requested) {
		return false;
	}
	if (active_id) {
		return true; // PAUSING ack / FETCHING / DOWNLOADING / FINALIZING steps
	}
	return !storage_paused && !wait_blocked && first_queued_locked() != 0;
}

static void *worker_main(void *unused) {
	(void)unused;
	// §8.5/power: background work on the 1-core device; see utils.h.
	thread_be_background("abs download");
	for (;;) {
		pthread_mutex_lock(&lock);
		while (!stop_requested && !pump_has_work_locked()) {
			pthread_cond_wait(&work_cv, &lock);
		}
		bool stop = stop_requested;
		pthread_mutex_unlock(&lock);
		if (stop) {
			break;
		}
		abs_download_pump();
	}
	return NULL;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool abs_download_init(const abs_download_config_t *config) {
	pthread_mutex_lock(&lock);
	if (armed || worker_alive) {
		fprintf(stderr, "abs-download: init called without quiesce_storage (wiring bug)\n");
		pthread_mutex_unlock(&lock);
		return false;
	}
	if (!config || !config->sd_root || !config->sd_root[0]) {
		fprintf(stderr, "abs-download: invalid config\n");
		pthread_mutex_unlock(&lock);
		return false;
	}
	int n = snprintf(cfg_sd_root, sizeof(cfg_sd_root), "%s", config->sd_root);
	if (n < 0 || n >= (int)sizeof(cfg_sd_root) || (size_t)n >= sizeof(db_path) - 40) {
		// db_path carries the root plus "/.local/audiobookshelf-downloads.db";
		// a root that cannot hold the whole path cannot host the queue.
		fprintf(stderr, "abs-download: sd root path is too long\n");
		pthread_mutex_unlock(&lock);
		return false;
	}
	cfg = *config;
	cfg.sd_root = cfg_sd_root;
	cfg.ops = config->ops;
	snprintf(db_path, sizeof(db_path), "%.480s/.local/audiobookshelf-downloads.db", cfg_sd_root);
	char dir[520];
	snprintf(dir, sizeof(dir), "%.480s/.local", cfg_sd_root);
	if (mkdir(dir, 0777) != 0 && errno != EEXIST) {
		fprintf(stderr, "abs-download: cannot create %s: %s\n", dir, strerror(errno));
		pthread_mutex_unlock(&lock);
		return false;
	}

	stop_requested = false;
	storage_paused = false;
	wait_blocked = false;
	import_running = false;
	op_cancel_store(0);
	op_running = false;
	// §8.5 line 1028: the wake slot is module-lifetime state; re-init after a
	// quiesce re-arms it (the destroy ran in the quiesce). init re-entry
	// without a quiesce is refused above.
	http_op_init(&queue_http_op);
	active_auth_done = false;
	active_stop = STOP_NONE;
	auth_drop();
	jobs_count = 0;
	active_id = 0;
	active_state = ABS_DL_QUEUED;

	if (!open_db_locked() || !ensure_schema_locked()) {
		if (db) {
			sqlite3_close(db);
			db = NULL;
		}
		pthread_mutex_unlock(&lock);
		return false;
	}
	if (!recover_locked()) {
		fprintf(stderr, "abs-download: recovery failed on %s\n", db_path);
		sqlite3_close(db);
		db = NULL;
		pthread_mutex_unlock(&lock);
		return false;
	}
	// The database identity is read after the recovery commit: it is what all
	// later commands compare against (the card-generation guard).
	file_identity_read(db_path, &db_identity);
	armed = true;
	card_generation++;
	reload_locked();
	if (cfg.ops && cfg.enable_worker_thread &&
		pthread_create(&worker_thread, NULL, worker_main, NULL) == 0) {
		worker_alive = true;
	}
	pthread_mutex_unlock(&lock);
	return true;
}

void abs_download_quiesce_storage(void) {
	// One quiesce at a time, for §8.5's three lifecycle callers (see the
	// quiesce_lock note). A second call that arrives while the first is inside
	// the join waits here, then finds the queue already torn down and returns.
	// The body has no early exit between here and the unlock at the end.
	pthread_mutex_lock(&quiesce_lock);
	pthread_mutex_lock(&lock);
	// Whether the wake slot was ever created: only an armed (or worker-running)
	// queue owns an initialized slot. The lifecycle now permits quiesce before
	// any init (a card-detach event racing the boot init: system_start_services
	// starts the card supervisor before main.c reaches the DB-open block) and
	// a second quiesce after an earlier one; clearing or destroying a zeroed
	// -- worse, destroyed -- mutex is undefined. The guard keeps those shapes
	// honest no-ops.
	bool was_armed = armed || worker_alive || db != NULL;
	// Order per §8.5 (system.c/usb.c): stop the worker first, publish the stop
	// to the in-flight op, then wait for its acknowledgement before touching
	// the database. Well-behaved ops must observe the cancel flag between
	// units; the WS5 bounded-wait work bounds pathological waits.
	if (worker_alive) {
		stop_requested = true;
		op_cancel_store(1);
		// Wake the in-flight op out of its network wait so the bounded join
		// below really is bounded: storage must close promptly (§8.5).
		http_op_cancel(&queue_http_op);
		cv_signal();
		pthread_t thread = worker_thread;
		pthread_mutex_unlock(&lock);
		pthread_join(thread, NULL);
		pthread_mutex_lock(&lock);
		worker_alive = false;
		op_running = false;
		// The join is complete: nothing may publish into or wake the slot
		// until the next init.
		http_op_clear(&queue_http_op);
	}

	if (active_id && db) {
		// Persist the active job restart-clean while the card is still here:
		// the next boot maps these states the same way the §8.2 recovery
		// would, so a closed card and a pulled card converge.
		abs_download_job_t j;
		if (active_take_locked()) {
			j = active_job;
			bool cancelled = active_stop == STOP_CANCEL;
			bool paused = active_stop == STOP_PAUSE;
			db_exec("BEGIN");
			reset_writing_file_locked(j.id);
			j.state = cancelled ? ABS_DL_CANCELLED : (paused ? ABS_DL_PAUSED : ABS_DL_QUEUED);
			bool ok = false;
			if (persist_job_locked(&j) && db_exec("COMMIT")) {
				ok = true;
			} else {
				db_exec("ROLLBACK");
			}
			if (ok && cancelled) {
				delete_job_parts_locked(&j);
			}
		}
	}
	auth_drop();
	jobs_count = 0;
	active_id = 0;
	active_state = ABS_DL_QUEUED;
	active_auth_done = false;
	active_stop = STOP_NONE;
	op_cancel_store(0);
	if (was_armed) {
		// The slot belonged to the queue just torn down. When quiesce runs
		// without the queue ever having been armed (or a second time after
		// that), destroying the zeroed static slot -- twice -- is undefined.
		http_op_clear(&queue_http_op);
		http_op_destroy(&queue_http_op);
	}
	if (db) {
		sqlite3_close(db);
		db = NULL;
	}
	armed = false;
	stale = false;
	storage_paused = false;
	wait_blocked = false;
	cv_signal();
	pthread_mutex_unlock(&lock);
	// Released only now, so a waiting quiesce cannot start its teardown while
	// this one's sqlite close is still running behind it.
	pthread_mutex_unlock(&quiesce_lock);
}

unsigned abs_download_card_generation(void) {
	pthread_mutex_lock(&lock);
	unsigned gen = card_generation;
	pthread_mutex_unlock(&lock);
	return gen;
}

bool abs_download_probe_ops_unlocked(void) {
	// Try, then release. While an ops callback runs the pump has already
	// released `lock` (§12.1: no queue mutex across HTTP), so the try must
	// succeed from the op's own thread; if the mutex were still held -- the
	// bug this probe exists to catch -- the same thread holds it, the try
	// reports EDEADLK/EAGAIN, and this returns false deterministically.
	if (pthread_mutex_trylock(&lock) != 0) {
		return false;
	}
	pthread_mutex_unlock(&lock);
	return true;
}
