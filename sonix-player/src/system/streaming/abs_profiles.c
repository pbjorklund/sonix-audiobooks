// URL + token profiles for Audiobookshelf (spec §11.1, ADR-5).
//
// One durable file holds every profile plus the active id and the legacy 0.5.0
// server, so a power cut can never leave half a credential set behind: the
// writer builds a temporary file beside the real one (mode 0600 -- tokens are
// in it), flushes and fsyncs it, renames over the original, and syncs the
// directory where the filesystem supports that. The parser is strict on purpose:
// duplicate or unknown fields, an oversized line or file, a broken id sequence
// all reject the whole file rather than guess, because a guessed credential is
// worse than none.
//
// Migration from 0.5.0 happens once, inside abs_profiles_init(): the legacy
// [audiobookshelf] server/token pair becomes profile p1 with its normalized URL
// recorded as legacy_server (the binding target of old manifests without
// `server=`). The profile file is saved before the legacy keys are cleared, so
// a failed second step loses nothing.

#include "abs_profiles.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "src/system/core/config.h"

typedef struct {
	abs_profile_t profiles[ABS_PROFILE_MAX];
	int count;
	char active_id[ABS_PROFILE_ID_MAX]; // "" = none
	char legacy_server[ABS_SERVER_MAX]; // "" = unknowable
	long next_num;                      // numeric part of the next "p<N>" id
} profiles_store_t;

static profiles_store_t store;
static pthread_mutex_t store_lock = PTHREAD_MUTEX_INITIALIZER;

// ---------------------------------------------------------------------------
// Storage location
// ---------------------------------------------------------------------------

// Explicit injection for host and tests. On the device the fixed userdata path.
// A host without an explicit path keeps everything in memory: /usr/data must
// never be touched on a development machine (§11.1).
static const char *store_path(void) {
	const char *env = getenv("SONIX_AUDIOBOOKSHELF_PROFILES");
	if (env && env[0]) return env;
#ifndef HOST_BUILD
	return ABS_PROFILES_FILE;
#endif
	return NULL;
}

// ---------------------------------------------------------------------------
// Normalization and validation (§11.2)
// ---------------------------------------------------------------------------

void abs_profiles_normalize_server(const char *server, char *out, size_t size) {
	if (!out || size == 0) return;
	out[0] = '\0';
	if (!server) return;
	const char *p = server;
	while (isspace((unsigned char)*p)) p++;
	snprintf(out, size, "%s", p);
	size_t len = strlen(out);
	while (len && isspace((unsigned char)out[len - 1])) out[--len] = '\0';
	while (len > 0 && out[len - 1] == '/') out[--len] = '\0';
}

static void set_reason(char *reason, size_t reason_size, const char *text) {
	if (reason && reason_size) snprintf(reason, reason_size, "%s", text);
}

bool abs_profiles_server_valid(const char *server, char *normalized_out, size_t normalized_size,
							   char *reason_out, size_t reason_size) {
	if (normalized_out && normalized_size) normalized_out[0] = '\0';
	if (!server || !*server) {
		set_reason(reason_out, reason_size, "empty server");
		return false;
	}
	for (const unsigned char *p = (const unsigned char *)server; *p; p++) {
		if (*p < 32u || *p == 127u) {
			set_reason(reason_out, reason_size, "control character in URL");
			return false;
		}
	}
	// The identity is the normalized URL, so every rule below applies to it.
	char normalized[ABS_SERVER_MAX];
	if (strlen(server) >= ABS_SERVER_MAX) { // too long even before trimming
		set_reason(reason_out, reason_size, "URL too long");
		return false;
	}
	abs_profiles_normalize_server(server, normalized, sizeof(normalized));

	const char *rest = NULL;
	if (strncmp(normalized, "http://", 7) == 0) rest = normalized + 7;
	else if (strncmp(normalized, "https://", 8) == 0) rest = normalized + 8;
	else {
		set_reason(reason_out, reason_size, "must start with http:// or https://");
		return false;
	}
	if (!*rest || *rest == '/' || *rest == '?' || *rest == '#') {
		set_reason(reason_out, reason_size, "missing host");
		return false;
	}
	for (const char *p = normalized; *p; p++) {
		if (*p == '@') {
			set_reason(reason_out, reason_size, "userinfo not allowed");
			return false;
		}
		if (*p == '#') {
			set_reason(reason_out, reason_size, "fragment not allowed");
			return false;
		}
	}
	if (normalized_out && normalized_size) snprintf(normalized_out, normalized_size, "%s", normalized);
	return true;
}

