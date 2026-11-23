#include "audiobookshelf.h"
#include "abs_profiles.h"
#include "abs_sync.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "src/system/core/config.h"
#include "src/system/core/json.h"
#include "src/system/core/pathlimits.h"
#include "src/system/core/sha256.h"
#include "src/system/core/utils.h"
#include "src/system/decode/decode.h"
#include "src/system/library/audiobookdb.h"
#include "src/system/playback/audiobook.h"
#include "src/system/net/http.h"
#include "src/system/net/wifi.h"

#define ABS_BODY_LIMIT (3u * 1024u * 1024u)
#define ABS_TIMEOUT_SECS 30
#define ABS_DOWNLOAD_CHUNK 32768
// The linked-media ceilings live in one place (pathlimits.h) so the download,
// the durable checkpoint rows and the progress watch slots can never drift
// apart; re-deriving local copies here is what produced "downloaded fine,
// truncated on resume" in the first place (§11.5).
#define ABS_LOCAL_PATH_MAX SONIX_LINKED_PATH_MAX
#define ABS_SIDECAR_PATH_MAX SONIX_LINKED_SIDECAR_PATH_MAX
#define ABS_TEMP_PATH_MAX (SONIX_LINKED_SIDECAR_PATH_MAX + 10)

// Legacy 0.5.0 credentials, kept only for the migration fallback (§11.1,
// §11.3): request code resolves credentials through the profile store via
// request_credentials_copy() and never reads these globals directly.
static char server_url[512];
static char bearer_token[1024];
static pthread_mutex_t credentials_lock = PTHREAD_MUTEX_INITIALIZER;
static __thread char last_error[256];

// §8.3: one large-JSON catalog fetch at a time. The detail page (and its
// download/link callers) and the download queue's expanded identity fetch are
// guarded by this mutex, so two 3 MB-class response bodies can never be
// buffered at once and double the transient envelope. Queue progress GET/PATCH
// never waits for it, and audio transfer never holds it (§8.3).
static pthread_mutex_t abs_catalog_lock = PTHREAD_MUTEX_INITIALIZER;

#define ABS_PROGRESS_PATH_MAX 1280

typedef struct {
	char book[ABS_PROGRESS_PATH_MAX];
	char file[ABS_PROGRESS_PATH_MAX];
	double seconds;
	bool finished;
} progress_job_t;

static pthread_once_t progress_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t progress_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t progress_cond = PTHREAD_COND_INITIALIZER;
static bool progress_worker_ready;
static bool progress_wake;
static bool progress_reconnect = true;
// These two carry the same strings the durable AUDIOBOOK_ABS_SYNC row does,
// so they use the durable-row ceiling (§11.5): a wider slot here would accept
// a watch key the checkpoint row cannot hold.
static char progress_watch_file[SONIX_LINKED_DB_PATH_MAX];

// §11.4 step 6: an immediate checkpoint wake is keyed by (normalized server,
// book) instead of one unqualified path, so a checkpoint on server B can no
// longer erase the pending immediate hint of server A. Four is the profile
// bound (§11.1); the slots drain every pass, and the touch stamp only decides
// which hint a pathological fifth same-pass burst replaces.
typedef struct {
	char server[ABS_SERVER_MAX];
	char book[SONIX_LINKED_DB_PATH_MAX];
	long stamp;
} progress_forced_t;

static progress_forced_t progress_forced[ABS_PROFILE_MAX];
static long progress_forced_stamp;
static audiobookshelf_sync_status_t progress_status = ABS_SYNC_CHECKING;
static void progress_worker_start(void);

static void set_error(const char *message) {
	snprintf(last_error, sizeof(last_error), "%s", message ? message : "");
}

const char *audiobookshelf_last_error(void) { return last_error; }

static void normalize_server(const char *server, char *out, size_t size) {
	if (!out || size == 0) return;
	out[0] = '\0';
	if (!server) return;
	while (isspace((unsigned char)*server)) server++;
	snprintf(out, size, "%s", server);
	size_t len = strlen(out);
	while (len && isspace((unsigned char)out[len - 1])) out[--len] = '\0';
	while (len > 0 && out[len - 1] == '/') out[--len] = '\0';
}

void audiobookshelf_init(void) {
	normalize_server(config_get("audiobookshelf", "server", ""), server_url, sizeof(server_url));
	snprintf(bearer_token, sizeof(bearer_token), "%s", config_get("audiobookshelf", "token", ""));
#ifdef HOST_BUILD
	// Test a server without ever putting its secret in a checkout or simulator
	// config file. Device builds intentionally have no environment override.
	const char *host_server = getenv("SONIX_AUDIOBOOKSHELF_SERVER");
	const char *host_token = getenv("SONIX_AUDIOBOOKSHELF_TOKEN");
	if (host_server && host_server[0]) normalize_server(host_server, server_url, sizeof(server_url));
	if (host_token && host_token[0]) snprintf(bearer_token, sizeof(bearer_token), "%s", host_token);
#endif
	printf("audiobookshelf: %s\n", audiobookshelf_configured() ? "configured" : "not configured");
	if (audiobookshelf_configured()) pthread_once(&progress_once, progress_worker_start);
}

// ---------------------------------------------------------------------------
// Explicit auth/request context (§11.3)
//
// Every request's credentials are resolved here, never raced out of the legacy
// globals:
//
//   * A request bound to a manifest server (progress sync) resolves the
//     configured profile that owns that normalized server, re-read under the
//     store lock immediately before the request (§11.4 step 4). A token the
//     editor just rotated wins the very next request, and an inactive profile
//     never borrows the active one's token.
//   * Unbound requests (browse/catalog) use the active profile.
//   * The legacy [audiobookshelf] globals are only a fallback for the
//     migration window (§11.1): no profile store configured at all, or a
//     present-but-incomplete active profile. Once any profile is configured,
//     a server with no owning profile resolves to "not ours" and the caller
//     keeps its checkpoint (ABS_SYNC_OTHER_SERVER territory, §11.4).
// ---------------------------------------------------------------------------
static bool request_credentials_copy(const char *expected_server,
									 char *server, size_t server_size,
									 char *token, size_t token_size) {
	if (server && server_size) server[0] = '\0';
	if (token && token_size) token[0] = '\0';
	if (expected_server && expected_server[0] && abs_profiles_any_configured()) {
		char expected[ABS_SERVER_MAX];
		abs_profiles_normalize_server(expected_server, expected, sizeof(expected));
		abs_profile_t profiles[ABS_PROFILE_MAX];
		int count = abs_profiles_list(profiles, ABS_PROFILE_MAX);
		for (int i = 0; i < count; i++) {
			char normalized[ABS_SERVER_MAX];
			abs_profiles_normalize_server(profiles[i].server, normalized, sizeof(normalized));
			if (!normalized[0] || strcmp(normalized, expected) != 0) continue;
			// Its own profile (even an inactive one, even with a rotated token)
			// owns these credentials. The snapshot is wiped before returning:
			// only the caller's short-lived copies keep the token bytes.
			abs_auth_snapshot_t snapshot;
			if (abs_profiles_snapshot(profiles[i].id, &snapshot)) {
				bool complete = snapshot.server[0] && snapshot.token[0];
				if (server && server_size) snprintf(server, server_size, "%s", snapshot.server);
				if (token && token_size) snprintf(token, token_size, "%s", snapshot.token);
				abs_profiles_snapshot_wipe(&snapshot);
				if (complete) return true;
				break; // owning profile lacks a token: do not fall through to the active one
			}
		}
		return false;
	}
	abs_auth_snapshot_t snapshot;
	memset(&snapshot, 0, sizeof(snapshot));
	bool from_active = abs_profiles_snapshot_active(&snapshot) &&
		snapshot.server[0] && snapshot.token[0];
	if (from_active) {
		if (server && server_size) snprintf(server, server_size, "%s", snapshot.server);
		if (token && token_size) snprintf(token, token_size, "%s", snapshot.token);
	}
	abs_profiles_snapshot_wipe(&snapshot);
	if (from_active) return true;
	// Migration fallback (§11.1): the saved 0.5.0 pair, while the profile store
	// still provides nothing the active id can authorize with.
	bool legacy = false;
	pthread_mutex_lock(&credentials_lock);
	if (server && server_size) snprintf(server, server_size, "%s", server_url);
	if (token && token_size) snprintf(token, token_size, "%s", bearer_token);
	legacy = server_url[0] != '\0' && bearer_token[0] != '\0';
	pthread_mutex_unlock(&credentials_lock);
	return legacy;
}

// What the progress worker waits on (§11.3): any configured profile, or the
// legacy pair during the migration window. An active but incomplete profile
// does not count -- there is nothing to authorize a pass with.
static bool sync_credentials_available(void) {
	if (abs_profiles_any_configured()) return true;
	pthread_mutex_lock(&credentials_lock);
	bool legacy = server_url[0] != '\0' && bearer_token[0] != '\0';
	pthread_mutex_unlock(&credentials_lock);
	return legacy;
}

// The multi-profile reading of ABS_SYNC_OTHER_SERVER (§11.4): the manifest's
// server is covered by no configured profile. With an entirely unconfigured
// store the comparison keeps its 0.5.0 meaning against the legacy identity.
static bool bound_server_known(const char *bound_server, const char *legacy_identity) {
	if (abs_profiles_any_configured()) return abs_profiles_has_server(bound_server);
	return legacy_identity[0] && strcmp(bound_server, legacy_identity) == 0;
}

// The server a 0.5.0 manifest without `server=` resolves to (§11.5): the
// persisted legacy_server, or -- only while no profile store exists at all --
// the sole configured legacy identity, which in that window IS the original
// binding. Never the currently active profile: a second profile must not
// inherit old manifests by happening to be selected.
static void legacy_manifest_binding(char *out, size_t size) {
	char legacy[ABS_SERVER_MAX], normalized[ABS_SERVER_MAX];
	abs_profiles_legacy_server(legacy, sizeof(legacy));
	abs_profiles_normalize_server(legacy, normalized, sizeof(normalized));
	snprintf(out, size, "%s", normalized);
	if (!normalized[0] && abs_profiles_count() == 0) {
		char identity[ABS_SERVER_MAX];
		if (request_credentials_copy(NULL, identity, sizeof(identity), NULL, 0))
			snprintf(out, size, "%s", identity);
	}
}

// §11.3: "configured" means a browsable request identity exists -- the active
// profile's complete server+token, or the legacy pair during the migration
// window. Sync independently uses sync_credentials_available() (all profiles).
bool audiobookshelf_configured(void) {
	char server[ABS_SERVER_MAX], token[ABS_TOKEN_MAX];
	bool configured = request_credentials_copy(NULL, server, sizeof(server), token, sizeof(token));
	// The token only ever sat in this scratch copy; wipe it on the way out.
	memset(token, 0, sizeof(token));
	return configured;
}
// Bounded display copy under the credentials lock (§11.3). The old function
// lent an unlocked pointer into a buffer another thread could rewrite; callers
// get an atomic snapshot of the text instead. Browse requests do not go
// through this -- api_request() resolves its own identity.
const char *audiobookshelf_server(void) {
	static char display[ABS_SERVER_MAX];
	pthread_mutex_lock(&credentials_lock);
	snprintf(display, sizeof(display), "%s", server_url);
	pthread_mutex_unlock(&credentials_lock);
	return display;
}

void audiobookshelf_set_credentials(const char *server, const char *token) {
	pthread_mutex_lock(&credentials_lock);
	normalize_server(server, server_url, sizeof(server_url));
	snprintf(bearer_token, sizeof(bearer_token), "%s", token ? token : "");
	config_set("audiobookshelf", "server", server_url);
	config_set("audiobookshelf", "token", bearer_token);
	pthread_mutex_unlock(&credentials_lock);
	config_save();
	if (audiobookshelf_configured()) pthread_once(&progress_once, progress_worker_start);
	pthread_mutex_lock(&progress_lock);
	progress_wake = true;
	pthread_cond_signal(&progress_cond);
	pthread_mutex_unlock(&progress_lock);
}

void audiobookshelf_clear_credentials(void) { audiobookshelf_set_credentials("", ""); }

static void error_from_response(int status, const char *body) {
	if (status == 401 || status == 403) {
		set_error("Audiobookshelf refused the token.");
		return;
	}
	if (body && body[0]) {
		json_doc_t doc;
		if (json_parse(body, &doc)) {
			char message[192] = "";
			int root = json_root(&doc);
			json_obj_str(&doc, root, "error", message, sizeof(message));
			if (!message[0]) json_obj_str(&doc, root, "message", message, sizeof(message));
			json_free(&doc);
			if (message[0]) {
				set_error(message);
				return;
			}
		}
	}
	char message[96];
	snprintf(message, sizeof(message), "Audiobookshelf answered %d.", status);
	set_error(message);
}

// One URL join + Bearer + buffered request (§11.3). `server`/`token` are the
// exact credential copies the caller resolved (the queue's §8.1 attempt
// snapshot, or api_request()'s own resolution below); never the mutable
// globals mid-request. `server` is the identity the URL is joined against.
// `op`, when given (the queue worker's attempt), carries the wake slot: a
// pause/cancel shut the in-flight request down instead of waiting out the
// receive timeout (§8.5 line 1028).
static bool api_request_with(const char *server, const char *token, const char *path,
							 const http_req_t *request, char **body, int timeout_secs, int *status_out,
							 http_op_t *op) {
	char url[ABS_URL_MAX];
	if (!server || !server[0] ||
		!path || (size_t)snprintf(url, sizeof(url), "%s%s", server, path) >= sizeof(url)) {
		// Bounded APIs (§10.3) refuse over-long queries before asking the
		// network; here the joined URL itself is over the ABS_URL_MAX ceiling.
		set_error("The request is too long for this server address.");
		return false;
	}
	char headers[ABS_TOKEN_MAX + 32];
	snprintf(headers, sizeof(headers), "Authorization: Bearer %s\r\n", token);
	http_req_t req = request ? *request : (http_req_t){0};
	req.extra_headers = headers;
	req.want_error_body = true;
	req.op = op; // NULL for every non-queue caller; the wake contract rides here

	int status = 0;
	bool received = http_request(url, &req, body, NULL, ABS_BODY_LIMIT, timeout_secs, &status);
	if (!received || status < 200 || status >= 300) {
		const char *why = http_last_error();
		if (status >= 400) error_from_response(status, body && *body ? *body : NULL);
		else set_error(why && why[0] ? why : "Audiobookshelf did not reply.");
		if (status_out) *status_out = status;
		return false;
	}
	if (status_out) *status_out = status;
	set_error("");
	return true;
}

// One URL join + Bearer + buffered request (§11.3). `expected_server`, when
// given, is the manifest's bound server: the immutable snapshot comparison of
// the old global-race check. Credentials always come from
// request_credentials_copy(), which re-reads the owning profile under the
// store lock immediately before the request.
static bool api_request(const char *path, const http_req_t *request, char **body, int *status_out,
						const char *expected_server) {
	char server[ABS_SERVER_MAX], token[ABS_TOKEN_MAX];
	if (!request_credentials_copy(expected_server, server, sizeof(server), token, sizeof(token)) ||
		(expected_server && strcmp(server, expected_server) != 0)) {
		memset(token, 0, sizeof(token));
		set_error("Audiobookshelf settings changed; checkpoint retained.");
		return false;
	}
	bool ok = api_request_with(server, token, path, request, body, expected_server ? 10 : ABS_TIMEOUT_SECS,
							   status_out, NULL);
	memset(token, 0, sizeof(token));
	return ok;
}

static bool update_progress(const char *item_id, double current, double duration, bool finished,
	long long updated_ms, const char *server) {
	char id[ABS_ID_MAX * 3];
	http_url_encode(item_id, id, sizeof(id));
	char path[ABS_ID_MAX * 3 + 32];
	snprintf(path, sizeof(path), "/api/me/progress/%s", id);
	char json[256], timestamp[64] = "";
	if (updated_ms > 0) snprintf(timestamp, sizeof(timestamp), ",\"lastUpdate\":%lld", updated_ms);
	snprintf(json, sizeof(json),
			 "{\"currentTime\":%.3f,\"duration\":%.3f,\"isFinished\":%s%s}", current, duration,
			 finished ? "true" : "false", timestamp);
	http_req_t req = {
		.method = "PATCH",
		.body = json,
		.content_type = "application/json",
		.want_error_body = true,
		.allow_empty_body = true,
	};
	char *body = NULL;
	bool ok = api_request(path, &req, &body, NULL, server);
	free(body);
	return ok;
}

static bool read_progress(const char *item_id, const char *server, abs_remote_progress_t *out) {
	memset(out, 0, sizeof(*out));
	char id[ABS_ID_MAX * 3], path[ABS_ID_MAX * 3 + 32];
	http_url_encode(item_id, id, sizeof(id));
	snprintf(path, sizeof(path), "/api/me/progress/%s", id);
	char *body = NULL;
	int status = 0;
	http_req_t req = {.allow_empty_body = true};
	bool ok = api_request(path, &req, &body, &status, server);
	out->server_now_ms = http_last_server_time_ms();
	if (!ok) {
		free(body);
		return status == 404;
	}
	json_doc_t doc;
	if (!body || !json_parse(body, &doc)) { free(body); return false; }
	int root = json_root(&doc);
	char returned_id[ABS_ID_MAX];
	json_obj_str(&doc, root, "libraryItemId", returned_id, sizeof(returned_id));
	out->seconds = json_double(&doc, json_get(&doc, root, "currentTime"), -1);
	out->finished = json_obj_bool(&doc, root, "isFinished", false);
	out->updated_ms = json_obj_llong(&doc, root, "lastUpdate", 0);
	out->exists = true;
	ok = strcmp(returned_id, item_id) == 0 && isfinite(out->seconds) && out->seconds >= 0;
	json_free(&doc);
	free(body);
	return ok;
}

static bool local_progress(const progress_job_t *job, char *item_id, size_t id_size,
						   double *current_out, double *duration_out, char *bound_server, size_t server_size,
						   char *remote_file, double *remote_seconds) {
	set_error("");
	char folder[ABS_PROGRESS_PATH_MAX];
	snprintf(folder, sizeof(folder), "%s", job->book);
	struct stat st;
	if (stat(folder, &st) != 0) {
		// A folder book can have a synthetic database key when several albums
		// share one directory. Its playing file still names the real directory.
		snprintf(folder, sizeof(folder), "%s", job->file);
		char *slash = strrchr(folder, '/');
		if (!slash) return false;
		*slash = '\0';
	} else if (!S_ISDIR(st.st_mode)) {
		char *slash = strrchr(folder, '/');
		if (!slash) return false;
		*slash = '\0';
	}
	char manifest[ABS_PROGRESS_PATH_MAX + 32];
	if ((size_t)snprintf(manifest, sizeof(manifest), "%s/.audiobookshelf", folder) >= sizeof(manifest))
		return false;
	FILE *f = fopen(manifest, "r");
	if (!f) {
		if (errno != ENOENT) set_error("The Audiobookshelf link could not be read.");
		return false;
	}

	item_id[0] = '\0';
	double duration = 0;
	double track_start = 0;
	bool track_found = false;
	if (remote_file) remote_file[0] = '\0';
	const char *basename = strrchr(job->file, '/');
	basename = basename ? basename + 1 : job->file;
	char line[1024];
	for (int lines = 0; lines < ABS_TRACK_MAX + 16 && fgets(line, sizeof(line), f); lines++) {
		line[strcspn(line, "\r\n")] = '\0';
		if (strncmp(line, "item_id=", 8) == 0) {
			size_t len = strlen(line + 8);
			if (len >= id_size) len = id_size - 1;
			memcpy(item_id, line + 8, len);
			item_id[len] = '\0';
		}
		else if (strncmp(line, "server=", 7) == 0) snprintf(bound_server, server_size, "%.*s", (int)server_size - 1, line + 7);
		else if (strncmp(line, "duration=", 9) == 0) duration = strtod(line + 9, NULL);
		else if (strncmp(line, "track=", 6) == 0) {
			char *first = strchr(line, '\t');
			char *second = first ? strchr(first + 1, '\t') : NULL;
			char *third = second ? strchr(second + 1, '\t') : NULL;
			if (first && second && third && strcmp(third + 1, basename) == 0) {
				track_start = strtod(first + 1, NULL);
				track_found = true;
			}
			if (remote_file && first && second && third) {
				double start = strtod(first + 1, NULL), length = strtod(second + 1, NULL);
				double target = job->finished ? 0 : job->seconds;
				const char *name = third + 1;
				if (isfinite(start) && isfinite(length) && start >= 0 && length > 0 &&
					target >= start && (target < start + length ||
					(fabs(target - duration) <= 0.001 && fabs(start + length - duration) <= 0.01)) && name[0] &&
					!strchr(name, '/') && !strchr(name, '\\') && strcmp(name, ".") && strcmp(name, "..")) {
					if ((size_t)snprintf(remote_file, 512, "%s/%s", folder, name) >= 512) remote_file[0] = '\0';
					*remote_seconds = target - start;
				}
			}
		}
	}
	fclose(f);
	if (!item_id[0] || !track_found || !isfinite(duration) || duration <= 0 ||
		!isfinite(track_start) || track_start < 0 || !isfinite(job->seconds)) {
		set_error("The Audiobookshelf link does not match this file.");
		return false;
	}

	double current = job->finished ? duration : track_start + job->seconds;
	if (current < 0) current = 0;
	if (current > duration) current = duration;
	*current_out = current;
	*duration_out = duration;
	return true;
}

