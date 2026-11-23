#ifndef ABS_DOWNLOAD_H
#define ABS_DOWNLOAD_H

#include <stdbool.h>
#include <stddef.h>

#include "abs_profiles.h" // abs_auth_snapshot_t only (header-only use)
#include "src/system/core/pathlimits.h" // SONIX_LINKED_DB_PATH_MAX
#include "src/system/net/http.h"        // http_op_t wake slot (§8.5 line 1028)

// ---------------------------------------------------------------------------
// Workstream 2 — Audiobookshelf download queue: storage foundation (spec §8).
//
// One process-global queue on the card: <sd>/.local/audiobookshelf-downloads.db,
// schema version 1 (§8.2). The database is the durable intent; the module's RAM
// mirror exists for snapshots. Tokens never reach the database: rows carry the
// profile id and the normalized server only, and the auth snapshot lives in the
// worker for one job at a time.
//
// One worker, one active job, one active file, no per-job threads (§8.1). The
// media surface (identity fetch, per-file transfer, finalize, acknowledgement
// reset) is injected through abs_download_ops_t so the state machine, schema,
// recovery and barriers are testable without HTTP (fake transfer callbacks in
// tools/audiobookshelf-queue-test.c). The real implementations are split out of
// audiobookshelf_download() in a later wave — see .specs/forge-m3-store-report.md
// for the exact missing entry points.
//
// Concurrency: every public call locks the queue mutex. The ops callbacks run
// WITHOUT the lock held (they are slow, network-bound work) and poll
// ctx->cancel between units; pause/cancel set that flag. Snapshot is the
// coalesced view for a >=4 Hz GUI poll (§8.1); state transitions are
// generation-tagged.
// ---------------------------------------------------------------------------

#define ABS_DL_KIND_BOOK "book"
#define ABS_DL_KIND_EPISODE "podcastEpisode"

// 16 stored jobs including terminal history (§8.1); "Clear finished" and
// explicit Remove free capacity. An enqueue that would exceed the bound fails
// before any network activity.
#define ABS_DL_JOB_CAP 16
// More than 256 tracks is an explicit unsupported error (§8.2 step 2).
#define ABS_DL_FILE_CAP 256

#define ABS_DL_KIND_MAX 16       // "podcastEpisode" + terminator
#define ABS_DL_PROFILE_ID_MAX ABS_PROFILE_ID_MAX
#define ABS_DL_SERVER_MAX ABS_SERVER_MAX
#define ABS_DL_ID_MAX 128        // item/episode/library ids
#define ABS_DL_TITLE_MAX 256     // display only
#define ABS_DL_DEST_MAX SONIX_LINKED_DB_PATH_MAX // card-relative directory identity
#define ABS_DL_ERROR_MAX 256     // bounded, sanitized (§8.4)
#define ABS_DL_ERROR_STAGE_MAX 16 // auth|metadata|network|write|sidecar|manifest|index
#define ABS_DL_URL_MAX 1024      // same-origin source URL; no token/query credential
#define ABS_DL_NAME_MAX 256      // final_name basename

// Job states (§8.1. State machine). The strings below are what the database
// stores; abs_download_state_name() returns them.
typedef enum {
	ABS_DL_QUEUED = 0,
	ABS_DL_FETCHING,
	ABS_DL_DOWNLOADING,
	ABS_DL_PAUSING,   // stop requested; media ack pending (worker-owned)
	ABS_DL_PAUSED,    // user pause; no automatic attempt
	ABS_DL_WAITING_NETWORK, // Wi-Fi/server unavailable; retry on change
	ABS_DL_FINALIZING,      // sidecars/manifest commit; barrier held
	ABS_DL_DONE,
	ABS_DL_FAILED,
	ABS_DL_CANCELLED,
	ABS_DL_STATE_COUNT
} abs_download_state_t;

const char *abs_download_state_name(abs_download_state_t state);

