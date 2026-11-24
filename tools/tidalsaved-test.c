#include "src/system/streaming/tidalsaved.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
	assert(argc == 2);
	const char *root = argv[1];
	tidal_album_t out[2];
	tidal_album_t a = {.id_text = "12", .title = "Eine Nacht / Live", .artist = "Artist's Name",
		.cover = "https://example.invalid/cover", .released = "1999", .track_count = 3};
	tidal_album_t b = {.id_text = "13", .title = "Another album"};
	assert(!tidalsaved_has(root, a.id_text));
	assert(tidalsaved_list(root, 0, out, 2) == 0);
	assert(!tidalsaved_set(NULL, &a, true));
	assert(tidalsaved_set(root, &a, true));
	assert(tidalsaved_set(root, &a, true));
	assert(tidalsaved_has(root, a.id_text));
	assert(tidalsaved_set(root, &b, true));
	assert(tidalsaved_list(root, 0, out, 1) == 1 && strcmp(out[0].id_text, b.id_text) == 0);
	assert(tidalsaved_list(root, 1, out, 2) == 1 && strcmp(out[0].id_text, a.id_text) == 0);
	assert(strcmp(out[0].title, a.title) == 0 && strcmp(out[0].artist, a.artist) == 0);
	assert(strcmp(out[0].cover, a.cover) == 0 && strcmp(out[0].released, a.released) == 0);
	assert(out[0].track_count == 3);
	assert(tidalsaved_set(root, &a, false));
	assert(tidalsaved_set(root, &a, false));
	assert(!tidalsaved_has(root, a.id_text));
	assert(tidalsaved_list(root, 0, out, 2) == 1 && strcmp(out[0].id_text, b.id_text) == 0);
	const long ids[] = {3, 1, 3, 2};
	long actual[4];
	tidal_playlist_t playlist[2];
	assert(tidalsaved_playlist_set(root, "p1", "Offline mix", ids, 4));
	assert(tidalsaved_playlists(root, 0, playlist, 2) == 1);
	assert(strcmp(playlist[0].id, "p1") == 0 && strcmp(playlist[0].name, "Offline mix") == 0);
	assert(tidalsaved_playlist_tracks(root, "p1", 0, actual, 4) == 4);
	assert(memcmp(ids, actual, sizeof(ids)) == 0);
	assert(tidalsaved_playlist_set(root, "p1", "Updated", ids + 1, 2));
	assert(tidalsaved_playlist_tracks(root, "p1", 1, actual, 4) == 1 && actual[0] == 3);
	puts("tidalsaved-test: persisted albums and offline playlist order passed");
}
