#ifndef TIDALCACHE_H
#define TIDALCACHE_H

#include <stdbool.h>
#include <stddef.h>

#include "src/system/streaming/tidal.h"

// Tidal tracks downloaded to the card and then played like any other file.
//
// The twin of qobuzcache.h, and the reasons for downloading instead of decoding
// on the fly are the same: decoder_open() wants a path, seeking inside a track
// requires going backwards, and with the file on disk the queue, the equalizer,
// ReplayGain, chapters and the progress bar work with no extra code. See
// qobuzcache.h for the full reasoning.
//
// Separate directory, and not for tidiness: the two services number tracks
// independently, and nothing prevents track 12345678 existing on both. In a
// single directory the second would find the first's file, decide it already
// had it and play the wrong song -- a silent failure of the kind that looks
// like witchcraft. Distinct directories make that impossible by construction,
// and give qobuzcache_owns()/tidalcache_owns() a clean answer about who owns a
// path: that is how the player knows which badge to draw and which account to
// send the star to.
//
// What is here and not in Qobuz: segments. Qobuz answers with a URL and one
// file is downloaded. Tidal, in hi-res, answers with a list of MP4 segments to
// reassemble. That work lives in mp4flac.c; here is the loop that requests them
// one by one and feeds them in. From outside nothing changes: a growing .flac
// comes out, and whatever plays it neither knows nor needs to know where it
// came from.

#define TIDALCACHE_DIR ".local/tidal-cache"

// The same ceiling as Qobuz. The two count separately, so in the worst case --
// using both on the same day -- they take twice as much. That is deliberate:
// they are two transient caches that empty themselves, and a shared budget
// would mean listening to Tidal deletes what was being listened to on Qobuz.
#define TIDALCACHE_MAX_BYTES (1024L * 1024L * 1024L)

// Where the card is. Once at startup and on every card change; NULL or empty
// disables the cache (and with it Tidal, which cannot play anything without
// it).
void tidalcache_set_root(const char *sd_root);
bool tidalcache_ready(void);

// The path this track would have. Says nothing about whether it exists.
void tidalcache_path(long track_id, const char *mime, char *out, size_t size);

// True if the track has been downloaded in full.
bool tidalcache_has(long track_id, const char *mime, char *out, size_t size);

// Like tidalcache_has, but without knowing the format: tries every extension
// this cache may have used. Needed by callers that queue a track before asking
// Tidal for it -- the name is guessed from the selected quality, and if Tidal
// sent a different container the file is still there under another name. It
// matters more here than on Qobuz: which container arrives depends on the
// manifest, and the manifest is only seen at play time.
bool tidalcache_find(long track_id, char *out, size_t size);

// Starts downloading the track described by `stream` and puts the local path in
// `out`.
//
// Returns as soon as there is enough material for the decoder to open the file;
// the rest keeps downloading on its own thread, and the caller can hand the
// path to playback immediately. A track already in the cache returns
// immediately without touching the network.
//
// Blocks for the duration of the prebuffer, so call it from a worker thread,
// never from the GUI thread.
//
// `duration_secs` is the duration according to the API: it gives the bytes per
// second playback will consume, and hence how much must be in hand before
// starting. 0 when unknown.
//
// `stream` is copied: the caller can free its own as soon as this returns.
bool tidalcache_start(long track_id, const tidal_stream_t *stream, int duration_secs, char *out, size_t size);

// Fetches a fresh file into an isolated staging path and waits for completion.
// complete_transfer means the byte count or segment count was known and matched.
bool tidalcache_download(long track_id, const tidal_stream_t *stream, char *out, size_t size, bool *complete_transfer);

// The extension this stream's file will have, without the dot. Gives the name
// before the download begins.
const char *tidalcache_extension(const tidal_stream_t *stream);

// Waits until no download is in progress. Only one runs at a time, for the same
// reason as Qobuz: a caller prefetching the next track calls this before
// starting, otherwise it competes for network and card with the track being
// listened to right now.
//
// The turn is shared with qobuzcache: it is the network and the card that are
// single, not the service. See streamturn.h.
void tidalcache_wait_idle(void);

// Called when the prebuffer will take more than a moment, with the estimate in
// seconds. Called from the download thread: the receiver must bounce to the GUI
// thread itself.
void tidalcache_set_slow_start_cb(void (*cb)(int seconds));

// Abandons downloads in progress. Call when switching to another track: the
// previous one is no longer needed and the bandwidth belongs to this one.
void tidalcache_abandon_all(void);

// The id of the track downloading right now, 0 if none.
long tidalcache_downloading_id(void);

// Active track progress (0–99), or -1 if the size is unknown or it is not transferring.
int tidalcache_download_percent(long track_id);

// "Tidal needs the network", even when nothing is downloading at this instant.
// Needed by the Wi-Fi parking logic (power.c): see the long comment on
// qobuzcache_set_network_wanted().
void tidalcache_set_network_wanted(bool wanted);
bool tidalcache_network_wanted(void);

