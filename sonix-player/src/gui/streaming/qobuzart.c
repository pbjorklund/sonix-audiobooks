#include "qobuzart.h"

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "src/system/net/http.h"
#include "src/system/core/md5.h"
#include "src/system/core/utils.h"

#define ART_DIR ".local/qobuz-art"
#define ART_TIMEOUT_SECS 15
// Four megabytes, matching the player's own cover loader
// (podcastcache_write_sidecars). The two limits have to agree: podcasts
// routinely publish 3000x3000 covers, and a lower limit here would drop from
// the list an image the player still shows.
#define ART_MAX_BYTES (4 * 1024 * 1024)

typedef struct {
	char url[QOBUZART_URL_MAX];
	int size;
	bool local;

	uint32_t generation; // bumped by every request and every release
	bool pending;
	bool running;

	bool done;
	bool found;
	cover_image_t image;
} slot_t;

static slot_t slots[QOBUZART_SLOTS];
static char art_dir[512];

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wakeup = PTHREAD_COND_INITIALIZER;
static bool started;

// An encoded file on disk still needs a JPEG decode for each row. Keep the
// last few small remote thumbnails so repeated tracks and podcast episodes
// can take their own pixel buffers without re-decoding the source.
#define DECODED_SLOTS 4
#define DECODED_MAX_BYTES (120u * 120 * 2)
static struct {
	char url[QOBUZART_URL_MAX];
	int size;
	cover_image_t image;
} decoded[DECODED_SLOTS];
static unsigned decoded_next, decoded_epoch;

static void decoded_clear_locked(void) {
	for (int i = 0; i < DECODED_SLOTS; i++) cover_free(&decoded[i].image);
	decoded_next = 0;
	decoded_epoch++;
}

static bool copy_image(cover_image_t *out, const cover_image_t *image) {
	uint8_t *pixels = malloc(image->dsc.data_size);
	if (!pixels) return false;
	memcpy(pixels, image->pixels, image->dsc.data_size);
	*out = *image;
	out->pixels = pixels;
	out->dsc.data = pixels;
	return true;
}

void qobuzart_set_root(const char *sd_root) {
	char dir[sizeof(art_dir)] = "";
	if (sd_root && *sd_root) snprintf(dir, sizeof(dir), "%s/%s", sd_root, ART_DIR);
	pthread_mutex_lock(&lock);
	if (strcmp(art_dir, dir)) {
		strcpy(art_dir, dir);
		decoded_clear_locked();
	}
	pthread_mutex_unlock(&lock);
}

// The cache file is named after the MD5 of the URL. The name inside the URL is
// no good: Qobuz reuses it across the different sizes of the same album, and a
// URL fragment is not a valid file name.
static bool art_path(const char *url, char *out, size_t size) {
	pthread_mutex_lock(&lock);
	bool have_dir = art_dir[0] != '\0';
	char dir[512];
	snprintf(dir, sizeof(dir), "%s", art_dir);
	pthread_mutex_unlock(&lock);

	if (!have_dir || !url || !url[0]) {
		out[0] = '\0';
		return false;
	}

	char hash[MD5_HEX_LEN];
	md5_hex(url, strlen(url), hash);
	snprintf(out, size, "%.400s/%.32s.img", dir, hash);
	return true;
}

static bool ensure_dir(void) {
	pthread_mutex_lock(&lock);
	char dir[512];
	snprintf(dir, sizeof(dir), "%s", art_dir);
	pthread_mutex_unlock(&lock);
	if (!dir[0]) {
		return false;
	}

	for (char *p = dir + 1; *p; p++) {
		if (*p != '/') {
			continue;
		}
		*p = '\0';
		mkdir(dir, 0777);
		*p = '/';
	}
	return mkdir(dir, 0777) == 0 || errno == EEXIST;
}

