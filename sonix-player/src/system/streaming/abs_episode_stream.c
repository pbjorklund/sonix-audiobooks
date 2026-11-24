#include "abs_episode_stream.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "src/system/core/utils.h"
#include "src/system/decode/growfile.h"
#include "src/system/streaming/streamturn.h"

#define STREAM_MAX_BYTES (384L * 1024 * 1024)
#define STREAM_HEAD_BYTES (128L * 1024)
#define STREAM_PATH_MAX 512

typedef struct {
	abs_auth_snapshot_t auth;
	char url[ABS_URL_MAX];
	char path[STREAM_PATH_MAX];
	double resume, duration;
	long long bytes;
	bool progressive;
	unsigned generation;
} transfer_t;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static pthread_once_t once = PTHREAD_ONCE_INIT;
static http_op_t op;
static char slots[2][STREAM_PATH_MAX];
static bool complete[2];
static bool running, ready, success;
static int active_slot;

void abs_episode_stream_cancel(void) {
	pthread_mutex_lock(&lock);
	if (running) http_op_cancel(&op);
	pthread_mutex_unlock(&lock);
}

static void setup(void) {
	http_op_init(&op);
	op.same_origin_only = true;
	streamturn_register_abandon(abs_episode_stream_cancel);
}

void abs_episode_stream_quiesce(void) {
	abs_episode_stream_cancel();
	pthread_mutex_lock(&lock);
	while (running) pthread_cond_wait(&changed, &lock);
	pthread_mutex_unlock(&lock);
}

bool abs_episode_stream_owns(const char *path) {
	if (!path || !strstr(path, "/.local/abs-episode-stream-")) return false;
	pthread_mutex_lock(&lock);
	bool ok = (slots[0][0] && !strcmp(path, slots[0])) || (slots[1][0] && !strcmp(path, slots[1]));
	pthread_mutex_unlock(&lock);
	return ok;
}

bool abs_episode_stream_complete(const char *path) {
	if (!path || !strstr(path, "/.local/abs-episode-stream-")) return false;
	pthread_mutex_lock(&lock);
	bool ok = (complete[0] && !strcmp(path, slots[0])) || (complete[1] && !strcmp(path, slots[1]));
	pthread_mutex_unlock(&lock);
	return ok;
}

static bool current(const transfer_t *job) {
	return !http_op_cancelled(&op) && job->generation == abs_download_card_generation();
}

