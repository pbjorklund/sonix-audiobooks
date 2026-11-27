// Unit tests for the Tidal download queue (tidalkeep) and the "keep on card"
// rules of the Tidal cache (tidalkeep/tidalcache).
//
// The queue's per-track fetch is injected (tidalkeep_set_executor): the tests
// simulate a download by writing the track file into a temporary cache root,
// so the queue's real subject -- enqueue, dedupe, retry, failure, retry
// batch, keep marker -- runs against the real cache code and a real
// directory, without any network.

#include "src/system/streaming/tidalkeep.h"
#include "src/system/streaming/tidalcache.h"
#include "src/system/net/http.h"
#include "src/system/decode/decode.h"
#include "src/system/library/lrclib.h"

#include <assert.h>
#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static char root[512];
static int files_created;

// The scripted outcome of the next fetch: negative counts down failures.
static int fail_next;
// Ids that were fetched, in order.
static long fetched[64];
static int fetched_count;
static bool corrupt_once;
static bool short_audio;
static bool extras_fixture;
static int saved_lyrics;
static bool bad_cover;
static int cover_fetches;
static _Thread_local bool count_tag_reads;
static _Thread_local int tag_reads;

FILE *__real_fopen(const char *path, const char *mode);
FILE *__wrap_fopen(const char *path, const char *mode) {
	if (count_tag_reads && mode[0] == 'r' && strstr(path, ".link.tags")) tag_reads++;
	return __real_fopen(path, mode);
}

static void make_file(long id, const char *ext) {
	char path[600];
	snprintf(path, sizeof(path), "%s/.local/tidal-cache/%ld.%s", root, id, ext);
	FILE *f = fopen(path, "w");
	assert(f);
	fprintf(f, "track %ld", id);
	fclose(f);
	files_created++;
}

// The simulated download: succeeds by writing the file, or fails as often as
// fail_next says.
static int fetched_quality;
static pthread_mutex_t fetch_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t fetch_gate = PTHREAD_COND_INITIALIZER;
static bool hold_fetch;
static bool fetch_waiting;

static bool fake_fetch(const tidal_track_t *track, int quality, char *path, size_t size, bool *complete_transfer) {
	pthread_mutex_lock(&fetch_lock);
	if (hold_fetch) {
		fetch_waiting = true;
		pthread_cond_signal(&fetch_gate);
		while (hold_fetch) pthread_cond_wait(&fetch_gate, &fetch_lock);
	}
	pthread_mutex_unlock(&fetch_lock);
	fetched_quality = quality;
	*complete_transfer = false;
	assert(fetched_count < (int)(sizeof(fetched) / sizeof(fetched[0])));
	fetched[fetched_count++] = track->id;
	if (fail_next > 0) {
		fail_next--;
		return false;
	}
	make_file(track->id, "flac");
	short_audio = corrupt_once;
	corrupt_once = false;
	char marker[600];
	snprintf(marker, sizeof(marker), "%s/.local/tidal-cache/%ld.flac.incompleto", root, track->id);
	unlink(marker);
	snprintf(path, size, "%s/.local/tidal-cache/%ld.flac", root, track->id);
	return true;
}

static tidal_track_t track(long id, int number) {
	tidal_track_t t = {0};
	t.id = id;
	t.streamable = true;
	snprintf(t.title, sizeof(t.title), "Track %ld", id);
	snprintf(t.artist, sizeof(t.artist), "Artist");
	snprintf(t.album, sizeof(t.album), "Album %ld", id / 10);
	snprintf(t.album_id, sizeof(t.album_id), "al-%ld", id / 10);
	t.track_number = number;
	return t;
}

static void enqueue_track(long id, int number) {
	tidal_track_t t = track(id, number);
	assert(tidalkeep_enqueue(&t) == TIDALKEEP_ADDED);
}

static void drain(tidalkeep_state_t *final) {
	// The worker is asynchronous with a retry delay of seconds on failure;
	// tests that avoid failures drain quickly, failure tests are given a
	// generous bound.
	for (int i = 0; i < 3000; i++) {
		usleep(10 * 1000);
		*final = tidalkeep_state();
		if (!final->active) {
			return;
		}
	}
	assert(!"queue did not drain");
}

static void write_tags(long id, const char *title, int number, const char *album_id) {
	char path[600];
	char audio[512];
	assert(tidalcache_find(id, audio, sizeof(audio)));
	snprintf(path, sizeof(path), "%s.tags", audio);
	FILE *f = fopen(path, "w");
	assert(f);
	fprintf(f, "title=%s\nartist=Artist\nalbum=Album\nalbum_id=%s\ntrack=%d\ntidal_id=%ld\ncover_px=480\n", title,
			album_id, number, id);
	fclose(f);
}

