// Focused §8.2/§8.5 tests for the ABS download queue's real transfer adapter
// (Bob spec §8.2 steps 1–4). The fake-op queue tests in
// tools/audiobookshelf-queue-test.c are untouched; this harness drives the
// REAL media surface from audiobookshelf.c (audiobookshelf_queue_media_ops()).
//
// Two shapes, built from one source:
//
//   * Offline (default): the buffered API surface is a stubbed http_request()
//     answering canned expanded-JSON, so the metadata gates are pinned without
//     HTTP: the explicit >256-track refusal (§8.2 step 2, never silent
//     truncation), cross-origin contentUrl refusal (§11.3), zero-track
//     refusal, destination path overflow / basename-column refusals before any
//     card write, and auth negatives against the in-memory profile store. The
//     stream functions are stubs that poison any probe that touches them —
//     there are no fake transfer callbacks anywhere: every transfer-shaped
//     assertion in the offline binary is a refusal that must decide BEFORE any
//     connection is opened.
//
//   * Live (-DABS_QUEUE_TRANSFER_LIVE): the real HTTP stack (http.c + tls.c,
//     no stubs at all) against tools/audiobookshelf_mock.py, once per
//     checked-in fixture alias. Phases per alias:
//       1. Direct driver: real fetch with the server-hash destination verified
//          against an independently computed SHA-256; a deterministically
//          cancelled transfer (the flag is preset, so the loop's first poll
//          stops with a retained empty .part); the restart-from-zero
//          re-transfer; byte-exact read-backs of both tracks.
//       2. Engine run: a fresh enqueue through abs_download_pump() — real auth
//          step, real fetch, real plan ingestion, real per-file transfer under
//          the queue's own commit gates — where every completed file is
//          RETAINED without one media request, ending at the honest
//          not-wired-yet finalize refusal; Retry redoes finalize only (§8.1).
//       3. Fresh engine transfer under a §8.1 mid-transfer pause: a watcher
//          thread pauses from the RAM byte counter — deterministic, because
//          the counter is monotonic per file and a poll interval of 50 µs can
//          never cover more than one 32 KiB read unit of the 16 MiB track —
//          the PAUSING ack retains the partial .part, and Resume re-downloads
//          it from byte zero to a byte-exact file.
//       4. Negatives: the episode refusal, an unresolvable profile (the auth
//          stage fails before any request), and token-leak scans over the
//          queue database, every card file and the mock's request log.
//
// Queue schema, state-machine and recovery tests remain in the fake-op
// harness; nothing here duplicates them.

#include "src/system/core/sha256.h"
#include "src/system/db/sqlite3.h"
#include "src/system/library/audiobookdb.h"
#include "src/system/net/http.h"
#include "src/system/streaming/abs_download.h"
#include "src/system/streaming/abs_profiles.h"
#include "src/system/streaming/audiobookshelf.h"

#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHECK(cond, ...)                                                    \
	do {                                                                    \
		if (!(cond)) {                                                      \
			fprintf(stderr, "transfer test: CHECK failed %s:%d: %s\n",      \
					__FILE__, __LINE__, #cond);                             \
			fprintf(stderr, __VA_ARGS__);                                   \
			exit(1);                                                        \
		}                                                                   \
	} while (0)

// The sandbox root. Offline keeps everything under argv[1]; each live alias
// run points at <root>/<alias>/, prepared by
// tools/audiobookshelf-queue-transfer-smoke.sh together with the profile-store
// environment (SONIX_AUDIOBOOKSHELF_PROFILES) and the fixture mock.
static char work[900];

#define STUB_ID "stub-book"
#define STUB_SERVER "http://stub.invalid"
#define STUB_TOKEN "transfer-token-AAAA-1"
#define OTHER_TOKEN "transfer-token-BBBB-2"

// ---------------------------------------------------------------------------
// Sandbox helpers
// ---------------------------------------------------------------------------

static char card[900];

static void make_card(void) {
	snprintf(card, sizeof(card), "%.700s/card", work);
	if (mkdir(card, 0777) != 0 && errno != EEXIST) {
		CHECK(0, "cannot create %s: %s\n", card, strerror(errno));
	}
}

static void write_bytes_abs(const char *path, const char *bytes, size_t len) {
	FILE *f = fopen(path, "wb");
	CHECK(f != NULL, "cannot write %s: %s\n", path, strerror(errno));
	if (len > 0) {
		CHECK(fwrite(bytes, 1, len, f) == len, "short write %s\n", path);
	}
	CHECK(fclose(f) == 0, "close %s: %s\n", path, strerror(errno));
}

static void make_file(const char *path, size_t size, char fill) {
	char *buf = malloc(size);
	CHECK(buf != NULL, "oom for %s\n", path);
	memset(buf, fill, size);
	write_bytes_abs(path, buf, size);
	free(buf);
}

static void mkpath_abs(const char *abs) {
	char walk[1400];
	snprintf(walk, sizeof(walk), "%s", abs);
	for (char *p = walk + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			mkdir(walk, 0777);
			*p = '/';
		}
	}
	mkdir(walk, 0777);
}

static bool read_bytes_abs(const char *path, char **out, size_t *len_out) {
	*out = NULL;
	*len_out = 0;
	FILE *f = fopen(path, "rb");
	if (!f) return false;
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return false;
	}
	long size = ftell(f);
	if (size < 0) {
		fclose(f);
		return false;
	}
	rewind(f);
	char *buf = malloc((size_t)size + 1);
	CHECK(buf != NULL, "oom for %s\n", path);
	CHECK(fread(buf, 1, (size_t)size, f) == (size_t)size, "short read %s\n", path);
	fclose(f);
	buf[size] = '\0';
	*out = buf;
	*len_out = (size_t)size;
	return true;
}

static bool file_exists_abs(const char *path) {
	struct stat st;
	return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static long long file_bytes_abs(const char *path) {
	struct stat st;
	if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return -1;
	return (long long)st.st_size;
}

// Reads a work-root file (argv[1]-relative), trimmed of line endings.
static void read_root_file(const char *name, char *out, size_t size) {
	char path[1000];
	snprintf(path, sizeof(path), "%.700s/%.200s", work, name);
	FILE *f = fopen(path, "r");
	CHECK(f != NULL, "%s is missing\n", name);
	char line[ABS_SERVER_MAX + 8];
	CHECK(fgets(line, sizeof(line), f) != NULL, "%s is empty\n", name);
	fclose(f);
	size_t len = strcspn(line, "\r\n");
	CHECK(len > 0 && len < size, "%s unusable\n", name);
	memcpy(out, line, len);
	out[len] = '\0';
}

// Recursively scans every regular file under `root` for `needle`.
static bool tree_contains(const char *root, const char *needle, char *hit, size_t hit_size) {
	DIR *dir = opendir(root);
	if (!dir) return false;
	struct dirent *entry;
	bool found = false;
	while ((entry = readdir(dir)) != NULL && !found) {
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
		char path[1100];
		if ((size_t)snprintf(path, sizeof(path), "%s/%s", root, entry->d_name) >= sizeof(path)) continue;
		struct stat st;
		if (stat(path, &st) != 0) continue;
		if (S_ISDIR(st.st_mode)) {
			found = tree_contains(path, needle, hit, hit_size);
			continue;
		}
		if (!S_ISREG(st.st_mode)) continue;
		char *bytes = NULL;
		size_t len = 0;
		if (!read_bytes_abs(path, &bytes, &len)) continue;
		if (len >= strlen(needle) && memmem(bytes, len, needle, strlen(needle))) {
			found = true;
			if (hit) snprintf(hit, hit_size, "%s", path);
		}
		free(bytes);
	}
	closedir(dir);
	return found;
}

// The §11.5 destination the fetch must produce, hashed independently in this
// harness (the production hash helper is never reused here).
static void expected_destination(const char *server, const char *author, const char *title,
								 char *out_card_relative, size_t size) {
	char hex[SHA256_HEX_LEN];
	sha256_ctx ctx;
	sha256_init(&ctx);
	sha256_update(&ctx, server, strlen(server));
	sha256_final_hex(&ctx, hex);
	snprintf(out_card_relative, size, "Audiobooks/Audiobookshelf/abs-%.12s/%s/%s", hex, author, title);
}

// The absolute card path of "<destination>/<name>".
static void media_path_for(const char *card_root, const char *destination, const char *name,
						   char *out, size_t size) {
	snprintf(out, size, "%.660s/%.500s/%.260s", card_root, destination, name);
}

// ---------------------------------------------------------------------------
// Queue + ops handles shared by both shapes
// ---------------------------------------------------------------------------

static abs_download_ops_t ops_storage;
static abs_download_ops_t *ops;
static char live_server[ABS_SERVER_MAX];
static char live_profile[ABS_PROFILE_ID_MAX];

static void queue_init(void) {
	ops_storage = *audiobookshelf_queue_media_ops();
	ops = &ops_storage;
	abs_download_config_t cfg = {.sd_root = card, .ops = ops, .enable_worker_thread = false};
	CHECK(abs_download_init(&cfg), "queue init failed\n");
}

static void queue_stop(void) {
	abs_download_quiesce_storage();
}

static abs_download_job_t snapshot_job(long long id) {
	abs_download_snapshot_t snap;
	CHECK(abs_download_snapshot(&snap), "snapshot unavailable\n");
	for (int i = 0; i < snap.job_count; i++) {
		if (snap.jobs[i].id == id) return snap.jobs[i];
	}
	CHECK(0, "job %lld vanished from the snapshot\n", id);
	exit(1);
}

static abs_download_state_t job_state(long long id) {
	return snapshot_job(id).state;
}

// Drives the engine until the job reaches a terminal state for the phase
// (paused counts as terminal for the pause phase) or the loop bound trips.
static abs_download_state_t pump_until_terminal(long long job_id) {
	for (int steps = 0; steps < 4096; steps++) {
		if (!abs_download_pump()) break;
		abs_download_job_t j = snapshot_job(job_id);
		switch (j.state) {
		case ABS_DL_DONE:
		case ABS_DL_FAILED:
		case ABS_DL_CANCELLED:
		case ABS_DL_PAUSED:
		case ABS_DL_WAITING_NETWORK:
			return j.state;
		default:
			continue;
		}
	}
	CHECK(0, "job %lld never reached a terminal state\n", job_id);
	exit(1);
}

static long long enqueue_book(const char *profile, const char *server, const char *item,
							  const char *title) {
	abs_download_enqueue_t req;
	memset(&req, 0, sizeof(req));
	req.kind = ABS_DL_KIND_BOOK;
	req.profile_id = profile;
	req.server = server;
	req.library_id = "";
	req.item_id = item;
	req.episode_id = "";
	req.title = title;
	req.destination = "";
	req.remote_progress = -1;
	long long id = 0;
	bool deduped = false;
	char error[256] = {0};
	CHECK(abs_download_enqueue_book(&req, &id, &deduped, error, sizeof(error)),
		  "enqueue failed: %s\n", error);
	CHECK(!deduped, "unexpected dedup for %s\n", item);
	return id;
}

static long long enqueue_episode(const char *profile, const char *server, const char *item,
								 const char *title, const char *destination) {
	abs_download_enqueue_t req;
	memset(&req, 0, sizeof(req));
	req.kind = ABS_DL_KIND_EPISODE;
	req.profile_id = profile;
	req.server = server;
	req.library_id = "";
	req.item_id = item;
	req.episode_id = "ep-1";
	req.title = title;
	req.destination = destination;
	req.remote_progress = -1;
	long long id = 0;
	bool deduped = false;
	char error[256] = {0};
	CHECK(abs_download_enqueue_episode(&req, &id, &deduped, error, sizeof(error)),
		  "episode enqueue failed: %s\n", error);
	return id;
}

// ---------------------------------------------------------------------------
// Shared finalize-wave helpers (§8.2 steps 5–8): manifest reading, the resume
// mapping the row snapshot implies, and the REAL acknowledgement reset proof
// (a planted AUDIOBOOK_ABS_SYNC row that must die with the job's reset and
// survive every later reopen of a DONE row).
// ---------------------------------------------------------------------------

// The .audiobookshelf lines the finalize writes (and local_progress reads),
// parsed back for the assertions. `present` is false when nothing is there.
typedef struct {
	bool present;
	char version[16];
	char item_id[ABS_URL_MAX];
	char server[ABS_SERVER_MAX];
	char duration[32];
	char progress[32];
	char progress_updated[32];
	struct {
		long no;
		double offset;
		double duration;
		char name[ABS_DL_NAME_MAX];
	} tracks[8];
	int track_count;
} manifest_view_t;

static void read_manifest(const char *path, manifest_view_t *m) {
	memset(m, 0, sizeof(*m));
	FILE *f = fopen(path, "r");
	if (!f) {
		return;
	}
	char line[ABS_URL_MAX + 64];
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = '\0';
		if (strncmp(line, "version=", 8) == 0) {
			snprintf(m->version, sizeof(m->version), "%.15s", line + 8);
		} else if (strncmp(line, "item_id=", 8) == 0) {
			snprintf(m->item_id, sizeof(m->item_id), "%.95s", line + 8);
		} else if (strncmp(line, "server=", 7) == 0) {
			snprintf(m->server, sizeof(m->server), "%.511s", line + 7);
		} else if (strncmp(line, "duration=", 9) == 0) {
			snprintf(m->duration, sizeof(m->duration), "%.31s", line + 9);
		} else if (strncmp(line, "progress=", 9) == 0) {
			snprintf(m->progress, sizeof(m->progress), "%.31s", line + 9);
		} else if (strncmp(line, "progress_updated=", 17) == 0) {
			snprintf(m->progress_updated, sizeof(m->progress_updated), "%.31s", line + 17);
		} else if (strncmp(line, "track=", 6) == 0) {
			if (m->track_count < 8) {
				char *second = strchr(line + 6, '\t');
				char *third = second ? strchr(second + 1, '\t') : NULL;
				char *fourth = third ? strchr(third + 1, '\t') : NULL;
				if (second && third && fourth) {
					manifest_view_t *v = m;
					v->tracks[m->track_count].no = atol(line + 6);
					v->tracks[m->track_count].offset = strtod(second + 1, NULL);
					v->tracks[m->track_count].duration = strtod(third + 1, NULL);
					snprintf(v->tracks[m->track_count].name, sizeof(v->tracks[0].name), "%.255s",
							 fourth + 1);
				}
			}
			m->track_count++;
		}
	}
	fclose(f);
	m->present = true;
}

// The resume mapping the job's own remote whole-book snapshot implies over
// the given plan/file records — exactly the algorithm both finalize shapes
// implement (audiobookshelf_resume_target() for the normal window, the row
// rebuild for crash recovery). `finished` books restart at part one.
static void expected_resume(const abs_download_file_req_t *files, int count,
							double remote_progress, int remote_finished, char *name_out,
							size_t name_size, double *seconds_out) {
	double whole = remote_finished ? 0.0 : remote_progress;
	if (!(whole > 0)) {
		whole = 0;
	}
	if (count > 0 && files[count - 1].duration > 0) {
		double total = files[count - 1].start_offset + files[count - 1].duration;
		if (total > 0 && whole > total) {
			whole = total;
		}
	}
	int selected = 0;
	for (int i = 1; i < count; i++) {
		if (files[i].start_offset <= whole + 0.001) {
			selected = i;
		} else {
			break;
		}
	}
	snprintf(name_out, name_size, "%s", files[selected].final_name);
	double relative = whole - files[selected].start_offset;
	if (relative < 0) {
		relative = 0;
	}
	if (files[selected].duration > 0 && relative > files[selected].duration) {
		relative = files[selected].duration;
	}
	if (seconds_out) {
		*seconds_out = relative;
	}
}

// The queue's one active engine step at a time lets the test probe exactly
// between the FINALIZING commit (the barrier opens with it) and the finalize
// op itself (§8.2 step 6).
static void pump_to(long long job_id, abs_download_state_t want) {
	for (int steps = 0; steps < 4096; steps++) {
		abs_download_job_t j = snapshot_job(job_id);
		if (j.state == want) {
			return;
		}
		if (j.state == ABS_DL_DONE || j.state == ABS_DL_FAILED || j.state == ABS_DL_CANCELLED ||
			j.state == ABS_DL_PAUSED || j.state == ABS_DL_WAITING_NETWORK) {
			CHECK(0, "job %lld reached %s before %s\n", job_id, abs_download_state_name(j.state),
				  abs_download_state_name(want));
		}
		CHECK(abs_download_pump(), "the pump starved before %s for job %lld\n",
			  abs_download_state_name(want), job_id);
	}
	CHECK(0, "pump_to(%lld, %s) exhausted\n", job_id, abs_download_state_name(want));
}

