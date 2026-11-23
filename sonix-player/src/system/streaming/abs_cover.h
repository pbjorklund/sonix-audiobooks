#ifndef ABS_COVER_H
#define ABS_COVER_H

#include <stdbool.h>
#include <stddef.h>

#include "abs_profiles.h" // abs_auth_snapshot_t

// ---------------------------------------------------------------------------
// The authenticated Audiobookshelf cover cache (spec §9.1 cover endpoint and
// §9.3 cache ownership).
//
// The detail screen needs the tapped book's artwork, and ABS serves that only
// behind its Bearer token:
//
//     GET /api/items/{itemId}/cover?format=jpeg&width=900&height=1280
//
// The reviewed route supports width/height/format at both fixture revisions.
// The server scales its source to this size; the player then decodes the file
// into its own 450x640-shape contain box (§9.5 -- screen-fit, never
// original-resolution pan/zoom). abs_cover_fetch() streams the response
// straight to a cache file under the card so a re-opened book costs one file
// read and the LVGL-facing decode always reads a local path, exactly like the
// queue's media transfers stream to disk.
//
// The cache is display-only scratch: <card>/.local/abs-cover-cache/, at most
// ABS_COVER_FILES_MAX files and ABS_COVER_BYTES_MAX bytes, the
// least-recently-used evicted. Nothing local ever reads it back except the
// image decoders, so removing entries costs nothing but one refetch.
//
// Ownership and safety rules this module enforces (§9.3):
//
//   * The caller resolves the auth snapshot (audiobookshelf_browse_credentials(),
//     §11.3 -- the browsing identity api_request() itself would have used) and
//     hands it in; the token never appears in a filename, in an error, or on
//     the log. File names are two SHA-256 truncations: the normalized server
//     (the manifest identity, §11.5) and the item id.
//   * Same-origin by construction: the request URL's authority IS the
//     snapshot's server -- nothing server-supplied ever contributes a
//     component of it -- and http.c drops that Authorization header across a
//     cross-origin redirect hop, so an object-storage hop for the cover is
//     followed without ever disclosing the token to another origin. The queue
//     additionally re-checks URLs that arrive FROM the server (§11.3 defense
//     in depth); nothing here arrives from the server to re-check.
//   * The encoded response is capped at ABS_COVER_ENCODED_MAX bytes (§9.1:
//     2 MiB). Content-Length over the cap is refused before a byte is
//     written; a body that grows past the cap mid-stream aborts the write.
//     The transfer lands in ".part", closes checked, and is atomically
//     renamed only when complete (§8.2 step 4 semantics, borrowed from the
//     queue's file transfers).
//   * Cancellation: the caller's cancel flag is polled between read units,
//     exactly like the queue's ops; a cancelled fetch leaves no file.
//   * Card close: a card removal quiesces the module before the mount goes
//     down; fetches refuse until the card attaches again.
// ---------------------------------------------------------------------------

// Where the cache lives on the card, next to the other streaming caches.
#define ABS_COVER_CACHE_DIR ".local/abs-cover-cache"

// §9.3: keep eight files / at most 16 MiB, evicting least-recently-used.
#define ABS_COVER_FILES_MAX 8
#define ABS_COVER_BYTES_MAX (16 * 1024 * 1024)

// §9.1: the encoded response is limited to 2 MiB and streamed to a cache
// file. Both the Content-Length and the running byte count are held under it.
#define ABS_COVER_ENCODED_MAX (2 * 1024 * 1024)

// The cached path must be representable by every downstream consumer of it:
// the coverloader slot holds 512 bytes and the detail/fullscreen viewers
// copy a path of at most this length. A longer card root is a refusal BEFORE
// any write, never a truncated name decoded by accident (§11.5's path
// ownership rule applied to this cache).
#define ABS_COVER_PATH_MAX 512

// Point-of-use network timeout. The metadata requests use the shared
// ABS_TIMEOUT_SECS; the cover is smaller but the same patience applies.
#define ABS_COVER_TIMEOUT_SECS 15

// Stores the card root the cache lives on. Call at boot and on every card
// attach, like the other streaming caches' *_set_root(). Clears quiescence.
// NULL/empty only marks the module unconfigured (fetches refuse).
void abs_cover_set_root(const char *card_root);

// Card removal (§8.5 same call site as the download queue's storage
// quiesce): fetches refuse until the next set_root, so no fetch can race the
// mount going away.
void abs_cover_quiesce(void);

// Downloads `item_id`'s cover from the snapshot's server into the cache and
// receives the cached file path. The snapshot is caller-pinned and wiped by
// the caller afterwards; `cancel` (NULL allowed) is polled between read
// units; `error` receives a short sanitized English reason (never a URL or a
// token) on failure. True when `path_out` holds a usable, local, complete
// cover file. A failure leaves the §9.2 text-only detail; it never fails the
// metadata job that already succeeded.
bool abs_cover_fetch(const abs_auth_snapshot_t *auth, const char *item_id,
					 const volatile int *cancel, char *path_out, size_t path_size,
					 char *error, size_t error_size);

#endif /* ABS_COVER_H */