// Mirror of one DOWNLOAD_JOB row, plus worker RAM fields (last two). The
// database, not this struct, is the source of truth; the volatile generation
// changes on every committed, user-visible state transition so a >=4 Hz
// snapshot poll can cheaply detect that something changed (§8.1).
typedef struct {
	long long id;
	char kind[ABS_DL_KIND_MAX];
	char profile_id[ABS_DL_PROFILE_ID_MAX];
	char server[ABS_DL_SERVER_MAX]; // normalized identity snapshot
	char library_id[ABS_DL_ID_MAX];
	char item_id[ABS_DL_ID_MAX];
	char episode_id[ABS_DL_ID_MAX]; // empty for books
	char title[ABS_DL_TITLE_MAX];   // display only
	char destination[ABS_DL_DEST_MAX]; // card-relative directory identity
	abs_download_state_t state;
	long long seq;                  // monotonic FIFO order
	int file_index;                 // 0-based current file (DOWNLOADING)
	int file_count;
	long long bytes_done;           // 64-bit current-file diagnostic
	long long bytes_total;          // 64-bit; -1 unknown
	char error_stage[ABS_DL_ERROR_STAGE_MAX];
	char error[ABS_DL_ERROR_MAX];
	int index_pending;
	double remote_progress;         // remote whole-book/whole-episode progress; -1 unknown
	int remote_finished;
	long long remote_updated_ms;
	char resume_file[ABS_DL_NAME_MAX];
	double resume_seconds;          // part-relative import position; -1 none
	int ack_reset_done;             // persisted finalization barrier flag (§8.2 step 8)
	long long created_ms;
	long long updated_ms;
	// --- worker RAM, not in the database ---
	unsigned generation;            // transition generation tag
} abs_download_job_t;

// Mirror of one DOWNLOAD_FILE row (§8.2). `state` is one of "pending",
// "writing", "complete" as stored bytes.
typedef struct {
	long long job_id;
	int ordinal;                    // 1-based
	char source_url[ABS_DL_URL_MAX];
	char final_name[ABS_DL_NAME_MAX];
	double start_offset;            // ABS whole-book track offset
	double duration;                // part duration used by resume mapping; -1 unknown
	long long expected_size;        // 64-bit; -1 unknown
	long long final_size;           // 64-bit; -1 not committed
	char state[16];
} abs_download_file_t;

// One file of the plan an identity fetch produces (§8.2 steps 2–4).
typedef struct {
	const char *source_url;         // same-origin URL, no credentials
	const char *final_name;         // basename only
	double start_offset;
	double duration;
	long long expected_size;        // -1 unknown
} abs_download_file_req_t;

// The plan: produced by ops->fetch_identity, consumed by the worker. Any empty
// `destination` here leaves the job's destination untouched; a destination is
// mandatory by the time the first file is written.
typedef struct {
	const char *destination;        // card-relative directory; "" = keep job's
	const abs_download_file_req_t *files;
	int file_count;
	double remote_progress;         // -1 unknown
	int remote_finished;
	long long remote_updated_ms;
} abs_download_plan_t;

// The resume mapping the finalize step computes (§8.2 step 5).
typedef struct {
	char resume_file[ABS_DL_NAME_MAX];
	double resume_seconds;          // -1 none
} abs_download_resume_t;

// Context handed to every ops callback. `cancel` is set (with the queue mutex
// held) by abs_download_pause/cancel/quiesce on the active job; slow ops poll
// it and stop between units.
//
// `op` is the §8.5 line-1028 active-operation wake slot for THIS attempt: a
// pause/cancel/quiesce also wakes it, so an op blocked inside a network wait
// (connect, headers, a buffered/streaming body read) returns at once instead
// of riding out its receive timeout. The ops run WITHOUT the queue mutex
// held; they publish each blocking network phase through this slot (the
// http.c entry points do it when the op is attached; a fake transfer can do
// the same itself) and classify a failed read via ctx->cancel. The ops must
// additionally not call back into abs_download_pump().
typedef struct {
	long long job_id;
	abs_download_state_t state;     // FETCHING | DOWNLOADING | FINALIZING
	const volatile int *cancel;
	http_op_t *op;                  // wake slot; never NULL from the real queue
	void *user;                     // == ops->user
} abs_download_op_ctx_t;

// The ops' view of the queue's stop flag. Commands mutex-write it on other
// threads; the ops read it for PROMPTNESS (the wake slot is the real
// promptness mechanism), so the read is an acquire load: a stale read costs
// one poll step, never correctness.
static inline bool abs_download_op_cancelled(const volatile int *flag) {
	return flag && __atomic_load_n(flag, __ATOMIC_ACQUIRE) != 0;
}