// Downloads unless already cached. Written to a temporary and renamed: a
// half-written image -- power loss, network drop -- would otherwise stay there
// forever, rejected by the decoder on every load.
static bool fetch(const char *url, const char *path) {
	struct stat st;
	if (stat(path, &st) == 0 && st.st_size > 0) {
		return true;
	}
	if (!ensure_dir()) {
		return false;
	}

	char *body = NULL;
	size_t len = 0;
	if (!http_get(url, &body, &len, ART_MAX_BYTES, ART_TIMEOUT_SECS) || !body || len == 0) {
		// Log the URL and the full reason: a missing cover looks identical from
		// the outside -- a grey note -- whether the network is down, the server
		// answered 403, or the certificate failed to verify.
		const char *why = http_last_error();
		fprintf(stderr, "art: '%s' not downloaded: %s\n", url, why && why[0] ? why : "no reason reported");
		free(body);
		return false;
	}

	char temp[600];
	snprintf(temp, sizeof(temp), "%.560s.parte", path);
	FILE *f = fopen(temp, "wb");
	if (!f) {
		free(body);
		return false;
	}
	bool ok = fwrite(body, 1, len, f) == len;
	if (fclose(f) != 0) {
		ok = false;
	}
	free(body);

	if (!ok || rename(temp, path) != 0) {
		remove(temp);
		return false;
	}
	return true;
}

// Call with the lock held.
static int next_pending(void) {
	for (int i = 0; i < QOBUZART_SLOTS; i++) {
		if (slots[i].pending && !slots[i].running) {
			return i;
		}
	}
	return -1;
}

static void *worker_main(void *arg) {
	(void)arg;
	// Background priority like every other worker: a list thumbnail is not
	// worth a dropped UI frame.
	thread_be_background("qobuz art");

	pthread_mutex_lock(&lock);
	for (;;) {
		int index = next_pending();
		if (index < 0) {
			pthread_cond_wait(&wakeup, &lock);
			continue;
		}

		slot_t *slot = &slots[index];
		char url[QOBUZART_URL_MAX];
		memcpy(url, slot->url, sizeof(url));
		int size = slot->size;
		bool local = slot->local;
		uint32_t generation = slot->generation;
		unsigned epoch = decoded_epoch;
		cover_image_t image = {0};
		bool reused = false;
		if (!local) {
			for (int i = 0; i < DECODED_SLOTS; i++) {
				if (decoded[i].image.pixels && decoded[i].size == size && !strcmp(decoded[i].url, url)) {
					reused = copy_image(&image, &decoded[i].image);
					break;
				}
			}
		}

		slot->pending = false;
		slot->running = true;
		pthread_mutex_unlock(&lock);

		char path[600];
		bool found = reused;
		if (local) {
			found = cover_load_image_file(url, size, size, COVER_FIT_COVER, &image);
		} else if (!reused && art_path(url, path, sizeof(path)) && fetch(url, path)) {
			found = cover_load_image_file(path, size, size, COVER_FIT_COVER, &image);
			if (!found) {
				// Downloaded but undecodable is a different fault from never
				// downloaded, and both look like the same grey square.
				fprintf(stderr, "art: '%s' downloaded but does not decode\n", path);
			}
		}

		pthread_mutex_lock(&lock);
		slot->running = false;
		if (found && !local && !reused && epoch == decoded_epoch && image.dsc.data_size <= DECODED_MAX_BYTES) {
			cover_image_t copy;
			if (copy_image(&copy, &image)) {
				unsigned i = decoded_next++ % DECODED_SLOTS;
				cover_free(&decoded[i].image);
				decoded[i].image = copy;
				decoded[i].size = size;
				strcpy(decoded[i].url, url);
			}
		}
		if (slot->generation != generation || (!local && epoch != decoded_epoch)) {
			if (slot->generation == generation) { slot->done = true; slot->found = false; }
			// The slot was reused for something else while the fetch ran (the
			// list scrolled), so nobody wants this image any more.
			if (found) {
				cover_free(&image);
			}
		} else {
			slot->done = true;
			slot->found = found;
			slot->image = image;
		}
	}
	return NULL;
}

static void start_worker(void) {
	if (started) {
		return;
	}
	pthread_t thread;
	if (pthread_create(&thread, NULL, worker_main, NULL) == 0) {
		pthread_detach(thread);
		started = true;
	}
}

// Call with the lock held: clears the slot and frees any image it already
// holds.
static void reset_locked(slot_t *slot) {
	slot->generation++;
	slot->pending = false;
	if (slot->done && slot->found) {
		cover_free(&slot->image);
	}
	slot->done = false;
	slot->found = false;
	memset(&slot->image, 0, sizeof(slot->image));
}