static bool progress_online(void) {
#ifdef HOST_BUILD
	return true;
#else
	return wifi_get_enabled();
#endif
}

static void progress_publish(const char *file, audiobookshelf_sync_status_t status) {
	pthread_mutex_lock(&progress_lock);
	if (strcmp(file, progress_watch_file) == 0) progress_status = status;
	pthread_mutex_unlock(&progress_lock);
}

static void progress_refresh_status(void) {
	// All three carry durable-row strings (watch slot and checkpoint row),
	// so all three use the durable-row ceiling.
	char file[SONIX_LINKED_DB_PATH_MAX], book[SONIX_LINKED_DB_PATH_MAX];
	char server[ABS_SERVER_MAX];
	// Browsing status answers for the active request identity; the
	// OTHER_SERVER comparison itself is profile-aware (§11.4).
	bool identity = request_credentials_copy(NULL, server, sizeof(server), NULL, 0);
	pthread_mutex_lock(&progress_lock);
	snprintf(file, sizeof(file), "%s", progress_watch_file);
	pthread_mutex_unlock(&progress_lock);
	if (!file[0]) return;
	audiobookdb_abs_checkpoint_t checkpoint;
	if (!audiobookdb_book_for_file(file, book, sizeof(book)) || !audiobookdb_abs_get(book, &checkpoint)) {
		progress_publish(file, ABS_SYNC_LOCAL);
		return;
	}
	bool linked = checkpoint.linked;
	if (!checkpoint.known) {
		progress_job_t job = {0};
		snprintf(job.book, sizeof(job.book), "%s", book);
		snprintf(job.file, sizeof(job.file), "%s", file);
		char item[ABS_ID_MAX];
		double current, duration;
		char binding[ABS_SERVER_MAX];
		legacy_manifest_binding(binding, sizeof(binding));
		snprintf(checkpoint.server, sizeof(checkpoint.server), "%s", binding);
		linked = local_progress(&job, item, sizeof(item), &current, &duration, checkpoint.server, sizeof(checkpoint.server), NULL, NULL);
		if (!linked && last_error[0]) {
			progress_publish(file, ABS_SYNC_RETRY);
			return;
		}
	}
	audiobookshelf_sync_status_t status = !linked ? ABS_SYNC_LOCAL :
		!identity ? ABS_SYNC_SETUP :
		!bound_server_known(checkpoint.server, server) ? ABS_SYNC_OTHER_SERVER :
		!progress_online() ? (checkpoint.synced ? ABS_SYNC_SYNCED_OFFLINE : ABS_SYNC_OFFLINE) :
		checkpoint.synced ? ABS_SYNC_SYNCED :
		checkpoint.failed ? ABS_SYNC_RETRY :
		checkpoint.file[0] ? ABS_SYNC_PENDING : ABS_SYNC_LINKED;
	progress_publish(file, status);
}

// 0: retain/retry, 1: acknowledge sent checkpoint, 2: newer remote imported.
static int reconcile_progress(audiobookdb_abs_checkpoint_t *checkpoint, const progress_job_t *job,
	const char *item, double current, double duration, const char *server) {
	abs_remote_progress_t remote;
	if (!read_progress(item, server, &remote)) return 0;
	checkpoint->sync_error = 0;
	for (int check = 0; check < 2; check++) {
		abs_decision_t decision = abs_sync_decide(current, checkpoint->finished, checkpoint->updated_ms,
			&remote, (long long)time(NULL) * 1000);
		if (decision == ABS_DECISION_EQUAL) {
			checkpoint->server_updated_ms = remote.updated_ms;
			return 1;
		}
		if (decision == ABS_DECISION_HOLD) return 0;
		if (decision == ABS_DECISION_DOWNLOAD) {
			progress_job_t incoming = *job;
			incoming.seconds = remote.seconds;
			incoming.finished = remote.finished;
			// file/owner carry durable-row strings (the watch key and the
			// checkpoint row) -- same ceiling as everywhere else.
			char file[SONIX_LINKED_DB_PATH_MAX], owner[SONIX_LINKED_DB_PATH_MAX];
			char bound[ABS_SERVER_MAX], ignored[ABS_ID_MAX];
			double seconds = 0, unused, total;
			snprintf(bound, sizeof(bound), "%s", server);
			if (!local_progress(&incoming, ignored, sizeof(ignored), &unused, &total, bound, sizeof(bound), file, &seconds) ||
				!file[0] || strcmp(bound, server) || !audiobookdb_book_for_file(file, owner, sizeof(owner)) ||
				strcmp(owner, checkpoint->book) || access(file, R_OK)) return 0;
			long long stamp = remote.updated_ms >= 1577836800000LL &&
				remote.updated_ms <= remote.server_now_ms + 300000 ? remote.updated_ms : 0;
			if (!audiobook_sync_import(checkpoint, server, file, seconds, remote.finished, stamp)) return 0;
			printf("audiobookshelf: imported newer server progress %.1f s\n", remote.seconds);
			return 2;
		}
		// Re-read immediately before the write; ABS has no conditional PATCH.
		if (check == 0 && !read_progress(item, server, &remote)) return 0;
	}
	long long stamp = abs_sync_clock_valid(checkpoint->updated_ms, &remote, (long long)time(NULL) * 1000) ?
		checkpoint->updated_ms : 0;
	bool restarting = remote.exists && remote.finished && !checkpoint->finished && current > 0;
	if (!update_progress(item, current, duration, checkpoint->finished, stamp, server)) return 0;
	if (restarting) {
		// ABS clears currentTime when un-finishing; restore it only after
		// confirming that transition, not if another client moved meanwhile.
		if (!read_progress(item, server, &remote) || remote.finished ||
			(remote.seconds != 0 && fabs(remote.seconds - current) > 0.01)) return 0;
		if (remote.seconds == 0 && !update_progress(item, current, duration, false, stamp, server)) return 0;
	}
	// A successful status alone is not proof that the server kept our position.
	if (!read_progress(item, server, &remote) || !remote.exists || remote.finished != checkpoint->finished ||
		fabs(remote.seconds - current) > 0.01) return 0;
	checkpoint->server_updated_ms = remote.updated_ms;
	return 1;
}

static void *progress_worker(void *unused) {
	(void)unused;
	thread_be_background("abs-progress");
	// Fair round-robin (§11.4): which profile opens each pass rotates, so a
	// busy first server cannot hold the budget away from quieter ones every
	// pass.
	unsigned rotation = 0;
	for (;;) {
		// One sleeping worker, no directory walk or decoder work. Pending
		// uploads are differences from the existing durable local checkpoints.
		pthread_mutex_lock(&progress_lock);
		if (!progress_wake && !progress_online()) {
			pthread_cond_wait(&progress_cond, &progress_lock);
		} else if (!progress_wake) {
			struct timespec wake;
			clock_gettime(CLOCK_REALTIME, &wake);
			wake.tv_sec += 15;
			pthread_cond_timedwait(&progress_cond, &progress_lock, &wake);
		}
		progress_wake = false;
		bool reconnect = progress_reconnect;
		progress_reconnect = false;
		// Drain every pending (server, book) immediate, not just the latest:
		// each slot re-arms its own durable row, so one profile's checkpoint
		// can never push another's out of the pass (§11.4 step 6). The server
		// half of the key is what made the individual hints holdable at all;
		// audiobookdb_abs_retry() re-arms by book, which names one row.
		progress_forced_t forced[ABS_PROFILE_MAX];
		memcpy(forced, progress_forced, sizeof(forced));
		memset(progress_forced, 0, sizeof(progress_forced));
		pthread_mutex_unlock(&progress_lock);
		// No flush hook here: upstream's background writer is a single-slot
		// queue guarded by queue_lock/card_epoch (see H2/H3 of the rebase
		// plan). A checkpoint still queued when this pass reads the card is
		// picked up by the next poll -- the outbox model is acknowledged-or-
		// pending by construction, and audiobookdb_abs_import() refuses to
		// import while a write is outstanding.
		if (reconnect) audiobookdb_abs_reconnect();
		for (int i = 0; i < ABS_PROFILE_MAX; i++) {
			if (forced[i].book[0]) audiobookdb_abs_retry(forced[i].book);
		}
		progress_refresh_status();
		// Offline first: a device with no data connection takes no pass at all,
		// whatever a profile might want (§11.4 step 5 keeps the wake/sleep
		// semantics; this is only the gate, as before).
		if (!progress_online() || !sync_credentials_available()) continue;
		// One pass, profiles strictly serial (§11.4): a fixed global budget of
		// eight book operations, one profile at a time in rotating order, and
		// no per-server thread or extra database.
		abs_auth_snapshot_t passes[ABS_PROFILE_MAX];
		memset(passes, 0, sizeof(passes));
		int pass_count = abs_profiles_snapshots_all(passes, ABS_PROFILE_MAX);
		if (pass_count == 0) {
			// Migration window (§11.1): the profile store has nothing
			// configured and the legacy pair is all there is. Sync continues
			// bound to it exactly as 0.5.0 did until the store wins.
			char legacy_server[512], legacy_token[1024];
			request_credentials_copy(NULL, legacy_server, sizeof(legacy_server),
									 legacy_token, sizeof(legacy_token));
			snprintf(passes[0].profile_id, sizeof(passes[0].profile_id), "legacy");
			snprintf(passes[0].server, sizeof(passes[0].server), "%s", legacy_server);
			snprintf(passes[0].token, sizeof(passes[0].token), "%s", legacy_token);
			pass_count = 1; // the availability gate above guarantees the pair resolved
		}
		int budget = 8;
		// §11.5: a manifest without `server=` binds to the persisted legacy
		// server, never to whichever profile's pass happens to find it. The
		// owning profile's loop picks it up on a later pass through the row's
		// server column.
		char legacy_binding[ABS_SERVER_MAX];
		legacy_manifest_binding(legacy_binding, sizeof(legacy_binding));
		for (int i = 0; i < pass_count && budget > 0; i++) {
			const abs_auth_snapshot_t *profile = &passes[(rotation + i) % pass_count];
			for (; budget > 0; budget--) {
				audiobookdb_abs_checkpoint_t checkpoint;
				if (!audiobookdb_abs_next(profile->server, &checkpoint)) break;
				progress_job_t job = {0};
				snprintf(job.book, sizeof(job.book), "%s", checkpoint.book);
				snprintf(job.file, sizeof(job.file), "%s", checkpoint.file);
				job.seconds = checkpoint.seconds;
				job.finished = checkpoint.finished;
				char item[ABS_ID_MAX], bound[ABS_SERVER_MAX];
				snprintf(bound, sizeof(bound), "%s", legacy_binding);
				double current = 0, duration = 0;
				bool linked = local_progress(&job, item, sizeof(item), &current, &duration, bound, sizeof(bound), NULL, NULL);
				bool ok = false, failed = last_error[0] != '\0';
				int result = 0;
				if (linked && strcmp(bound, profile->server) == 0 && progress_online()) {
					// reconcile_progress() re-resolves the owning profile's
					// snapshot under the store lock per request (§11.4 step 4);
					// this comparison only keeps a manifest from drifting.
					progress_publish(checkpoint.file, ABS_SYNC_SENDING);
					result = reconcile_progress(&checkpoint, &job, item, current, duration, bound);
					ok = result != 0;
					failed = !ok;
					if (ok) printf("audiobookshelf: synced progress %.1f / %.1f s\n", current, duration);
					else fprintf(stderr, "audiobookshelf: progress sync failed; saved checkpoint retained\n");
				}
				if (result != 2) audiobookdb_abs_result(&checkpoint, bound, linked || failed, ok, failed);
				progress_refresh_status();
				if (failed) break; // one unavailable server must not block the next profile (§11.4 step 2)
			}
		}
		// Token bytes live here between requests; nothing retains them.
		for (int i = 0; i < pass_count; i++) abs_profiles_snapshot_wipe(&passes[i]);
		rotation++;
	}
	return NULL;
}

static void progress_worker_start(void) {
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 96 * 1024);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	pthread_t thread;
	progress_worker_ready = pthread_create(&thread, &attr, progress_worker, NULL) == 0;
	pthread_attr_destroy(&attr);
	if (!progress_worker_ready) fprintf(stderr, "audiobookshelf: could not start progress worker\n");
}

void audiobookshelf_queue_progress(const char *book_path, const char *file, double seconds,
								   bool finished, bool immediate) {
	(void)seconds;
	(void)finished;
	if (!audiobookshelf_configured() || !book_path || !book_path[0] || !file || !file[0]) return;
	pthread_once(&progress_once, progress_worker_start);
	if (!progress_worker_ready) return;
	pthread_mutex_lock(&progress_lock);
	if (strcmp(file, progress_watch_file) == 0 &&
		(progress_status == ABS_SYNC_SYNCED || progress_status == ABS_SYNC_SYNCED_OFFLINE)) {
		progress_status = progress_online() ? ABS_SYNC_PENDING : ABS_SYNC_OFFLINE;
	}
	if (immediate) {
		// §11.4 step 6: key the hint by (server, book), not just the path. The
		// server half is this device's current unbound identity -- the active
		// profile or the legacy pair in its migration window (§11.3) -- which is
		// what a checkpoint queued here binds to: the link flow above just
		// linked against it, and local playback queues under whatever is active
		// now. A hint on server B can no longer evict the still-pending one on
		// server A; each slot drains its own row next pass.
		char server[ABS_SERVER_MAX];
		request_credentials_copy(NULL, server, sizeof(server), NULL, 0);
		long stamp = ++progress_forced_stamp;
		int target = -1; // an exact (server, book) match refreshes in place
		for (int i = 0; i < ABS_PROFILE_MAX && target < 0; i++) {
			if (progress_forced[i].book[0] &&
				strcmp(progress_forced[i].server, server) == 0 &&
				strcmp(progress_forced[i].book, book_path) == 0)
				target = i;
		}
		int oldest = -1; // all slots full and no match: evict the least recent touch
		for (int i = 0; i < ABS_PROFILE_MAX && target < 0; i++) {
			if (!progress_forced[i].book[0]) { target = i; break; }
			if (oldest < 0 || progress_forced[i].stamp <= progress_forced[oldest].stamp) oldest = i;
		}
		if (target < 0) target = oldest;
		snprintf(progress_forced[target].server, sizeof(progress_forced[target].server), "%s", server);
		snprintf(progress_forced[target].book, sizeof(progress_forced[target].book), "%s", book_path);
		progress_forced[target].stamp = stamp;
	}
	progress_wake = true;
	pthread_cond_signal(&progress_cond);
	pthread_mutex_unlock(&progress_lock);
}

void audiobookshelf_network_changed(void) {
	pthread_mutex_lock(&progress_lock);
	progress_wake = true;
	progress_reconnect = true;
	pthread_cond_signal(&progress_cond);
	pthread_mutex_unlock(&progress_lock);
}

audiobookshelf_sync_status_t audiobookshelf_sync_status(const char *file) {
	if (!file || !file[0]) {
		pthread_mutex_lock(&progress_lock);
		progress_watch_file[0] = '\0';
		progress_status = ABS_SYNC_LOCAL;
		pthread_mutex_unlock(&progress_lock);
		return ABS_SYNC_LOCAL;
	}
	if (!audiobookshelf_configured()) return ABS_SYNC_LOCAL;
	pthread_once(&progress_once, progress_worker_start);
	pthread_mutex_lock(&progress_lock);
	if (strcmp(file, progress_watch_file) != 0) {
		snprintf(progress_watch_file, sizeof(progress_watch_file), "%s", file);
		progress_status = ABS_SYNC_CHECKING;
		progress_wake = true;
		pthread_cond_signal(&progress_cond);
	}
	audiobookshelf_sync_status_t status = progress_status;
	pthread_mutex_unlock(&progress_lock);
	return status;
}

static void first_author(const json_doc_t *doc, int metadata, char *out, size_t size) {
	out[0] = '\0';
	json_obj_str(doc, metadata, "authorName", out, size);
	if (out[0]) return;
	int authors = json_get(doc, metadata, "authors");
	int first = json_at(doc, authors, 0);
	json_obj_str(doc, first, "name", out, size);
}

static void first_series(const json_doc_t *doc, int metadata, char *name, size_t name_size,
						 char *sequence, size_t sequence_size) {
	name[0] = '\0';
	if (sequence && sequence_size) sequence[0] = '\0';
	json_obj_str(doc, metadata, "seriesName", name, name_size);
	int series = json_get(doc, metadata, "series");
	int first = json_at(doc, series, 0);
	if (!name[0]) json_obj_str(doc, first, "name", name, name_size);
	if (sequence && sequence_size) json_obj_str(doc, first, "sequence", sequence, sequence_size);
}

static void read_item(const json_doc_t *doc, int object, audiobookshelf_item_t *item) {
	memset(item, 0, sizeof(*item));
	json_obj_str(doc, object, "id", item->id, sizeof(item->id));
	int media = json_get(doc, object, "media");
	int metadata = json_get(doc, media, "metadata");
	json_obj_str(doc, metadata, "title", item->title, sizeof(item->title));
	first_author(doc, metadata, item->author, sizeof(item->author));
	char unused[2];
	first_series(doc, metadata, item->series, sizeof(item->series), unused, sizeof(unused));
	item->duration = json_double(doc, json_get(doc, media, "duration"), 0);
	item->tracks = (int)json_obj_long(doc, media, "numTracks", 0);
	if (!item->tracks) item->tracks = json_len(doc, json_get(doc, media, "tracks"));

	char cover_path[ABS_URL_MAX] = "";
	json_obj_str(doc, media, "coverPath", cover_path, sizeof(cover_path));
	if (cover_path[0] && item->id[0])
		snprintf(item->cover, sizeof(item->cover), "/api/items/%s/cover", item->id);

	int progress = json_get(doc, object, "userMediaProgress");
	item->progress = json_double(doc, json_get(doc, progress, "currentTime"), 0);
	item->finished = json_obj_bool(doc, progress, "isFinished", false);
	if (!item->title[0]) snprintf(item->title, sizeof(item->title), "Untitled");
}

// One server items page into the caller's bounded array. `total` is the
// server's overall count for the current sort/filter (§10.1 page controls).
static int abs_read_items_page(const char *body, audiobookshelf_item_t *out, int max,
							   int *total_out, const char *invalid_error) {
	json_doc_t doc;
	if (!body || !json_parse(body, &doc)) {
		set_error(invalid_error);
		return -1;
	}
	int root = json_root(&doc);
	if (total_out) *total_out = (int)json_obj_long(&doc, root, "total", 0);
	int results = json_get(&doc, root, "results");
	int count = json_len(&doc, results);
	if (count > max) count = max;
	for (int i = 0; i < count; i++) read_item(&doc, json_at(&doc, results, i), &out[i]);
	json_free(&doc);
	return count;
}

