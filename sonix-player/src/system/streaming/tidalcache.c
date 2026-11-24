#include "tidalcache.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <pthread.h>

#include "src/system/decode/decode.h"
#include "src/system/decode/growfile.h"
#include "src/system/net/http.h"
#include "src/system/library/mp4flac.h"
#include "src/system/library/lrclib.h"
#include "src/system/streaming/streamturn.h"
#include "src/system/core/utils.h"
#include "src/system/core/lang.h"

#define FETCH_TIMEOUT_SECS 20
#define FETCH_CHUNK 32768

// A DASH segment is a few hundred kilobytes -- four seconds of audio -- and is
// fetched into memory whole before being unwrapped. The ceiling is deliberately
// generous: it only stops a runaway response from taking all the RAM there is.
#define SEGMENT_LIMIT (8 * 1024 * 1024)

// How many segments are fetched before playback starts. Each is worth about
// four seconds, so three put roughly a dozen seconds in hand: the same order of
// magnitude as the Qobuz prebuffer, arrived at by a different route.
#define DASH_HEAD_SEGMENTS 3

#define TIDALCACHE_KEPT_LIST_MAX 200

static char cache_dir[512];
static char music_dir[512];

static __thread char last_error[192];

const char *tidalcache_last_error(void) { return last_error; }

static void set_error(const char *what) { snprintf(last_error, sizeof(last_error), "%s", what ? what : ""); }

// The keep support is defined at the bottom of the file (it needs read_tag and
// prune); the sweep and the scanner only ask these two questions.
static long entry_track_id(const char *name);
static bool is_kept_id(long track_id);
static bool find_saved(long track_id, char *out, size_t size);
static bool tidalcache_unkeep(long track_id);


void tidalcache_abandon_all(void);

static void register_abandon_once(void) {
	static bool done;
	if (!done) {
		done = true;
		streamturn_register_abandon(tidalcache_abandon_all);
	}
}

void tidalcache_set_root(const char *sd_root) {
	register_abandon_once();
	if (!sd_root || !*sd_root) {
		cache_dir[0] = '\0';
		music_dir[0] = '\0';
		return;
	}
	snprintf(cache_dir, sizeof(cache_dir), "%s/%s", sd_root, TIDALCACHE_DIR);
	snprintf(music_dir, sizeof(music_dir), "%s/Music/Tidal", sd_root);
}

bool tidalcache_ready(void) { return cache_dir[0] != '\0'; }

// The extension from the MIME type. The decoder picks by extension: nothing
// will open a FLAC called .bin.
static const char *extension_for(const char *mime) {
	if (!mime || !*mime) {
		return "flac";
	}
	if (strstr(mime, "mpeg") || strstr(mime, "mp3")) {
		return "mp3";
	}
	if (strstr(mime, "flac")) {
		return "flac";
	}
	if (strstr(mime, "mp4") || strstr(mime, "m4a") || strstr(mime, "aac")) {
		return "m4a";
	}
	return "flac";
}

// For a real stream the codec is checked before the container, which is where
// Tidal differs from Qobuz: a DASH manifest declares mimeType "audio/mp4" even
// when FLAC is inside, because MP4 is only the wrapper. After mp4flac the
// wrapper is gone and the file is a .flac for all purposes -- calling it .m4a
// would hand it to the wrong decoder.
const char *tidalcache_extension(const tidal_stream_t *stream) {
	if (!stream) {
		return "flac";
	}
	if (stream->kind == TIDAL_STREAM_DASH) {
		return mp4flac_extension_for(stream->codecs);
	}
	if (stream->codecs[0] && (strncmp(stream->codecs, "flac", 4) == 0 || strncmp(stream->codecs, "fLaC", 4) == 0)) {
		return "flac";
	}
	return extension_for(stream->mime);
}

void tidalcache_path(long track_id, const char *mime, char *out, size_t size) {
	if (!out || size == 0) {
		return;
	}
	if (!tidalcache_ready()) {
		out[0] = '\0';
		return;
	}
	// The folder is clamped to what fits alongside the file name: a path
	// truncated halfway would open, or delete, something else entirely, and
	// do it silently.
	snprintf(out, size, "%.400s/%ld.%s", cache_dir, track_id, extension_for(mime));
}

static void path_with_ext(long track_id, const char *ext, char *out, size_t size) {
	snprintf(out, size, "%.400s/%ld.%s", cache_dir, track_id, ext);
}

// The file may exist and still be an interrupted download: while it grows an
// empty marker sits beside it, removed only once the download is complete.
static void marker_path(const char *path, char *out, size_t size) {
	snprintf(out, size, "%.500s.incompleto", path);
}

static bool have_file(const char *path, char *out, size_t size) {
	char marker[544];
	marker_path(path, marker, sizeof(marker));
	struct stat unused;
	if (stat(marker, &unused) == 0) {
		return false; // interrupted: download it again
	}

	struct stat st;
	if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size == 0) {
		return false;
	}
	if (out && size) {
		snprintf(out, size, "%s", path);
	}
	return true;
}

bool tidalcache_has(long track_id, const char *mime, char *out, size_t size) {
	char path[512];
	tidalcache_path(track_id, mime, path, sizeof(path));
	if (!path[0]) {
		return false;
	}
	return have_file(path, out, size);
}

static bool find_in(const char *dir, long track_id, char *out, size_t size) {
	if (!*dir || track_id <= 0) {
		return false;
	}
	static const char *const exts[] = {"flac", "m4a", "mp3"};
	for (size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); i++) {
		char path[512];
		snprintf(path, sizeof(path), "%.400s/%ld.%s", dir, track_id, exts[i]);
		if (have_file(path, out, size)) {
			return true;
		}
	}
	return false;
}

bool tidalcache_find(long track_id, char *out, size_t size) {
	if (find_saved(track_id, out, size) ||
		find_in(cache_dir, track_id, out, size)) {
		return true;
	}
	if (out && size) {
		out[0] = '\0';
	}
	return false;
}

static bool ensure_path(const char *dir) {
	if (!*dir) {
		return false;
	}

	char path[512];
	snprintf(path, sizeof(path), "%s", dir);
	for (char *p = path + 1; *p; p++) {
		if (*p != '/') {
			continue;
		}
		*p = '\0';
		mkdir(path, 0777);
		*p = '/';
	}
	if (mkdir(path, 0777) != 0 && errno != EEXIST) {
		fprintf(stderr, "tidalcache: cannot create %s: %s\n", path, strerror(errno));
		return false;
	}
	return true;
}

static bool ensure_dir(void) { return ensure_path(cache_dir); }

// ---------------------------------------------------------------------------
// measuring and pruning
// ---------------------------------------------------------------------------

typedef struct {
	char name[64];
	long size;
	long atime; // last access: what tells which tracks are still listened to
} entry_t;

static int scan(entry_t *out, int max, long *total_out) {
	long total = 0;
	int count = 0;

	DIR *d = opendir(cache_dir);
	if (!d) {
		if (total_out) {
			*total_out = 0;
		}
		return 0;
	}

	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		if (e->d_name[0] == '.') {
			continue;
		}
		char path[640];
		if ((size_t)snprintf(path, sizeof(path), "%s/%s", cache_dir, e->d_name) >= sizeof(path)) {
			continue;
		}
		struct stat st;
		if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
			continue;
		}
		// A marker survives even after its audio was moved out of the cache.
		// Protect retained audio here only for downloads not yet migrated.
		long kept_id = entry_track_id(e->d_name);
		if (kept_id > 0 && is_kept_id(kept_id) &&
			(strcmp(strchr(e->d_name, '.'), ".keep") == 0 || strcmp(strchr(e->d_name, '.'), ".link.tags") == 0 ||
			 !find_saved(kept_id, NULL, 0))) {
			continue;
		}
		total += st.st_size;

		// What is downloading right now cannot be deleted, nor can its
		// sidecars: the name is compared without its extension, so
		// "101.flac", "101.flac.tags" and "101.jpg" are all protected while
		// "101.flac" is still growing.
		char stem[640];
		snprintf(stem, sizeof(stem), "%.500s/%.63s", cache_dir, e->d_name);
		char *dot = strrchr(stem + strlen(cache_dir) + 1, '.');
		if (dot) {
			*dot = '\0';
		}
		if (growfile_prefix_is_growing(stem)) {
			continue;
		}
		if (out && count < max && strlen(e->d_name) < sizeof(out[0].name)) {
			snprintf(out[count].name, sizeof(out[count].name), "%.63s", e->d_name);
			out[count].size = (long)st.st_size;
			out[count].atime = (long)st.st_atime;
			count++;
		}
	}
	closedir(d);

	if (total_out) {
		*total_out = total;
	}
	return count;
}

long tidalcache_bytes(void) {
	if (!tidalcache_ready()) {
		return 0;
	}
	long total = 0;
	scan(NULL, 0, &total);
	return total;
}



#define PRUNE_MAX_ENTRIES 512

