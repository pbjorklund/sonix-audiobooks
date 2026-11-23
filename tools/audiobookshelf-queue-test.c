// Focused §8 foundation tests for the WS2 download queue (Bob spec §8.1–8.2).
//
// No GUI code and no LVGL here; no HTTP either. The queue's media surface is
// injected through abs_download_ops_t with fake transfer callbacks that write
// real small files into a temporary sandbox card, so the durable state machine
// runs under ASan/UBSan exactly as bob §8.5's test row asks: schema, no-token
// persistence, capacity and dedup, pause/resume/cancel/remove/retry, the
// FINALIZING sync barrier, bounded network waiting with the FIFO suspension,
// the card-generation guard, crash/restart recovery, and the §8.5 storage
// lifecycle this wave wires: boot-shape init (worker on, no page anywhere)
// running real jobs, quiesce of an active blocked op without a deadlock, the
// competing-quiesce shapes of the three lifecycle callers, and the
// card-reopen generation over a stale racing response.
//
// Crash simulation forks: the child drives the queue to a chosen point and
// then _exit()s without quiescing (no cleanup, no flush — the closest a test
// can come to pulling power), and the parent re-opens the card and checks the
// §8.2 recovery table. The child's _exit skips the ASan leak check on purpose:
// it models a crash, not a clean run.
//
// The real implementations behind the fake ops (expanded-identity fetch,
// per-file HTTP transfer, finalize/manifest writer, acknowledgement reset)
// are the later M3 waves; see .specs/forge-m3-store-report.md.

#include "src/system/db/sqlite3.h"
#include "src/system/streaming/abs_download.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static const char *card; // argv[1]: the sandbox root
// The card root the fakes and the assertion helpers use. Every test arms the
// queue on its own sandbox subdirectory and make_config() records it: the
// fakes must name files under the SAME root the queue was given (the queue
// resolves job paths against its own sd_root, §8.2).
static const char *sd;

#define FAKE_SERVER "http://abs.local"
#define FAKE_TOKEN "TOK-SECRET-123"
#define FAKE_PROFILE "p1"
#define FAKE_DEST "Audiobooks/Audiobookshelf/abs-4b1f/Author/Title"

#define CHECK(cond, ...)                                                           \
	do {                                                                           \
		if (!(cond)) {                                                             \
			fprintf(stderr, "audiobookshelf-queue-test: CHECK failed %s:%d: %s\n", \
					__FILE__, __LINE__, #cond);                                    \
			fprintf(stderr, __VA_ARGS__);                                          \
			exit(1);                                                               \
		}                                                                          \
	} while (0)

#define MAX_STEPS 32 // one pump step per transition for a 3-file book

static abs_download_file_t f_record; // scratch for file_record checks

// Defined with the assertion helpers below. The fake fetch's cancel-ack probe
// (the next FIFO job must not begin before the previous one acknowledged,
// §8.1/§8.2 line 463) also reads the durable mirror.
static const abs_download_job_t *snap_job(long long id, abs_download_snapshot_t *snap_out);

// ---------------------------------------------------------------------------
// Sandbox card helpers
// ---------------------------------------------------------------------------

static char sandbox_path[600];

static const char *mkcard(const char *name) {
	snprintf(sandbox_path, sizeof(sandbox_path), "%.560s/%.24s", card, name);
	if (mkdir(sandbox_path, 0777) != 0 && errno != EEXIST) {
		CHECK(0, "sandbox %s: %s\n", sandbox_path, strerror(errno));
	}
	return sandbox_path;
}

static void mkpath(const char *rel) {
	char walk[700];
	snprintf(walk, sizeof(walk), "%.580s/%.110s", sd, rel);
	for (char *p = walk + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			mkdir(walk, 0777);
			*p = '/';
		}
	}
	mkdir(walk, 0777);
}

static void write_bytes(const char *path, size_t n, char fill) {
	FILE *f = fopen(path, "wb");
	CHECK(f != NULL, "cannot write %s: %s\n", path, strerror(errno));
	for (size_t i = 0; i < n; i++) {
		CHECK(fwrite(&fill, 1, 1, f) == 1, "short write %s\n", path);
	}
	fclose(f);
}

static bool file_exists(const char *path) {
	struct stat st;
	return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static long long file_bytes(const char *path) {
	struct stat st;
	if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
		return -1;
	}
	return (long long)st.st_size;
}

static const char *itemname(int i) {
	static char names[ABS_DL_JOB_CAP + 2][16];
	static bool built;
	if (!built) {
		// Hand-rolled, because "%02d" analysis warns on an unbounded int and
		// the ids are two digits by construction here.
		for (int k = 0; k < ABS_DL_JOB_CAP + 2; k++) {
			char *p = names[k];
			*p++ = 'i';
			*p++ = 't';
			*p++ = 'e';
			*p++ = 'm';
			*p++ = '-';
			if (k >= 10) {
				*p++ = (char)('0' + k / 10);
			}
			*p++ = (char)('0' + k % 10);
			*p = '\0';
		}
		built = true;
	}
	return names[i];
}

static const char *card_file(const char *relative, char *out, size_t size) {
	snprintf(out, size, "%.580s/%.500s", sd ? sd : card, relative);
	return out;
}

static void scan_file_for(const char *path, const char *needle, bool *found) {
	FILE *f = fopen(path, "rb");
	if (!f) {
		return;
	}
	size_t nlen = strlen(needle);
	char buf[4096];
	size_t have = 0;
	size_t n;
	while (!*found && (n = fread(buf + have, 1, sizeof(buf) - have, f)) > 0) {
		size_t total = have + n;
		if (memmem(buf, total, needle, nlen)) {
			*found = true;
			break;
		}
		size_t keep = nlen > 1 ? nlen - 1 : 0;
		if (keep > total) {
			keep = total;
		}
		memmove(buf, buf + total - keep, keep);
		have = keep;
	}
	fclose(f);
}

// ---------------------------------------------------------------------------
// Fake backend (the later M3 media waves replace these)
// ---------------------------------------------------------------------------

static abs_download_file_req_t plan_files[280];
static abs_download_plan_t fake_plan;
static char plan_url[280][256];
static char plan_name[280][32];
static char plan_dest_buf[ABS_DL_DEST_MAX];
static char resume_file_buf[ABS_DL_NAME_MAX];

static bool auth_fail;
static bool auth_was_released;
static const char *fetch_fail_stage; // "" = succeed
static const char *fetch_fail_error;
static bool fetch_oversize;  // 260 files -> explicit unsupported error
static bool fetch_foreign_url; // one file off-origin
static bool fetch_token_url;   // one file with ?token=
static bool write_fail_once;   // ENOSPC-style card pause
static bool write_fail_done;
static bool finalize_fail;
static bool pause_mid_file;          // the op itself issues abs_download_pause()
static bool cancel_mid_file;         // ... or abs_download_cancel()
static bool exit_after_stop_request; // crash child: stop persisted, ack never runs
static bool crash_on_file2;          // crash child: .part written, no rename, _exit
static bool crash_on_finalize;       // crash child: manifest written, then _exit
static int file_writes[16];          // per-ordinal transfer count (resume granularity)
static int ack_resets;
static bool seen_blocked_rel, seen_blocked_abs; // sync probe from inside finalize
static bool block_fetch_next;        // wake tests: the identity fetch blocks like a stalled server
static bool block_stream_next;       // wake tests: the file transfer blocks the same way
static bool probe_cancelled_prev;    // wake tests: the NEXT fetch must see the previous job acknowledged
static long long prev_job_probe;

static void fake_reset(void) {
	memset(plan_files, 0, sizeof(plan_files));
	memset(&fake_plan, 0, sizeof(fake_plan));
	memset(plan_url, 0, sizeof(plan_url));
	memset(plan_name, 0, sizeof(plan_name));
	plan_dest_buf[0] = '\0';
	resume_file_buf[0] = '\0';
	auth_fail = false;
	auth_was_released = false;
	fetch_fail_stage = "";
	fetch_fail_error = "";
	fetch_oversize = false;
	fetch_foreign_url = false;
	fetch_token_url = false;
	write_fail_once = false;
	write_fail_done = false;
	finalize_fail = false;
	pause_mid_file = false;
	cancel_mid_file = false;
	exit_after_stop_request = false;
	crash_on_file2 = false;
	crash_on_finalize = false;
	memset(file_writes, 0, sizeof(file_writes));
	ack_resets = 0;
	seen_blocked_rel = false;
	seen_blocked_abs = false;
	block_fetch_next = false;
	block_stream_next = false;
	probe_cancelled_prev = false;
	prev_job_probe = 0;
}

// ---------------------------------------------------------------------------
// Wake-harness synchronization (§8.5 line 1028 tests): no wall-clock sleeps in
// the unblock path. The fake blocks in a receive on a socketpair it published
// into ctx->op — a read with NO timeout — so only the queue command's
// http_op_cancel() (shutdown) can end it. Signalling is mutex/cond based; the
// only polling is over worker-ack state transitions (§8.1 semantics, not the
// unblock mechanism).
// ---------------------------------------------------------------------------

static pthread_mutex_t wake_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake_cv = PTHREAD_COND_INITIALIZER;
static int blocked_seen;    // the fake is blocked inside its published read
static int unblocked_seen;  // the wake ended the blocked read
static int close_count;     // stream stand-in closes (owner-only, below)
static bool op_thread_closed; // the close ran on the thread that ran the op
static bool next_fetch_cancel_ok;

static void wait_flag(int *flag, const char *what) {
	pthread_mutex_lock(&wake_mu);
	struct timespec until;
	clock_gettime(CLOCK_REALTIME, &until);
	until.tv_sec += 10;
	while (!*flag) {
		int rc = pthread_cond_timedwait(&wake_cv, &wake_mu, &until);
		if (rc == ETIMEDOUT) {
			pthread_mutex_unlock(&wake_mu);
			CHECK(0, "timed out waiting for %s\n", what);
		}
	}
	pthread_mutex_unlock(&wake_mu);
}

static void set_flag(int *flag) {
	pthread_mutex_lock(&wake_mu);
	*flag = 1;
	pthread_cond_broadcast(&wake_cv);
	pthread_mutex_unlock(&wake_mu);
}

static void wake_reset(void) {
	pthread_mutex_lock(&wake_mu);
	blocked_seen = 0;
	unblocked_seen = 0;
	close_count = 0;
	pthread_mutex_unlock(&wake_mu);
	op_thread_closed = false;
	next_fetch_cancel_ok = false;
	block_fetch_next = false;
	block_stream_next = false;
}

