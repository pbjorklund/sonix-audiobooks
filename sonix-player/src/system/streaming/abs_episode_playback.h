#ifndef ABS_EPISODE_PLAYBACK_H
#define ABS_EPISODE_PLAYBACK_H

#include "abs_episode_progress.h"

bool abs_episode_playback_identity(const char *path, abs_episode_manifest_t *out);
void abs_episode_playback_register(const char *path, const abs_episode_manifest_t *id);
double abs_episode_playback_resume(const char *path);
void abs_episode_playback_note(const char *path, double seconds, double duration, bool force, bool finished);
bool abs_episode_playback_active(const abs_episode_manifest_t *id);
void abs_episode_playback_deactivate(void);
bool abs_episode_mark(const abs_episode_manifest_t *id, bool played);

#endif
