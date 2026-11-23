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

// One URL join + Bearer + buffered request (§11.3). `expected_server`, when
// given, is the manifest's bound server: the immutable snapshot comparison of
// the old global-race check. Credentials always come from
// request_credentials_copy(), which re-reads the owning profile under the
// store lock immediately before the request.
static bool api_request(const char *path, const http_req_t *request, char **body, int *status_out,
						const char *expected_server) {
	char url[ABS_URL_MAX];
	char server[ABS_SERVER_MAX], token[ABS_TOKEN_MAX];
	if (!request_credentials_copy(expected_server, server, sizeof(server), token, sizeof(token)) ||
		(expected_server && strcmp(server, expected_server) != 0)) {
		memset(token, 0, sizeof(token));
		set_error("Audiobookshelf settings changed; checkpoint retained.");
		return false;
	}
	if (!path || (size_t)snprintf(url, sizeof(url), "%s%s", server, path) >= sizeof(url)) {
		memset(token, 0, sizeof(token));
		// Bounded APIs (§10.3) refuse over-long queries before asking the
		// network; here the joined URL itself is over the ABS_URL_MAX ceiling.
		set_error("The request is too long for this server address.");
		return false;
	}
	char headers[ABS_TOKEN_MAX + 32];
	snprintf(headers, sizeof(headers), "Authorization: Bearer %s\r\n", token);
	memset(token, 0, sizeof(token));
	http_req_t req = request ? *request : (http_req_t){0};
	req.extra_headers = headers;
	req.want_error_body = true;

	int status = 0;
	bool received = http_request(url, &req, body, NULL, ABS_BODY_LIMIT,
								 expected_server ? 10 : ABS_TIMEOUT_SECS, &status);
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

int audiobookshelf_groups_ex(const char *library_id, abs_browse_filter_t group,
							 audiobookshelf_library_t **out) {
	const char *key = abs_group_key(group);
	if (out) *out = NULL;
	if (!library_id || !key || !out) return 0;
	char id[ABS_ID_MAX * 3], path[384], *body = NULL;
	http_url_encode(library_id, id, sizeof(id));
	snprintf(path, sizeof(path), "/api/libraries/%s/filterdata", id);
	if (!api_request(path, NULL, &body, NULL, NULL)) { free(body); return -1; }
	json_doc_t doc;
	if (!json_parse(body, &doc)) { free(body); set_error("Invalid library filters."); return -1; }
	int array = json_get(&doc, json_root(&doc), key);
	int count = json_len(&doc, array), written = 0;
	audiobookshelf_library_t *groups = count > 0 ? calloc(count, sizeof(*groups)) : NULL;
	if (count > 0 && !groups) { json_free(&doc); free(body); set_error("Not enough memory for library filters."); return -1; }
	for (int i = 0; i < count; i++) {
		int entry = json_at(&doc, array, i);
		audiobookshelf_library_t *slot = &groups[written];
		if (!abs_group_name_only(group)) {
			json_obj_str(&doc, entry, "id", slot->id, sizeof(slot->id));
			json_obj_str(&doc, entry, "name", slot->name, sizeof(slot->name));
			// An author/series choice needs id and name.
			if (slot->id[0] && slot->name[0]) written++;
			continue;
		}
		// §10.1: a genre/tag choice's value is the string itself. The checked-
		// in fixtures model the entries as {id,name} objects, reviewed servers
		// as bare strings -- the name is what the filter encodes either way.
		char text[ABS_NAME_MAX];
		if (json_str(&doc, entry, text, sizeof(text)) && text[0])
			snprintf(slot->name, sizeof(slot->name), "%s", text);
		else
			json_obj_str(&doc, entry, "name", slot->name, sizeof(slot->name));
		if (slot->name[0]) written++;
	}
	if (written > 1) qsort(groups, written, sizeof(*groups), group_compare);
	json_free(&doc); free(body); *out = groups;
	return written;
}

int audiobookshelf_groups(const char *library_id, bool series, audiobookshelf_library_t **out) {
	return audiobookshelf_groups_ex(library_id, series ? ABS_FILTER_SERIES : ABS_FILTER_AUTHOR, out);
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

bool audiobookshelf_book(const char *item_id, audiobookshelf_book_t *out) {
	if (!item_id || !item_id[0] || !out) return false;
	memset(out, 0, sizeof(*out));
	char id[ABS_ID_MAX * 3];
	http_url_encode(item_id, id, sizeof(id));
	char path[384];
	snprintf(path, sizeof(path), "/api/items/%s?expanded=1&include=progress,authors", id);
	char *body = NULL;
	if (!api_request(path, NULL, &body, NULL, NULL)) {
		free(body);
		return false;
	}
	json_doc_t doc;
	if (!json_parse(body, &doc)) {
		free(body);
		set_error("Audiobookshelf returned invalid book details.");
		return false;
	}
	int root = json_root(&doc);
	read_item(&doc, root, &out->item);
	int media = json_get(&doc, root, "media");
	int metadata = json_get(&doc, media, "metadata");
	json_obj_str(&doc, metadata, "description", out->description, sizeof(out->description));
	first_series(&doc, metadata, out->item.series, sizeof(out->item.series), out->series_part, sizeof(out->series_part));
	int progress = json_get(&doc, root, "userMediaProgress");
	out->progress_updated_ms = json_obj_llong(&doc, progress, "lastUpdate", 0);

	int tracks = json_get(&doc, media, "tracks");
	int count = json_len(&doc, tracks);
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
		json_obj_str(&doc, json_get(&doc, object, "metadata"), "filename", track->filename, sizeof(track->filename));
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
	free(body);
	if (count == 0) {
		set_error("This Audiobookshelf item has no downloadable audio tracks.");
		return false;
	}
	return true;
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

// True when the local manifest names this server item AND bound this same
// server (§11.5): matching item_id alone let server B's copy of one book claim
// server A's local link. A manifest without `server=` (0.5.0 vintage) resolves
// to the persisted legacy_server; before any profile store exists it binds to
// the sole configured identity, exactly as 0.5.0 did, and never to a guess.
bool audiobookshelf_local_linked(const char *local_book, const char *item_id) {
	if (!local_book || !local_book[0] || !item_id || !item_id[0]) return false;

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

	// All three answers are collected: `server=` is written after `item_id=`
	// in the manifest, so neither line may stop the scan early, and `version=`
	// is the manifest's own format commitment (§11.5: validate version and
	// required fields before trusting the binding).
	bool id_matched = false;
	bool version_ok = false;
	char bound[ABS_SERVER_MAX] = "";
	char line[ABS_SERVER_MAX + 32];
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = '\0';
		if (strncmp(line, "item_id=", 8) == 0) {
			id_matched = line[8] != '\0' && strcmp(line + 8, item_id) == 0;
		} else if (strncmp(line, "server=", 7) == 0 && !bound[0]) {
			snprintf(bound, sizeof(bound), "%.*s", (int)sizeof(bound) - 1, line + 7);
		} else if (strcmp(line, "version=1") == 0) {
			version_ok = true;
		}
	}
	fclose(f);
	// A manifest without a recognized version or a matching item id is not a
	// linkable binding: it must never open a copy that some other state wrote.
	if (!version_ok || !id_matched) return false;

	char resolved[ABS_SERVER_MAX];
	abs_profiles_normalize_server(bound, resolved, sizeof(resolved));
	if (!resolved[0]) legacy_manifest_binding(resolved, sizeof(resolved));
	if (!resolved[0]) return false; // §11.5: no reliable server -> explicit re-link, keep progress pending
	char identity[ABS_SERVER_MAX], normalized_identity[ABS_SERVER_MAX];
	if (!request_credentials_copy(NULL, identity, sizeof(identity), NULL, 0) || !identity[0]) return false;
	abs_profiles_normalize_server(identity, normalized_identity, sizeof(normalized_identity));
	return strcmp(resolved, normalized_identity) == 0;
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
