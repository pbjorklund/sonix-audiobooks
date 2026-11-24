#ifndef ABS_SHOW_READER_H
#define ABS_SHOW_READER_H

#include "audiobookshelf.h"

// Parses the one expanded show response without retaining skipped JSON values.
// The read callback returns positive bytes, 0 at EOF, or -1 on failure.
int abs_show_read(int (*read)(void *user, void *buf, int capacity), void *user,
	const char *show_id, abs_episode_row_t **out);

#endif