static bool file_in(const char *folder, const char *suffix, long id) {
	char path[600];
	snprintf(path, sizeof(path), "%s/%s/%ld%s", root, folder, id, suffix);
	struct stat st;
	return stat(path, &st) == 0;
}

static bool file_exists(const char *suffix, long id) { return file_in(".local/tidal-cache", suffix, id); }
static bool on_card(const char *suffix, long id) {
	char path[512];
	if (!tidalcache_find(id, path, sizeof(path)) || tidalcache_owns(path)) {
		return false;
	}
	char *dot = strrchr(path, '.');
	assert(dot);
	snprintf(dot, sizeof(path) - (size_t)(dot - path), "%s", suffix);
	return access(path, F_OK) == 0;
}

static void write_marker(long id) {
	char path[600];
	snprintf(path, sizeof(path), "%s/.local/tidal-cache/%ld.keep", root, id);
	FILE *f = fopen(path, "w");
	assert(f && fclose(f) == 0);
}

static int dir_entries(void) {
	DIR *d = opendir(root);
	assert(d);
	int n = 0;
	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		if (e->d_name[0] != '.') {
			n++;
		}
	}
	closedir(d);
	return n;
}

static void truncate_file(long id, long bytes) {
	char path[600];
	snprintf(path, sizeof(path), "%s/.local/tidal-cache/%ld.flac", root, id);
	FILE *f = fopen(path, "w");
	assert(f);
	fclose(f);
	assert(truncate(path, bytes) == 0);
}

static bool keep_fixture(const tidal_track_t *t) {
	char path[512];
	return tidalcache_find(t->id, path, sizeof(path)) && tidalcache_keep(t, path, false);
}

static void test_download_percent(void);
static void test_reuse(void);