// The REAL acknowledgement reset target: one audiobookdb on the work root.
// AUDIOBOOK_ABS_SYNC rows are keyed by the card-resolved folder path; the
// test plants and checks them with raw SQL through the same vendored sqlite.
static char audiobook_db[1000];

static void audiobook_db_open(void) {
	snprintf(audiobook_db, sizeof(audiobook_db), "%.990s/audiobooks.db", work);
	CHECK(audiobookdb_open(audiobook_db), "audiobookdb_open failed\n");
	// The import/gate phases poll the durable rows from their own connections
	// while the module (and the progress worker) write. Rollback-journal mode
	// makes every such read a shared lock a writer's COMMIT can collide with
	// (SQLITE_BUSY both ways); WAL takes that interleaving out of the harness.
	// File-local, test-sandbox-only: the device index keeps its default mode.
	sqlite3 *raw = NULL;
	if (sqlite3_open(audiobook_db, &raw) == SQLITE_OK) {
		sqlite3_exec(raw, "PRAGMA journal_mode=WAL", NULL, NULL, NULL);
		sqlite3_close(raw);
	}
}

// One raw read with the two retry steps the interleaving above can still
// produce (a checkpoint being cut): callers see the settled state, not a
// transient BUSY.
static bool audiobook_sqlite_step_retry(sqlite3_stmt *stmt, bool *row) {
	for (int try = 0; try < 3; try++) {
		int rc = sqlite3_step(stmt);
		if (rc == SQLITE_ROW || rc == SQLITE_DONE) {
			*row = rc == SQLITE_ROW;
			return true;
		}
		usleep(5000);
	}
	return false;
}

static void checkpoint_plant(const char *abs_book) {
	sqlite3 *raw = NULL;
	CHECK(sqlite3_open(audiobook_db, &raw) == SQLITE_OK, "checkpoint plant open\n");
	sqlite3_stmt *stmt = NULL;
	CHECK(sqlite3_prepare_v2(raw, "INSERT OR REPLACE INTO AUDIOBOOK_ABS_SYNC"
							  "(book,server,linked,file,seconds,finished,attempted,failed)"
							  " VALUES(?1,'plant',1,'f',3.0,0,0,0)",
						  -1, &stmt, NULL) == SQLITE_OK,
		  "checkpoint plant prepare\n");
	sqlite3_bind_text(stmt, 1, abs_book, -1, SQLITE_TRANSIENT);
	CHECK(sqlite3_step(stmt) == SQLITE_DONE, "checkpoint plant step\n");
	sqlite3_finalize(stmt);
	sqlite3_close(raw);
}

static bool checkpoint_present(const char *abs_book) {
	sqlite3 *raw = NULL;
	bool present = false;
	if (sqlite3_open(audiobook_db, &raw) == SQLITE_OK) {
		sqlite3_stmt *stmt = NULL;
		if (sqlite3_prepare_v2(raw, "SELECT 1 FROM AUDIOBOOK_ABS_SYNC WHERE book=?1", -1, &stmt,
							   NULL) == SQLITE_OK) {
			sqlite3_bind_text(stmt, 1, abs_book, -1, SQLITE_TRANSIENT);
			bool row = false;
			if (audiobook_sqlite_step_retry(stmt, &row)) {
				present = row;
			}
			sqlite3_finalize(stmt);
		}
		sqlite3_close(raw);
	}
	return present;
}

// §8.3 import assertions, read straight back from the durable rows: what the
// coordinator wrote for a book (AUDIOBOOK_TABLE resume) and what its
// compare-and-swap acknowledgement row carries (AUDIOBOOK_ABS_SYNC).
static bool audiobook_resume_row(const char *abs_book, char *file_out, size_t file_size,
								 double *seconds_out, bool *finished_out) {
	sqlite3 *raw = NULL;
	bool found = false;
	if (sqlite3_open(audiobook_db, &raw) == SQLITE_OK) {
		sqlite3_stmt *stmt = NULL;
		if (sqlite3_prepare_v2(raw, "SELECT resume_file,resume_pos,resume_completed FROM"
								   " AUDIOBOOK_TABLE WHERE path=?1",
							   -1, &stmt, NULL) == SQLITE_OK) {
			sqlite3_bind_text(stmt, 1, abs_book, -1, SQLITE_TRANSIENT);
			bool row = false;
			if (audiobook_sqlite_step_retry(stmt, &row) && row) {
				snprintf(file_out, file_size, "%s", (const char *)sqlite3_column_text(stmt, 0));
				*seconds_out = sqlite3_column_double(stmt, 1);
				*finished_out = sqlite3_column_int(stmt, 2) != 0;
				found = true;
			}
			sqlite3_finalize(stmt);
		}
		sqlite3_close(raw);
	}
	return found;
}

static bool audiobook_sync_row(const char *abs_book, char *server_out, size_t server_size,
							   char *file_out, size_t file_size, double *seconds_out,
							   bool *finished_out, long long *server_updated_ms_out,
							   int *linked_out) {
	sqlite3 *raw = NULL;
	bool found = false;
	if (sqlite3_open(audiobook_db, &raw) == SQLITE_OK) {
		sqlite3_stmt *stmt = NULL;
		if (sqlite3_prepare_v2(raw, "SELECT server,file,seconds,finished,server_updated_ms,linked"
								   " FROM AUDIOBOOK_ABS_SYNC WHERE book=?1",
							   -1, &stmt, NULL) == SQLITE_OK) {
			sqlite3_bind_text(stmt, 1, abs_book, -1, SQLITE_TRANSIENT);
			bool row = false;
			if (audiobook_sqlite_step_retry(stmt, &row) && row) {
				snprintf(server_out, server_size, "%s", (const char *)sqlite3_column_text(stmt, 0));
				snprintf(file_out, file_size, "%s",
						 sqlite3_column_text(stmt, 1) ? (const char *)sqlite3_column_text(stmt, 1) : "");
				*seconds_out = sqlite3_column_double(stmt, 2);
				*finished_out = sqlite3_column_int(stmt, 3) != 0;
				*server_updated_ms_out = sqlite3_column_int64(stmt, 4);
				*linked_out = sqlite3_column_int(stmt, 5) != 0;
				found = true;
			}
			sqlite3_finalize(stmt);
		}
		sqlite3_close(raw);
	}
	return found;
}

// Bounded wait for the queue mirror to clear index_pending (the scan thread
// runs the coordinator's import pass inside its notification, so the caller
// cannot observe the clear synchronously).
static void wait_index_pending_cleared(const char *item, long long *job_out) {
	for (int i = 0; i < 4000; i++) {
		abs_download_snapshot_t snap;
		CHECK(abs_download_snapshot(&snap), "snapshot during import wait\n");
		bool any_pending = false;
		for (int k = 0; k < snap.job_count; k++) {
			if (strcmp(snap.jobs[k].item_id, item) == 0) {
				*job_out = snap.jobs[k].id;
				if (snap.jobs[k].state == ABS_DL_DONE && snap.jobs[k].index_pending) {
					any_pending = true;
				}
			}
		}
		if (!any_pending) {
			return;
		}
		usleep(1000);
	}
	CHECK(0, "index_pending never cleared for %s\n", item);
}

// A crash child re-enters the card in a fresh process. The finalize-wave
// offline bodies below define their own per-phase crash children; the shared
// region stays HTTP-free.


// ---------------------------------------------------------------------------
// Offline shape: canned expanded JSON, real files, real engine steps
// ---------------------------------------------------------------------------

#ifndef ABS_QUEUE_TRANSFER_LIVE

// Canned expanded JSON the stub answers.
static char stub_body[262144];
static size_t stub_len;
static char stub_url[2048];
static bool stream_was_reached; // stream stubs must never run offline
// Stub bookkeeping: how many buffered API requests the stub served, and which
// cover URL (if any) reached the poisoned stream surface.
static long long stub_requests;
static bool cover_stream_seen;
static char cover_stream_url[2048];

static void stub_reset(void) {
	stub_len = 0;
	stub_body[0] = '\0';
	stub_url[0] = '\0';
	stream_was_reached = false;
	cover_stream_seen = false;
	cover_stream_url[0] = '\0';
	stub_requests = 0;
}

static void stub_append(const char *text) {
	size_t len = strlen(text);
	CHECK(stub_len + len + 1 <= sizeof(stub_body), "stub body overflow\n");
	memcpy(stub_body + stub_len, text, len + 1);
	stub_len += len;
}

// Builds one expanded item. `sizes[i] >= 0` emits a metadata size; -1 keeps
// the field absent (the queue must treat that as unknown). `absolute_urls`
// makes every contentUrl cross-origin. `name_pad` pads the filename with 'N'
// bytes for the basename-column probe; 0 uses "Part <n>.mp3".
static void build_expanded(const char *id, const char *title, const char *author, int tracks,
						   const long long *sizes, bool absolute_urls, bool with_progress,
						   int name_pad) {
	stub_reset();
	char piece[700];
	snprintf(piece, sizeof(piece),
			 "{\"id\":\"%s\",\"media\":{\"duration\":200.0,\"coverPath\":\"/api/items/%s/cover\","
			 "\"metadata\":{\"title\":\"%s\",\"authorName\":\"%s\","
			 "\"description\":\"Stub words about the canned book.\"},\"tracks\":[",
			 id, id, title, author);
	stub_append(piece);
	for (int i = 0; i < tracks; i++) {
		if (i) stub_append(",");
		snprintf(piece, sizeof(piece),
				 "{\"index\":%d,\"startOffset\":%d.0,\"duration\":10.0,\"title\":\"Part %d\","
				 "\"contentUrl\":\"%s/s/%s/file/%d\",\"mimeType\":\"audio/mpeg\","
				 "\"metadata\":{\"filename\":\"",
				 i + 1, i * 10, i + 1, absolute_urls ? "http://evil.invalid" : "", id, i + 1);
		stub_append(piece);
		if (name_pad > 0) {
			for (int k = 0; k < name_pad; k++) stub_append("N");
			stub_append(".mp3");
		} else {
			snprintf(piece, sizeof(piece), "Part %d.mp3", i + 1);
			stub_append(piece);
		}
		stub_append("\"");
		if (sizes && sizes[i] >= 0) {
			snprintf(piece, sizeof(piece), ",\"size\":%lld", sizes[i]);
			stub_append(piece);
		}
		stub_append("}");
		stub_append("}");
	}
	stub_append("]}"); // tracks array + media object
	if (with_progress) {
		stub_append(",\"userMediaProgress\":{\"currentTime\":13.0,\"isFinished\":false,"
					"\"lastUpdate\":1700000000000}");
	}
	stub_append("}");
}

bool http_request(const char *url, const http_req_t *req, char **out, size_t *out_len,
				  size_t limit, int timeout_secs, int *status_out) {
	(void)req;
	(void)limit;
	(void)timeout_secs;
	stub_requests++;
	snprintf(stub_url, sizeof(stub_url), "%s", url ? url : "");
	if (status_out) *status_out = 200;
	char *body = malloc(stub_len + 1);
	CHECK(body != NULL, "oom for stub body\n");
	memcpy(body, stub_body, stub_len + 1);
	*out = body;
	if (out_len) *out_len = stub_len;
	return true;
}

// The stream surface stays poisoned for every offline probe EXCEPT the cover
// sidecar of the finalize path: its failure is the §8.2 cover rule under test
// (cover.jpg is the only NONFATAL sidecar). Any probe that opens a stream for
// real media is still the broken decision it always was — and now it is loud.
bool http_stream_open_ex(http_stream_t *st, const char *url, const char *extra_headers,
						 int timeout_secs) {
	(void)st;
	(void)extra_headers;
	(void)timeout_secs;
	if (url && strstr(url, "/cover")) {
		cover_stream_seen = true;
		snprintf(cover_stream_url, sizeof(cover_stream_url), "%s", url);
		return false;
	}
	stream_was_reached = true;
	fprintf(stderr, "transfer test: offline probe touched http_stream_open_ex (%s)\n",
			url ? url : "");
	abort(); // loud: a transfer must decide before it ever opens a stream
}

int http_stream_read(http_stream_t *st, void *buf, int len) {
	(void)st;
	(void)buf;
	(void)len;
	stream_was_reached = true;
	return -1;
}

// Link-level delegates for the op-aware entry points (the wake wave, §8.5
// line 1028): the offline probes never reach an open, so the stubs simply
// carry the poisoned behavior above across the new symbols.
bool http_stream_open_ex_op(http_stream_t *st, const char *url, const char *extra_headers,
							int timeout_secs, http_op_t *op, int *status_out) {
	(void)op;
	(void)status_out;
	return http_stream_open_ex(st, url, extra_headers, timeout_secs);
}

int http_stream_read_op(http_stream_t *st, void *buf, int len, http_op_t *op) {
	(void)op;
	return http_stream_read(st, buf, len);
}

void http_stream_close(http_stream_t *st) {
	(void)st;
}

const char *http_last_error(void) {
	return "offline stub: the stream refused to open";
}

// §9.5 plumbing: the offline stub never truncates anything, so its signal is
// a permanent false; the real http.c provides it in the live build.
bool http_last_truncated(void) { return false; }

static abs_auth_snapshot_t held;
static volatile int offline_cancel;
static abs_download_plan_t offline_plan_out;
static char offline_error[512];
static const char *offline_stage;
// The crash-child runner and finalize-wave phase cards (§8.2 steps 5–8),
// defined below with their phase bodies.
static void crash_child_finalize(bool write_manifest, bool keep_files);
static void run_crash_child(bool write_manifest, bool keep_files);
static void phase_card(const char *name);
static void offline_create_media(void);

// auth_acquire + fetch_identity, exactly the queue engine's calling shape.
static bool offline_fetch(void) {
	memset(&offline_plan_out, 0, sizeof(offline_plan_out));
	offline_stage = "(none)";
	offline_error[0] = '\0';
	if (!ops->auth_acquire(NULL, "p1", STUB_SERVER, &held)) {
		offline_stage = "auth";
		snprintf(offline_error, sizeof(offline_error), "auth acquire refused");
		return false;
	}
	abs_download_op_ctx_t ctx = {.job_id = 1, .state = ABS_DL_FETCHING, .cancel = &offline_cancel,
								 .user = NULL};
	abs_download_job_t job;
	memset(&job, 0, sizeof(job));
	job.id = 1;
	snprintf(job.kind, sizeof(job.kind), "%s", ABS_DL_KIND_BOOK);
	snprintf(job.profile_id, sizeof(job.profile_id), "p1");
	snprintf(job.server, sizeof(job.server), "%s", STUB_SERVER);
	snprintf(job.item_id, sizeof(job.item_id), "%s", STUB_ID);
	job.state = ABS_DL_FETCHING;
	return ops->fetch_identity(NULL, &ctx, &job, &offline_plan_out, &offline_stage, offline_error,
							   sizeof(offline_error));
}

// file_download alone: the caller owns the attempt's auth (the engine runs
// one auth per attempt, then fetch, then the file ops).
static bool offline_transfer(long long job_id, const abs_download_file_t *row, long long *size_out) {
	offline_stage = "(none)";
	offline_error[0] = '\0';
	abs_download_op_ctx_t ctx = {.job_id = job_id, .state = ABS_DL_DOWNLOADING,
								 .cancel = &offline_cancel, .user = NULL};
	return ops->file_download(NULL, &ctx, row, size_out, &offline_stage, offline_error,
							  sizeof(offline_error));
}

// Fresh per-phase card directory: every phase arms its own queue on an empty
// sandbox (a crash child needs one nobody armed before the fork).
static void phase_card(const char *name) {
	snprintf(card, sizeof(card), "%.700s/%.180s", work, name);
	if (mkdir(card, 0777) != 0 && errno != EEXIST) {
		CHECK(0, "cannot create %s: %s\n", card, strerror(errno));
	}
}

// §8.2 material for the finalize phases: the canned expanded JSON (2 tracks,
// known sizes, whole-book progress 13 s stamped 1700000000000) and the audio
// already on the card at the exact sizes, so the queue's retention path closes
// the loop without a single connection.
static void offline_create_media(void) {
	long long sizes[2] = {1200, 1200};
	build_expanded(STUB_ID, "Stub Book", "Stub Author", 2, sizes, false, true, 0);
	char want_dest[600];
	expected_destination(STUB_SERVER, "Stub Author", "Stub Book", want_dest, sizeof(want_dest));
	char dest_abs[1400];
	snprintf(dest_abs, sizeof(dest_abs), "%.700s/%.599s", card, want_dest);
	mkpath_abs(dest_abs);
	char final1[1400];
	media_path_for(card, want_dest, "001 - Part 1.mp3", final1, sizeof(final1));
	make_file(final1, 1200, 'k');
	char final2[1400];
	media_path_for(card, want_dest, "002 - Part 2.mp3", final2, sizeof(final2));
	make_file(final2, 1200, 'k');
}

