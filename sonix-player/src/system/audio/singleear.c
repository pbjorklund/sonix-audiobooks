#include "singleear.h"

#include <stdatomic.h>

// This is an override, not a change to the user's saved mono or balance.
// Start in stereo after a reboot.
static atomic_int mode;

singleear_mode_t singleear_get(void) {
	return atomic_load_explicit(&mode, memory_order_relaxed);
}

void singleear_cycle(void) {
	atomic_store_explicit(&mode, (singleear_get() + 1) % 3, memory_order_relaxed);
}

#define ROUTE(name, sample, wide) \
void name(singleear_mode_t selected, const sample *in, sample *out, int count, int channels) { \
	for (int i = 0; i < count; i++) { \
		sample left = in[i * channels]; \
		sample right = channels == 2 ? in[i * 2 + 1] : left; \
		if (selected != SINGLEEAR_STEREO) { \
			sample mono = (sample)(((wide)left + right) / 2); \
			left = selected == SINGLEEAR_LEFT ? mono : 0; \
			right = selected == SINGLEEAR_RIGHT ? mono : 0; \
		} \
		out[i * 2] = left; \
		out[i * 2 + 1] = right; \
	} \
}
ROUTE(singleear_s16, int16_t, int32_t)
ROUTE(singleear_s32, int32_t, int64_t)