// Drops the least recently played until the cache fits again. By last access
// and not creation date: an album downloaded a month ago but played again
// yesterday has to stay.
static void prune(long keep_room_for) {
	// Borrowed for the length of the pass and given straight back: this runs
	// only when a download is about to overflow the cache, so forty kilobytes
	// are not worth holding for the life of the player.
	entry_t *entries = malloc(sizeof(*entries) * PRUNE_MAX_ENTRIES);
	if (!entries) {
		return; // nothing is deleted, and the ceiling is checked again next time
	}

	long total = 0;
	int count = scan(entries, PRUNE_MAX_ENTRIES, &total);
	if (total + keep_room_for <= TIDALCACHE_MAX_BYTES) {
		free(entries);
		return;
	}

	for (int i = 0; i < count; i++) {
		int oldest = i;
		for (int k = i + 1; k < count; k++) {
			if (entries[k].atime < entries[oldest].atime) {
				oldest = k;
			}
		}
		entry_t tmp = entries[i];
		entries[i] = entries[oldest];
		entries[oldest] = tmp;
	}

	for (int i = 0; i < count && total + keep_room_for > TIDALCACHE_MAX_BYTES; i++) {
		char path[640];
		snprintf(path, sizeof(path), "%.500s/%.63s", cache_dir, entries[i].name);
		if (remove(path) == 0) {
			total -= entries[i].size;
			printf("tidalcache: removed %s (%ld KB)\n", entries[i].name, entries[i].size / 1024);
		}
	}

	free(entries);
}

static bool is_recent_stem(const char *stem);

// The tracks the queue still has ahead of it, which the automatic clear must
// not touch.
static pthread_mutex_t protected_lock = PTHREAD_MUTEX_INITIALIZER;
static char protected_paths[TIDALCACHE_PROTECTED_MAX][512];
static int protected_count;

void tidalcache_set_protected(const char *const *paths, int count) {
	pthread_mutex_lock(&protected_lock);
	protected_count = 0;
	for (int i = 0; i < count && protected_count < TIDALCACHE_PROTECTED_MAX; i++) {
		if (paths[i] && paths[i][0]) {
			snprintf(protected_paths[protected_count++], sizeof(protected_paths[0]), "%s", paths[i]);
		}
	}
	pthread_mutex_unlock(&protected_lock);
}

static bool is_protected_stem(const char *stem) {
	size_t len = strlen(stem);
	pthread_mutex_lock(&protected_lock);
	bool match = false;
	for (int i = 0; i < protected_count && !match; i++) {
		match = strncmp(protected_paths[i], stem, len) == 0;
	}
	pthread_mutex_unlock(&protected_lock);
	return match;
}

static void clear_files(bool keep_playing) {
	if (!tidalcache_ready()) {
		return;
	}
	DIR *d = opendir(cache_dir);
	if (!d) {
		return;
	}
	int removed = 0;
	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		if (e->d_name[0] == '.') {
			continue;
		}
		char path[640];
		if ((size_t)snprintf(path, sizeof(path), "%s/%s", cache_dir, e->d_name) >= sizeof(path)) {
			continue;
		}
		long kept_id = entry_track_id(e->d_name);
		if (kept_id > 0 && is_kept_id(kept_id) &&
			(strcmp(strchr(e->d_name, '.'), ".keep") == 0 || strcmp(strchr(e->d_name, '.'), ".link.tags") == 0 ||
			 !find_saved(kept_id, NULL, 0))) {
			continue;
		}
		if (keep_playing) {
			char stem[640];
			snprintf(stem, sizeof(stem), "%.500s/%.63s", cache_dir, e->d_name);
			char *dot = strrchr(stem + strlen(cache_dir) + 1, '.');
			if (dot) {
				*dot = '\0';
			}
			if (growfile_prefix_is_growing(stem) || is_protected_stem(stem) || is_recent_stem(stem)) {
				continue;
			}
		}
		if (remove(path) == 0) {
			removed++;
		}
	}
	closedir(d);
	printf("tidalcache: cleared (%d files)\n", removed);
}

void tidalcache_clear(void) { clear_files(false); }

// ---------------------------------------------------------------------------
// the periodic clear
// ---------------------------------------------------------------------------

static int played_since_clear;

#define RECENT_MAX 8
static pthread_mutex_t recent_lock = PTHREAD_MUTEX_INITIALIZER;
static char recent_paths[RECENT_MAX][512];
static int recent_next;

static bool is_recent_stem(const char *stem) {
	size_t len = strlen(stem);
	pthread_mutex_lock(&recent_lock);
	bool match = false;
	for (int i = 0; i < RECENT_MAX && !match; i++) {
		match = recent_paths[i][0] && strncmp(recent_paths[i], stem, len) == 0;
	}
	pthread_mutex_unlock(&recent_lock);
	return match;
}

void tidalcache_note_played(const char *path) {
	if (!tidalcache_ready()) {
		return;
	}

	if (path && path[0]) {
		pthread_mutex_lock(&recent_lock);
		snprintf(recent_paths[recent_next], sizeof(recent_paths[0]), "%s", path);
		recent_next = (recent_next + 1) % RECENT_MAX;
		pthread_mutex_unlock(&recent_lock);
	}

	played_since_clear++;
	if (played_since_clear < TIDALCACHE_CLEAR_EVERY) {
		return;
	}
	played_since_clear = 0;
	printf("tidalcache: %d tracks since the last sweep, cleaning up\n", TIDALCACHE_CLEAR_EVERY);
	clear_files(true);
}

// Cover art lives in a sibling folder of the tracks (.local/tidal-art next to
// .local/tidal-cache) and has the same lifetime: transient data that must not
// survive a power cycle.
static void clear_art_files(void) {
	char art[sizeof(cache_dir) + 16];
	snprintf(art, sizeof(art), "%s", cache_dir);
	char *slash = strrchr(art, '/');
	if (!slash) {
		return;
	}
	snprintf(slash + 1, sizeof(art) - (size_t)(slash + 1 - art), "tidal-art");

	DIR *d = opendir(art);
	if (!d) {
		return;
	}
	int removed = 0;
	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		if (e->d_name[0] == '.') {
			continue;
		}
		char path[sizeof(art) + 300];
		if ((size_t)snprintf(path, sizeof(path), "%s/%s", art, e->d_name) >= sizeof(path)) {
			continue;
		}
		if (remove(path) == 0) {
			removed++;
		}
	}
	closedir(d);
	printf("tidalcache: covers cleared (%d files)\n", removed);
}

void tidalcache_clear_on_exit(void) {
	if (!tidalcache_ready()) {
		return;
	}
	printf("tidalcache: shutting down, clearing the cache\n");
	clear_files(false);
	clear_art_files();
}

// ---------------------------------------------------------------------------
// the sidecar files
// ---------------------------------------------------------------------------

static bool read_tag(const char *path, const char *key, char *out, size_t size) {
	if (!out || size == 0) {
		return false;
	}
	out[0] = '\0';

	char tags[544];
	snprintf(tags, sizeof(tags), "%.500s.tags", path);
	FILE *f = fopen(tags, "r");
	if (!f) {
		return false;
	}

	size_t key_len = strlen(key);
	char line[512];
	bool found = false;
	while (!found && fgets(line, sizeof(line), f)) {
		if (strncmp(line, key, key_len) != 0 || line[key_len] != '=') {
			continue;
		}
		char *value = line + key_len + 1;
		char *end = value + strlen(value);
		while (end > value && (end[-1] == '\n' || end[-1] == '\r')) {
			*--end = '\0';
		}
		snprintf(out, size, "%s", value);
		found = out[0] != '\0';
	}
	fclose(f);
	return found;
}

const char *tidalcache_dir(void) { return cache_dir[0] ? cache_dir : NULL; }

bool tidalcache_owns(const char *path) {
	size_t n = strlen(cache_dir);
	return path && n && strncmp(path, cache_dir, n) == 0 && path[n] == '/';
}

static bool owns_saved(const char *path) {
	size_t n = strlen(music_dir);
	return path && n && strncasecmp(path, music_dir, n) == 0 && path[n] == '/';
}

bool tidalcache_album_id(const char *path, char *out, size_t size) {
	if (!tidalcache_owns(path) && !owns_saved(path)) {
		if (out && size) {
			out[0] = '\0';
		}
		return false;
	}
	return read_tag(path, "album_id", out, size);
}

long tidalcache_track_id(const char *path) {
	if (owns_saved(path)) {
		char value[32], *end;
		if (read_tag(path, "tidal_id", value, sizeof(value))) {
			long id = strtol(value, &end, 10);
			return id > 0 && !*end ? id : 0;
		}
		if (strchr(path + strlen(music_dir) + 1, '/')) {
			return 0;
		}
	} else if (!tidalcache_owns(path)) {
		return 0;
	}
	const char *name = strrchr(path, '/');
	name = name ? name + 1 : path;
	if (*name < '0' || *name > '9') {
		return 0;
	}
	char *end = NULL;
	long id = strtol(name, &end, 10);
	// The digits must be followed by the extension's dot: "12.flac" counts,
	// "12abc.flac" does not.
	if (!end || *end != '.' || id <= 0) {
		return 0;
	}
	return id;
}

