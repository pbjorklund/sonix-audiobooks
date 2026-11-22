#include <string.h>

#include "pathlimits.h"

// Compile-time guard on the relationship between the two ceilings: the durable
// row must be strictly narrower than the on-card ceiling, otherwise
// sonix_linked_path_representable() would accept paths the database cannot hold
// and the whole point of the shared check disappears.
_Static_assert(SONIX_LINKED_DB_PATH_MAX < SONIX_LINKED_PATH_MAX,
			   "durable-row path limit must be narrower than the on-card limit");
_Static_assert(SONIX_LINKED_SIDECAR_PATH_MAX > SONIX_LINKED_PATH_MAX,
			   "sidecar allowance must cover the audio path plus its suffix");

// The single decision point for "can every linked-media consumer carry this
// path?". Kept in one place so a future widening of the narrowest consumer
// changes the answer everywhere at once.
//
// A durable row (char book[512]) and the progress watch slot must hold the
// path plus one terminator, and the on-card ceiling must not be exceeded.
bool sonix_linked_path_representable(size_t len) {
	return len > 0 && len < SONIX_LINKED_PATH_MAX && len <= (size_t)(SONIX_LINKED_DB_PATH_MAX - 1);
}

// The download-side gate: true when `path` is non-empty and every linked-media
// consumer can represent it. Call this before the first byte of a new book is
// written; a destination that fails here must be rejected, not downloaded and
// then truncated on resume or sync (§11.5).
bool sonix_linked_destination_ok(const char *path) {
	if (!path || !path[0]) return false;
	return sonix_linked_path_representable(strlen(path));
}
