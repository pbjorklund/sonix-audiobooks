#include "abs_episode_card.h"

#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "src/system/core/sha256.h"
#include "src/system/library/audiobookdb.h"

static void hash(const char *a, const char *b, char hex[65]) {
	sha256_ctx ctx;
	sha256_init(&ctx);
	sha256_update(&ctx, a, strlen(a) + 1);
	if (b) sha256_update(&ctx, b, strlen(b));
	sha256_final_hex(&ctx, hex);
}

bool abs_episode_destination(const char *server, const char *owner, const char *show,
	const char *episode, char *out, size_t cap) {
	if (!server || !server[0] || !owner || !owner[0] || !show || !show[0] || !episode || !episode[0]) return false;
	char a[65], s[65], e[65];
	hash(server, owner, a);
	hash(show, NULL, s);
	hash(episode, NULL, e);
	int n = snprintf(out, cap, ABS_EPISODE_TREE "/%.24s/%.24s/%.24s", a, s, e);
	return n > 0 && (size_t)n < cap;
}

static bool binding_path(const abs_auth_snapshot_t *auth, char out[SONIX_LINKED_PATH_MAX]) {
	if (!auth || !auth->server[0] || !auth->token[0]) return false;
	char hex[65], relative[128];
	sha256_ctx ctx;
	sha256_init(&ctx);
	sha256_update(&ctx, auth->server, strlen(auth->server) + 1);
	sha256_update(&ctx, auth->profile_id, strlen(auth->profile_id) + 1);
	sha256_update(&ctx, auth->token, strlen(auth->token));
	sha256_final_hex(&ctx, hex);
	snprintf(relative, sizeof(relative), ".local/abs-podcast-owner-%s", hex);
	return abs_download_card_path(relative, out, SONIX_LINKED_PATH_MAX);
}

bool abs_episode_owner_cached(const abs_auth_snapshot_t *auth, char owner[ABS_ID_MAX]) {
	owner[0] = '\0';
	char path[SONIX_LINKED_PATH_MAX];
	if (!binding_path(auth, path)) return false;
	int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
	if (fd < 0) return false;
	struct stat st;
	ssize_t n = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) ? read(fd, owner, ABS_ID_MAX) : -1;
	close(fd);
	if (n <= 0 || n >= ABS_ID_MAX || memchr(owner, '\0', (size_t)n) ||
		memchr(owner, '\n', (size_t)n) || memchr(owner, '\r', (size_t)n)) {
		owner[0] = '\0';
		return false;
	}
	owner[n] = '\0';
	return true;
}

bool abs_episode_owner_remember(const abs_auth_snapshot_t *auth, const char *owner) {
	char path[SONIX_LINKED_PATH_MAX], tmp[SONIX_LINKED_PATH_MAX + 24];
	if (!owner || !owner[0] || strlen(owner) >= ABS_ID_MAX || strchr(owner, '\n') || strchr(owner, '\r') ||
		!binding_path(auth, path)) return false;
	char cached[ABS_ID_MAX];
	if (abs_episode_owner_cached(auth, cached) && !strcmp(cached, owner)) return true;
	snprintf(tmp, sizeof(tmp), "%s.XXXXXX", path);
	int fd = mkstemp(tmp);
	if (fd < 0) return false;
	size_t len = strlen(owner);
	bool ok = write(fd, owner, len) == (ssize_t)len && fsync(fd) == 0;
	if (close(fd) != 0) ok = false;
	if (!ok || rename(tmp, path) != 0) { unlink(tmp); return false; }
	return true;
}

static bool valid_ext(const char *ext) {
	const char *exts[] = {"mp3", "m4a", "aac", "ogg", "opus", "flac", "wav", "m4b", "wma"};
	for (size_t i = 0; i < sizeof(exts) / sizeof(*exts); i++) if (!strcmp(ext, exts[i])) return true;
	return false;
}