// ---------------------------------------------------------------------------
// File format: strict parser and atomic writer (§11.1)
//
//     # comment / blank lines are ignored
//     version=1
//     next_id=<int>
//     active=<profile-id or empty>
//     legacy_server=<normalized URL or empty>
//
//     [profile.<id>]
//     name=<display name, may be empty>
//     server=<normalized URL>
//     token=<token, may be empty while the profile is incomplete>
// ---------------------------------------------------------------------------

typedef struct {
	char line[ABS_PROFILES_LINE_MAX_BYTES];
	int line_no;
} parse_error_t;

static void copy_field(const char *value, char *out, size_t out_size) {
	// No silent truncation: callers check the fit before calling.
	snprintf(out, out_size, "%s", value);
}

static bool fits(const char *value, size_t capacity_with_nul) { return strlen(value) < capacity_with_nul; }

// "p" + 1..15 digits. The id is locally opaque; the shape keeps next_id
// arithmetic total and the file greppable over ADB.
static bool profile_id_shape(const char *id, long *num_out) {
	if (id[0] != 'p' || !isdigit((unsigned char)id[1])) return false;
	long num = 0;
	int digits = 0;
	for (const char *p = id + 1; *p; p++) {
		if (!isdigit((unsigned char)*p)) return false;
		num = num * 10 + (*p - '0');
		digits++;
	}
	if (digits < 1 || digits > 15) return false;
	if (num_out) *num_out = num;
	return true;
}

static bool parse_uint(const char *value, long *out) {
	if (!*value) return false;
	long n = 0;
	for (const char *p = value; *p; p++) {
		if (!isdigit((unsigned char)*p)) return false;
		n = n * 10 + (*p - '0');
		if (n > 999999999L) return false; // far beyond any id this store can hold
	}
	*out = n;
	return true;
}

// Reads one whole file into `buf` (NUL-terminated). False when the file is
// missing, unreadable, or at/over the size ceiling -- an oversized profile
// file is corrupt and must not be parsed partially.
static bool slurp_file(const char *path, char *buf, size_t buf_size) {
	FILE *f = fopen(path, "rb");
	if (!f) return false;
	size_t total = 0;
	bool too_big = false;
	for (;;) {
		if (total + 1 >= buf_size) {
			// The buffer cannot take another byte: probe whether the file has one.
			too_big = fgetc(f) != EOF;
			break;
		}
		size_t n = fread(buf + total, 1, buf_size - total - 1, f);
		total += n;
		if (n == 0) break;
	}
	buf[total] = '\0';
	bool err = ferror(f);
	fclose(f);
	return !too_big && !err;
}