// True if `path` is inside the Tidal cache.
bool tidalcache_owns(const char *path);

// The cache directory, or NULL with no card.
const char *tidalcache_dir(void);

// Writes beside the track what the API knows and the file does not: title,
// artist, album (in `<file>.tags`) and the cover (in `<stem>.jpg`).
//
// This matters more than on Qobuz, not less: a FLAC stitched back from DASH
// segments has no tags at all, because the tags live in the metadata blocks the
// MP4 does not carry. Without this sidecar the player would show
// "12345678.flac" for every hi-res track.
void tidalcache_write_sidecars(const tidal_track_t *track, const char *mime, const char *cover_url);

// The Tidal album id the track belongs to, read back from the sidecar. False if
// the path is not from this cache or the sidecar does not say. Used by "Show
// album" in the player.
bool tidalcache_album_id(const char *path, char *out, size_t size);

// Why the last tidalcache_start() failed, as a sentence fit to display. Empty
// on success. Per thread, like http_last_error().
const char *tidalcache_last_error(void);

// The Tidal track id from the local path, 0 if that path is not a track of this
// cache. The file name is the id ("12345678.flac"): a playing file always maps
// back to the Tidal track, which is what lets the player's favourite star and
// playlists reach the account, and the Tidal badge appear on the cover.
long tidalcache_track_id(const char *path);

// How many tracks between automatic cache sweeps.
#define TIDALCACHE_CLEAR_EVERY 10

// Call for every track that enters the cache, with its path.
void tidalcache_note_played(const char *path);

#define TIDALCACHE_PROTECTED_MAX 64

// The tracks the queue expects to play: the current one and those after it. The
// automatic sweep skips them. Call from the GUI thread.
void tidalcache_set_protected(const char *const *paths, int count);

// Call on the way out (shutdown, reboot).
void tidalcache_clear_on_exit(void);

// Current size, and how to empty it (kept tracks survive this too).
long tidalcache_bytes(void);
void tidalcache_clear(void);

// ---------------------------------------------------------------------------
// kept downloads
//
// The cache is transient by design: it empties on exit and every so many
// tracks while running. A kept track is exempt from all of that -- it stays
// on the card until it is explicitly removed, and it is what the Downloads
// list plays from. The marker is "<id>.keep": keyed to the track id, so the
// tags and cover sidecars (which share the id) are covered by the same
// exemption, and a container change (flac named m4a by a manifest) does not
// orphan it.
// ---------------------------------------------------------------------------

// Publishes completed audio and metadata under Music/Tidal. Idempotent.
bool tidalcache_keep(const tidal_track_t *track, const char *path, bool complete_transfer);
enum {
	TIDALCACHE_QUALITY_NONE = -3,
	TIDALCACHE_QUALITY_PROTECTED = -2, // linked imports or legacy cache-only downloads
	TIDALCACHE_QUALITY_UNKNOWN = -1,  // Sonix-owned download made before quality was recorded
};
int tidalcache_kept_quality(long track_id);
bool tidalcache_keep_quality(const tidal_track_t *track, const char *path, bool complete_transfer,
	int quality, const char *replace_path, bool playlist_only);
bool tidalcache_is_playlist_only(long track_id);
bool tidalcache_promote_playlist_track(const tidal_track_t *track);
bool tidalcache_has_album_folder(const tidal_album_t *album);
// Link identifiable existing files without opening audio or taking ownership.
int tidalcache_link_album_tracks(const tidal_album_t *album, const tidal_track_t *tracks, int count);
bool tidalcache_reuse(const tidal_track_t *track);
void tidalcache_download_extras(const tidal_track_t *track);
// Refresh artwork for Sonix-owned downloads without replacing existing lyrics
// or artwork next to imported audio.
void tidalcache_refresh_extras(const tidal_track_t *track);

// Moves previously kept cache files into Music/Tidal without redownloading.
void tidalcache_publish_legacy(void);

// True if this track is marked kept and its audio file is complete.
bool tidalcache_is_kept(long track_id);

// The kept tracks on the card, `max` at a time from `offset`, sorted by
// album and track number so an album's tracks stay in order in the Downloads
// list. Returns the number filled (0 at the end of the list).
int tidalcache_kept_list(tidal_track_t *out, int max, int offset);
int tidalcache_kept_filtered(tidal_track_t *out, int max, int offset, const char *album, const char *artist);
int tidalcache_kept_groups(tidal_album_t *out, int max, int offset, bool artists, const char *artist);
bool tidalcache_kept_track(long id, tidal_track_t *out);

// Count complete, kept tracks by album ID in one scan of the card.
void tidalcache_kept_album_counts(const char *const *album_ids, int *counts, int count);

// Deletes a track's files and sidecars and lifts its keep marker.
bool tidalcache_remove_track(long track_id);

// Drops least-recently played NON-KEPT tracks until `bytes` fit under the
// cache ceiling. Kept tracks are never dropped here.
void tidalcache_make_room(long bytes);

#endif /* TIDALCACHE_H */