const char *abs_episode_extension(const audiobookshelf_track_t *track) {
	const char *dot = strrchr(track->filename, '.');
	const char *exts[] = {"mp3", "m4a", "aac", "ogg", "opus", "flac", "wav", "m4b", "wma"};
	if (dot) for (size_t i = 0; i < sizeof(exts) / sizeof(*exts); i++)
		if (!strcasecmp(dot + 1, exts[i])) return exts[i];
	if (!strcmp(track->mime, "audio/mpeg")) return "mp3";
	if (!strcmp(track->mime, "audio/mp4") || !strcmp(track->mime, "audio/x-m4a")) return "m4a";
	if (!strcmp(track->mime, "audio/aac")) return "aac";
	if (!strcmp(track->mime, "audio/ogg")) return "ogg";
	if (!strcmp(track->mime, "audio/opus")) return "opus";
	if (!strcmp(track->mime, "audio/flac")) return "flac";
	if (!strcmp(track->mime, "audio/wav")) return "wav";
	return NULL;
}

bool abs_episode_filename(const abs_episode_t *episode, char *out, size_t cap) {
	const char *ext = abs_episode_extension(&episode->track);
	if (!ext) return false;
	char stamp[48], version[65];
	snprintf(stamp, sizeof(stamp), "%lld:%lld", episode->file_updated_ms, episode->track.size);
	hash(episode->file_id, stamp, version);
	int n = snprintf(out, cap, "audio-%.16s.%s", version, ext);
	return n > 0 && (size_t)n < cap;
}

bool abs_episode_card_dir(const char *relative, char *absolute, size_t cap) {
	const size_t prefix = sizeof(ABS_EPISODE_TREE) - 1;
	if (!relative || strncmp(relative, ABS_EPISODE_TREE "/", prefix + 1) ||
		strlen(relative) != prefix + 75) return false;
	for (size_t i = 0; i < 75; i++) {
		char c = relative[prefix + i];
		if (i % 25 == 0 ? c != '/' : !strchr("0123456789abcdef", c)) return false;
	}
	if (!abs_download_card_path(relative, absolute, cap)) return false;
	char *p = absolute + strlen(absolute) - strlen(relative);
	// Walk only the managed portion under the configured card mount. Missing
	// directories can be created by the queue; symlinks and non-directories
	// in that tree must never be followed.
	for (;;) {
		p = strchr(p, '/');
		if (p) *p = '\0';
		struct stat st;
		int rc = lstat(absolute, &st);
		bool ok = rc == 0 ? S_ISDIR(st.st_mode) : errno == ENOENT;
		if (p) *p++ = '/';
		if (!ok) return false;
		if (!p) return true;
	}
}

static bool good_text(const char *s, size_t cap, bool required) {
	size_t n = strnlen(s, cap);
	return n < cap && (!required || n > 0) && !memchr(s, '\r', n) && !memchr(s, '\n', n);
}

static bool manifest_valid(const abs_episode_manifest_t *m) {
	return good_text(m->server, sizeof(m->server), true) && good_text(m->owner, sizeof(m->owner), true) &&
		good_text(m->show, sizeof(m->show), true) && good_text(m->episode, sizeof(m->episode), true) &&
		good_text(m->file, sizeof(m->file), true) && good_text(m->audio, sizeof(m->audio), true) &&
		good_text(m->title, sizeof(m->title), false) && good_text(m->show_title, sizeof(m->show_title), false) &&
		m->published_ms >= 0 && !strncmp(m->audio, "audio-", 6) &&
		strspn(m->audio + 6, "0123456789abcdef") == 16 && m->audio[22] == '.' &&
		valid_ext(m->audio + 23) && m->bytes > 0 && isfinite(m->duration) && m->duration >= 0;
}

static bool path_for(const char *folder, const char *name, char path[SONIX_LINKED_PATH_MAX]) {
	int n = snprintf(path, SONIX_LINKED_PATH_MAX, "%s/%s", folder, name);
	return n > 0 && n < SONIX_LINKED_PATH_MAX;
}

static bool audio_valid(const char *folder, const abs_episode_manifest_t *m) {
	char path[SONIX_LINKED_PATH_MAX];
	struct stat st;
	return path_for(folder, m->audio, path) && lstat(path, &st) == 0 && S_ISREG(st.st_mode) &&
		(long long)st.st_size == m->bytes;
}