// A crash child: the engine drives the job to the FINALIZING commit and then
// dies at a chosen point, so the parent opens a database whose reconciliation
// the real recovery path must complete.
static void crash_child_finalize(bool write_manifest, bool keep_files) {
	queue_init();
	offline_create_media();
	long long id = enqueue_book("p1", STUB_SERVER, STUB_ID, "Stub Book");
	CHECK(id > 0, "crash child enqueue failed\n");
	pump_to(id, ABS_DL_FINALIZING);
	if (write_manifest) {
		// The real finalize op runs while the attempt state is still held —
		// the §8.2 step 7 write — and then the process dies before any
		// acknowledgement: rows FINALIZING, manifest committed, no reset.
		abs_download_job_t job = snapshot_job(id);
		abs_download_file_t files[ABS_DL_FILE_CAP];
		int n = abs_download_job_files(id, files, ABS_DL_FILE_CAP);
		CHECK(n == 2, "crash child file rows %d\n", n);
		abs_download_op_ctx_t ctx = {.job_id = id, .state = ABS_DL_FINALIZING,
									 .cancel = &offline_cancel, .user = NULL};
		abs_download_resume_t resume;
		memset(&resume, 0, sizeof(resume));
		resume.resume_seconds = -1;
		const char *stage = "";
		char error[256] = {0};
		CHECK(ops->finalize(NULL, &ctx, &job, files, n, &resume, &stage, error, sizeof(error)),
			  "crash child finalize failed: %s (%s)\n", error, stage);
		CHECK(resume.resume_file[0] != '\0', "crash child resume mapping empty\n");
	} else if (!keep_files) {
		// A file lost after its commit record: recovery must refuse to fake a
		// commit over recorded-complete-but-gone audio (§8.2 file rules).
		char want_dest[600];
		expected_destination(STUB_SERVER, "Stub Author", "Stub Book", want_dest, sizeof(want_dest));
		char final2[1400];
		media_path_for(card, want_dest, "002 - Part 2.mp3", final2, sizeof(final2));
		CHECK(unlink(final2) == 0, "crash child could not drop the second file\n");
	}
	_exit(80); // the closest a test can come to power being pulled
}

static void run_crash_child(bool write_manifest, bool keep_files) {
	pid_t pid = fork();
	CHECK(pid >= 0, "fork failed: %s\n", strerror(errno));
	if (pid == 0) {
		crash_child_finalize(write_manifest, keep_files); // _exits
		_exit(99);
	}
	int status = 0;
	CHECK(waitpid(pid, &status, 0) == pid, "waitpid failed\n");
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 80,
		  "crash child exited %d, wanted a clean _exit(80)\n",
		  WIFEXITED(status) ? WEXITSTATUS(status) : -1);
}

static void offline_run(void) {
	abs_profiles_init();
	char id_out[ABS_PROFILE_ID_MAX];
	CHECK(abs_profiles_add("Transfer A", STUB_SERVER, STUB_TOKEN, id_out, sizeof(id_out)),
		  "cannot add profile A\n");
	char id_b[ABS_PROFILE_ID_MAX];
	CHECK(abs_profiles_add("Transfer B", "http://other.invalid", OTHER_TOKEN, id_b, sizeof(id_b)),
		  "cannot add profile B\n");
	CHECK(strcmp(id_b, "p2") == 0, "unexpected second profile id %s\n", id_b);

	make_card();
	queue_init();

	// --- auth negatives: another profile's token never resolves across
	// --- servers; a refused attempt leaves zero token bytes behind.
	abs_auth_snapshot_t probe;
	memset(&probe, 0, sizeof(probe));
	char resolve_error[256];
	CHECK(!audiobookshelf_queue_auth_resolve("p2", STUB_SERVER, &probe, resolve_error,
											 sizeof(resolve_error)),
		  "p2's token must not resolve for the stub server\n");
	CHECK(probe.token[0] == '\0', "the refused snapshot leaked token bytes\n");
	CHECK(resolve_error[0] != '\0', "the refusal carries no reason\n");
	CHECK(!audiobookshelf_queue_auth_resolve("p9", STUB_SERVER, &probe, resolve_error,
											 sizeof(resolve_error)),
		  "an unknown profile must not resolve\n");
	CHECK(probe.token[0] == '\0', "the unknown-profile refusal leaked bytes\n");

	// --- explicit >256-track refusal (§8.2 step 2) ---------------------------
	long long sizes[258];
	for (int i = 0; i < 258; i++) sizes[i] = 1000;
	build_expanded(STUB_ID, "Stub Book", "Stub Author", 257, sizes, false, true, 0);
	CHECK(!offline_fetch(), "a 257-track item must be refused\n");
	CHECK(offline_stage && strcmp(offline_stage, "metadata") == 0, "stage %s\n", offline_stage);
	CHECK(strstr(offline_error, "more than 256") != NULL, "unsupported error missing: %s\n",
		  offline_error);
	CHECK(strstr(stub_url, "/api/items/stub-book?expanded=1&include=progress,authors") != NULL,
		  "the expanded identity request is missing: %s\n", stub_url);
	{
		char audiobooks[1000];
		snprintf(audiobooks, sizeof(audiobooks), "%.700s/Audiobooks", card);
		struct stat st;
		CHECK(stat(audiobooks, &st) != 0, "a refused 257-track fetch wrote to the card\n");
	}
	ops->auth_release(NULL, &held);

	// --- cross-origin contentUrl refusal (§11.3) ------------------------------
	build_expanded(STUB_ID, "Stub Book", "Stub Author", 2, NULL, true, true, 0);
	CHECK(!offline_fetch(), "cross-origin media must be refused\n");
	CHECK(strcmp(offline_stage, "metadata") == 0, "stage %s\n", offline_stage);
	CHECK(strstr(offline_error, "hosted by another server") != NULL, "origin refusal missing: %s\n",
		  offline_error);
	ops->auth_release(NULL, &held);

	// --- zero-track refusal ---------------------------------------------------
	build_expanded(STUB_ID, "Stub Book", "Stub Author", 0, NULL, false, true, 0);
	CHECK(!offline_fetch(), "a trackless item must be refused\n");
	CHECK(strstr(offline_error, "no downloadable audio tracks") != NULL,
		  "trackless refusal missing: %s\n", offline_error);
	ops->auth_release(NULL, &held);

	// --- destination path overflow before any card write (§8.2 step 3) --------
	// A safe-component author/title plus a padded basename push the per-track
	// absolute path over the shared 512-byte consumer ceiling while the
	// basename itself stays inside its column: this isolates the §11.5 path
	// gate from the column gate probed next.
	char big_author[140];
	memset(big_author, 'L', sizeof(big_author) - 1);
	big_author[sizeof(big_author) - 1] = '\0';
	char big_title[200];
	memset(big_title, 'T', sizeof(big_title) - 1);
	big_title[sizeof(big_title) - 1] = '\0';
	build_expanded(STUB_ID, big_title, big_author, 2, NULL, false, true, 240);
	CHECK(!offline_fetch(), "an overflowing destination must be refused\n");
	CHECK(strstr(offline_error, "too long") != NULL, "overflow refusal missing: %s\n", offline_error);
	ops->auth_release(NULL, &held);

	// --- oversized basename: the legacy naming clamp keeps parity -----------
	// local_track_name() bounds its component at 239 bytes ("001 - " + one
	// component + NUL can never pass the 256-byte DOWNLOAD_FILE column), so a
	// server filename longer than that is clipped exactly the way the legacy
	// 0.5.0 download named its files. The fetch must accept it with the
	// identical, byte-stable name.
	build_expanded(STUB_ID, "Stub Book", "Stub Author", 2, NULL, false, true, 250);
	CHECK(offline_fetch(), "an oversized server filename must still fetch\n");
	{
		char clamped[280] = "001 - ";
		memset(clamped + 6, 'N', 239);
		clamped[6 + 239] = '\0';
		CHECK(offline_plan_out.files[0].final_name &&
				  strcmp(offline_plan_out.files[0].final_name, clamped) == 0,
			  "clamp mismatch: %.290s\n", offline_plan_out.files[0].final_name);
	}
	ops->auth_release(NULL, &held);
	{
		char audiobooks[1000];
		snprintf(audiobooks, sizeof(audiobooks), "%.700s/Audiobooks", card);
		struct stat st;
		CHECK(stat(audiobooks, &st) != 0, "the overflow probes left card residue\n");
	}
	ops->auth_release(NULL, &held);

	// --- positive fetch: plan shape, server-hash destination ------------------
	long long pair_sizes[2] = {1200, 1200};
	build_expanded(STUB_ID, "Stub Book", "Stub Author", 2, pair_sizes, false, true, 0);
	CHECK(offline_fetch(), "the plain fetch failed: %s (%s)\n", offline_error, offline_stage);
	abs_download_plan_t *plan = &offline_plan_out;
	CHECK(plan->file_count == 2, "plan file count %d\n", plan->file_count);
	char want_dest[600];
	expected_destination(STUB_SERVER, "Stub Author", "Stub Book", want_dest, sizeof(want_dest));
	CHECK(plan->destination && strcmp(plan->destination, want_dest) == 0,
		  "destination mismatch:\n got %s\n want %s\n", plan->destination, want_dest);
	CHECK(strcmp(plan->files[0].final_name, "001 - Part 1.mp3") == 0, "name0 %s\n",
		  plan->files[0].final_name);
	CHECK(strcmp(plan->files[1].final_name, "002 - Part 2.mp3") == 0, "name1 %s\n",
		  plan->files[1].final_name);
	char url1[600];
	snprintf(url1, sizeof(url1), "%s/s/stub-book/file/1", STUB_SERVER);
	char url2[600];
	snprintf(url2, sizeof(url2), "%s/s/stub-book/file/2", STUB_SERVER);
	CHECK(strcmp(plan->files[0].source_url, url1) == 0, "url0 %s\n", plan->files[0].source_url);
	CHECK(strcmp(plan->files[1].source_url, url2) == 0, "url1 %s\n", plan->files[1].source_url);
	CHECK(plan->files[0].expected_size == 1200 && plan->files[1].expected_size == 1200,
		  "expected sizes %lld/%lld\n", plan->files[0].expected_size, plan->files[1].expected_size);
	CHECK(plan->files[0].start_offset == 0.0 && plan->files[1].start_offset == 10.0, "offsets\n");
	CHECK(plan->remote_progress == 13.0, "remote progress %f\n", plan->remote_progress);
	CHECK(!plan->remote_finished && plan->remote_updated_ms == 1700000000000, "remote stamp\n");
	{
		char audiobooks[1000];
		snprintf(audiobooks, sizeof(audiobooks), "%.700s/Audiobooks", card);
		struct stat st;
		CHECK(stat(audiobooks, &st) != 0, "the fetch touched the card before its transfer\n");
	}

	// --- transfer gates in the fetch's own attempt (job id 1) ----------------
	// The stream stubs poison any probe that ever reaches a connection: each
	// of these refusals and the retention path must decide before that.
	abs_download_file_t row1;
	memset(&row1, 0, sizeof(row1));
	row1.job_id = 1;
	row1.ordinal = 1;
	snprintf(row1.source_url, sizeof(row1.source_url), "%s", url1);
	snprintf(row1.final_name, sizeof(row1.final_name), "001 - Part 1.mp3");
	row1.expected_size = 1200;

	abs_download_file_t rowx = row1;
	snprintf(rowx.source_url, sizeof(rowx.source_url), "http://evil.invalid/x.mp3");
	long long size_out = -1;
	CHECK(!offline_transfer(row1.job_id, &rowx, &size_out),
		  "a cross-origin row must be refused by the transfer\n");
	CHECK(stream_was_reached == false, "the origin gate must decide before the stream\n");
	CHECK(strstr(offline_error, "another server") != NULL, "origin message missing: %s\n",
		  offline_error);

	// An existing final file of the exact size is retained without a stream.
	char dest_abs[1400];
	snprintf(dest_abs, sizeof(dest_abs), "%.700s/%.599s", card, want_dest);
	mkpath_abs(dest_abs);
	char final1[1400];
	snprintf(final1, sizeof(final1), "%.1100s/001 - Part 1.mp3", dest_abs);
	make_file(final1, 1200, 'k');
	size_out = -1;
	CHECK(offline_transfer(row1.job_id, &row1, &size_out),
		  "an exact existing final must be retained: %s\n", offline_error);
	CHECK(stream_was_reached == false, "retention must not open a stream\n");
	CHECK(size_out == 1200, "retained size %lld\n", size_out);

	// A differently sized existing file is a conflict, not a skip.
	char final2[1400];
	snprintf(final2, sizeof(final2), "%.1100s/002 - Part 2.mp3", dest_abs);
	make_file(final2, 601, 'j');
	abs_download_file_t row2 = row1;
	row2.ordinal = 2;
	snprintf(row2.final_name, sizeof(row2.final_name), "002 - Part 2.mp3");
	size_out = -1;
	CHECK(!offline_transfer(row1.job_id, &row2, &size_out),
		  "a wrong-size existing file must be refused\n");
	CHECK(strstr(offline_error, "differently sized") != NULL, "conflict message missing: %s\n",
		  offline_error);
	ops->auth_release(NULL, &held);

	// --- forget semantics: the attempt dies with the release ------------------
	// A fetch always re-acquires its own snapshot; a stray transfer op cannot
	// ride on a released attempt and must refuse at the auth stage.
	build_expanded(STUB_ID, "Stub Book", "Stub Author", 1, NULL, false, false, 0);
	size_out = -1;
	CHECK(!offline_transfer(row1.job_id, &row1, &size_out),
		  "a transfer after auth_release must refuse\n");
	CHECK(offline_stage && strcmp(offline_stage, "auth") == 0, "stage %s\n", offline_stage);

	queue_stop();
	printf("transfer test: offline gates passed\n");
}

// ---------------------------------------------------------------------------
// Offline finalize-wave phases (§8.2 steps 5–8). One phase per fresh card:
// the engine runs the real adapter end to end with pre-created audio (the
// retention loop needs no stream) and the canned-JSON stub answers the
// expanded identity fetches. The stream stub only ever opens — and refuses —
// the cover URL, which simultaneously pins the cover-is-nonfatal rule.
// ---------------------------------------------------------------------------

// The destination dir + absolute card paths the phases share.
static void offline_paths(char *want_dest, size_t dest_size, char *dest_abs, size_t abs_size,
						  char *final1, size_t f1_size, char *final2, size_t f2_size) {
	expected_destination(STUB_SERVER, "Stub Author", "Stub Book", want_dest, dest_size);
	snprintf(dest_abs, abs_size, "%.700s/%.599s", card, want_dest);
	media_path_for(card, want_dest, "001 - Part 1.mp3", final1, f1_size);
	media_path_for(card, want_dest, "002 - Part 2.mp3", final2, f2_size);
}

static long long offline_enqueue(void) {
	return enqueue_book("p1", STUB_SERVER, STUB_ID, "Stub Book");
}