int audiobookshelf_libraries(audiobookshelf_library_t *out, int max) {
	if (!out || max <= 0) return 0;
	char *body = NULL;
	if (!api_request("/api/libraries", NULL, &body, NULL, NULL)) {
		free(body);
		return -1;
	}
	json_doc_t doc;
	if (!json_parse(body, &doc)) {
		free(body);
		set_error("Audiobookshelf returned invalid library data.");
		return -1;
	}
	int libraries = json_get(&doc, json_root(&doc), "libraries");
	int count = json_len(&doc, libraries);
	if (count > max) count = max;
	int written = 0;
	for (int i = 0; i < count; i++) {
		int object = json_at(&doc, libraries, i);
		audiobookshelf_library_t entry = {0};
		json_obj_str(&doc, object, "id", entry.id, sizeof(entry.id));
		json_obj_str(&doc, object, "name", entry.name, sizeof(entry.name));
		json_obj_str(&doc, object, "mediaType", entry.media_type, sizeof(entry.media_type));
		if (entry.id[0] && (!entry.media_type[0] || strcmp(entry.media_type, "book") == 0)) out[written++] = entry;
	}
	json_free(&doc);
	free(body);
	return written;
}

int audiobookshelf_items(const char *library_id, int page, audiobookshelf_item_t *out, int max, int *total_out) {
	return audiobookshelf_filtered_items(library_id, page, "", out, max, total_out);
}

// Legacy M1 entry point, kept for the un-migrated GUI (§10.3). The request
// below is byte-for-byte what 0.6.0 M1 sends (no `desc`, `sort=sequence` for
// series filters) so the mock-log and GUI-smoke assertions keep their meaning;
// the general contract lives in audiobookshelf_browse() above.
int audiobookshelf_filtered_items(const char *library_id, int page, const char *filter,
	 audiobookshelf_item_t *out, int max, int *total_out) {
	if (!library_id || !library_id[0] || !out || max <= 0) return 0;
	if (max > ABS_PAGE_LIMIT) max = ABS_PAGE_LIMIT;
	char id[ABS_ID_MAX * 3];
	http_url_encode(library_id, id, sizeof(id));
	char encoded[600], path[1200];
	http_url_encode(filter ? filter : "", encoded, sizeof(encoded));
	snprintf(path, sizeof(path), "/api/libraries/%s/items?limit=%d&page=%d&sort=%s&minified=0&collapseseries=0&filter=%s",
			 id, max, page < 0 ? 0 : page,
			 filter && strncmp(filter, "series.", 7) == 0 ? "sequence" : "media.metadata.title", encoded);
	char *body = NULL;
	if (!api_request(path, NULL, &body, NULL, NULL)) {
		free(body);
		return -1;
	}
	int count = abs_read_items_page(body, out, max, total_out,
									"Audiobookshelf returned invalid book data.");
	free(body);
	return count;
}

// Standard padded Base64 (§10.1): the "authors./series./genres./tags." filter
// values are built with the STANDARD alphabet -- "+" and "/" -- and "="-padded,
// explicitly not the URL-safe variant. URL encoding below is what carries "+",
// "/" and "=" through the query string. False when `len`'s output would not
// fit `size` bytes with the terminator.
static bool abs_b64_encode(const char *in, size_t len, char *out, size_t size) {
	static const char alphabet[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	if (!out || size == 0 || !in || (len + 2) / 3 * 4 + 1 > size) return false;
	size_t k = 0;
	for (size_t i = 0; i < len; i += 3) {
		unsigned value = (unsigned char)in[i] << 16;
		if (i + 1 < len) value |= (unsigned char)in[i + 1] << 8;
		if (i + 2 < len) value |= (unsigned char)in[i + 2];
		out[k++] = alphabet[(value >> 18) & 63];
		out[k++] = alphabet[(value >> 12) & 63];
		out[k++] = i + 1 < len ? alphabet[(value >> 6) & 63] : '=';
		out[k++] = i + 2 < len ? alphabet[value & 63] : '=';
	}
	out[k] = '\0';
	return true;
}

// The value prefix for one group filter (§10.1). NULL for no group or an
// out-of-range input.
static const char *abs_filter_prefix(abs_browse_filter_t group) {
	switch (group) {
	case ABS_FILTER_AUTHOR: return "authors";
	case ABS_FILTER_SERIES: return "series";
	case ABS_FILTER_GENRE: return "genres";
	case ABS_FILTER_TAG: return "tags";
	case ABS_FILTER_NONE: break;
	}
	return NULL;
}

bool abs_browse_query_default_desc(abs_browse_sort_t sort) {
	// §10.1 default direction: only "recently added" opens newest-first; the
	// duration toggle permits longest first, and everything else reads ascending.
	return sort == ABS_SORT_ADDED_AT;
}

const char *abs_browse_sort_field(abs_browse_sort_t sort) {
	switch (sort) {
	case ABS_SORT_TITLE: return "media.metadata.title";
	case ABS_SORT_AUTHOR: return "media.metadata.authorNameLF"; // last-name-first
	case ABS_SORT_ADDED_AT: return "addedAt";
	case ABS_SORT_DURATION: return "media.duration";
	case ABS_SORT_SERIES_POSITION: return "sequence";
	}
	return NULL; // out-of-range enum cast: caller error, never guess
}

bool abs_browse_query_build_filter(abs_browse_filter_t group, const char *value,
								   char *out, size_t size) {
	if (!out || size == 0) return false;
	out[0] = '\0';
	const char *prefix = value ? abs_filter_prefix(group) : NULL;
	if (!prefix || !value[0]) return false;
	char encoded[(ABS_NAME_MAX + 2) / 3 * 4 + 1];
	if (!abs_b64_encode(value, strlen(value), encoded, sizeof(encoded))) return false;
	// The whole "<prefix>.<base64>" value is URL-encoded (§10.1). Compute the
	// encoded length first: http_url_encode() truncates silently and a cut
	// filter would look like a valid but wrong query to the server.
	char raw[8 + sizeof(encoded)];
	size_t raw_len = (size_t)snprintf(raw, sizeof(raw), "%s.%s", prefix, encoded);
	if (raw_len >= sizeof(raw)) return false;
	// Same safe-set decision http_url_encode() makes: unreserved ASCII takes
	// one byte, everything else expands to %XX.
	size_t need = 1;
	for (size_t i = 0; i < raw_len; i++) {
		unsigned char c = (unsigned char)raw[i];
		need += ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
				 (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
				 c == '~') ? 1 : 3;
	}
	if (need > size) return false;
	http_url_encode(raw, out, size);
	return out[0] != '\0';
}

// Fills and validates a bounded browse query. Series-position sort is refused
// without a series filter (§10.1: "series-filter default only") -- a guess
// would silently order series-less rows by a field they do not carry.
bool abs_browse_query_init(abs_browse_query_t *query, const char *library_id,
						   abs_browse_sort_t sort, abs_browse_filter_t group,
						   const char *group_value, int page, bool descending) {
	if (!query || !library_id || !library_id[0]) return false;
	memset(query, 0, sizeof(*query));
	if (!abs_browse_sort_field(sort)) return false;
	if (group != ABS_FILTER_NONE && !abs_filter_prefix(group)) return false;
	if (sort == ABS_SORT_SERIES_POSITION && group != ABS_FILTER_SERIES) return false;
	if (strlen(library_id) >= sizeof(query->library_id)) return false;
	snprintf(query->library_id, sizeof(query->library_id), "%s", library_id);
	query->sort = sort;
	query->descending = descending;
	query->filter = group;
	if (group != ABS_FILTER_NONE) {
		// Author/series take an object id; genre/tag take the string itself
		// (§10.1). Both arrive as filter_value; the builder knows the shape.
		char empty = '\0';
		const char *value = group_value ? group_value : &empty;
		if (!value[0] || strlen(value) >= sizeof(query->filter_value)) return false;
		snprintf(query->filter_value, sizeof(query->filter_value), "%s", value);
	}
	query->page = page < 0 ? 0 : page; // §10.1 pages are zero-based
	return true;
}

// The §10.1 request template, byte for byte: limit, page, sort, desc,
// minified, collapseseries, filter. `filter=` is present even when empty --
// the template always carries it, and an empty value is a valid "no filter"
// to the reviewed route (the mock decodes "" to no filter as well).
static bool abs_browse_request_path(const abs_browse_query_t *query, char *path, size_t size) {
	if (!query || !path || size == 0) return false;
	const char *field = abs_browse_sort_field(query->sort);
	if (!field) return false;
	if (query->filter != ABS_FILTER_NONE && !abs_filter_prefix(query->filter)) return false;
	if (query->sort == ABS_SORT_SERIES_POSITION && query->filter != ABS_FILTER_SERIES) return false;
	// Worst URL-encoded filter: "authors." (8) + Base64 of a 255-byte value
	// (ceil(255/3)*4 = 340) + terminator, with every byte at %XX (3x) ≈ 1048.
	char id[ABS_ID_MAX * 3], filter[1050];
	http_url_encode(query->library_id, id, sizeof(id));
	if (query->filter == ABS_FILTER_NONE) {
		filter[0] = '\0';
	} else if (!abs_browse_query_build_filter(query->filter, query->filter_value,
												 filter, sizeof(filter))) {
		return false;
	}
	int page = query->page < 0 ? 0 : query->page;
	int written = snprintf(path, size,
						   "/api/libraries/%s/items?limit=%d&page=%d&sort=%s&desc=%s&minified=0&collapseseries=0&filter=%s",
						   id, ABS_BROWSE_PAGE_LIMIT, page, field,
						   query->descending ? "1" : "0", filter);
	return written > 0 && (size_t)written < size;
}

int audiobookshelf_browse(const abs_browse_query_t *query, audiobookshelf_item_t *out,
						  int max, int *total_out) {
	if (!query || !out || max <= 0) return 0;
	char path[1600];
	if (!abs_browse_request_path(query, path, sizeof(path))) {
		// A genre/tag string close to the name ceiling can outgrow one URL;
		// refusing beats shipping a server-rewriting truncated value.
		set_error("The browse query is too long for this server address.");
		return 0;
	}
	char *body = NULL;
	if (!api_request(path, NULL, &body, NULL, NULL)) {
		free(body);
		return -1;
	}
	int count = abs_read_items_page(body, out, max, total_out,
									"Audiobookshelf returned invalid book data.");
	free(body);
	return count;
}

static int group_compare(const void *a, const void *b) {
	return strcasecmp(((const audiobookshelf_library_t *)a)->name, ((const audiobookshelf_library_t *)b)->name);
}

// filterdata choices for one group (§10.1). Authors and series arrive as
// {id,name} objects and need both fields. Genres and tags carry "the string
// itself": reviewed servers send bare strings while the checked-in fixtures
// model them as {id,name} objects -- either way the FILTER VALUE is the name,
// never an object id, so it goes into `name` alone and `id` stays empty.
static const char *abs_group_key(abs_browse_filter_t group) {
	switch (group) {
	case ABS_FILTER_AUTHOR: return "authors";
	case ABS_FILTER_SERIES: return "series";
	case ABS_FILTER_GENRE: return "genres";
	case ABS_FILTER_TAG: return "tags";
	case ABS_FILTER_NONE: break;
	}
	return NULL;
}

static bool abs_group_name_only(abs_browse_filter_t group) {
	return group == ABS_FILTER_GENRE || group == ABS_FILTER_TAG;
}

// Decoded byte length of a JSON string -- exactly what json_str() would write
// (same escape walk, including \u and surrogate pairs) -- but counted instead
// of copied, and stopped once it passes `limit`: the caller only ever asks
// whether a filter-group value fits its fixed contract, never its exact size.
static int abs_hex4(const char *s) {
	int value = 0;
	for (int i = 0; i < 4; i++) {
		char c = s[i];
		value <<= 4;
		if (c >= '0' && c <= '9') value |= c - '0';
		else if (c >= 'a' && c <= 'f') value |= c - 'a' + 10;
		else if (c >= 'A' && c <= 'F') value |= c - 'A' + 10;
		else return -1;
	}
	return value;
}

static size_t json_decoded_len(const json_doc_t *doc, int tok, size_t limit) {
	if (!doc || !doc->toks || tok < 0 || tok >= doc->count ||
		doc->toks[tok].type != JSON_STRING) return 0;
	const char *src = doc->text + doc->toks[tok].start;
	int len = doc->toks[tok].end - doc->toks[tok].start;
	size_t w = 0;
	for (int i = 0; i < len && w <= limit; i++) {
		if (src[i] != '\\') { w++; continue; }
		if (i + 1 >= len) break;
		char esc = src[++i];
		switch (esc) {
		case 'u': {
			if (i + 4 >= len) { i = len; break; }
			int cp = abs_hex4(src + i + 1);
			if (cp < 0) { i = len; break; }
			i += 4;
			if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 < len) {
				int low = abs_hex4(src + i + 1);
				if (low >= 0xDC00 && low <= 0xDFFF) {
					cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
					i += 6;
				}
			}
			// Same encoded sizes utf8_put() produces for these code points.
			w += cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
			break;
		}
		default:
			w++; // every other escape decodes to exactly one byte
			break;
		}
	}
	return w;
}

// Fixed-size entries make the §10.2 byte bound structural: the projection's
// memory is entries × sizeof(entry), so capping entries caps bytes. The
// assert keeps that true if the struct ever grows.
_Static_assert(ABS_FILTER_GROUP_LIMIT * (int)sizeof(audiobookshelf_library_t)
				   <= ABS_FILTER_GROUP_BYTES_MAX,
			   "512-entry group projection must fit the 192 KiB byte bound");

// The tighter of the two §10.2 projection bounds.
static int group_projection_cap(void) {
	int by_bytes = ABS_FILTER_GROUP_BYTES_MAX / (int)sizeof(audiobookshelf_library_t);
	return ABS_FILTER_GROUP_LIMIT < by_bytes ? ABS_FILTER_GROUP_LIMIT : by_bytes;
}

// Copies one filter-group value: for author/series the object ID is the
// filter value, for genre/tag the raw string itself. A decoded value that
// does not fit its contract is never silently truncated into a
// working-looking but wrong filter: the visible head is kept for display and
// the entry is flagged oversized.
static void group_value_copy(const json_doc_t *doc, int value_tok, size_t contract,
							 char *display, size_t display_size, bool *oversized) {
	if (!json_str(doc, value_tok, display, display_size)) return;
	*oversized = json_decoded_len(doc, value_tok, contract) > contract;
}

int audiobookshelf_groups_ex(const char *library_id, abs_browse_filter_t group,
							 audiobookshelf_library_t **out, bool *truncated_out,
							 int *oversized_out) {
	const char *key = abs_group_key(group);
	if (out) *out = NULL;
	if (truncated_out) *truncated_out = false;
	if (oversized_out) *oversized_out = 0;
	if (!library_id || !key || !out) return 0;
	char id[ABS_ID_MAX * 3], path[384], *body = NULL;
	http_url_encode(library_id, id, sizeof(id));
	snprintf(path, sizeof(path), "/api/libraries/%s/filterdata", id);
	if (!api_request(path, NULL, &body, NULL, NULL)) { free(body); return -1; }
	json_doc_t doc;
	if (!json_parse(body, &doc)) { free(body); set_error("Invalid library filters."); return -1; }
	int array = json_get(&doc, json_root(&doc), key);
	int received = json_len(&doc, array);
	// §10.2: response count and projection bytes are checked BEFORE anything
	// is allocated. Past a bound only the first, still pageable set is
	// retained (in received order, sorted afterwards) and the caller is told,
	// so a multi-megabyte filterdata can never expand into unbounded memory.
	int cap = group_projection_cap();
	bool truncated = received > cap;
	int count = truncated ? cap : received;
	int oversized = 0, written = 0;
	audiobookshelf_library_t *groups = count > 0 ? calloc((size_t)count, sizeof(*groups)) : NULL;
	if (count > 0 && !groups) { json_free(&doc); free(body); set_error("Not enough memory for library filters."); return -1; }
	for (int i = 0; i < count; i++) {
		int entry = json_at(&doc, array, i);
		if (entry < 0) continue;
		audiobookshelf_library_t *slot = &groups[written];
		if (!abs_group_name_only(group)) {
			// Authors/series: {id,name} with both required. The ID is the
			// filter value: an id that json_str() could not carry whole would
			// encode a filter that matches nothing, so the entry is flagged
			// oversized and retained for the caller's disabled display; an
			// over-long name only affects what is shown.
			int id_tok = json_get(&doc, entry, "id");
			int name_tok = json_get(&doc, entry, "name");
			if (!json_str(&doc, id_tok, slot->id, sizeof(slot->id)) ||
				!json_str(&doc, name_tok, slot->name, sizeof(slot->name)))
				continue; // missing id or name: absent from the projection
			// An author/series choice needs both -- an empty id or name would
			// encode a filter that matches nothing or a row nobody can read.
			if (!slot->id[0] || !slot->name[0]) continue;
			slot->oversized = json_decoded_len(&doc, id_tok, sizeof(slot->id) - 1)
											  > (size_t)(sizeof(slot->id) - 1);
			if (slot->oversized) oversized++;
			written++;
			continue;
		}
		// §10.1: a genre/tag choice's value is the string itself. Reviewed
		// servers send bare strings, the checked-in fixtures {id,name} objects
		// -- the name is what the filter encodes either way.
		int value_tok = doc.toks[entry].type == JSON_STRING
			? entry : json_get(&doc, entry, "name");
		group_value_copy(&doc, value_tok, ABS_NAME_MAX - 1,
						 slot->name, sizeof(slot->name), &slot->oversized);
		if (!slot->name[0]) continue;
		if (slot->oversized) oversized++;
		written++;
	}
	if (written > 1) qsort(groups, written, sizeof(*groups), group_compare);
	json_free(&doc); free(body); *out = groups;
	if (truncated_out) *truncated_out = truncated;
	if (oversized_out) *oversized_out = oversized;
	return written;
}

int audiobookshelf_groups(const char *library_id, bool series, audiobookshelf_library_t **out) {
	return audiobookshelf_groups_ex(library_id, series ? ABS_FILTER_SERIES : ABS_FILTER_AUTHOR,
									out, NULL, NULL);
}

void audiobookshelf_group_filter(bool series, const char *id, char *out, size_t size) {
	if (!out || size == 0) return;
	out[0] = '\0';
	if (!id || strlen(id) >= ABS_ID_MAX) return;
	char encoded[ABS_ID_MAX * 2];
	if (!abs_b64_encode(id, strlen(id), encoded, sizeof(encoded))) return;
	snprintf(out, size, "%s.%s", series ? "series" : "authors", encoded);
}

int audiobookshelf_search(const char *library_id, const char *query, audiobookshelf_item_t *out, int max) {
	if (!library_id || !query || !query[0] || !out || max <= 0) return 0;
	if (max > ABS_PAGE_LIMIT) max = ABS_PAGE_LIMIT;
	char id[ABS_ID_MAX * 3], encoded[ABS_NAME_MAX * 3], path[1200];
	http_url_encode(library_id, id, sizeof(id));
	http_url_encode(query, encoded, sizeof(encoded));
	snprintf(path, sizeof(path), "/api/libraries/%s/search?q=%s&limit=%d", id, encoded, max);
	char *body = NULL;
	if (!api_request(path, NULL, &body, NULL, NULL)) { free(body); return -1; }
	json_doc_t doc;
	if (!json_parse(body, &doc)) {
		free(body);
		set_error("Audiobookshelf returned invalid search data.");
		return -1;
	}
	int results = json_get(&doc, json_root(&doc), "book");
	int count = json_len(&doc, results), written = 0;
	for (int i = 0; i < count && written < max; i++) {
		audiobookshelf_item_t item;
		read_item(&doc, json_get(&doc, json_at(&doc, results, i), "libraryItem"), &item);
		if (item.id[0]) out[written++] = item;
	}
	json_free(&doc);
	free(body);
	return written;
}

