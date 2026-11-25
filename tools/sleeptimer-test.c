#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "src/system/core/config.h"
#include "src/system/playback/sleeptimer.h"

static long long ms;
static time_t wall;
static long values[] = {-1, -1, -1, -1, -1};
static const char *const keys[] = {"enabled", "minutes", "scheduled", "start", "end"};

int __wrap_clock_gettime(clockid_t clock, struct timespec *ts) {
	assert(clock == CLOCK_MONOTONIC);
	*ts = (struct timespec){.tv_sec = ms / 1000, .tv_nsec = ms % 1000 * 1000000};
	return 0;
}

time_t __wrap_time(time_t *out) {
	if (out) *out = wall;
	return wall;
}

long config_get_int(const char *section, const char *key, long fallback) {
	if (!strcmp(section, "audiobookshelf") && !strcmp(key, "sleep_minutes")) return 15;
	assert(!strcmp(section, "sleep_timer"));
	for (int i = 0; i < 5; i++) if (!strcmp(key, keys[i])) return values[i] == -1 ? fallback : values[i];
	abort();
}

void config_set_int(const char *section, const char *key, long value) {
	assert(!strcmp(section, "sleep_timer"));
	for (int i = 0; i < 5; i++) if (!strcmp(key, keys[i])) { values[i] = value; return; }
	abort();
}

bool config_save(void) { return true; }

static void wall_at(int hours, int minutes) { wall = (hours * 60 + minutes) * 60; }
static void tick(void) { assert(!sleeptimer_expired()); }

int main(void) {
	assert(setenv("TZ", "UTC0", 1) == 0);
	tzset();
	sleeptimer_init();
	assert(!sleeptimer_armed() && sleeptimer_minutes() == 15);
	sleeptimer_set_minutes(30);
	sleeptimer_set_scheduled(true);
	sleeptimer_set_window(22 * 60, 7 * 60);
	sleeptimer_init();
	assert(sleeptimer_minutes() == 30 && sleeptimer_scheduled());
	assert(sleeptimer_start_minute() == 1320 && sleeptimer_end_minute() == 420);
	wall_at(21, 59); assert(!sleeptimer_armed());
	wall_at(22, 0); assert(sleeptimer_armed());
	wall_at(0, 0); assert(sleeptimer_armed());
	wall_at(6, 59); assert(sleeptimer_armed());
	wall_at(7, 0); assert(!sleeptimer_armed());
	sleeptimer_set_window(12 * 60, 13 * 60);
	wall_at(11, 59); assert(!sleeptimer_armed());
	wall_at(12, 0); assert(sleeptimer_armed());
	wall_at(13, 0); assert(!sleeptimer_armed());
	sleeptimer_set_window(0, 0);
	assert(sleeptimer_armed());
	sleeptimer_set_scheduled(false);
	assert(!sleeptimer_armed());
	sleeptimer_set_enabled(true);
	assert(sleeptimer_armed());
	sleeptimer_note_playing(1);
	tick();
	ms += 600000;
	tick();
	sleeptimer_note_playing(0);
	sleeptimer_note_playing(1);
	tick();
	ms += 1799999;
	tick();
	assert(sleeptimer_gain_q10() == 1024);
	ms += 1501;
	tick();
	assert(sleeptimer_gain_q10() == 512);
	ms += 1499;
	tick();
	assert(sleeptimer_gain_q10() == 1);
	ms++;
	assert(sleeptimer_expired() && sleeptimer_gain_q10() == 0);
	tick();
	sleeptimer_note_playing(2); // Changing inputs starts a fresh countdown.
	tick();
	assert(sleeptimer_gain_q10() == 1024);
	sleeptimer_set_minutes(60);
	assert(sleeptimer_minutes() == 60);
	sleeptimer_set_enabled(false);
	sleeptimer_set_minutes(15);
	sleeptimer_set_scheduled(true);
	sleeptimer_set_window(22 * 60, 7 * 60);
	wall_at(21, 59); tick();
	wall_at(22, 0); tick(); // Window starts with playback already active.
	ms += 901000;
	tick();
	assert(sleeptimer_gain_q10() < 1024);
	wall_at(7, 0); tick();
	assert(sleeptimer_gain_q10() == 1024);
	return 0;
}
