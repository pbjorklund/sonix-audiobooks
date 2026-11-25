#ifndef ABS_SHOW_READER_H
#define ABS_SHOW_READER_H

#include "audiobookshelf.h"

// Parses the one expanded show response without retaining skipped JSON values.
// The read callback returns positive bytes, 0 at EOF, or -1 on failure.
int abs_show_read(int (*read)(void *user, void *buf, int capacity), void *user,
	const char *show_id, abs_episode_row_t **out);

// Projects a window of the show without changing the row identities.
typedef enum { ABS_EPISODE_RELEASED, ABS_EPISODE_TITLE, ABS_EPISODE_ADDED } abs_episode_sort_t;
void abs_episode_sort(abs_episode_row_t *rows, int count, abs_episode_sort_t sort);
// First two unfinished episodes in release order, regardless of list sort.
int abs_episode_next_pair(const abs_episode_row_t *rows, int count, bool oldest, int picks[2]);
bool abs_show_read_title(int (*read)(void *, void *, int), void *user, const char *show_id, char title[ABS_NAME_MAX]);
enum {
	ABS_EP_UNPLAYED = 1, ABS_EP_STARTED = 2, ABS_EP_FINISHED = 4,
	ABS_EP_ALL = ABS_EP_UNPLAYED | ABS_EP_STARTED | ABS_EP_FINISHED,
};
int abs_episode_project(const abs_episode_row_t *rows, int count, bool reverse, unsigned states,
	int skip, int *indices, int cap);

#endif