// Report bytes for the active file. Only meaningful during DOWNLOADING; the
// counters are RAM-backed and persisted at pause/cancel/state transitions
// (§8.2), never on every read.
void abs_download_op_progress(const abs_download_op_ctx_t *ctx, long long done, long long total);

// ---------------------------------------------------------------------------
// Injected media surface. The queue test injects fakes; production wires the
// real implementations from the audiobookshelf_download() split (spec §8.2
// steps 1–8, §8.5). All callbacks return false + set `error` (a short
// sanitized English message) on failure. `*stage_out` picks the recorded
// stage: "auth"|"metadata"|"network"|"write"|"sidecar"|"manifest"|"index".
// Returning stage "network" from a transfer callback sends the job to
// WAITING_NETWORK; every other failure is terminal FAILED.
// ---------------------------------------------------------------------------

// Resolve the latest token for `profile_id` and verify the URL still matches
// the job's stored server (§8.1). The snapshot is used for one job run and
// released right after.
typedef struct abs_download_ops {
	void *user;

	// Fetch expanded identity for the job, validate all tracks (never silently
	// truncate: >256 files is an unsupported error), and produce the plan. May
	// fill plan->destination when the job enqueued without one.
	bool (*fetch_identity)(void *user, const abs_download_op_ctx_t *ctx,
						   const abs_download_job_t *job, abs_download_plan_t *plan,
						   const char **stage_out, char *error, size_t error_size);

	// Transfer one file to "<destination>/<final_name>"; truncate any orphaned
	// .part from byte zero (§8.1 resume semantics). Poll ctx->cancel. On success
	// the file is closed and atomically renamed; *final_size_out is its exact
	// size (the queue commits the file record only after the rename).
	bool (*file_download)(void *user, const abs_download_op_ctx_t *ctx,
						  const abs_download_file_t *file, long long *final_size_out,
						  const char **stage_out, char *error, size_t error_size);

	// Write sidecars, cover, manifest and the resume mapping for a finished
	// job's files; runs while the queue holds the FINALIZING sync barrier, so
	// progress reconciliation cannot observe the new manifest early (§8.2
	// steps 6–7).
	bool (*finalize)(void *user, const abs_download_op_ctx_t *ctx,
					 const abs_download_job_t *job, const abs_download_file_t *files,
					 int file_count, abs_download_resume_t *resume_out,
					 const char **stage_out, char *error, size_t error_size);

	// The §8.2 step 8 acknowledgement reset: audiobookdb_abs_reset() for books,
	// the episode equivalent for episodes. Called after finalize succeeds,
	// before the DONE+ack_reset_done=1 transaction releases the barrier.
	bool (*ack_reset)(void *user, const abs_download_job_t *job, char *error, size_t error_size);

	// §8.3 line 556: the scan-completion import of ONE DONE row's resume
	// state into the audiobook index. Per-row by construction: the op receives
	// only this row's own stored values (destination, resume_file basename,
	// part-relative resume_seconds, remote finished/updated_ms) plus the
	// normalized server, and performs the existing compare-and-swap import
	// exactly like the legacy page's resume import did. It runs on the thread
	// that called abs_download_import_pending() — typically the audiobook scan
	// thread — without the queue lock. False (error filled, sanitized English)
	// keeps the row's index_pending=1 for the next completed scan: audio stays
	// linked, ordinary background sync stays authoritative, DONE is not
	// downgraded and ack_reset is never re-run for it (§8.2 line 541).
	bool (*import_resume)(void *user, const abs_download_job_t *job, char *error,
						  size_t error_size);

	// Auth snapshot for the active job (§8.1). acquire copies the latest token
	// under the profile lock; release zeroes it when the job ends.
	bool (*auth_acquire)(void *user, const char *profile_id, const char *server,
						 abs_auth_snapshot_t *out);
	void (*auth_release)(void *user, abs_auth_snapshot_t *snapshot);
} abs_download_ops_t;