// §10.1 search: the reviewed route has no offset/page/total and does not
// accept the item-query sort/filter contract, so there is nothing to page
// through -- the whole ceiling is asked for in one request and the 101st
// usable match becomes `has_more` evidence. `q=` is URL-encoded by
// http_url_encode(); retention is min(max, ABS_SEARCH_DISPLAY_LIMIT) in
// server-returned order (ABS does not document a relevance order, and no
// local re-sort may claim otherwise).
int audiobookshelf_search_ex(const char *library_id, const char *query, audiobookshelf_item_t *out,
							 int max, bool *has_more) {
	if (has_more) *has_more = false;
	if (!library_id || !library_id[0] || !query || !query[0] || !out || max <= 0) return 0;
	int keep = max > ABS_SEARCH_DISPLAY_LIMIT ? ABS_SEARCH_DISPLAY_LIMIT : max;
	char id[ABS_ID_MAX * 3], encoded[ABS_NAME_MAX * 3], path[1200];
	http_url_encode(library_id, id, sizeof(id));
	// A query over ABS_NAME_MAX - 1 bytes would be silently truncated by the
	// encoder into a different search; refuse instead. (The legacy entry point
	// keeps its old silent truncation for the un-migrated GUI.)
	if (strlen(query) >= ABS_NAME_MAX) {
		set_error("The search text is too long.");
		return 0;
	}
	http_url_encode(query, encoded, sizeof(encoded));
	snprintf(path, sizeof(path), "/api/libraries/%s/search?q=%s&limit=%d", id, encoded,
			 ABS_SEARCH_REQUEST_LIMIT);
	char *body = NULL;
	if (!api_request(path, NULL, &body, NULL, NULL)) { free(body); return -1; }
	json_doc_t doc;
	if (!json_parse(body, &doc)) {
		free(body);
		set_error("Audiobookshelf returned invalid search data.");
		return -1;
	}
	int results = json_get(&doc, json_root(&doc), "book");
	int count = json_len(&doc, results), written = 0;
	bool probe = false;
	for (int i = 0; i < count && !probe; i++) {
		audiobookshelf_item_t item;
		read_item(&doc, json_get(&doc, json_at(&doc, results, i), "libraryItem"), &item);
		if (!item.id[0]) continue; // malformed entries are not probe evidence
		if (written < keep) out[written++] = item;
		else probe = true; // the 101st usable match: honest overflow (§10.1)
	}
	json_free(&doc);
	free(body);
	if (has_more) *has_more = probe;
	return written;
}

// One expanded server item into the caller's book: item identity, first
// author/series, description, first-progress stamp and the full track list.
//
// `reject_overflow` selects the queue rule (§8.2 step 2): more than
// ABS_TRACK_MAX tracks is an explicit unsupported error — never the silent
// display truncation the legacy detail page keeps (its caller needs bounded
// data, not a refusal). One parser, so the two readings can never disagree
// about a field.
static bool abs_parse_expanded(const char *body, bool reject_overflow, audiobookshelf_book_t *out) {
	json_doc_t doc;
	if (!json_parse(body, &doc)) {
		set_error("Audiobookshelf returned invalid book details.");
		return false;
	}
	int root = json_root(&doc);
	read_item(&doc, root, &out->item);
	int media = json_get(&doc, root, "media");
	int metadata = json_get(&doc, media, "metadata");
	json_obj_str(&doc, metadata, "description", out->description, sizeof(out->description));
	first_series(&doc, metadata, out->item.series, sizeof(out->item.series), out->series_part,
				 sizeof(out->series_part));
	int progress = json_get(&doc, root, "userMediaProgress");
	out->progress_updated_ms = json_obj_llong(&doc, progress, "lastUpdate", 0);

	int tracks = json_get(&doc, media, "tracks");
	int count = json_len(&doc, tracks);
	if (reject_overflow && count > ABS_TRACK_MAX) {
		json_free(&doc);
		set_error("This item has more than 256 files; the queue does not support it yet");
		return false;
	}
	if (count > ABS_TRACK_MAX) count = ABS_TRACK_MAX;
	for (int i = 0; i < count; i++) {
		int object = json_at(&doc, tracks, i);
		audiobookshelf_track_t *track = &out->tracks[i];
		track->index = (int)json_obj_long(&doc, object, "index", i + 1);
		track->start_offset = json_double(&doc, json_get(&doc, object, "startOffset"), 0);
		track->duration = json_double(&doc, json_get(&doc, object, "duration"), 0);
		json_obj_str(&doc, object, "title", track->title, sizeof(track->title));
		json_obj_str(&doc, object, "contentUrl", track->content_url, sizeof(track->content_url));
		json_obj_str(&doc, object, "mimeType", track->mime, sizeof(track->mime));
		json_obj_str(&doc, json_get(&doc, object, "metadata"), "filename", track->filename,
					 sizeof(track->filename));
		// The server's exact byte size when it states one; -1 keeps an absent
		// field "unknown" for the queue's expected_size records.
		track->size = json_obj_llong(&doc, json_get(&doc, object, "metadata"), "size", -1);
		if (!track->filename[0]) {
			size_t title_len = strnlen(track->title, sizeof(track->title));
			if (title_len >= sizeof(track->filename)) title_len = sizeof(track->filename) - 1;
			memcpy(track->filename, track->title, title_len);
			track->filename[title_len] = '\0';
		}
	}
	out->track_count = count;
	out->item.tracks = count;
	json_free(&doc);
	if (count == 0) {
		set_error("This Audiobookshelf item has no downloadable audio tracks.");
		return false;
	}
	return true;
}

bool audiobookshelf_book(const char *item_id, audiobookshelf_book_t *out) {
	if (!item_id || !item_id[0] || !out) return false;
	memset(out, 0, sizeof(*out));
	char id[ABS_ID_MAX * 3];
	http_url_encode(item_id, id, sizeof(id));
	char path[384];
	snprintf(path, sizeof(path), "/api/items/%s?expanded=1&include=progress,authors", id);
	// §8.3: one large-JSON catalog fetch at a time. The detail page and the
	// queue's identity fetch share this mutex, so two 3 MB-class bodies can
	// never be buffered at once and double the transient envelope.
	pthread_mutex_lock(&abs_catalog_lock);
	char *body = NULL;
	bool fetched = api_request(path, NULL, &body, NULL, NULL);
	if (fetched) {
		fetched = abs_parse_expanded(body, false, out);
	}
	pthread_mutex_unlock(&abs_catalog_lock);
	free(body);
	return fetched;
}

// ---------------------------------------------------------------------------
// Media origin rule (§11.3): relative contentUrl resolves against the request
// identity; an absolute contentUrl keeps its Bearer only when scheme, host and
// effective port are identical. Anything else is cross-origin and is rejected
// for 0.6.0 instead of risking the token on a foreign host. This compares at
// request time only -- the profile store's own normalization is untouched, so
// old manifests never rebind (§11.2).
// ---------------------------------------------------------------------------

// Splits an authority ("host", "host:port", "[v6-literal]:port") into host and
// explicit port. *port_out is 0 when the URL carries no port; the caller fills
// the scheme default. Anything else (unbracketed multi-colon authorities,
// non-numeric ports) fails the comparison: not real ABS output, and a
// cross-origin rejection is the safe reading.
static bool media_authority(const char *authority, char *host_out, size_t host_size, int *port_out) {
	*port_out = 0;
	size_t len = strlen(authority);
	if (len == 0) return false;
	if (authority[0] == '[') {
		const char *close = memchr(authority, ']', len);
		if (!close) return false;
		size_t literal = (size_t)(close - authority) + 1;
		if (literal >= host_size) return false;
		memcpy(host_out, authority, literal);
		host_out[literal] = '\0';
		if (literal < len) {
			if (authority[literal] != ':') return false;
			const char *digits = authority + literal + 1;
			if (!*digits) return false;
			for (const char *d = digits; *d; d++) if (!isdigit((unsigned char)*d)) return false;
			int port = atoi(digits);
			if (port <= 0 || port > 65535) return false;
			*port_out = port;
		}
		return true;
	}
	const char *colon = memchr(authority, ':', len);
	if (colon) {
		const char *digits = colon + 1;
		if (!*digits) return false;
		for (const char *d = digits; *d; d++) if (!isdigit((unsigned char)*d)) return false;
		size_t host_len = (size_t)(colon - authority);
		if (host_len == 0 || host_len >= host_size) return false;
		memcpy(host_out, authority, host_len);
		host_out[host_len] = '\0';
		int port = atoi(digits);
		if (port <= 0 || port > 65535) return false;
		*port_out = port;
		return true;
	}
	if (len >= host_size) return false;
	memcpy(host_out, authority, len + 1);
	return true;
}

// Normalized origin of one URL: lowercased scheme and host plus the effective
// port (explicit or scheme default). False for URLs this rule will not compare.
static bool media_origin_of(const char *url, char *out, size_t size) {
	out[0] = '\0';
	if (!url) return false;
	const char *separator = strstr(url, "://");
	if (!separator || separator == url) return false;
	size_t scheme_len = (size_t)(separator - url);
	char scheme[16];
	if (scheme_len >= sizeof(scheme)) return false;
	memcpy(scheme, url, scheme_len);
	scheme[scheme_len] = '\0';
	for (char *p = scheme; *p; p++) *p = (char)tolower((unsigned char)*p);
	int default_port = 0;
	if (strcmp(scheme, "http") == 0) default_port = 80;
	else if (strcmp(scheme, "https") == 0) default_port = 443;
	else return false;
	const char *authority = separator + 3;
	const char *path = strpbrk(authority, "/?#");
	size_t authority_len = path ? (size_t)(path - authority) : strlen(authority);
	if (authority_len == 0 || authority_len >= ABS_SERVER_MAX) return false;
	char authority_text[ABS_SERVER_MAX];
	memcpy(authority_text, authority, authority_len);
	authority_text[authority_len] = '\0';
	char host[ABS_SERVER_MAX];
	int port = 0;
	if (!media_authority(authority_text, host, sizeof(host), &port)) return false;
	// Scheme and host are case-insensitive in URIs; lowercase only the
	// comparison copy, never any stored identity string.
	for (char *p = host; *p; p++) *p = (char)tolower((unsigned char)*p);
	if ((size_t)snprintf(out, size, "%s://%s:%d", scheme, host, port > 0 ? port : default_port) >= size)
		return false;
	return true;
}

static bool media_origin_equal(const char *url, const char *server) {
	char url_origin[ABS_SERVER_MAX * 2], server_origin[ABS_SERVER_MAX * 2];
	return media_origin_of(url, url_origin, sizeof(url_origin)) &&
		media_origin_of(server, server_origin, sizeof(server_origin)) &&
		strcmp(url_origin, server_origin) == 0;
}

static bool make_dirs(const char *path) {
	char copy[1024];
	if (!path || !path[0] || strlen(path) >= sizeof(copy)) return false;
	snprintf(copy, sizeof(copy), "%s", path);
	for (char *p = copy + 1; *p; p++) {
		if (*p != '/') continue;
		*p = '\0';
		if (mkdir(copy, 0777) != 0 && errno != EEXIST) return false;
		*p = '/';
	}
	return mkdir(copy, 0777) == 0 || errno == EEXIST;
}

static void safe_component(const char *input, char *out, size_t size) {
	if (!out || size == 0) return;
	size_t w = 0;
	for (size_t i = 0; input && input[i] && w + 1 < size; i++) {
		unsigned char c = (unsigned char)input[i];
		if (c < 32 || strchr("<>:\"/\\|?*", c)) c = '_';
		if ((c == ' ' || c == '.') && w == 0) continue;
		out[w++] = (char)c;
	}
	while (w && (out[w - 1] == ' ' || out[w - 1] == '.')) w--;
	out[w] = '\0';
	if (!out[0]) snprintf(out, size, "Unknown");
}

// ---------------------------------------------------------------------------
// New-book destination (§11.5)
//
// Every book downloaded after 0.6.0 lives under a server-qualified directory:
//
//     <sd>/Audiobooks/Audiobookshelf/abs-<sha256(normalized-server)[0:12]>/
//       <safe-author>/<safe-title>/
//
// The pre-0.6.0 layout (<sd>/Audiobooks/Audiobookshelf/<author>/<title>) put
// one physical copy under author/title alone, so two servers carrying the same
// title -- even the same item id -- would collapse into one shareable
// directory and its single manifest. The hash prefix separates them, and the
// prefix is only a fast path: a download whose hash-prefix directory already
// carries another server's manifest is failed outright (below), because the
// manifest server -- not the prefix -- is the binding identity. Legacy 0.5.0
// books are never moved or rewritten; their manifest-less binding keeps
// resolving through legacy_server (§11.5).
// ---------------------------------------------------------------------------

// Twelve hex characters are 48 bits. Full digests would make every path name
// unwieldy; the collision case is covered by reading full manifest servers
// before any directory is shared, not by widening the prefix.
#define ABS_BOOK_HASH_LEN 12

// First ABS_BOOK_HASH_LEN hex characters of the SHA-256 over the normalized
// server identity. The same string goes into the manifest's server= line, so
// the directory a book lands in and the identity its manifest binds can never
// disagree about which server downloads it.
static void abs_book_hash_prefix(const char *server, char *out, size_t size) {
	char hex[SHA256_HEX_LEN];
	sha256_ctx ctx;
	sha256_init(&ctx);
	sha256_update(&ctx, server, strlen(server));
	sha256_final_hex(&ctx, hex);
	snprintf(out, size, "%.*s", ABS_BOOK_HASH_LEN, hex);
}

// Reads the server= binding of one manifest and normalizes it. Books under an
// abs- root are always written with server= (audiobookshelf_download), so a
// manifest that cannot be resolved to a server is treated as a foreign claim,
// never as ours (§11.5: fail the job rather than share the directory).
static bool abs_manifest_server(const char *manifest, char *out, size_t size) {
	if (out && size) out[0] = '\0';
	FILE *f = fopen(manifest, "r");
	if (!f) return false;
	char line[ABS_SERVER_MAX + 16];
	bool found = false;
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = '\0';
		if (strncmp(line, "server=", 7) == 0) {
			abs_profiles_normalize_server(line + 7, out, size);
			found = out[0] != '\0';
			break;
		}
	}
	fclose(f);
	return found;
}

// True when any manifest under the abs- hash root names a different
// Audiobookshelf server than `ours`, or a manifest with no resolvable server
// at all. Book manifests live two levels below the root (author/title/...);
// the depth cap keeps a surprise deep tree from walking the whole card, and
// only exact ".audiobookshelf" files are read, so .tmp sidecars, covers and
// partial .part files can never look like a claim.
static bool abs_dir_has_foreign_claim(const char *root, const char *ours, int depth) {
	DIR *dir = opendir(root);
	if (!dir) return false;
	char path[ABS_LOCAL_PATH_MAX];
	bool foreign = false;
	struct dirent *entry;
	while ((entry = readdir(dir)) != NULL && !foreign) {
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
		// d_type is unreliable on some card filesystems; opendir() is the test.
		if ((size_t)snprintf(path, sizeof(path), "%s/%s", root, entry->d_name) >= sizeof(path)) continue;
		if (strcmp(entry->d_name, ".audiobookshelf") == 0) {
			char bound[ABS_SERVER_MAX];
			if (!abs_manifest_server(path, bound, sizeof(bound))) foreign = true;
			else foreign = strcmp(bound, ours) != 0;
			continue;
		}
		DIR *sub = opendir(path);
		if (!sub) continue; // a file that is not a manifest carries no claim
		closedir(sub);
		if (entry->d_name[0] == '.') continue; // hidden helper directories
		if (depth > 0) foreign = abs_dir_has_foreign_claim(path, ours, depth - 1);
	}
	closedir(dir);
	return foreign;
}

typedef enum {
	ABS_DEST_OK,       // `out` holds the destination; the hash root is unclaimed
	ABS_DEST_COLLIDE,  // hash root exists and is bound to another server
	ABS_DEST_UNNAMEABLE // the destination cannot even be named (component overflow)
} abs_destination_t;

// Builds the new-book destination and checks the hash root's claim. Called
// BEFORE make_dirs and before any network or card write (§11.5): a refused
// destination must not leave traces on the card.
static abs_destination_t abs_book_destination(const char *sd_root, const char *server,
											  const char *author, const char *title,
											  char *out, size_t size) {
	char hash[16];
	abs_book_hash_prefix(server, hash, sizeof(hash));
	char root[ABS_LOCAL_PATH_MAX];
	if ((size_t)snprintf(root, sizeof(root), "%s/Audiobooks/Audiobookshelf/abs-%s", sd_root, hash) >= sizeof(root))
		return ABS_DEST_UNNAMEABLE;
	if ((size_t)snprintf(out, size, "%s/%s/%s", root, author, title) >= size)
		return ABS_DEST_UNNAMEABLE;
	struct stat st;
	if (stat(root, &st) != 0 || !S_ISDIR(st.st_mode)) return ABS_DEST_OK;
	if (abs_dir_has_foreign_claim(root, server, 2)) return ABS_DEST_COLLIDE;
	return ABS_DEST_OK;
}

static void encode_content_path(const char *input, char *out, size_t size) {
	static const char hex[] = "0123456789ABCDEF";
	size_t w = 0;
	for (size_t i = 0; input && input[i] && w + 4 < size; i++) {
		unsigned char c = (unsigned char)input[i];
		bool safe = isalnum(c) || strchr("/-._~%?=&", c);
		if (safe) out[w++] = (char)c;
		else {
			out[w++] = '%';
			out[w++] = hex[c >> 4];
			out[w++] = hex[c & 15];
		}
	}
	out[w] = '\0';
}

static bool write_sidecars(const char *audio, const audiobookshelf_book_t *book,
						   const audiobookshelf_track_t *track, int number) {
	char tags[ABS_SIDECAR_PATH_MAX], temporary[ABS_TEMP_PATH_MAX];
	if ((size_t)snprintf(tags, sizeof(tags), "%s.tags", audio) >= sizeof(tags) ||
		(size_t)snprintf(temporary, sizeof(temporary), "%s.tmp", tags) >= sizeof(temporary)) return false;
	FILE *f = fopen(temporary, "w");
	if (!f) return false;
	const char *display_title = book->track_count == 1 ? book->item.title :
		(track->title[0] ? track->title : track->filename);
	fprintf(f, "title=%s\n", display_title);
	fprintf(f, "artist=%s\n", book->item.author);
	fprintf(f, "album_artist=%s\n", book->item.author);
	fprintf(f, "album=%s\n", book->item.title);
	fprintf(f, "series=%s\n", book->item.series);
	fprintf(f, "series_part=%s\n", book->series_part);
	fprintf(f, "track=%d\n", number);
	fprintf(f, "abs_item_id=%s\n", book->item.id);
	if (fclose(f) != 0 || rename(temporary, tags) != 0) {
		remove(temporary);
		return false;
	}
	return true;
}

static bool write_description(const char *audio, const char *description) {
	if (!description || !description[0]) return true;
	char path[ABS_SIDECAR_PATH_MAX], temporary[ABS_TEMP_PATH_MAX];
	if ((size_t)snprintf(path, sizeof(path), "%s.description", audio) >= sizeof(path) ||
		(size_t)snprintf(temporary, sizeof(temporary), "%s.tmp", path) >= sizeof(temporary)) return false;
	FILE *f = fopen(temporary, "wb");
	if (!f) return false;
	size_t len = strlen(description);
	bool ok = fwrite(description, 1, len, f) == len;
	if (fclose(f) != 0) ok = false;
	if (!ok || rename(temporary, path) != 0) {
		remove(temporary);
		return false;
	}
	return true;
}

