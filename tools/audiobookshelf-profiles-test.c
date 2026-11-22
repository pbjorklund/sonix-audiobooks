// Focused tests for the 0.6.0 M1 core helpers (spec §11.3-11.6, ADR-5):
//   - abs_profiles: profile store, strict parser/writer, atomic persistence,
//     0.5.0 migration, identity rules, auth snapshots;
//   - pathlimits: the shared linked-media path ceiling.
//
// No mock servers and no fixtures here (those belong to the WS5 integration
// tests); this exercises the store against a real temp directory with ASan/UBSan.

#include "src/system/core/config.h"
#include "src/system/core/pathlimits.h"
#include "src/system/streaming/abs_profiles.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *work; // from argv[1]

static void path_join(char *out, size_t size, const char *name) { snprintf(out, size, "%s/%s", work, name); }

static void write_file(const char *path, const char *text) {
	FILE *f = fopen(path, "wb");
	assert(f);
	fputs(text, f);
	fclose(f);
}

static bool file_exists(const char *path) { return access(path, F_OK) == 0; }

static mode_t file_mode(const char *path) {
	struct stat st;
	assert(stat(path, &st) == 0);
	return (mode_t)(st.st_mode & 0777);
}

// ---------------------------------------------------------------------------
// pathlimits (§11.5/§11.6)
// ---------------------------------------------------------------------------

static void pathlimits_tests(void) {
	// The durable row (char[512] + NUL) is the binding consumer: 511 fits, 512 does not.
	assert(!sonix_linked_path_representable(0));
	assert(sonix_linked_path_representable(1));
	assert(sonix_linked_path_representable(SONIX_LINKED_DB_PATH_MAX - 1)); // 511
	assert(!sonix_linked_path_representable(SONIX_LINKED_DB_PATH_MAX));   // 512
	assert(!sonix_linked_path_representable(SONIX_LINKED_PATH_MAX - 1));  // 1279: fine on card, not in DB

	char path[600];
	memset(path, 'x', sizeof(path) - 1);
	path[sizeof(path) - 1] = '\0';
	assert(!sonix_linked_destination_ok(NULL));
	assert(!sonix_linked_destination_ok(""));
	memset(path, 'x', SONIX_LINKED_DB_PATH_MAX - 1); // exactly 511 chars
	path[SONIX_LINKED_DB_PATH_MAX - 1] = '\0';
	assert(sonix_linked_destination_ok(path));
	path[SONIX_LINKED_DB_PATH_MAX - 1] = 'x'; // 512 chars: fits on card, not in the DB row
	path[SONIX_LINKED_DB_PATH_MAX] = '\0';
	assert(!sonix_linked_destination_ok(path));
}

// ---------------------------------------------------------------------------
// Normalization and validation (§11.2)
// ---------------------------------------------------------------------------

static void normalize_tests(void) {
	char out[ABS_SERVER_MAX];
	abs_profiles_normalize_server("  http://a/b/  ", out, sizeof(out));
	assert(strcmp(out, "http://a/b") == 0);
	abs_profiles_normalize_server("http://a///", out, sizeof(out));
	assert(strcmp(out, "http://a") == 0);
	abs_profiles_normalize_server(NULL, out, sizeof(out));
	assert(out[0] == '\0');
	abs_profiles_normalize_server("", out, sizeof(out));
	assert(out[0] == '\0');
	// Inner whitespace is data, not padding: only the outer edges move.
	abs_profiles_normalize_server("http://a b/", out, sizeof(out));
	assert(strcmp(out, "http://a b") == 0);
}

static bool valid(const char *server, const char *expect_normalized) {
	char normalized[ABS_SERVER_MAX] = "", reason[96];
	bool ok = abs_profiles_server_valid(server, normalized, sizeof(normalized), reason, sizeof(reason));
	if (ok != (expect_normalized != NULL)) {
		fprintf(stderr, "valid(%s) -> %d (%s)\n", server ? server : "(null)", ok, reason);
		return false;
	}
	if (expect_normalized && strcmp(normalized, expect_normalized) != 0) {
		fprintf(stderr, "valid(%s) normalized to '%s', want '%s'\n", server, normalized, expect_normalized);
		return false;
	}
	return true;
}