static bool write_tags(const char *path, const tidal_track_t *track, int cover_px) {
	char tags[544], tmp[560];
	snprintf(tags, sizeof(tags), "%.500s.tags", path);
	snprintf(tmp, sizeof(tmp), "%s.XXXXXX", tags);
	int fd = mkstemp(tmp);
	if (fd < 0) {
		return false;
	}
	FILE *f = fdopen(fd, "w");
	if (!f) {
		close(fd);
		remove(tmp);
		return false;
	}
	int written = fprintf(f, "title=%.*s\nartist=%.*s\nalbum=%.*s\nalbum_artist=%.*s\n"
		"album_id=%.*s\ntrack=%d\ndisc=%d\nyear=%d\ntidal_id=%ld\ncover_px=%d\n",
		(int)strcspn(track->title, "\r\n"), track->title,
		(int)strcspn(track->artist, "\r\n"), track->artist,
		(int)strcspn(track->album, "\r\n"), track->album,
		(int)strcspn(track->album_artist, "\r\n"), track->album_artist,
		(int)strcspn(track->album_id, "\r\n"), track->album_id,
		track->track_number, track->disc_number, track->release_year, track->id, cover_px);
	bool ok = fclose(f) == 0 && written >= 0 && rename(tmp, tags) == 0;
	if (!ok) {
		remove(tmp);
	}
	return ok;
}

void tidalcache_write_sidecars(const tidal_track_t *track, const char *mime, const char *cover_url) {
	long track_id = track->id;
	if (!tidalcache_ready()) {
		return;
	}

	// The real file, not the one the MIME type would suggest.
	//
	// For a hi-res track the two differ: the DASH manifest declares
	// "audio/mp4" (the wrapper), but after mp4flac the wrapper is gone and the
	// file on the card is named .flac. Trusting the MIME type would put the
	// tags in "<id>.m4a.tags", which nothing reads.
	char path[512];
	if (!tidalcache_find(track_id, path, sizeof(path))) {
		tidalcache_path(track_id, mime, path, sizeof(path));
	}
	if (!path[0]) {
		return;
	}

	char had[16] = "";
	bool same_size = read_tag(path, "cover_px", had, sizeof(had)) && atoi(had) == tidal_cover_px();
	if (!owns_saved(path)) {
		write_tags(path, track, atoi(had));
	}
	if (!cover_url || !*cover_url) {
		return;
	}

	// The name stays "<id>.jpg", and has to.
	//
	// Putting the size in the name -- "<id>-480.jpg" -- would make a size
	// change a new file, but the cover is looked up by albumart.c, which next
	// to the track looks for an image with the track's own name minus the
	// extension: "<id>.flac" -> "<id>.jpg". With the size glued to the name
	// that match fails and no cover is found at all, trading "a bit soft" for
	// "nothing".
	//
	// The size is recorded in the tags file next door instead. If the recorded
	// size is not the one being asked for now, the cover is fetched again, so
	// changing `[tidal] cover_size` also applies to tracks already cached.
	char cover[544];
	const char *dot = strrchr(path, '.');
	snprintf(cover, sizeof(cover), "%.*s.jpg", dot ? (int)(dot - path) : (int)strlen(path), path);

	struct stat st;
	if (same_size && stat(cover, &st) == 0 && st.st_size > 0) {
		return; // already fetched, and at this size
	}

	char *body = NULL;
	size_t len = 0;
	if (!http_get(cover_url, &body, &len, 4 * 1024 * 1024, FETCH_TIMEOUT_SECS) || !body) {
		const char *why = http_last_error();
		fprintf(stderr, "tidalcache: cover '%s' not downloaded: %s\n", cover_url,
				why && why[0] ? why : "no reason given");
		return;
	}

	FILE *img = fopen(cover, "wb");
	if (img) {
		size_t written = fwrite(body, 1, len, img);
		if (fclose(img) == 0 && written == len && !owns_saved(path)) {
			write_tags(path, track, tidal_cover_px());
		}
	}
	free(body);
}

// ---------------------------------------------------------------------------
// downloading
//
// Two shapes, and they really are different.
//
//   FILE   one socket, opened and held open, read in chunks. Same as Qobuz.
//   DASH   one request per segment, each complete, the content handed to
//          mp4flac, which unwraps it into the file.
//
// What does not change is what the rest of the player sees: a growing file,
// announced to growfile, openable by the decoder before it is finished.
// ---------------------------------------------------------------------------

typedef struct {
	long track_id;
	char path[512];
	char marker[544];
	FILE *f;
	long done;	// bytes written to the file
	long total; // bytes expected, 0 when unknown
	unsigned generation;

	tidal_stream_t stream;

	// FILE
	http_stream_t http;
	bool http_open;

	// DASH
	mp4flac_t unwrap;
	int next_segment; // the next one to request, in Tidal's numbering
	bool *complete_transfer; // only set for a synchronous download
} download_t;

static unsigned generation = 1;
static pthread_mutex_t gen_lock = PTHREAD_MUTEX_INITIALIZER;

static void (*slow_start_cb)(int seconds);

void tidalcache_set_slow_start_cb(void (*cb)(int seconds)) { slow_start_cb = cb; }

void tidalcache_wait_idle(void) { streamturn_wait_idle(); }

static pthread_mutex_t active_lock = PTHREAD_MUTEX_INITIALIZER;
static http_stream_t *active_stream;
static long active_track_id;
static int active_percent = -1;

int tidalcache_download_percent(long track_id) {
	pthread_mutex_lock(&active_lock);
	int percent = track_id > 0 && track_id == active_track_id ? active_percent : -1;
	pthread_mutex_unlock(&active_lock);
	return percent;
}

static void report_progress(long done, long total) {
	if (total > 0) {
		int percent = done >= total ? 99 : (int)(100.0 * done / total);
		pthread_mutex_lock(&active_lock);
		active_percent = percent;
		pthread_mutex_unlock(&active_lock);
	}
}

long tidalcache_downloading_id(void) {
	pthread_mutex_lock(&active_lock);
	long id = active_track_id;
	pthread_mutex_unlock(&active_lock);
	return id;
}

static volatile bool network_wanted;

void tidalcache_set_network_wanted(bool wanted) { network_wanted = wanted; }
bool tidalcache_network_wanted(void) { return network_wanted; }

void tidalcache_abandon_all(void) {
	pthread_mutex_lock(&gen_lock);
	generation++;
	pthread_mutex_unlock(&gen_lock);

	// The tug on the socket, as in qobuzcache: bumping the generation alone is
	// not enough, because the downloader only notices at the next chunk and a
	// stalled socket can keep it waiting twenty seconds.
	//
	// FILE only. DASH has no socket to tug -- each segment is its own request
	// -- and needs none: a segment is four seconds of audio, a few hundred
	// kilobytes, so the check between segments comes round fast enough not to
	// be noticed.
	pthread_mutex_lock(&active_lock);
	if (active_stream) {
		http_stream_wake(active_stream);
	}
	pthread_mutex_unlock(&active_lock);
}

static unsigned current_generation(void) {
	pthread_mutex_lock(&gen_lock);
	unsigned value = generation;
	pthread_mutex_unlock(&gen_lock);
	return value;
}

static bool download_end(download_t *d, bool ok) {
	// The stream is about to die: take it out of sight of anything that might
	// tug it first, and only then close it.
	pthread_mutex_lock(&active_lock);
	if (active_stream == &d->http) {
		active_stream = NULL;
	}
	if (active_track_id == d->track_id) {
		active_track_id = 0;
	}
	pthread_mutex_unlock(&active_lock);

	if (d->f) {
		if (fclose(d->f) != 0) {
			ok = false;
		}
		d->f = NULL;
	}
	if (d->http_open) {
		http_stream_close(&d->http);
		d->http_open = false;
	}

	if (ok && d->stream.kind == TIDAL_STREAM_FILE && d->total > 0 && d->done != d->total) {
		fprintf(stderr, "tidalcache: %ld cut short at %ld of %ld bytes\n", d->track_id, d->done, d->total);
		ok = false;
	}
	// On DASH the file starts with a locally written header, so it is never
	// empty even when not one sample arrived. The right comparison is against
	// that header: a file still exactly its length got no music, and marking
	// it complete would keep a silent track in the cache forever, never
	// downloaded again.
	if (ok && d->stream.kind == TIDAL_STREAM_DASH && d->done <= d->unwrap.header_bytes) {
		fprintf(stderr, "tidalcache: %ld brought no samples\n", d->track_id);
		ok = false;
	}
	struct stat st;
	if (ok && (d->done <= 0 || stat(d->path, &st) != 0 || st.st_size != d->done ||
		remove(d->marker) != 0)) {
		ok = false;
	}

	if (d->complete_transfer) {
		*d->complete_transfer = ok && (d->stream.kind == TIDAL_STREAM_DASH || d->total > 0);
	}
	growfile_finish(d->path, ok);

	if (ok) {
		printf("tidalcache: %ld complete (%ld KB)\n", d->track_id, d->done / 1024);
	} else {
		// The marker is left in place: next time round the track counts as
		// incomplete and is downloaded again instead of playing half of it.
		printf("tidalcache: %ld not completed\n", d->track_id);
	}

	// The turn is released last, not as soon as the stream is closed: while
	// the marker is still there the track counts as incomplete, and whoever
	// was waiting for the turn would start downloading what has just finished
	// downloading.
	streamturn_release();

	free(d);
	return ok;
}