static bool manifest_read_marker(const char *folder, const char *marker, abs_episode_manifest_t *out, bool require_audio) {
	memset(out, 0, sizeof(*out));
	char path[SONIX_LINKED_PATH_MAX];
	if (!path_for(folder, marker, path)) return false;
	int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
	if (fd < 0) return false;
	struct stat st;
	if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size > 2048) { close(fd); return false; }
	FILE *f = fdopen(fd, "r");
	if (!f) { close(fd); return false; }
	char line[ABS_SERVER_MAX + 96];
	const char *keys[] = {"server=", "owner=", "show=", "episode=", "file=", "audio=", "title=", "show_title="};
	char *fields[] = {out->server, out->owner, out->show, out->episode, out->file, out->audio, out->title, out->show_title};
	size_t sizes[] = {sizeof(out->server), sizeof(out->owner), sizeof(out->show), sizeof(out->episode),
		sizeof(out->file), sizeof(out->audio), sizeof(out->title), sizeof(out->show_title)};
	bool header = fgets(line, sizeof(line), f) != NULL;
	bool v2 = header && !strcmp(line, "version=2\n");
	bool ok = v2 || (header && !strcmp(line, "version=1\n"));
	for (int i = 0; ok && i < (v2 ? 8 : 7); i++) {
		ok = fgets(line, sizeof(line), f) && !strncmp(line, keys[i], strlen(keys[i]));
		if (!ok) break;
		const char *value = line + strlen(keys[i]);
		size_t n = strcspn(value, "\n");
		ok = value[n] == '\n' && n < sizes[i];
		if (ok) { memcpy(fields[i], value, n); fields[i][n] = '\0'; }
	}
	if (ok && v2) ok = fscanf(f, "published=%lld", &out->published_ms) == 1 && fgetc(f) == '\n';
	if (ok) ok = fscanf(f, "bytes=%lld\nduration=%lf", &out->bytes, &out->duration) == 2 &&
		fgetc(f) == '\n' && fgetc(f) == EOF && !ferror(f);
	fclose(f);
	return ok && manifest_valid(out) && (!require_audio || audio_valid(folder, out));
}

bool abs_episode_manifest_read(const char *folder, abs_episode_manifest_t *out) {
	return manifest_read_marker(folder, ".abs-episode", out, true);
}