// Streams one file. The Bearer is built from the caller's snapshot token for
// the resolved identity server (§11.3) -- never from the legacy globals --
// and http_stream_open_ex still strips it on a cross-origin redirect.
static bool download_file(const char *url, const char *token, const char *destination, int track_no,
						  int track_count, audiobookshelf_progress_cb cb, void *user) {
	struct stat existing;
	if (stat(destination, &existing) == 0 && existing.st_size > 0) return true;

	char headers[ABS_TOKEN_MAX + 32];
	headers[0] = '\0';
	// A bearer-less fetch is not used in 0.6.0 (every ABS origin requires the
	// token); the guard only keeps a malformed caller from printing "(null)".
	if (token && token[0]) snprintf(headers, sizeof(headers), "Authorization: Bearer %s\r\n", token);
	http_stream_t stream;
	if (!http_stream_open_ex(&stream, url, headers, ABS_TIMEOUT_SECS)) {
		const char *why = http_last_error();
		set_error(why && why[0] ? why : "The audiobook file could not be downloaded.");
		return false;
	}
	char partial[ABS_TEMP_PATH_MAX];
	if ((size_t)snprintf(partial, sizeof(partial), "%s.part", destination) >= sizeof(partial)) {
		http_stream_close(&stream);
		set_error("The audiobook download path is too long.");
		return false;
	}
	FILE *f = fopen(partial, "wb");
	if (!f) {
		http_stream_close(&stream);
		set_error(errno == ENOSPC ? "The SD card is full." : "The audiobook file could not be written.");
		return false;
	}
	char buffer[ABS_DOWNLOAD_CHUNK];
	long long done = 0;
	bool ok = true;
	for (;;) {
		int n = http_stream_read(&stream, buffer, sizeof(buffer));
		if (n == 0) break;
		if (n < 0 || fwrite(buffer, 1, (size_t)n, f) != (size_t)n) {
			ok = false;
			break;
		}
		done += n;
		if (cb) cb(track_no, track_count, done, stream.content_length, user);
	}
	if (fclose(f) != 0) ok = false;
	http_stream_close(&stream);
	if (!ok || done == 0 || (stream.content_length > 0 && done != stream.content_length) || rename(partial, destination) != 0) {
		remove(partial);
		set_error(errno == ENOSPC ? "The SD card is full." : "The audiobook download was interrupted.");
		return false;
	}
	return true;
}

static bool local_track_name(const audiobookshelf_track_t *track, int index, char *out, size_t size) {
	char filename[240];
	safe_component(track->filename[0] ? track->filename : track->title, filename, sizeof(filename));
	return (size_t)snprintf(out, size, "%03d - %s", index + 1, filename) < size;
}

bool audiobookshelf_resume_target(const audiobookshelf_book_t *book, const char *book_path,
									 char *file_out, size_t file_size, double *seconds_out,
									 bool *finished_out) {
	if (file_out && file_size) file_out[0] = '\0';
	if (seconds_out) *seconds_out = 0;
	if (finished_out) *finished_out = false;
	if (!book || !book_path || !book_path[0] || !file_out || file_size == 0 || book->track_count <= 0)
		return false;

	bool finished = book->item.finished;
	double whole = finished ? 0 : book->item.progress;
	if (whole < 0) whole = 0;
	if (book->item.duration > 0 && whole > book->item.duration) whole = book->item.duration;

	int selected = 0;
	if (!finished) {
		for (int i = 1; i < book->track_count; i++) {
			if (book->tracks[i].start_offset <= whole + 0.001) selected = i;
			else break;
		}
	}

	char local_name[280];
	if (!local_track_name(&book->tracks[selected], selected, local_name, sizeof(local_name)) ||
		(size_t)snprintf(file_out, file_size, "%s/%s", book_path, local_name) >= file_size) {
		file_out[0] = '\0';
		return false;
	}

	double relative = whole - book->tracks[selected].start_offset;
	if (relative < 0) relative = 0;
	if (book->tracks[selected].duration > 0 && relative > book->tracks[selected].duration)
		relative = book->tracks[selected].duration;
	if (seconds_out) *seconds_out = relative;
	if (finished_out) *finished_out = finished;
	return true;
}

typedef struct {
	char (*paths)[ABS_LOCAL_PATH_MAX];
	int capacity;
	int count;
} local_files_t;

static bool collect_local_file(const char *path, const char *title, void *user) {
	(void)title;
	local_files_t *files = user;
	if (files->count >= files->capacity) return false;
	snprintf(files->paths[files->count++], ABS_LOCAL_PATH_MAX, "%s", path);
	return true;
}

static bool local_file_duration(const char *path, double *seconds) {
	decode_format_t format = decode_detect_format(path);
	if (format == DECODE_FORMAT_UNKNOWN) return false;
	decoder_t *dec = decoder_open(path, format);
	if (!dec) return false;
	int rate = decoder_sample_rate(dec);
	uint64_t frames = decoder_total_pcm_frames(dec);
	decoder_close(dec);
	if (rate <= 0 || frames == 0) return false;
	*seconds = (double)frames / (double)rate;
	return *seconds > 0;
}

// Reads a local book's .audiobookshelf manifest (§11.5). True when the file
// opened; the three answers are all collected regardless of what matches:
// `server=` is written after `item_id=` in the manifest, so neither line may
// stop the scan early, and `version=` is the manifest's own format commitment
// (validate version and required fields before trusting the binding).
static bool read_local_manifest(const char *local_book, bool *version_ok,
								char *item_id, size_t item_id_size,
								char *bound_server, size_t bound_size) {
	if (version_ok) *version_ok = false;
	if (item_id && item_id_size) item_id[0] = '\0';
	if (bound_server && bound_size) bound_server[0] = '\0';
	if (!local_book || !local_book[0]) return false;

	char folder[ABS_LOCAL_PATH_MAX];
	snprintf(folder, sizeof(folder), "%s", local_book);
	if (!audiobookdb_is_folder_book(local_book)) {
		char *slash = strrchr(folder, '/');
		if (!slash) return false;
		*slash = '\0';
	}

	char manifest[ABS_LOCAL_PATH_MAX];
	if ((size_t)snprintf(manifest, sizeof(manifest), "%s/.audiobookshelf", folder) >= sizeof(manifest))
		return false;
	FILE *f = fopen(manifest, "r");
	if (!f) return false;

	char line[ABS_SERVER_MAX + 32];
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = '\0';
		if (strncmp(line, "item_id=", 8) == 0) {
			// Same bounded copy as the server line below: item_id_size is the
			// caller's column ceiling, and the line itself is attacker-free
			// never. (Precision form also keeps -Wformat-truncation quiet when
			// the reader is inlined into larger callers.)
			if (item_id && item_id_size)
				snprintf(item_id, item_id_size, "%.*s", (int)item_id_size - 1, line + 8);
		} else if (strncmp(line, "server=", 7) == 0 && (!bound_server || !bound_server[0])) {
			if (bound_server) snprintf(bound_server, bound_size, "%.*s", (int)bound_size - 1, line + 7);
		} else if (strcmp(line, "version=1") == 0) {
			if (version_ok) *version_ok = true;
		}
	}
	fclose(f);
	return true;
}

// Resolves a manifest's server binding with the §11.5 rules: the raw `server=`
// line or, for a 0.5.0 manifest without one, the persisted legacy server.
// Empty output means no reliable binding -- an explicit re-link is required.
static void manifest_bound_server(const char *bound_server, char *resolved, size_t size) {
	resolved[0] = '\0';
	if (!bound_server || !bound_server[0]) {
		legacy_manifest_binding(resolved, size);
		return;
	}
	abs_profiles_normalize_server(bound_server, resolved, size);
}

// True when the local book's manifest binds THIS server item: a valid
// version=1 manifest whose resolved server and item_id both match (§11.5).
// Matching item_id alone would let server B's copy of one book claim
// server A's local files. A manifest without `server=` (0.5.0 vintage) resolves
// to the persisted legacy_server; before any profile store exists it binds to
// the sole configured identity, exactly as 0.5.0 did, and never to a guess.
// With no reliable binding, false -- an explicit re-link is required and local
// progress stays pending.
// This lets the server library open a previously linked copy directly instead
// of offering to link the same book again.
bool audiobookshelf_local_linked(const char *local_book, const char *item_id) {
	if (!item_id || !item_id[0]) return false;

	bool version_ok = false;
	char bound_item[ABS_ID_MAX], bound[ABS_SERVER_MAX] = "";
	if (!read_local_manifest(local_book, &version_ok, bound_item, sizeof(bound_item),
							 bound, sizeof(bound)))
		return false;
	// A manifest without a recognized version or a matching item id is not a
	// linkable binding: it must never open a copy that some other state wrote.
	if (!version_ok || strcmp(bound_item, item_id) != 0) return false;

	char resolved[ABS_SERVER_MAX];
	manifest_bound_server(bound, resolved, sizeof(resolved));
	if (!resolved[0]) return false; // §11.5: no reliable server -> explicit re-link, keep progress pending
	char identity[ABS_SERVER_MAX], normalized_identity[ABS_SERVER_MAX];
	if (!request_credentials_copy(NULL, identity, sizeof(identity), NULL, 0) || !identity[0]) return false;
	abs_profiles_normalize_server(identity, normalized_identity, sizeof(normalized_identity));
	return strcmp(resolved, normalized_identity) == 0;
}

// UI decision for a unique local match that is not linked to this server item
// (§11.5): another server's copy. The manifest is a valid version=1 binding
// that resolves to a server OTHER than the active browsing identity -- the
// user is offered a download from this server, never a re-link (rewriting the
// other server's manifest would move its binding), and the download lands in
// this server's own abs-<hash> directory without touching that copy. With no
// reliable binding, or a binding to this very server with a different item id
// (an owned copy bound to a different server item), the existing explicit
// re-link question still applies.
bool audiobookshelf_local_owned_elsewhere(const char *local_book) {
	bool version_ok = false;
	char bound_item[ABS_ID_MAX], bound[ABS_SERVER_MAX] = "";
	if (!read_local_manifest(local_book, &version_ok, bound_item, sizeof(bound_item),
							 bound, sizeof(bound)) || !version_ok)
		return false;
	char resolved[ABS_SERVER_MAX];
	manifest_bound_server(bound, resolved, sizeof(resolved));
	if (!resolved[0]) return false; // no reliable binding -> re-link path, not "another server"
	char identity[ABS_SERVER_MAX], normalized_identity[ABS_SERVER_MAX];
	if (!request_credentials_copy(NULL, identity, sizeof(identity), NULL, 0) || !identity[0])
		return false; // no active identity to compare against: keep the re-link path
	abs_profiles_normalize_server(identity, normalized_identity, sizeof(normalized_identity));
	return strcmp(resolved, normalized_identity) != 0;
}

bool audiobookshelf_link_local(const audiobookshelf_book_t *book, const char *local_book,
								 char *file_out, size_t file_size, double *seconds_out,
								 bool *finished_out) {
	if (file_out && file_size) file_out[0] = '\0';
	if (seconds_out) *seconds_out = 0;
	if (finished_out) *finished_out = false;
	if (!book || !local_book || !local_book[0] || book->track_count <= 0 ||
		!file_out || file_size == 0 || book->item.duration <= 0) {
		set_error("This local book cannot be linked.");
		return false;
	}
	// The manifest must bind the exact server the item was browsed from
	// (§11.3/§11.5): one identity decides, and nothing reads the legacy
	// globals directly. Normalized exactly like the download's manifest so the
	// two writers can never disagree on the binding spelling (round-trip
	// through a scratch buffer: the normalizer clears its output first).
	char server[ABS_SERVER_MAX];
	if (!request_credentials_copy(NULL, server, sizeof(server), NULL, 0) || !server[0]) {
		set_error("Audiobookshelf is not configured.");
		return false;
	}
	char normalized[ABS_SERVER_MAX];
	abs_profiles_normalize_server(server, normalized, sizeof(normalized));
	if (!normalized[0]) {
		set_error("The Audiobookshelf server is invalid.");
		return false;
	}
	snprintf(server, sizeof(server), "%s", normalized);

	local_files_t files = {
		.paths = calloc((size_t)book->track_count, ABS_LOCAL_PATH_MAX),
		.capacity = book->track_count,
	};
	if (!files.paths) {
		set_error("There is not enough memory to link this book.");
		return false;
	}
	if (audiobookdb_is_folder_book(local_book))
		audiobookdb_parts_for_each(local_book, collect_local_file, &files);
	else {
		snprintf(files.paths[0], ABS_LOCAL_PATH_MAX, "%s", local_book);
		files.count = 1;
	}
	if (files.count != book->track_count) {
		free(files.paths);
		set_error("The local and server copies have different part counts.");
		return false;
	}

	double *durations = calloc((size_t)files.count, sizeof(*durations));
	if (!durations) {
		free(files.paths);
		set_error("There is not enough memory to link this book.");
		return false;
	}
	double total = 0;
	for (int i = 0; i < files.count; i++) {
		if (!local_file_duration(files.paths[i], &durations[i])) {
			free(durations);
			free(files.paths);
			set_error("A local part could not be measured.");
			return false;
		}
		total += durations[i];
	}
	double difference = total - book->item.duration;
	if (difference < 0) difference = -difference;
	double tolerance = 30.0 + book->item.duration * 0.001;
	if (difference > tolerance) {
		free(durations);
		free(files.paths);
		set_error("The local and server copies have different running times.");
		return false;
	}

	char folder[ABS_LOCAL_PATH_MAX];
	snprintf(folder, sizeof(folder), "%s", files.paths[0]);
	char *slash = strrchr(folder, '/');
	if (!slash) {
		free(durations);
		free(files.paths);
		set_error("The local audiobook path is invalid.");
		return false;
	}
	*slash = '\0';
	char manifest[ABS_LOCAL_PATH_MAX], temporary[ABS_TEMP_PATH_MAX];
	if ((size_t)snprintf(manifest, sizeof(manifest), "%s/.audiobookshelf", folder) >= sizeof(manifest) ||
		(size_t)snprintf(temporary, sizeof(temporary), "%s.tmp", manifest) >= sizeof(temporary)) {
		free(durations);
		free(files.paths);
		set_error("The local audiobook path is too long.");
		return false;
	}
	FILE *f = fopen(temporary, "w");
	if (!f) {
		free(durations);
		free(files.paths);
		set_error("The Audiobookshelf link could not be written.");
		return false;
	}
	fprintf(f, "version=1\nitem_id=%s\nserver=%s\nduration=%.3f\nprogress=%.3f\nprogress_updated=%lld\n",
			book->item.id, server, total, book->item.progress, book->progress_updated_ms);
	double start = 0;
	for (int i = 0; i < files.count; i++) {
		const char *base = strrchr(files.paths[i], '/');
		base = base ? base + 1 : files.paths[i];
		fprintf(f, "track=%d\t%.3f\t%.3f\t%s\n", i + 1, start, durations[i], base);
		start += durations[i];
	}
	bool written = fclose(f) == 0 && rename(temporary, manifest) == 0;
	if (!written) {
		remove(temporary);
		free(durations);
		free(files.paths);
		set_error("The Audiobookshelf link could not be written.");
		return false;
	}

	bool finished = book->item.finished;
	audiobookdb_abs_reset(local_book);
	double whole = finished ? 0 : book->item.progress;
	if (whole < 0) whole = 0;
	if (whole > total) whole = total;
	int selected = 0;
	start = 0;
	for (int i = 0; i < files.count; i++) {
		if (i + 1 < files.count && whole >= start + durations[i]) {
			start += durations[i];
			selected = i + 1;
			continue;
		}
		break;
	}
	snprintf(file_out, file_size, "%s", files.paths[selected]);
	if (seconds_out) *seconds_out = whole - start;
	if (finished_out) *finished_out = finished;
	free(durations);
	free(files.paths);
	set_error("");
	return true;
}

bool audiobookshelf_download(const audiobookshelf_book_t *book, const char *sd_root,
								 audiobookshelf_progress_cb cb, void *user, char *book_path, size_t path_size) {
	if (!book || !sd_root || !sd_root[0] || book->track_count <= 0) {
		set_error("No SD card is available for the download.");
		return false;
	}
	// One identity for the whole job (§11.3): the book was fetched from this
	// server, so its contentUrls, manifest and Bearer all belong to the same
	// snapshot. Resolve it once and never read the legacy globals below.
	char server[ABS_SERVER_MAX], token[ABS_TOKEN_MAX];
	if (!request_credentials_copy(NULL, server, sizeof(server), token, sizeof(token)) || !server[0]) {
		set_error("Audiobookshelf is not configured.");
		return false;
	}
	// §11.5: the destination hash and the manifest's server= line must be the
	// SAME string, so the identity is re-normalized here even though profile
	// storage already normalizes on write -- a manifest and a directory hash
	// derived from different spellings of one URL would defeat the collision
	// gate below. (abs_profiles_normalize_server clears its output first, so
	// the round-trip goes through a scratch buffer rather than in place.)
	char normalized[ABS_SERVER_MAX];
	abs_profiles_normalize_server(server, normalized, sizeof(normalized));
	if (!normalized[0]) {
		set_error("The Audiobookshelf server is invalid.");
		memset(token, 0, sizeof(token));
		return false;
	}
	snprintf(server, sizeof(server), "%s", normalized);
	char author[160], title[220];
	safe_component(book->item.author[0] ? book->item.author : "Unknown Author", author, sizeof(author));
	safe_component(book->item.title, title, sizeof(title));
	char folder[900];
	// §11.5: the destination is refused BEFORE any byte is written. The hash
	// gate runs first (a directory claimed by another server is never shared),
	// then the shared path ceiling: every final track path must be
	// representable by the narrowest downstream consumer (the durable
	// checkpoint row and the progress watch slots carry the same strings) --
	// still before make_dirs, so a refused job leaves no card residue.
	abs_destination_t destination = abs_book_destination(sd_root, server, author, title, folder, sizeof(folder));
	if (destination == ABS_DEST_COLLIDE) {
		set_error("The download path is already used by another Audiobookshelf server.");
		memset(token, 0, sizeof(token));
		return false;
	}
	if (destination != ABS_DEST_OK) {
		set_error("The audiobook download path is too long.");
		memset(token, 0, sizeof(token));
		return false;
	}
	for (int i = 0; i < book->track_count; i++) {
		char numbered[280];
		char track_destination[ABS_LOCAL_PATH_MAX];
		if (!local_track_name(&book->tracks[i], i, numbered, sizeof(numbered)) ||
			(size_t)snprintf(track_destination, sizeof(track_destination), "%s/%s", folder, numbered) >= sizeof(track_destination) ||
			!sonix_linked_path_representable(strlen(track_destination))) {
			set_error("The audiobook download path is too long.");
			memset(token, 0, sizeof(token));
			return false;
		}
	}
	if (!make_dirs(folder)) {
		set_error("The Audiobookshelf download folder could not be created.");
		memset(token, 0, sizeof(token));
		return false;
	}

	for (int i = 0; i < book->track_count; i++) {
		const audiobookshelf_track_t *track = &book->tracks[i];
		char numbered[280];
		char track_destination[ABS_LOCAL_PATH_MAX];
		if (!local_track_name(track, i, numbered, sizeof(numbered)) ||
			(size_t)snprintf(track_destination, sizeof(track_destination), "%s/%s", folder, numbered) >= sizeof(track_destination)) {
			set_error("The audiobook download path is too long.");
			memset(token, 0, sizeof(token));
			return false;
		}

		char encoded[ABS_URL_MAX * 2];
		encode_content_path(track->content_url, encoded, sizeof(encoded));
		char url[ABS_URL_MAX * 2 + 512];
		if (strncmp(track->content_url, "http://", 7) == 0 || strncmp(track->content_url, "https://", 8) == 0) {
			// §11.3 media rule: an absolute contentUrl only keeps its Bearer
			// when scheme, host and effective port match the request identity.
			// Cross-origin media is rejected before anything is fetched; the
			// redirect path in http.c still strips Authorization on its own.
			if (!media_origin_equal(track->content_url, server)) {
				set_error("The audiobook file is hosted by another server.");
				memset(token, 0, sizeof(token));
				return false;
			}
			// Schemes need their colon intact. Audiobookshelf normally returns
			// a relative /s/item path; tolerate an absolute one without
			// treating its authority as a path segment.
			const char *path = strstr(track->content_url, "://");
			path = path ? strchr(path + 3, '/') : NULL;
			if (path) {
				char origin[512];
				size_t origin_len = (size_t)(path - track->content_url);
				if (origin_len >= sizeof(origin)) origin_len = sizeof(origin) - 1;
				memcpy(origin, track->content_url, origin_len);
				origin[origin_len] = '\0';
				encode_content_path(path, encoded, sizeof(encoded));
				snprintf(url, sizeof(url), "%s%s", origin, encoded);
			} else snprintf(url, sizeof(url), "%s", track->content_url);
		} else if (encoded[0] == '/') snprintf(url, sizeof(url), "%s%s", server, encoded);
		else snprintf(url, sizeof(url), "%s/%s", server, encoded);

		if (!download_file(url, token, track_destination, i + 1, book->track_count, cb, user) ||
			!write_sidecars(track_destination, book, track, i + 1) ||
			(i == 0 && !write_description(track_destination, book->description))) {
			memset(token, 0, sizeof(token));
			return false;
		}
	}

	// Embedded artwork remains the first choice. Audiobookshelf's own cover is
	// a useful fallback for books whose source files carry none; asking for JPEG
	// keeps it compatible with the R1's folder-art decoder. A missing cover must
	// never turn a successful audio download into a failed book download.
	if (book->item.cover[0]) {
		char encoded[ABS_URL_MAX * 2];
		encode_content_path(book->item.cover, encoded, sizeof(encoded));
		// The cover path is always the server's own /api route, so it shares
		// the request identity's origin by construction.
		char cover_url[ABS_URL_MAX * 2 + 544];
		if (encoded[0] == '/') snprintf(cover_url, sizeof(cover_url), "%s%s?format=jpeg", server, encoded);
		else snprintf(cover_url, sizeof(cover_url), "%s/%s?format=jpeg", server, encoded);
		char cover_path[ABS_LOCAL_PATH_MAX];
		if ((size_t)snprintf(cover_path, sizeof(cover_path), "%s/cover.jpg", folder) < sizeof(cover_path))
			(void)download_file(cover_url, token, cover_path, 1, 1, NULL, NULL);
	}
	memset(token, 0, sizeof(token));

	char manifest[1000], temporary[1020];
	snprintf(manifest, sizeof(manifest), "%s/.audiobookshelf", folder);
	snprintf(temporary, sizeof(temporary), "%s.tmp", manifest);
	FILE *f = fopen(temporary, "w");
	if (f) {
		fprintf(f, "version=1\nitem_id=%s\nserver=%s\nduration=%.3f\nprogress=%.3f\nprogress_updated=%lld\n",
				book->item.id, server, book->item.duration, book->item.progress, book->progress_updated_ms);
		for (int i = 0; i < book->track_count; i++) {
			char local_name[280];
			if (!local_track_name(&book->tracks[i], i, local_name, sizeof(local_name))) continue;
			fprintf(f, "track=%d\t%.3f\t%.3f\t%s\n", i + 1, book->tracks[i].start_offset,
					book->tracks[i].duration, local_name);
		}
		fclose(f);
		rename(temporary, manifest);
	}
	if (book_path && path_size) snprintf(book_path, path_size, "%s", folder);
	audiobookdb_abs_reset(folder);
	set_error("");
	return true;
}