static bool parse_store_file(const char *text, profiles_store_t *out, parse_error_t *err) {
	memset(out, 0, sizeof(*out));
	long version = -1, next_num = -1;
	bool have_version, have_next;
	char active_id[ABS_PROFILE_ID_MAX] = "";
	char legacy_server[ABS_SERVER_MAX] = "";
	bool have_active, have_legacy;
	int section = -1; // index into out->profiles of the open [profile.<id>]
	int seen_name = 0, seen_server = 0, seen_token = 0; // per open section: duplicates are corruption

	have_version = have_next = have_active = have_legacy = false;

	char line[ABS_PROFILES_LINE_MAX_BYTES];
	const char *p = text;
	int line_no = 0;
	while (*p) {
		line_no++;
		const char *line_end = strchr(p, '\n');
		size_t len = line_end ? (size_t)(line_end - p) : strlen(p);
		if (len >= sizeof(line)) {
			snprintf(err->line, sizeof(err->line), "line %d exceeds %d bytes", line_no, ABS_PROFILES_LINE_MAX_BYTES);
			err->line_no = line_no;
			return false;
		}
		memcpy(line, p, len);
		line[len] = '\0';
		p = line_end ? line_end + 1 : p + len;

		char *s = line;
		while (isspace((unsigned char)*s)) s++;
		if (!*s || *s == '#') continue; // blank or comment

		char *eq = strchr(s, '=');
		char *section_name = NULL;
		if (*s == '[') {
			char *close = strchr(s, ']');
			if (!close) {
				snprintf(err->line, sizeof(err->line), "line %d: unterminated section", line_no);
				err->line_no = line_no;
				return false;
			}
			*close = '\0';
			section_name = s + 1;
			while (isspace((unsigned char)*section_name)) section_name++;
			char *tail = close + 1;
			while (*tail && isspace((unsigned char)*tail)) tail++;
			if (*tail) { // junk after the closing bracket
				snprintf(err->line, sizeof(err->line), "line %d: text after section name", line_no);
				err->line_no = line_no;
				return false;
			}
		} else if (!eq) {
			snprintf(err->line, sizeof(err->line), "line %d: not a key=value or [section] line", line_no);
			err->line_no = line_no;
			return false;
		}

		if (section_name) {
			if (strncmp(section_name, "profile.", 8) != 0) {
				snprintf(err->line, sizeof(err->line), "line %d: unknown section", line_no);
				err->line_no = line_no;
				return false;
			}
			char *id = section_name + 8;
			while (isspace((unsigned char)*id)) id++;
			long num;
			if (!profile_id_shape(id, &num)) {
				snprintf(err->line, sizeof(err->line), "line %d: bad profile id", line_no);
				err->line_no = line_no;
				return false;
			}
			for (int i = 0; i < out->count; i++) {
				if (strcmp(out->profiles[i].id, id) == 0) {
					snprintf(err->line, sizeof(err->line), "line %d: duplicate profile %s", line_no, id);
					err->line_no = line_no;
					return false;
				}
			}
			if (out->count >= ABS_PROFILE_MAX) {
				snprintf(err->line, sizeof(err->line), "line %d: more than %d profiles", line_no, ABS_PROFILE_MAX);
				err->line_no = line_no;
				return false;
			}
			abs_profile_t *prof = &out->profiles[out->count++];
			memset(prof, 0, sizeof(*prof));
			snprintf(prof->id, sizeof(prof->id), "%s", id);
			section = out->count - 1;
			seen_name = seen_server = seen_token = 0;
			continue;
		}

		// key=value
		*eq = '\0';
		char *key = s;
		while (isspace((unsigned char)*key)) key++;
		char *value = eq + 1;
		while (isspace((unsigned char)*value)) value++;
		size_t klen = strlen(key);
		for (size_t i = klen; i-- > 0;) {
			if (isspace((unsigned char)key[i])) key[i] = '\0';
			else break;
		}

		if (section < 0) {
			if (strcmp(key, "version") == 0) {
				if (have_version) goto duplicate;
				if (!parse_uint(value, &version) || version != 1) {
					snprintf(err->line, sizeof(err->line), "line %d: unsupported version", line_no);
					err->line_no = line_no;
					return false;
				}
				have_version = true;
			} else if (strcmp(key, "next_id") == 0) {
				if (have_next) goto duplicate;
				if (!parse_uint(value, &next_num) || next_num < 1) {
					snprintf(err->line, sizeof(err->line), "line %d: bad next_id", line_no);
					err->line_no = line_no;
					return false;
				}
				have_next = true;
			} else if (strcmp(key, "active") == 0) {
				if (have_active) goto duplicate;
				if (!fits(value, sizeof(active_id))) {
					snprintf(err->line, sizeof(err->line), "line %d: active id too long", line_no);
					err->line_no = line_no;
					return false;
				}
				copy_field(value, active_id, sizeof(active_id));
				have_active = true;
			} else if (strcmp(key, "legacy_server") == 0) {
				if (have_legacy) goto duplicate;
				if (!fits(value, sizeof(legacy_server))) {
					snprintf(err->line, sizeof(err->line), "line %d: legacy_server too long", line_no);
					err->line_no = line_no;
					return false;
				}
				copy_field(value, legacy_server, sizeof(legacy_server));
				have_legacy = true;
			} else {
				snprintf(err->line, sizeof(err->line), "line %d: unknown field '%.*s'", line_no,
						 (int)(sizeof(err->line) - 64), key);
				err->line_no = line_no;
				return false;
			}
		} else {
			abs_profile_t *prof = &out->profiles[section];
			if (strcmp(key, "name") == 0) {
				if (seen_name++) goto duplicate;
				if (!fits(value, sizeof(prof->name))) goto too_long;
				copy_field(value, prof->name, sizeof(prof->name));
			} else if (strcmp(key, "server") == 0) {
				if (seen_server++) goto duplicate;
				if (!fits(value, sizeof(prof->server))) goto too_long;
				copy_field(value, prof->server, sizeof(prof->server));
			} else if (strcmp(key, "token") == 0) {
				if (seen_token++) goto duplicate;
				if (!fits(value, sizeof(prof->token))) goto too_long;
				copy_field(value, prof->token, sizeof(prof->token));
			} else {
				snprintf(err->line, sizeof(err->line), "line %d: unknown field '%.*s'", line_no,
						 (int)(sizeof(err->line) - 64), key);
				err->line_no = line_no;
				return false;
			}
		}
		continue;

	duplicate:
		snprintf(err->line, sizeof(err->line), "line %d: duplicate field '%.*s'", line_no,
				 (int)(sizeof(err->line) - 64), key);
		err->line_no = line_no;
		return false;
	too_long:
		snprintf(err->line, sizeof(err->line), "line %d: value too long for '%.*s'", line_no,
				 (int)(sizeof(err->line) - 64), key);
		err->line_no = line_no;
		return false;
	}

	if (!have_version || !have_next) {
		snprintf(err->line, sizeof(err->line), "missing version or next_id");
		return false;
	}
	for (int i = 0; i < out->count; i++) {
		long num;
		if (!profile_id_shape(out->profiles[i].id, &num) || num >= next_num) {
			snprintf(err->line, sizeof(err->line), "next_id not greater than profile %s", out->profiles[i].id);
			return false;
		}
	}
	if (active_id[0]) {
		bool found = false;
		for (int i = 0; i < out->count; i++) {
			if (strcmp(out->profiles[i].id, active_id) == 0) found = true;
		}
		if (!found) {
			snprintf(err->line, sizeof(err->line), "active profile %s does not exist", active_id);
			return false;
		}
	}
	// A normalized server URL may appear in only one profile.
	for (int i = 0; i < out->count; i++) {
		if (!out->profiles[i].server[0]) continue;
		for (int j = i + 1; j < out->count; j++) {
			if (strcmp(out->profiles[i].server, out->profiles[j].server) == 0) {
				snprintf(err->line, sizeof(err->line), "duplicate server URL in %s and %s",
						 out->profiles[i].id, out->profiles[j].id);
				return false;
			}
		}
	}

	out->next_num = next_num;
	if (have_active) snprintf(out->active_id, sizeof(out->active_id), "%s", active_id);
	snprintf(out->legacy_server, sizeof(out->legacy_server), "%s", legacy_server);
	return true;
}

