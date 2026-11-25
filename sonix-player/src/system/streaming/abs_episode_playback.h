#ifndef ABS_EPISODE_PLAYBACK_H
#define ABS_EPISODE_PLAYBACK_H

#include "abs_episode_progress.h"

bool abs_episode_playback_identity(const char *path, abs_episode_manifest_t *out);
void abs_episode_playback_register(const char *path, const abs_episode_manifest_t *id);
double abs_episode_playback_resume(const char *path);
// Resolve only the same source version of the same account's active episode.
bool abs_episode_playback_local(const char *source_path, char *local, size_t cap);
void abs_episode_playback_did_switch(const char *source_path, const char *local);
bool abs_episode_playback_switched(const char *source_path);
bool abs_episode_playback_local_in_use(const char *source_path, char *local, size_t cap);
void abs_episode_playback_note(const char *path, double seconds, double duration, bool force, bool finished);
bool abs_episode_playback_active(const abs_episode_manifest_t *id);
void abs_episode_playback_deactivate(void);
bool abs_episode_mark(const abs_episode_manifest_t *id, bool played);

#endif