// The main↔worker hand-off flags are mutex-mediated on purpose: without it,
// the harness itself is a data race (and TSan is right to say so), even though
// every racing order here would still behave.
static void wake_put_bool(bool *flag, bool value) {
	pthread_mutex_lock(&wake_mu);
	*flag = value;
	pthread_cond_broadcast(&wake_cv);
	pthread_mutex_unlock(&wake_mu);
}

static bool wake_take_bool(bool *flag) {
	pthread_mutex_lock(&wake_mu);
	bool value = *flag;
	*flag = false;
	pthread_mutex_unlock(&wake_mu);
	return value;
}

static bool wake_get_bool(bool *flag) {
	pthread_mutex_lock(&wake_mu);
	bool value = *flag;
	pthread_mutex_unlock(&wake_mu);
	return value;
}

// The cancel-ack ordering probe: armed by main before the cancel (the FIFO
// cannot reach the next job before the ack, so the next fetch is guaranteeably
// the first taker).
static void wake_arm_prev_probe(long long job) {
	pthread_mutex_lock(&wake_mu);
	prev_job_probe = job;
	probe_cancelled_prev = true;
	pthread_mutex_unlock(&wake_mu);
}

static bool wake_take_prev_probe(void) {
	pthread_mutex_lock(&wake_mu);
	bool armed = probe_cancelled_prev;
	probe_cancelled_prev = false;
	pthread_mutex_unlock(&wake_mu);
	return armed;
}

// The fake-blocking stand-in for a stalled HTTP server: publish the socketpair
// fd in the attempt's wake slot exactly like http.c does around a body read,
// probe the §12.1 lock order (the op must run with the queue mutex released),
// then block in a receive with no timeout. Nothing writes to that socket —
// only http_op_cancel()'s shutdown can return from here, which is the
// deterministic "wakes the blocked read, not a poll interval" proof.
static void block_like_network(const abs_download_op_ctx_t *ctx) {
	CHECK(abs_download_probe_ops_unlocked(),
		  "a blocking ops callback must run without the queue mutex (§12.1)\n");
	CHECK(ctx->op != NULL, "the op context must carry the wake slot\n");
	int sv[2];
	CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair: %s\n", strerror(errno));
	http_op_publish_fd(ctx->op, sv[1]);
	pthread_t self = pthread_self(); // recorded before any close happens
	set_flag(&blocked_seen);
	char byte;
	ssize_t got = recv(sv[1], &byte, 1, 0);
	CHECK(got <= 0, "nothing is written to the blocked read; only a wake ends it\n");
	CHECK(http_op_cancelled(ctx->op), "the read ended without the cancel flag\n");
	CHECK(http_op_woken_current(ctx->op), "the wake did not target this operation\n");
	// The owner (this op thread) alone unpublishes and closes its stand-in.
	http_op_unpublish(ctx->op);
	close(sv[1]);
	close_count++;
	op_thread_closed = pthread_equal(pthread_self(), self);
	close(sv[0]);
	set_flag(&unblocked_seen);
}

static bool op_auth_acquire(void *user, const char *profile_id, const char *server,
							abs_auth_snapshot_t *out) {
	(void)user;
	CHECK(profile_id != NULL && strcmp(profile_id, FAKE_PROFILE) == 0, "unexpected profile\n");
	CHECK(strcmp(server, FAKE_SERVER) == 0, "unexpected server %s\n", server);
	if (auth_fail) {
		return false;
	}
	snprintf(out->profile_id, sizeof(out->profile_id), "%s", profile_id);
	snprintf(out->server, sizeof(out->server), "%s", server);
	snprintf(out->token, sizeof(out->token), "%s", FAKE_TOKEN); // RAM only
	return true;
}

static void op_auth_release(void *user, abs_auth_snapshot_t *snapshot) {
	(void)user;
	if (snapshot) {
		memset(snapshot, 0, sizeof(*snapshot));
	}
	auth_was_released = true;
}

static bool op_fetch_identity(void *user, const abs_download_op_ctx_t *ctx,
							  const abs_download_job_t *job, abs_download_plan_t *plan,
							  const char **stage_out, char *error, size_t error_size) {
	(void)user;
	CHECK(job != NULL, "no job handed to fetch\n");
	if (wake_take_prev_probe()) {
		// §8.1/§8.2 line 463: the next FIFO job must not begin until the
		// previous job's stop was acknowledged. This fetch runs on the worker
		// after begin_next_locked(), so the probe reads the durable mirror.
		abs_download_snapshot_t snap;
		CHECK(abs_download_snapshot(&snap), "probe snapshot\n");
		const abs_download_job_t *before = snap_job(prev_job_probe, &snap);
		CHECK(before != NULL && before->state == ABS_DL_CANCELLED,
			  "the next job started before the cancel was acknowledged\n");
		wake_put_bool(&next_fetch_cancel_ok, true);
	}
	if (wake_take_bool(&block_fetch_next)) {
		// Pause during the blocked identity fetch: the wake slot contract
		// (§8.5 line 1028) — the stop wakes this read instead of waiting it out.
		block_like_network(ctx);
		*stage_out = "network";
		snprintf(error, error_size, "The download was paused or cancelled.");
		return false; // the queue records the stop
	}
	if (fetch_fail_stage && fetch_fail_stage[0]) {
		*stage_out = fetch_fail_stage;
		snprintf(error, error_size, "%s",
				 fetch_fail_error[0] ? fetch_fail_error : "identity fetch failed");
		return false;
	}
	int count = strcmp(job->kind, ABS_DL_KIND_BOOK) == 0 ? 3 : 1;
	if (fetch_oversize) {
		count = 260;
	}
	CHECK(count <= (int)(sizeof(plan_files) / sizeof(plan_files[0])), "plan too big\n");
	for (int i = 0; i < count; i++) {
		if (strcmp(job->kind, ABS_DL_KIND_BOOK) == 0) {
			snprintf(plan_name[i], sizeof(plan_name[i]), "track%02d.mp3", i + 1);
		} else {
			snprintf(plan_name[i], sizeof(plan_name[i]), "ep%02d.mp3", i + 1);
		}
		snprintf(plan_url[i], sizeof(plan_url[i]), "%s/api/items/%s/files/%s", FAKE_SERVER, job->item_id,
				 plan_name[i]);
		plan_files[i].source_url = plan_url[i];
		plan_files[i].final_name = plan_name[i];
		plan_files[i].start_offset = i * 300.0;
		plan_files[i].duration = 300.0;
		plan_files[i].expected_size = 42 + i;
	}
	if (fetch_foreign_url && count > 1) {
		snprintf(plan_url[1], sizeof(plan_url[1]), "http://evil.invalid/track02.mp3");
		plan_files[1].source_url = plan_url[1];
	}
	if (fetch_token_url) {
		snprintf(plan_url[0], sizeof(plan_url[0]), "%s/api/items/%s/files/%s?token=sekrit", FAKE_SERVER,
				 job->item_id, plan_name[0]);
		plan_files[0].source_url = plan_url[0];
	}
	// Episodes enqueue without a destination: the identity fetch computes it,
	// like the WS5 server-qualified destination will for books.
	if (job->destination[0]) {
		snprintf(plan_dest_buf, sizeof(plan_dest_buf), "%s", job->destination);
	} else {
		snprintf(plan_dest_buf, sizeof(plan_dest_buf), "Podcasts/abs-4b1f/%s/%.200s",
				 strcmp(job->kind, ABS_DL_KIND_BOOK) == 0 ? "Author" : "Show", job->title);
	}
	fake_plan.destination = plan_dest_buf;
	fake_plan.files = plan_files;
	fake_plan.file_count = count;
	fake_plan.remote_progress = 0.5;
	fake_plan.remote_finished = 0;
	fake_plan.remote_updated_ms = 12345;
	*plan = fake_plan;
	return true;
}

// Where the fake puts one job's files. Mirrors the queue's own path builder:
// card-relative destination + basename (+ .part while writing). One active job
// at a time (§8.1), so a single static destination cell is exact here.
static void fake_paths(const char *name, const char *suffix, char *abs, size_t abs_size) {
	char rel[1400];
	snprintf(rel, sizeof(rel), "%.511s/%.255s%s", plan_dest_buf, name, suffix ? suffix : "");
	snprintf(abs, abs_size, "%.580s/%.500s", sd, rel);
}

static void fake_transfer(const abs_download_file_t *file, size_t size, bool leave_part) {
	char final_path[1400];
	char part[1400];
	fake_paths(file->final_name, "", final_path, sizeof(final_path));
	snprintf(part, sizeof(part), "%.1394s.part", final_path);
	mkpath(plan_dest_buf);
	write_bytes(part, size, 'x');
	if (!leave_part) {
		CHECK(rename(part, final_path) == 0, "rename %s: %s\n", final_path, strerror(errno));
	}
}

static bool op_file_download(void *user, const abs_download_op_ctx_t *ctx, const abs_download_file_t *file,
							 long long *final_size_out, const char **stage_out, char *error,
							 size_t error_size) {
	(void)user;
	int ordinal = file->ordinal;
	CHECK(ordinal >= 1 && ordinal < (int)(sizeof(file_writes) / sizeof(file_writes[0])), "ordinal %d\n",
		  ordinal);
	file_writes[ordinal]++;

	if (wake_take_bool(&block_stream_next)) {
		// Cancel during a blocked STREAMING body read (§8.5 line 1157): a
		// partial .part exists first, so the cancel's cleanup is observable.
		fake_transfer(file, 10, true);
		block_like_network(ctx);
		*stage_out = "network";
		snprintf(error, error_size, "The download was paused or cancelled.");
		return false; // the queue records the stop
	}
	if (ordinal == 2 && (pause_mid_file || cancel_mid_file)) {
		bool pause = pause_mid_file;
		pause_mid_file = false;
		cancel_mid_file = false;
		char err[128] = {0};
		// A partial .part exists before the stop request, so the recovery
		// tests can watch it being retained (pause) or removed (cancel).
		if (exit_after_stop_request) {
			fake_transfer(file, 10, true);
		}
		bool ok = pause ? abs_download_pause(ctx->job_id, err, sizeof(err))
						: abs_download_cancel(ctx->job_id, err, sizeof(err));
		CHECK(ok, "stop request from op failed: %s\n", err);
		if (exit_after_stop_request) {
			// §8.1: a PAUSING row persisted; the worker acknowledgement and
			// every later step die with the power.
			_exit(pause ? 78 : 79);
		}
		CHECK(*ctx->cancel, "the stop did not publish the cancel flag\n");
		abs_download_op_progress(ctx, 5, file->expected_size); // partial bytes, RAM-backed
		return false;                                          // the queue records the stop
	}
	if (ordinal == 2 && crash_on_file2 && file_writes[ordinal] == 1) {
		// Power loss mid-file: a partial .part exists, nothing is renamed,
		// the DOWNLOADING row and the 'writing' file record stay on disk.
		fake_transfer(file, 10, true);
		_exit(77);
	}
	if (write_fail_once && !write_fail_done) {
		write_fail_done = true;
		*stage_out = "write";
		snprintf(error, error_size, "No space left on device while writing the file");
		return false;
	}
	fake_transfer(file, (size_t)file->expected_size, false);
	abs_download_op_progress(ctx, file->expected_size, file->expected_size);
	*final_size_out = file->expected_size;
	return true;
}