typedef struct {
	const char *sd_root;            // card root; DB is <sd_root>/.local/audiobookshelf-downloads.db
	const abs_download_ops_t *ops;  // NULL = storage-only queue (worker idles, commands still work)
	bool enable_worker_thread;      // one SCHED_IDLE worker thread; tests drive abs_download_pump() instead
} abs_download_config_t;

// Enqueue request. `server` must already be the normalized server URL (the
// identity; callers normalize through abs_profiles_*). episode_id is empty for
// books. `destination` may be empty when the identity fetch will compute it
// (books); episodes know their directory up front.
typedef struct {
	const char *kind;               // ABS_DL_KIND_BOOK | ABS_DL_KIND_EPISODE
	const char *profile_id;
	const char *server;
	const char *library_id;         // may be ""
	const char *item_id;
	const char *episode_id;         // "" for books
	const char *title;
	const char *destination;        // card-relative directory; may be ""
	double remote_progress;         // seed from the browse view; -1 unknown
	int remote_finished;
	long long remote_updated_ms;
} abs_download_enqueue_t;

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

// Opens (or creates) the card database, ensures schema v1, runs restart
// recovery (§8.2 recovery table) into a transaction, and starts the worker
// thread per config. Re-running after abs_download_quiesce_storage() is the
// card-reopened path: recoveries are idempotent and the card generation
// increases. False when the card or database is unusable.
bool abs_download_init(const abs_download_config_t *cfg);

// Stops the worker (ack of any pause request is awaited within a bounded
// join), persists RAM counters at the transition, closes the database, and
// disarms the queue: every other entry point refuses until the next init.
// Called before the card/SQLite close (§8.5: system.c/usb.c quiesce order).
void abs_download_quiesce_storage(void);

// Monotonic card generation: +1 on every successful init. Later waves carry
// this across the usb.c/ system.c quiesce handshake (§8.5).
unsigned abs_download_card_generation(void);

// True while anything is in FETCHING/DOWNLOADING/PAUSING/FINALIZING: the
// Wi-Fi/player activity decision inputs of §8.5 (power.c) and the busy-back
// rule of §8.1 (no download-modal back guard — busy never blocks navigation).
bool abs_download_busy(void);

// True while the storage holds a sync barrier for `destination` (a FINALIZING
// job or a DONE job whose ack_reset_done=0 commit has not happened yet).
// `destination` may be the directory itself or any file path under it,
// card-relative or absolute (the card prefix is stripped). audiobookdb's
// book/episode reconcile must call this before writing local progress (§8.5).
bool abs_download_sync_blocked(const char *destination);

// Signals a connectivity change: eligible WAITING_NETWORK jobs return to
// QUEUED (bounded per-job backoff applies), the worker wakes and continues
// the FIFO (§8.1, §8.4).
void abs_download_network_changed(void);

// ---------------------------------------------------------------------------
// Scan-completion import coordinator (§8.3 line 556)
// ---------------------------------------------------------------------------

// Iterates every DONE row with index_pending=1 and runs ops->import_resume()
// once per row with EXACTLY that row's stored values (remote progress mapped
// into the persisted resume_file/resume_seconds, destination, normalized
// server). index_pending clears only after a successful import; a failed one
// leaves the row DONE and importable again at the next completed scan.
//
// Driven by the audiobook scan-completion notification (§8.5 line 592) — the
// coordinator never starts a scan and no page imports directly. Each row is
// handled from its own copy: there is no shared "current download" state to
// overwrite, so several pending rows import independently. Runs on the
// caller's thread (the ops like every media surface, without the queue lock,
// single-flight); returns how many rows imported this pass (>= 0), or -1 when
// the queue is disarmed or has seen a card swap (error filled, English).
int abs_download_import_pending(char *error, size_t error_size);

// ---------------------------------------------------------------------------
// Commands (all transactional; false + English `error` when they cannot run)
// ---------------------------------------------------------------------------

// Enqueues durable intent. Deduplicates unfinished jobs by
// (kind, normalized_server, item_id, episode_id) — when the identity is
// already queued, the existing row id is returned and *deduped_out is true
// (§8.1: selecting an already queued item opens its existing row). Capacity is
// checked before any network activity. The dedup backstop is a partial unique
// index, so a race can never insert a second unfinished copy.
bool abs_download_enqueue_book(const abs_download_enqueue_t *req, long long *job_id_out,
							   bool *deduped_out, char *error, size_t error_size);
