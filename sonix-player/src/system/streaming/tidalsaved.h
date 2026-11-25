#ifndef TIDALSAVED_H
#define TIDALSAVED_H

#include "tidal.h"

// A device-local collection stored on the selected card. No network calls.
bool tidalsaved_has(const char *root, const char *id);
bool tidalsaved_set(const char *root, const tidal_album_t *album, bool saved);
// Returns zero for a new/empty collection, or -1 if the card cannot be read.
int tidalsaved_list(const char *root, int offset, tidal_album_t *out, int max);

bool tidalsaved_playlist_set(const char *root, const char *id, const char *name, const long *tracks, int count);
int tidalsaved_playlists(const char *root, int offset, tidal_playlist_t *out, int max);
int tidalsaved_playlist_tracks(const char *root, const char *id, int offset, long *out, int max);

#endif