static void validation_tests(void) {
	assert(valid("http://192.168.1.10", "http://192.168.1.10"));
	assert(valid("https://abs.example.com/path", "https://abs.example.com/path"));
	assert(valid(" http://a/b/ ", "http://a/b")); // trimmed before storage
	assert(valid("http://a/?x=1", "http://a/?x=1"));

	char long_url[700];
	long_url[0] = 'h';
	memcpy(long_url + 1, "ttps://", 6);
	memset(long_url + 7, 'a', sizeof(long_url) - 8);
	long_url[sizeof(long_url) - 1] = '\0';

	// valid(server, NULL) asserts that the candidate is rejected.
	assert(valid("", NULL));
	assert(valid("   ", NULL)); // whitespace only: empty after trim
	assert(valid("ftp://a", NULL));
	assert(valid("HTTP://A", NULL)); // scheme is matched exactly; case variants are a different identity
	assert(valid("http://", NULL));
	assert(valid("http:///path", NULL));
	assert(valid("http://user:pass@host/", NULL));
	assert(valid("http://a/#frag", NULL));
	assert(valid("http://a/\x01b", NULL));
	assert(valid(long_url, NULL)); // 699 chars > ABS_SERVER_MAX - 1
}

// ---------------------------------------------------------------------------
// In-memory store: identity rules, capacity, snapshots (§11.1/§11.3)
// ---------------------------------------------------------------------------

static void memory_store_tests(void) {
	unsetenv("SONIX_AUDIOBOOKSHELF_PROFILES");
	unsetenv("SONIX_AUDIOBOOKSHELF_SERVER");
	unsetenv("SONIX_AUDIOBOOKSHELF_TOKEN");

	assert(abs_profiles_init() == 0); // no file, no config: unconfigured store
	assert(abs_profiles_count() == 0);
	assert(!abs_profiles_any_configured());
	char id[ABS_PROFILE_ID_MAX];
	assert(!abs_profiles_active(id, sizeof(id)) && id[0] == '\0');
	abs_auth_snapshot_t snaps[ABS_PROFILE_MAX];
	assert(abs_profiles_snapshots_all(snaps, ABS_PROFILE_MAX) == 0);
	char legacy[ABS_SERVER_MAX];
	abs_profiles_legacy_server(legacy, sizeof(legacy));
	assert(legacy[0] == '\0');

	char new_id[ABS_PROFILE_ID_MAX];
	assert(abs_profiles_add("Home", "http://a/", "tok-a", new_id, sizeof(new_id)));
	assert(strcmp(new_id, "p1") == 0);
	assert(abs_profiles_count() == 1);
	assert(abs_profiles_active(id, sizeof(id)) && strcmp(id, "p1") == 0);

	abs_profile_t list[ABS_PROFILE_MAX];
	assert(abs_profiles_list(list, ABS_PROFILE_MAX) == 1);
	assert(strcmp(list[0].name, "Home") == 0);
	assert(strcmp(list[0].server, "http://a") == 0); // normalized on the way in
	assert(strcmp(list[0].token, "tok-a") == 0);

	// One profile per normalized URL: exact and slash-variant duplicates both fail.
	assert(!abs_profiles_add("Dup", "http://a/", "t2", NULL, 0));
	assert(!abs_profiles_add("Dup", "http://a///", "t2", NULL, 0));
	assert(abs_profiles_count() == 1);

	// An incomplete profile (no token) is stored but takes no sync pass.
	assert(abs_profiles_add("Work", "http://b", "", new_id, sizeof(new_id)));
	assert(strcmp(new_id, "p2") == 0); // ids keep counting up
	assert(abs_profiles_any_configured()); // p1 still counts
	assert(abs_profiles_snapshots_all(snaps, ABS_PROFILE_MAX) == 1);
	assert(strcmp(snaps[0].profile_id, "p1") == 0 && strcmp(snaps[0].token, "tok-a") == 0);

	// Selection is a browsing-only change; unknown ids are refused.
	assert(abs_profiles_select("p1"));
	assert(abs_profiles_active(id, sizeof(id)) && strcmp(id, "p1") == 0);
	assert(!abs_profiles_select("nope"));

	// Edit: NULL keeps, blank token retains (immutable URL removes the ambiguity).
	assert(abs_profiles_edit("p2", "Bee", ""));
	assert(abs_profiles_edit("p2", NULL, "tok-b"));
	assert(abs_profiles_list(list, ABS_PROFILE_MAX) == 2);
	assert(strcmp(list[1].name, "Bee") == 0);
	assert(strcmp(list[1].server, "http://b") == 0); // server is immutable by construction
	assert(strcmp(list[1].token, "tok-b") == 0);
	assert(abs_profiles_snapshots_all(snaps, ABS_PROFILE_MAX) == 2);
	assert(strcmp(snaps[0].profile_id, "p1") == 0 && strcmp(snaps[1].profile_id, "p2") == 0);

	// Snapshots copy under the lock and can be wiped once the job is done.
	abs_auth_snapshot_t snap;
	assert(abs_profiles_snapshot("p1", &snap));
	assert(strcmp(snap.server, "http://a") == 0 && strcmp(snap.token, "tok-a") == 0);
	memset(&snap, 'A', sizeof(snap)); // poison first: wipe must clear every byte
	abs_auth_snapshot_t zeros;
	memset(&zeros, 0, sizeof(zeros));
	abs_profiles_snapshot_wipe(&snap);
	assert(memcmp(&snap, &zeros, sizeof(snap)) == 0);
	assert(abs_profiles_snapshot_active(&snap) && strcmp(snap.profile_id, "p1") == 0);
	assert(!abs_profiles_snapshot("missing", &snap));

	// Four is the bound; the fifth add fails without disturbing the store.
	assert(abs_profiles_add("Three", "http://c", "t3", NULL, 0));
	assert(abs_profiles_add("Four", "http://d", "t4", NULL, 0));
	assert(!abs_profiles_add("Five", "http://e", "t5", new_id, sizeof(new_id)));
	assert(abs_profiles_count() == 4);

	// Removal: the queue check is a no-op in M1. Removing a non-active profile
	// keeps the active one; removing the active profile leaves none active.
	assert(abs_profiles_select("p4"));
	assert(abs_profiles_remove("p3"));
	assert(abs_profiles_count() == 3);
	assert(abs_profiles_active(id, sizeof(id)) && strcmp(id, "p4") == 0);
	assert(abs_profiles_remove("p4"));
	assert(!abs_profiles_active(id, sizeof(id)));
	assert(abs_profiles_count() == 2);
	assert(!abs_profiles_remove("zzz")); // unknown id refused
}