int main(int argc, char **argv) {
	assert(argc == 2);
	snprintf(root, sizeof(root), "%s", argv[1]);
	mkdir(root, 0777);
	char local[600], cache_dir[600];
	snprintf(local, sizeof(local), "%s/.local", root);
	snprintf(cache_dir, sizeof(cache_dir), "%s/.local/tidal-cache", root);
	mkdir(local, 0777);
	mkdir(cache_dir, 0777);
	tidalcache_set_root(root);
	tidalkeep_set_executor(fake_fetch);

	// --- the queue: three tracks, serial, kept ------------------------------
	tidalkeep_state_t state;
	pthread_mutex_lock(&fetch_lock);
	hold_fetch = true;
	pthread_mutex_unlock(&fetch_lock);
	enqueue_track(101, 1);
	pthread_mutex_lock(&fetch_lock);
	struct timespec deadline;
	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += 5;
	while (!fetch_waiting) assert(pthread_cond_timedwait(&fetch_gate, &fetch_lock, &deadline) == 0);
	enqueue_track(102, 2);
	enqueue_track(103, 3);
	assert(tidalkeep_activity(101, NULL) == TIDALKEEP_ACTIVE);
	assert(tidalkeep_activity(102, NULL) == TIDALKEEP_QUEUED);
	assert(tidalkeep_activity(0, "al-10") == TIDALKEEP_ACTIVE);
	assert(tidalkeep_activity(0, "al-99") == TIDALKEEP_IDLE);
	hold_fetch = false;
	pthread_cond_signal(&fetch_gate);
	pthread_mutex_unlock(&fetch_lock);
	drain(&state);
	assert(tidalkeep_activity(101, NULL) == TIDALKEEP_IDLE);
	assert(tidalkeep_activity(0, "al-10") == TIDALKEEP_IDLE);
	assert(state.done == 3 && state.failed == 0 && state.pending == 0);
	assert(tidalcache_is_kept(101) && tidalcache_is_kept(102) && tidalcache_is_kept(103));
	assert(on_card(".flac", 101) && on_card(".flac.tags", 101) && file_exists(".keep", 101));
	assert(!file_exists(".flac", 101));

	// --- dedupe: done and queued ids do not re-enter -------------------------
	fetched_count = 0;
	tidal_track_t kept = track(102, 2);
	assert(tidalkeep_enqueue(&kept) == TIDALKEEP_SKIPPED);
	tidal_track_t pending = track(104, 4);
	assert(tidalkeep_enqueue(&pending) == TIDALKEEP_ADDED);
	assert(tidalkeep_enqueue(&pending) == TIDALKEEP_SKIPPED);
	enqueue_track(105, 5);
	drain(&state);
	assert(state.done == 5);
	assert(fetched_count == 2); // exactly two fetches: 104 and 105

	// --- a failed fetch is retried, then kept --------------------------------
	fail_next = 2;
	fetched_count = 0;
	enqueue_track(106, 6);
	drain(&state);
	assert(state.done == 6 && state.failed == 0);
	assert(fetched_count == 3); // two failures then the success
	assert(tidalcache_is_kept(106));

	// --- a track that never succeeds lands in the failed list ----------------
	// Five attempts with a two-second retry delay: the drain bound covers it.
	fail_next = 99;
	enqueue_track(107, 7);
	drain(&state);
	assert(state.failed == 1 && !tidalcache_is_kept(107));
	fail_next = 0;

	// --- retry_failed puts it back and it succeeds this time -----------------
	tidalkeep_retry_failed();
	drain(&state);
	assert(state.failed == 0 && state.done == 7);
	assert(tidalcache_is_kept(107));

	// --- the keep marker survives every cache clearing ------------------------
	// Exit clear: kept tracks stay, the unkept one goes.
	make_file(108, "flac"); // an unkept file
	tidalcache_clear_on_exit();
	assert(on_card(".flac", 101) && file_exists(".keep", 101));
	assert(!file_exists(".flac", 108) && !on_card(".flac", 108));

	// The periodic sweep (every TIDALCACHE_CLEAR_EVERY tracks) leaves kept
	// files alone: 106 and 107 are kept, 108 was already swept away.
	char path108[600];
	snprintf(path108, sizeof(path108), "%s/.local/tidal-cache/106.flac", root);
	for (int i = 0; i < TIDALCACHE_CLEAR_EVERY; i++) {
		tidalcache_note_played(path108);
	}
	assert(on_card(".flac", 106) && on_card(".flac", 107));
	assert(tidalcache_remove_track(106));
	assert(!tidalcache_is_kept(106));
	tidalcache_clear();
	assert(!on_card(".flac", 106) && !file_exists(".keep", 106));
	assert(on_card(".flac", 101));

	// --- kept_list: sorted, paged, metadata read from the sidecars ------------
	// A controlled set: three tagged albums and a track without album metadata.
	assert(tidalcache_remove_track(104));
	assert(tidalcache_remove_track(105));
	write_tags(101, "First", 2, "al-10");
	write_tags(103, "Second", 1, "al-10");
	write_tags(102, "Other album", 1, "al-9");
	write_tags(107, "107", 0, "");
	// Empty album ID first, then al-10 in track order, then al-9.
	tidal_track_t page[2];
	int n = tidalcache_kept_list(page, 2, 0);
	assert(n == 2);
	assert(page[0].id == 107 && page[1].id == 103);
	assert(page[1].track_number == 1 && !strcmp(page[1].title, "Second"));
	n = tidalcache_kept_list(page, 2, 2);
	assert(n == 2);
	assert(page[0].id == 101 && page[1].id == 102);
	assert(!strcmp(page[0].title, "First")); // the sidecar title was read back
	assert(!strcmp(page[1].album_id, "al-9"));
	n = tidalcache_kept_list(page, 2, 4);
	assert(n == 0); // the end of the list

	const char *albums[] = {"al-10", "al-9", "missing"};
	int counts[3];
	make_file(109, "m4a");
	tidal_track_t aac = track(109, 2);
	snprintf(aac.album_id, sizeof(aac.album_id), "al-9");
	snprintf(aac.album_artist, sizeof(aac.album_artist), "Album Artist");
	aac.disc_number = 2;
	aac.release_year = 2024;
	snprintf(aac.title, sizeof(aac.title), "A/B: pièce?");
	assert(keep_fixture(&aac));
	assert(on_card(".m4a", 109) && !file_exists(".m4a", 109));
	make_file(110, "flac");
	make_file(110, "flac.incompleto");
	tidal_track_t incomplete = track(110, 3);
	assert(!keep_fixture(&incomplete));
	assert(!on_card(".flac", 110));
	incomplete.id = 111;
	assert(!keep_fixture(&incomplete));
	tidalcache_kept_album_counts(albums, counts, 3);
	assert(counts[0] == 2 && counts[1] == 2 && counts[2] == 0);
	assert(tidalcache_kept_list(page, 2, 3) == 2 && page[0].id == 102 && page[1].id == 109);
	assert(page[1].disc_number == 2 && strcmp(page[1].album_artist, "Album Artist") == 0);
	char saved_path[512];
	assert(tidalcache_find(109, saved_path, sizeof(saved_path)));
	assert(strstr(saved_path, "/Music/Tidal/Album Artist/Album 10 [al-9] [2024]/02-02 - Artist - A_B_ pièce_.m4a"));
	assert(tidalcache_track_id(saved_path) == 109 && !tidalcache_owns(saved_path));
	// Same name from a distinct track must not overwrite the first download.
	aac.id = 114;
	make_file(114, "m4a");
	assert(keep_fixture(&aac));
	assert(tidalcache_find(114, saved_path, sizeof(saved_path)) && strstr(saved_path, " [114].m4a"));
	assert(tidalcache_remove_track(114));
	assert(tidalcache_remove_track(109));
	tidalcache_kept_album_counts(albums, counts, 3);
	assert(counts[0] == 2 && counts[1] == 1 && counts[2] == 0);

	// --- remove_track: file, tags, cover, marker -------------------------------
	assert(tidalcache_find(103, path108, sizeof(path108)));
	char *dot = strrchr(path108, '.');
	assert(dot);
	strcpy(dot, ".jpg");
	FILE *cover = fopen(path108, "w");
	assert(cover);
	fclose(cover);
	assert(tidalcache_remove_track(103));
	assert(!on_card(".flac", 103) && !file_exists(".keep", 103));
	assert(!on_card(".flac.tags", 103) && !on_card(".jpg", 103));
	assert(!tidalcache_is_kept(103));

	// --- make_room evicts only unkept tracks -----------------------------------
	// Sparse files: big on paper, nothing on disk. Two unkept tracks of
	// 600 MB each overflow the 1 GB ceiling; the kept one is not counted.
	truncate_file(108, 600L * 1024 * 1024);
	truncate_file(105, 600L * 1024 * 1024);
	// The room asked for is another 600 MB: both unkept tracks have to go.
	tidalcache_make_room(600L * 1024 * 1024);
	assert(!file_exists(".flac", 108) && !file_exists(".flac", 105));
	assert(on_card(".flac", 101));

	make_file(113, "mp3");
	write_marker(113);
	assert(tidalcache_is_kept(113));
	tidalcache_publish_legacy();
	tidalcache_publish_legacy();
	assert(on_card(".mp3", 113) && !file_exists(".mp3", 113));
	assert(tidalcache_is_kept(113));

	// --- enqueue refuses what is already kept on the card -----------------------
	files_created = 0;
	fetched_count = 0;
	kept = track(101, 2);
	assert(tidalkeep_enqueue(&kept) == TIDALKEEP_SKIPPED);
	drain(&state);
	assert(fetched_count == 0 && files_created == 0); // no fetch for a kept track
	enqueue_track(103, 3); // an explicitly removed track can be downloaded again
	drain(&state);
	assert(fetched_count == 1 && state.done == 8 && tidalcache_is_kept(103));

	// Decoder length and TIDAL duration are independent checks.
	corrupt_once = true;
	fetched_count = 0;
	enqueue_track(115, 1);
	drain(&state);
	assert(fetched_count == 2 && tidalcache_is_kept(115));
	assert(!file_exists(".flac.incompleto", 115));
	make_file(116, "flac");
	tidal_track_t bad = track(116, 1);
	bad.duration = 10;
	assert(!keep_fixture(&bad) && !tidalcache_is_kept(116));
	assert(file_exists(".flac.incompleto", 116));
	test_download_percent();
	test_reuse();
	assert(tidalkeep_quality() == TIDAL_QUALITY_LOSSLESS);
	tidalkeep_set_quality(TIDAL_QUALITY_HIGH);
	tidalkeep_set_quality(99);
	enqueue_track(121, 1);
	tidalkeep_set_quality(TIDAL_QUALITY_LOW);
	drain(&state);
	assert(fetched_quality == TIDAL_QUALITY_HIGH && tidalkeep_quality() == TIDAL_QUALITY_LOW);
	tidal_track_t change = track(121, 1);
	char before[512], after[512];
	assert(tidalcache_find(change.id, before, sizeof(before)));
	assert(tidalcache_kept_quality(change.id) == TIDAL_QUALITY_HIGH);
	assert(tidalkeep_enqueue_quality(&change, TIDAL_QUALITY_LOW, false) == TIDALKEEP_SKIPPED);
	assert(tidalkeep_enqueue_quality(&change, TIDAL_QUALITY_HIGH, true) == TIDALKEEP_SKIPPED);
	fetched_count = 0;
	fail_next = 99;
	assert(tidalkeep_enqueue_quality(&change, TIDAL_QUALITY_LOW, true) == TIDALKEEP_ADDED);
	drain(&state);
	assert(state.failed == 1 && fetched_count == 5);
	assert(tidalcache_find(change.id, after, sizeof(after)) && !strcmp(before, after));
	assert(tidalcache_kept_quality(change.id) == TIDAL_QUALITY_HIGH);
	fail_next = 0;
	tidalkeep_retry_failed();
	drain(&state);
	assert(state.failed == 0 && tidalcache_kept_quality(change.id) == TIDAL_QUALITY_LOW);
	assert(tidalcache_find(change.id, after, sizeof(after)) && strcmp(before, after));
	assert(access(before, F_OK) != 0);
	assert(tidalkeep_enqueue_quality(&change, TIDAL_QUALITY_HIRES, true) == TIDALKEEP_ADDED);
	drain(&state);
	assert(tidalcache_kept_quality(change.id) == TIDAL_QUALITY_HIRES);

	tidal_track_t single = track(129, 1);
	assert(tidalkeep_enqueue_quality(&single, TIDAL_QUALITY_LOW, false) == TIDALKEEP_ADDED);
	drain(&state);
	assert(tidalcache_is_kept(single.id) && !tidalcache_is_playlist_only(single.id));

	tidal_track_t playlist = track(130, 1);
	const char *album_ids[] = {playlist.album_id};
	assert(tidalkeep_enqueue_batch(&playlist, TIDAL_QUALITY_LOW, false, true) == TIDALKEEP_ADDED);
	drain(&state);
	assert(tidalcache_is_playlist_only(playlist.id));
	assert(tidalcache_find(playlist.id, before, sizeof(before)) && strstr(before, "/.playlist-tracks/130/"));
	tidal_track_t listed;
	assert(tidalcache_kept_track(playlist.id, &listed) && !strcmp(listed.album, playlist.album));
	assert(tidalcache_kept_filtered(&listed, 1, 0, playlist.album_id, NULL) == 0);
	tidalcache_kept_album_counts(album_ids, counts, 1);
	assert(counts[0] == 0);
	fetched_count = 0;
	assert(tidalkeep_enqueue_batch(&playlist, TIDAL_QUALITY_LOW, false, true) == TIDALKEEP_SKIPPED);
	assert(tidalkeep_enqueue_quality(&playlist, TIDAL_QUALITY_HIGH, true) == TIDALKEEP_ADDED);
	drain(&state);
	assert(tidalcache_is_playlist_only(playlist.id) && tidalcache_kept_quality(playlist.id) == TIDAL_QUALITY_HIGH);
	assert(tidalcache_find(playlist.id, before, sizeof(before)));
	fetched_count = 0;
	assert(tidalkeep_enqueue_batch(&playlist, TIDAL_QUALITY_HIGH, false, false) == TIDALKEEP_ADDED);
	drain(&state);
	assert(fetched_count == 0 && !tidalcache_is_playlist_only(playlist.id));
	assert(tidalcache_find(playlist.id, after, sizeof(after)) && strcmp(before, after) && access(before, F_OK) != 0);
	assert(tidalcache_kept_filtered(&listed, 1, 0, playlist.album_id, NULL) == 1);
	assert(tidalkeep_enqueue_batch(&playlist, TIDAL_QUALITY_HIRES, true, true) == TIDALKEEP_ADDED);
	drain(&state);
	assert(tidalcache_kept_quality(playlist.id) == TIDAL_QUALITY_HIRES && !tidalcache_is_playlist_only(playlist.id));
	puts("tidalkeep-test: queue, retry, kept albums, removal, eviction and transfer progress passed");
	(void)dir_entries;
	return 0;
}