// Builds the file text into `buf`. Returns false when it would not fit -- a
// truncated credential file is worse than no save at all.
static bool render_store_file(const profiles_store_t *in, char *buf, size_t buf_size) {
	size_t off = 0;
#define APPEND(...)                                                                                                    \
	do {                                                                                                               \
		int n_appended = snprintf(buf + off, buf_size - off, __VA_ARGS__);                                              \
		if (n_appended < 0 || (size_t)n_appended >= buf_size - off) return false;                                       \
		off += (size_t)n_appended;                                                                                      \
	} while (0)
	APPEND("# sonix audiobookshelf profiles v1. Rewritten by the player.\n");
	APPEND("version=1\n");
	APPEND("next_id=%ld\n", in->next_num);
	APPEND("active=%s\n", in->active_id);
	APPEND("legacy_server=%s\n", in->legacy_server);
	for (int i = 0; i < in->count; i++) {
		const abs_profile_t *p = &in->profiles[i];
		APPEND("\n[profile.%s]\nname=%s\nserver=%s\ntoken=%s\n", p->id, p->name, p->server, p->token);
	}
#undef APPEND
	return true;
}

// Atomic replace: temp file beside the target at mode 0600 (it carries tokens),
// checked flush + fsync + close, rename over the original, directory sync where
// supported. On any failure the old file is left untouched and the temp removed.
static bool atomic_write_file(const char *path, const char *text) {
	char dir[560];
	snprintf(dir, sizeof(dir), "%s", path);
	char *slash = strrchr(dir, '/');
	if (slash && slash != dir) *slash = '\0';
	else if (slash == dir) dir[1] = '\0'; // "/" -> ""
	else snprintf(dir, sizeof(dir), ".");

	char temp[640];
	snprintf(temp, sizeof(temp), "%s.tmp", path);

	int fd = open(temp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0) return false;
	bool ok = true;
	size_t len = strlen(text);
	size_t written = 0;
	while (written < len) {
		ssize_t n = write(fd, text + written, len - written);
		if (n <= 0) {
			ok = false;
			break;
		}
		written += (size_t)n;
	}
	if (ok && fsync(fd) != 0) ok = false;
	if (close(fd) != 0) ok = false;

	if (!ok || rename(temp, path) != 0) {
		unlink(temp);
		return false;
	}

	// Persist the directory entry itself where the filesystem allows it.
	int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (dfd >= 0) {
		fsync(dfd); // failure is not fatal: some filesystems refuse to sync a dir
		close(dfd);
	}
	return true;
}