// ---------------------------------------------------------------------------
// Ephemeral host credentials (§11.1 step 3)
// ---------------------------------------------------------------------------

static void ephemeral_host_tests(void) {
	unsetenv("SONIX_AUDIOBOOKSHELF_PROFILES");
	setenv("SONIX_AUDIOBOOKSHELF_SERVER", "http://envhost/", 1);
	setenv("SONIX_AUDIOBOOKSHELF_TOKEN", "envtok", 1);

	assert(abs_profiles_init() == 1); // single active profile, in memory only
	char id[ABS_PROFILE_ID_MAX];
	assert(abs_profiles_active(id, sizeof(id)) && strcmp(id, "p1") == 0);
	abs_auth_snapshot_t snap;
	assert(abs_profiles_snapshot_active(&snap));
	assert(strcmp(snap.server, "http://envhost") == 0 && strcmp(snap.token, "envtok") == 0);
	assert(abs_profiles_any_configured());

	unsetenv("SONIX_AUDIOBOOKSHELF_SERVER");
	unsetenv("SONIX_AUDIOBOOKSHELF_TOKEN");
}

// ---------------------------------------------------------------------------
// Persistence: round-trip, atomic file mode, id continuity (§11.1)
// ---------------------------------------------------------------------------

static void persistence_tests(void) {
	char prof[512];
	path_join(prof, sizeof(prof), "prof.ini");
	if (file_exists(prof)) unlink(prof);
	setenv("SONIX_AUDIOBOOKSHELF_PROFILES", prof, 1);

	assert(abs_profiles_init() == 0); // no file yet, config not loaded: nothing to migrate
	const char *tricky = "tok with spaces & = signs";
	char new_id[ABS_PROFILE_ID_MAX];
	assert(abs_profiles_add("Home", "http://a/", tricky, new_id, sizeof(new_id)));
	assert(strcmp(new_id, "p1") == 0);
	assert(abs_profiles_add("Work", "https://b.example.com/x", "", NULL, 0));
	assert(abs_profiles_select("p1"));
	assert(abs_profiles_edit("p2", NULL, "t2"));

	// A restart re-reads the same state, byte for byte.
	assert(abs_profiles_init() == 2);
	abs_profile_t list[ABS_PROFILE_MAX];
	assert(abs_profiles_list(list, ABS_PROFILE_MAX) == 2);
	assert(strcmp(list[0].id, "p1") == 0 && strcmp(list[0].name, "Home") == 0);
	assert(strcmp(list[0].server, "http://a") == 0 && strcmp(list[0].token, tricky) == 0);
	assert(strcmp(list[1].id, "p2") == 0 && strcmp(list[1].server, "https://b.example.com/x") == 0);
	assert(strcmp(list[1].token, "t2") == 0);
	char id[ABS_PROFILE_ID_MAX];
	assert(abs_profiles_active(id, sizeof(id)) && strcmp(id, "p1") == 0);

	// next_id survived the round-trip: the next profile is p3, not p1.
	assert(abs_profiles_add("Third", "http://c", "t3", new_id, sizeof(new_id)));
	assert(strcmp(new_id, "p3") == 0);

	// The file carries tokens: it must be 0600, and no temp may linger.
	assert(file_mode(prof) == 0600);
	char tmp[560];
	snprintf(tmp, sizeof(tmp), "%s.tmp", prof);
	assert(!file_exists(tmp));

	// No migration happened here: legacy_server stays empty.
	char legacy[ABS_SERVER_MAX];
	abs_profiles_legacy_server(legacy, sizeof(legacy));
	assert(legacy[0] == '\0');
}