// --- link stubs: the tests replace the fetch, so the real network path is
// --- never taken; these satisfy the symbols the linked modules reference.
static void test_reuse(void) {
	char artist[600], album[600], audio[700], found[512];
	snprintf(artist, sizeof(artist), "%s/Music/Tidal/Example", root);
	assert(mkdir(artist, 0755) == 0);
	snprintf(album, sizeof(album), "%s/Music/Tidal/Example/Old Name [12345] [2021]", root);
	assert(mkdir(album, 0755) == 0);
	snprintf(audio, sizeof(audio), "%s/01 - Example - Song.flac", album);
	FILE *f = fopen(audio, "w");
	assert(f && fputs("audio fixture", f) >= 0 && fclose(f) == 0);
	tidal_track_t t = {.id = 120, .track_number = 1, .duration = 1, .artist = "Example", .title = "Song", .album = "New Name", .album_id = "12345"};
	tidal_album_t hint = {.id_text = "12345", .title = "New Name", .artist = "Example", .released = "2021"};
	assert(tidalcache_has_album_folder(&hint));
	assert(!tidalcache_is_kept(t.id)); // Discovery did not open or register the track.
	char duplicate[600];
	snprintf(duplicate, sizeof(duplicate), "%s/Music/Tidal/Example/Duplicate [12345] [2021]", root);
	assert(mkdir(duplicate, 0755) == 0);
	assert(!tidalcache_has_album_folder(&hint) && !tidalcache_reuse(&t));
	assert(rmdir(duplicate) == 0);
	char ambiguous[700];
	snprintf(ambiguous, sizeof(ambiguous), "%s/01 - Example - Song.m4a", album);
	f = fopen(ambiguous, "w");
	assert(f && fputs("duplicate", f) >= 0 && fclose(f) == 0);
	assert(tidalcache_link_album_tracks(&hint, &t, 1) == 0);
	assert(unlink(ambiguous) == 0);
	char live[700];
	snprintf(live, sizeof(live), "%s/01 - Example - Song (Live).flac", album);
	assert(rename(audio, live) == 0);
	strcpy(audio, live);
	snprintf(ambiguous, sizeof(ambiguous), "%s/01 - Example - Song (Live).m4a", album);
	f = fopen(ambiguous, "w");
	assert(f && fputs("duplicate", f) >= 0 && fclose(f) == 0);
	assert(tidalcache_link_album_tracks(&hint, &t, 1) == 0 && !tidalcache_reuse(&t));
	assert(unlink(ambiguous) == 0);
	t.track_number = 2;
	assert(tidalcache_link_album_tracks(&hint, &t, 1) == 0);
	t.track_number = 1;
	char sidecar[710];
	snprintf(sidecar, sizeof(sidecar), "%s.tags", audio);
	f = fopen(sidecar, "w");
	assert(f && fputs("tidal_id=99999\n", f) >= 0 && fclose(f) == 0);
	assert(tidalcache_link_album_tracks(&hint, &t, 1) == 0 && !tidalcache_reuse(&t));
	assert(unlink(sidecar) == 0);
	char unlabelled[600];
	snprintf(unlabelled, sizeof(unlabelled), "%s/Music/Tidal/Example/New Name", root);
	assert(rename(album, unlabelled) == 0);
	assert(tidalcache_has_album_folder(&hint));
	assert(tidalcache_link_album_tracks(&hint, &t, 1) == 0 && !tidalcache_reuse(&t));
	assert(rename(unlabelled, album) == 0);
	// Exercise both callers against an actual typo, not just the Live suffix.
	char typo[700], near[700];
	snprintf(typo, sizeof(typo), "%s/01 - Example - Midnight Reflctions.flac", album);
	snprintf(near, sizeof(near), "%s/01 - Example - Midnigt Reflection.flac", album);
	assert(rename(audio, typo) == 0);
	strcpy(t.title, "Midnight Reflections");
	f = fopen(near, "w");
	assert(f && fputs("second candidate", f) >= 0 && fclose(f) == 0);
	assert(tidalcache_link_album_tracks(&hint, &t, 1) == 0 && !tidalcache_reuse(&t));
	assert(unlink(near) == 0);
	strcpy(t.artist, "Exampl");
	assert(tidalcache_link_album_tracks(&hint, &t, 1) == 0 && !tidalcache_reuse(&t));
	strcpy(t.artist, "Example");
	assert(tidalcache_link_album_tracks(&hint, &t, 1) == 1);
	assert(tidalcache_remove_track(t.id) && access(typo, R_OK) == 0);
	assert(tidalcache_reuse(&t));
	assert(tidalcache_find(t.id, found, sizeof(found)) && !strcmp(found, typo));
	assert(tidalcache_remove_track(t.id) && rename(typo, audio) == 0);
	strcpy(t.title, "Song");
	assert(tidalcache_link_album_tracks(&hint, &t, 1) == 1);
	assert(tidalcache_link_album_tracks(&hint, &t, 1) == 0);
	assert(tidalcache_remove_track(t.id) && access(audio, R_OK) == 0);
	strcpy(hint.id_text, "54321");
	assert(!tidalcache_has_album_folder(&hint));
	assert(tidalcache_reuse(&t));
	assert(!tidalcache_reuse(&t));
	assert(tidalcache_find(t.id, found, sizeof(found)) && strcmp(found, audio) == 0);
	tidal_track_t kept;
	count_tag_reads = true;
	tag_reads = 0;
	assert(tidalcache_kept_track(t.id, &kept) && strcmp(kept.album_id, "12345") == 0);
	assert(tag_reads == 1);
	tag_reads = 0;
	assert(tidalcache_kept_filtered(&kept, 1, 0, "12345", "Example") == 1 && kept.id == t.id);
	assert(tag_reads == 1);
	count_tag_reads = false;
	tidal_album_t groups[2];
	assert(tidalcache_kept_groups(groups, 2, 0, false, "Example") == 1);
	assert(strcmp(groups[0].title, "New Name") == 0 && groups[0].track_count == 1);
	assert(tidalcache_kept_filtered(&kept, 1, 0, "12345", "Example") == 1 && kept.id == t.id);
	strcpy(t.cover, "https://example.invalid/cover");
	extras_fixture = true;
	tidalcache_download_extras(&t);
	char cover[700];
	snprintf(cover, sizeof(cover), "%s/cover.jpg", album);
	assert(access(cover, F_OK) == 0 && saved_lyrics == 1);
	assert(tidalcache_kept_groups(groups, 2, 0, false, "Example") == 1);
	assert(strcmp(groups[0].cover, cover) == 0);
	assert(tidalcache_kept_track(t.id, &kept) && !strcmp(kept.cover, cover));
	assert(tidalcache_kept_filtered(&kept, 1, 0, "12345", "Example") == 1 && !strcmp(kept.cover, cover));
	assert(cover_fetches == 1);
	tidalcache_refresh_extras(&t);
	assert(cover_fetches == 1 && saved_lyrics == 1); // Imported cover and existing lyrics stay intact.
	assert(remove(cover) == 0);
	tidalcache_refresh_extras(&t);
	assert(cover_fetches == 2 && saved_lyrics == 1 && access(cover, F_OK) == 0);
	char owned[512], owned_cover[560];
	assert(tidalcache_find(101, owned, sizeof(owned)));
	snprintf(owned_cover, sizeof(owned_cover), "%.*s/cover.jpg", (int)(strrchr(owned, '/') - owned), owned);
	f = fopen(owned_cover, "w");
	assert(f && fputs("old", f) >= 0 && fclose(f) == 0);
	tidal_track_t owned_extras = {.id = 101, .cover = "https://example.invalid/cover"};
	bad_cover = true;
	tidalcache_refresh_extras(&owned_extras);
	f = fopen(owned_cover, "rb");
	assert(f && fgetc(f) == 'o' && fclose(f) == 0);
	bad_cover = false;
	tidalcache_refresh_extras(&owned_extras);
	f = fopen(owned_cover, "rb");
	assert(f && fgetc(f) == 0xff && fclose(f) == 0);
	assert(access(owned, R_OK) == 0 && saved_lyrics == 1);
	extras_fixture = false;
	assert(tidalcache_remove_track(t.id) && access(audio, F_OK) == 0);
	strcpy(t.album, "Old Name");
	strcpy(t.album_id, "54321");
	assert(!tidalcache_reuse(&t)); // conflicting edition even though the title matches
	snprintf(album, sizeof(album), "%s/Music/Tidal/Example/Old Name", root);
	assert(mkdir(album, 0755) == 0);
	snprintf(audio, sizeof(audio), "%s/01 - Example - Song.flac", album);
	f = fopen(audio, "w");
	assert(f && fputs("audio fixture", f) >= 0 && fclose(f) == 0);
	assert(tidalcache_reuse(&t)); // artist + title fallback, no conflicting ID
	assert(tidalcache_find(t.id, found, sizeof(found)) && strcmp(found, audio) == 0);
	assert(tidalcache_remove_track(t.id) && access(audio, F_OK) == 0);
}