// Phase A: full book DONE end to end — audio + sidecars + .tags/.description
// + (refused) cover + manifest + real audiobookdb reset + the DONE
// transaction; the sync barrier is visible from the FINALIZING commit and is
// released only by the DONE commit.
static void offline_phase_done(void) {
	printf("transfer test: offline DONE end to end... ");
	phase_card("fin-done");
	audiobook_db_open();
	offline_create_media();
	char want_dest[600], dest_abs[1400], final1[1400], final2[1400];
	offline_paths(want_dest, sizeof(want_dest), dest_abs, sizeof(dest_abs), final1, sizeof(final1),
				  final2, sizeof(final2));
	char manifest[1400];
	snprintf(manifest, sizeof(manifest), "%.1300s/.audiobookshelf", dest_abs);
	char abs_book[1400];
	snprintf(abs_book, sizeof(abs_book), "%s", dest_abs); // the scanner's key form
	checkpoint_plant(abs_book);

	queue_init();
	long long id = offline_enqueue();
	pump_to(id, ABS_DL_FINALIZING);
	// §8.2 step 6: the barrier opens with the FINALIZING commit, BEFORE the
	// manifest (nothing is exposed yet).
	CHECK(!file_exists_abs(manifest), "a manifest appeared before the finalize op\n");
	CHECK(abs_download_sync_blocked(want_dest), "barrier during FINALIZING (relative)\n");
	char abs_probe[1400];
	snprintf(abs_probe, sizeof(abs_probe), "%s", final1); // any file under the destination
	CHECK(abs_download_sync_blocked(abs_probe), "barrier during FINALIZING (absolute)\n");
	CHECK(pump_until_terminal(id) == ABS_DL_DONE, "the job must finish DONE\n");

	abs_download_job_t j = snapshot_job(id);
	CHECK(j.error[0] == '\0', "DONE carries an error: %s\n", j.error);
	CHECK(j.ack_reset_done == 1 && j.index_pending == 1, "DONE flags ack %d index %d\n",
		  j.ack_reset_done, j.index_pending);
	CHECK(fabs(j.remote_progress - 13.0) < 1e-9, "remote progress %f\n", j.remote_progress);
	// §8.2 step 5: the resume mapping persisted part-relative, exactly what
	// audiobookshelf_resume_target() maps (13 s into a 10 s first part -> part
	// two at 3 s).
	CHECK(strcmp(j.resume_file, "002 - Part 2.mp3") == 0, "resume file %.260s\n", j.resume_file);
	CHECK(fabs(j.resume_seconds - 3.0) < 1e-9, "resume seconds %f\n", j.resume_seconds);
	{
		abs_download_file_t rows[8];
		int n = abs_download_job_files(id, rows, 8);
		CHECK(n == 2, "rows %d\n", n);
		for (int i = 0; i < 2; i++) {
			CHECK(strcmp(rows[i].state, "complete") == 0, "row %d state %s\n", i, rows[i].state);
			CHECK(rows[i].final_size == 1200, "row %d size %lld\n", i, rows[i].final_size);
		}
	}
	// Sidecars: both tracks carry .tags with the legacy fields and ordering;
	// the canned description rode the metadata, so part one carries it.
	char tags1[1400], tags2[1400], description[1400];
	snprintf(tags1, sizeof(tags1), "%.1300s.tags", final1);
	snprintf(tags2, sizeof(tags2), "%.1300s.tags", final2);
	snprintf(description, sizeof(description), "%.1300s.description", final1);
	char *bytes = NULL;
	size_t len = 0;
	CHECK(read_bytes_abs(tags1, &bytes, &len), "track-one .tags missing\n");
	CHECK(strstr(bytes, "title=Part 1\n") && strstr(bytes, "artist=Stub Author\n") &&
			  strstr(bytes, "album=Stub Book\n") && strstr(bytes, "track=1\n") &&
			  strstr(bytes, "abs_item_id=stub-book\n") && strstr(bytes, "series=\n") &&
			  strstr(bytes, "series_part=\n"),
		  "track-one .tags fields wrong: %s\n", bytes);
	free(bytes);
	CHECK(read_bytes_abs(tags2, &bytes, &len), "track-two .tags missing\n");
	CHECK(strstr(bytes, "track=2\n") != NULL, "track-two numbering wrong: %s\n", bytes);
	free(bytes);
	CHECK(read_bytes_abs(description, &bytes, &len), "first-track .description missing\n");
	CHECK(len == strlen("Stub words about the canned book.") &&
			  memcmp(bytes, "Stub words about the canned book.", len) == 0,
		  "description bytes differ: %zu\n", len);
	free(bytes);
	// The cover is the ONLY nonfatal sidecar: the stub stream refused it, the
	// book still DONEs, and no cover.jpg may appear.
	CHECK(cover_stream_seen, "the cover sidecar was never attempted\n");
	CHECK(strstr(cover_stream_url, "/api/items/stub-book/cover?format=jpeg") != NULL,
		  "cover url %s\n", cover_stream_url);
	{
		char cover_path[1500];
		snprintf(cover_path, sizeof(cover_path), "%.1300s/cover.jpg", dest_abs);
		CHECK(!file_exists_abs(cover_path), "a refused cover must not leave a file behind\n");
	}
	// The manifest: written LAST, byte shape of the legacy writer.
	manifest_view_t mv;
	read_manifest(manifest, &mv);
	CHECK(mv.present, "the manifest must be on the card\n");
	CHECK(strcmp(mv.version, "1") == 0, "manifest version %s\n", mv.version);
	CHECK(strcmp(mv.item_id, STUB_ID) == 0, "manifest item %s\n", mv.item_id);
	char normalized_server[ABS_SERVER_MAX];
	abs_profiles_normalize_server(STUB_SERVER, normalized_server, sizeof(normalized_server));
	CHECK(strcmp(mv.server, normalized_server) == 0, "manifest server %s\n", mv.server);
	CHECK(strcmp(mv.duration, "200.000") == 0, "manifest duration %s\n", mv.duration);
	CHECK(strcmp(mv.progress, "13.000") == 0, "manifest progress %s\n", mv.progress);
	CHECK(strcmp(mv.progress_updated, "1700000000000") == 0, "manifest stamp %s\n",
		  mv.progress_updated);
	CHECK(mv.track_count == 2, "manifest tracks %d\n", mv.track_count);
	CHECK(mv.tracks[0].no == 1 && fabs(mv.tracks[0].offset) < 1e-9 &&
			  fabs(mv.tracks[0].duration - 10.0) < 1e-9 &&
			  strcmp(mv.tracks[0].name, "001 - Part 1.mp3") == 0,
		  "manifest track one: %ld %.3f %.3f %.260s\n", mv.tracks[0].no, mv.tracks[0].offset,
		  mv.tracks[0].duration, mv.tracks[0].name);
	CHECK(mv.tracks[1].no == 2 && fabs(mv.tracks[1].offset - 10.0) < 1e-9 &&
			  fabs(mv.tracks[1].duration - 10.0) < 1e-9 &&
			  strcmp(mv.tracks[1].name, "002 - Part 2.mp3") == 0,
		  "manifest track two: %ld %.3f %.3f %.260s\n", mv.tracks[1].no, mv.tracks[1].offset,
		  mv.tracks[1].duration, mv.tracks[1].name);
	// The REAL acknowledgement reset: the planted checkpoint row died with it.
	CHECK(stub_requests == 1, "the engine ran %lld API requests, wanted exactly one fetch\n",
		  stub_requests);
	CHECK(!checkpoint_present(abs_book), "the planted acknowledgement row must be reset\n");
	CHECK(!abs_download_sync_blocked(want_dest), "the DONE transaction must release the barrier\n");

	// Reopen the card: a DONE row is terminal — no re-reset, no extra fetch.
	queue_stop();
	checkpoint_plant(abs_book);
	long long requests_before = stub_requests;
	queue_init();
	CHECK(job_state(id) == ABS_DL_DONE, "reopened state %s\n", abs_download_state_name(job_state(id)));
	CHECK(checkpoint_present(abs_book), "a DONE acknowledgement must never be reset again\n");
	CHECK(stub_requests == requests_before, "reopening must not touch the server\n");
	CHECK(!abs_download_sync_blocked(want_dest), "no barrier may linger after DONE\n");
	queue_stop();
	printf("ok\n");
}

// Phase A2 (§8.3 line 556): the scan-completion import coordinator over the
// REAL adapter. Before the audiobook index exists the import must refuse and
// leave the DONE row exactly as it is; the real scan (the §8.5 line-592
// notification wired through audiobookshelf_queue_import_install()) then
// imports through the compare-and-swap, and the durable rows must carry
// EXACTLY the row's stored values. The acknowledgement reset is never re-run
// behind any of it: the imported sync row survives the coordinator pass.
static void offline_phase_import(void) {
	printf("transfer test: offline import coordinator (§8.3)... ");
	phase_card("fin-import");
	audiobook_db_open();
	offline_create_media();
	char want_dest[600], dest_abs[1400], final1[1400], final2[1400];
	offline_paths(want_dest, sizeof(want_dest), dest_abs, sizeof(dest_abs), final1, sizeof(final1),
				  final2, sizeof(final2));
	char abs_book[1400];
	snprintf(abs_book, sizeof(abs_book), "%s", dest_abs);
	checkpoint_plant(abs_book);

	// The scan-completion listener is registered exactly like main.c wires it.
	audiobookshelf_queue_import_install();

	queue_init();
	long long id = offline_enqueue();
	CHECK(pump_until_terminal(id) == ABS_DL_DONE, "the job must finish DONE\n");
	abs_download_job_t done = snapshot_job(id);
	CHECK(done.ack_reset_done == 1 && done.index_pending == 1, "DONE flags\n");
	CHECK(!checkpoint_present(abs_book), "the real reset must have died with the job\n");
	long long requests_before = stub_requests; // exactly the one identity fetch

	// -- Before any index exists: the row's own import refuses --------------
	char error[256] = {0};
	int imported = abs_download_import_pending(error, sizeof(error));
	CHECK(imported == 0, "nothing can import without the index, got %d\n", imported);
	CHECK(strstr(error, "not indexed") != NULL, "refusal text: %s\n", error);
	abs_download_job_t kept = snapshot_job(id);
	CHECK(kept.state == ABS_DL_DONE && kept.index_pending == 1,
		  "a refused import must keep the row importable (state %s/%d)\n",
		  abs_download_state_name(kept.state), kept.index_pending);
	CHECK(kept.error[0] == '\0', "a soft import failure must not smear an error onto DONE: %s\n",
		  kept.error);
	CHECK(stub_requests == requests_before, "the coordinator must not touch the server\n");
	CHECK(!checkpoint_present(abs_book), "the refusal must not create a sync row\n");

	// -- The real scan completes and the listener imports the row -----------
	printf("(scan)... ");
	char scan_root[600];
	snprintf(scan_root, sizeof(scan_root), "%.580s/Audiobooks", card);
	CHECK(audiobookdb_scan_start(scan_root), "audiobook scan refused\n");
	for (int i = 0; i < 4000 && audiobookdb_scan_running(); i++) {
		usleep(5000);
	}
	CHECK(!audiobookdb_scan_running(), "the scan never finished\n");
	wait_index_pending_cleared(STUB_ID, &id);
	CHECK(job_state(id) == ABS_DL_DONE, "DONE must survive the import\n");

	// -- The durable rows carry exactly the row's stored values -------------
	// The stub identity carried currentTime 13.0 / lastUpdate 1700000000000,
	// which audiobookshelf_resume_target() mapped to part two at 3.0 s
	// (§8.2 step 5, asserted on the row at DONE time in phase A).
	double seconds = -1;
	bool finished = true;
	char resume_file[600];
	CHECK(audiobook_resume_row(abs_book, resume_file, sizeof(resume_file), &seconds, &finished),
		  "the imported book must be in the index\n");
	CHECK(strcmp(resume_file, final2) == 0, "imported resume file %s, wanted %s\n", resume_file,
		  final2);
	CHECK(fabs(seconds - 3.0) < 1e-9, "imported resume seconds %f, wanted 3.0\n", seconds);
	CHECK(!finished, "an unfinished download must not mark the book finished\n");
	char server_row[ABS_SERVER_MAX], file_row[SONIX_LINKED_DB_PATH_MAX];
	int linked = 0;
	long long stamp = 0;
	bool row_finished = true;
	double row_seconds = -1;
	CHECK(audiobook_sync_row(abs_book, server_row, sizeof(server_row), file_row, sizeof(file_row),
							 &row_seconds, &row_finished, &stamp, &linked),
		  "the compare-and-swap sync row must exist\n");
	char normalized[ABS_SERVER_MAX];
	abs_profiles_normalize_server(STUB_SERVER, normalized, sizeof(normalized));
	CHECK(strcmp(server_row, normalized) == 0, "sync server %s\n", server_row);
	CHECK(strcmp(file_row, final2) == 0, "sync file %s\n", file_row);
	CHECK(fabs(row_seconds - 3.0) < 1e-9, "sync seconds %f\n", row_seconds);
	CHECK(!row_finished && linked == 1, "sync flags\n");
	CHECK(stamp == 1700000000000LL, "sync stamp %lld, wanted the row's remote_updated_ms\n", stamp);
	// Idempotence in the real chain: the imported acknowledgement row survives
	// the pass that created it — the coordinator never re-runs a reset
	// (§8.2 line 541), which would have deleted this very row.
	CHECK(checkpoint_present(abs_book), "the imported sync row must survive its own pass\n");
	CHECK(stub_requests == requests_before, "the coordinator pass must not touch the server\n");

	// -- A second pass has nothing to do ------------------------------------
	CHECK(abs_download_import_pending(error, sizeof(error)) == 0, "empty pass after import\n");
	// The audiobookdb handle stays open for the later crash phases (they plant
	// and reset through the same module state); the offline main closes it.
	queue_stop();
	printf("ok\n");
}

// Phase B: manifest close/rename failure -> the job fails at the finalize
// stage, the barrier releases into the recoverable failed state, and nothing
// pretends the destination was committed.
static void offline_phase_manifest_fail(void) {
	printf("transfer test: offline manifest failure... ");
	phase_card("fin-manifest-fail");
	offline_create_media();
	char want_dest[600], dest_abs[1400], final1[1400], final2[1400];
	offline_paths(want_dest, sizeof(want_dest), dest_abs, sizeof(dest_abs), final1, sizeof(final1),
				  final2, sizeof(final2));
	char manifest[1400];
	snprintf(manifest, sizeof(manifest), "%.1300s/.audiobookshelf", dest_abs);

	queue_init();
	long long id = offline_enqueue();
	pump_to(id, ABS_DL_FINALIZING);
	// Occupy the temporary-file name: the checked open fails after the
	// sidecars succeeded, isolating the manifest stage.
	char tmpdir[1400];
	snprintf(tmpdir, sizeof(tmpdir), "%.1300s.tmp", manifest);
	CHECK(mkdir(tmpdir, 0777) == 0, "cannot stage the tmp collision: %s\n", strerror(errno));
	CHECK(pump_until_terminal(id) == ABS_DL_FAILED, "the manifest failure must fail the job\n");
	abs_download_job_t j = snapshot_job(id);
	CHECK(strcmp(j.error_stage, "manifest") == 0, "stage %s\n", j.error_stage);
	CHECK(strstr(j.error, "manifest") != NULL, "error %s\n", j.error);
	// Sidecars preceded the manifest and stay; there is no manifest and no
	// barrier: a progress reconcile may look at a destination with no commit.
	CHECK(file_exists_abs(manifest) == false, "no manifest on failure\n");
	{
		char tags1[1400];
		snprintf(tags1, sizeof(tags1), "%.1300s.tags", final1);
		CHECK(file_exists_abs(tags1), "the sidecars that succeeded must stay\n");
	}
	// The failure released the barrier: nothing half-finalized is hidden from
	// a progress reconcile, and Retry redoes finalize only (§8.1: one more
	// identity fetch, zero media requests).
	CHECK(!abs_download_sync_blocked(want_dest), "the failure must release the barrier\n");
	{
		char tags1[1400];
		snprintf(tags1, sizeof(tags1), "%.1300s.tags", final1);
		CHECK(file_exists_abs(tags1), "the sidecars that succeeded must stay\n");
	}
	long long requests_before = stub_requests;
	CHECK(abs_download_retry(id, NULL, 0), "retry refused\n");
	// The temporary collision is still in place, so the retried finalize hits
	// the identical checked-open failure (§8.2 step 7: the job never commits).
	CHECK(pump_until_terminal(id) == ABS_DL_FAILED, "retry state\n");
	abs_download_job_t j2 = snapshot_job(id);
	CHECK(strcmp(j2.error_stage, "manifest") == 0, "retry stage %s\n", j2.error_stage);
	CHECK(!abs_download_sync_blocked(want_dest), "retry failure released the barrier\n");
	CHECK(stub_requests == requests_before + 1, "the retry must refetch the identity once, got %lld\n",
		  stub_requests - requests_before);
	queue_stop();
	printf("ok\n");
}