// --- the FILE shape --------------------------------------------------------

static bool pump_file(download_t *d, long until, bool *finished) {
	char buf[FETCH_CHUNK];
	*finished = false;

	while (d->done < until || until == 0) {
		if (current_generation() != d->generation) {
			return false; // something else is playing now
		}

		int n = http_stream_read(&d->http, buf, (int)sizeof(buf));
		if (n < 0) {
			return false;
		}
		if (n == 0) {
			*finished = true;
			return true;
		}
		if (fwrite(buf, 1, (size_t)n, d->f) != (size_t)n) {
			fprintf(stderr, "tidalcache: write failed (card full?)\n");
			return false;
		}

		// Push out what was written before announcing it: without the fflush a
		// reader would go looking on the card for bytes still sitting in the
		// stdio buffer.
		if (fflush(d->f) != 0) {
			return false;
		}
		d->done += n;
		growfile_progress(d->path, d->done);
		report_progress(d->done, d->total);
	}
	return true;
}

// --- the DASH shape --------------------------------------------------------

// One segment: request, unwrap, write. `count` of them, or to the end when
// count is 0.
static bool pump_dash(download_t *d, int count, bool *finished) {
	*finished = false;

	// In long: the sum of two ints coming off the network can overflow, and a
	// negative limit ended the track before it began.
	long last = (long)d->stream.start_number + (long)d->stream.segment_count;

	int done_now = 0;
	while (count == 0 || done_now < count) {
		if ((long)d->next_segment >= last) {
			*finished = true;
			return true;
		}
		if (current_generation() != d->generation) {
			return false;
		}

		char url[TIDAL_URL_MAX];
		if (!tidal_dash_segment_url(&d->stream, d->next_segment, url, sizeof(url))) {
			fprintf(stderr, "tidalcache: cannot build segment %d\n", d->next_segment);
			return false;
		}

		char *body = NULL;
		size_t len = 0;
		if (!http_get(url, &body, &len, SEGMENT_LIMIT, FETCH_TIMEOUT_SECS) || !body) {
			fprintf(stderr, "tidalcache: segment %d did not arrive\n", d->next_segment);
			free(body);
			return false;
		}
		if (len == 0) {
			// An empty segment is not a segment. Carrying on would leave a
			// silent gap in the middle of the track, in a file then marked
			// complete and never downloaded again.
			fprintf(stderr, "tidalcache: segment %d empty\n", d->next_segment);
			free(body);
			return false;
		}

		bool ok = mp4flac_segment(&d->unwrap, (const uint8_t *)body, len);
		free(body);
		if (!ok) {
			return false;
		}

		if (fflush(d->f) != 0) {
			return false;
		}
		d->done = d->unwrap.written;
		growfile_progress(d->path, d->done);

		d->next_segment++;
		report_progress(d->next_segment - d->stream.start_number, d->stream.segment_count);
		done_now++;
	}
	return true;
}

static void *download_thread(void *arg) {
	// In the background like every other worker: there is only one core, and a
	// thread writing to the card at full priority takes it from the interface.
	thread_be_background("tidal download");

	download_t *d = arg;
	bool finished = false;
	bool ok = d->stream.kind == TIDAL_STREAM_DASH ? pump_dash(d, 0, &finished) : pump_file(d, 0, &finished);
	download_end(d, ok && finished);
	return NULL;
}

// --- starting a download ---------------------------------------------------

// What both shapes have in common: the marker and the output file.
static bool open_output(download_t *d) {
	// The marker before the file: if power is lost between the two lines, an
	// orphan marker (which does nothing) is better than an incomplete file
	// that looks good.
	FILE *marker = fopen(d->marker, "wb");
	if (marker) {
		fclose(marker);
	}

	d->f = fopen(d->path, "wb");
	if (!d->f) {
		fprintf(stderr, "tidalcache: %s does not open for writing: %s\n", d->path, strerror(errno));
		set_error(errno == ENOSPC ? tr("card_full") : tr("card_write_failed"));
		return false;
	}
	return true;
}

// The two start_* functions have a precise contract with their caller, spelled
// out because getting it wrong here is a use-after-free.
//
// `*consumed` comes out true when download_end() has already run: the track
// finished during prebuffering, or it failed after the file was open. In both
// cases `d` no longer exists, and the caller must not touch it nor release the
// turn (download_end already did).
//
// False means `d` is still alive: either it went well and the rest downloads
// on a thread, or it failed before opening anything and must be freed by hand.
static bool start_dash(download_t *d, bool *consumed, char *out, size_t size) {
	// The init segment. Not audio: it carries the STREAMINFO, without which
	// the file is not a FLAC. If it does not arrive there is nothing to be
	// done, and saying so now beats discovering it at the first frame.
	char *init = NULL;
	size_t init_len = 0;
	if (!http_get(d->stream.init_url, &init, &init_len, SEGMENT_LIMIT, FETCH_TIMEOUT_SECS) || !init) {
		const char *why = http_last_error();
		set_error(why && why[0] ? why : tr("tidal_cannot_reach_tidal"));
		free(init);
		return false;
	}

	// Make room before starting, which needs an idea of how much the track
	// will take. A single file has Content-Length; DASH does not, so it is
	// estimated: a segment is about four seconds, and 24-bit FLAC runs about
	// half a megabyte per second. The estimate is coarse and deliberately high
	// -- pruning a little more than necessary costs a cover re-fetch, pruning
	// too little fills the user's card. prune(0) here would prune nothing at
	// all and let the cache grow past its ceiling.
	prune((long)d->stream.segment_count * 2L * 1024L * 1024L);

	if (!open_output(d)) {
		free(init);
		return false;
	}

	bool ok = mp4flac_begin(&d->unwrap, d->f, d->stream.codecs, (const uint8_t *)init, init_len);
	free(init);
	if (!ok) {
		set_error(tr("tidal_format_unsupported"));
		fclose(d->f);
		d->f = NULL;
		return false;
	}
	d->done = d->unwrap.written;
	d->next_segment = d->stream.start_number;

	growfile_announce(d->path, 0); // total unknown: it emerges as the file grows
	snprintf(out, size, "%.500s", d->path);

	// Prebuffer: a few segments in hand before the decoder starts. Each is
	// worth about four seconds.
	int head = DASH_HEAD_SEGMENTS;
	if (head > d->stream.segment_count) {
		head = d->stream.segment_count;
	}

	struct timespec t0;
	clock_gettime(CLOCK_MONOTONIC, &t0);

	bool finished = false;
	if (!pump_dash(d, head, &finished)) {
		set_error(current_generation() != d->generation ? tr("download_cancelled") : tr("download_interrupted"));
		download_end(d, false);
		*consumed = true;
		out[0] = '\0';
		return false;
	}

	// If the first segments took more than a couple of seconds, the rest will
	// take as long per piece, which is worth telling the user.
	if (!finished && slow_start_cb) {
		struct timespec t1;
		clock_gettime(CLOCK_MONOTONIC, &t1);
		double elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
		if (elapsed >= 2.0) {
			slow_start_cb((int)elapsed);
		}
	}

	if (finished) {
		download_end(d, true); // short track: it is all here already
		*consumed = true;
	}
	return true;
}