// ---------------------------------------------------------------------------
// Migration from 0.5.0 (§11.1)
// ---------------------------------------------------------------------------

static void migration_tests(void) {
	char cfg[512], prof[512];
	path_join(cfg, sizeof(cfg), "device_config.ini");
	path_join(prof, sizeof(prof), "mig.ini");
	if (file_exists(prof)) unlink(prof);
	setenv("SONIX_AUDIOBOOKSHELF_PROFILES", prof, 1);

	// A 0.5.0 device: legacy credentials in the settings file only. config_init
	// reports false for a not-yet-existing file but still arms the store.
	config_init(cfg);
	assert(config_path() && strcmp(config_path(), cfg) == 0);
	config_set("audiobookshelf", "server", " http://legacy.example.com/ "); // unnormalized on purpose
	config_set("audiobookshelf", "token", "legtok");
	assert(config_save());

	assert(abs_profiles_init() == 1);
	abs_profile_t list[ABS_PROFILE_MAX];
	assert(abs_profiles_list(list, ABS_PROFILE_MAX) == 1);
	assert(strcmp(list[0].id, "p1") == 0 && strcmp(list[0].name, "Audiobookshelf") == 0);
	assert(strcmp(list[0].server, "http://legacy.example.com") == 0); // normalized exactly once
	assert(strcmp(list[0].token, "legtok") == 0);
	char id[ABS_PROFILE_ID_MAX];
	assert(abs_profiles_active(id, sizeof(id)) && strcmp(id, "p1") == 0);
	char legacy[ABS_SERVER_MAX];
	abs_profiles_legacy_server(legacy, sizeof(legacy));
	assert(strcmp(legacy, "http://legacy.example.com") == 0); // old manifests bind here

	// The profile file won before the legacy keys were cleared.
	assert(file_mode(prof) == 0600);
	assert(strcmp(config_get("audiobookshelf", "server", ""), "") == 0);
	assert(strcmp(config_get("audiobookshelf", "token", ""), "") == 0);

	// A second boot sees the file, not the (now empty) legacy keys.
	assert(abs_profiles_init() == 1);
	assert(abs_profiles_list(list, ABS_PROFILE_MAX) == 1);
	assert(strcmp(list[0].server, "http://legacy.example.com") == 0);

	// Missing token: no profile is invented from half a credential pair.
	char cfg2[512], prof2[512];
	path_join(cfg2, sizeof(cfg2), "device_config_two.ini");
	path_join(prof2, sizeof(prof2), "mig2.ini");
	if (file_exists(prof2)) unlink(prof2);
	setenv("SONIX_AUDIOBOOKSHELF_PROFILES", prof2, 1);
	config_init(cfg2); // fresh file: false is expected, store still armed
	assert(config_path() && strcmp(config_path(), cfg2) == 0);
	config_set("audiobookshelf", "server", "http://half.example.com");
	// no token key at all
	assert(abs_profiles_init() == 0);
	assert(!file_exists(prof2)); // unconfigured store: nothing written
	assert(strcmp(config_get("audiobookshelf", "server", ""), "http://half.example.com") == 0); // untouched
}

