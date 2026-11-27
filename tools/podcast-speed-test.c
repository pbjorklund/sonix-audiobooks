#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/system/core/utils.h"
#include "src/system/streaming/abs_episode_playback.h"
#include "src/system/streaming/podcast.h"
#include "src/system/streaming/podcastsubs.h"

long config_get_int(const char *section, const char *key, long fallback) {
	assert(!strcmp(section, "podcast") && !strcmp(key, "speed") && fallback == 1000);
	return 1500; // Existing global preference remains the default, not a shared override.
}

bool abs_episode_playback_identity(const char *path, abs_episode_manifest_t *out) {
	if (strncmp(path, "abs-", 4)) return false;
	memset(out, 0, sizeof(*out));
	snprintf(out->server, sizeof(out->server), "https://server-%c.example", path[4]);
	snprintf(out->owner, sizeof(out->owner), "owner-%c", path[6]);
	snprintf(out->show, sizeof(out->show), "show-%c", path[8]);
	return true;
}

bool podcastcache_feed_id(const char *path, long long *out) {
	if (strncmp(path, "rss-", 4)) return false;
	*out = strtoll(path + 4, NULL, 10);
	return *out > 0;
}

void podcastrss_remember(long long id, const char *url) { (void)id; (void)url; }

int main(int argc, char **argv) {
	assert(argc == 2);
	podcastsubs_set_root(argv[1]);
	assert(podcast_speed_for("abs-a-a-a-episode1") == 1500);
	assert(podcast_remember_speed("abs-a-a-a-episode1", 1250));
	assert(podcast_remember_speed("abs-a-a-b-episode1", 1000));
	assert(podcast_remember_speed("rss-19-episode1", 1750));
	assert(podcast_remember_speed("rss-20-episode1", 1000));
	assert(!podcast_remember_speed("no-identity", 1250));
	assert(!podcast_remember_speed("rss-19-episode1", -1));

	podcastsubs_set_root(NULL);
	assert(!podcast_remember_speed("rss-19-episode1", 2000));
	podcastsubs_set_root(argv[1]);
	assert(podcast_speed_for("abs-a-a-a-episode2") == 1250);
	assert(podcast_speed_for("abs-a-a-b-episode2") == 1000);
	assert(podcast_speed_for("abs-a-b-a-episode2") == 1500);
	assert(podcast_speed_for("abs-b-a-a-episode2") == 1500);
	assert(podcast_speed_for("rss-19-episode2") == 1750);
	assert(podcast_speed_for("rss-20-episode2") == 1000);
	assert(podcast_speed_for("rss-21-episode2") == 1500);
	puts("Podcast speeds: same-show reuse, feed/profile isolation, explicit 1x, default and persistence passed.");
}
