#define _POSIX_C_SOURCE 200809L
#include "sleeptimer.h"

#include <stdio.h>
#include <time.h>
#include "src/system/core/config.h"

#define SECTION "sleep_timer"
#define FADE_MS 3000

static bool enabled, scheduled, counting;
static int minutes, start_minute, end_minute, playing_source;
static long long deadline;
static int gain_q10 = 1024;

static long long now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static bool valid_duration(long value) { return value == 15 || value == 30 || value == 60; }
static bool valid_minute(long value) { return value >= 0 && value < 1440; }

static void reset(void) {
	counting = false;
	gain_q10 = 1024;
}

void sleeptimer_init(void) {
	// Only migrate the Audiobookshelf preference if no global preference exists.
	// Keep it as the selected duration, but don't unexpectedly arm other audio.
	long duration = config_get_int(SECTION, "minutes", config_get_int("audiobookshelf", "sleep_minutes", 30));
	minutes = valid_duration(duration) ? (int)duration : 30;
	enabled = config_get_int(SECTION, "enabled", 0) != 0;
	scheduled = config_get_int(SECTION, "scheduled", 0) != 0;
	long start = config_get_int(SECTION, "start", 22 * 60);
	long end = config_get_int(SECTION, "end", 7 * 60);
	start_minute = valid_minute(start) ? (int)start : 22 * 60;
	end_minute = valid_minute(end) ? (int)end : 7 * 60;
	playing_source = 0;
	reset();
}

bool sleeptimer_enabled(void) { return enabled; }
int sleeptimer_minutes(void) { return minutes; }
bool sleeptimer_scheduled(void) { return scheduled; }
int sleeptimer_start_minute(void) { return start_minute; }
int sleeptimer_end_minute(void) { return end_minute; }

bool sleeptimer_armed(void) {
	if (enabled) return true;
	if (!scheduled) return false;
	time_t now = time(NULL);
	struct tm local;
	if (now == (time_t)-1 || !localtime_r(&now, &local)) return false;
	int minute = local.tm_hour * 60 + local.tm_min;
	return start_minute < end_minute ? minute >= start_minute && minute < end_minute
		: minute >= start_minute || minute < end_minute;
}

void sleeptimer_set_enabled(bool value) {
	if (enabled == value) return;
	enabled = value;
	config_set_int(SECTION, "enabled", value);
	config_save();
}

void sleeptimer_set_minutes(int value) {
	if (!valid_duration(value) || minutes == value) return;
	minutes = value;
	config_set_int(SECTION, "minutes", value);
	config_save();
	reset();
}

void sleeptimer_set_scheduled(bool value) {
	if (scheduled == value) return;
	scheduled = value;
	config_set_int(SECTION, "scheduled", value);
	config_save();
}

void sleeptimer_set_window(int start, int end) {
	if (!valid_minute(start) || !valid_minute(end) || (start == start_minute && end == end_minute)) return;
	start_minute = start;
	end_minute = end;
	config_set_int(SECTION, "start", start);
	config_set_int(SECTION, "end", end);
	config_save();
}

void sleeptimer_note_playing(int source) {
	if (source == playing_source) return;
	playing_source = source;
	reset();
}

bool sleeptimer_expired(void) {
	if (!playing_source || !sleeptimer_armed()) {
		reset();
		return false;
	}
	long long now = now_ms();
	if (!counting) {
		deadline = now + (long long)minutes * 60000;
		counting = true;
		fprintf(stderr, "sleeptimer: %d min\n", minutes);
	}
	if (gain_q10 == 0) return false;
	long long elapsed = now - deadline;
	if (elapsed >= FADE_MS) {
		gain_q10 = 0;
		return true;
	}
	if (elapsed > 0) gain_q10 = (int)(((FADE_MS - elapsed) * 1024 + FADE_MS - 1) / FADE_MS);
	return false;
}

int sleeptimer_gain_q10(void) { return gain_q10; }