static bool op_finalize(void *user, const abs_download_op_ctx_t *ctx, const abs_download_job_t *job,
						const abs_download_file_t *files, int file_count, abs_download_resume_t *resume_out,
						const char **stage_out, char *error, size_t error_size) {
	(void)user;
	(void)ctx;
	CHECK(job != NULL && files != NULL && file_count > 0, "finalize without files\n");
	CHECK(snprintf(plan_dest_buf, sizeof(plan_dest_buf), "%s", job->destination) > 0,
		  "finalize destination\n");
	// §8.2 step 6: the barrier must be visible from inside the finalize
	// window, for the destination directory and for a file path under it.
	seen_blocked_rel = abs_download_sync_blocked(job->destination);
	char abs_probe[1400];
	fake_paths(files[0].final_name, "", abs_probe, sizeof(abs_probe));
	seen_blocked_abs = abs_download_sync_blocked(abs_probe);
	if (crash_on_finalize) {
		char manifest[1400];
		fake_paths(".audiobookshelf", "", manifest, sizeof(manifest));
		mkpath(plan_dest_buf);
		write_bytes(manifest, 32, 'm');
		_exit(80);
	}
	if (finalize_fail) {
		*stage_out = "manifest";
		snprintf(error, error_size, "Manifest write failed");
		return false;
	}
	char manifest[1400];
	char tags[1400];
	fake_paths(".audiobookshelf", "", manifest, sizeof(manifest));
	fake_paths("track01.tags", "", tags, sizeof(tags));
	mkpath(plan_dest_buf);
	write_bytes(manifest, 96, 'm');
	write_bytes(tags, 48, 't');
	snprintf(resume_file_buf, sizeof(resume_file_buf), "track01.mp3");
	snprintf(resume_out->resume_file, sizeof(resume_out->resume_file), "%s", resume_file_buf);
	resume_out->resume_seconds = 12.5;
	return true;
}

static bool op_ack_reset(void *user, const abs_download_job_t *job, char *error, size_t error_size) {
	(void)user;
	(void)error;
	(void)error_size;
	CHECK(job != NULL, "ack without job\n");
	ack_resets++;
	return true;
}

static const abs_download_ops_t fake_ops = {
	.user = NULL,
	.fetch_identity = op_fetch_identity,
	.file_download = op_file_download,
	.finalize = op_finalize,
	.ack_reset = op_ack_reset,
	.auth_acquire = op_auth_acquire,
	.auth_release = op_auth_release,
};

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------

static abs_download_config_t make_config(const char *root, bool thread) {
	abs_download_config_t c;
	memset(&c, 0, sizeof(c));
	c.sd_root = root;
	c.ops = &fake_ops;
	c.enable_worker_thread = thread;
	sd = root;
	return c;
}

static abs_download_enqueue_t book_req(const char *item, const char *title, const char *dest) {
	abs_download_enqueue_t r;
	memset(&r, 0, sizeof(r));
	r.kind = ABS_DL_KIND_BOOK;
	r.profile_id = FAKE_PROFILE;
	r.server = FAKE_SERVER;
	r.library_id = "lib1";
	r.item_id = item;
	r.episode_id = "";
	r.title = title;
	r.destination = dest;
	r.remote_progress = -1;
	r.remote_finished = 0;
	r.remote_updated_ms = 0;
	return r;
}

static abs_download_enqueue_t episode_req(const char *item, const char *episode, const char *title) {
	abs_download_enqueue_t r = book_req(item, title, "");
	r.kind = ABS_DL_KIND_EPISODE;
	r.episode_id = episode;
	return r;
}

static long long enqueue_book(const char *item, const char *title) {
	long long id = 0;
	char err[128] = {0};
	abs_download_enqueue_t req = book_req(item, title, FAKE_DEST);
	CHECK(abs_download_enqueue_book(&req, &id, NULL, err, sizeof(err)), "enqueue %s: %s\n", item, err);
	return id;
}

static const abs_download_job_t *snap_job(long long id, abs_download_snapshot_t *snap_out) {
	CHECK(abs_download_snapshot(snap_out), "snapshot refused\n");
	for (int i = 0; i < snap_out->job_count; i++) {
		if (snap_out->jobs[i].id == id) {
			return &snap_out->jobs[i];
		}
	}
	return NULL;
}

static abs_download_state_t job_state(long long id) {
	abs_download_snapshot_t snap;
	const abs_download_job_t *j = snap_job(id, &snap);
	CHECK(j != NULL, "job %lld missing from snapshot\n", id);
	return j->state;
}

static void pump_until(long long id, abs_download_state_t want) {
	for (int i = 0; i < MAX_STEPS; i++) {
		abs_download_state_t s = job_state(id);
		if (s == want) {
			return;
		}
		if (s == ABS_DL_FAILED && want != ABS_DL_FAILED) {
			abs_download_snapshot_t snap;
			const abs_download_job_t *j = snap_job(id, &snap);
			CHECK(0, "job %lld FAILED at %s while waiting for %s\n", id, j->error,
				  abs_download_state_name(want));
		}
		abs_download_pump();
	}
	CHECK(0, "pump_until(%lld, %s) exhausted\n", id, abs_download_state_name(want));
}

static void run_to_done(long long id) {
	pump_until(id, ABS_DL_DONE);
	abs_download_snapshot_t snap;
	const abs_download_job_t *j = snap_job(id, &snap);
	CHECK(strcmp(j->error, "") == 0, "DONE with error: %s\n", j->error);
	CHECK(j->index_pending == 1 && j->ack_reset_done == 1, "DONE flags wrong (index %d ack %d)\n",
		  j->index_pending, j->ack_reset_done);
	CHECK(j->file_count == 0 || j->file_index == j->file_count, "DONE past the last file\n");
}

static void file_record(long long job_id, int ordinal, abs_download_file_t *out) {
	abs_download_file_t rows[8];
	int n = abs_download_job_files(job_id, rows, 8);
	CHECK(ordinal >= 1 && ordinal <= n, "file %d of %d missing for job %lld\n", ordinal, n, job_id);
	*out = rows[ordinal - 1];
}

static long long sole_job(void) {
	abs_download_snapshot_t snap;
	CHECK(abs_download_snapshot(&snap) && snap.job_count == 1, "expected exactly one job, have %d\n",
		  snap.job_count);
	return snap.jobs[0].id;
}

static void check_db_no_token(const char *root, bool expected) {
	char db[700];
	snprintf(db, sizeof(db), "%.580s/.local/audiobookshelf-downloads.db", root);
	bool found = false;
	scan_file_for(db, FAKE_TOKEN, &found);
	CHECK(found == expected, "token scan of %s: found %d, wanted %d\n", db, found, expected);
}

// The crash child body. Drives the queue to the chosen fault point; every
// branch ends in _exit(code) so the parent can check exactly where it died.
static void crash_child(const char *mode, const char *root) {
	fake_reset();
	if (strcmp(mode, "download") == 0) {
		crash_on_file2 = true;
	} else if (strcmp(mode, "pause") == 0) {
		pause_mid_file = true;
		exit_after_stop_request = true;
	} else if (strcmp(mode, "cancel") == 0) {
		cancel_mid_file = true;
		exit_after_stop_request = true;
	} else if (strcmp(mode, "finalize") == 0) {
		crash_on_finalize = true;
	} else {
		_exit(98);
	}
	abs_download_config_t cfg = make_config(root, false);
	CHECK(abs_download_init(&cfg), "child init failed\n");
	long long id = enqueue_book("crashitem", "Crash Title");
	CHECK(id > 0, "child enqueue failed\n");
	for (int i = 0; i < 12; i++) {
		abs_download_pump();
	}
	CHECK(0, "child finished without crashing (mode %s)\n", mode);
}

static void fork_crash(const char *mode, const char *root, int code) {
	pid_t pid = fork();
	CHECK(pid >= 0, "fork failed: %s\n", strerror(errno));
	if (pid == 0) {
		crash_child(mode, root);
		_exit(99); // only reached on a bug: the fault points _exit earlier
	}
	int status = 0;
	CHECK(waitpid(pid, &status, 0) == pid, "waitpid failed\n");
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == code,
		  "crash child %s exited %d, wanted clean _exit(%d)\n", mode,
		  WIFEXITED(status) ? WEXITSTATUS(status) : -1, code);
}

static abs_download_file_t f_record; // scratch for file_record checks

// ---------------------------------------------------------------------------
// 1. Crash/Restart recovery (§8.2 recovery table)
//    Runs first: children fork before the parent process ever arms the queue,
//    so the child starts from a clean process image.
// ---------------------------------------------------------------------------

static void test_crash_download(void) {
	printf("crash: mid-download... ");
	const char *root = mkcard("crash-dl");
	fork_crash("download", root, 77);

	// The parent re-opens the card: no quiesce ever ran, only the rows.
	fake_reset();
	abs_download_config_t cfg = make_config(root, false);
	CHECK(abs_download_init(&cfg), "recovery init failed\n");
	long long id = sole_job();
	CHECK(job_state(id) == ABS_DL_QUEUED, "recovered state %s\n", abs_download_state_name(job_state(id)));

	abs_download_file_t f1, f2, f3;
	file_record(id, 1, &f1);
	file_record(id, 2, &f2);
	file_record(id, 3, &f3);
	CHECK(strcmp(f1.state, "complete") == 0 && f1.final_size == 42, "file1 %s/%lld\n", f1.state,
		  f1.final_size);
	CHECK(strcmp(f2.state, "pending") == 0, "file2 %s\n", f2.state);
	CHECK(strcmp(f3.state, "pending") == 0, "file3 %s\n", f3.state);

	char part2[1400];
	card_file(FAKE_DEST "/track02.mp3.part", part2, sizeof(part2));
	CHECK(file_bytes(part2) == 10, "orphan .part should be retained (got %lld bytes)\n",
		  file_bytes(part2));
	// §8.2: the orphan restarts from zero on the next attempt; the queue keeps
	// it until then and never treats the partial bytes as content.
	abs_download_snapshot_t snap;
	CHECK(abs_download_snapshot(&snap) && !snap.storage_paused, "recovery side flags\n");
	CHECK(!abs_download_busy(), "recovering queues is never busy\n");

	run_to_done(id);
	CHECK(file_writes[1] == 0, "completed file redownloaded after recovery\n");
	CHECK(file_writes[2] == 1, "interrupted file restarted %d times\n", file_writes[2]);
	CHECK(file_writes[3] == 1, "pending file downloaded %d times\n", file_writes[3]);
	CHECK(ack_resets == 1 && auth_was_released, "finish flags after recovery\n");

	abs_download_quiesce_storage();
	printf("ok\n");
}

