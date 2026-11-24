#ifndef ABS_EPISODE_CARD_H
#define ABS_EPISODE_CARD_H

#include "audiobookshelf.h"

#define ABS_EPISODE_TREE "Podcast/ABS"

typedef struct {
	char server[ABS_SERVER_MAX];
	char owner[ABS_ID_MAX];
	char show[ABS_ID_MAX];
	char episode[ABS_ID_MAX];
	char file[ABS_ID_MAX];
	char audio[32];
	char title[ABS_NAME_MAX];
	long long bytes;
	double duration;
} abs_episode_manifest_t;

bool abs_episode_destination(const char *server, const char *owner, const char *show,
	const char *episode, char *out, size_t cap);
const char *abs_episode_extension(const audiobookshelf_track_t *track);
bool abs_episode_filename(const abs_episode_t *episode, char *out, size_t cap);
bool abs_episode_card_dir(const char *relative, char *absolute, size_t cap);
// Card-local identity of the exact last verified profile/key. The key itself
// is used only in memory and is never stored on the card.
bool abs_episode_owner_remember(const abs_auth_snapshot_t *auth, const char *owner);
bool abs_episode_owner_cached(const abs_auth_snapshot_t *auth, char owner[ABS_ID_MAX]);
bool abs_episode_manifest_read(const char *folder, abs_episode_manifest_t *out);
bool abs_episode_manifest_commit(const char *folder, const abs_episode_manifest_t *manifest);
bool abs_episode_download_path(const char *server, const char *owner, const char *show,
	const char *episode, abs_episode_manifest_t *manifest, char *path, size_t cap);
int abs_episode_download_list(const char *server, const char *owner, int offset,
	abs_episode_row_t *out, int cap, bool *more);
bool abs_episode_download_remove(const char *server, const char *owner, const char *show,
	const char *episode, const char *in_use);
bool abs_episode_card_identity(const char *path, abs_episode_manifest_t *out);

#endif
