#include "src/system/audio/speed.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static int fill(void *user, short *out, int frames) {
	unsigned *supplied = user;
	memset(out, 0, (size_t)frames * sizeof(*out));
	*supplied += (unsigned)frames;
	return frames;
}

int main(void) {
	speed_t *s = speed_open(1, 44100);
	assert(s && speed_pending_input(NULL) == 0);
	short pcm[4096];
	unsigned supplied = 0;
	uint64_t input = 0;
	assert(speed_pull(s, pcm, 4096, fill, &supplied, &input) == 4096);
	assert(input == supplied && speed_pending_input(s) == 0);
	speed_set_factor(s, 1.5);
	supplied = 0;
	assert(speed_pull(s, pcm, 4096, fill, &supplied, &input) == 4096);
	assert(speed_pending_input(s) > 0 && input + speed_pending_input(s) == supplied);
	speed_reset(s);
	assert(speed_pending_input(s) == 0);
	speed_close(s);
	puts("Speed handoff: pending input counts buffered source frames.");
}