// ---------------------------------------------------------------------------
// Store operations
// ---------------------------------------------------------------------------

static abs_profile_t *find_profile(const char *id) {
	if (!id || !*id) return NULL;
	for (int i = 0; i < store.count; i++) {
		if (strcmp(store.profiles[i].id, id) == 0) return &store.profiles[i];
	}
	return NULL;
}

static bool server_in_use(const char *server, const char *except_id) {
	for (int i = 0; i < store.count; i++) {
		if (except_id && strcmp(store.profiles[i].id, except_id) == 0) continue;
		if (store.profiles[i].server[0] && strcmp(store.profiles[i].server, server) == 0) return true;
	}
	return false;
}

// Persists the whole store. Only called with the lock held. Returns false when
// there is no path to persist to (host without injection: in-memory by design)
// or the write failed -- callers must not report success then (§11.1).
static bool save_store_locked(void) {
	const char *path = store_path();
	if (!path) return true; // ephemeral host store: nothing to persist, not an error
	char buf[ABS_PROFILES_FILE_MAX_BYTES];
	if (!render_store_file(&store, buf, sizeof(buf))) return false;
	return atomic_write_file(path, buf);
}

int abs_profiles_init(void) {
	pthread_mutex_lock(&store_lock);
	memset(&store, 0, sizeof(store));
	store.next_num = 1; // a fresh store hands out p1 first; file/migration override this

	const char *path = store_path();
	bool migrated = false;

	if (path) {
		char text[ABS_PROFILES_FILE_MAX_BYTES];
		struct stat st;
		bool file_present = stat(path, &st) == 0;
		if (slurp_file(path, text, sizeof(text))) {
			parse_error_t err = {0};
			if (!parse_store_file(text, &store, &err)) {
				// Keep the file on disk so the cause is diagnosable; behave as
				// unconfigured rather than running on a half-read credential set.
				memset(&store, 0, sizeof(store));
				store.next_num = 1;
				fprintf(stderr, "abs_profiles: %s unreadable (%s); continuing unconfigured\n", path, err.line);
				pthread_mutex_unlock(&store_lock);
				return -1;
			}
			pthread_mutex_unlock(&store_lock);
			return store.count;
		}
		if (file_present) {
			// Present but unreadable: migrating over it would hide the failure.
			fprintf(stderr, "abs_profiles: %s exists but cannot be read\n", path);
			pthread_mutex_unlock(&store_lock);
			return -1;
		}
		// No profile file yet: migrate the 0.5.0 credentials if they exist. The
		// config store must already be loaded (main.c does this first); without
		// it there is nothing to migrate from and no error.
		const char *legacy_server = "";
		const char *legacy_token = "";
		if (config_path()) {
			legacy_server = config_get("audiobookshelf", "server", "");
			legacy_token = config_get("audiobookshelf", "token", "");
		}
		char normalized[ABS_SERVER_MAX];
		if (legacy_server && legacy_server[0] && legacy_token && legacy_token[0]) {
			abs_profiles_normalize_server(legacy_server, normalized, sizeof(normalized));
			if (normalized[0]) {
				abs_profile_t *p = &store.profiles[store.count++];
				memset(p, 0, sizeof(*p));
				snprintf(p->id, sizeof(p->id), "p1");
				snprintf(p->name, sizeof(p->name), "Audiobookshelf");
				snprintf(p->server, sizeof(p->server), "%s", normalized);
				snprintf(p->token, sizeof(p->token), "%s", legacy_token);
				snprintf(store.active_id, sizeof(store.active_id), "p1");
				snprintf(store.legacy_server, sizeof(store.legacy_server), "%s", normalized);
				store.next_num = 2;
				migrated = true;
			}
		}
		if (migrated && !save_store_locked()) {
			// The profile file is authoritative only once saved. Keep p1 for this
			// session and leave the legacy keys in place: nothing is lost, and the
			// next boot retries the migration.
			fprintf(stderr, "abs_profiles: could not save %s; keeping legacy settings\n", path);
		}
		if (migrated) {
			// Best-effort second step: the profile file already won on disk.
			config_set("audiobookshelf", "server", "");
			config_set("audiobookshelf", "token", "");
			if (!config_save()) {
				fprintf(stderr, "abs_profiles: legacy keys left in place; profile file is authoritative\n");
			}
		}
		pthread_mutex_unlock(&store_lock);
		return store.count;
	}

#ifdef HOST_BUILD
	// Host-only ephemeral credentials for tests (§11.1 step 3): a single active
	// profile that never touches disk or the config file.
	const char *host_server = getenv("SONIX_AUDIOBOOKSHELF_SERVER");
	const char *host_token = getenv("SONIX_AUDIOBOOKSHELF_TOKEN");
	if (host_server && host_server[0] && host_token && host_token[0]) {
		char normalized[ABS_SERVER_MAX];
		abs_profiles_normalize_server(host_server, normalized, sizeof(normalized));
		if (normalized[0]) {
			abs_profile_t *p = &store.profiles[store.count++];
			memset(p, 0, sizeof(*p));
			snprintf(p->id, sizeof(p->id), "p1");
			snprintf(p->name, sizeof(p->name), "Audiobookshelf");
			snprintf(p->server, sizeof(p->server), "%s", normalized);
			snprintf(p->token, sizeof(p->token), "%s", host_token);
			snprintf(store.active_id, sizeof(store.active_id), "p1");
			store.next_num = 2;
		}
	}
#endif
	pthread_mutex_unlock(&store_lock);
	return store.count;
}