static void test_crash_pause(void) {
	printf("crash: persist a pause request... ");
	const char *root = mkcard("crash-pause");
	fork_crash("pause", root, 78);

	fake_reset();
	abs_download_config_t cfg = make_config(root, false);
	CHECK(abs_download_init(&cfg), "recovery init failed\n");
	long long id = sole_job();
	// §8.2: a PAUSING row preserves the user's pause intent and becomes PAUSED.
	CHECK(job_state(id) == ABS_DL_PAUSED, "recovered state %s\n", abs_download_state_name(job_state(id)));
	char part2[1400];
	card_file(FAKE_DEST "/track02.mp3.part", part2, sizeof(part2));
	CHECK(file_bytes(part2) == 10, "job-owned .part must be retained for pause\n");
	abs_download_file_t f1;
	file_record(id, 1, &f1);
	CHECK(strcmp(f1.state, "complete") == 0 && f1.final_size == 42, "file1 kept: %s/%lld\n", f1.state,
		  f1.final_size);
	file_record(id, 2, &f1);
	CHECK(strcmp(f1.state, "pending") == 0, "in-flight row must reset to pending\n");
	CHECK(!abs_download_busy(), "paused job is not busy\n");

	CHECK(abs_download_resume(id, NULL, 0), "resume failed\n");
	run_to_done(id); // §8.1: file-granular; the part restarts from zero
	CHECK(file_writes[1] == 0, "file1 redownloaded\n");
	CHECK(file_writes[2] == 1, "file2 not restarted from zero\n");

	abs_download_quiesce_storage();
	printf("ok\n");
}

static void test_crash_cancel(void) {
	printf("crash: cancel intent on disk... ");
	const char *root = mkcard("crash-cancel");
	fork_crash("cancel", root, 79);

	// §8.2 maps a recovered PAUSING row to PAUSED whether the intent was a
	// pause or a cancel (the cancel intent itself is worker RAM; the part is
	// cleaned by the ack of the actual stop). The card content keeps track.
	fake_reset();
	abs_download_config_t cfg = make_config(root, false);
	CHECK(abs_download_init(&cfg), "recovery init failed\n");
	long long id = sole_job();
	CHECK(job_state(id) == ABS_DL_PAUSED, "recovered state %s\n", abs_download_state_name(job_state(id)));
	char part2[1400];
	char final1[1400];
	card_file(FAKE_DEST "/track02.mp3.part", part2, sizeof(part2));
	card_file(FAKE_DEST "/track01.mp3", final1, sizeof(final1));
	CHECK(file_exists(part2) && file_exists(final1), "completed file + part retained\n");

	// A user seeing a paused job can still cancel: part goes, completions stay.
	char err[128] = {0};
	CHECK(abs_download_cancel(id, err, sizeof(err)), "cancel after recovery: %s\n", err);
	CHECK(job_state(id) == ABS_DL_CANCELLED, "cancel state\n");
	CHECK(!file_exists(part2), "cancel must remove the job-owned .part\n");
	CHECK(file_exists(final1), "cancel must retain completed audio\n");

	abs_download_quiesce_storage();
	printf("ok\n");
}

static void test_crash_finalize(void) {
	printf("crash: inside finalize... ");
	const char *root = mkcard("crash-fin");
	fork_crash("finalize", root, 80);

	// §8.2 line 540: the FINALIZING row with ack_reset_done=0 is recovered by
	// re-establishing the sync barrier (the row persists as it crashed) and
	// re-running the finalize step, whose ops reconcile from the card. The
	// fake finalize succeeds here (it rewrote the manifest), so the recovery
	// completes offline: resume rebuild + acknowledgement reset + the DONE
	// transaction, with no file redownloaded.
	fake_reset();
	abs_download_config_t cfg = make_config(root, false);
	CHECK(abs_download_init(&cfg), "recovery init failed\n");
	long long id = sole_job();
	char manifest[1400];
	card_file(FAKE_DEST "/.audiobookshelf", manifest, sizeof(manifest));
	CHECK(job_state(id) == ABS_DL_FINALIZING, "recovered state %s\n",
		  abs_download_state_name(job_state(id)));
	// The barrier is held again the moment the queue is armed, before any
	// worker step runs: book/episode reconciliation skips this destination
	// (§8.2 step 6).
	CHECK(abs_download_sync_blocked(FAKE_DEST), "the recovered barrier must hold\n");
	run_to_done(id);
	CHECK(file_writes[1] == 0 && file_writes[2] == 0 && file_writes[3] == 0,
		  "recovery redownloaded audio (%d/%d/%d)\n", file_writes[1], file_writes[2],
		  file_writes[3]);
	CHECK(file_exists(manifest) && file_bytes(manifest) == 96, "manifest not rewritten by recovery\n");
	abs_download_snapshot_t snap;
	const abs_download_job_t *j = snap_job(id, &snap);
	CHECK(strcmp(j->resume_file, "track01.mp3") == 0, "recovered resume file %s\n", j->resume_file);
	CHECK(j->resume_seconds == 12.5, "recovered resume seconds %f\n", j->resume_seconds);
	CHECK(ack_resets == 1, "ack reset performed %d times\n", ack_resets);
	// The recovered attempt never resolved a snapshot (no auth step ran), so
	// there is nothing for the engine to release here; the real adapter wipes
	// any self-refetched snapshot in its own auth_forget path.
	CHECK(!abs_download_sync_blocked(FAKE_DEST), "barrier must release after the DONE transaction\n");
	// Idempotence: reopening the card never touches a DONE acknowledgement.
	abs_download_quiesce_storage();
	cfg = make_config(root, false);
	CHECK(abs_download_init(&cfg), "second init failed\n");
	CHECK(ack_resets == 1, "a DONE row must not be reset again (ack %d)\n", ack_resets);
	CHECK(job_state(id) == ABS_DL_DONE, "second recovery state %s\n",
		  abs_download_state_name(job_state(id)));
	abs_download_quiesce_storage();
	printf("ok\n");
}

static void test_crash_finalize_fail(void) {
	printf("crash: finalize fails on recovery... ");
	const char *root = mkcard("crash-fin2");
	fork_crash("finalize", root, 80);

	// The reconciliation retries only finalization; when the media ops still
	// cannot finalize, the job records the honest terminal error and the
	// barrier releases into the recoverable failed state (§8.2 step 7 rule:
	// manifest failure means the job is not DONE).
	fake_reset();
	finalize_fail = true;
	abs_download_config_t cfg = make_config(root, false);
	CHECK(abs_download_init(&cfg), "recovery init failed\n");
	long long id = sole_job();
	pump_until(id, ABS_DL_FAILED);
	abs_download_snapshot_t snap;
	const abs_download_job_t *j = snap_job(id, &snap);
	CHECK(strcmp(j->error_stage, "manifest") == 0, "error stage %s\n", j->error_stage);
	CHECK(strstr(j->error, "manifest write failed") != NULL, "error message: %s\n", j->error);
	CHECK(!abs_download_sync_blocked(FAKE_DEST), "barrier must release on FAILED\n");
	char manifest[1400];
	card_file(FAKE_DEST "/.audiobookshelf", manifest, sizeof(manifest));
	CHECK(file_exists(manifest) && file_bytes(manifest) == 32,
		  "the crash-written manifest is retained for retry\n");
	file_record(id, 3, &f_record);
	CHECK(strcmp(f_record.state, "complete") == 0, "audio record kept\n");

	// Retry redoes finalize only (§8.1): fetch reuses every completed record.
	finalize_fail = false;
	char err[128] = {0};
	CHECK(abs_download_retry(id, err, sizeof(err)), "retry: %s\n", err);
	run_to_done(id);
	CHECK(file_writes[1] == 0 && file_writes[2] == 0 && file_writes[3] == 0,
		  "finalize-only retry redownloaded audio\n");
	CHECK(file_bytes(manifest) == 96, "manifest not rewritten by retry\n");
	CHECK(ack_resets == 1, "ack reset performed %d times\n", ack_resets);

	abs_download_quiesce_storage();
	printf("ok\n");
}

// ---------------------------------------------------------------------------
// 2. Schema (§8.2)
// ---------------------------------------------------------------------------

static void test_schema(void) {
	printf("schema... ");
	const char *root = mkcard("schema");
	fake_reset();
	abs_download_config_t cfg = make_config(root, false);
	CHECK(abs_download_init(&cfg), "init failed\n");

	char db[700];
	card_file(".local/audiobookshelf-downloads.db", db, sizeof(db));
	CHECK(file_exists(db), "queue database missing at %s\n", db);

	sqlite3 *raw = NULL;
	CHECK(sqlite3_open(db, &raw) == SQLITE_OK, "cannot open raw db\n");
	sqlite3_stmt *stmt = NULL;
	CHECK(sqlite3_prepare_v2(raw, "PRAGMA user_version", -1, &stmt, NULL) == SQLITE_OK, "pragma failed\n");
	CHECK(sqlite3_step(stmt) == SQLITE_ROW, "no user_version row\n");
	int version = sqlite3_column_int(stmt, 0);
	sqlite3_finalize(stmt);
	CHECK(version == 1, "schema version is %d, wanted 1\n", version);

	bool found_job = false, found_file = false, found_index = false;
	CHECK(sqlite3_prepare_v2(raw, "SELECT name, type FROM sqlite_master", -1, &stmt, NULL) == SQLITE_OK,
		  "sqlite_master failed\n");
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		const char *name = (const char *)sqlite3_column_text(stmt, 0);
		const char *type = (const char *)sqlite3_column_text(stmt, 1);
		if (!name || !type) {
			continue;
		}
		if (strcmp(name, "DOWNLOAD_JOB") == 0 && strcmp(type, "table") == 0) {
			found_job = true;
		}
		if (strcmp(name, "DOWNLOAD_FILE") == 0 && strcmp(type, "table") == 0) {
			found_file = true;
		}
		if (strcmp(name, "download_job_unfinished") == 0 && strcmp(type, "index") == 0) {
			found_index = true;
		}
	}
	sqlite3_finalize(stmt);
	CHECK(found_job && found_file && found_index, "schema objects missing (job %d file %d index %d)\n",
		  found_job, found_file, found_index);
	sqlite3_close(raw);

	abs_download_quiesce_storage();
	printf("ok\n");
}

