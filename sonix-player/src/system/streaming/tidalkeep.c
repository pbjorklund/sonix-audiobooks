#include "tidalkeep.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "src/system/core/config.h"
#include "src/system/core/utils.h"
#include "src/system/streaming/tidalcache.h"

// How many times one track is tried before it is given up as failed. A play
// press abandons whatever is downloading (streamturn_abandon_all()), so the
// budget is what lets a download survive being interrupted by its own user.
#define TIDALKEEP_ATTEMPTS 5

// Between attempts, so a failure that will repeat (no network, the account
// refusing) does not spin.
#define TIDALKEEP_RETRY_DELAY_SECS 2

typedef struct {
	tidal_track_t track;
	int attempts;
	int quality;
	int replace_quality;
	bool playlist, promote;
} tidalkeep_item_t;

int tidalkeep_quality(void) {
	long value = config_get_int("tidal", "download_quality", TIDAL_QUALITY_LOSSLESS);
	return value >= TIDAL_QUALITY_LOW && value <= TIDAL_QUALITY_HIRES ? (int)value : TIDAL_QUALITY_LOSSLESS;
}

void tidalkeep_set_quality(int value) {
	if (value >= TIDAL_QUALITY_LOW && value <= TIDAL_QUALITY_HIRES) {
		config_set_int("tidal", "download_quality", value);
		config_save();
	}
}

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;

typedef struct keep_node {
	tidalkeep_item_t item;
	struct keep_node *next;
} keep_node_t;

typedef struct {
	keep_node_t *head, *tail;
	int count;
} keep_list_t;

static keep_list_t queue, failed;

static void push(keep_list_t *list, keep_node_t *node) {
	node->next = NULL;
	if (list->tail) list->tail->next = node;
	else list->head = node;
	list->tail = node;
	list->count++;
}

static keep_node_t *pop(keep_list_t *list) {
	keep_node_t *node = list->head;
	list->head = node->next;
	if (!list->head) list->tail = NULL;
	list->count--;
	return node;
}

static void forget_failed(long id) {
	keep_node_t *prev = NULL;
	for (keep_node_t *node = failed.head; node; prev = node, node = node->next) {
		if (node->item.track.id != id) continue;
		if (prev) prev->next = node->next;
		else failed.head = node->next;
		if (failed.tail == node) failed.tail = prev;
		failed.count--;
		free(node);
		break;
	}
}
static int done_count;
static long current_id;
static char current_album[TIDAL_ID_MAX];
static bool started;

static tidalkeep_done_cb_t done_cb;

static void *worker_main(void *unused);

// The per-track operation. The default is the real path: manifest, download
// into the cache. The seam exists for the unit tests.
static bool fetch_default(const tidal_track_t *track, int quality, char *path, size_t size, bool *complete_transfer) {
	tidal_stream_t *stream = calloc(1, sizeof(*stream));
	if (!stream) {
		return false;
	}
	if (!tidal_track_stream_quality(track->id, quality, stream)) {
		free(stream);
		return false;
	}
	bool ok = tidalcache_download(track->id, stream, path, size, complete_transfer);
	free(stream);
	return ok;
}

static tidalkeep_fetch_t fetch = fetch_default;

void tidalkeep_set_executor(tidalkeep_fetch_t fn) {
	fetch = fn ? fn : fetch_default;
}

void tidalkeep_set_done_cb(tidalkeep_done_cb_t cb) { done_cb = cb; }

static bool is_queued(long id) {
	if (current_id == id) {
		return true;
	}
	for (keep_node_t *node = queue.head; node; node = node->next) {
		if (node->item.track.id == id) {
			return true;
		}
	}
	return false;
}

bool tidalkeep_quality_differs(int kept_quality, int requested_quality) {
	return kept_quality >= TIDALCACHE_QUALITY_UNKNOWN && kept_quality != requested_quality;
}

static tidalkeep_enqueue_result_t enqueue_quality(const tidal_track_t *track, int quality, bool replace,
		bool playlist, bool album) {
	if (!track || track->id <= 0 || !track->streamable || quality < TIDAL_QUALITY_LOW || quality > TIDAL_QUALITY_HIRES) {
		return TIDALKEEP_SKIPPED;
	}
	int kept_quality = tidalcache_kept_quality(track->id);
	bool promote = album && tidalcache_is_playlist_only(track->id);
	bool replace_existing = replace && tidalkeep_quality_differs(kept_quality, quality);
	if (kept_quality != TIDALCACHE_QUALITY_NONE && !replace_existing && !promote) {
		return TIDALKEEP_SKIPPED;
	}
	pthread_mutex_lock(&lock);
	if (is_queued(track->id)) {
		pthread_mutex_unlock(&lock);
		return TIDALKEEP_SKIPPED;
	}
	keep_node_t *node = malloc(sizeof(*node));
	if (!node) {
		pthread_mutex_unlock(&lock);
		return TIDALKEEP_REJECTED;
	}
	if (!started) {
		pthread_t thread;
		if (pthread_create(&thread, NULL, worker_main, NULL) == 0) {
			pthread_detach(thread);
			started = true;
		}
	}
	if (!started) {
		free(node);
		pthread_mutex_unlock(&lock);
		return TIDALKEEP_REJECTED;
	}
	forget_failed(track->id);
	node->item = (tidalkeep_item_t){.track = *track, .quality = quality, .replace_quality = replace_existing ? kept_quality : TIDALCACHE_QUALITY_NONE,
		.playlist = playlist, .promote = promote};
	push(&queue, node);
	pthread_cond_signal(&wake);
	pthread_mutex_unlock(&lock);
	return TIDALKEEP_ADDED;
}

