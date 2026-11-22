#ifndef PATHLIMITS_H
#define PATHLIMITS_H

#include <stdbool.h>
#include <stddef.h>

// One ceiling for every path that names linked (Audiobookshelf-bound) media.
//
// Before this header each consumer picked its own constant -- the ABS download
// used 1280, the progress worker's watch slot used 512, the SQLite rows
// allocate 512 -- and a download longer than one consumer's buffer produced a
// file it could download but never resume or sync. The spec (§11.5) forbids
// "successful download followed by path-truncation sync failure": everything
// below checks against one shared ceiling instead, and a destination that any
// downstream consumer could not represent is rejected before the first byte is
// written.
//
// The audio/manifest/sidecar path on the card. This is what
// audiobookshelf_download() builds and what the scanner, playback, metadata and
// sync layers all read back.
#define SONIX_LINKED_PATH_MAX 1280

// The narrowest downstream consumer that must represent a full linked path.
// AUDIOBOOK_TABLE/AUDIOBOOK_ABS_SYNC rows (char book[512]/file[512]) and the
// progress worker's watch/force slots are the binding ones; a path that cannot
// fit here would download fine and then silently fail to resume or sync.
#define SONIX_LINKED_DB_PATH_MAX 512

// Sidecar/manifest variants derive from the audio path with a suffix appended
// (".tags", ".part", ".tmp"); 1310 kept to the historical allowance.
#define SONIX_LINKED_SIDECAR_PATH_MAX (SONIX_LINKED_PATH_MAX + 30)

// True when `len` path bytes are representable by every linked-media consumer:
// the on-card ceiling, and -- because the checkpoint row and the watch slots
// carry the same string -- the durable-row ceiling as well. A destination that
// fails this must be rejected before any network or card write.
bool sonix_linked_path_representable(size_t len);

// The download-side gate: true when `path` is non-empty and representable by
// every consumer above. Call it before the first byte of a new book is written;
// a destination that fails here must be rejected, not downloaded and then
// truncated on resume or sync (§11.5).
bool sonix_linked_destination_ok(const char *path);

#endif /* PATHLIMITS_H */