// ---------------------------------------------------------------------------
// Strict parser: corruption is rejected whole, file kept for diagnosis (§11.1)
// ---------------------------------------------------------------------------

static void corrupt_file_tests(void) {
	struct {
		const char *name;
		const char *text;
	} cases[] = {
		{"duplicate-field", "version=1\nnext_id=2\nactive=\n[profile.p1]\nserver=http://a\ntoken=t\nserver=http://b\n"},
		{"unknown-top-level", "version=1\nnext_id=1\nbogus=1\n"},
		{"unknown-section", "version=1\nnext_id=1\n[other]\n"},
		{"unknown-section-field", "version=1\nnext_id=2\n[profile.p1]\nname=n\npassword=x\n"},
		{"bad-version", "version=2\nnext_id=1\n"},
		{"missing-next-id", "version=1\nactive=\n"},
		{"dangling-active", "version=1\nnext_id=2\nactive=p9\n[profile.p1]\nserver=http://a\ntoken=t\n"},
		{"duplicate-server", "version=1\nnext_id=3\n[profile.p1]\nserver=http://same\ntoken=a\n"
						 "[profile.p2]\nserver=http://same\ntoken=b\n"},
		{"stale-next-id", "version=1\nnext_id=1\n[profile.p2]\nserver=http://a\ntoken=t\n"},
		{"bad-profile-id", "version=1\nnext_id=2\n[profile.x1]\nserver=http://a\ntoken=t\n"},
		{"unterminated-section", "version=1\nnext_id=1\n[profile.p1\n"},
	};
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		char path[512];
		path_join(path, sizeof(path), cases[i].name);
		write_file(path, cases[i].text);
		setenv("SONIX_AUDIOBOOKSHELF_PROFILES", path, 1);
		assert(abs_profiles_init() == -1); // rejected whole
		assert(abs_profiles_count() == 0);
		assert(!abs_profiles_any_configured());
		assert(file_exists(path)); // kept on disk so the cause is diagnosable
	}

	// A line over 2 KiB and a file over 8 KiB are both corruption.
	char big_line[ABS_PROFILES_LINE_MAX_BYTES + 64];
	int off = snprintf(big_line, sizeof(big_line), "version=1\nnext_id=1\nlegacy_server=");
	memset(big_line + off, 'x', ABS_PROFILES_LINE_MAX_BYTES); // value alone is already over the line ceiling
	big_line[off + ABS_PROFILES_LINE_MAX_BYTES] = '\n';
	{
		char path[512];
		path_join(path, sizeof(path), "long-line");
		write_file(path, big_line);
		setenv("SONIX_AUDIOBOOKSHELF_PROFILES", path, 1);
		assert(abs_profiles_init() == -1);
	}
	{
		char path[512];
		path_join(path, sizeof(path), "long-file");
		FILE *f = fopen(path, "wb");
		assert(f);
		for (int i = 0; i < ABS_PROFILES_FILE_MAX_BYTES / 4 + 1; i++) fputs("version=1\n", f); // > 8 KiB
		fclose(f);
		setenv("SONIX_AUDIOBOOKSHELF_PROFILES", path, 1);
		assert(abs_profiles_init() == -1);
	}

	unsetenv("SONIX_AUDIOBOOKSHELF_PROFILES");
}

int main(int argc, char **argv) {
	if (argc != 2) {
		fprintf(stderr, "usage: %s <workdir>\n", argv[0]);
		return 2;
	}
	work = argv[1];

	pathlimits_tests();
	normalize_tests();
	validation_tests();
	memory_store_tests();
	ephemeral_host_tests();
	persistence_tests();
	migration_tests();
	corrupt_file_tests();

	printf("audiobookshelf-profiles-test: all tests passed\n");
	return 0;
}