// Phase C: crash AFTER the manifest rename, BEFORE the acknowledgement commit.
// Recovery re-establishes the barrier, completes from the valid manifest
// offline (no refetch, no card rewrite), performs the reset and the DONE
// transaction, then releases the barrier.
static void offline_phase_crash_valid(void) {
	printf("transfer test: offline crash before the ack (valid manifest)... ");
	phase_card("fin-crash-ok");
	offline_create_media();
	cover_stream_seen = false;
	run_crash_child(true, true);

	char want_dest[600], dest_abs[1400], final1[1400], final2[1400];
	offline_paths(want_dest, sizeof(want_dest), dest_abs, sizeof(dest_abs), final1, sizeof(final1),
				  final2, sizeof(final2));
	char manifest[1400];
	snprintf(manifest, sizeof(manifest), "%.1300s/.audiobookshelf", dest_abs);
	char bytes_before[4096];
	size_t bytes_len = 0;
	{
		char *b = NULL;
		CHECK(read_bytes_abs(manifest, &b, &bytes_len), "the crashed manifest is missing\n");
		CHECK(bytes_len < sizeof(bytes_before), "manifest too big for the probe buffer\n");
		memcpy(bytes_before, b, bytes_len);
		free(b);
	}
	char tags1[1400];
	snprintf(tags1, sizeof(tags1), "%.1300s.tags", final1);
	CHECK(file_exists_abs(tags1), "the crashed sidecars are missing\n");

	// The barrier is re-established by the armed queue itself, before any
	// worker step runs (§8.2 line 540). The crashed row is already there —
	// recovery adopts it, nothing is re-enqueued.
	queue_init();
	abs_download_snapshot_t snap;
	CHECK(abs_download_snapshot(&snap) && snap.job_count == 1, "crash rows\n");
	long long job_id = snap.jobs[0].id;
	CHECK(job_state(job_id) == ABS_DL_FINALIZING, "crash state %s\n",
		  abs_download_state_name(job_state(job_id)));
	CHECK(abs_download_sync_blocked(want_dest), "the recovered barrier must hold\n");
	char abs_book[1400];
	snprintf(abs_book, sizeof(abs_book), "%s", dest_abs);
	checkpoint_plant(abs_book);
	long long requests_before = stub_requests;
	CHECK(pump_until_terminal(job_id) == ABS_DL_DONE, "recovery must complete DONE\n");

	abs_download_job_t j = snapshot_job(job_id);
	CHECK(j.error[0] == '\0', "recovery DONE carries an error: %s\n", j.error);
	CHECK(j.ack_reset_done == 1 && j.index_pending == 1, "recovery DONE flags\n");
	CHECK(strcmp(j.resume_file, "002 - Part 2.mp3") == 0, "recovered resume file %.260s\n",
		  j.resume_file);
	// §8.2 line 540: the resume mapping rebuilt from the valid manifest and
	// the row's remote snapshot must equal what the normal mapper computes.
	CHECK(fabs(j.resume_seconds - 3.0) < 1e-9, "recovered resume seconds %f\n", j.resume_seconds);
	// Completing the acknowledgement needs no network and no card rewrite.
	CHECK(stub_requests == requests_before, "recovery fetched the identity %lld times\n",
		  stub_requests - requests_before);
	{
		char *b = NULL;
		size_t len = 0;
		CHECK(read_bytes_abs(manifest, &b, &len), "manifest gone after recovery\n");
		CHECK(len == bytes_len && memcmp(b, bytes_before, len) == 0,
			  "recovery rewrote the valid manifest\n");
		free(b);
	}
	CHECK(!checkpoint_present(abs_book), "the recovery must perform the acknowledgement reset\n");
	CHECK(!abs_download_sync_blocked(want_dest), "the recovered barrier must release at DONE\n");
	// Idempotence: reopen — never a second reset.
	queue_stop();
	checkpoint_plant(abs_book);
	queue_init();
	CHECK(job_state(job_id) == ABS_DL_DONE, "reopened state %s\n",
		  abs_download_state_name(job_state(job_id)));
	CHECK(checkpoint_present(abs_book), "a DONE acknowledgement must never be reset again\n");
	queue_stop();
	printf("ok\n");
}

// Phase D: crash BEFORE the manifest (only files + FINALIZING row). Recovery
// retries ONLY finalization: one identity refetch (committed audio untouched),
// the full finalize (sidecars + refused cover + manifest), the reset, DONE.
static void offline_phase_crash_incomplete(void) {
	printf("transfer test: offline crash before the manifest (refetch)... ");
	phase_card("fin-crash-refetch");
	offline_create_media();
	cover_stream_seen = false;
	run_crash_child(false, true);

	char want_dest[600], dest_abs[1400], final1[1400], final2[1400];
	offline_paths(want_dest, sizeof(want_dest), dest_abs, sizeof(dest_abs), final1, sizeof(final1),
				  final2, sizeof(final2));
	char manifest[1400];
	snprintf(manifest, sizeof(manifest), "%.1300s/.audiobookshelf", dest_abs);
	CHECK(!file_exists_abs(manifest), "the child wrote no manifest\n");

	queue_init();
	abs_download_snapshot_t snap;
	CHECK(abs_download_snapshot(&snap) && snap.job_count == 1, "crash rows\n");
	long long job_id = snap.jobs[0].id;
	CHECK(job_state(job_id) == ABS_DL_FINALIZING, "crash state %s\n",
		  abs_download_state_name(job_state(job_id)));
	CHECK(abs_download_sync_blocked(want_dest), "the recovered barrier must hold\n");
	char abs_book[1400];
	snprintf(abs_book, sizeof(abs_book), "%.1399s", dest_abs);
	checkpoint_plant(abs_book);
	char abs_probe[1400];
	snprintf(abs_probe, sizeof(abs_probe), "%.700s/%.599s", card, want_dest);
	long long requests_before = stub_requests;
	CHECK(pump_until_terminal(job_id) == ABS_DL_DONE, "the finalize-only retry must complete\n");
	CHECK(stub_requests == requests_before + 1, "the recovery must refetch the identity once, got %lld\n",
		  stub_requests - requests_before);
	abs_download_job_t jr = snapshot_job(job_id);
	CHECK(strcmp(jr.resume_file, "002 - Part 2.mp3") == 0, "resume file after retry %.260s\n",
		  jr.resume_file);
	CHECK(fabs(jr.resume_seconds - 3.0) < 1e-9, "resume seconds after retry %f\n", jr.resume_seconds);
	{
		abs_download_file_t rows[8];
		CHECK(abs_download_job_files(job_id, rows, 8) == 2, "rows after retry\n");
		CHECK(strcmp(rows[0].state, "complete") == 0 && strcmp(rows[1].state, "complete") == 0,
			  "retry must not disturb committed rows\n");
	}
	manifest_view_t mv;
	read_manifest(manifest, &mv);
	CHECK(mv.present && strcmp(mv.version, "1") == 0 && mv.track_count == 2,
		  "the retried manifest is invalid\n");
	{
		char tags1[1400];
		snprintf(tags1, sizeof(tags1), "%.1300s.tags", final1);
		char description[1400];
		snprintf(description, sizeof(description), "%.1300s.description", final1);
		CHECK(file_exists_abs(tags1) && file_exists_abs(description),
			  "the retried finalize must write the sidecars\n");
	}
	CHECK(cover_stream_seen, "the retried finalize must have attempted the cover\n");
	CHECK(!checkpoint_present(abs_book), "the retried job must reset its acknowledgement\n");
	CHECK(!abs_download_sync_blocked(abs_probe), "the barrier must release at DONE\n");
	queue_stop();
	printf("ok\n");
}

// Phase E: recovery over a recorded file that vanished from the card. §8.2
// file rules quarantine it for user retry — recovery must not refetch, not
// finalize, not fake a commit, and must release the barrier.
static void offline_phase_crash_missing(void) {
	printf("transfer test: offline crash with a deleted file... ");
	phase_card("fin-crash-missing");
	offline_create_media();
	run_crash_child(false, false); // drops the second file, keeps the row

	char want_dest[600], dest_abs[1400], final1[1400], final2[1400];
	offline_paths(want_dest, sizeof(want_dest), dest_abs, sizeof(dest_abs), final1, sizeof(final1),
				  final2, sizeof(final2));
	queue_init();
	abs_download_snapshot_t snap;
	CHECK(abs_download_snapshot(&snap) && snap.job_count == 1, "crash rows\n");
	long long job_id = snap.jobs[0].id;
	CHECK(abs_download_sync_blocked(want_dest), "the recovered barrier must hold\n");
	char abs_probe[1400];
	snprintf(abs_probe, sizeof(abs_probe), "%.700s/%.599s", card, want_dest);
	long long requests_before = stub_requests;
	CHECK(pump_until_terminal(job_id) == ABS_DL_FAILED, "a vanished file must fail the job\n");
	abs_download_job_t jm = snapshot_job(job_id);
	CHECK(strcmp(jm.error_stage, "write") == 0, "stage %s\n", jm.error_stage);
	CHECK(stub_requests == requests_before, "recovery must not refetch over missing audio\n");
	CHECK(!abs_download_sync_blocked(abs_probe), "the failure must release the barrier\n");
	queue_stop();
	printf("ok\n");
}

// Offline main, inside the same conditional as its shape.
int main(int argc, char **argv) {
	if (argc != 2 || !argv[1] || !argv[1][0]) {
		fprintf(stderr, "usage: audiobookshelf-queue-transfer-test <workdir>\n");
		return 2;
	}
	snprintf(work, sizeof(work), "%.880s", argv[1]);
	offline_run();
	// The finalize wave (§8.2 steps 5–8): one phase per fresh card so every
	// crash child arms a queue nobody has armed in-process before the fork.
	offline_phase_done();
	offline_phase_import();
	offline_phase_manifest_fail();
	offline_phase_crash_valid();
	offline_phase_crash_incomplete();
	offline_phase_crash_missing();
	audiobookdb_close();
	return 0;
}

#else // ABS_QUEUE_TRANSFER_LIVE
// ---------------------------------------------------------------------------
// Live shape: real HTTP against the fixture mock (one alias per run).
// The runner prepares <work>/server.txt, <work>/token.txt, <work>/media/
// (part1.mp3 16 MiB, part2.mp3 4 KiB), the request-log path, an empty card/,
// and points SONIX_AUDIOBOOKSHELF_PROFILES at a fresh profile file.
// ---------------------------------------------------------------------------

static char media_dir[1000];
static char request_log[1000];
static char mock_token[256];

// Lines of the mock's request log that contain `needle`. A missing log counts
// as zero; the first request creates it.
static long long count_log_lines_with(const char *needle) {
	FILE *f = fopen(request_log, "r");
	if (!f) return 0;
	char line[4096];
	long long hits = 0;
	while (fgets(line, sizeof(line), f)) {
		if (strstr(line, needle)) hits++;
	}
	fclose(f);
	return hits;
}

// ---------------------------------------------------------------------------
// §8.5 line 588/1028/1157 — deterministic wake selftest: the REAL HTTP stack
// against a local stall server this test drives. No mock, no DNS, no
// wall-clock pass paths: the canceller synchronizes onto an actually published
// (i.e. blocked) network phase via http_op_published() and then cancels; only
// the shutdown can end the blocked wait, because every socket here carries a
// 30 s receive timeout that the assertion bounds are far below.
// ---------------------------------------------------------------------------

enum { WAKE_BODY_PARTIAL = 0, WAKE_HEADERS_PARTIAL = 1, WAKE_SILENT = 2 };

static volatile int wake_mode;
static volatile long wake_connections;
static int wake_listen_fd = -1;

static void wake_server_send(int fd, const char *bytes) {
	size_t len = strlen(bytes);
	size_t sent = 0;
	while (sent < len) {
		ssize_t n = send(fd, bytes + sent, len - sent, 0);
		if (n <= 0) {
			return;
		}
		sent += (size_t)n;
	}
}

static void *wake_server_main(void *unused) {
	(void)unused;
	// The listener fd is captured once: its happens-before with the creator
	// (pthread_create) is the only synchronization this loop needs, so main
	// may close and reset the global at stop time without racing the server.
	int listener = wake_listen_fd;
	for (;;) {
		// poll(), never a bare blocking accept: close() does not reliably
		// wake a thread parked in accept(), and the stop below closes the
		// listener. A 200 ms poll tick re-checks the closed/faulted state,
		// so the join ends in at most one tick.
		struct pollfd watch = {.fd = listener, .events = POLLIN};
		int rc = poll(&watch, 1, 200);
		if (rc < 0) {
			if (errno == EINTR) {
				continue;
			}
			return NULL;
		}
		if (rc == 0) {
			continue;
		}
		if (watch.revents & (POLLNVAL | POLLERR)) {
			return NULL; // the listener was closed; the selftest is over
		}
		int fd = accept(listener, NULL, NULL);
		if (fd < 0) {
			// Only a closed listener ends the loop. A client that aborts its
			// connection before the server accepts it races this accept —
			// the cancellers below are designed to do exactly that — and
			// Linux surfaces ECONNABORTED/EAGAIN there, never a fault of
			// the server itself.
			if (errno == EBADF) {
				return NULL;
			}
			continue;
		}
		wake_connections++;
		if (wake_mode == WAKE_BODY_PARTIAL) {
			// Full headers plus four body bytes, then nothing: the promised
			// 4 MiB never arrives, so the client's buffered (or streaming)
			// body read is blocked with data that can never satisfy a loop.
			wake_server_send(fd, "HTTP/1.1 200 OK\r\nContent-Length: 4194304\r\n\r\npart");
		} else if (wake_mode == WAKE_HEADERS_PARTIAL) {
			// Half a header block: the client's read_headers consumes it and
			// blocks on the continuation that never comes.
			wake_server_send(fd, "HTTP/1.1 200 OK\r\nContent-Type: audio/mpeg\r\nContent-Leng");
		}
		// WAKE_SILENT: not a byte; the very first header read blocks.
	}
}

typedef struct wake_server {
	pthread_t thread;
	char url[64];
} wake_server_t;