// ---------------------------------------------------------------------------
// 3. Capacity + dedup (§8.1)
// ---------------------------------------------------------------------------

static void test_capacity_dedup(void) {
	printf("capacity/dedup... ");
	const char *root = mkcard("capacity");
	fake_reset();
	abs_download_config_t cfg = make_config(root, false);
	CHECK(abs_download_init(&cfg), "init failed\n");

	char title[32];
	char err[128] = {0};
	long long ids[ABS_DL_JOB_CAP] = {0};
	for (int i = 0; i < ABS_DL_JOB_CAP; i++) {
		snprintf(title, sizeof(title), "Book %02d", i);
		abs_download_enqueue_t req = book_req(itemname(i), title, FAKE_DEST);
		CHECK(abs_download_enqueue_book(&req, &ids[i], NULL, err, sizeof(err)), "enqueue %d: %s\n", i, err);
		CHECK(ids[i] > 0, "enqueue %d got id %lld\n", i, ids[i]);
	}
	abs_download_snapshot_t snap;
	CHECK(abs_download_snapshot(&snap) && snap.job_count == ABS_DL_JOB_CAP, "mirror has %d jobs\n",
		  snap.job_count);

	abs_download_enqueue_t over = book_req("item-99", "Book over", FAKE_DEST);
	CHECK(!abs_download_enqueue_book(&over, NULL, NULL, err, sizeof(err)), "17th enqueue passed\n");
	CHECK(strstr(err, "full") != NULL, "wrong full-queue error: %s\n", err);

	// §8.1: selecting an already queued item opens its existing row.
	bool deduped = false;
	long long again = 0;
	abs_download_enqueue_t dup = book_req(itemname(5), "Book 05", FAKE_DEST);
	CHECK(abs_download_enqueue_book(&dup, &again, &deduped, err, sizeof(err)), "dedup enqueue failed\n");
	CHECK(deduped && again == ids[5], "dedup mismatch (deduped %d id %lld vs %lld)\n", deduped, again,
		  ids[5]);
	CHECK(abs_download_snapshot(&snap) && snap.job_count == ABS_DL_JOB_CAP, "dedup added a row\n");

	// The 16-job bound counts terminal history too (§8.1); capacity frees only
	// when the finished row is removed or cleared.
	CHECK(abs_download_cancel(ids[7], err, sizeof(err)), "cancel failed: %s\n", err);
	CHECK(!abs_download_enqueue_book(&over, NULL, NULL, err, sizeof(err)),
		  "enqueue past the bound with terminal history present\n");
	CHECK(abs_download_remove(ids[7], err, sizeof(err)), "remove failed: %s\n", err);
	CHECK(abs_download_enqueue_book(&over, NULL, NULL, err, sizeof(err)), "enqueue after remove failed\n");

	// A finished identity can be enqueued again after removal (§8.2 partial
	// unique index covers unfinished rows only).
	CHECK(abs_download_cancel(ids[8], err, sizeof(err)), "cancel 8 failed: %s\n", err);
	CHECK(abs_download_remove(ids[8], err, sizeof(err)), "remove 8 failed: %s\n", err);
	bool deduped2 = true;
	long long twin = 0;
	abs_download_enqueue_t fresh8 = book_req(itemname(8), "Book 08", FAKE_DEST);
	CHECK(abs_download_enqueue_book(&fresh8, &twin, &deduped2, err, sizeof(err)), "re-enqueue failed\n");
	CHECK(!deduped2 && twin != ids[8], "terminal row leaked into dedup\n");

	// The bound counts terminal history (§8.1): nothing is terminal right now
	// (ids 7/8 were removed and re-enqueued), so Clear-finished must remove
	// nothing; the explicit cancels then give the second clear work to do.
	CHECK(abs_download_clear_finished(err, sizeof(err)), "clear finished failed: %s\n", err);
	CHECK(abs_download_snapshot(&snap) && snap.job_count == ABS_DL_JOB_CAP,
		  "first clear removed unfinished rows (%d)\n", snap.job_count);
	CHECK(abs_download_cancel(ids[1], err, sizeof(err)) && abs_download_cancel(ids[2], err, sizeof(err)),
		  "cancels failed: %s\n", err);
	CHECK(abs_download_clear_finished(err, sizeof(err)), "second clear failed: %s\n", err);
	CHECK(abs_download_snapshot(&snap) && snap.job_count == ABS_DL_JOB_CAP - 2,
		  "clear left %d rows (wanted 14 unfinished)\n", snap.job_count);

	// Freed capacity is usable again, and the dedup row of `over` is still the
	// one row (two refills cannot stack).
	long long again_over = 0;
	CHECK(abs_download_enqueue_book(&over, &again_over, NULL, err, sizeof(err)), "refill enqueue failed\n");
	long long over_id = 0;
	CHECK(abs_download_enqueue_book(&over, &over_id, NULL, err, sizeof(err)), "dedup refill failed\n");
	CHECK(again_over == over_id, "deduped enqueues returned different ids\n");
	// Two fresh enqueues reach the bound; a third distinct identity is refused
	// before any network activity (§8.1).
	long long r1 = 0, r2 = 0;
	abs_download_enqueue_t f1 = book_req("item-96", "Book 96", FAKE_DEST);
	abs_download_enqueue_t f2 = book_req("item-97", "Book 97", FAKE_DEST);
	CHECK(abs_download_enqueue_book(&f1, &r1, NULL, err, sizeof(err)), "refill 96 failed\n");
	CHECK(abs_download_enqueue_book(&f2, &r2, NULL, err, sizeof(err)), "refill 97 failed\n");
	abs_download_enqueue_t third = book_req("item-98", "Book 98", FAKE_DEST);
	CHECK(!abs_download_enqueue_book(&third, NULL, NULL, err, sizeof(err)), "capacity leaked\n");
	CHECK(abs_download_remove(r1, err, sizeof(err)) && abs_download_remove(r2, err, sizeof(err)),
		  "refill cleanup failed\n");

	// Field validation: books reject episode ids; episodes require one.
	abs_download_enqueue_t bogus = book_req("item-x0", "X", FAKE_DEST);
	bogus.episode_id = "e9";
	CHECK(!abs_download_enqueue_book(&bogus, NULL, NULL, err, sizeof(err)), "book with episode passed\n");
	abs_download_enqueue_t ep = episode_req("item-x1", "", "Episode X1");
	CHECK(!abs_download_enqueue_episode(&ep, NULL, NULL, err, sizeof(err)), "episode without id passed\n");

	// Book and episode identities are disjoint even with the same item id.
	long long book_id = 0, ep_id = 0;
	abs_download_enqueue_t b = book_req("shared-item", "Shared", FAKE_DEST);
	abs_download_enqueue_t e = episode_req("shared-item", "ep-1", "Shared Ep");
	CHECK(abs_download_enqueue_book(&b, &book_id, NULL, err, sizeof(err)), "book enqueue: %s\n", err);
	CHECK(abs_download_enqueue_episode(&e, &ep_id, NULL, err, sizeof(err)), "episode enqueue: %s\n", err);
	CHECK(book_id != ep_id, "episode collided with the book identity\n");

	abs_download_quiesce_storage();
	printf("ok\n");
}

// ---------------------------------------------------------------------------
// 4. State machine and commands (§8.1)
// ---------------------------------------------------------------------------