static bool start_file(download_t *d, bool *consumed, int duration_secs, char *out, size_t size) {
	if (!http_stream_open(&d->http, d->stream.url, FETCH_TIMEOUT_SECS)) {
		const char *why = http_last_error();
		fprintf(stderr, "tidalcache: %ld does not open%s%s\n", d->track_id, why && why[0] ? ": " : "",
				why && why[0] ? why : "");
		set_error(why && why[0] ? why : tr("tidal_cannot_reach_tidal"));
		return false;
	}
	d->http_open = true;
	d->total = d->http.content_length;
	prune(d->total > 0 ? d->total : 0);

	if (!open_output(d)) {
		return false;
	}

	pthread_mutex_lock(&active_lock);
	active_stream = &d->http;
	pthread_mutex_unlock(&active_lock);

	growfile_announce(d->path, d->total);

	// The prebuffer, identical to Qobuz's: a first chunk is downloaded against
	// the clock, which gives the network's speed, and from that how much must
	// be in hand for playback not to catch up with the download. The reasoning
	// in full is in qobuzcache.c and qobuzcache.h: same formula, same numbers,
	// and a change has to be made in both places.
	long want = STREAM_PREBUFFER;
	if (d->total > 0 && d->total < want) {
		want = d->total;
	}

	long probe = want < STREAM_PROBE ? want : STREAM_PROBE;
	struct timespec t0;
	clock_gettime(CLOCK_MONOTONIC, &t0);

	bool finished = false;
	if (!pump_file(d, probe, &finished)) {
		set_error(current_generation() != d->generation ? tr("download_cancelled") : tr("download_interrupted"));
		download_end(d, false);
		*consumed = true;
		out[0] = '\0';
		return false;
	}

	if (!finished && d->total > 0 && duration_secs > 0) {
		struct timespec t1;
		clock_gettime(CLOCK_MONOTONIC, &t1);
		double elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;

		double measured = elapsed > 0.3 ? d->done / elapsed : 0.0;
		double required = (double)d->total / duration_secs;

		if (required > 0) {
			long floor_bytes = (long)(required * STREAM_HEAD_SECS);
			if (floor_bytes > want) {
				want = floor_bytes;
			}
			if (want > d->total) {
				want = d->total;
			}
		}

		if (measured > 0 && required > 0) {
			double head = (double)d->total * (1.0 - measured / (required * 1.2));
			double cap = required * STREAM_HEAD_MAX_SECS;
			if (head > cap) {
				head = cap;
			}
			if (head > want) {
				want = (long)head;
			}
			if (want > d->total) {
				want = d->total;
			}
			printf("tidalcache: %ld needs %.0f KB/s, the network gives %.0f -> starting with %ld KB of %ld\n", d->track_id,
				   required / 1024, measured / 1024, want / 1024, d->total / 1024);

			int wait_secs = (int)((want - d->done) / measured);
			if (wait_secs >= 2 && slow_start_cb) {
				slow_start_cb(wait_secs);
			}
		}
	}

	if (!finished && d->done < want && !pump_file(d, want, &finished)) {
		set_error(current_generation() != d->generation ? tr("download_cancelled") : tr("download_interrupted"));
		download_end(d, false);
		*consumed = true;
		out[0] = '\0';
		return false;
	}

	snprintf(out, size, "%.500s", d->path);

	if (finished) {
		download_end(d, true); // short track: it is all here already
		*consumed = true;
	}
	return true;
}

static bool start_download(long track_id, const tidal_stream_t *stream, int duration_secs, char *out, size_t size,
						   bool *complete_transfer) {
	if (!out || size == 0) {
		return false;
	}
	out[0] = '\0';
	set_error("");

	if (!tidalcache_ready()) {
		fprintf(stderr, "tidalcache: no card to write to\n");
		set_error(tr("download_no_card"));
		return false;
	}
	if (!stream) {
		set_error(tr("tidal_no_track_address"));
		return false;
	}
	if (stream->kind == TIDAL_STREAM_FILE && !stream->url[0]) {
		set_error(tr("tidal_no_track_address"));
		return false;
	}
	if (stream->kind == TIDAL_STREAM_DASH && (!stream->init_url[0] || stream->segment_count <= 0)) {
		set_error(tr("tidal_manifest_incomplete"));
		return false;
	}

	const char *ext = tidalcache_extension(stream);

	char path[512];
	if (complete_transfer) {
		*complete_transfer = false;
		snprintf(path, sizeof(path), "%.400s/%ld.download.%s", cache_dir, track_id, ext);
	} else {
		path_with_ext(track_id, ext, path, sizeof(path));
	}

	// Streaming may reuse its cache. Explicit downloads request their own quality.
	if (!complete_transfer && have_file(path, out, size)) {
		return true;
	}

	if (!ensure_dir()) {
		set_error(tr("cache_folder_failed"));
		return false;
	}

	// This track is already downloading, and it is the one being listened to.
	// Starting over would mean reopening the file for writing -- truncating it
	// -- under the nose of the decoder reading it.
	if (growfile_is_growing(path)) {
		snprintf(out, size, "%.500s", path);
		return true;
	}

	download_t *d = calloc(1, sizeof(*d));
	if (!d) {
		set_error(tr("out_of_memory"));
		return false;
	}
	d->track_id = track_id;
	d->complete_transfer = complete_transfer;
	d->stream = *stream;
	snprintf(d->path, sizeof(d->path), "%s", path);
	marker_path(d->path, d->marker, sizeof(d->marker));

	// One at a time, and the turn is shared with Qobuz: there is only one
	// network.
	//
	// The download in progress is not abandoned from here: this function is
	// also called by the prefetch, and abandoning would cut off the track
	// being listened to in order to get ahead with the next one. Abandoning is
	// done by whoever received a user command -- the page, via
	// streamturn_abandon_all().
	streamturn_acquire();

	// The generation is taken AFTER the wait: a value read before it would be
	// stale by the time the turn arrives, and a track change that happened
	// during the wait would cancel this download the moment it starts.
	d->generation = current_generation();

	pthread_mutex_lock(&active_lock);
	active_track_id = track_id;
	active_percent = -1;
	pthread_mutex_unlock(&active_lock);

	bool consumed = false;
	bool started = d->stream.kind == TIDAL_STREAM_DASH ? start_dash(d, &consumed, out, size)
													   : start_file(d, &consumed, duration_secs, out, size);

	// From here on `d` may be touched only while consumed is false: when true,
	// download_end() has already freed it, turn included.
	if (consumed) {
		return started && (!complete_transfer || have_file(out, NULL, 0));
	}

	if (!started) {
		// Failed before opening anything: nothing to close here except the
		// turn, which download_end never got the chance to release.
		if (d->http_open) {
			http_stream_close(&d->http);
		}
		pthread_mutex_lock(&active_lock);
		if (active_stream == &d->http) {
			active_stream = NULL;
		}
		if (active_track_id == track_id) {
			active_track_id = 0;
		}
		pthread_mutex_unlock(&active_lock);
		streamturn_release();
		free(d);
		return false;
	}

	if (complete_transfer) {
		bool finished = false;
		bool ok = stream->kind == TIDAL_STREAM_DASH ? pump_dash(d, 0, &finished) : pump_file(d, 0, &finished);
		return download_end(d, ok && finished);
	}

	// Both values are read before the thread starts: from that moment `d`
	// belongs to the thread, and a short file can download in full and free it
	// before any line below could read it.
	long started_done = d->done;
	bool started_dash = d->stream.kind == TIDAL_STREAM_DASH;

	pthread_t thread;
	if (pthread_create(&thread, NULL, download_thread, d) != 0) {
		set_error(tr("cannot_start_the_download"));
		download_end(d, false); // releases the turn as well
		out[0] = '\0';
		return false;
	}
	pthread_detach(thread);

	printf("tidalcache: %ld starts at %ld KB%s\n", track_id, started_done / 1024,
		   started_dash ? " (segments)" : "");
	return true;
}

bool tidalcache_start(long track_id, const tidal_stream_t *stream, int duration_secs, char *out, size_t size) {
	return start_download(track_id, stream, duration_secs, out, size, NULL);
}

bool tidalcache_download(long track_id, const tidal_stream_t *stream, char *out, size_t size, bool *complete_transfer) {
	return complete_transfer && start_download(track_id, stream, 0, out, size, complete_transfer);
}

// ---------------------------------------------------------------------------
// kept downloads
// ---------------------------------------------------------------------------

static void keep_marker(long track_id, char *out, size_t size) {
	snprintf(out, size, "%.400s/%ld.keep", cache_dir, track_id);
}

// The track id a directory entry belongs to, 0 when it is not one. The id is
// the leading digits of the name, so a track ("101.flac"), its tags
// ("101.flac.tags"), its cover ("101.jpg") and the marker itself
// ("101.keep") all resolve to the same id.
static long entry_track_id(const char *name) {
	if (*name < '0' || *name > '9') {
		return 0;
	}
	char *end = NULL;
	long id = strtol(name, &end, 10);
	if (!end || *end != '.' || id <= 0) {
		return 0;
	}
	return id;
}

static bool is_kept_id(long track_id) {
	if (!tidalcache_ready() || track_id <= 0) {
		return false;
	}
	char marker[512];
	keep_marker(track_id, marker, sizeof(marker));
	struct stat st;
	return stat(marker, &st) == 0;
}

// Markers record a path relative to Music/Tidal. Older markers were empty.
static bool saved_path(long track_id, char *out, size_t size, bool *imported) {
	if (imported) *imported = false;
	if (!is_kept_id(track_id)) {
		return false;
	}
	char marker[512], relative[512], path[512];
	keep_marker(track_id, marker, sizeof(marker));
	FILE *f = fopen(marker, "r");
	if (!f) {
		return false;
	}
	bool recorded = fgets(relative, sizeof(relative), f) != NULL;
	fclose(f);
	if (!recorded) {
		return find_in(music_dir, track_id, out, size);
	}
	char *newline = strchr(relative, '\n');
	if (!newline) {
		return false;
	}
	*newline = '\0';
	bool linked = relative[0] == '!';
	const char *rel = relative + linked;
	for (const char *p = rel; p; ) {
		size_t len = strcspn(p, "/");
		if (!len || (len == 1 && p[0] == '.') || (len == 2 && p[0] == '.' && p[1] == '.')) {
			return false;
		}
		p = p[len] ? p + len + 1 : NULL;
	}
	bool ok = snprintf(path, sizeof(path), "%s/%s", music_dir, rel) < (int)sizeof(path) &&
		(linked || tidalcache_track_id(path) == track_id) && have_file(path, out, size);
	if (ok && imported) *imported = linked;
	return ok;
}