lrclib_result_t lrclib_search(const char *title, const char *artist, const char *album, int secs, lrclib_record_t **records, int *count) {
	static lrclib_record_t fixture[] = {
		{.track = "Song", .artist = "Example", .album = "Wrong album", .duration = 1, .text = "Wrong"},
		{.track = "Song", .artist = "Example", .album = "New Name", .duration = 1, .text = "[00:00.00]Correct"},
	};
	(void)title; (void)artist; (void)album; (void)secs;
	*records = extras_fixture ? fixture : NULL;
	*count = extras_fixture ? 2 : 0;
	return extras_fixture ? LRCLIB_OK : LRCLIB_NOT_FOUND;
}
void lrclib_free(lrclib_record_t *records, int count) { (void)records; (void)count; }
bool lrclib_save(const char *path, const char *text) {
	assert((strstr(path, "01 - Example - Song.flac") || strstr(path, "01 - Example - Song (Live).flac")) &&
		strcmp(text, "[00:00.00]Correct") == 0);
	char lrc[700];
	assert(snprintf(lrc, sizeof(lrc), "%.*s.lrc", (int)(strrchr(path, '.') - path), path) < (int)sizeof(lrc));
	FILE *f = fopen(lrc, "wx");
	assert(f && fputs(text, f) >= 0 && fclose(f) == 0);
	saved_lyrics++;
	return true;
}