// ---------------------------------------------------------------------------
// The download queue's real media surface (§8.2 steps 1–4, §8.5 split)
//
// One active job at a time (§8.1): the resolved auth snapshot, the expanded
// book and the computed destination below are module state valid for the
// active job's attempt. The queue worker runs exactly one op at a time and
// never from the LVGL thread, so these statics — like the progress worker's
// own state — need no extra lock; every entry point asserts the attempt it
// belongs to before touching anything. The expanded book is held only while
// its job is the active one and dies with the attempt snapshot (§8.2
// steps 2, 8: freed before the next job starts).
//
// Cancel/pause reach every op through ctx->cancel (§8.1) and through the
// op context's wake slot (§8.5 line 1028): the transfer and the finalize's
// identity refetch poll the flag between units, the identity fetch while it
// waits for the shared catalog mutex, and every blocked network phase inside
// the http calls is woken by the slot's shutdown — a pause below the receive
// timeout. The finalize and acknowledgement-reset ops are the §8.2 steps 5–8
// commit: sidecars, cover, resume mapping and the manifest (written LAST,
// atomic), then the audiobookdb reset and the queue's DONE transaction —
// books only; episode jobs are the podcast wave's documented refusals.
// ---------------------------------------------------------------------------

typedef struct {
	long long job_id;                 // re-armed by the identity fetch
	char server[ABS_SERVER_MAX];      // normalized job server identity
	char destination[ABS_DL_DEST_MAX]; // card-relative, computed at fetch
	abs_auth_snapshot_t auth;         // the attempt's own snapshot (§8.1)
	audiobookshelf_book_t book;       // §8.2 step 2: held for the active job
	bool held;                        // auth+server valid between attempt steps
} abs_queue_media_t;

static abs_queue_media_t queue_media;
// Plan scratch: strings the plan points at must live until the queue ingests
// it (plan_ingest copies them into DOWNLOAD_FILE rows) — the next op for this
// job is the only thing allowed to overwrite them.
static abs_download_file_req_t queue_plan_files[ABS_TRACK_MAX];
static char queue_plan_urls[ABS_TRACK_MAX][ABS_DL_URL_MAX];
static char queue_plan_names[ABS_TRACK_MAX][ABS_DL_NAME_MAX];

// Progress rows are polled at >=4 Hz (§8.1); one publish per 64 KiB keeps the
// mutex traffic bounded without stretching the visible bar.
#define ABS_QUEUE_PROGRESS_COALESCE_BYTES (long long)(64 * 1024)
// Bounded bound of the cancellable wait for the shared catalog mutex: longer
// than any single catalog fetch can legally take (§8.3 serializes two of
// them), so a wedged holder cannot stall the worker forever.
#define ABS_QUEUE_CATALOG_WAIT_SECS 60

static void abs_queue_fail(const char **stage_out, char *error, size_t error_size,
						   const char *stage, const char *message) {
	*stage_out = stage;
	snprintf(error, error_size, "%s", message);
}

// Cancellable wait for the shared large-catalog mutex (§8.3): the queue must
// not lose a pause behind a detail page. `false` when the cancel flag rose or
// the bounded window ran out (a wedged holder cannot stall a job forever).
static bool abs_catalog_lock_wait(const volatile int *cancel) {
	struct timespec pause = {0, 20 * 1000 * 1000}; // 20 ms
	time_t deadline = time(NULL) + ABS_QUEUE_CATALOG_WAIT_SECS;
	for (;;) {
		if (pthread_mutex_trylock(&abs_catalog_lock) == 0) return true;
		nanosleep(&pause, NULL);
		if (abs_download_op_cancelled(cancel)) return false;
		if (time(NULL) >= deadline) return false;
	}
}

bool audiobookshelf_queue_auth_resolve(const char *profile_id, const char *server,
									   abs_auth_snapshot_t *out, char *error, size_t error_size) {
#define RESOLVE_FAIL(message)                                                                   \
	do {                                                                                        \
		if (error && error_size) snprintf(error, error_size, "%s", message);                    \
		abs_profiles_snapshot_wipe(out);                                                        \
		return false;                                                                           \
	} while (0)

	if (error && error_size) error[0] = '\0';
	if (!out) return false;
	memset(out, 0, sizeof(*out));
	if (!server || !server[0]) {
		RESOLVE_FAIL("The download job has no server address to authenticate against.");
	}
	char expected[ABS_SERVER_MAX];
	abs_profiles_normalize_server(server, expected, sizeof(expected));
	if (!expected[0]) {
		RESOLVE_FAIL("The download job has no usable server address.");
	}
	// §11.4: a row's own profile owns its token — the active one is only the
	// fallback for rows that carry no profile id (the migration window), and
	// even then only after the same-server check below. The snapshot is read
	// under the store lock, so the latest token wins every attempt (§8.1).
	bool resolved = false;
	if (profile_id && profile_id[0]) {
		resolved = abs_profiles_snapshot(profile_id, out);
		if (!resolved) {
			RESOLVE_FAIL("Could not resolve the download's login profile.");
		}
	} else {
		resolved = abs_profiles_snapshot_active(out);
		if (!resolved) {
			RESOLVE_FAIL("Could not resolve the login profile for the download.");
		}
	}
	if (!out->token[0]) {
		RESOLVE_FAIL("Profile token for the audiobook server isn't set");
	}
	char normalized[ABS_SERVER_MAX];
	abs_profiles_normalize_server(out->server, normalized, sizeof(normalized));
	if (!normalized[0] || strcmp(normalized, expected) != 0) {
		// A snapshot whose server drifted (or another profile's server
		// entirely) is never adopted: another server's token must not leak
		// into this job, whichever profile it came from.
		RESOLVE_FAIL("This download's server does not match the profile's server.");
	}
	return true;
#undef RESOLVE_FAIL
}

void audiobookshelf_queue_auth_forget(abs_auth_snapshot_t *snapshot) {
	// §8.1: the snapshot never outlives one attempt; §8.2 step 8 releases the
	// expanded book with it. The scratch plan dies here too: its strings were
	// ingested (or are useless with the attempt gone).
	queue_media.job_id = 0;
	queue_media.server[0] = '\0';
	queue_media.destination[0] = '\0';
	memset(&queue_media.auth, 0, sizeof(queue_media.auth));
	memset(&queue_media.book, 0, sizeof(queue_media.book));
	memset(queue_plan_files, 0, sizeof(queue_plan_files));
	memset(queue_plan_urls, 0, sizeof(queue_plan_urls));
	memset(queue_plan_names, 0, sizeof(queue_plan_names));
	queue_media.held = false;
	abs_profiles_snapshot_wipe(snapshot); // volatile sink keeps the pass alive
}

static bool abs_queue_auth_acquire(void *user, const char *profile_id, const char *server,
								   abs_auth_snapshot_t *out) {
	(void)user;
	if (out) {
		memset(out, 0, sizeof(*out));
	}
	if (!server || !server[0]) {
		return false;
	}
	char error[256];
	if (!audiobookshelf_queue_auth_resolve(profile_id, server, out, error, sizeof(error))) {
		// The queue records its own stage-auth message; the resolver's reason
		// is for direct drivers and would only duplicate it in a row.
		return false;
	}
	// The attempt's working copy: identity fetch and transfers use this, and
	// auth_release wipes the whole state when the attempt ends. The server is
	// re-normalized exactly like audiobookshelf_download() does (scratch first:
	// the normalizer clears its output), so the destination hash and every
	// origin comparison spell one string (§11.5).
	char normalized[ABS_SERVER_MAX];
	abs_profiles_normalize_server(server, normalized, sizeof(normalized));
	if (!normalized[0]) {
		return false;
	}
	memset(&queue_media, 0, sizeof(queue_media)); // a fresh attempt's state
	snprintf(queue_media.server, sizeof(queue_media.server), "%s", normalized);
	queue_media.auth = *out;
	queue_media.held = true;
	return true;
}

static void abs_queue_auth_release(void *user, abs_auth_snapshot_t *snapshot) {
	(void)user;
	audiobookshelf_queue_auth_forget(snapshot);
}

// The attempt the op belongs to: the state must be held, name the same server
// the job does, and (from the fetch on) the same job id the context carries.
// A miss is a caller bug -- the real queue cannot produce one -- and stage
// "metadata" is the honest permanent refusal, never a guessed retry.
static bool abs_queue_attempt_ok(const abs_download_op_ctx_t *ctx, const abs_download_job_t *job,
								 char *error, size_t error_size) {
	if (!queue_media.held || !queue_media.auth.token[0]) {
		snprintf(error, error_size, "%s", "The download's login snapshot was not resolved.");
		return false;
	}
	char normalized[ABS_SERVER_MAX];
	abs_profiles_normalize_server(job ? job->server : "", normalized, sizeof(normalized));
	if (!normalized[0] || strcmp(normalized, queue_media.server) != 0) {
		snprintf(error, error_size, "%s", "The download's server does not match its login snapshot.");
		return false;
	}
	if (ctx && queue_media.job_id && ctx->job_id != queue_media.job_id) {
		snprintf(error, error_size, "%s", "The download's media state belongs to another job.");
		return false;
	}
	return true;
}

// Reads the .audiobookshelf manifest that may occupy the computed destination
// and tells whether it belongs to this very job (same normalized server AND
// same item). A manifest for another item — even from our own server — is the
// §8.2 path conflict: a destination is a job identity, not a scratch share.
static bool abs_dest_manifest_mine(const char *dest_abs, const char *server, const char *item_id) {
	char path[SONIX_LINKED_SIDECAR_PATH_MAX];
	if ((size_t)snprintf(path, sizeof(path), "%.1280s/.audiobookshelf", dest_abs) >= sizeof(path)) {
		return false;
	}
	struct stat st;
	if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
		return true; // no manifest: the destination is unowned, first binding wins
	}
	char bound[ABS_SERVER_MAX];
	char bound_item[ABS_SERVER_MAX + 32];
	bool server_ok_line = false;
	bool item_ok_line = false;
	FILE *f = fopen(path, "r");
	if (!f) return false; // unreadable: refuse to share blindly
	char line[ABS_SERVER_MAX + 32];
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = '\0';
		if (strncmp(line, "server=", 7) == 0) {
			abs_profiles_normalize_server(line + 7, bound, sizeof(bound));
			server_ok_line = bound[0] != '\0' && strcmp(bound, server) == 0;
		} else if (strncmp(line, "item_id=", 8) == 0) {
			// Lossless within the line's own bound: a prefix-truncated id
			// could never prove an identity match.
			snprintf(bound_item, sizeof(bound_item), "%s", line + 8);
			item_ok_line = bound_item[0] != '\0' && strcmp(bound_item, item_id) == 0;
		}
	}
	fclose(f);
	// §11.5: a manifest without a resolvable server is a foreign claim, never
	// ours (pre-0.6.0 books never live under the hashed roots).
	return server_ok_line && item_ok_line;
}