static bool find_saved(long track_id, char *out, size_t size) {
	return saved_path(track_id, out, size, NULL);
}

static void saved_metadata(long id, char *path, size_t size) {
	snprintf(path, size, "%.400s/%ld.link", cache_dir, id);
}

static void kept_metadata(long id, char *path, size_t size) {
	bool imported = false;
	if (saved_path(id, NULL, 0, &imported) && imported) {
		saved_metadata(id, path, size);
	}
}

static bool store_kept_path(long id, const char *relative) {
	char marker[512], tmp[528];
	keep_marker(id, marker, sizeof(marker));
	snprintf(tmp, sizeof(tmp), "%s.XXXXXX", marker);
	int fd = mkstemp(tmp);
	if (fd < 0) {
		return false;
	}
	FILE *f = fdopen(fd, "w");
	if (!f) {
		close(fd);
		remove(tmp);
		return false;
	}
	bool written = !relative || fprintf(f, "%s\n", relative) >= 0;
	bool ok = fclose(f) == 0 && written && rename(tmp, marker) == 0;
	if (!ok) {
		remove(tmp);
	}
	return ok;
}

static void filename_component(const char *input, const char *fallback, char *out, size_t size) {
	size_t len = 0;
	while (*input && len + 1 < size) {
		unsigned char c = (unsigned char)*input;
		if (c < 32 || c == 127 || strchr("<>:\"/\\|?*", c)) {
			out[len++] = '_';
			input++;
		} else {
			// Keep whole UTF-8 characters when a component must be shortened.
			size_t bytes = 1;
			while (((unsigned char)input[bytes] & 0xc0) == 0x80) {
				bytes++;
			}
			if (len + bytes >= size) {
				break;
			}
			memcpy(out + len, input, bytes);
			len += bytes;
			input += bytes;
		}
	}
	while (len && (out[len - 1] == ' ' || out[len - 1] == '.')) {
		len--;
	}
	out[len] = '\0';
	if (!len) {
		snprintf(out, size, "%s", fallback);
	} else if (out[0] == '.') {
		out[0] = '_'; // the library scan ignores dotfiles and dotdirs
	}
}

static bool path_missing(const char *path) {
	struct stat st;
	return lstat(path, &st) != 0 && errno == ENOENT;
}

// Score an existing edition within the album artist's directory. A different
// bracketed ID is never a name match. Equal best matches are ambiguous.
static int album_match(const char *name, const char *album, const char *id, int year) {
	char edition[64];
	snprintf(edition, sizeof(edition), " [%s]", id);
	size_t k = strlen(edition);
	const char *match = *id ? strstr(name, edition) : NULL;
	if (match && (!match[k] || (strlen(match + k) == 7 && match[k] == ' ' && match[k + 1] == '[' &&
		match[k + 6] == ']' && strspn(match + k + 2, "0123456789") == 4))) {
		return match[k] ? 4 : 3;
	}
	size_t n = strlen(album);
	if (strncasecmp(name, album, n) != 0 || (name[n] && strncmp(name + n, " [", 2) != 0)) return 0;
	const char *suffix = name + n;
	if (!*suffix) return 1;
	char date[16];
	snprintf(date, sizeof(date), " [%d]", year);
	return year > 0 && strcmp(suffix, date) == 0 ? 2 : 0;
}

static bool album_folder(const tidal_track_t *track, char *out, size_t size) {
	char artist[65], album[81], id[TIDAL_ID_MAX], parent[512];
	filename_component(track->album_artist[0] ? track->album_artist : track->artist, "Unknown Artist", artist, sizeof(artist));
	filename_component(track->album, "Unknown Album", album, sizeof(album));
	filename_component(track->album_id, "", id, sizeof(id));
	if (snprintf(parent, sizeof(parent), "%s/%s", music_dir, artist) >= (int)sizeof(parent)) return false;
	char canonical[160];
	int n = snprintf(canonical, sizeof(canonical), "%s%s%s%s", album, *id ? " [" : "", id, *id ? "]" : "");
	if (track->release_year >= 1000 && track->release_year <= 9999) {
		snprintf(canonical + n, sizeof(canonical) - (size_t)n, " [%d]", track->release_year);
	}
	char best[256] = "";
	int score = 0;
	bool tie = false;
	DIR *d = opendir(parent);
	if (d) {
		struct dirent *e;
		while ((e = readdir(d))) {
			int rank = album_match(e->d_name, album, id, track->release_year);
			if (!rank || rank < score) continue;
			char path[512];
			struct stat st;
			if (snprintf(path, sizeof(path), "%s/%s", parent, e->d_name) >= (int)sizeof(path) ||
				lstat(path, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
			if (rank == score) { tie = true; continue; }
			score = rank;
			tie = false;
			snprintf(best, sizeof(best), "%s", e->d_name);
		}
		closedir(d);
	}
	return snprintf(out, size, "%s/%s", parent, score && !tie ? best : canonical) < (int)size;
}

static void track_stem(const tidal_track_t *track, char *out, size_t size) {
	char performer[65], title[101], prefix[32] = "";
	filename_component(track->artist[0] ? track->artist : track->album_artist, "Unknown Artist", performer, sizeof(performer));
	filename_component(track->title, "Unknown Track", title, sizeof(title));
	if (track->track_number > 0 && track->disc_number > 1) {
		snprintf(prefix, sizeof(prefix), "%02d-%02d - ", track->disc_number, track->track_number);
	} else if (track->track_number > 0) {
		snprintf(prefix, sizeof(prefix), "%02d - ", track->track_number);
	}
	snprintf(out, size, "%s%s - %s", prefix, performer, title);
}

static bool publish_kept(const tidal_track_t *track, const char *from) {
	char folder[512], to[512], stem[200];
	track_stem(track, stem, sizeof(stem));
	const char *ext = strrchr(from, '.');
	if (!ext || !album_folder(track, folder, sizeof(folder)) || !ensure_path(folder)) {
		return false;
	}
	char to_tags[544];
	for (int attempt = 0; ; attempt++) {
		char collision[24] = "";
		if (attempt) {
			snprintf(collision, sizeof(collision), " [%ld]", track->id);
		}
		if (snprintf(to, sizeof(to), "%s/%s%s%s", folder, stem, collision, ext) >= 500) {
			return false;
		}
		snprintf(to_tags, sizeof(to_tags), "%.500s.tags", to);
		if (path_missing(to) && path_missing(to_tags)) {
			break;
		}
		if (attempt) {
			return false;
		}
	}
	char cover_px[16] = "";
	read_tag(from, "cover_px", cover_px, sizeof(cover_px));
	if (!write_tags(to, track, atoi(cover_px))) {
		return false;
	}
	bool legacy = is_kept_id(track->id);
	// Publish metadata first; the final rename makes complete audio visible.
	if (!store_kept_path(track->id, to + strlen(music_dir) + 1) || rename(from, to) != 0) {
		if (legacy) {
			store_kept_path(track->id, NULL);
		} else {
			tidalcache_unkeep(track->id);
		}
		remove(to_tags);
		return false;
	}
	char sidecar[544];
	snprintf(sidecar, sizeof(sidecar), "%.500s.tags", from);
	remove(sidecar);
	char from_cover[544], to_cover[544];
	snprintf(from_cover, sizeof(from_cover), "%.*s.jpg", (int)(ext - from), from);
	snprintf(to_cover, sizeof(to_cover), "%.*s.jpg", (int)(strrchr(to, '.') - to), to);
	if (path_missing(to_cover)) {
		rename(from_cover, to_cover);
	}
	return true;
}

static bool verify_audio(const char *path, int expected_seconds, bool complete_transfer) {
	decode_format_t format = decode_detect_format(path);
	if (format == DECODE_FORMAT_UNKNOWN) {
		return false;
	}
	decoder_t *dec = decoder_open(path, format);
	if (!dec) {
		return false;
	}
	int rate = decoder_sample_rate(dec);
	int channels = decoder_channels(dec);
	uint64_t frames = 0, reported = decoder_total_pcm_frames(dec);
	short pcm[2048];
	uint64_t minimum = rate > 0 && expected_seconds > 2 ? (uint64_t)rate * (uint64_t)(expected_seconds - 2) : 1;
	// Probe the first and last audio of a complete, indexed transfer. Unknown
	// lengths and imports still get a full decode. MP3 seeks can build an index.
	if (complete_transfer && (format == DECODE_FORMAT_FLAC || format == DECODE_FORMAT_AAC_MP4) &&
		rate > 0 && reported > (uint64_t)rate && channels > 0 && channels <= 8) {
		uint64_t chunk = sizeof(pcm) / sizeof(pcm[0]) / channels;
		uint64_t padding = format == DECODE_FORMAT_FLAC ? 0 : (uint64_t)rate / 20;
		uint64_t tail = (uint64_t)rate / 4;
		bool ok = reported >= minimum && decoder_read_pcm_frames_s16(dec, chunk, pcm) > 0 &&
			decoder_seek_to_frame(dec, reported - tail);
		if (ok) {
			uint64_t got;
			while (frames < tail && (got = decoder_read_pcm_frames_s16(dec, chunk, pcm)) != 0) frames += got;
			ok = frames > 0 && frames + padding >= tail;
		}
		decoder_close(dec);
		return ok;
	}
	if (rate > 0 && channels > 0 && channels <= 8) {
		uint64_t got;
		while ((got = decoder_read_pcm_frames_s16(dec, sizeof(pcm) / sizeof(pcm[0]) / channels, pcm)) != 0) {
			frames += got;
		}
	}
	decoder_close(dec);
	// API durations are whole seconds; leave room for rounding and codec padding.
	// Compare against container length too when the decoder reports one.
	if (!frames || rate <= 0) {
		return false;
	}
	uint64_t padding = format == DECODE_FORMAT_FLAC ? 0 : (uint64_t)rate / 20;
	return (!reported || frames + padding >= reported) &&
		(expected_seconds <= 2 || frames >= (uint64_t)rate * (uint64_t)(expected_seconds - 2));
}

bool tidalcache_reuse(const tidal_track_t *track) {
	if (!track || track->id <= 0 || !track->album[0] || !track->title[0] || !track->artist[0] ||
		track->track_number <= 0 || !tidalcache_ready()) return false;
	char folder[512], stem[200], path[512];
	if (!album_folder(track, folder, sizeof(folder))) return false;
	track_stem(track, stem, sizeof(stem));
	DIR *d = opendir(folder);
	if (!d) return false;
	int score = 0;
	char match[512] = "";
	bool tie = false;
	struct dirent *entry;
	while ((entry = readdir(d))) {
		const char *ext = strrchr(entry->d_name, '.');
		if (!ext || (strcasecmp(ext, ".flac") && strcasecmp(ext, ".m4a") && strcasecmp(ext, ".mp3"))) continue;
		if (snprintf(path, sizeof(path), "%s/%s", folder, entry->d_name) >= 500) continue;
		struct stat st;
		if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode) || !have_file(path, NULL, 0)) continue;
		long existing_id = tidalcache_track_id(path);
		char edition[TIDAL_ID_MAX];
		if ((existing_id && existing_id != track->id) ||
			(track->album_id[0] && read_tag(path, "album_id", edition, sizeof(edition)) && strcmp(track->album_id, edition))) continue;
		size_t len = (size_t)(ext - entry->d_name);
		int rank = existing_id ? 2 : (strlen(stem) == len && strncasecmp(entry->d_name, stem, len) == 0);
		if (!rank || rank < score) continue;
		if (rank == score) { tie = true; continue; }
		score = rank;
		tie = false;
		snprintf(match, sizeof(match), "%s", path);
	}
	closedir(d);
	if (!score || tie || !verify_audio(match, track->duration, false) || !ensure_dir()) return false;
	char metadata[512], relative[512];
	saved_metadata(track->id, metadata, sizeof(metadata));
	snprintf(relative, sizeof(relative), "!%s", match + strlen(music_dir) + 1);
	return write_tags(metadata, track, 0) && store_kept_path(track->id, relative);
}