static void test_lifecycle(void) {
	printf("state transitions... ");
	const char *root = mkcard("lifecycle");
	fake_reset();
	abs_download_config_t cfg = make_config(root, false);
	CHECK(abs_download_init(&cfg), "init failed\n");
	char err[128] = {0};

	long long b1 = enqueue_book("t1", "Transitions 1");
	long long b2 = enqueue_book("t2", "Transitions 2");
	long long b3 = enqueue_book("t3", "Transitions 3");

	// Pause/cancel while QUEUED: immediate and durable (§8.1).
	CHECK(abs_download_pause(b2, err, sizeof(err)), "pause queued: %s\n", err);
	CHECK(job_state(b2) == ABS_DL_PAUSED, "pause state\n");
	CHECK(!abs_download_busy(), "paused job keeps busy\n");
	CHECK(abs_download_resume(b2, err, sizeof(err)), "resume: %s\n", err);
	CHECK(job_state(b2) == ABS_DL_QUEUED, "resume state\n");
	CHECK(abs_download_pause(b2, err, sizeof(err)), "re-pause\n");
	CHECK(abs_download_cancel(b3, err, sizeof(err)), "cancel queued: %s\n", err);
	CHECK(job_state(b3) == ABS_DL_CANCELLED, "cancel state\n");
	CHECK(abs_download_remove(b3, err, sizeof(err)), "remove: %s\n", err);
	abs_download_snapshot_t snap;
	CHECK(abs_download_snapshot(&snap), "snapshot\n");
	for (int i = 0; i < snap.job_count; i++) {
		CHECK(snap.jobs[i].id != b3, "removed job still in the mirror\n");
	}

	// Active flow: FETCHING resolves auth then identity; DOWNLOADING commits
	// file records only after each transfer's rename (§8.2 step 4).
	pump_until(b1, ABS_DL_FETCHING);
	pump_until(b1, ABS_DL_DOWNLOADING);
	CHECK(abs_download_busy(), "active job must be busy\n");
	abs_download_file_t f;
	file_record(b1, 1, &f);
	CHECK(strcmp(f.state, "pending") == 0, "file1 starts pending, is %s\n", f.state);
	CHECK(abs_download_pump(), "the file 1 pump did nothing\n"); // transfer + commit
	file_record(b1, 1, &f);
	CHECK(strcmp(f.state, "complete") == 0 && f.final_size == 42, "file1 record: %s/%lld\n", f.state,
		  f.final_size);
	char final1[1400];
	char part1[1400];
	card_file(FAKE_DEST "/track01.mp3", final1, sizeof(final1));
	card_file(FAKE_DEST "/track01.mp3.part", part1, sizeof(part1));
	CHECK(file_bytes(final1) == 42 && !file_exists(part1), "file1 renamed, no .part left\n");
	CHECK(job_state(b1) == ABS_DL_DOWNLOADING, "still downloading\n");

	// Pause the active job from the op callback (what the GUI thread does).
	pause_mid_file = true;
	pump_until(b1, ABS_DL_PAUSING);
	pump_until(b1, ABS_DL_PAUSED);
	CHECK(!abs_download_busy(), "paused active job must leave busy\n");
	char part2[1400];
	card_file(FAKE_DEST "/track02.mp3.part", part2, sizeof(part2));
	CHECK(!file_exists(part2), "paused job wrote nothing for file2\n");
	file_record(b1, 2, &f);
	CHECK(strcmp(f.state, "pending") == 0, "interrupted row must be pending\n");
	file_record(b1, 1, &f);
	CHECK(strcmp(f.state, "complete") == 0, "file1 record retained\n");

	// Resume: file-granular (§8.1). File 1 must not transfer again; file 2
	// restarts from byte zero (its aborted attempt is attempt #1, the resume
	// from zero is #2 — the fake counter counts attempts).
	int writes1 = file_writes[1];
	int writes2 = file_writes[2];
	CHECK(abs_download_resume(b1, err, sizeof(err)), "resume b1: %s\n", err);
	run_to_done(b1);
	CHECK(file_writes[1] == writes1, "completed file redownloaded on resume\n");
	CHECK(file_writes[2] == writes2 + 1 && file_writes[3] == 1, "remaining files transferred\n");
	CHECK(ack_resets == 1 && auth_was_released, "ack + auth release after DONE\n");
	CHECK(seen_blocked_rel && seen_blocked_abs,
		  "sync barrier not visible from inside finalize (rel %d abs %d)\n", seen_blocked_rel,
		  seen_blocked_abs);
	CHECK(!abs_download_sync_blocked(FAKE_DEST), "barrier must release after DONE\n");
	char manifest[1400];
	card_file(FAKE_DEST "/.audiobookshelf", manifest, sizeof(manifest));
	CHECK(file_bytes(manifest) == 96, "manifest missing (%lld bytes)\n", file_bytes(manifest));

	// The paused b2 takes its turn and completes as a second FIFO job.
	CHECK(abs_download_resume(b2, err, sizeof(err)), "resume b2: %s\n", err);
	run_to_done(b2);
	CHECK(ack_resets == 2, "second ack reset\n");

	// §8.4: one network stall suspends the FIFO; a change inside the backoff
	// window must not cause an instant retry storm.
	fetch_fail_stage = "network";
	fetch_fail_error = "Connection refused while fetching identity";
	long long w1 = enqueue_book("n1", "Net 1");
	long long w2 = enqueue_book("n2", "Net 2");
	pump_until(w1, ABS_DL_WAITING_NETWORK);
	CHECK(job_state(w2) == ABS_DL_QUEUED, "FIFO must suspend behind a waiting job\n");
	CHECK(!abs_download_pump(), "suspended FIFO resumed picking on its own\n");
	abs_download_network_changed();
	CHECK(job_state(w1) == ABS_DL_WAITING_NETWORK, "backoff window must hold\n");
	CHECK(abs_download_pause(w1, err, sizeof(err)), "pause waiting: %s\n", err);
	fetch_fail_stage = "";
	CHECK(abs_download_resume(w1, err, sizeof(err)), "resume waiting: %s\n", err); // user action lifts it
	run_to_done(w1);
	run_to_done(w2);
	CHECK(ack_resets == 4, "ack resets after the waiting pair\n");

	// §8.4: a card write failure pauses the queue as a whole until user action.
	write_fail_once = true;
	long long s1 = enqueue_book("s1", "Space 1");
	long long s2 = enqueue_book("s2", "Space 2");
	pump_until(s1, ABS_DL_PAUSED);
	CHECK(abs_download_snapshot(&snap) && snap.storage_paused, "card pause flag missing\n");
	const abs_download_job_t *j = snap_job(s1, &snap);
	CHECK(strcmp(j->error_stage, "write") == 0, "stage %s\n", j->error_stage);
	CHECK(job_state(s2) == ABS_DL_QUEUED, "next job must not start into the same failure\n");
	CHECK(!abs_download_pump(), "paused queue picked the next job\n");
	write_fail_once = false; // the user frees space on the card
	CHECK(abs_download_resume(s1, err, sizeof(err)), "resume past the card pause: %s\n", err);
	CHECK(abs_download_snapshot(&snap) && !snap.storage_paused, "resume must lift the card pause\n");
	run_to_done(s1);
	run_to_done(s2);
	CHECK(ack_resets == 6, "ack resets after the space pair\n");

	// Permanent metadata failure: FAILED, then reuse on retry (§8.1).
	fetch_fail_stage = "metadata";
	fetch_fail_error = "HTTP 404 from the item endpoint";
	long long f1id = enqueue_book("f1", "Fail 1");
	pump_until(f1id, ABS_DL_FAILED);
	j = snap_job(f1id, &snap);
	CHECK(strcmp(j->error_stage, "metadata") == 0, "stage %s\n", j->error_stage);
	CHECK(strstr(j->error, "404") != NULL, "error lost: %s\n", j->error);
	fetch_fail_stage = "";
	CHECK(abs_download_retry(f1id, err, sizeof(err)), "retry failed: %s\n", err);
	run_to_done(f1id);
	CHECK(ack_resets == 7, "ack resets after the failure retry\n");

	// Finalization failure: FAILED keeps every completed file record; Retry
	// redoes finalize only (§8.1 retry semantics).
	finalize_fail = true;
	long long m1 = enqueue_book("m1", "Manifest 1");
	pump_until(m1, ABS_DL_FAILED);
	j = snap_job(m1, &snap);
	CHECK(strcmp(j->error_stage, "manifest") == 0, "stage %s\n", j->error_stage);
	file_record(m1, 3, &f);
	CHECK(strcmp(f.state, "complete") == 0, "audio record must survive a manifest failure\n");
	int writes_before = file_writes[1] + file_writes[2] + file_writes[3];
	finalize_fail = false;
	CHECK(abs_download_retry(m1, err, sizeof(err)), "retry finalize: %s\n", err);
	run_to_done(m1);
	CHECK(file_writes[1] + file_writes[2] + file_writes[3] == writes_before,
		  "finalize-only retry redownloaded audio\n");
	CHECK(ack_resets == 8, "ack resets after finalize retry\n");

	// §8.4/§8.5: 401-class auth failures are permanent FAILED jobs.
	auth_fail = true;
	long long a1 = enqueue_book("a1", "Auth 1");
	pump_until(a1, ABS_DL_FAILED);
	j = snap_job(a1, &snap);
	CHECK(strcmp(j->error_stage, "auth") == 0, "stage %s\n", j->error_stage);
	auth_fail = false;

	// Episodes: no destination at enqueue, the fetch computes it; single file.
	long long ep = 0;
	abs_download_enqueue_t ereq = episode_req("shared-ep", "ep-9", "Episode Nine");
	CHECK(abs_download_enqueue_episode(&ereq, &ep, NULL, err, sizeof(err)), "episode enqueue: %s\n", err);
	run_to_done(ep);
	char ep_final[1400];
	card_file("Podcasts/abs-4b1f/Show/Episode Nine/ep01.mp3", ep_final, sizeof(ep_final));
	CHECK(file_bytes(ep_final) == 42, "episode file missing (%lld)\n", file_bytes(ep_final));
	CHECK(!abs_download_sync_blocked("Podcasts/abs-4b1f/Show/Episode Nine"),
		  "episode barrier must release after DONE\n");

	abs_download_quiesce_storage();
	printf("ok\n");
}

// ---------------------------------------------------------------------------
// 5. No-token persistence + sanitization (§8.2, §8.4)
// ---------------------------------------------------------------------------

static void test_no_token(void) {
	printf("no-token persistence... ");
	const char *root = mkcard("notoken");
	fake_reset();
	abs_download_config_t cfg = make_config(root, false);
	CHECK(abs_download_init(&cfg), "init failed\n");
	char err[128] = {0};

	long long ok1 = enqueue_book("nt1", "Clean 1");
	run_to_done(ok1);
	check_db_no_token(root, false); // the strongest form: absence
	CHECK(auth_was_released, "snapshot must be released at job end\n");

	// A failed job must not smuggle credentials into the error column either.
	auth_fail = true;
	long long a2 = enqueue_book("nt2", "Auth 2");
	pump_until(a2, ABS_DL_FAILED);
	check_db_no_token(root, false);
	auth_fail = false;

	fetch_fail_stage = "metadata";
	fetch_fail_error = "rejected: Authorization: Bearer eyJhbGciOi and token=qwerty1234";
	long long e1 = enqueue_book("nt3", "Leak attempt");
	pump_until(e1, ABS_DL_FAILED);
	abs_download_snapshot_t snap;
	const abs_download_job_t *j = snap_job(e1, &snap);
	CHECK(strstr(j->error, "[redacted]") != NULL, "error not redacted: %s\n", j->error);
	CHECK(!strstr(j->error, "eyJhbGciOi") && !strstr(j->error, "qwerty1234"),
		  "credential text survived: %s\n", j->error);
	check_db_no_token(root, false);

	// Download-file rows: cross-origin and credential URLs never persist.
	fetch_fail_stage = "";
	fetch_foreign_url = true;
	long long eo = enqueue_book("nt4", "Foreign file");
	pump_until(eo, ABS_DL_FAILED);
	j = snap_job(eo, &snap);
	CHECK(strcmp(j->error_stage, "metadata") == 0, "foreign URL stage %s\n", j->error_stage);
	abs_download_file_t rows[8];
	CHECK(abs_download_job_files(eo, rows, 8) == 0, "foreign URL row persisted\n");

	fetch_foreign_url = false;
	fetch_token_url = true;
	long long et = enqueue_book("nt5", "Tokened URL");
	pump_until(et, ABS_DL_FAILED);
	j = snap_job(et, &snap);
	CHECK(strcmp(j->error_stage, "metadata") == 0, "tokened URL stage %s\n", j->error_stage);
	CHECK(abs_download_job_files(et, rows, 8) == 0, "tokened URL row persisted\n");
	check_db_no_token(root, false); // sekrit must not have reached the disk
	fetch_token_url = false;

	// >256 files is an explicit unsupported error, never truncation (§8.2).
	fetch_oversize = true;
	long long os = enqueue_book("nt6", "Too many files");
	pump_until(os, ABS_DL_FAILED);
	j = snap_job(os, &snap);
	CHECK(strstr(j->error, "256") != NULL, "oversize error: %s\n", j->error);
	CHECK(abs_download_job_files(os, rows, 8) == 0, "oversize plan persisted rows\n");
	fetch_oversize = false;

	// The queue refuses server values that are not clean normalized URLs.
	abs_download_enqueue_t bad = book_req("nt7", "Bad server", FAKE_DEST);
	bad.server = "http://user:pass@abs.local";
	CHECK(!abs_download_enqueue_book(&bad, NULL, NULL, err, sizeof(err)), "credential server passed\n");
	bad.server = "ftp://abs.local";
	CHECK(!abs_download_enqueue_book(&bad, NULL, NULL, err, sizeof(err)), "non-http server passed\n");

	abs_download_quiesce_storage();
	printf("ok\n");
}

// ---------------------------------------------------------------------------
// 6. Card-generation guard (§8.5 system.c/usb.c handshake, foundation side)
// ---------------------------------------------------------------------------

