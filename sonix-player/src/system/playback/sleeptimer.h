#ifndef SLEEPTIMER_H
#define SLEEPTIMER_H

#include <stdbool.h>

void sleeptimer_init(void);
bool sleeptimer_enabled(void);
void sleeptimer_set_enabled(bool enabled);
int sleeptimer_minutes(void);
void sleeptimer_set_minutes(int minutes);
bool sleeptimer_scheduled(void);
void sleeptimer_set_scheduled(bool scheduled);
int sleeptimer_start_minute(void);
int sleeptimer_end_minute(void);
void sleeptimer_set_window(int start, int end);

// Minutes since midnight in local time; equal endpoints mean a full day.
bool sleeptimer_armed(void);
// Source 0 is not playing. A pause, source change or window opening starts
// a new countdown. Leaving the window cancels the countdown and fade.
void sleeptimer_note_playing(int source);
// Advance the countdown and return true once, after the three-second fade.
bool sleeptimer_expired(void);
int sleeptimer_gain_q10(void);

#endif