void tidalcache_download_extras(const tidal_track_t *track) {
	char path[512];
	if (!find_saved(track->id, path, sizeof(path))) return;
	char sidecar[544], per_track[544];
	const char *slash = strrchr(path, '/');
	const char *dot = strrchr(path, '.');
	if (!slash || !dot || dot < slash) return;
	snprintf(sidecar, sizeof(sidecar), "%.*s/cover.jpg", (int)(slash - path), path);
	snprintf(per_track, sizeof(per_track), "%.*s.jpg", (int)(dot - path), path);
	if (path_missing(sidecar)) {
		char *body = NULL;
		size_t len = 0;
		FILE *source = fopen(per_track, "rb");
		if (source) {
			struct stat st;
			if (fstat(fileno(source), &st) == 0 && st.st_size > 0 && st.st_size <= 4 * 1024 * 1024) {
				len = st.st_size;
				body = malloc(len);
				if (!body || fread(body, 1, len, source) != len) len = 0;
			}
			fclose(source);
		} else if (track->cover[0] && !http_get(track->cover, &body, &len, 4 * 1024 * 1024, 8)) {
			len = 0;
		}
		if (body && len >= 4 && (unsigned char)body[0] == 0xff && (unsigned char)body[1] == 0xd8 &&
			(unsigned char)body[len - 2] == 0xff && (unsigned char)body[len - 1] == 0xd9) {
			char tmp[560];
			snprintf(tmp, sizeof(tmp), "%s.XXXXXX", sidecar);
			int fd = mkstemp(tmp);
			if (fd >= 0) {
				FILE *f = fdopen(fd, "wb");
				if (f) {
					bool ok = fwrite(body, 1, len, f) == len;
					if (fclose(f) == 0 && ok && path_missing(sidecar)) rename(tmp, sidecar);
				} else close(fd);
				remove(tmp);
			}
		}
		free(body);
	}
	snprintf(sidecar, sizeof(sidecar), "%.*s.lrc", (int)(dot - path), path);
	if (!path_missing(sidecar) || track->duration <= 0 || !track->artist[0] || !track->title[0]) return;
	lrclib_record_t *records = NULL;
	int count = 0;
	if (lrclib_search(track->title, track->artist, track->album, track->duration, &records, &count) == LRCLIB_OK) {
		for (int i = 0; i < count; i++) {
			const lrclib_record_t *r = &records[i];
			if (r->text && *r->text && r->duration > 0 && abs(r->duration - track->duration) <= 2 &&
				strcasecmp(r->track, track->title) == 0 && strcasecmp(r->artist, track->artist) == 0 &&
				(!track->album[0] || strcasecmp(r->album, track->album) == 0)) {
				if (path_missing(sidecar)) lrclib_save(path, r->text);
				break;
			}
		}
	}
	lrclib_free(records, count);
}

bool tidalcache_keep(const tidal_track_t *track, const char *path, bool complete_transfer) {
	if (!track || track->id <= 0) {
		return false;
	}
	if (find_saved(track->id, NULL, 0)) {
		return true;
	}
	if (!tidalcache_owns(path) || tidalcache_track_id(path) != track->id || !have_file(path, NULL, 0)) {
		return false;
	}
	if (!verify_audio(path, track->duration, complete_transfer)) {
		fprintf(stderr, "tidalcache: %ld failed audio verification\n", track->id);
		// Do not retry by reusing the same bad bytes from the transient cache.
		char marker[544];
		marker_path(path, marker, sizeof(marker));
		FILE *f = fopen(marker, "w");
		if (f) {
			fclose(f);
		} else {
			remove(path);
		}
		return false;
	}
	return publish_kept(track, path);
}

static void migrate_track(long id) {
	char path[512];
	bool imported;
	if (saved_path(id, path, sizeof(path), &imported)) {
		if (imported || strchr(path + strlen(music_dir) + 1, '/')) {
			return;
		}
	} else if (!find_in(cache_dir, id, path, sizeof(path))) {
		return;
	}
	tidal_track_t track = {.id = id};
	read_tag(path, "artist", track.artist, sizeof(track.artist));
	read_tag(path, "album_artist", track.album_artist, sizeof(track.album_artist));
	read_tag(path, "album", track.album, sizeof(track.album));
	read_tag(path, "album_id", track.album_id, sizeof(track.album_id));
	if (!read_tag(path, "title", track.title, sizeof(track.title))) {
		snprintf(track.title, sizeof(track.title), "%ld", id);
	}
	char value[16];
	if (read_tag(path, "track", value, sizeof(value))) {
		track.track_number = atoi(value);
	}
	if (read_tag(path, "disc", value, sizeof(value))) {
		track.disc_number = atoi(value);
	}
	if (read_tag(path, "year", value, sizeof(value))) {
		track.release_year = atoi(value);
	}
	publish_kept(&track, path);
}

void tidalcache_publish_legacy(void) {
	DIR *d = tidalcache_ready() ? opendir(cache_dir) : NULL;
	if (!d) {
		return;
	}
	struct dirent *entry;
	while ((entry = readdir(d)) != NULL) {
		const char *ext = strrchr(entry->d_name, '.');
		if (ext && strcmp(ext, ".keep") == 0) {
			migrate_track(entry_track_id(entry->d_name));
		}
	}
	closedir(d);
}

bool tidalcache_is_kept(long track_id) { return is_kept_id(track_id) && tidalcache_find(track_id, NULL, 0); }

static long kept_path(const char *name, char *path, size_t size) {
	const char *dot = strrchr(name, '.');
	if (!dot || strcmp(dot, ".keep") != 0) {
		return 0;
	}
	long id = entry_track_id(name);
	return id > 0 && tidalcache_find(id, path, size) ? id : 0;
}