static void test_card_generation(void) {
	printf("card generation... ");
	const char *root = mkcard("gen");
	fake_reset();
	unsigned base = abs_download_card_generation();
	abs_download_config_t cfg = make_config(root, false);

	// init raises the generation exactly once per successful init.
	CHECK(abs_download_init(&cfg), "init\n");
	CHECK(abs_download_card_generation() == base + 1, "generation after init\n");
	long long g1 = enqueue_book("g1", "Generation 1");

	// Quiesced storage refuses commands but does not change the generation.
	abs_download_quiesce_storage();
	char err[128] = {0};
	abs_download_enqueue_t over = book_req("g2", "Generation 2", FAKE_DEST);
	CHECK(!abs_download_enqueue_book(&over, NULL, NULL, err, sizeof(err)), "enqueue while quiesced\n");
	CHECK(strstr(err, "not available") != NULL, "wrong quiesced error: %s\n", err);
	abs_download_snapshot_t snap;
	CHECK(!abs_download_snapshot(&snap) && snap.generation == 0, "snapshot while quiesced must refuse\n");
	CHECK(abs_download_card_generation() == base + 1, "quiesce changed the generation\n");

	// Re-init on the same card: recovery is idempotent, the job persists, the
	// generation rises (the §8.5 card-reopened handshake).
	CHECK(abs_download_init(&cfg), "re-init\n");
	CHECK(abs_download_card_generation() == base + 2, "generation after re-init\n");
	CHECK(job_state(g1) == ABS_DL_QUEUED, "job lost across quiesce cycle\n");

	// Card swapped under the armed queue: every access refuses until quiesce
	// + re-init; the generation counter itself does not move.
	char db[700];
	card_file(".local/audiobookshelf-downloads.db", db, sizeof(db));
	CHECK(unlink(db) == 0, "cannot drop the db: %s\n", strerror(errno));
	char garbage[700];
	card_file(".local/audiobookshelf-downloads.db", garbage, sizeof(garbage));
	write_bytes(garbage, 64, 'g');
	CHECK(!abs_download_snapshot(&snap) && snap.generation == 0, "snapshot after swap\n");
	CHECK(!abs_download_enqueue_book(&over, NULL, NULL, err, sizeof(err)), "enqueue after swap\n");
	abs_download_file_t rows[8];
	CHECK(abs_download_job_files(g1, rows, 8) == 0, "files read after swap must fail cleanly\n");
	CHECK(abs_download_card_generation() == base + 2, "stale state moved the generation\n");
	CHECK(!abs_download_sync_blocked(FAKE_DEST), "blocked check after swap\n");

	// The card came back without our download database but with a foreign
	// file at its path: init must refuse rather than overwrite (schema
	// version -1), and quiesce closes what is left of the armed state.
	abs_download_quiesce_storage();
	CHECK(!abs_download_init(&cfg), "a foreign file was overwritten\n");
	// Now the foreign file is gone and the card hosts a fresh queue again.
	CHECK(unlink(garbage) == 0, "cannot drop the swapped file: %s\n", strerror(errno));
	CHECK(abs_download_init(&cfg), "init after swap cleanup\n");
	CHECK(abs_download_card_generation() == base + 3, "generation after swap recovery\n");
	CHECK(abs_download_snapshot(&snap) && snap.job_count == 0, "stale mirror survived re-init\n");

	abs_download_quiesce_storage();
	printf("ok\n");
}

// ---------------------------------------------------------------------------
// 7. Worker thread (§8.1: one persistent worker) — production-mode smoke
// ---------------------------------------------------------------------------

static void test_worker_thread(void) {
	printf("worker thread... ");
	const char *root = mkcard("thread");
	fake_reset();
	abs_download_config_t cfg = make_config(root, true);
	CHECK(abs_download_init(&cfg), "init\n");

	enqueue_book("w9a", "Thread 1");
	enqueue_book("w9b", "Thread 2");
	bool both_done = false;
	for (int i = 0; i < 20000 && !both_done; i++) {
		abs_download_snapshot_t snap;
		CHECK(abs_download_snapshot(&snap), "snapshot in thread mode\n");
		both_done = snap.job_count == 2;
		for (int k = 0; both_done && k < snap.job_count; k++) {
			if (snap.jobs[k].state != ABS_DL_DONE) {
				both_done = false;
			}
		}
		usleep(1000);
	}
	CHECK(both_done, "worker thread did not finish the FIFO in time\n");
	CHECK(!abs_download_busy(), "busy after the FIFO drains\n");
	// Both books share the per-ordinal fake counters: 2 transfers each.
	CHECK(ack_resets == 2 && file_writes[1] == 2 && file_writes[2] == 2 && file_writes[3] == 2,
		  "fake counters wrong (ack %d w1 %d w2 %d w3 %d)\n", ack_resets, file_writes[1], file_writes[2],
		  file_writes[3]);

	abs_download_quiesce_storage(); // joins the thread before the card closes
	printf("ok\n");
}

// ---------------------------------------------------------------------------
// 8. Active-operation wake (§8.2 line 463 pause semantics, §8.5 line 1028/1157)
// ---------------------------------------------------------------------------

// Bounded wait for a state transition driven by the worker thread (§8.1 ack).
// The wake itself is never behind this loop: the blocked read ended on the
// shutdown, and the wake_cv handshake saw to it. This only polls the ack.
static void wait_state(long long id, abs_download_state_t want, const char *what) {
	bool reached = false;
	for (int i = 0; i < 20000 && !reached; i++) {
		reached = job_state(id) == want;
		if (!reached) {
			usleep(1000);
		}
	}
	CHECK(reached, "job %lld never reached %s (now %s): %s\n", id, abs_download_state_name(want),
		  abs_download_state_name(job_state(id)), what);
}

// §8.2 line 463: pausing the ACTIVE job records PAUSING durably ("Stopping"
// in the snapshot, honestly) and wakes the blocked op; only the worker marks
// PAUSED after cleanup. The blocked op here is the BUFFERED identity fetch.
static void test_wake_pause_during_fetch(void) {
	printf("wake: pause wakes a blocked buffered op... ");
	const char *root = mkcard("wake-pause");
	fake_reset();
	wake_reset();
	abs_download_config_t cfg = make_config(root, true);
	CHECK(abs_download_init(&cfg), "init\n");
	long long id = enqueue_book("wka", "Wake Pause");

	wake_put_bool(&block_fetch_next, true);
	wait_flag(&blocked_seen, "the identity fetch to block");
	CHECK(abs_download_probe_ops_unlocked(), "self-check: the probe works outside ops\n");
	abs_download_snapshot_t snap;
	const abs_download_job_t *j = snap_job(id, &snap);
	CHECK(j->state == ABS_DL_FETCHING, "state while blocked: %s\n", abs_download_state_name(j->state));

	char err[128] = {0};
	CHECK(abs_download_pause(id, err, sizeof(err)), "pause: %s\n", err);
	j = snap_job(id, &snap);
	CHECK(j->state == ABS_DL_PAUSING, "the snapshot must say Stopping at once: %s\n",
		  abs_download_state_name(j->state));
	wait_flag(&unblocked_seen, "the blocked fetch to wake on cancel");
	CHECK(op_thread_closed, "the op thread owns the close of its stand-in stream\n");
	wait_state(id, ABS_DL_PAUSED, "the worker must acknowledge the pause promptly");

	// Queued intent, durable. Resume finishes the book with ordinary fakes;
	// the fetch's wake slot is drained, so nothing of the old stop leaks in
	// (http_op_clear at attempt boundaries).
	CHECK(abs_download_resume(id, err, sizeof(err)), "resume: %s\n", err);
	wait_state(id, ABS_DL_DONE, "resumed run completes");
	CHECK(ack_resets == 1, "acknowledgement reset ran\n");

	abs_download_quiesce_storage();
	printf("ok\n");
}

// §8.5 line 1157: a cancelled streaming body read is woken mid-body; the op
// thread alone closes its connection; the cancelled .part goes; and the next
// FIFO job begins ONLY after the cancel was acknowledged (§8.1).
static void test_wake_cancel_next_job_ordering(void) {
	printf("wake: cancel wakes a blocked stream, FIFO waits for the ack... ");
	const char *root = mkcard("wake-cancel");
	fake_reset();
	wake_reset();
	abs_download_config_t cfg = make_config(root, true);
	CHECK(abs_download_init(&cfg), "init\n");
	char err[128] = {0};
	abs_download_enqueue_t req_a = book_req("wkb", "Wake Cancel A", FAKE_DEST);
	long long a = 0;
	CHECK(abs_download_enqueue_book(&req_a, &a, NULL, err, sizeof(err)), "enqueue A: %s\n", err);
	// A distinct destination: the probe job must not share A's files.
	char dest_b[ABS_DL_DEST_MAX];
	snprintf(dest_b, sizeof(dest_b), "%s-B", FAKE_DEST);
	abs_download_enqueue_t req_b = book_req("wkc", "Wake Cancel B", dest_b);
	long long b = 0;
	CHECK(abs_download_enqueue_book(&req_b, &b, NULL, err, sizeof(err)), "enqueue B: %s\n", err);

	wake_put_bool(&block_stream_next, true);
	wait_flag(&blocked_seen, "the streaming transfer to block");
	wait_state(a, ABS_DL_DOWNLOADING, "the active transfer is running");
	char part[1400];
	fake_paths("track01.mp3", ".part", part, sizeof(part));
	CHECK(file_exists(part), "the transfer's .part exists while blocked\n");

	abs_download_snapshot_t snap;
	const abs_download_job_t *j = snap_job(b, &snap);
	CHECK(j->state == ABS_DL_QUEUED, "the next job must still be queued\n");

	// Arm the ordering probe BEFORE the cancel: the FIFO cannot reach B until
	// A's stop is acknowledged, so B's fetch is guaranteed to be the next
	// probe point — and it must then see A durably CANCELLED.
	wake_arm_prev_probe(a);

	CHECK(abs_download_cancel(a, err, sizeof(err)), "cancel: %s\n", err);
	j = snap_job(a, &snap);
	CHECK(j->state == ABS_DL_PAUSING, "stopping state after cancel: %s\n", abs_download_state_name(j->state));
	wait_flag(&unblocked_seen, "the blocked stream read to wake on cancel");
	CHECK(close_count == 1 && op_thread_closed,
		  "the op thread alone closed its woken stream exactly once\n");
	wait_state(a, ABS_DL_CANCELLED, "the worker must acknowledge the cancel promptly");
	CHECK(!file_exists(part), "cancel removes the job-owned .part\n");

	// The FIFO may only touch B after the ack; B's fetch ran the probe that
	// re-read A's durable state under the queue mutex.
	wait_state(b, ABS_DL_DONE, "the next job proceeds after the ack");
	CHECK(wake_get_bool(&next_fetch_cancel_ok), "the next job's fetch never saw the cancelled ack\n");

	abs_download_quiesce_storage();
	printf("ok\n");
}

