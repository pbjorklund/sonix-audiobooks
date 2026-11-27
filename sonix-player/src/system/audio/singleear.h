#ifndef SINGLEEAR_H
#define SINGLEEAR_H

#include <stdint.h>

typedef enum { SINGLEEAR_STEREO, SINGLEEAR_LEFT, SINGLEEAR_RIGHT } singleear_mode_t;

singleear_mode_t singleear_get(void);
void singleear_cycle(void);
// Stereo: operate in-place or into a separate stereo buffer. Mono: expand
// into a separate stereo buffer so the unused ear can actually be silent.
void singleear_s16(singleear_mode_t mode, const int16_t *in, int16_t *out, int count, int channels);
void singleear_s32(singleear_mode_t mode, const int32_t *in, int32_t *out, int count, int channels);

#endif
