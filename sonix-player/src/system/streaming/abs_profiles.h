#ifndef ABS_PROFILES_H
#define ABS_PROFILES_H

#include <stdbool.h>
#include <stddef.h>

// ---------------------------------------------------------------------------
// URL + token profiles (spec §11.1, ADR-5)
//
// One active browsing profile, up to four configured profiles, and a durable
// atomic store in userdata. The normalized server URL is the sync identity
// every manifest binds to; the profile id is locally opaque and never leaves
// this device. Tokens exist here, in short-lived auth snapshots handed to
// abs_client, and nowhere else.
//
// The progress worker reconciles every configured profile serially; switching
// the active profile is a browsing change only. See §11.4.
// ---------------------------------------------------------------------------

#define ABS_PROFILE_MAX 4
#define ABS_PROFILE_ID_MAX 17    // "p" + up to 15 digits + terminator
#define ABS_PROFILE_NAME_MAX 64
#define ABS_SERVER_MAX 512       // normalized URL, not a display label
#define ABS_TOKEN_MAX 1024

typedef struct {
	char id[ABS_PROFILE_ID_MAX];       // local opaque ID, never sync identity
	char name[ABS_PROFILE_NAME_MAX];
	char server[ABS_SERVER_MAX];       // normalized URL, authoritative identity
	char token[ABS_TOKEN_MAX];         // userdata only; never logged or synced
} abs_profile_t;

// Immutable credentials for one network operation. Copied under the profile
// lock, carried for the job's lifetime, and zeroed when done. GUI code passes
// profile ids only -- snapshots must not reach the LVGL thread.
typedef struct {
	char profile_id[ABS_PROFILE_ID_MAX];
	char server[ABS_SERVER_MAX];
	char token[ABS_TOKEN_MAX];
} abs_auth_snapshot_t;

// Loads (or migrates) the profile store. Must be called before
// audiobookshelf_init(); see main.c. Returns the number of stored profiles or
// -1 when a persisted file could not be read (the store then behaves as
// unconfigured -- the file is kept on disk so the cause is diagnosable).
int abs_profiles_init(void);

int abs_profiles_list(abs_profile_t *out, int max);
int abs_profiles_count(void);

// Copies the active profile id ("" when none). False when no active profile.
bool abs_profiles_active(char *id_out, size_t id_size);

// Marks `id` active and persists. Browsing-only change: no manifest or
// pending checkpoint is touched.
bool abs_profiles_select(const char *id);

// Adds a profile. `server` is normalized+validated (http/https, no userinfo,
// no fragment, no control bytes, no duplicate normalized URL). `id_out` (may
// be NULL) receives the new id. Fails at four profiles. Over-length display
// names (>= 64 bytes) or tokens (>= 1024 bytes) are rejected whole -- like the
// file parser, never silently truncated (§11.1).
bool abs_profiles_add(const char *name, const char *server, const char *token,
					  char *id_out, size_t id_size);

// Edits display name and/or token of an existing profile. The server URL is
// immutable after creation (§11.2); pass NULL to keep a value. A blank token
// retains the stored one, because with an immutable URL there is no
// "reuse the old host's token" ambiguity left to remove. Over-length values
// are rejected whole, never silently truncated (§11.1).
bool abs_profiles_edit(const char *id, const char *name, const char *token);

// Blocks while a download queue references the profile (none exists yet in
// 0.6.0 M1, so it always succeeds through the queue check); existing linked
// media is never touched. Selecting another profile afterwards is allowed.
bool abs_profiles_remove(const char *id);

// Store-change notification for consumers that mirror profile identity in
// their own RAM (§12.4 line 1201: the per-profile sync diagnostics clear on
// profile replacement). Fired after every add/edit/remove and after a factory
// reset — never for selecting a different active profile (an activation
// replaces no profile) and never for init. The callback runs on the calling
// thread, outside the store lock, with a NULL-terminated contract: `fn` may
// be unregistered by passing NULL afterwards. `user` rides through verbatim.
typedef void (*abs_profiles_store_fn)(void *user);
void abs_profiles_set_store_listener(abs_profiles_store_fn fn, void *user);

// Copies `id`'s credentials into `out` under the lock. False when unknown or
// unconfigured. Callers zero the snapshot after use.
bool abs_profiles_snapshot(const char *id, abs_auth_snapshot_t *out);

// Same, for the active profile.
bool abs_profiles_snapshot_active(abs_auth_snapshot_t *out);

// One snapshot per configured profile, in store order, for the sync worker's
// round-robin pass. Returns how many were filled.
int abs_profiles_snapshots_all(abs_auth_snapshot_t *out, int max);

// True when at least one profile carries a server and a token. This is what
// progress sync waits on; an active-but-incomplete profile does not count.
bool abs_profiles_any_configured(void);

// True when `server` (already normalized, or normalized here) matches some
// configured profile's normalized URL. This is the multi-profile reading of
// ABS_SYNC_OTHER_SERVER: "no configured profile covers this manifest server".
bool abs_profiles_has_server(const char *server);

// The original normalized 0.5.0 server, persisted by migration. Legacy
// manifests without `server=` resolve here -- never to whichever profile
// happens to be active. Empty when unknowable (then relinking is required and
// local progress stays pending).
void abs_profiles_legacy_server(char *out, size_t size);

// Normalization shared with manifests: trim outer whitespace and trailing
// slashes. No port/host canonicalization -- that would rebind old manifests.
void abs_profiles_normalize_server(const char *server, char *out, size_t size);

// Full validation of a candidate server URL for new/edited profiles: http(s)
// scheme, no control bytes, no userinfo, no fragment, normalized into
// `normalized_out`. `reason_out` receives a short English reason on failure.
bool abs_profiles_server_valid(const char *server, char *normalized_out, size_t normalized_size,
							   char *reason_out, size_t reason_size);

// Zeroes a snapshot's credential bytes once a job finished with it. The
// volatile sink keeps the pass from being optimized away.
void abs_profiles_snapshot_wipe(abs_auth_snapshot_t *snap);

// Factory reset (§11.6): locks the store, clears it (no new snapshot hands out
// a credential again), and unlinks both the profile file and its temporary, so
// no 0600 token-bearing file survives the reset. The legacy [audiobookshelf]
// config keys are cleared the same way migration does, or the next boot would
// re-migrate the credentials. No physical secure erase is claimed for UBIFS.
// Called by the device's factory reset; also callable from tests. On a host
// build without an injected path, nothing is written to disk by design.
void abs_profiles_factory_reset(void);

// Storage location override for host tests; device builds use
// /usr/data/audiobookshelf_profiles.ini.
#define ABS_PROFILES_FILE "/usr/data/audiobookshelf_profiles.ini"
#define ABS_PROFILES_FILE_MAX_BYTES 8192
#define ABS_PROFILES_LINE_MAX_BYTES 2048

#endif /* ABS_PROFILES_H */