// ---------------------------------------------------------------------------
// 9. Busy gate: abs_download_busy() is the §8.3/§8.5 power-decision input
//    (power.c consults it for the Wi-Fi radio and the shutdown/standby
//    timer). This is its exact truth table over the active states, asserted
//    deterministically in pump mode. The wiring consult itself is proven
//    structurally in audiobookshelf-queue-test.sh (power.c compiles no
//    standalone unit; see that script).
// ---------------------------------------------------------------------------

static void test_busy_gate(void) {
	printf("busy gate (the power decision's input)... ");
	const char *root = mkcard("busy");
	fake_reset();
	abs_download_config_t cfg = make_config(root, false);
	CHECK(abs_download_init(&cfg), "init\n");
	char err[128] = {0};

	// The pause lands from the op callback at ordinal 2, so the run passes
	// through every active state once, with a real ack between.
	pause_mid_file = true;
	long long id = enqueue_book("bg1", "Busy Gate");
	CHECK(!abs_download_busy(), "a fresh QUEUED job must not hold the power busy bit\n");

	pump_until(id, ABS_DL_FETCHING);
	CHECK(abs_download_busy(), "FETCHING must hold the power decisions (identity fetch on the server)\n");
	pump_until(id, ABS_DL_DOWNLOADING);
	CHECK(abs_download_busy(), "DOWNLOADING must hold the power decisions (file transfer on the card)\n");
	pump_until(id, ABS_DL_PAUSING);
	CHECK(abs_download_busy(), "PAUSING is the honest Stopping state and must stay busy\n");
	pump_until(id, ABS_DL_PAUSED);
	CHECK(!abs_download_busy(), "a fully acknowledged pause must release the power decisions\n");

	CHECK(abs_download_resume(id, err, sizeof(err)), "resume: %s\n", err);
	pump_until(id, ABS_DL_DOWNLOADING);
	CHECK(abs_download_busy(), "the resumed run is active again\n");
	bool reached_finalizing = false;
	for (int i = 0; i < MAX_STEPS && !reached_finalizing; i++) {
		reached_finalizing = job_state(id) == ABS_DL_FINALIZING;
		if (!reached_finalizing) {
			CHECK(abs_download_pump(), "pump must have work left before DONE\n");
		}
	}
	CHECK(reached_finalizing, "the FINALIZING window never appeared\n");
	CHECK(abs_download_busy(), "FINALIZING (sidecars/manifest commit) must stay busy\n");

	run_to_done(id);
	CHECK(!abs_download_busy(), "a terminal drain must release the power decisions\n");
	abs_download_quiesce_storage();
	printf("ok\n");
}

// ---------------------------------------------------------------------------
// 10. Storage lifecycle (§8.5: main.c/system.c/usb.c wiring). Production
//     shape: the worker thread is enabled and no page exists anywhere in the
//     process -- the queue runs from init exactly as boot runs it now. Proven
//     here: quiesce during an active blocked op (wake, worker ack, disarm,
//     no deadlock, and the op's racing classification discarded), quiesce
//     before any init and after an earlier one (the guard shapes), two
//     lifecycle threads racing the same quiesce, and the card-reopen cycle
//     (generation + stale-generation refusal).
// ---------------------------------------------------------------------------

// A lifecycle thread that only calls abs_download_quiesce_storage(): the
// system.c/usb.c/main.c caller shape. Returns through pthread_join.
static void *quiesce_thread(void *arg) {
	(void)arg;
	abs_download_quiesce_storage();
	return NULL;
}

static void test_storage_lifecycle(void) {
	printf("storage lifecycle (boot, quiesce active op, card cycle)... ");
	const char *root = mkcard("storage");
	fake_reset();
	wake_reset();
	const unsigned gen0 = abs_download_card_generation();

	// Boot shape: worker on, media ops armed, nobody driving pumps -- this
	// test never calls abs_download_pump() in thread mode.
	abs_download_config_t cfg = make_config(root, true);
	CHECK(abs_download_init(&cfg), "boot init\n");
	CHECK(abs_download_card_generation() == gen0 + 1, "boot init must raise the generation once\n");
	CHECK(!abs_download_busy(), "an armed but idle queue is not busy\n");

	// A job the enqueue hands the queue completes by the worker alone, with
	// no page present: this binary has no GUI of any kind. It blocks inside
	// the first transfer like a stalled server so the teardown below has an
	// op in flight to wake.
	wake_put_bool(&block_stream_next, true);
	long long id1 = enqueue_book("lc1", "Lifecycle 1");
	wait_flag(&blocked_seen, "the worker to reach the blocked transfer");
	wait_state(id1, ABS_DL_DOWNLOADING, "the active transfer owns the job");
	CHECK(abs_download_busy(), "a blocked op is exactly the busy state the power gate exists for\n");
	char part[1400];
	fake_paths("track01.mp3", ".part", part, sizeof(part));
	CHECK(file_exists(part), "the blocked attempt owns its .part\n");

	// §8.5 system.c/usb.c shape: before the card and its databases are taken
	// away, storage quiesces. The blocked op must be woken through its slot
	// (not waited out), the worker must acknowledge, and this call must
	// return -- bounded, deadlocks would hang below.
	abs_download_quiesce_storage();
	wait_flag(&unblocked_seen, "the teardown to wake the blocked op");
	CHECK(op_thread_closed, "the op thread alone closed its woken stream\n");
	CHECK(!abs_download_busy(), "quiesce must leave the power decisions idle\n");
	abs_download_snapshot_t snap;
	CHECK(!abs_download_snapshot(&snap) && snap.generation == 0,
		  "the queue must be disarmed after quiesce\n");
	CHECK(abs_download_card_generation() == gen0 + 1, "quiesce must not move the generation\n");
	char err[128] = {0};
	abs_download_enqueue_t after = book_req("lc2", "Lifecycle 2", FAKE_DEST);
	CHECK(!abs_download_enqueue_book(&after, NULL, NULL, err, sizeof(err)),
		  "enqueue must refuse while quiesced\n");
	CHECK(strstr(err, "not available") != NULL, "wrong quiesced error: %s\n", err);
	CHECK(file_exists(part), "a STOP_NONE quiesce leaves the orphaned .part for the next attempt\n");

	// The guard shapes: a second quiesce after an earlier one, and one that
	// arrives before any init in this lifecycle (the card-detach event racing
	// the boot init, or a shutdown quiesce behind a detach). Both were unsafe
	// (the wake slot would be destroyed twice) before the quiesce guard.
	abs_download_quiesce_storage();

	// The stale-op answer, deterministically: the job's next attempt is
	// blocked inside the transfer BEFORE the queue is re-armed, then storage
	// tears down twice at once. The op's own classification after the wake
	// says "network" -- a WAITING_NETWORK job -- but the teardown must win
	// over the racing response: the durable row comes back QUEUED, and the
	// reopened generation accepts nothing of the pre-removal attempt. The
	// flag goes up before the re-init so the worker cannot finish the book
	// out underneath the test.
	wake_reset();
	wake_put_bool(&block_stream_next, true);
	CHECK(abs_download_init(&cfg), "re-init after the double quiesce\n");
	CHECK(abs_download_card_generation() == gen0 + 2, "re-init after the card return\n");
	const abs_download_job_t *j = snap_job(id1, &snap);
	CHECK(j->state == ABS_DL_QUEUED, "the quiesce had to persist the active job restart-clean, is %s\n",
		  abs_download_state_name(j->state));
	CHECK(j->error[0] == '\0', "a quiesce must not invent an error: %s\n", j->error);
	abs_download_file_t rows[8];
	CHECK(abs_download_job_files(id1, rows, 8) == 3, "file rows\n");
	CHECK(strcmp(rows[0].state, "pending") == 0, "the writing row must reset for the next attempt\n");

	wait_state(id1, ABS_DL_DOWNLOADING, "the resumed attempt reaches the blocked file");
	wait_flag(&blocked_seen, "the second blocked transfer");
	pthread_t q1, q2;
	CHECK(pthread_create(&q1, NULL, quiesce_thread, NULL) == 0, "quiesce thread 1\n");
	CHECK(pthread_create(&q2, NULL, quiesce_thread, NULL) == 0, "quiesce thread 2\n");
	CHECK(pthread_join(q1, NULL) == 0 && pthread_join(q2, NULL) == 0, "both quiesces must return\n");
	CHECK(abs_download_snapshot(&snap) != true && snap.generation == 0, "quiesced after the joint teardown\n");
	CHECK(abs_download_card_generation() == gen0 + 2, "the joint teardown must not move the generation\n");

	CHECK(abs_download_init(&cfg), "re-init after the joint teardown\n");
	CHECK(abs_download_card_generation() == gen0 + 3, "the third init\n");
	j = snap_job(id1, &snap);
	CHECK(j->state == ABS_DL_QUEUED,
		  "the racing op's network classification must be rejected by the reopen (stopping wins), is %s\n",
		  abs_download_state_name(j->state));

	// The reopened queue finishes the job: the orphaned .part is truncated
	// and the book completes on the new generation.
	wait_state(id1, ABS_DL_DONE, "the reopened queue must finish the resumed book");
	char final1[1400];
	fake_paths("track01.mp3", "", final1, sizeof(final1));
	CHECK(file_bytes(final1) == 42, "the restarted file must be committed whole (%lld bytes)\n",
		  file_bytes(final1));
	CHECK(!file_exists(part), "no .part may survive the committed file\n");
	CHECK(!abs_download_busy(), "busy again false after the drain\n");

	abs_download_quiesce_storage();
	printf("ok\n");
}

// ---------------------------------------------------------------------------

int main(int argc, char **argv) {
	if (argc != 2 || !argv[1][0]) {
		fprintf(stderr, "usage: %s <workdir>\n", argv[0]);
		return 2;
	}
	card = argv[1];
	if (mkdir(card, 0777) != 0 && errno != EEXIST) {
		CHECK(0, "workdir %s: %s\n", card, strerror(errno));
	}

	// Crash scenarios first: children fork while the parent's queue module has
	// never been armed in this process.
	test_crash_download();
	test_crash_pause();
	test_crash_cancel();
	test_crash_finalize();
	test_crash_finalize_fail();

	test_schema();
	test_capacity_dedup();
	test_lifecycle();
	test_no_token();
	test_card_generation();
	test_worker_thread();
	test_wake_pause_during_fetch();
	test_wake_cancel_next_job_ordering();
	test_busy_gate();
	test_storage_lifecycle();

	printf("audiobookshelf-queue-test: queue foundation passed\n");
	return 0;
}