// Who currently owns the slot pool.
//
// There is one pool of QOBUZART_SLOTS slots and two pages using it. Without an
// owner each page would believe it held every slot: one page's rebuild would
// cancel the other's downloads, images would be drawn on the wrong page's rows,
// and slots still in flight would never be collected, leaving the cover timer
// running for the rest of the session.
//
// The owner is any pointer, as long as it is stable and differs between the two
// pages. Requesting a cover claims ownership and clears what was there; a page
// that is no longer the owner gets "finished, nothing to take" from
// qobuzart_take(), which is what lets it stop asking and shut down its timer.
static const void *pool_owner;

static void take_pool_locked(const void *owner) {
	if (pool_owner == owner) {
		return;
	}
	for (int i = 0; i < QOBUZART_SLOTS; i++) {
		reset_locked(&slots[i]);
		slots[i].url[0] = '\0';
	}
	pool_owner = owner;
}

static void request_art(const void *owner, int slot_index, const char *url, int size, bool local) {
	if (slot_index < 0 || slot_index >= QOBUZART_SLOTS || !url || !url[0] || size <= 0) {
		return;
	}

	pthread_mutex_lock(&lock);
	take_pool_locked(owner);
	start_worker();

	slot_t *slot = &slots[slot_index];
	// Skip the fetch only if the same image is ready and still held here.
	//
	// Still held matters: qobuzart_take() hands the image over and the slot
	// stops owning it, so without the found check reopening the same list would
	// drop the request as already done with nothing left to hand over, and the
	// rows would stay grey.
	if (slot->done && slot->found && slot->size == size && slot->local == local && strcmp(slot->url, url) == 0) {
		pthread_mutex_unlock(&lock);
		return;
	}

	reset_locked(slot);
	snprintf(slot->url, sizeof(slot->url), "%s", url);
	slot->size = size;
	slot->local = local;
	slot->pending = true;
	pthread_cond_signal(&wakeup);
	pthread_mutex_unlock(&lock);
}

void qobuzart_request(const void *owner, int slot_index, const char *url, int size) {
	request_art(owner, slot_index, url, size, false);
}

void qobuzart_request_local(const void *owner, int slot_index, const char *path, int size) {
	request_art(owner, slot_index, path, size, true);
}

void qobuzart_release(const void *owner, int slot_index) {
	if (slot_index < 0 || slot_index >= QOBUZART_SLOTS) {
		return;
	}
	pthread_mutex_lock(&lock);
	// A page that no longer owns the pool must not release slots: they belong
	// to someone else now, and clearing them would cancel their covers.
	if (pool_owner == owner) {
		reset_locked(&slots[slot_index]);
		slots[slot_index].url[0] = '\0';
	}
	pthread_mutex_unlock(&lock);
}

bool qobuzart_take(const void *owner, int slot_index, cover_image_t *out, bool *finished) {
	if (finished) {
		*finished = false;
	}
	if (slot_index < 0 || slot_index >= QOBUZART_SLOTS || !out) {
		return false;
	}

	pthread_mutex_lock(&lock);
	if (pool_owner != owner) {
		// The pool moved to the other page. Reporting finished is correct:
		// nothing will ever arrive, and the caller has to be able to stop
		// polling instead of waiting forever.
		pthread_mutex_unlock(&lock);
		if (finished) {
			*finished = true;
		}
		return false;
	}
	slot_t *slot = &slots[slot_index];
	if (!slot->done) {
		pthread_mutex_unlock(&lock);
		return false;
	}

	if (finished) {
		*finished = true;
	}
	bool found = slot->found;
	if (found) {
		*out = slot->image;
		// Ownership passes to the caller from here: the slot no longer frees
		// this image.
		slot->found = false;
		memset(&slot->image, 0, sizeof(slot->image));
	}
	pthread_mutex_unlock(&lock);
	return found;
}

void qobuzart_clear(void) {
	pthread_mutex_lock(&lock);
	decoded_clear_locked();
	char dir[512];
	snprintf(dir, sizeof(dir), "%s", art_dir);
	for (int i = 0; i < QOBUZART_SLOTS; i++) {
		reset_locked(&slots[i]);
		slots[i].url[0] = '\0';
	}
	pool_owner = NULL;
	pthread_mutex_unlock(&lock);

	if (!dir[0]) {
		return;
	}
	DIR *d = opendir(dir);
	if (!d) {
		return;
	}
	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		if (e->d_name[0] == '.') {
			continue;
		}
		char path[640];
		if ((size_t)snprintf(path, sizeof(path), "%s/%s", dir, e->d_name) < sizeof(path)) {
			remove(path);
		}
	}
	closedir(d);
}