bool abs_download_enqueue_episode(const abs_download_enqueue_t *req, long long *job_id_out,
								  bool *deduped_out, char *error, size_t error_size);

// Pause: QUEUED/WAITING_NETWORK jobs become PAUSED immediately (durable
// state). The active job becomes PAUSING (the snapshot's honest "Stopping"),
// its media op is woken through the context's wake slot and polled through
// ctx->cancel, and only the worker records PAUSED after cleanup (§8.1, §8.2
// line 463: the bounded, non-interruptible DNS phase is part of "Stopping").
// Cancel during FINALIZING is deferred until the atomic finalize finishes.
bool abs_download_pause(long long job_id, char *error, size_t error_size);

// Resume: PAUSED -> QUEUED (0.6.0 is file-granular; completed files recorded
// complete are skipped, an interrupted current file restarts from byte zero).
// Resume is the user action that lifts the queue-wide state pauses of §8.4
// (a card write failure or a network stall); a fresh failure re-applies them.
bool abs_download_resume(long long job_id, char *error, size_t error_size);

// Cancel: stop the active job (ack via PAUSING) or, for non-active jobs,
// immediately remove any job-owned .part and record CANCELLED. Completed
// audio/sidecars are retained. Idempotent on already-cancelled jobs.
bool abs_download_cancel(long long job_id, char *error, size_t error_size);

// Remove: deletes the queue rows and any job-owned .part; never completed
// audio, sidecars, covers, manifests, resume state or remote progress. Active
// jobs must acknowledge cancellation first (busy error).
bool abs_download_remove(long long job_id, char *error, size_t error_size);

// Retry: FAILED/CANCELLED -> QUEUED. Destination and validated file records
// are reused; completed files are not redownloaded. Retry is itself user
// action (§8.4): it re-arms the FIFO after a stall; pausing a card-full job
// and resuming is the recovery path for a card write failure.
bool abs_download_retry(long long job_id, char *error, size_t error_size);

// "Clear finished": removes every DONE/FAILED/CANCELLED row (frees capacity).
bool abs_download_clear_finished(char *error, size_t error_size);

// The file rows of a job (bounded to ABS_DL_FILE_CAP).
int abs_download_job_files(long long job_id, abs_download_file_t *out, int max);

// "<card>/<card-relative>", bounded and never truncated. This is the one card
// path join for the queue's media ops (they live outside this module per
// §8.5, so they must not grow a second spelling of what recovery, cancel and
// the file records already name one way). False while quiesced, after a
// detected card swap, or when `relative` is empty or the join does not fit.
bool abs_download_card_path(const char *relative, char *out, size_t size);

// ---------------------------------------------------------------------------
// Snapshot (coalesced, >=4 Hz GUI poll; §8.1/§8.5)
// ---------------------------------------------------------------------------

typedef struct {
	unsigned generation;            // 0 while quiesced/stale
	int job_count;                  // FIFO order by seq
	bool storage_paused;            // queue-wide ENOSPC/card pause (§8.4)
	abs_download_job_t jobs[ABS_DL_JOB_CAP];
} abs_download_snapshot_t;

// Copies the current queue state under the mutex into the caller's (typically
// screen-lifetime) buffer. Returns false while quiesced or after a detected
// card swap (generation 0): the GUI must stop polling until re-init.
bool abs_download_snapshot(abs_download_snapshot_t *out);

// ---------------------------------------------------------------------------
// Worker (§8.1: one persistent worker, FIFO). Production runs the worker
// thread enabled by config; the tests drive the same engine synchronously.
// ---------------------------------------------------------------------------

// Performs at most one worker step (state transition / media op / cleanup).
// Returns true when it did something and more may remain. Never blocks.
bool abs_download_pump(void);

// §12.1 lock-order probe for the wake tests: true when the queue mutex is NOT
// held by the calling worker step, i.e. the media ops really do run unlocked.
// A blocked fake callback takes it once and thereby proves that a pause
// command could have acquired the same lock while the callback sat blocked.
bool abs_download_probe_ops_unlocked(void);

#endif /* ABS_DOWNLOAD_H */