static void wake_server_start(wake_server_t *w) {
	CHECK(wake_listen_fd < 0, "wake server left armed\n");
	wake_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
	CHECK(wake_listen_fd >= 0, "wake listener socket\n");
	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = 0;
	CHECK(bind(wake_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0, "wake bind\n");
	CHECK(listen(wake_listen_fd, 8) == 0, "wake listen\n");
	struct sockaddr_in bound;
	socklen_t blen = sizeof(bound);
	CHECK(getsockname(wake_listen_fd, (struct sockaddr *)&bound, &blen) == 0, "wake getsockname\n");
	snprintf(w->url, sizeof(w->url), "http://127.0.0.1:%d/file", ntohs(bound.sin_port));
	wake_mode = WAKE_SILENT;
	wake_connections = 0;
	CHECK(pthread_create(&w->thread, NULL, wake_server_main, NULL) == 0, "wake server thread\n");
}

static void wake_server_stop(wake_server_t *w) {
	close(wake_listen_fd); // the blocking accept returns and the thread exits
	wake_listen_fd = -1;
	pthread_join(w->thread, NULL);
}

// The selftest's canceller: wait until the caller's op is inside a published
// (blocked) phase that PERSISTS, then cancel — the wake lands on a genuinely
// blocked read, never on a poll interval or a lucky pre-check. Loopback
// connect/write/header phases are gone within microseconds; a publication
// still present ~200 µs later is a stalled wait. Bounded; a miss is a failed
// test, not a slow pass.
static void wake_wait_published_then_cancel(http_op_t *op, const char *what) {
	for (int i = 0; i < 100000; i++) {
		if (http_op_published(op, NULL)) {
			usleep(200);
			if (http_op_published(op, NULL)) {
				fprintf(stderr, "wake-canceller: cancelling %s (published twice)\n", what);
				http_op_cancel(op);
				return;
			}
		}
		usleep(100);
	}
	fprintf(stderr, "wake-canceller: timeout; cancelled=%d\n", http_op_cancelled(op));
	CHECK(0, "%s: the op never reached a blocked read\n", what);
}

// The racy canceller for the safety-net loop: cancel as soon as — and as often
// as — the op is anywhere near a network phase, with no synchronization on
// purpose. The overlap between the owner's publish/unpublish/close and this
// thread's wake is exactly what the sanitizer must sweep clean.
static void *wake_racy_canceller(void *arg) {
	http_op_t *op = arg;
	for (int round = 0; round < 64 && !http_op_cancelled(op); round++) {
		if (http_op_published(op, NULL)) {
			http_op_cancel(op);
			return NULL;
		}
		usleep(50);
	}
	http_op_cancel(op); // the stop must land regardless
	return NULL;
}

typedef struct {
	http_op_t op;
	char url[64];
	volatile bool done;
	bool received;   // buffered: the request result
	bool opened;     // streaming: the open result
	int read_bytes;  // streaming read: the byte count (or <=0 on wake/end)
	int status;
} wake_client_t;

// Buffered: request a body the server never completes; the blocked body read
// must end on the wake alone (the 30 s receive timeout is far past the join
// deadline below).
static void *wake_buffered_client(void *arg) {
	wake_client_t *w = arg;
	char *body = NULL;
	http_req_t req = {0};
	req.op = &w->op;
	w->received = http_request(w->url, &req, &body, NULL, 1 << 20, 30, &w->status);
	free(body);
	w->done = true;
	return NULL;
}

// Streaming, two stages so the wake can be aimed at each of them.
static void *wake_stream_open_client(void *arg) {
	wake_client_t *w = arg;
	http_stream_t st;
	w->opened = http_stream_open_ex_op(&st, w->url, NULL, 30, &w->op, &w->status);
	if (w->opened) {
		http_stream_close(&st);
	}
	w->done = true;
	return NULL;
}

typedef struct {
	http_op_t op;
	char url[64];
	volatile bool done;
	int read_bytes;
	bool stream_closed; // the OWNER thread closed the stream (never the canceller)
} wake_reader_t;

static void *wake_stream_reader(void *arg) {
	wake_reader_t *w = arg;
	http_stream_t st;
	int status = 0;
	bool opened = http_stream_open_ex_op(&st, w->url, NULL, 30, &w->op, &status);
	if (opened) {
		char buffer[4096];
		// Drain the header parser's lead bytes, then the real body read runs
		// on the socket and blocks there (the server promises 4 MiB and sent
		// four bytes): the wake has no choice but to end THAT read.
		for (int reads = 0; reads < 1000; reads++) {
			w->read_bytes = http_stream_read_op(&st, buffer, sizeof(buffer), &w->op);
			if (w->read_bytes <= 0) {
				break;
			}
		}
		http_stream_close(&st); // the owner alone closes
		w->stream_closed = true;
	}
	w->done = true;
	return NULL;
}

static void wake_selftest(void) {
	printf("wake: http stack (buffered/stream/headers/connect)... ");
	wake_server_t server;
	wake_server_start(&server);

	// 1. A cancel BEFORE the request stops it without one connection attempt.
	{
		wake_client_t w;
		memset(&w, 0, sizeof(w));
		http_op_init(&w.op);
		http_op_cancel(&w.op); // stop before anything begins
		snprintf(w.url, sizeof(w.url), "%s", server.url);
		pthread_t thread;
		CHECK(pthread_create(&thread, NULL, wake_buffered_client, &w) == 0, "client\n");
		pthread_join(thread, NULL);
		CHECK(!w.received, "a pre-cancelled request must not succeed\n");
		CHECK(wake_connections == 0, "a pre-cancelled request must not connect\n");
		CHECK(!http_op_woken_current(&w.op),
			  "a stop recorded before this operation must not claim its result\n");
		http_op_destroy(&w.op);
	}

	// 2. Blocked BUFFERED body read: the wake ends it far before the 30 s
	//    receive timeout, and the result generation attributes the outcome.
	{
		wake_client_t w;
		memset(&w, 0, sizeof(w));
		http_op_init(&w.op);
		snprintf(w.url, sizeof(w.url), "%s", server.url);
		wake_mode = WAKE_BODY_PARTIAL;
		pthread_t thread;
		CHECK(pthread_create(&thread, NULL, wake_buffered_client, &w) == 0, "client\n");
		wake_wait_published_then_cancel(&w.op, "the buffered body read");
		pthread_join(thread, NULL);
		CHECK(!w.received, "an interrupted body must fail, not pass as success\n");
		CHECK(http_op_woken_current(&w.op), "the aborted body read must be wake-attributed\n");
		http_op_destroy(&w.op);
	}

	// 3. Blocked header read (streaming open): the open is wakeable too.
	{
		wake_client_t w;
		memset(&w, 0, sizeof(w));
		http_op_init(&w.op);
		snprintf(w.url, sizeof(w.url), "%s", server.url);
		wake_mode = WAKE_HEADERS_PARTIAL;
		pthread_t thread;
		CHECK(pthread_create(&thread, NULL, wake_stream_open_client, &w) == 0, "client\n");
		wake_wait_published_then_cancel(&w.op, "the blocked header read");
		pthread_join(thread, NULL);
		CHECK(!w.opened, "an interrupted open must fail\n");
		CHECK(http_op_woken_current(&w.op), "open wake attribution\n");
		http_op_destroy(&w.op);
	}

	// 4. Blocked STREAMING body read: the wake ends the read; the reader
	//    thread (the owner) alone closed the stream.
	{
		wake_reader_t w;
		memset(&w, 0, sizeof(w));
		snprintf(w.url, sizeof(w.url), "%s", server.url);
		http_op_init(&w.op);
		wake_mode = WAKE_BODY_PARTIAL;
		pthread_t thread;
		CHECK(pthread_create(&thread, NULL, wake_stream_reader, &w) == 0, "reader\n");
		wake_wait_published_then_cancel(&w.op, "the streaming body read");
		pthread_join(thread, NULL);
		CHECK(w.read_bytes <= 0 && http_op_woken_current(&w.op),
			  "the woken read must end in the cancelled state (got %d)\n", w.read_bytes);
		CHECK(w.stream_closed, "the owner thread must be the one that closed the stream\n");
		http_op_destroy(&w.op);
	}

	// 5. Wake during CONNECT: a non-routable address holds the connect; one
	//    cancel ends it (this kernel aborts a connecting socket on shutdown).
	{
		wake_client_t w;
		memset(&w, 0, sizeof(w));
		http_op_init(&w.op);
		snprintf(w.url, sizeof(w.url), "%s", "http://240.0.0.1:9/file");
		pthread_t thread;
		CHECK(pthread_create(&thread, NULL, wake_stream_open_client, &w) == 0, "client\n");
		wake_wait_published_then_cancel(&w.op, "the blocked connect");
		pthread_join(thread, NULL);
		CHECK(!w.opened, "an interrupted connect must fail\n");
		CHECK(http_op_woken_current(&w.op), "connect wake attribution\n");
		http_op_destroy(&w.op);
	}

	// 6. The safety net, run hard under the sanitizer: repeated open/cancel
	//    races against a silent server, with the canceller firing whenever it
	//    can — sometimes between phases, sometimes mid-block. Any
	//    publish/unpublish-vs-close misuse (use-after-free, fd reuse, double
	//    close) is ASan territory; a correct run simply keeps returning.
	{
		wake_mode = WAKE_SILENT;
		for (int i = 0; i < 32; i++) {
			wake_client_t w;
			memset(&w, 0, sizeof(w));
			http_op_init(&w.op);
			snprintf(w.url, sizeof(w.url), "%s", server.url);
			pthread_t canceller, client;
			CHECK(pthread_create(&client, NULL, wake_stream_open_client, &w) == 0, "client\n");
			CHECK(pthread_create(&canceller, NULL, wake_racy_canceller, &w.op) == 0, "canceller\n");
			pthread_join(client, NULL);
			if (w.opened) {
				fprintf(stderr, "wake-race round %d: the open SURVIVED the cancels\n", i);
			}
			pthread_join(canceller, NULL);
			CHECK(!w.opened, "a cancelled open must fail (round %d)\n", i);
			http_op_destroy(&w.op);
		}
	}

	wake_server_stop(&server);
	printf("ok\n");
}

typedef struct {
	pthread_t thread;
	long long job_id;
	long long threshold;
	bool fired;
} pause_watcher_t;

// §8.1 mid-transfer pause: fires abs_download_pause() once the queue's RAM
// counter shows the active file past `threshold`. Deterministic on a 16 MiB
// track: the counter is monotonic per file, one 50 µs poll can never miss it,
// and the remaining ~15.9 MiB keeps the transfer in flight well past the
// pause call. `fired` is release-stored so the joining reader needs no further
// fence.
static void *pause_watcher_main(void *arg) {
	pause_watcher_t *w = arg;
	for (int i = 0; i < 400000 && !__atomic_load_n(&w->fired, __ATOMIC_ACQUIRE); i++) {
		abs_download_snapshot_t snap;
		if (abs_download_snapshot(&snap)) {
			for (int j = 0; j < snap.job_count; j++) {
				if (snap.jobs[j].id != w->job_id || snap.jobs[j].state != ABS_DL_DOWNLOADING) continue;
				if (snap.jobs[j].bytes_done < w->threshold) continue;
				char err[128] = {0};
				if (abs_download_pause(w->job_id, err, sizeof(err))) {
					__atomic_store_n(&w->fired, true, __ATOMIC_RELEASE);
					return NULL;
				}
				fprintf(stderr, "pause retry: %s\n", err);
			}
		}
		usleep(50);
	}
	return NULL;
}

static void live_run(void) {
	read_root_file("server.txt", live_server, sizeof(live_server));
	read_root_file("token.txt", mock_token, sizeof(mock_token));
	snprintf(media_dir, sizeof(media_dir), "%.860s/media", work);
	snprintf(request_log, sizeof(request_log), "%.700s/progress.requests", work);

	// Phase 0 — the wake machinery of §8.5 against the real HTTP stack, with
	// no mock and no DNS (deterministic, sanitizer-run both here and in the
	// queue-engine test's fake-blocking harness).
	wake_selftest();

	// The profile store is the runner's fresh file; the real auth surface is
	// exercised through it exactly like a device session.
	CHECK(abs_profiles_init() >= 0, "profiles init failed\n");
	CHECK(abs_profiles_add("Transfer", live_server, mock_token, live_profile,
						   sizeof(live_profile)),
		  "cannot add the live profile\n");
	CHECK(strcmp(live_profile, "p1") == 0, "unexpected profile id %s\n", live_profile);

	make_card();
	queue_init();

	// Media reference bytes (the same files the mock serves).
	char ref_path[1300];
	snprintf(ref_path, sizeof(ref_path), "%.900s/part1.mp3", media_dir);
	char *part1 = NULL;
	size_t part1_len = 0;
	CHECK(read_bytes_abs(ref_path, &part1, &part1_len), "part1.mp3 missing\n");
	snprintf(ref_path, sizeof(ref_path), "%.900s/part2.mp3", media_dir);
	char *part2 = NULL;
	size_t part2_len = 0;
	CHECK(read_bytes_abs(ref_path, &part2, &part2_len), "part2.mp3 missing\n");
	CHECK(part1_len > (1 << 20) && part2_len > 0 && part2_len < 65536,
		  "the runner must make part1 16 MiB and part2 small\n");

	char want_dest[600];
	expected_destination(live_server, "Test Author", "The Mock Journey", want_dest, sizeof(want_dest));
	char url1[ABS_DL_URL_MAX];
	snprintf(url1, sizeof(url1), "%s/api/items/mock-book/file/1", live_server);
	char url2[ABS_DL_URL_MAX];
	snprintf(url2, sizeof(url2), "%s/api/items/mock-book/file/2", live_server);
	char dest_abs[1300];
	snprintf(dest_abs, sizeof(dest_abs), "%.700s/%.500s", card, want_dest);
	char final1[1400], final2[1400], part1_file[1420], part2_file[1420];
	media_path_for(card, want_dest, "001 - Part One.mp3", final1, sizeof(final1));
	media_path_for(card, want_dest, "002 - Part Two.mp3", final2, sizeof(final2));
	snprintf(part1_file, sizeof(part1_file), "%.1300s.part", final1);
	snprintf(part2_file, sizeof(part2_file), "%.1300s.part", final2);

	// ------------------------------------------------------------------
	// §8.8 >2 GiB Content-Length probe (forge-m6 fixes wave): the mock
	// stages a header no 32-bit long can hold on /file/gi2g. The framing must
	// parse the claim as long long end to end -- a 32-bit-`long` MIPS strtol
	// would wrap it -- then read exactly what arrived and end the read
	// honestly at the staged close. Every byte counter further down the
	// transfer contract leans on this never wrapping.
	// ------------------------------------------------------------------
	{
		char giant_url[ABS_DL_URL_MAX];
		snprintf(giant_url, sizeof(giant_url), "%s/api/items/mock-book/file/gi2g", live_server);
		// The session's own Bearer: the mock answers 401 to an anonymous
		// probe, exactly as the ops' real transfers would hit.
		char giant_headers[128];
		snprintf(giant_headers, sizeof(giant_headers), "Authorization: Bearer %s\r\n", mock_token);
		http_stream_t giant;
		CHECK(http_stream_open_ex(&giant, giant_url, giant_headers, 30),
			  "the staged >2 GiB probe stream did not open\n");
		CHECK(giant.content_length == 3000000000LL,
			  "the >2 GiB Content-Length parsed as %lld, wanted exactly 3000000000: a 32-bit-long wrap\n",
			  giant.content_length);
		char giant_buf[8192];
		int giant_total = 0, giant_got;
		while ((giant_got = http_stream_read(&giant, giant_buf, sizeof(giant_buf))) > 0) {
			CHECK(giant_total + giant_got <= 4096,
				  "the staged prefix grew past its exact 4096 bytes (read %d at %d)\n", giant_got,
				  giant_total);
			giant_total += giant_got;
		}
		CHECK(giant_total == 4096,
			  "the staged prefix delivered %d bytes, wanted the exact 4096\n", giant_total);
		http_stream_close(&giant);
		printf("live: >2 GiB Content-Length probe ok\n");
	}

	// ------------------------------------------------------------------
	// Phase 1 — direct driver (§8.2 steps 1–4, engine-not-involved)
	// ------------------------------------------------------------------
	long long direct_job = enqueue_book(live_profile, live_server, "mock-book", "The Mock Journey");

	abs_auth_snapshot_t held;
	memset(&held, 0, sizeof(held));
	CHECK(ops->auth_acquire(NULL, live_profile, live_server, &held), "live auth failed\n");
	CHECK(strcmp(held.token, mock_token) == 0, "live snapshot token mismatch\n");

	abs_download_plan_t plan;
	memset(&plan, 0, sizeof(plan));
	const char *stage = "";
	char error[512];
	abs_download_job_t jobrow = snapshot_job(direct_job);
	volatile int direct_cancel = 0;
	abs_download_op_ctx_t ctx = {.job_id = direct_job, .state = ABS_DL_QUEUED,
								 .cancel = &direct_cancel, .user = NULL};
	CHECK(ops->fetch_identity(NULL, &ctx, &jobrow, &plan, &stage, error, sizeof(error)),
		  "live fetch failed: %s (%s)\n", error, stage);

	CHECK(plan.destination && strcmp(plan.destination, want_dest) == 0,
		  "live destination mismatch:\n got %s\n want %s\n", plan.destination, want_dest);
	CHECK(plan.file_count == 2, "live plan file count %d\n", plan.file_count);
	CHECK(strcmp(plan.files[0].source_url, url1) == 0, "live url0 %s\n", plan.files[0].source_url);
	CHECK(strcmp(plan.files[1].source_url, url2) == 0, "live url1 %s\n", plan.files[1].source_url);
	CHECK(strcmp(plan.files[0].final_name, "001 - Part One.mp3") == 0, "live name0 %s\n",
		  plan.files[0].final_name);
	CHECK(strcmp(plan.files[1].final_name, "002 - Part Two.mp3") == 0, "live name1 %s\n",
		  plan.files[1].final_name);
	// The fixture states no sizes for mock-book; the plan must stay unknown
	// and let Content-Length govern the exact-length check.
	CHECK(plan.files[0].expected_size == -1 && plan.files[1].expected_size == -1,
		  "expected sizes %lld/%lld must stay unknown\n", plan.files[0].expected_size,
		  plan.files[1].expected_size);
	CHECK(plan.files[0].start_offset == 0.0 && plan.files[1].start_offset == 10.0, "offsets\n");
	CHECK(plan.files[0].duration == 10.0 && plan.files[1].duration == 10.0, "durations\n");
	CHECK(plan.remote_progress > 0.0, "remote progress %f\n", plan.remote_progress);
	CHECK(!plan.remote_finished, "remote finished\n");

	// The resume mapping the DONE rows must carry, captured from the phase-1
	// fetch snapshot. The plan strings belong to the adapter attempt and die
	// with auth_release, so every value that outlives phase 1 is copied here,
	// while it is live.
	char resume_name[8][ABS_DL_NAME_MAX];
	double resume_offset[8], resume_duration[8];
	long long resume_expected[8];
	int resume_count = plan.file_count;
	double snap_progress = plan.remote_progress;
	int snap_finished = plan.remote_finished;
	abs_download_file_req_t saved_plan[8];
	CHECK(resume_count >= 1 && resume_count <= 8, "fixture tracks %d\n", resume_count);
	for (int i = 0; i < resume_count; i++) {
		snprintf(resume_name[i], sizeof(resume_name[i]), "%s", plan.files[i].final_name);
		resume_offset[i] = plan.files[i].start_offset;
		resume_duration[i] = plan.files[i].duration;
		resume_expected[i] = plan.files[i].expected_size;
		saved_plan[i].source_url = url1; // unread by the resume mapper
		saved_plan[i].final_name = resume_name[i];
		saved_plan[i].start_offset = resume_offset[i];
		saved_plan[i].duration = resume_duration[i];
		saved_plan[i].expected_size = resume_expected[i];
	}
	char want_resume_name[8][ABS_DL_NAME_MAX];
	double want_resume_seconds[8];
	expected_resume(saved_plan, resume_count, snap_progress, snap_finished, want_resume_name[0],
					sizeof(want_resume_name[0]), &want_resume_seconds[0]);

	abs_download_file_t row1;
	memset(&row1, 0, sizeof(row1));
	row1.job_id = direct_job;
	row1.ordinal = 1;
	snprintf(row1.source_url, sizeof(row1.source_url), "%s", plan.files[0].source_url);
	snprintf(row1.final_name, sizeof(row1.final_name), "%s", plan.files[0].final_name);
	row1.expected_size = plan.files[0].expected_size;
	abs_download_file_t row2 = row1;
	row2.ordinal = 2;
	snprintf(row2.source_url, sizeof(row2.source_url), "%s", plan.files[1].source_url);
	snprintf(row2.final_name, sizeof(row2.final_name), "%s", plan.files[1].final_name);
	row2.expected_size = plan.files[1].expected_size;

	// A preset flag stops the loop at its first poll unit: the op must honour
	// an externally set cancel between units, retain the .part, and commit
	// nothing.
	direct_cancel = 1;
	long long size_out = -1;
	CHECK(!ops->file_download(NULL, &ctx, &row1, &size_out, &stage, error, sizeof(error)),
		  "the preset cancel must stop the transfer\n");
	CHECK(file_exists_abs(part1_file), "the stopped transfer must retain its .part (§8.1 pause)\n");
	CHECK(file_bytes_abs(part1_file) == 0, "a first-unit stop leaves an empty .part: %lld\n",
		  file_bytes_abs(part1_file));
	CHECK(!file_exists_abs(final1), "no final file may appear for a cancelled transfer\n");
	direct_cancel = 0;

	// The next attempt truncates the orphaned .part from byte zero (§8.2) and
	// completes for real.
	CHECK(ops->file_download(NULL, &ctx, &row1, &size_out, &stage, error, sizeof(error)),
		  "the restarted transfer failed: %s (%s)\n", error, stage);
	CHECK(size_out == (long long)part1_len, "final size %lld wants %zu\n", size_out, part1_len);
	CHECK(!file_exists_abs(part1_file), "the completed .part must be gone\n");
	{
		char *got = NULL;
		size_t got_len = 0;
		CHECK(read_bytes_abs(final1, &got, &got_len), "cannot read back %s\n", final1);
		CHECK(got_len == part1_len && memcmp(got, part1, got_len) == 0, "part1 bytes differ\n");
		free(got);
	}

	CHECK(ops->file_download(NULL, &ctx, &row2, &size_out, &stage, error, sizeof(error)),
		  "the second transfer failed: %s (%s)\n", error, stage);
	{
		char *got = NULL;
		size_t got_len = 0;
		CHECK(read_bytes_abs(final2, &got, &got_len), "cannot read back %s\n", final2);
		CHECK(got_len == part2_len && memcmp(got, part2, got_len) == 0, "part2 bytes differ\n");
		free(got);
	}

	// Cross-origin refusal before ANY connection: localhost is a different
	// origin from 127.0.0.1 even though the mock would answer it.
	{
		const char *host = strstr(live_server, "127.0.0.1");
		CHECK(host != NULL, "the runner must address the mock as 127.0.0.1\n");
		char cross_origin[ABS_DL_URL_MAX];
		snprintf(cross_origin, sizeof(cross_origin), "http://localhost:%s", host + strlen("127.0.0.1"));
		size_t path_start = strlen(cross_origin);
		snprintf(cross_origin + path_start, sizeof(cross_origin) - path_start,
				 "/api/items/mock-book/file/1");
		abs_download_file_t rowx = row1;
		snprintf(rowx.source_url, sizeof(rowx.source_url), "%s", cross_origin);
		long long media1_before = count_log_lines_with("/api/items/mock-book/file/1");
		CHECK(!ops->file_download(NULL, &ctx, &rowx, &size_out, &stage, error, sizeof(error)),
			  "a cross-origin transfer must be refused\n");
		CHECK(strstr(error, "another server") != NULL, "origin message missing: %s\n", error);
		CHECK(count_log_lines_with("/api/items/mock-book/file/1") == media1_before,
			  "the origin refusal must not touch the socket\n");
	}
	ops->auth_release(NULL, &held);

	// After the release the attempt is gone: the next fetch refuses at auth.
	{
		abs_download_plan_t stale_plan;
		memset(&stale_plan, 0, sizeof(stale_plan));
		const char *stale_stage = "";
		CHECK(!ops->fetch_identity(NULL, &ctx, &jobrow, &stale_plan, &stale_stage, error,
								   sizeof(error)),
			  "a fetch without a held snapshot must refuse\n");
		CHECK(stale_stage && strcmp(stale_stage, "auth") == 0, "stage %s\n", stale_stage);
	}
	CHECK(abs_download_remove(direct_job, error, sizeof(error)),
		  "cannot remove the direct-flow row: %s\n", error);

	// ------------------------------------------------------------------
	// Phase 2 — engine run: retention, real finalize, DONE end to end
	// ------------------------------------------------------------------
	long long items_probe = count_log_lines_with("expanded=1&include=progress,authors");
	long long media1_probe = count_log_lines_with("/api/items/mock-book/file/1");
	long long media2_probe = count_log_lines_with("/api/items/mock-book/file/2");
	long long cover_probe = count_log_lines_with("/api/items/mock-book/cover");
	CHECK(items_probe == 1 && media1_probe == 2 && media2_probe == 1 && cover_probe == 0,
		  "unexpected baseline: items %lld media1 %lld media2 %lld cover %lld\n", items_probe,
		  media1_probe, media2_probe, cover_probe);

	// The REAL acknowledgement-reset proof: a planted AUDIOBOOK_ABS_SYNC row.
	char abs_book[1000];
	snprintf(abs_book, sizeof(abs_book), "%.990s", dest_abs);
	audiobook_db_open();
	checkpoint_plant(abs_book);

	long long job2 = enqueue_book(live_profile, live_server, "mock-book", "The Mock Journey");
	// §8.2 step 6: the FINALIZING commit holds the sync barrier before the
	// finalize op exposes anything.
	pump_to(job2, ABS_DL_FINALIZING);
	CHECK(abs_download_sync_blocked(want_dest), "the finalize barrier must hold live too\n");
	CHECK(pump_until_terminal(job2) == ABS_DL_DONE, "phase 2 state %s\n",
		  abs_download_state_name(job_state(job2)));
	abs_download_job_t seen2 = snapshot_job(job2);
	CHECK(seen2.error[0] == '\0', "DONE carries an error: %s\n", seen2.error);
	CHECK(seen2.ack_reset_done == 1 && seen2.index_pending == 1, "DONE flags ack %d index %d\n",
		  seen2.ack_reset_done, seen2.index_pending);
	CHECK(seen2.file_count == 2, "file count %d\n", seen2.file_count);
	CHECK(strcmp(seen2.resume_file, want_resume_name[0]) == 0, "resume file %.260s, wanted %.260s\n",
		  seen2.resume_file, want_resume_name[0]);
	CHECK(fabs(seen2.resume_seconds - want_resume_seconds[0]) < 0.001,
		  "resume seconds %f, wanted %f\n", seen2.resume_seconds, want_resume_seconds[0]);
	CHECK(!checkpoint_present(abs_book), "the DONE job must reset its acknowledgement row\n");

	{
		char *got = NULL;
		size_t got_len = 0;
		CHECK(read_bytes_abs(final1, &got, &got_len), "cannot read back %s\n", final1);
		CHECK(got_len == part1_len && memcmp(got, part1, got_len) == 0, "part1 bytes differ\n");
		free(got);
		got = NULL;
		CHECK(read_bytes_abs(final2, &got, &got_len), "cannot read back %s\n", final2);
		CHECK(got_len == part2_len && memcmp(got, part2, got_len) == 0, "part2 bytes differ\n");
		free(got);
	}
	{
		abs_download_file_t rows[8];
		int n = abs_download_job_files(job2, rows, 8);
		CHECK(n == 2, "rows %d\n", n);
		CHECK(strcmp(rows[0].state, "complete") == 0 && strcmp(rows[1].state, "complete") == 0,
			  "row states %s/%s\n", rows[0].state, rows[1].state);
		CHECK(rows[0].final_size == (long long)part1_len && rows[1].final_size == (long long)part2_len,
			  "committed sizes %lld/%lld\n", rows[0].final_size, rows[1].final_size);
		CHECK(strcmp(rows[0].source_url, url1) == 0 && strcmp(rows[1].source_url, url2) == 0,
			  "row urls %s / %s\n", rows[0].source_url, rows[1].source_url);
	}
	// Sidecars the finalize must have written: tags per track, the description
	// on the first, the cover (the mock serves one), the manifest LAST.
	{
		char tags1[1420], tags2[1420], description[1420];
		snprintf(tags1, sizeof(tags1), "%.1300s.tags", final1);
		snprintf(tags2, sizeof(tags2), "%.1300s.tags", final2);
		snprintf(description, sizeof(description), "%.1300s.description", final1);
		char *bytes = NULL;
		size_t len = 0;
		CHECK(read_bytes_abs(tags1, &bytes, &len), "track-one .tags missing\n");
		CHECK(strstr(bytes, "title=Part One\n") && strstr(bytes, "artist=Test Author\n") &&
				  strstr(bytes, "album=The Mock Journey\n") && strstr(bytes, "series=Fixture Series\n") &&
				  strstr(bytes, "series_part=2\n") && strstr(bytes, "track=1\n") &&
				  strstr(bytes, "abs_item_id=mock-book\n"),
			  "track-one .tags fields wrong: %s\n", bytes);
		free(bytes);
		CHECK(read_bytes_abs(tags2, &bytes, &len), "track-two .tags missing\n");
		CHECK(strstr(bytes, "track=2\n") != NULL, "track-two numbering wrong: %s\n", bytes);
		free(bytes);
		CHECK(read_bytes_abs(description, &bytes, &len), "first-track .description missing\n");
		CHECK(memmem(bytes, len, "A downloaded fixture used to prove resume import.",
					 strlen("A downloaded fixture used to prove resume import.")) != NULL,
			  "description bytes differ\n");
		free(bytes);
		char cover_path[1500];
		snprintf(cover_path, sizeof(cover_path), "%.1400s/cover.jpg", dest_abs);
		CHECK(file_exists_abs(cover_path) && file_bytes_abs(cover_path) > 0, "live cover missing\n");
	}
	{
		manifest_view_t mv;
		char manifest_path[1420];
		snprintf(manifest_path, sizeof(manifest_path), "%.1300s/.audiobookshelf", dest_abs);
		read_manifest(manifest_path, &mv);
		CHECK(mv.present && strcmp(mv.version, "1") == 0, "manifest present/version\n");
		CHECK(strcmp(mv.item_id, "mock-book") == 0, "manifest item %s\n", mv.item_id);
		char normalized_live[ABS_SERVER_MAX];
		abs_profiles_normalize_server(live_server, normalized_live, sizeof(normalized_live));
		CHECK(strcmp(mv.server, normalized_live) == 0, "manifest server %s\n", mv.server);
		CHECK(strcmp(mv.duration, "20.000") == 0, "manifest duration %s\n", mv.duration);
		CHECK(strcmp(mv.progress, "13.000") == 0, "manifest progress %s\n", mv.progress);
		CHECK(strcmp(mv.progress_updated, "1700000000000") == 0, "manifest stamp %s\n",
			  mv.progress_updated);
		CHECK(mv.track_count == 2, "manifest tracks %d\n", mv.track_count);
		CHECK(mv.tracks[0].no == 1 && strcmp(mv.tracks[0].name, "001 - Part One.mp3") == 0 &&
				  fabs(mv.tracks[0].offset) < 1e-9 && fabs(mv.tracks[0].duration - 10.0) < 1e-9,
			  "manifest track one\n");
		CHECK(mv.tracks[1].no == 2 && strcmp(mv.tracks[1].name, "002 - Part Two.mp3") == 0 &&
				  fabs(mv.tracks[1].offset - 10.0) < 1e-9 && fabs(mv.tracks[1].duration - 10.0) < 1e-9,
			  "manifest track two\n");
	}
	CHECK(!abs_download_sync_blocked(want_dest), "the DONE transaction must release the barrier\n");
	CHECK(count_log_lines_with("/api/items/mock-book/file/1") == media1_probe,
		  "retention must not re-fetch file 1\n");
	CHECK(count_log_lines_with("/api/items/mock-book/file/2") == media2_probe,
		  "retention must not re-fetch file 2\n");
	CHECK(count_log_lines_with("/api/items/mock-book?expanded") == items_probe + 1,
		  "the engine fetch is one items request, got %lld\n",
		  count_log_lines_with("/api/items/mock-book?expanded"));
	CHECK(count_log_lines_with("/api/items/mock-book/cover") == cover_probe + 1,
		  "the finalize must fetch the cover once, got %lld\n",
		  count_log_lines_with("/api/items/mock-book/cover"));

	// Reopen the card: a DONE row is terminal — no second reset, no traffic.
	queue_stop();
	checkpoint_plant(abs_book);
	long long items_before_reopen = items_probe + 1;
	queue_init();
	CHECK(job_state(job2) == ABS_DL_DONE, "reopened state %s\n", abs_download_state_name(job_state(job2)));
	CHECK(checkpoint_present(abs_book), "a DONE acknowledgement must never be reset again\n");
	CHECK(count_log_lines_with("expanded=1&include=progress,authors") == items_before_reopen,
		  "reopening must not touch the server\n");
	CHECK(!abs_download_sync_blocked(want_dest), "no barrier may linger after DONE\n");

	// ------------------------------------------------------------------
	// Phase 3 — fresh engine transfer under a mid-transfer §8.1 pause
	// ------------------------------------------------------------------
	char retry_error[256] = {0};
	CHECK(unlink(final1) == 0 && unlink(final2) == 0, "cannot clear the completed files\n");
	long long job3 = enqueue_book(live_profile, live_server, "mock-book", "The Mock Journey");
	pause_watcher_t watcher = {.job_id = job3, .threshold = 128 * 1024, .fired = false};
	CHECK(pthread_create(&watcher.thread, NULL, pause_watcher_main, &watcher) == 0, "watcher\n");
	abs_download_state_t state3 = pump_until_terminal(job3);
	// Join before reading the watcher's flag: the watcher stops the moment
	// its pause lands, so the join is short, and it is the memory fence TSan
	// needs for `fired`.
	pthread_join(watcher.thread, NULL);
	CHECK(watcher.fired, "the watcher must fire while the 16 MiB track transfers\n");
	CHECK(state3 == ABS_DL_PAUSED, "phase 3 state %s\n", abs_download_state_name(state3));
	abs_download_job_t seen3 = snapshot_job(job3);
	CHECK(seen3.file_index == 0, "the pause must be on file 1, index %d\n", seen3.file_index);
	CHECK(file_exists_abs(part1_file), "the paused .part must be retained\n");
	long long paused_part = file_bytes_abs(part1_file);
	CHECK(paused_part > 0 && paused_part < (long long)part1_len, "partial .part %lld\n",
		  paused_part);
	CHECK(!file_exists_abs(final1), "no final file for a paused first file\n");

	retry_error[0] = '\0';
	CHECK(abs_download_resume(job3, retry_error, sizeof(retry_error)), "resume failed: %s\n",
		  retry_error);
	CHECK(pump_until_terminal(job3) == ABS_DL_DONE, "resumed state\n");
	seen3 = snapshot_job(job3);
	CHECK(seen3.error[0] == '\0', "resumed DONE carries an error: %s\n", seen3.error);
	CHECK(seen3.ack_reset_done == 1 && seen3.index_pending == 1, "resumed DONE flags\n");
	CHECK(strcmp(seen3.resume_file, want_resume_name[0]) == 0,
		  "resumed resume file %.260s, wanted %.260s\n", seen3.resume_file, want_resume_name[0]);
	CHECK(fabs(seen3.resume_seconds - want_resume_seconds[0]) < 0.001,
		  "resumed resume seconds %f, wanted %f\n", seen3.resume_seconds, want_resume_seconds[0]);
	{
		char *got = NULL;
		size_t got_len = 0;
		CHECK(read_bytes_abs(final1, &got, &got_len), "cannot read back %s\n", final1);
		CHECK(got_len == part1_len && memcmp(got, part1, got_len) == 0,
			  "the redownloaded part1 differs (restart-from-zero)\n");
		free(got);
		got = NULL;
		CHECK(read_bytes_abs(final2, &got, &got_len), "cannot read back %s\n", final2);
		CHECK(got_len == part2_len && memcmp(got, part2, got_len) == 0, "part2 bytes differ\n");
		free(got);
	}
	CHECK(!file_exists_abs(part1_file), "the completed .part must be gone\n");
	// Wait: the final .part policy — file 2 completed cleanly too.
	CHECK(count_log_lines_with("/api/items/mock-book/file/1") == media1_probe + 2,
		  "file 1 must be transferred twice here (interrupted + restart), got %lld\n",
		  count_log_lines_with("/api/items/mock-book/file/1"));
	CHECK(count_log_lines_with("/api/items/mock-book/file/2") == media2_probe + 1,
		  "file 2 once, got %lld\n", count_log_lines_with("/api/items/mock-book/file/2"));
	CHECK(count_log_lines_with("/api/items/mock-book?expanded") == items_probe + 3,
		  "identity fetches: job3 + resume (job2's was counted), got %lld\n",
		  count_log_lines_with("/api/items/mock-book?expanded"));
	// The resumed job's finalize kept the cover (skip-if-exists, no request)
	// and left a valid manifest.
	CHECK(count_log_lines_with("/api/items/mock-book/cover") == cover_probe + 1,
		  "the retained cover must not be re-fetched, got %lld\n",
		  count_log_lines_with("/api/items/mock-book/cover"));

	// ------------------------------------------------------------------
	// Phase 4 — negatives
	// ------------------------------------------------------------------
	// An episode job is refused by the adapter until the podcast wave.
	long long ep_job = enqueue_episode(live_profile, live_server, "pod-41",
									   "Fixture Forty-One Podcast", "");
	CHECK(pump_until_terminal(ep_job) == ABS_DL_FAILED, "episode state\n");
	abs_download_job_t seen_ep = snapshot_job(ep_job);
	CHECK(strcmp(seen_ep.error_stage, "metadata") == 0, "stage %s\n", seen_ep.error_stage);
	CHECK(strstr(seen_ep.error, "not wired yet") != NULL, "episode refusal: %s\n", seen_ep.error);
	CHECK(count_log_lines_with("expanded=1&include=progress,authors") == items_probe + 3,
		  "the episode refusal must not fetch anything, got %lld\n",
		  count_log_lines_with("expanded=1&include=progress,authors"));

	// An unresolvable profile fails at the auth stage before any request.
	long long auth_job = enqueue_book("p9", live_server, "pod-41", "Fixture Forty-One Podcast");
	CHECK(pump_until_terminal(auth_job) == ABS_DL_FAILED, "auth-failure state\n");
	abs_download_job_t seen_auth = snapshot_job(auth_job);
	CHECK(strcmp(seen_auth.error_stage, "auth") == 0, "stage %s\n", seen_auth.error_stage);
	CHECK(count_log_lines_with("expanded=1&include=progress,authors") == items_probe + 3,
		  "a refused auth must not fetch, got %lld\n",
		  count_log_lines_with("expanded=1&include=progress,authors"));

	// ------------------------------------------------------------------
	// Phase 5 — §8.4 line 568 / line 1180 status visibility: refused tokens
	// and vanished items are permanent FAILED jobs with their own stage, no
	// retry, and nothing local deleted.
	// ------------------------------------------------------------------
	{
		// 401: the job's own profile rotates to a token the server rejects
		// (a duplicate-server twin profile is refused by the store by design —
		// a job MUST NOT be resolvable to another profile's token, §11.1).
		// The failure must be auth-staged, terminal, and requested exactly
		// once: no retry with another profile/token, nothing local deleted.
		char bad_token[ABS_TOKEN_MAX];
		snprintf(bad_token, sizeof(bad_token), "rotated-away-%.32s", mock_token);
		CHECK(abs_profiles_edit(live_profile, NULL, bad_token), "cannot rotate the profile token\n");
		long long items_before_401 = count_log_lines_with("expanded=1&include=progress,authors");
		// A distinct item id: the smoke runner greps the LAST mock-book row
		// as its DONE check, and this refusal must never displace it.
		long long auth401 = enqueue_book(live_profile, live_server, "rotated-token-item", "Rotated Token");
		CHECK(pump_until_terminal(auth401) == ABS_DL_FAILED, "the rotated-token job must fail\n");
		abs_download_job_t seen_401 = snapshot_job(auth401);
		CHECK(strcmp(seen_401.error_stage, "auth") == 0, "401 stage %s\n", seen_401.error_stage);
		CHECK(strstr(seen_401.error, "credentials need editing") != NULL, "401 message: %s\n",
			  seen_401.error);
		CHECK(count_log_lines_with("expanded=1&include=progress,authors") == items_before_401 + 1,
			  "the refused fetch reached the server exactly once, got %lld of %lld\n",
			  count_log_lines_with("expanded=1&include=progress,authors"), items_before_401 + 1);
		for (int settle = 0; settle < 8; settle++) {
			abs_download_pump();
		}
		CHECK(count_log_lines_with("expanded=1&include=progress,authors") == items_before_401 + 1,
			  "an auth-failed job must never retry with another profile/token\n");
		CHECK(job_state(auth401) == ABS_DL_FAILED, "the auth failure must stay terminal\n");
		abs_download_file_t rows_401[2];
		CHECK(abs_download_job_files(auth401, rows_401, 2) == 0, "an auth failure wrote no file rows\n");
		seen_401 = snapshot_job(auth401);
		CHECK(seen_401.destination[0] == '\0', "a refused fetch must not claim a destination\n");
		// Restore the working token for the 404 probe (and the final state).
		CHECK(abs_profiles_edit(live_profile, NULL, mock_token), "cannot restore the token\n");

		// 404 item: permanent missing-media failure with its own message; the
		// FAILED row carries no file records and touches nothing local.
		long long items_before_404 = count_log_lines_with("expanded=1&include=progress,authors");
		long long missing404 = enqueue_book(live_profile, live_server, "no-such-item", "Missing Item");
		CHECK(pump_until_terminal(missing404) == ABS_DL_FAILED, "the vanished item must fail\n");
		abs_download_job_t seen_404 = snapshot_job(missing404);
		CHECK(strcmp(seen_404.error_stage, "metadata") == 0, "404 stage %s\n", seen_404.error_stage);
		CHECK(strstr(seen_404.error, "no longer on the server") != NULL, "404 message: %s\n",
			  seen_404.error);
		CHECK(count_log_lines_with("expanded=1&include=progress,authors") == items_before_404 + 1,
			  "the vanished item is requested exactly once\n");
		CHECK(job_state(missing404) == ABS_DL_FAILED, "the 404 failure must stay terminal\n");
		abs_download_file_t rows_404[2];
		CHECK(abs_download_job_files(missing404, rows_404, 2) == 0, "a 404 wrote no file rows\n");
		seen_404 = snapshot_job(missing404);
		CHECK(seen_404.destination[0] == '\0', "a failed fetch must not claim a destination\n");
	}

	// ------------------------------------------------------------------
	// Phase 6 — §8.3 scan-completion import over the REAL audiobook index,
	// and the §8.2 step 6 reconcile gate against the real progress worker.
	//
	// a. Before any scan exists, the per-row import refuses on both DONE rows
	//    (job2 and job3) and leaves them DONE + index_pending.
	// b. The real Audiobooks scan completes; the §8.5 line-592 notification
	//    wired through audiobookshelf_queue_import_install() runs the
	//    coordinator on the scan thread and both rows clear, having written
	//    exactly their stored resume values into audiobookdb.
	// c. While a second re-download sits inside its FINALIZING sync barrier,
	//    the progress worker's newer-remote import must SKIP the destination;
	//    the pass runs (the request log proves it) and writes nothing.
	// d. After the DONE transaction releases the barrier, the same newer
	//    remote import goes through, and job4's own row imports from the
	//    coordinator. The media transfer count never moves: no re-download.
	// ------------------------------------------------------------------
	printf("transfer test: live scan-completion import... ");
	audiobookshelf_queue_import_install();
	char import_error[256] = {0};
	CHECK(abs_download_import_pending(import_error, sizeof(import_error)) == 0, "no index yet\n");
	CHECK(strstr(import_error, "not indexed") != NULL, "refusal text: %s\n", import_error);
	CHECK(job_state(job2) == ABS_DL_DONE && job_state(job3) == ABS_DL_DONE,
		  "a refused import must keep DONE\n");
	{
		abs_download_job_t p2 = snapshot_job(job2);
		abs_download_job_t p3 = snapshot_job(job3);
		CHECK(p2.index_pending == 1 && p3.index_pending == 1,
			  "refused rows must stay importable\n");
	}
	long long media1_before_import = count_log_lines_with("/api/items/mock-book/file/1");
	long long media2_before_import = count_log_lines_with("/api/items/mock-book/file/2");

	// (b) the real scan; the listener chain does the importing
	char scan_root[600];
	snprintf(scan_root, sizeof(scan_root), "%.580s/Audiobooks", card);
	CHECK(audiobookdb_scan_start(scan_root), "the audiobook scan refused\n");
	for (int i = 0; i < 4000 && audiobookdb_scan_running(); i++) {
		usleep(5000);
	}
	CHECK(!audiobookdb_scan_running(), "the scan never finished\n");
	{
		abs_download_snapshot_t snap;
		CHECK(abs_download_snapshot(&snap), "snapshot after the import pass\n");
		for (int k = 0; k < snap.job_count; k++) {
			if (snap.jobs[k].state == ABS_DL_DONE) {
				CHECK(snap.jobs[k].index_pending == 0, "row %lld never imported\n",
					  snap.jobs[k].id);
			}
		}
	}
	double import_seconds = -1;
	bool import_finished = true;
	char import_file[600];
	CHECK(audiobook_resume_row(abs_book, import_file, sizeof(import_file), &import_seconds,
							   &import_finished),
		  "the imported book must be in the index\n");
	CHECK(strcmp(import_file, final2) == 0, "imported resume file %s, wanted %s\n", import_file,
		  final2);
	CHECK(fabs(import_seconds - want_resume_seconds[0]) < 0.001,
		  "imported resume seconds %f, wanted %f\n", import_seconds, want_resume_seconds[0]);
	CHECK(!import_finished, "an unfinished download must not mark the book finished\n");
	{
		char srv[ABS_SERVER_MAX], f[SONIX_LINKED_DB_PATH_MAX];
		int linked = 0;
		long long stamp = 0;
		bool fin = true;
		double s = -1;
		CHECK(audiobook_sync_row(abs_book, srv, sizeof(srv), f, sizeof(f), &s, &fin, &stamp,
								 &linked),
			  "the compare-and-swap sync row must exist\n");
		char normalized_live[ABS_SERVER_MAX];
		abs_profiles_normalize_server(live_server, normalized_live, sizeof(normalized_live));
		CHECK(linked == 1 && strcmp(srv, normalized_live) == 0 && strcmp(f, final2) == 0 &&
				  fabs(s - want_resume_seconds[0]) < 0.001 && !fin && stamp == 1700000000000LL,
			  "sync row drifted: %s %s %f %lld\n", srv, f, s, stamp);
	}
	printf("ok (import)... ");

	// (c) the reconcile gate: a held FINALIZING barrier must make the sync
	// worker skip the destination. The override progress file is the mock's
	// external channel for mock-book (the same one the smoke's conflict test
	// uses), and a stamp newer than the imported one turns the next
	// reconciliation into the local-progress import this gate guards.
	char progress_override[1000];
	snprintf(progress_override, sizeof(progress_override), "%.700s/progress.json", work);
	char progress_json[128];
	snprintf(progress_json, sizeof(progress_json),
			 "{\"currentTime\":5.0,\"isFinished\":false,\"lastUpdate\":%lld}",
			 (long long)time(NULL) * 1000 + 1000);
	write_bytes_abs(progress_override, progress_json, strlen(progress_json));

	double pre_seconds = -1;
	bool pre_finished = true;
	char pre_file[600];
	CHECK(audiobook_resume_row(abs_book, pre_file, sizeof(pre_file), &pre_seconds, &pre_finished),
		  "the imported resume row must exist before the gate\n");

	long long job4 = enqueue_book(live_profile, live_server, "mock-book", "The Mock Journey");
	pump_to(job4, ABS_DL_FINALIZING); // barrier held; the finalize op has not run
	CHECK(abs_download_sync_blocked(want_dest), "the re-download must hold the barrier\n");
	long long gets_before = count_log_lines_with("/api/me/progress/mock-book");
	// §11.4 step 6: the immediate hint re-arms the row (its fresh
	// acknowledgement would otherwise be skipped as recently attempted) and
	// wakes the worker for one pass.
	audiobookshelf_queue_progress(abs_book, final2, want_resume_seconds[0], false, true);
	bool pass_ran = false;
	for (int i = 0; i < 5000 && !pass_ran; i++) {
		if (count_log_lines_with("/api/me/progress/mock-book") > gets_before) {
			pass_ran = true;
		}
		usleep(1000);
	}
	CHECK(pass_ran, "the sync worker never ran a pass while the barrier was held\n");
	{
		char f[600];
		double s = -1;
		bool fin = true;
		CHECK(audiobook_resume_row(abs_book, f, sizeof(f), &s, &fin),
			  "the resume row vanished under the gate\n");
		CHECK(strcmp(f, final2) == 0 && fabs(s - want_resume_seconds[0]) < 0.001,
			  "the gated worker must not write local progress (got %s %f)\n", f, s);
	}

	// (d) the DONE transaction releases the barrier; the same newer remote
	// import must then go through, and job4's own row imports from the queue.
	CHECK(pump_until_terminal(job4) == ABS_DL_DONE, "the re-download must reach DONE\n");
	CHECK(!abs_download_sync_blocked(want_dest), "the DONE transaction must release the barrier\n");
	CHECK(abs_download_import_pending(import_error, sizeof(import_error)) == 1,
		  "the released row must import\n");
	gets_before = count_log_lines_with("/api/me/progress/mock-book");
	audiobookshelf_queue_progress(abs_book, final2, want_resume_seconds[0], false, true);
	bool imported_remote = false;
	for (int i = 0; i < 8000 && !imported_remote; i++) {
		char f[600];
		double s = -1;
		bool fin = true;
		if (audiobook_resume_row(abs_book, f, sizeof(f), &s, &fin) && strcmp(f, final1) == 0 &&
			fabs(s - 5.0) < 0.001) {
			imported_remote = true;
		}
		usleep(2000);
	}
	CHECK(imported_remote, "after the barrier released, the newer remote must import\n");
	{
		char srv[ABS_SERVER_MAX], f[SONIX_LINKED_DB_PATH_MAX];
		int linked = 0;
		long long stamp = 0;
		bool fin = true;
		double s = -1;
		CHECK(audiobook_sync_row(abs_book, srv, sizeof(srv), f, sizeof(f), &s, &fin, &stamp,
								 &linked),
			  "the acknowledged sync row must exist after the worker import\n");
		CHECK(strcmp(f, final1) == 0 && fabs(s - 5.0) < 0.001 && !fin && linked == 1,
			  "worker-imported sync row drifted: %s %f\n", f, s);
		// The remote stamp carried through the import: the worker's CAS wrote
		// exactly the override's lastUpdate, and the clock may have ticked a
		// little since the override was written.
		CHECK(llabs(stamp - ((long long)time(NULL) * 1000 + 1000)) < 60000,
			  "the remote stamp must have carried through: %lld\n", stamp);
	}
	// No re-download anywhere in the import/gate phase.
	CHECK(count_log_lines_with("/api/items/mock-book/file/1") == media1_before_import &&
			  count_log_lines_with("/api/items/mock-book/file/2") == media2_before_import,
		  "the import and gate phases must never re-download (file1 %lld, file2 %lld)\n",
		  count_log_lines_with("/api/items/mock-book/file/1"),
		  count_log_lines_with("/api/items/mock-book/file/2"));
	printf("ok (gate)... ");

	// ------------------------------------------------------------------
	// Token-leak scans: card tree, queue database, mock request log
	// ------------------------------------------------------------------
	{
		char hit[500];
		CHECK(!tree_contains(card, mock_token, hit, sizeof(hit)),
			  "the token leaked into the card: %s\n", hit);
		CHECK(!tree_contains(card, "Bearer ", hit, sizeof(hit)),
			  "a Bearer fragment leaked into the card: %s\n", hit);
		CHECK(!tree_contains(card, "token=", hit, sizeof(hit)),
			  "a credential-bearing URL leaked: %s\n", hit);
		char *log_bytes = NULL;
		size_t log_len = 0;
		CHECK(read_bytes_abs(request_log, &log_bytes, &log_len), "request log missing\n");
		CHECK(!memmem(log_bytes, log_len, mock_token, strlen(mock_token)),
			  "the token leaked into the request log\n");
		CHECK(!memmem(log_bytes, log_len, "Authorization", 13),
			  "an Authorization header leaked into the request log\n");
		free(log_bytes);
	}

	free(part1);
	free(part2);
	queue_stop();
	audiobookdb_close();
	printf("transfer test: live alias passed\n");
}

int main(int argc, char **argv) {
	if (argc != 2 || !argv[1] || !argv[1][0]) {
		fprintf(stderr, "usage: audiobookshelf-queue-transfer-test <workdir>\n");
		return 2;
	}
	snprintf(work, sizeof(work), "%.880s", argv[1]);
	live_run();
	return 0;
}
#endif // ABS_QUEUE_TRANSFER_LIVE