static void *download(void *arg) {
	transfer_t *job = arg;
	thread_be_background("abs-podcast-stream");
	bool turn = false, grew = false, ok = false;
	while (current(job)) {
		if (streamturn_try_acquire()) { turn = true; break; }
		struct timespec delay = {.tv_nsec = 50000000};
		nanosleep(&delay, NULL);
	}
	if (!turn || !current(job)) goto finish;
	char headers[ABS_TOKEN_MAX + 32];
	snprintf(headers, sizeof(headers), "Authorization: Bearer %s\r\n", job->auth.token);
	http_stream_t stream;
	bool opened = http_stream_open_ex_op(&stream, job->url, headers, 15, &op, NULL);
	memset(headers, 0, sizeof(headers));
	abs_profiles_snapshot_wipe(&job->auth);
	if (!opened) goto finish;
	long long total = stream.content_length > 0 ? stream.content_length : job->bytes;
	if (total > STREAM_MAX_BYTES || stream.icy_metaint || !strncasecmp(stream.content_type, "text/", 5) ||
		!strncasecmp(stream.content_type, "application/json", 16) ||
		(stream.content_length > 0 && job->bytes > 0 && stream.content_length != job->bytes)) {
		http_stream_close(&stream);
		goto finish;
	}
	int fd = open(job->path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
	if (fd < 0) { http_stream_close(&stream); goto finish; }
	long head = STREAM_HEAD_BYTES;
	if (job->resume > 0 && job->duration > job->resume && total > 0) {
		double through = job->resume / job->duration;
		// Leave margin for variable bit rate. The decoder still checks that
		// the requested frame is reachable while the bytes continue to grow.
		head = (long)fmin(total, total * through * 1.15 + STREAM_HEAD_BYTES);
	}
	bool progressive = job->progressive && total > head && (job->resume <= 0 || job->duration > job->resume);
	if (progressive) { growfile_announce(job->path, (long)total); grew = true; }
	unsigned char buffer[16384];
	long done = 0;
	while (current(job)) {
		int got = http_stream_read_op(&stream, buffer, sizeof(buffer), &op);
		if (!got) { ok = done > 0 && (total <= 0 || done == total); break; }
		if (got < 0 || got > STREAM_MAX_BYTES - done || (total > 0 && got > total - done) ||
			write(fd, buffer, (size_t)got) != got) break;
		done += got;
		if (grew) {
			growfile_progress(job->path, done);
			if (done >= head) {
				pthread_mutex_lock(&lock);
				if (!ready) { ready = true; pthread_cond_broadcast(&changed); }
				pthread_mutex_unlock(&lock);
			}
		}
	}
	if (close(fd) != 0 || !current(job)) ok = false;
	http_stream_close(&stream);
finish:
	pthread_mutex_lock(&lock);
	complete[active_slot] = ok;
	pthread_mutex_unlock(&lock);
	if (grew) growfile_finish(job->path, ok);
	if (!ok) unlink(job->path);
	if (turn) streamturn_release();
	abs_profiles_snapshot_wipe(&job->auth);
	free(job);
	pthread_mutex_lock(&lock);
	success = ok;
	running = false;
	pthread_cond_broadcast(&changed);
	pthread_mutex_unlock(&lock);
	return NULL;
}

bool abs_episode_stream_start(const abs_auth_snapshot_t *auth, const abs_episode_t *episode,
	double resume, const char *in_use, http_op_t *request, char *path, size_t cap) {
	if (!auth || !episode || !path || !cap || !auth->token[0] || episode->track.size > STREAM_MAX_BYTES) return false;
	const char *ext = abs_episode_extension(&episode->track);
	if (!ext) return false;
	pthread_once(&once, setup);
	abs_episode_stream_quiesce();
	if (http_op_cancelled(request)) return false;
	streamturn_abandon_all();
	transfer_t *job = calloc(1, sizeof(*job));
	if (!job) return false;
	job->generation = abs_download_card_generation();
	job->bytes = episode->track.size;
	job->resume = resume;
	job->duration = episode->track.duration;
	job->progressive = !strcmp(ext, "mp3") || !strcmp(ext, "flac");
	int n = snprintf(job->url, sizeof(job->url), "%s%s", auth->server, episode->track.content_url);
	if (!job->generation || n <= 0 || (size_t)n >= sizeof(job->url)) { free(job); return false; }
	pthread_mutex_lock(&lock);
	int slot = in_use && slots[0][0] && !strcmp(in_use, slots[0]) ? 1 : 0;
	char relative[80];
	// Two fixed slots: remove all extensions of the inactive slot, including
	// leftovers of a power cut. The current input's slot is never unlinked.
	const char *suffixes[] = {"mp3", "m4a", "aac", "ogg", "opus", "flac", "wav", "m4b", "wma"};
	for (size_t i = 0; i < sizeof(suffixes) / sizeof(*suffixes); i++) {
		snprintf(relative, sizeof(relative), ".local/abs-episode-stream-%d.%s", slot, suffixes[i]);
		char stale[STREAM_PATH_MAX];
		if (abs_download_card_path(relative, stale, sizeof(stale))) unlink(stale);
	}
	snprintf(relative, sizeof(relative), ".local/abs-episode-stream-%d.%s", slot, ext);
	if (!abs_download_card_path(relative, job->path, sizeof(job->path)) || strlen(job->path) >= cap) {
		pthread_mutex_unlock(&lock); free(job); return false;
	}
	job->auth = *auth;
	strcpy(slots[slot], job->path);
	complete[slot] = false;
	active_slot = slot;
	http_op_clear(&op);
	success = ready = false;
	running = true;
	pthread_t thread;
	if (pthread_create(&thread, NULL, download, job) != 0) {
		running = false;
		pthread_mutex_unlock(&lock);
		abs_profiles_snapshot_wipe(&job->auth); free(job);
		return false;
	}
	pthread_detach(thread);
	while (running && !ready) {
		if (http_op_cancelled(request)) { http_op_cancel(&op); break; }
		struct timespec until;
		clock_gettime(CLOCK_REALTIME, &until);
		until.tv_nsec += 100000000;
		if (until.tv_nsec >= 1000000000) { until.tv_nsec -= 1000000000; until.tv_sec++; }
		pthread_cond_timedwait(&changed, &lock, &until);
	}
	bool ok = !http_op_cancelled(request) && !http_op_cancelled(&op) && (running ? ready : success);
	if (ok) strcpy(path, slots[slot]);
	pthread_mutex_unlock(&lock);
	return ok;
}