const char *tr(const char *text) { return text; }
bool http_get(const char *url, char **body, size_t *len, size_t cap, int secs) {
	(void)cap; (void)secs;
	if (!extras_fixture) return false;
	assert(strcmp(url, "https://example.invalid/cover") == 0);
	static const unsigned char image[] = {0xff,0xd8,0xff,0xc0,0,7,8,0,1,0,1,0xff,0xd9};
	cover_fetches++;
	*len = bad_cover ? 4 : sizeof(image);
	*body = malloc(*len);
	assert(*body);
	memcpy(*body, image, *len);
	return true;
}
const char *http_last_error(void) { return "stub"; }
static bool probe_fixture, bad_tail;
static int probe_reads;
struct decoder { bool read; bool tail; };
static struct decoder decoder;
decode_format_t decode_detect_format(const char *path) {
	return strstr(path, ".m4a") ? DECODE_FORMAT_AAC_MP4 : DECODE_FORMAT_FLAC;
}
decoder_t *decoder_open(const char *path, decode_format_t format) {
	(void)path; (void)format;
	decoder = (struct decoder){0};
	return &decoder;
}
void decoder_close(decoder_t *dec) { (void)dec; }
int decoder_sample_rate(const decoder_t *dec) { (void)dec; return 100; }
int decoder_channels(const decoder_t *dec) { (void)dec; return 2; }
uint64_t decoder_total_pcm_frames(const decoder_t *dec) { (void)dec; return probe_fixture ? 1000 : 100; }
uint64_t decoder_read_pcm_frames_s16(decoder_t *dec, uint64_t max, short *pcm) {
	(void)pcm;
	assert(max >= 100);
	if (probe_fixture) {
		probe_reads++;
		return dec->tail ? (bad_tail ? 0 : 25) : 100;
	}
	if (dec->read) return 0;
	dec->read = true;
	return short_audio ? 50 : 100;
}