// §8.2 step 1–3: fetch the expanded identity with the attempt's snapshot,
// validate every track (explicitly refusing >256) and produce the plan with
// the server-qualified destination and its path gates — all before the first
// network byte of any track.
static bool abs_queue_fetch_identity(void *user, const abs_download_op_ctx_t *ctx,
									 const abs_download_job_t *job, abs_download_plan_t *plan,
									 const char **stage_out, char *error, size_t error_size) {
	(void)user;
	*stage_out = "metadata";
	if (!ctx || !job || !plan) {
		abs_queue_fail(stage_out, error, error_size, "metadata",
					   "The queue handed the fetch an incomplete request.");
		return false;
	}
	if (!abs_queue_attempt_ok(ctx, job, error, error_size)) {
		*stage_out = "auth";
		return false;
	}

	// Episodes arrive with the podcast wave (WS1 identity/`.abs-episode`
	// rules): refuse them explicitly instead of downloading with book rules.
	if (strcmp(job->kind, ABS_DL_KIND_BOOK) != 0) {
		abs_queue_fail(stage_out, error, error_size, "metadata",
					   "Podcast episode downloads are not wired yet.");
		return false;
	}

	char id[ABS_ID_MAX * 3];
	http_url_encode(job->item_id, id, sizeof(id));
	char path[384];
	snprintf(path, sizeof(path), "/api/items/%s?expanded=1&include=progress,authors", id);

	// §8.3: wait for the shared catalog mutex — cancellable, so a pause never
	// hangs behind a detail page in flight.
	if (!abs_catalog_lock_wait(ctx->cancel)) {
		abs_queue_fail(stage_out, error, error_size, "network",
					   "The download was paused or cancelled while waiting for its turn.");
		return false;
	}

	bool requested = false;
	bool parse_failed = false;
	int status = 0;
	{
		char *body = NULL;
		// The attempt snapshot's own token, not the mutable globals: the
		// request identity is the job's server (§8.2 step 1). The attempt's
		// wake slot rides the request: a pause/cancel ends the blocked
		// request at once (§8.5 line 1028).
		requested = api_request_with(queue_media.server, queue_media.auth.token, path, NULL, &body,
									 ABS_TIMEOUT_SECS, &status, ctx->op);
		// §8.2 step 2: validate all tracks before writing anything. A
		// 257-track item is an explicit unsupported error, never silence.
		if (requested) {
			memset(&queue_media.book, 0, sizeof(queue_media.book));
			if (!abs_parse_expanded(body ? body : "", true, &queue_media.book)) {
				requested = false;
				parse_failed = true;
			}
		}
		free(body);
	}
	pthread_mutex_unlock(&abs_catalog_lock);
	if (!requested) {
		// §8.4 line 568: 401/403/404 and malformed metadata are permanent
		// FAILED jobs, and the job continues to the next one; network stalls
		// go to WAITING_NETWORK. The two classes need the response status:
		// a refused token must never look like a dead server, and a vanished
		// item must never look like a bad connection.
		if (status == 401 || status == 403) {
			// Line 1180: never a second profile/token for this job; mark the
			// job (and, visibly for now, the profile) auth-failed; every
			// local file and checkpoint is retained and nothing syncs away.
			// The persisted wording avoids credential-shaped words (§8.4's
			// sanitizer redacts them on storage); the stage "auth" is what
			// the queue view keys on later.
			fprintf(stderr, "abs-queue: job %lld: the server rejected the token (HTTP %d); "
							"the profile's login needs editing\n",
					ctx->job_id, status);
			abs_queue_fail(stage_out, error, error_size, "auth",
						   "This server refused this profile's login; the credentials need editing.");
			return false;
		}
		if (status == 404) {
			// Line 1181: permanent missing media; nothing local is deleted
			// and the row is retryable by the user.
			fprintf(stderr, "abs-queue: job %lld: the item is no longer on the server (HTTP 404)\n",
					ctx->job_id);
			abs_queue_fail(stage_out, error, error_size, "metadata",
						   "This item is no longer on the server.");
			return false;
		}
		// The failure is described on this thread by the request layer (a
		// refused token, a dead server) or by the parse (malformed JSON, >256
		// tracks, no tracks). A dead server or a refused identity stalls into
		// WAITING_NETWORK; metadata problems are the explicit terminal
		// refusals of §8.4 — the fetch tells the two apart.
		const char *why = audiobookshelf_last_error();
		const char *message = why && why[0] ? why
							  : parse_failed ? "The expanded identity of this item was invalid."
											 : "The expanded identity could not be fetched.";
		abs_queue_fail(stage_out, error, error_size, parse_failed ? "metadata" : "network", message);
		return false;
	}

	audiobookshelf_book_t *book = &queue_media.book;

	// §8.2 step 3 + §11.5: the server-qualified destination, computed and
	// gated BEFORE any network media write. The hash root and the manifest's
	// server= line come from the same normalized string, so a directory and a
	// manifest can never disagree about which server owns them.
	char author[160], title[220];
	safe_component(book->item.author[0] ? book->item.author : "Unknown Author", author, sizeof(author));
	safe_component(book->item.title, title, sizeof(title));
	char hash[16];
	abs_book_hash_prefix(queue_media.server, hash, sizeof(hash));
	char hash_root_rel[64];
	snprintf(hash_root_rel, sizeof(hash_root_rel), "Audiobooks/Audiobookshelf/abs-%s", hash);
	char dest_rel[ABS_DL_DEST_MAX];
	if ((size_t)snprintf(dest_rel, sizeof(dest_rel), "%s/%s/%s", hash_root_rel, author, title) >=
		sizeof(dest_rel)) {
		abs_queue_fail(stage_out, error, error_size, "metadata", "The audiobook download path is too long.");
		return false;
	}
	char hash_root_abs[SONIX_LINKED_PATH_MAX];
	if (!abs_download_card_path(hash_root_rel, hash_root_abs, sizeof(hash_root_abs))) {
		abs_queue_fail(stage_out, error, error_size, "metadata",
					   "The SD card for downloads is not available.");
		return false;
	}
	struct stat st;
	if (stat(hash_root_abs, &st) == 0 && S_ISDIR(st.st_mode)) {
		// A hash root claimed by another server is never shared (§11.5): the
		// manifest server, not the prefix, is the binding identity.
		if (abs_dir_has_foreign_claim(hash_root_abs, queue_media.server, 2)) {
			abs_queue_fail(stage_out, error, error_size, "metadata",
						   "The download path is already used by another Audiobookshelf server.");
			return false;
		}
	}
	char dest_abs[SONIX_LINKED_PATH_MAX];
	if (!abs_download_card_path(dest_rel, dest_abs, sizeof(dest_abs))) {
		abs_queue_fail(stage_out, error, error_size, "metadata", "The audiobook download path is too long.");
		return false;
	}
	if (!abs_dest_manifest_mine(dest_abs, queue_media.server, book->item.id)) {
		// §8.2 file rules: a destination bound to another identity is a path
		// conflict — a permanent refusal, not a successful skip.
		abs_queue_fail(stage_out, error, error_size, "metadata",
					   "The download path is already bound to another audiobook.");
		return false;
	}

	// The destination is adopted now: the plan points at the adapter's state,
	// which the queue validates and stores as the identical string.
	snprintf(queue_media.destination, sizeof(queue_media.destination), "%s", dest_rel);
	queue_media.job_id = ctx->job_id;

	// The plan: file rows before any card write, each absolute path gated
	// against the narrowest downstream consumer (§11.5 via pathlimits).
	memset(queue_plan_files, 0, sizeof(queue_plan_files));
	memset(queue_plan_names, 0, sizeof(queue_plan_names));
	memset(queue_plan_urls, 0, sizeof(queue_plan_urls));
	for (int i = 0; i < book->track_count; i++) {
		const audiobookshelf_track_t *track = &book->tracks[i];
		char numbered[280];
		char track_abs[SONIX_LINKED_PATH_MAX];
		if (!local_track_name(track, i, numbered, sizeof(numbered)) ||
			strlen(numbered) >= ABS_DL_NAME_MAX ||
			(size_t)snprintf(track_abs, sizeof(track_abs), "%.1279s/%.255s", dest_abs, numbered) >=
				sizeof(track_abs) ||
			!sonix_linked_path_representable(strlen(track_abs))) {
			if (strlen(numbered) >= ABS_DL_NAME_MAX) {
				// DOWNLOAD_FILE.final_name is a 256-byte column: a longer
				// basename is refused whole, never stored truncated (§8.2).
				abs_queue_fail(stage_out, error, error_size, "metadata",
							   "A filename in this item is too long for the download record.");
			} else {
				abs_queue_fail(stage_out, error, error_size, "metadata",
							   "The audiobook download path is too long.");
			}
			return false;
		}
		char encoded[ABS_URL_MAX * 2];
		encode_content_path(track->content_url, encoded, sizeof(encoded));
		char url[ABS_URL_MAX * 2 + 512];
		if (strncmp(track->content_url, "http://", 7) == 0 || strncmp(track->content_url, "https://", 8) == 0) {
			// §11.3 media rule: an absolute contentUrl keeps its Bearer only
			// when scheme, host and effective port match the job identity.
			// Cross-origin media is refused here, before anything is fetched;
			// the redirect path in http.c still strips Authorization on its own.
			if (!media_origin_equal(track->content_url, queue_media.server)) {
				abs_queue_fail(stage_out, error, error_size, "metadata",
							   "The audiobook file is hosted by another server.");
				return false;
			}
			// Schemes need their colon intact. Audiobookshelf normally returns
			// a relative /s/item path; tolerate an absolute one without
			// treating its authority as a path segment.
			const char *slide = strstr(track->content_url, "://");
			slide = slide ? strchr(slide + 3, '/') : NULL;
			if (slide) {
				char origin[512];
				size_t origin_len = (size_t)(slide - track->content_url);
				if (origin_len >= sizeof(origin)) origin_len = sizeof(origin) - 1;
				memcpy(origin, track->content_url, origin_len);
				origin[origin_len] = '\0';
				encode_content_path(slide, encoded, sizeof(encoded));
				snprintf(url, sizeof(url), "%s%s", origin, encoded);
			} else {
				snprintf(url, sizeof(url), "%s", track->content_url);
			}
		} else if (encoded[0] == '/') {
			snprintf(url, sizeof(url), "%s%s", queue_media.server, encoded);
		} else {
			snprintf(url, sizeof(url), "%s/%s", queue_media.server, encoded);
		}
		if (!url[0] || strlen(url) >= ABS_DL_URL_MAX) {
			// DOWNLOAD_FILE rows carry the same-origin link; one that cannot
			// be stored whole is not stored truncated (§8.2 schema contract).
			abs_queue_fail(stage_out, error, error_size, "metadata",
						   "The download link for one of this item's files is too long.");
			return false;
		}
		snprintf(queue_plan_urls[i], sizeof(queue_plan_urls[i]), "%.1023s", url);
		snprintf(queue_plan_names[i], sizeof(queue_plan_names[i]), "%.255s", numbered);
		queue_plan_files[i].source_url = queue_plan_urls[i];
		queue_plan_files[i].final_name = queue_plan_names[i];
		queue_plan_files[i].start_offset = track->start_offset;
		queue_plan_files[i].duration = track->duration;
		// The server's stated file size; Content-Length remains the transfer's
		// own exact check (§8.2 step 4).
		queue_plan_files[i].expected_size = track->size;
	}

	// The strings the plan points at (destination, urls, names) are the
	// adapter's statics: valid until the queue ingests them or the attempt
	// ends, whichever comes first (§8.2 step 4).
	plan->destination = queue_media.destination;
	plan->files = queue_plan_files;
	plan->file_count = book->track_count;
	// Whole-book progress in the unit the resume mapping needs (§8.2 step 5):
	// item.progress is the remote whole-book position in seconds; rows whose
	// item returned no progress at all stay -1 (unknown). finished/updated
	// travel with the same snapshot.
	plan->remote_progress = book->progress_updated_ms > 0 ? book->item.progress : -1;
	plan->remote_finished = book->item.finished ? 1 : 0;
	plan->remote_updated_ms = book->progress_updated_ms;
	return true;
}


// §8.2 step 4: one file. The queue has already marked the row `writing`; this
// streams into <final>.part from byte zero (0.6.0 has no HTTP Range resume),
// verifies the exact 64-bit length, closes checked and renames atomically —
// the queue commits the file record only after that rename (§8.2 step 4).
// Cancellation is polled between read units; progress is published coarsely,
// at most once per coalesce window (§8.1/§8.4).
static bool abs_queue_file_transfer(void *user, const abs_download_op_ctx_t *ctx,
									const abs_download_file_t *file, long long *final_size_out,
									const char **stage_out, char *error, size_t error_size) {
	(void)user;
	*final_size_out = -1;
	*stage_out = "network";
	if (error_size) {
		error[0] = '\0';
	}
	if (!ctx || !file) {
		abs_queue_fail(stage_out, error, error_size, "metadata",
					   "The queue handed the transfer an incomplete request.");
		return false;
	}
	// The attempt's own state: a resolved token, the fetch's destination and
	// the same job the context names. The adapter state is the transfer's
	// only view of the job — the queue validated the identical destination
	// when it ingested the plan.
	if (!queue_media.held || !queue_media.auth.token[0]) {
		abs_queue_fail(stage_out, error, error_size, "auth",
					   "The download's login snapshot was not resolved.");
		return false;
	}
	if (!queue_media.destination[0] || ctx->job_id != queue_media.job_id) {
		abs_queue_fail(stage_out, error, error_size, "metadata",
					   "The download's media state is missing for this file.");
		return false;
	}
	// §11.3 defense in depth: the queue ingested a same-origin URL, and the
	// transfer re-checks it against the attempt's own server before a byte of
	// its Authorization reaches a connection.
	if (!media_origin_equal(file->source_url, queue_media.server)) {
		abs_queue_fail(stage_out, error, error_size, "metadata",
					   "The audiobook file is hosted by another server.");
		return false;
	}

	char rel[ABS_DL_DEST_MAX + ABS_DL_NAME_MAX + 2];
	if ((size_t)snprintf(rel, sizeof(rel), "%.511s/%.255s", queue_media.destination,
						 file->final_name) >= sizeof(rel)) {
		abs_queue_fail(stage_out, error, error_size, "metadata", "The audiobook download path is too long.");
		return false;
	}
	char final_path[SONIX_LINKED_SIDECAR_PATH_MAX];
	if (!abs_download_card_path(rel, final_path, sizeof(final_path))) {
		abs_queue_fail(stage_out, error, error_size, "metadata", "The audiobook download path is too long.");
		return false;
	}
	char part_path[SONIX_LINKED_SIDECAR_PATH_MAX];
	if ((size_t)snprintf(part_path, sizeof(part_path), "%.1300s.part", final_path) >= sizeof(part_path)) {
		abs_queue_fail(stage_out, error, error_size, "metadata", "The audiobook download path is too long.");
		return false;
	}

	struct stat existing;
	if (stat(final_path, &existing) == 0 && S_ISREG(existing.st_mode) && existing.st_size > 0) {
		if (file->expected_size >= 0 && (long long)existing.st_size != file->expected_size) {
			// §8.2 file rules: an existing file of the wrong size is a path
			// conflict for user retry, never "valid content" by accident.
			abs_queue_fail(stage_out, error, error_size, "metadata",
						   "A differently sized file already exists at the destination.");
			return false;
		}
		// A complete final file (renamed before a crash, or a previous
		// attempt) is retained; the queue's commit gate compares it against
		// the row's own record.
		*final_size_out = (long long)existing.st_size;
		return true;
	}

	// The directory arrives with the first file: the destination gate ran at
	// fetch time, so the first write creates no stray residue for a refused
	// job (§8.2 step 3).
	char dir[SONIX_LINKED_PATH_MAX];
	if ((size_t)snprintf(dir, sizeof(dir), "%s", final_path) >= sizeof(dir)) {
		abs_queue_fail(stage_out, error, error_size, "metadata", "The audiobook download path is too long.");
		return false;
	}
	char *slash = strrchr(dir, '/');
	if (!slash || slash == dir) {
		abs_queue_fail(stage_out, error, error_size, "metadata", "The audiobook download path is too long.");
		return false;
	}
	*slash = '\0';
	if (!make_dirs(dir)) {
		int dir_errno = errno;
		abs_queue_fail(stage_out, error, error_size, "write",
					   dir_errno == ENOSPC ? "The SD card is full."
										   : "The audiobook download folder could not be created.");
		return false;
	}

	// The token copy lives in this buffer only for the request build and is
	// wiped immediately after (§8.1). The attempt's wake slot rides the open:
	// a pause/cancel ends a blocked connect/handshake/header read at once
	// (§8.5 line 1028), and the response status drives the §8.4 line 568
	// classification below.
	char headers[ABS_TOKEN_MAX + 32];
	snprintf(headers, sizeof(headers), "Authorization: Bearer %s\r\n", queue_media.auth.token);
	http_stream_t stream;
	int open_status = 0;
	bool opened = http_stream_open_ex_op(&stream, file->source_url, headers, ABS_TIMEOUT_SECS, ctx->op,
										 &open_status);
	memset(headers, 0, sizeof(headers));
	if (!opened) {
		if (open_status == 401 || open_status == 403) {
			// Line 1180: the file-level twin of the fetch mapping — this
			// job's token was refused mid-attempt; permanent auth failure,
			// never a second profile/token, nothing local deleted.
			fprintf(stderr, "abs-queue: job %lld: the server rejected the token for a file (HTTP %d)\n",
					ctx->job_id, open_status);
			abs_queue_fail(stage_out, error, error_size, "auth",
						   "This server refused this profile's login; the credentials need editing.");
			return false;
		}
		if (open_status == 404) {
			fprintf(stderr, "abs-queue: job %lld: a file is no longer on the server (HTTP 404)\n",
					ctx->job_id);
			abs_queue_fail(stage_out, error, error_size, "metadata",
						   "This file is no longer on the server.");
			return false;
		}
		const char *why = http_last_error();
		abs_queue_fail(stage_out, error, error_size, "network",
					   why && why[0] ? why : "The audiobook file could not be downloaded.");
		return false;
	}
	FILE *f = fopen(part_path, "wb");
	if (!f) {
		const char *message = errno == ENOSPC ? "The SD card is full."
											  : "The audiobook file could not be written.";
		http_stream_close(&stream);
		abs_queue_fail(stage_out, error, error_size, "write", message);
		return false;
	}

	// Counters and length checks are 64-bit end to end (§8.4): a >2 GB track
	// must neither wrap a counter nor pass a truncated length comparison.
	// An unknown Content-Length falls back to the row's expected size for the
	// progress total; both unknown stays -1 (equal-weight progress).
	long long total = stream.content_length > 0 ? stream.content_length
						: (file->expected_size >= 0 ? file->expected_size : -1);
	long long done = 0;
	long long published = 0;
	abs_download_op_progress(ctx, 0, total);
	char buffer[ABS_DOWNLOAD_CHUNK];
	bool io_failed = false;
	bool disk_failed = false;
	int disk_errno = 0;
	bool stopped = false;
	for (;;) {
		if (abs_download_op_cancelled(ctx->cancel)) {
			// The stop belongs to the queue: it records PAUSED/CANCELLED and
			// retains (pause) or deletes (cancel) this .part in its own ack
			// step, so the op only stops between units — never deletes.
			stopped = true;
			break;
		}
		// The cancellable read (§8.5 line 1157): the read is wrapped in the
		// wake slot's publish window, so a pause/cancel both wakes a read
		// blocked on the socket AND stops the loop on the next attempt.
		int n = http_stream_read_op(&stream, buffer, sizeof(buffer), ctx->op);
		if (n == 0) {
			// EOF is success only when nothing stopped it: a wake surfaces
			// as this same EOF, and the length check below would call it a
			// short transfer. The queue's ack step owns the outcome either
			// way; the classification here just stays honest.
			if (abs_download_op_cancelled(ctx->cancel) || http_op_woken_current(ctx->op)) {
				stopped = true;
			}
			break;
		}
		if (n < 0) {
			// A wake (or a flag that rose before this read) is the queue's
			// stop, not a broken transfer — the ack step owns the outcome,
			// but the counters and the honest stage still come from here.
			if (abs_download_op_cancelled(ctx->cancel) || http_op_woken_current(ctx->op)) {
				stopped = true;
			} else {
				io_failed = true;
			}
			break;
		}
		if (fwrite(buffer, 1, (size_t)n, f) != (size_t)n) {
			disk_errno = errno;
			disk_failed = true;
			break;
		}
		done += (long long)n;
		if (done - published >= ABS_QUEUE_PROGRESS_COALESCE_BYTES) {
			abs_download_op_progress(ctx, done, total);
			published = done;
		}
	}
	// Publish the tail exactly once more: the stop and every failure still
	// leaves its (coalesced) byte count in the mirror for the ack step.
	abs_download_op_progress(ctx, done, total);
	bool closed = fclose(f) == 0;
	http_stream_close(&stream);
	if (!closed) {
		disk_failed = true;
	}

	if (stopped) {
		// The queue races this against its ack step; the state it records
		// comes from the stop request, not this stage.
		abs_queue_fail(stage_out, error, error_size, "network", "The download was paused or cancelled.");
		return false;
	}
	if (disk_failed) {
		remove(part_path);
		abs_queue_fail(stage_out, error, error_size, "write",
					   disk_errno == ENOSPC ? "The SD card is full."
											: "The audiobook file could not be written.");
		return false;
	}
	if (io_failed || done == 0 ||
		(stream.content_length > 0 && done != stream.content_length) ||
		(file->expected_size >= 0 && done != file->expected_size)) {
		// §8.2: nonzero bytes and the exact promised length, when supplied;
		// a mismatch is a broken transfer, retried as a network failure.
		remove(part_path);
		abs_queue_fail(stage_out, error, error_size, "network",
					   "The audiobook download was interrupted or the server sent the wrong length.");
		return false;
	}
	if (rename(part_path, final_path) != 0) {
		const char *message = errno == ENOSPC ? "The SD card is full."
											  : "The audiobook file could not be moved into place.";
		remove(part_path);
		abs_queue_fail(stage_out, error, error_size, "write", message);
		return false;
	}
	*final_size_out = done;
	return true;
}

// A recorded-complete file must still be on the card: a final file whose
// on-disk size no longer matches its commit record is a §8.2 file-rule
// anomaly, quarantined for user retry — never "valid content" by accident.
// Returns the first offending ordinal (0-based), -1 when every file checks.
static int abs_queue_file_on_disk(const abs_download_job_t *job, const abs_download_file_t *files,
								  int file_count, char *bad_name, size_t bad_size) {
	for (int i = 0; i < file_count; i++) {
		// Basename sanity for a row a foreign writer may have touched.
		const char *name = files[i].final_name;
		if (!name[0] || strlen(name) >= ABS_DL_NAME_MAX || strchr(name, '/') || strchr(name, '\\')) {
			snprintf(bad_name, bad_size, "%.255s", name ? name : "");
			return i;
		}
		if (strcmp(files[i].state, "complete") != 0) {
			snprintf(bad_name, bad_size, "%.255s", files[i].final_name);
			return i;
		}
		char path[SONIX_LINKED_SIDECAR_PATH_MAX];
		char rel[ABS_DL_DEST_MAX + ABS_DL_NAME_MAX + 2];
		if ((size_t)snprintf(rel, sizeof(rel), "%.511s/%.255s", job->destination, files[i].final_name) >=
				sizeof(rel) ||
			!abs_download_card_path(rel, path, sizeof(path))) {
			snprintf(bad_name, bad_size, "%.255s", files[i].final_name);
			return i;
		}
		struct stat st;
		if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
			snprintf(bad_name, bad_size, "%.255s", files[i].final_name);
			return i;
		}
		if (files[i].final_size >= 0 && (long long)st.st_size != files[i].final_size) {
			snprintf(bad_name, bad_size, "%.255s", files[i].final_name);
			return i;
		}
	}
	return -1;
}

// Reads the destination's .audiobookshelf manifest and validates it against
// THIS job's identity and file records (§8.2 recovery table line 540: a
// manifest proves file finalization, so recovery trusts it only when it
// really is ours and really describes these rows). Same format family the
// finalize writes and local_progress() reads: "version=1", "item_id=",
// "server=" and tab-separated "track=<no>\t<offset>\t<duration>\t<name>".
// Offsets/durations are written with %.3f, so the comparison carries the
// same 0.001 tolerance resume_target uses.
static bool abs_queue_manifest_matches(const char *dest_abs, const abs_download_job_t *job,
									   const abs_download_file_t *files, int file_count) {
	char path[SONIX_LINKED_SIDECAR_PATH_MAX];
	if ((size_t)snprintf(path, sizeof(path), "%.1300s/.audiobookshelf", dest_abs) >= sizeof(path)) {
		return false;
	}
	struct stat st;
	if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
		return false;
	}
	char normalized_job[ABS_SERVER_MAX];
	abs_profiles_normalize_server(job->server, normalized_job, sizeof(normalized_job));
	if (!normalized_job[0]) {
		return false;
	}
	bool version_ok = false;
	bool server_ok_line = false;
	bool item_ok_line = false;
	int tracks = 0;
	double want_offset = 0, want_duration = 0;
	const char *want_name = NULL;
	char line[ABS_SERVER_MAX + 32];
	FILE *f = fopen(path, "r");
	if (!f) {
		return false;
	}
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = '\0';
		if (strncmp(line, "version=", 8) == 0) {
			version_ok = strcmp(line + 8, "1") == 0;
		} else if (strncmp(line, "server=", 7) == 0) {
			char normalized_bound[ABS_SERVER_MAX];
			abs_profiles_normalize_server(line + 7, normalized_bound, sizeof(normalized_bound));
			server_ok_line = normalized_bound[0] != '\0' &&
							 strcmp(normalized_bound, normalized_job) == 0;
		} else if (strncmp(line, "item_id=", 8) == 0) {
			item_ok_line = line[8] != '\0' && strcmp(line + 8, job->item_id) == 0;
		} else if (strncmp(line, "track=", 6) == 0) {
			if (tracks >= file_count) {
				tracks++; // too many lines: the count check below fails either way
				continue;
			}
			char *second = strchr(line + 6, '\t');
			char *third = second ? strchr(second + 1, '\t') : NULL;
			char *fourth = third ? strchr(third + 1, '\t') : NULL;
			if (!second || !third || !fourth) {
				fclose(f);
				return false;
			}
			int ordinal = atoi(line + 6);
			want_offset = strtod(second + 1, NULL);
			want_duration = strtod(third + 1, NULL);
			want_name = files[tracks].final_name;
			bool ok = ordinal == tracks + 1 && !strchr(fourth + 1, '\t') &&
					  strcmp(fourth + 1, want_name) == 0 && isfinite(want_offset) &&
					  want_offset >= 0 && fabs(want_offset - files[tracks].start_offset) <= 0.001 &&
					  isfinite(want_duration) && want_duration >= 0 &&
					  (files[tracks].duration < 0 || fabs(want_duration - files[tracks].duration) <= 0.001);
			if (!ok) {
				fclose(f);
				return false;
			}
			tracks++;
		}
	}
	fclose(f);
	// §11.5: a manifest with no resolvable server is a foreign claim, never
	// ours. The track-line count must cover every file record exactly.
	return version_ok && server_ok_line && item_ok_line && tracks == file_count;
}