void tidalcache_kept_album_counts(const char *const *album_ids, int *counts, int count) {
	for (int i = 0; i < count; i++) {
		counts[i] = 0;
	}
	DIR *d = count > 0 && tidalcache_ready() ? opendir(cache_dir) : NULL;
	if (!d) {
		return;
	}
	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		char path[512], album_id[TIDAL_ID_MAX];
		long id = kept_path(e->d_name, path, sizeof(path));
		if (!id) continue;
		kept_metadata(id, path, sizeof(path));
		if (!read_tag(path, "album_id", album_id, sizeof(album_id)) || !album_id[0]) {
			continue;
		}
		for (int i = 0; i < count; i++) {
			if (strcmp(album_ids[i], album_id) == 0) {
				counts[i]++;
			}
		}
	}
	closedir(d);
}

static bool tidalcache_unkeep(long track_id) {
	if (!is_kept_id(track_id)) {
		return true;
	}
	char marker[512];
	keep_marker(track_id, marker, sizeof(marker));
	return remove(marker) == 0;
}

static bool remove_saved(long track_id) {
	char path[512];
	bool imported;
	if (!saved_path(track_id, path, sizeof(path), &imported) || imported) {
		return true;
	}
	if (remove(path) != 0) {
		return false;
	}
	char sidecar[544];
	snprintf(sidecar, sizeof(sidecar), "%.500s.tags", path);
	remove(sidecar);
	const char *dot = strrchr(path, '.');
	if (dot) {
		snprintf(sidecar, sizeof(sidecar), "%.*s.jpg", (int)(dot - path), path);
		remove(sidecar);
	}
	return true;
}

bool tidalcache_remove_track(long track_id) {
	if (!tidalcache_ready() || track_id <= 0) {
		return false;
	}
	if (!remove_saved(track_id) || !tidalcache_unkeep(track_id)) {
		return false;
	}
	DIR *d = opendir(cache_dir);
	if (!d) {
		return errno == ENOENT;
	}
	bool ok = true;
	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		if (entry_track_id(e->d_name) != track_id) {
			continue;
		}
		char path[640];
		if ((size_t)snprintf(path, sizeof(path), "%s/%s", cache_dir, e->d_name) < sizeof(path) &&
			remove(path) != 0 && errno != ENOENT) {
			ok = false;
		}
	}
	closedir(d);
	return ok;
}

// One kept entry as collected from the directory, before it is sorted.
typedef struct {
	char album[TIDAL_TITLE_MAX];
	char album_artist[TIDAL_NAME_MAX];
	char title[TIDAL_TITLE_MAX];
	char artist[TIDAL_NAME_MAX];
	char album_id[TIDAL_ID_MAX];
	long id;
	int track_number;
	int disc_number;
} kept_entry_t;

static int kept_cmp(const void *a, const void *b) {
	const kept_entry_t *x = a, *y = b;
	int by_album = strcmp(x->album_id, y->album_id);
	if (by_album != 0) {
		return by_album;
	}
	if (x->disc_number != y->disc_number) {
		return x->disc_number < y->disc_number ? -1 : 1;
	}
	if (x->track_number != y->track_number) {
		return x->track_number < y->track_number ? -1 : 1;
	}
	if (strcmp(x->album, y->album) != 0) {
		return strcmp(x->album, y->album);
	}
	return strcmp(x->title, y->title);
}

static int collect_kept(kept_entry_t *all) {
	DIR *d = opendir(cache_dir);
	int count = 0;
	if (d) {
		struct dirent *e;
		while ((e = readdir(d)) != NULL && count < TIDALCACHE_KEPT_LIST_MAX) {
			// One marker per track, regardless of the actual audio container.
			char path[512];
			long id = kept_path(e->d_name, path, sizeof(path));
			if (!id) {
				continue;
			}
			kept_entry_t *kept = &all[count++];
			memset(kept, 0, sizeof(*kept));
			kept->id = id;
			kept_metadata(id, path, sizeof(path));
			read_tag(path, "title", kept->title, sizeof(kept->title));
			read_tag(path, "artist", kept->artist, sizeof(kept->artist));
			read_tag(path, "album", kept->album, sizeof(kept->album));
			read_tag(path, "album_artist", kept->album_artist, sizeof(kept->album_artist));
			read_tag(path, "album_id", kept->album_id, sizeof(kept->album_id));
			char track[16] = "";
			if (read_tag(path, "track", track, sizeof(track))) {
				kept->track_number = atoi(track);
			}
			if (read_tag(path, "disc", track, sizeof(track))) {
				kept->disc_number = atoi(track);
			}
		}
		closedir(d);
	}
	qsort(all, count, sizeof(*all), kept_cmp);
	return count;
}

static void kept_to_track(const kept_entry_t *entry, tidal_track_t *t) {
	memset(t, 0, sizeof(*t));
	t->id = entry->id;
	snprintf(t->title, sizeof(t->title), "%s", entry->title);
	snprintf(t->artist, sizeof(t->artist), "%s", entry->artist);
	snprintf(t->album, sizeof(t->album), "%s", entry->album);
	snprintf(t->album_id, sizeof(t->album_id), "%s", entry->album_id);
	snprintf(t->album_artist, sizeof(t->album_artist), "%s", entry->album_artist);
	t->track_number = entry->track_number;
	t->disc_number = entry->disc_number;
	t->streamable = true;
	if (!t->title[0]) {
		snprintf(t->title, sizeof(t->title), "%ld", entry->id);
	}
}

static const char *group_artist(const kept_entry_t *e) { return e->album_artist[0] ? e->album_artist : e->artist; }

int tidalcache_kept_filtered(tidal_track_t *out, int max, int offset, const char *album, const char *artist) {
	if (!out || max <= 0 || offset < 0 || !tidalcache_ready()) return 0;
	kept_entry_t *all = malloc(sizeof(*all) * TIDALCACHE_KEPT_LIST_MAX);
	if (!all) return 0;
	int count = collect_kept(all), filled = 0;
	for (int i = 0; i < count && filled < max; i++) {
		const char *key = all[i].album_id[0] ? all[i].album_id : all[i].album;
		if ((album && strcmp(album, key)) || (artist && strcmp(artist, group_artist(&all[i])))) continue;
		if (offset > 0) { offset--; continue; }
		kept_to_track(&all[i], &out[filled++]);
	}
	free(all);
	return filled;
}

int tidalcache_kept_list(tidal_track_t *out, int max, int offset) {
	return tidalcache_kept_filtered(out, max, offset, NULL, NULL);
}

bool tidalcache_kept_track(long id, tidal_track_t *out) {
	char path[512], value[16];
	if (!out || !tidalcache_is_kept(id) || !tidalcache_find(id, path, sizeof(path))) return false;
	kept_metadata(id, path, sizeof(path));
	*out = (tidal_track_t){.id = id, .streamable = true};
	read_tag(path, "title", out->title, sizeof(out->title));
	read_tag(path, "artist", out->artist, sizeof(out->artist));
	read_tag(path, "album", out->album, sizeof(out->album));
	read_tag(path, "album_id", out->album_id, sizeof(out->album_id));
	read_tag(path, "album_artist", out->album_artist, sizeof(out->album_artist));
	if (read_tag(path, "track", value, sizeof(value))) out->track_number = atoi(value);
	if (!out->title[0]) snprintf(out->title, sizeof(out->title), "%ld", id);
	return true;
}

int tidalcache_kept_groups(tidal_album_t *out, int max, int offset, bool artists, const char *artist) {
	if (!out || max <= 0 || offset < 0 || !tidalcache_ready()) return 0;
	kept_entry_t *all = malloc(sizeof(*all) * TIDALCACHE_KEPT_LIST_MAX);
	if (!all) return 0;
	int count = collect_kept(all), filled = 0;
	for (int i = 0; i < count && filled < max; i++) {
		const char *owner = group_artist(&all[i]);
		if (artist && strcmp(artist, owner)) continue;
		const char *key = all[i].album_id[0] ? all[i].album_id : all[i].album;
		int matches = 0;
		bool already = false;
		for (int j = 0; j < count; j++) {
			const char *other = all[j].album_id[0] ? all[j].album_id : all[j].album;
			if (strcmp(group_artist(&all[j]), owner) || (!artists && strcmp(key, other))) continue;
			if (j < i) { already = true; break; }
			matches++;
		}
		if (already) continue;
		if (offset > 0) { offset--; continue; }
		tidal_album_t *a = &out[filled++];
		*a = (tidal_album_t){.track_count = matches};
		snprintf(a->title, sizeof(a->title), "%s", artists ? owner : all[i].album);
		snprintf(a->artist, sizeof(a->artist), "%s", owner);
		snprintf(a->id_text, sizeof(a->id_text), "%s", all[i].album_id);
	}
	free(all);
	return filled;
}

void tidalcache_make_room(long bytes) { prune(bytes); }