static int stream_reads;
static bool known_length;
static bool truncate_transfer;
bool http_stream_open(http_stream_t *st, const char *url, int timeout_secs) {
	(void)timeout_secs;
	assert(strcmp(url, "fixture") == 0);
	stream_reads = 0;
	st->content_length = known_length ? 8 : 0;
	return true;
}
int http_stream_read(http_stream_t *st, void *buf, int len) {
	(void)st;
	assert(tidalcache_download_percent(112) == -1);
	if (stream_reads == 0) {
		assert(tidalcache_download_percent(111) == -1);
	} else if (stream_reads == 1) {
		assert(tidalcache_download_percent(111) == (known_length ? 50 : -1));
		if (truncate_transfer) return 0;
	} else {
		assert(tidalcache_download_percent(111) == (known_length ? 99 : -1));
		return 0;
	}
	stream_reads++;
	assert(len >= 4);
	memcpy(buf, "test", 4);
	return 4;
}

static void test_download_percent(void) {
	tidal_stream_t stream = {.kind = TIDAL_STREAM_FILE};
	snprintf(stream.url, sizeof(stream.url), "fixture");
	snprintf(stream.mime, sizeof(stream.mime), "audio/flac");
	char path[512];
	for (int i = 0; i < 2; i++) {
		known_length = i == 0;
		assert(tidalcache_start(111, &stream, 0, path, sizeof(path)));
		tidalcache_wait_idle();
		assert(stream_reads == 2 && tidalcache_download_percent(111) == -1);
		assert(tidalcache_find(111, NULL, 0));
		assert(tidalcache_remove_track(111));
	}
	known_length = true;
	make_file(111, "flac"); // a streaming copy must not satisfy the download
	bool complete;
	assert(tidalcache_download(111, &stream, path, sizeof(path), &complete));
	assert(complete && strstr(path, "111.download.flac") && stream_reads == 2);
	probe_fixture = true;
	tidal_track_t t = track(111, 1);
	t.duration = 10;
	assert(tidalcache_keep(&t, path, complete) && probe_reads == 2);
	assert(tidalcache_remove_track(111));
	assert(tidalcache_download(111, &stream, path, sizeof(path), &complete) && complete);
	bad_tail = true;
	assert(!tidalcache_keep(&t, path, complete) && !tidalcache_is_kept(111));
	probe_fixture = false;
	known_length = false;
	assert(tidalcache_download(111, &stream, path, sizeof(path), &complete) && !complete);
	known_length = true;
	truncate_transfer = true;
	assert(!tidalcache_download(111, &stream, path, sizeof(path), &complete) && !complete);
	assert(file_exists(".download.flac.incompleto", 111));
}
void http_stream_close(http_stream_t *st) { (void)st; }
void http_stream_wake(http_stream_t *st) { (void)st; }
int tidal_cover_px(void) { return 480; }
bool tidal_dash_segment_url(const tidal_stream_t *stream, int number, char *out, size_t size) {
	(void)stream; (void)number; (void)out; (void)size;
	return false;
}
bool tidal_track_stream_quality(long id, int quality, tidal_stream_t *out) { (void)id; (void)quality; (void)out; return false; }

static long stored_quality = -1;
long config_get_int(const char *section, const char *key, long fallback) {
	assert(strcmp(section, "tidal") == 0 && strcmp(key, "download_quality") == 0);
	return stored_quality == -1 ? fallback : stored_quality;
}
void config_set_int(const char *section, const char *key, long value) {
	assert(strcmp(section, "tidal") == 0 && strcmp(key, "download_quality") == 0);
	stored_quality = value;
}
bool config_save(void) { return true; }
int decoder_seek_to_frame(decoder_t *dec, uint64_t frame) {
	assert(probe_fixture && frame == 975);
	dec->tail = true;
	return 1;
}