int abs_profiles_list(abs_profile_t *out, int max) {
	if (!out || max <= 0) return 0;
	pthread_mutex_lock(&store_lock);
	int n = store.count < max ? store.count : max;
	memcpy(out, store.profiles, (size_t)n * sizeof(abs_profile_t));
	pthread_mutex_unlock(&store_lock);
	return n;
}

int abs_profiles_count(void) {
	pthread_mutex_lock(&store_lock);
	int n = store.count;
	pthread_mutex_unlock(&store_lock);
	return n;
}

bool abs_profiles_active(char *id_out, size_t id_size) {
	bool have;
	pthread_mutex_lock(&store_lock);
	if (id_out && id_size) snprintf(id_out, id_size, "%s", store.active_id);
	have = store.active_id[0] != '\0';
	pthread_mutex_unlock(&store_lock);
	return have;
}

bool abs_profiles_select(const char *id) {
	pthread_mutex_lock(&store_lock);
	if (!find_profile(id)) {
		pthread_mutex_unlock(&store_lock);
		return false;
	}
	snprintf(store.active_id, sizeof(store.active_id), "%s", id);
	bool ok = save_store_locked();
	pthread_mutex_unlock(&store_lock);
	return ok;
}

bool abs_profiles_add(const char *name, const char *server, const char *token, char *id_out, size_t id_size) {
	char normalized[ABS_SERVER_MAX];
	char reason[96] = "";
	if (!abs_profiles_server_valid(server, normalized, sizeof(normalized), reason, sizeof(reason))) return false;
	// The fixed store fields cap the name at 63 bytes and the token at 1023.
	// The file parser rejects over-length values instead of truncating, and so
	// does the API (§11.1's "no silent truncation"): a cut token would never
	// authenticate anything, and a cut name would only mislead the picker.
	if (name && !fits(name, ABS_PROFILE_NAME_MAX)) return false;
	if (token && !fits(token, ABS_TOKEN_MAX)) return false;

	pthread_mutex_lock(&store_lock);
	bool ok = false;
	if (store.count >= ABS_PROFILE_MAX) {
		goto done; // four is the R1/UI bound (§11.1)
	}
	if (server_in_use(normalized, NULL)) goto done; // one profile per normalized URL

	char id[ABS_PROFILE_ID_MAX];
	snprintf(id, sizeof(id), "p%ld", store.next_num);
	store.next_num++;

	abs_profile_t *p = &store.profiles[store.count++];
	memset(p, 0, sizeof(*p));
	snprintf(p->id, sizeof(p->id), "%s", id);
	if (name && name[0]) snprintf(p->name, sizeof(p->name), "%s", name);
	snprintf(p->server, sizeof(p->server), "%s", normalized);
	if (token) snprintf(p->token, sizeof(p->token), "%s", token);

	// A newly added profile becomes the active one: it is what the user just set
	// up and wants to browse.
	snprintf(store.active_id, sizeof(store.active_id), "%s", id);
	ok = save_store_locked();
	if (ok && id_out && id_size) snprintf(id_out, id_size, "%s", id);
done:
	pthread_mutex_unlock(&store_lock);
	return ok;
}