bool abs_episode_manifest_commit(const char *folder, const abs_episode_manifest_t *m) {
	if (!manifest_valid(m) || !audio_valid(folder, m)) return false;
	char path[SONIX_LINKED_PATH_MAX], temp[SONIX_LINKED_PATH_MAX];
	if (!path_for(folder, ".abs-episode", path) || !path_for(folder, ".abs-episode.part", temp)) return false;
	struct stat st;
	if (lstat(path, &st) == 0) {
		abs_episode_manifest_t old;
		return abs_episode_manifest_read(folder, &old) && !strcmp(old.server, m->server) &&
			!strcmp(old.owner, m->owner) && !strcmp(old.show, m->show) && !strcmp(old.episode, m->episode) &&
			!strcmp(old.file, m->file) && !strcmp(old.audio, m->audio) && old.bytes == m->bytes;
	}
	if (errno != ENOENT) return false;
	if (unlink(temp) != 0 && errno != ENOENT) return false;
	int fd = open(temp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
	if (fd < 0) return false;
	FILE *f = fdopen(fd, "w");
	if (!f) { close(fd); unlink(temp); return false; }
	bool ok = fprintf(f, "version=2\nserver=%s\nowner=%s\nshow=%s\nepisode=%s\nfile=%s\naudio=%s\ntitle=%s\nshow_title=%s\npublished=%lld\nbytes=%lld\nduration=%.3f\n",
		m->server, m->owner, m->show, m->episode, m->file, m->audio, m->title, m->show_title,
		m->published_ms, m->bytes, m->duration) > 0 &&
		fflush(f) == 0 && fsync(fd) == 0;
	if (fclose(f) != 0) ok = false;
	if (!ok || rename(temp, path) != 0) { unlink(temp); return false; }
	int dirfd = open(folder, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
	if (dirfd < 0) return false;
	int synced = fsync(dirfd), err = errno;
	close(dirfd);
	return synced == 0 || err == EINVAL;
}

static bool owner_matches(const abs_episode_manifest_t *m, const char *server, const char *owner,
	const char *show, const char *episode) {
	return !strcmp(m->server, server) && !strcmp(m->owner, owner) && !strcmp(m->show, show) && !strcmp(m->episode, episode);
}

bool abs_episode_download_path(const char *server, const char *owner, const char *show,
	const char *episode, abs_episode_manifest_t *manifest, char *path, size_t cap) {
	char rel[ABS_DL_DEST_MAX], folder[SONIX_LINKED_PATH_MAX];
	abs_episode_manifest_t m;
	if (!abs_episode_destination(server, owner, show, episode, rel, sizeof(rel)) ||
		!abs_episode_card_dir(rel, folder, sizeof(folder)) || abs_download_sync_blocked(rel) ||
		!abs_episode_manifest_read(folder, &m) || !owner_matches(&m, server, owner, show, episode)) return false;
	int n = snprintf(path, cap, "%s/%s", folder, m.audio);
	if (n <= 0 || (size_t)n >= cap) return false;
	if (manifest) *manifest = m;
	return true;
}

bool abs_episode_card_identity(const char *path, abs_episode_manifest_t *out) {
	if (!path || !out || !strstr(path, "/" ABS_EPISODE_TREE "/")) return false;
	size_t len = strnlen(path, SONIX_LINKED_PATH_MAX);
	if (len >= SONIX_LINKED_PATH_MAX) return false;
	char folder[SONIX_LINKED_PATH_MAX], checked[SONIX_LINKED_PATH_MAX];
	memcpy(folder, path, len + 1);
	char *base = strrchr(folder, '/');
	if (!base || base == folder) return false;
	*base++ = '\0';
	if (!abs_episode_manifest_read(folder, out) || strcmp(base, out->audio)) return false;
	// The manifest was already validated against the audio in this folder.
	// Verify the canonical location and download state without reopening it.
	char rel[ABS_DL_DEST_MAX];
	return abs_episode_destination(out->server, out->owner, out->show, out->episode, rel, sizeof(rel)) &&
		abs_episode_card_dir(rel, checked, sizeof(checked)) && !strcmp(checked, folder) &&
		!abs_download_sync_blocked(rel);
}

bool abs_episode_local_cover(const char *audio, char *out, size_t cap) {
	if (!out || !cap) return false;
	out[0] = '\0';
	const char *slash = audio ? strrchr(audio, '/') : NULL;
	if (!slash || (size_t)(slash - audio) >= SONIX_LINKED_PATH_MAX) return false;
	int n = snprintf(out, cap, "%.*s/" ABS_EPISODE_COVER, (int)(slash - audio), audio);
	struct stat st;
	if (n > 0 && (size_t)n < cap && lstat(out, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0) return true;
	out[0] = '\0';
	return false;
}

static void remove_cover(const char *folder) {
	char cover[SONIX_LINKED_PATH_MAX];
	if (path_for(folder, ABS_EPISODE_COVER, cover)) (void)unlink(cover);
	if (path_for(folder, ABS_EPISODE_COVER ".part", cover)) (void)unlink(cover);
}

static void finish_removal(const char *folder, const char *server, const char *owner) {
	abs_episode_manifest_t m;
	if (!manifest_read_marker(folder, ".abs-episode-removing", &m, false) ||
		strcmp(server, m.server) || strcmp(owner, m.owner)) return;
	char expected[ABS_DL_DEST_MAX], checked[SONIX_LINKED_PATH_MAX];
	char marker[SONIX_LINKED_PATH_MAX], audio[SONIX_LINKED_PATH_MAX], current[SONIX_LINKED_PATH_MAX];
	struct stat st;
	if (!abs_episode_destination(server, owner, m.show, m.episode, expected, sizeof(expected)) ||
		!abs_episode_card_dir(expected, checked, sizeof(checked)) || strcmp(checked, folder) ||
		abs_download_sync_blocked(expected) || !path_for(folder, ".abs-episode", current) ||
		!path_for(folder, ".abs-episode-removing", marker) || !path_for(folder, m.audio, audio)) return;
	int rc = lstat(current, &st);
	if (rc == 0) { (void)unlink(marker); return; }
	if (errno != ENOENT) return;
	rc = lstat(audio, &st);
	if (rc == 0 && (!S_ISREG(st.st_mode) || (long long)st.st_size != m.bytes || unlink(audio) != 0)) return;
	if (rc != 0 && errno != ENOENT) return;
	remove_cover(folder);
	(void)unlink(marker);
}

static bool hash_name(const char *name) {
	return strlen(name) == 24 && strspn(name, "0123456789abcdef") == 24;
}

static DIR *open_folder(const char *path) {
	int fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
	if (fd < 0) return NULL;
	DIR *dir = fdopendir(fd);
	if (!dir) close(fd);
	return dir;
}

// Retain only the sorted window the caller asked for. The card directory's
// enumeration order is not stable between scans or after a removal.
static int by_title(const void *a, const void *b) {
	const abs_episode_row_t *x = a, *y = b;
	int n = strcasecmp(x->title, y->title);
	if (!n) n = strcmp(x->show_id, y->show_id);
	return n ? n : strcmp(x->episode_id, y->episode_id);
}

int abs_episode_download_list(const char *server, const char *owner, const char *show, int offset,
	abs_episode_row_t *out, int cap, bool *more) {
	if (!server || !owner || !owner[0] || !out || !more || offset < 0 || cap <= 0 ||
		cap > ABS_PAGE_LIMIT || offset > 4096 - cap) return -1;
	*more = false;
	unsigned generation = abs_download_card_generation();
	if (!generation) return -1;
	char hex[65], rel[ABS_DL_DEST_MAX], root[SONIX_LINKED_PATH_MAX];
	hash(server, owner, hex);
	snprintf(rel, sizeof(rel), ABS_EPISODE_TREE "/%.24s", hex);
	if (!abs_download_card_path(rel, root, sizeof(root))) return -1;
	// Validate all parent components with the same gate as the media path.
	char probe[ABS_DL_DEST_MAX], checked[SONIX_LINKED_PATH_MAX];
	snprintf(probe, sizeof(probe), ABS_EPISODE_TREE "/%.24s/000000000000000000000000/000000000000000000000000", hex);
	if (!abs_episode_card_dir(probe, checked, sizeof(checked))) return -1;
	DIR *shows = open_folder(root);
	if (!shows) return errno == ENOENT ? 0 : -1;
	char wanted[65];
	if (show) hash(show, NULL, wanted);
	int count = 0, total = 0, keep = offset + cap;
	abs_episode_row_t *rows = calloc((size_t)keep, sizeof(*rows));
	if (!rows) { closedir(shows); return -1; }
	struct dirent *ds;
	while ((ds = readdir(shows)) != NULL) {
		if (!hash_name(ds->d_name) || (show && strncmp(ds->d_name, wanted, 24))) continue;
		abs_episode_row_t group = {0};
		char showdir[SONIX_LINKED_PATH_MAX];
		if (!path_for(root, ds->d_name, showdir)) continue;
		DIR *episodes = open_folder(showdir);
		if (!episodes) continue;
		struct dirent *de;
		while ((de = readdir(episodes)) != NULL) {
			if (!hash_name(de->d_name)) continue;
			char folder[SONIX_LINKED_PATH_MAX], expected[ABS_DL_DEST_MAX];
			struct stat st;
			if (!path_for(showdir, de->d_name, folder) || lstat(folder, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
			finish_removal(folder, server, owner);
			abs_episode_manifest_t m;
			if (!abs_episode_manifest_read(folder, &m) || strcmp(m.server, server) || strcmp(m.owner, owner) ||
				!abs_episode_destination(server, owner, m.show, m.episode, expected, sizeof(expected)) ||
				!abs_episode_card_dir(expected, checked, sizeof(checked)) || strcmp(checked, folder) ||
				abs_download_sync_blocked(expected)) continue;
			if (show && strcmp(show, m.show)) continue;
			if (!show) {
				if (!group.show_id[0]) strcpy(group.show_id, m.show);
				if (!group.title[0] && m.show_title[0]) strcpy(group.title, m.show_title);
				continue;
			}
			abs_episode_row_t row = {.duration = m.duration, .published_ms = m.published_ms, .downloaded = true};
			strcpy(row.show_id, m.show);
			strcpy(row.episode_id, m.episode);
			strcpy(row.title, m.title);
			total++;
			int at = count;
			if (at == keep) {
				if (by_title(&row, &rows[at - 1]) >= 0) continue;
				at--;
			} else count++;
			while (at > 0 && by_title(&row, &rows[at - 1]) < 0) { rows[at] = rows[at - 1]; at--; }
			rows[at] = row;
		}
		closedir(episodes);
		if (!show && group.show_id[0]) {
			total++;
			int at = count;
			if (at == keep) {
				if (by_title(&group, &rows[at - 1]) >= 0) continue;
				at--;
			} else count++;
			while (at > 0 && by_title(&group, &rows[at - 1]) < 0) { rows[at] = rows[at - 1]; at--; }
			rows[at] = group;
		}
	}
	closedir(shows);
	int n = count > offset ? count - offset : 0;
	if (generation != abs_download_card_generation()) n = -1;
	if (n > 0) memcpy(out, rows + offset, (size_t)n * sizeof(*out));
	*more = n >= 0 && total > keep;
	free(rows);
	return n;
}

bool abs_episode_download_remove(const char *server, const char *owner, const char *show,
	const char *episode, const char *in_use) {
	char audio[SONIX_LINKED_PATH_MAX], folder[SONIX_LINKED_PATH_MAX], marker[SONIX_LINKED_PATH_MAX];
	if (!abs_episode_download_path(server, owner, show, episode, NULL, audio, sizeof(audio)) ||
		(in_use && !strcmp(in_use, audio))) return false;
	strcpy(folder, audio);
	*strrchr(folder, '/') = '\0';
	char removing[SONIX_LINKED_PATH_MAX];
	if (!path_for(folder, ".abs-episode", marker) || !path_for(folder, ".abs-episode-removing", removing) ||
		rename(marker, removing) != 0) return false;
	// The renamed marker keeps the downloaded row out of the index during
	// removal. Restore visibility if the filesystem refuses the unlink.
	if (unlink(audio) != 0 && errno != ENOENT) { (void)rename(removing, marker); return false; }
	remove_cover(folder);
	(void)unlink(removing);
	(void)rmdir(folder);
	return true;
}

static bool removal_eligible(const abs_episode_removal_t *plan, const char *episode, const char *in_use) {
	abs_episode_manifest_t id;
	char audio[SONIX_LINKED_PATH_MAX];
	if (!abs_episode_download_path(plan->server, plan->owner, plan->show, episode, &id, audio, sizeof(audio)) ||
		(in_use && !strcmp(in_use, audio))) return false;
	if (!plan->finished_only) return true;
	abs_episode_checkpoint_t checkpoint;
	return audiobookdb_episode_get(&id, &checkpoint, NULL) && checkpoint.finished;
}

bool abs_episode_removal_plan(abs_episode_removal_t *plan, const char *in_use) {
	if (!plan || plan->episodes || !plan->show[0]) return false;
	plan->generation = abs_download_card_generation();
	if (!plan->generation) return false;
	plan->count = 0;
	int capacity = 0;
	abs_episode_row_t *rows = calloc(ABS_PAGE_LIMIT, sizeof(*rows));
	if (!rows) return false;
	bool more = false, ok = true;
	for (int offset = 0; ; offset += ABS_PAGE_LIMIT) {
		int n = abs_episode_download_list(plan->server, plan->owner, plan->show, offset, rows, ABS_PAGE_LIMIT, &more);
		if (n < 0) { ok = false; break; }
		for (int i = 0; i < n; i++) {
			if (!removal_eligible(plan, rows[i].episode_id, in_use)) continue;
			if (plan->count == capacity) {
				capacity = capacity ? capacity * 2 : ABS_PAGE_LIMIT;
				void *next = realloc(plan->episodes, (size_t)capacity * sizeof(*plan->episodes));
				if (!next) { ok = false; break; }
				plan->episodes = next;
			}
			strcpy(plan->episodes[plan->count++], rows[i].episode_id);
		}
		if (!ok || !more) break;
	}
	free(rows);
	if (!ok || plan->generation != abs_download_card_generation()) {
		free(plan->episodes);
		plan->episodes = NULL;
		plan->count = 0;
		return false;
	}
	return true;
}

int abs_episode_removal_apply(const abs_episode_removal_t *plan, const char *in_use) {
	if (!plan || !plan->episodes || !plan->generation || plan->generation != abs_download_card_generation()) return -1;
	int removed = 0;
	for (int i = 0; i < plan->count && plan->generation == abs_download_card_generation(); i++) {
		const char *episode = plan->episodes[i];
		if (removal_eligible(plan, episode, in_use) &&
			abs_episode_download_remove(plan->server, plan->owner, plan->show, episode, in_use)) removed++;
	}
	return removed;
}