tidalkeep_enqueue_result_t tidalkeep_enqueue_quality(const tidal_track_t *track, int quality, bool replace) {
	// Only playlist batches create pool entries; replacing a single track
	// preserves the location of its existing copy.
	bool playlist = replace && track && tidalcache_is_playlist_only(track->id);
	return enqueue_quality(track, quality, replace, playlist, false);
}

tidalkeep_enqueue_result_t tidalkeep_enqueue_batch(const tidal_track_t *track, int quality, bool replace, bool playlist) {
	return enqueue_quality(track, quality, replace, playlist, !playlist);
}

tidalkeep_enqueue_result_t tidalkeep_enqueue(const tidal_track_t *track) {
	return enqueue_quality(track, tidalkeep_quality(), false, false, false);
}

tidalkeep_state_t tidalkeep_state(void) {
	pthread_mutex_lock(&lock);
	tidalkeep_state_t state = {
		.pending = queue.count,
		.done = done_count,
		.failed = failed.count,
		.current = current_id,
		.active = queue.head || current_id != 0,
	};
	pthread_mutex_unlock(&lock);
	return state;
}

tidalkeep_activity_t tidalkeep_activity(long track_id, const char *album_id) {
	bool by_album = !track_id && album_id && *album_id;
	if (track_id <= 0 && !by_album) return TIDALKEEP_IDLE;
	pthread_mutex_lock(&lock);
	tidalkeep_activity_t state = TIDALKEEP_IDLE;
	if (current_id && (by_album ? strcmp(current_album, album_id) == 0 : current_id == track_id)) {
		state = TIDALKEEP_ACTIVE;
	} else {
		for (keep_node_t *node = queue.head; node; node = node->next) {
			const tidal_track_t *track = &node->item.track;
			if (by_album ? strcmp(track->album_id, album_id) == 0 : track->id == track_id) {
				state = TIDALKEEP_QUEUED;
				break;
			}
		}
	}
	pthread_mutex_unlock(&lock);
	return state;
}

void tidalkeep_retry_failed(void) {
	pthread_mutex_lock(&lock);
	// Preserve the failure order and give each retry a fresh attempt budget.
	while (failed.head) {
		keep_node_t *node = pop(&failed);
		node->item.attempts = 0;
		push(&queue, node);
	}
	pthread_cond_signal(&wake);
	pthread_mutex_unlock(&lock);
}

static void *worker_main(void *unused) {
	(void)unused;
	thread_be_background("tidal keep");
	for (;;) {
		pthread_mutex_lock(&lock);
		while (!queue.head) {
			current_id = 0;
			pthread_cond_wait(&wake, &lock);
		}
		keep_node_t *node = pop(&queue);
		tidalkeep_item_t item = node->item;
		current_id = item.track.id;
		snprintf(current_album, sizeof(current_album), "%s", item.track.album_id);
		pthread_mutex_unlock(&lock);

		// Streaming keeps priority: the queue waits for any playback download
		// to finish before taking the network and the card for itself.
		tidalcache_wait_idle();

		char path[512] = "", old[512] = "";
		bool replace = item.replace_quality != TIDALCACHE_QUALITY_NONE;
		bool ok;
		if (replace) {
			// Skip an obsolete request rather than replacing a different copy.
			ok = tidalcache_kept_quality(item.track.id) != item.replace_quality ||
				!tidalcache_find(item.track.id, old, sizeof(old));
		} else if (item.promote) {
			ok = tidalcache_promote_playlist_track(&item.track);
		} else {
			ok = tidalcache_is_kept(item.track.id) || tidalcache_reuse(&item.track);
		}
		bool complete_transfer = false;
		if (!ok && (!item.promote || replace) && fetch(&item.track, item.quality, path, sizeof(path), &complete_transfer)) {
			ok = path[0] && tidalcache_keep_quality(&item.track, path, complete_transfer, item.quality,
				replace ? old : NULL, item.playlist);
		}
		if (ok) {
			tidalcache_download_extras(&item.track);
		}

		bool retry = !ok && item.attempts + 1 < TIDALKEEP_ATTEMPTS;
		if (retry) {
			// Before the re-queue, not after: the loop is free the moment the
			// lock opens, and without this the same track is fetched again at
			// once.
			sleep(TIDALKEEP_RETRY_DELAY_SECS);
			item.attempts++;
		}

		pthread_mutex_lock(&lock);
		if (current_id == item.track.id) {
			current_id = 0;
		}
		if (ok) {
			done_count++;
			free(node);
		} else {
			node->item.attempts = item.attempts;
			push(retry ? &queue : &failed, node);
		}
		bool drained = !queue.head && current_id == 0;
		tidalkeep_done_cb_t cb = done_cb;
		pthread_mutex_unlock(&lock);

		if (drained && cb) {
			cb();
		}
	}
	return NULL;
}