bool abs_profiles_edit(const char *id, const char *name, const char *token) {
	// Same no-silent-truncation rule as abs_profiles_add() and the file parser:
	// an over-length value is rejected whole, before anything is modified.
	if (name && !fits(name, ABS_PROFILE_NAME_MAX)) return false;
	if (token && !fits(token, ABS_TOKEN_MAX)) return false;
	pthread_mutex_lock(&store_lock);
	abs_profile_t *p = find_profile(id);
	if (!p) {
		pthread_mutex_unlock(&store_lock);
		return false;
	}
	if (name && name[0]) snprintf(p->name, sizeof(p->name), "%s", name);
	// Blank token retains the stored one: with an immutable URL there is no
	// "reuse the old host's token" ambiguity left to remove (§11.2).
	if (token && token[0]) snprintf(p->token, sizeof(p->token), "%s", token);
	bool ok = save_store_locked();
	pthread_mutex_unlock(&store_lock);
	return ok;
}

bool abs_profiles_remove(const char *id) {
	pthread_mutex_lock(&store_lock);
	abs_profile_t *p = find_profile(id);
	if (!p) {
		pthread_mutex_unlock(&store_lock);
		return false;
	}
	// The download queue does not exist yet in 0.6.0 M1; when it does, a job
	// referencing this profile must block removal here (§11.2).
	int idx = (int)(p - store.profiles);
	for (int i = idx; i + 1 < store.count; i++) {
		store.profiles[i] = store.profiles[i + 1];
	}
	store.count--;
	if (strcmp(store.active_id, id) == 0) store.active_id[0] = '\0';
	bool ok = save_store_locked();
	pthread_mutex_unlock(&store_lock);
	return ok;
}

