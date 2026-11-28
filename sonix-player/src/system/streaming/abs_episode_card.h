#ifndef ABS_EPISODE_CARD_H
#define ABS_EPISODE_CARD_H

#include "audiobookshelf.h"

#define ABS_EPISODE_TREE "Podcast/ABS"
#define ABS_EPISODE_COVER ".abs-show.jpg"

typedef struct {
	char server[ABS_SERVER_MAX];
	char owner[ABS_ID_MAX];
	char show[ABS_ID_MAX];
	char episode[ABS_ID_MAX];
	char file[ABS_ID_MAX];
	char audio[32];
	char title[ABS_NAME_MAX];
	char show_title[ABS_NAME_MAX];
	long long published_ms;
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
// A NULL show lists one row per show. A show ID lists episodes matching the
// ABS_EP_* state mask, filtered before pagination using saved checkpoints.
int abs_episode_download_list(const char *server, const char *owner, const char *show, unsigned states, int offset,
	abs_episode_row_t *out, int cap, bool *more);
bool abs_episode_download_remove(const char *server, const char *owner, const char *show,
	const char *episode, const char *in_use);
typedef struct {
	unsigned generation;
	char server[ABS_SERVER_MAX], owner[ABS_ID_MAX], show[ABS_ID_MAX];
	bool finished_only;
	int count;
	struct { char show[ABS_ID_MAX], episode[ABS_ID_MAX]; } *episodes;
} abs_episode_removal_t;
// Snapshot the exact downloads offered for confirmation. An empty show is
// allowed only for played-only cleanup across all shows. Caller frees episodes.
bool abs_episode_removal_plan(abs_episode_removal_t *plan, const char *in_use);
// Returns the number removed, or -1 if the card changed before starting.
int abs_episode_removal_apply(const abs_episode_removal_t *plan, const char *in_use);
bool abs_episode_card_identity(const char *path, abs_episode_manifest_t *out);
// For a validated on-card episode audio path, find its optional show artwork.
bool abs_episode_local_cover(const char *audio, char *out, size_t cap);

#endif
