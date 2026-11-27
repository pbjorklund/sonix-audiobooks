#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "src/system/audio/singleear.h"

int main(void) {
	const int16_t stereo[] = {1200, 400, -600, 100, INT16_MAX, INT16_MAX, INT16_MIN, INT16_MIN};
	const int16_t left[] = {800, 0, -250, 0, INT16_MAX, 0, INT16_MIN, 0};
	const int16_t right[] = {0, 800, 0, -250, 0, INT16_MAX, 0, INT16_MIN};
	int16_t routed[8];
	assert(singleear_get() == SINGLEEAR_STEREO);
	singleear_cycle();
	assert(singleear_get() == SINGLEEAR_LEFT);
	memcpy(routed, stereo, sizeof(routed));
	singleear_s16(singleear_get(), routed, routed, 4, 2);
	assert(!memcmp(routed, left, sizeof(routed)));
	singleear_cycle();
	assert(singleear_get() == SINGLEEAR_RIGHT);
	singleear_s16(singleear_get(), stereo, routed, 4, 2);
	assert(!memcmp(routed, right, sizeof(routed)));
	singleear_cycle();
	assert(singleear_get() == SINGLEEAR_STEREO);
	singleear_s16(singleear_get(), stereo, routed, 4, 2);
	assert(!memcmp(routed, stereo, sizeof(routed)));

	const int32_t wide[] = {INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN, 0, 50000};
	const int32_t wide_left[] = {INT32_MAX, 0, INT32_MIN, 0, 25000, 0};
	int32_t output[6];
	singleear_s32(SINGLEEAR_LEFT, wide, output, 3, 2);
	assert(!memcmp(output, wide_left, sizeof(output)));

	const int32_t mono[] = {INT32_MIN, 58, 0};
	const int32_t mono_stereo[] = {INT32_MIN, INT32_MIN, 58, 58, 0, 0};
	const int32_t mono_right[] = {0, INT32_MIN, 0, 58, 0, 0};
	singleear_s32(SINGLEEAR_STEREO, mono, output, 3, 1);
	assert(!memcmp(output, mono_stereo, sizeof(output)));
	singleear_s32(SINGLEEAR_RIGHT, mono, output, 3, 1);
	assert(!memcmp(output, mono_right, sizeof(output)));
	puts("Single-ear audio: three-state cycle, S16/S32 downmix, inactive-ear silence, and mono expansion passed");
}