bool abs_profiles_snapshot(const char *id, abs_auth_snapshot_t *out) {
	if (!out) return false;
	memset(out, 0, sizeof(*out));
	pthread_mutex_lock(&store_lock);
	abs_profile_t *p = find_profile(id);
	bool ok = p != NULL && p->server[0] != '\0';
	if (ok) {
		snprintf(out->profile_id, sizeof(out->profile_id), "%s", p->id);
		snprintf(out->server, sizeof(out->server), "%s", p->server);
		snprintf(out->token, sizeof(out->token), "%s", p->token);
	}
	pthread_mutex_unlock(&store_lock);
	return ok;
}

bool abs_profiles_snapshot_active(abs_auth_snapshot_t *out) {
	if (!out) return false;
	char id[ABS_PROFILE_ID_MAX];
	bool have = abs_profiles_active(id, sizeof(id));
	if (!have) return false;
	return abs_profiles_snapshot(id, out);
}

int abs_profiles_snapshots_all(abs_auth_snapshot_t *out, int max) {
	if (!out || max <= 0) return 0;
	pthread_mutex_lock(&store_lock);
	int n = 0;
	for (int i = 0; i < store.count && n < max; i++) {
		const abs_profile_t *p = &store.profiles[i];
		if (!p->server[0] || !p->token[0]) continue; // only configured profiles take a sync pass
		memset(&out[n], 0, sizeof(out[n]));
		snprintf(out[n].profile_id, sizeof(out[n].profile_id), "%s", p->id);
		snprintf(out[n].server, sizeof(out[n].server), "%s", p->server);
		snprintf(out[n].token, sizeof(out[n].token), "%s", p->token);
		n++;
	}
	pthread_mutex_unlock(&store_lock);
	return n;
}

bool abs_profiles_any_configured(void) {
	pthread_mutex_lock(&store_lock);
	bool any = false;
	for (int i = 0; i < store.count; i++) {
		if (store.profiles[i].server[0] && store.profiles[i].token[0]) {
			any = true;
			break;
		}
	}
	pthread_mutex_unlock(&store_lock);
	return any;
}

bool abs_profiles_has_server(const char *server) {
	char normalized[ABS_SERVER_MAX];
	abs_profiles_normalize_server(server, normalized, sizeof(normalized));
	if (!normalized[0]) return false;
	pthread_mutex_lock(&store_lock);
	bool found = server_in_use(normalized, NULL);
	pthread_mutex_unlock(&store_lock);
	return found;
}

void abs_profiles_legacy_server(char *out, size_t size) {
	if (!out || !size) return;
	pthread_mutex_lock(&store_lock);
	snprintf(out, size, "%s", store.legacy_server);
	pthread_mutex_unlock(&store_lock);
}

void abs_profiles_snapshot_wipe(abs_auth_snapshot_t *snap) {
	if (!snap) return;
	volatile char *bytes = (volatile char *)snap;
	memset((void *)bytes, 0, sizeof(*snap));
}

void abs_profiles_factory_reset(void) {
	pthread_mutex_lock(&store_lock);
	// A cleared store cannot hand out credentials: every snapshot call after
	// this returns false because no profile id exists to look up. The secret
	// bytes die with the store, so nothing needs a separate zeroing pass.
	memset(&store, 0, sizeof(store));
	store.next_num = 1; // a re-added profile starts from p1 again
	const char *path = store_path();
	if (path) {
		char temp[640];
		snprintf(temp, sizeof(temp), "%s.tmp", path);
		if (unlink(temp) != 0 && errno != ENOENT)
			fprintf(stderr, "abs_profiles: factory reset could not remove %s\n", temp);
		if (unlink(path) != 0 && errno != ENOENT)
			fprintf(stderr, "abs_profiles: factory reset could not remove %s\n", path);
	}
	pthread_mutex_unlock(&store_lock);
	// The legacy 0.5.0 keys must not survive either, or migration would put
	// the credentials straight back on the next boot. Same best-effort second
	// step as migration: a failed config save leaves the in-memory clear in
	// place and is reported, not ignored.
	if (config_path()) {
		config_set("audiobookshelf", "server", "");
		config_set("audiobookshelf", "token", "");
		if (!config_save())
			fprintf(stderr, "abs_profiles: factory reset could not clear the legacy keys\n");
	}
}
