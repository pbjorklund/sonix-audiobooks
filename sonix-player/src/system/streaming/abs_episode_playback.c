#include "abs_episode_playback.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "src/system/library/audiobookdb.h"

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static char source[SONIX_LINKED_PATH_MAX];
static abs_episode_manifest_t identity;
static bool active;
static unsigned generation;
static double saved = -1.;
static bool saved_finished;
static long long saved_mono;

static long long clock_ms(clockid_t clock) {
	struct timespec ts;
	if (clock_gettime(clock, &ts) != 0) return 0;
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static bool equal_id(const abs_episode_manifest_t *a, const abs_episode_manifest_t *b) {
	return !strcmp(a->server, b->server) && !strcmp(a->owner, b->owner) &&
		!strcmp(a->show, b->show) && !strcmp(a->episode, b->episode);
}

bool abs_episode_playback_identity(const char *path, abs_episode_manifest_t *out) {
	if (!path || !path[0] || !out) return false;
	pthread_mutex_lock(&lock);
	bool ok = source[0] && !strcmp(source, path) && generation == audiobookdb_history_generation();
	if (ok) *out = identity;
	pthread_mutex_unlock(&lock);
	return ok || abs_episode_card_identity(path, out);
}

void abs_episode_playback_register(const char *path, const abs_episode_manifest_t *id) {
	pthread_mutex_lock(&lock);
	if (path && id && strlen(path) < sizeof(source)) {
		strcpy(source, path);
		identity = *id;
		generation = audiobookdb_history_generation();
		saved = -1.;
		saved_finished = false;
		saved_mono = 0;
		active = true;
	} else active = false;
	pthread_mutex_unlock(&lock);
}

bool abs_episode_playback_active(const abs_episode_manifest_t *id) {
	pthread_mutex_lock(&lock);
	bool yes = active && generation == audiobookdb_history_generation() && equal_id(id, &identity);
	pthread_mutex_unlock(&lock);
	return yes;
}

void abs_episode_playback_deactivate(void) {
	pthread_mutex_lock(&lock);
	active = false;
	pthread_mutex_unlock(&lock);
}

double abs_episode_playback_resume(const char *path) {
	abs_episode_manifest_t id;
	abs_episode_checkpoint_t s;
	if (!abs_episode_playback_identity(path, &id) || !audiobookdb_episode_get(&id, &s, NULL) || s.finished) return -1.;
	return id.duration > 0 && s.seconds >= id.duration ? -1. : s.seconds;
}

void abs_episode_playback_note(const char *path, double seconds, double duration, bool force, bool finished) {
	if (!isfinite(seconds) || seconds < 0 || !isfinite(duration) || duration <= 0) return;
	pthread_mutex_lock(&lock);
	if (!source[0] || strcmp(path, source) || generation != audiobookdb_history_generation()) {
		pthread_mutex_unlock(&lock);
		return;
	}
	active = !force && !finished;
	if (!finished && saved_finished && seconds >= duration - .1) {
		pthread_mutex_unlock(&lock);
		return;
	}
	if (finished) seconds = duration;
	if (seconds > duration) seconds = duration;
	long long now = clock_ms(CLOCK_MONOTONIC);
	if (!finished && ((!force && now - saved_mono < 5000) ||
		(saved >= 0 && !saved_finished && fabs(saved - seconds) < .03) || (saved < 0 && seconds == 0))) {
		pthread_mutex_unlock(&lock);
		return;
	}
	abs_episode_checkpoint_t value = {.identity = identity, .seconds = seconds, .duration = duration,
		.updated_ms = clock_ms(CLOCK_REALTIME), .finished = finished, .dirty = true};
	unsigned epoch = generation;
	pthread_mutex_unlock(&lock);
	if (!audiobookdb_episode_save(epoch, &value)) return;
	pthread_mutex_lock(&lock);
	if (generation == epoch && !strcmp(path, source)) {
		saved = seconds;
		saved_finished = finished;
		saved_mono = now;
	}
	pthread_mutex_unlock(&lock);
	if (force || finished) audiobookshelf_episode_progress_wake();
}

bool abs_episode_mark(const abs_episode_manifest_t *id, bool played) {
	unsigned epoch = audiobookdb_history_generation();
	abs_episode_checkpoint_t value = {.identity = *id, .seconds = played ? id->duration : 0,
		.duration = id->duration, .updated_ms = clock_ms(CLOCK_REALTIME), .finished = played, .dirty = true};
	if (!audiobookdb_episode_save(epoch, &value)) return false;
	audiobookshelf_episode_progress_wake();
	return true;
}