// The crash-recovery resume mapping (§8.2 line 540: rebuild offsets/durations
// + resume_file/resume_seconds from the valid manifest and the row's remote
// whole-book progress). Same algorithm as audiobookshelf_resume_target() over
// the row model the manifest was validated against: finished books map to
// part one at 0 s, an unknown position falls back to the book start, the
// selected part is the last one whose whole-book offset the position covers,
// and the stored seconds are part-relative. audiobookshelf_resume_target()
// itself stays the §8.2 step 5 mapper for the normal finalize (it needs the
// expanded book, which a fresh process legitimately no longer holds).
static void abs_queue_resume_from_rows(const abs_download_job_t *job, const abs_download_file_t *files,
									   int file_count, abs_download_resume_t *resume_out) {
	double whole = job->remote_finished ? 0.0 : job->remote_progress;
	if (!(whole > 0)) {
		whole = 0; // -1 unknown and 0 both start the book
	}
	if (file_count > 0 && files[file_count - 1].duration > 0) {
		double total = files[file_count - 1].start_offset + files[file_count - 1].duration;
		if (total > 0 && whole > total) {
			whole = total;
		}
	}
	int selected = 0;
	for (int i = 1; i < file_count; i++) {
		if (files[i].start_offset <= whole + 0.001) {
			selected = i;
		} else {
			break;
		}
	}
	double relative = whole - files[selected].start_offset;
	if (relative < 0) {
		relative = 0;
	}
	if (files[selected].duration > 0 && relative > files[selected].duration) {
		relative = files[selected].duration;
	}
	snprintf(resume_out->resume_file, sizeof(resume_out->resume_file), "%.255s",
			 files[selected].final_name);
	resume_out->resume_seconds = relative;
}

// §8.2 steps 5–7 for an attempt that holds the expanded book (the normal
// finalize window): sidecars, cover, the resume mapping and — LAST — the
// manifest, so a manifest's existence proves the sidecar phase completed.
// Every helper here is the legacy audiobookshelf_download() writer reused
// verbatim (§8.5): write_sidecars(), write_description(), local_track_name(),
// download_file() for the cover, audiobookshelf_resume_target() for step 5.
static bool abs_queue_finalize_full(const abs_download_op_ctx_t *ctx, const abs_download_job_t *job,
									const abs_download_file_t *files, int file_count,
									abs_download_resume_t *resume_out, const char **stage_out,
									char *error, size_t error_size) {
	*stage_out = "manifest";
	if (!abs_queue_attempt_ok(ctx, job, error, error_size)) {
		*stage_out = "auth";
		return false;
	}
	if (strcmp(job->kind, ABS_DL_KIND_BOOK) != 0) {
		abs_queue_fail(stage_out, error, error_size, "metadata",
					   "Podcast episode downloads are not wired yet.");
		return false;
	}
	audiobookshelf_book_t *book = &queue_media.book;
	if (!queue_media.destination[0] || strcmp(queue_media.destination, job->destination) != 0 ||
		book->track_count != file_count) {
		// The fetch computed and gated the destination from this very book;
		// anything else is a caller/adapter disagreement, never a guess.
		abs_queue_fail(stage_out, error, error_size, "metadata",
					   "The download destination does not match this item's identity.");
		return false;
	}
	char dest_abs[SONIX_LINKED_PATH_MAX];
	if (!abs_download_card_path(queue_media.destination, dest_abs, sizeof(dest_abs))) {
		abs_queue_fail(stage_out, error, error_size, "write", "The audiobook download path is too long.");
		return false;
	}
	// The directory arrives with the first file normally; after a quiesce or
	// a card cycle it is remade here so finalize alone still works (§8.1).
	if (!make_dirs(dest_abs)) {
		int dir_errno = errno;
		abs_queue_fail(stage_out, error, error_size, "write",
					   dir_errno == ENOSPC ? "The SD card is full."
										   : "The audiobook download folder could not be created.");
		return false;
	}

	// Audio first, then its sidecars, in track order (§8.2 step 5: audio and
	// required sidecars must succeed; they are the job's fatal floor).
	for (int i = 0; i < file_count; i++) {
		char numbered[280];
		if (!local_track_name(&book->tracks[i], i, numbered, sizeof(numbered)) ||
			strcmp(numbered, files[i].final_name) != 0) {
			abs_queue_fail(stage_out, error, error_size, "metadata",
						   "The audiobook changed on the server; remove and download it again.");
			return false;
		}
		char audio[SONIX_LINKED_SIDECAR_PATH_MAX];
		if ((size_t)snprintf(audio, sizeof(audio), "%.1300s/%.255s", dest_abs, files[i].final_name) >=
			sizeof(audio)) {
			abs_queue_fail(stage_out, error, error_size, "write", "The audiobook download path is too long.");
			return false;
		}
		struct stat st;
		if (stat(audio, &st) != 0 || !S_ISREG(st.st_mode) ||
			(files[i].final_size >= 0 && (long long)st.st_size != files[i].final_size)) {
			abs_queue_fail(stage_out, error, error_size, "write",
						   "A downloaded file is missing or the wrong size on the card; remove and download it again.");
			return false;
		}
		if (!write_sidecars(audio, book, &book->tracks[i], i + 1)) {
			abs_queue_fail(stage_out, error, error_size, "sidecar",
						   "The audiobook metadata could not be written.");
			return false;
		}
		if (i == 0 && !write_description(audio, book->description)) {
			abs_queue_fail(stage_out, error, error_size, "sidecar",
						   "The audiobook description could not be written.");
			return false;
		}
	}

	// Embedded artwork remains the first choice (legacy comment retained):
	// the server cover is a fallback for books whose files carry none, asked
	// for as JPEG. A missing cover must never turn a successful audio
	// download into a failed book download — cover failure is the ONLY
	// nonfatal sidecar (§8.2 step 5).
	if (book->item.cover[0]) {
		char encoded[ABS_URL_MAX * 2];
		encode_content_path(book->item.cover, encoded, sizeof(encoded));
		char cover_url[ABS_URL_MAX * 2 + 544];
		if (encoded[0] == '/') {
			snprintf(cover_url, sizeof(cover_url), "%s%s?format=jpeg", queue_media.server, encoded);
		} else {
			snprintf(cover_url, sizeof(cover_url), "%s/%s?format=jpeg", queue_media.server, encoded);
		}
		char cover_path[ABS_LOCAL_PATH_MAX];
		if ((size_t)snprintf(cover_path, sizeof(cover_path), "%s/cover.jpg", dest_abs) < sizeof(cover_path)) {
			(void)download_file(cover_url, queue_media.auth.token, cover_path, 1, 1, NULL, NULL);
		}
	}

	// §8.2 step 5: the resume mapping from the remote whole-book progress,
	// persisted by the queue (still inside the barrier) before completion.
	{
		char mapped[SONIX_LINKED_PATH_MAX + 64];
		double seconds = 0;
		bool finished = false;
		if (!audiobookshelf_resume_target(book, queue_media.destination, mapped, sizeof(mapped),
										  &seconds, &finished)) {
			abs_queue_fail(stage_out, error, error_size, "metadata", "The audiobook download path is too long.");
			return false;
		}
		const char *base = strrchr(mapped, '/');
		// The mapping is a local_track_name() basename (<= 240 bytes); the
		// precision is the same bounded-copy idiom the file records use, and
		// a longer string can only come from a corrupt book model.
		snprintf(resume_out->resume_file, sizeof(resume_out->resume_file), "%.255s",
				 base ? base + 1 : mapped);
		resume_out->resume_seconds = seconds;
	}

	// §8.2 step 7: the manifest goes LAST, through a temporary file, a CHECKED
	// close and a CHECKED rename. Its presence is what recovery trusts, and
	// the lines are byte-identical to the legacy download's (§8.5).
	char manifest[SONIX_LINKED_SIDECAR_PATH_MAX];
	char temporary[ABS_TEMP_PATH_MAX];
	if ((size_t)snprintf(manifest, sizeof(manifest), "%.1300s/.audiobookshelf", dest_abs) >=
			sizeof(manifest) ||
		(size_t)snprintf(temporary, sizeof(temporary), "%.1310s.tmp", manifest) >= sizeof(temporary)) {
		abs_queue_fail(stage_out, error, error_size, "manifest", "The audiobook download path is too long.");
		return false;
	}
	FILE *f = fopen(temporary, "w");
	if (!f) {
		abs_queue_fail(stage_out, error, error_size, "manifest",
					   errno == ENOSPC ? "The SD card is full."
									   : "The audiobook manifest could not be written.");
		return false;
	}
	fprintf(f, "version=1\nitem_id=%s\nserver=%s\nduration=%.3f\nprogress=%.3f\nprogress_updated=%lld\n",
			book->item.id, queue_media.server, book->item.duration, book->item.progress,
			book->progress_updated_ms);
	for (int i = 0; i < book->track_count; i++) {
		char local_name[280];
		if (!local_track_name(&book->tracks[i], i, local_name, sizeof(local_name))) {
			continue;
		}
		fprintf(f, "track=%d\t%.3f\t%.3f\t%s\n", i + 1, book->tracks[i].start_offset,
				book->tracks[i].duration, local_name);
	}
	if (fclose(f) != 0) {
		remove(temporary);
		abs_queue_fail(stage_out, error, error_size, "manifest",
					   errno == ENOSPC ? "The SD card is full."
									   : "The audiobook manifest could not be written.");
		return false;
	}
	if (rename(temporary, manifest) != 0) {
		remove(temporary);
		abs_queue_fail(stage_out, error, error_size, "manifest",
					   errno == ENOSPC ? "The SD card is full."
									   : "The audiobook manifest could not be moved into place.");
		return false;
	}
	set_error("");
	return true;
}

// §8.2 recovery-table FINALIZING line: the acknowledgement for a job whose
// manifest (may) already be committed runs WITHOUT rewriting the card. The
// fresh process holds no expanded book, so first the card state decides:
//
//   recorded files missing/wrong size  -> fatal write (§8.2 file rules: only
//                                         matching final files are valid)
//   manifest absent/invalid/foreign    -> files are intact, so ONLY
//                                         finalization is retried: the
//                                         identity is re-fetched (auth
//                                         re-resolved here; the queue sees
//                                         one atomic finalize op) and the
//                                         full finalize runs over the held
//                                         book — committed audio is reused
//                                         without one media byte
//   manifest valid and ours            -> rebuild the resume mapping from
//                                         the row's remote progress and
//                                         succeed; the engine persists the
//                                         mapping and runs the acknowledgement
//                                         reset + DONE transaction (§8.2
//                                         steps 5, 8)
static bool abs_queue_finalize_reconcile(const abs_download_op_ctx_t *ctx, const abs_download_job_t *job,
										 const abs_download_file_t *files, int file_count,
										 abs_download_resume_t *resume_out, const char **stage_out,
										 char *error, size_t error_size) {
	*stage_out = "manifest";
	if (strcmp(job->kind, ABS_DL_KIND_BOOK) != 0) {
		abs_queue_fail(stage_out, error, error_size, "metadata",
					   "Podcast episode downloads are not wired yet.");
		return false;
	}
	if (!job->destination[0]) {
		abs_queue_fail(stage_out, error, error_size, "manifest",
					   "The interrupted download has no destination on the card.");
		return false;
	}
	if (file_count <= 0) {
		abs_queue_fail(stage_out, error, error_size, "manifest", "The download has no file records.");
		return false;
	}
	// The offending basename is deliberately not echoed into the row's
	// bounded English error (§8.4): it is server-derived data, and the
	// recovery table wants an actionable, path-free message.
	char bad_name[ABS_DL_NAME_MAX];
	if (abs_queue_file_on_disk(job, files, file_count, bad_name, sizeof(bad_name)) >= 0) {
		abs_queue_fail(stage_out, error, error_size, "write",
					   "A downloaded file is missing or the wrong size on the card; remove and download it again.");
		return false;
	}
	char dest_abs[SONIX_LINKED_PATH_MAX];
	if (!abs_download_card_path(job->destination, dest_abs, sizeof(dest_abs))) {
		abs_queue_fail(stage_out, error, error_size, "manifest", "The audiobook download path is too long.");
		return false;
	}
	if (abs_queue_manifest_matches(dest_abs, job, files, file_count)) {
		// The manifest is valid and describes exactly these file records: the
		// sidecar phase finished before the manifest (it writes last), so the
		// card is final — complete the acknowledgement from the row snapshot.
		abs_queue_resume_from_rows(job, files, file_count, resume_out);
		return true;
	}

	// Manifest incomplete/invalid/foreign: §8.2 line 540 — retry ONLY
	// finalization. Re-resolve this job's auth (a fresh process holds no
	// snapshot), re-fetch the expanded identity under the shared catalog
	// mutex (cancellable, like the normal fetch) and run the full finalize.
	// plan_ingest is not involved: the file records stay exactly as committed.
	{
		abs_auth_snapshot_t snapshot;
		memset(&snapshot, 0, sizeof(snapshot));
		if (!audiobookshelf_queue_auth_resolve(job->profile_id, job->server, &snapshot, error,
											   error_size)) {
			*stage_out = "auth";
			return false;
		}
		char normalized[ABS_SERVER_MAX];
		abs_profiles_normalize_server(job->server, normalized, sizeof(normalized));
		if (!normalized[0]) {
			*stage_out = "auth";
			snprintf(error, error_size, "%s", "The download's server does not match its login snapshot.");
			return false;
		}
		memset(&queue_media, 0, sizeof(queue_media)); // a fresh attempt's state
		snprintf(queue_media.server, sizeof(queue_media.server), "%s", normalized);
		queue_media.auth = snapshot;
		queue_media.held = true;
	}
	abs_download_plan_t plan;
	memset(&plan, 0, sizeof(plan));
	if (!abs_queue_fetch_identity(NULL, ctx, job, &plan, stage_out, error, error_size)) {
		// The fetch reports its own honest stage (network stalls into
		// WAITING_NETWORK, permanent refusals fail the job §8.4).
		return false;
	}
	// The identity must still name the destination and tracks this job
	// committed — anything else is a changed item, a user Remove + re-download.
	if (!plan.destination || strcmp(plan.destination, job->destination) != 0) {
		abs_queue_fail(stage_out, error, error_size, "metadata",
					   "The audiobook changed on the server; remove and download it again.");
		return false;
	}
	return abs_queue_finalize_full(ctx, job, files, file_count, resume_out, stage_out, error, error_size);
}

// §8.2 steps 5–7 dispatcher. A held attempt (the normal window after
// auth+fetch+files) runs the full finalize; a finalizing job adopted after a
// crash has no attempt state at all and reconciles from the card instead.
static bool abs_queue_finalize(void *user, const abs_download_op_ctx_t *ctx,
							   const abs_download_job_t *job, const abs_download_file_t *files,
							   int file_count, abs_download_resume_t *resume_out, const char **stage_out,
							   char *error, size_t error_size) {
	(void)user;
	*stage_out = "manifest";
	if (error_size) {
		error[0] = '\0';
	}
	if (resume_out) {
		memset(resume_out, 0, sizeof(*resume_out));
		resume_out->resume_seconds = -1;
	}
	if (!ctx || !job || !files || file_count <= 0 || !resume_out) {
		abs_queue_fail(stage_out, error, error_size, "manifest",
					   "The queue handed the finalize an incomplete request.");
		return false;
	}
	char normalized[ABS_SERVER_MAX];
	abs_profiles_normalize_server(job ? job->server : "", normalized, sizeof(normalized));
	bool held_attempt = queue_media.held && queue_media.auth.token[0] && normalized[0] &&
						strcmp(normalized, queue_media.server) == 0 &&
						(queue_media.job_id == 0 || queue_media.job_id == ctx->job_id);
	if (held_attempt) {
		// The full finalize is terminal for the attempt: a failure leaves the
		// recovery untouched (the §8.2 manifest rules already applied).
		return abs_queue_finalize_full(ctx, job, files, file_count, resume_out, stage_out, error,
									   error_size);
	}
	// No held attempt: a FINALIZING row adopted straight from the recovered
	// database (§8.2 recovery table line 540).
	return abs_queue_finalize_reconcile(ctx, job, files, file_count, resume_out, stage_out, error,
										error_size);
}

// §8.2 step 8: the acknowledgement reset. Books reset durable ABS sync state
// (audiobookdb_abs_reset) for the card-relative destination the job names,
// resolved through the ONE card path builder the queue owns (§8.5), the same
// absolute folder path the scanner keys AUDIOBOOK_ABS_SYNC rows by — exactly
// like the legacy download's commit reset. The delete is idempotent, which is
// what makes repeating it after a crash-before-flag-commit safe (§8.2 line
// 540). Episodes are the podcast wave's: the slot stays a documented refusal
// (and episode jobs never reach finalize in 0.6.0 anyway).
static bool abs_queue_ack_reset(void *user, const abs_download_job_t *job, char *error,
								size_t error_size) {
	(void)user;
	if (!job) {
		snprintf(error, error_size, "%s", "The queue handed the reset an incomplete request.");
		return false;
	}
	error[0] = '\0';
	if (strcmp(job->kind, ABS_DL_KIND_BOOK) != 0) {
		snprintf(error, error_size, "%s", "The episode acknowledgement reset is wired with the podcast wave.");
		return false;
	}
	char dest_abs[SONIX_LINKED_PATH_MAX];
	if (!abs_download_card_path(job->destination, dest_abs, sizeof(dest_abs))) {
		snprintf(error, error_size, "%s", "The SD card for downloads is not available.");
		return false;
	}
	audiobookdb_abs_reset(dest_abs);
	return true;
}

// Populated at call time, not as a file-scope initializer: a const table of
// function pointers would root the whole adapter in every binary that links
// this module (gc-sections cannot prune relocations out of kept data), and
// builds that never wire the queue — the browse tests, the legacy GUI path —
// must stay free of it.
static abs_download_ops_t queue_media_ops;

const abs_download_ops_t *audiobookshelf_queue_media_ops(void) {
	queue_media_ops.user = NULL;
	queue_media_ops.fetch_identity = abs_queue_fetch_identity;
	queue_media_ops.file_download = abs_queue_file_transfer;
	queue_media_ops.finalize = abs_queue_finalize;
	queue_media_ops.ack_reset = abs_queue_ack_reset;
	queue_media_ops.auth_acquire = abs_queue_auth_acquire;
	queue_media_ops.auth_release = abs_queue_auth_release;
	return &queue_media_ops;
}
