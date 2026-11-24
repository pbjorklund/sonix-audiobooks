#ifndef ABS_EPISODE_STREAM_H
#define ABS_EPISODE_STREAM_H

#include "abs_episode_playback.h"

// Returns once a usable audio prefix is on the card (or the file is complete
// for containers that need seekable input). The transfer may continue.
bool abs_episode_stream_start(const abs_auth_snapshot_t *auth, const abs_episode_t *episode,
	double resume, const char *in_use, http_op_t *request, char *path, size_t cap);
void abs_episode_stream_cancel(void);
void abs_episode_stream_quiesce(void);
bool abs_episode_stream_owns(const char *path);
bool abs_episode_stream_complete(const char *path);

#endif
