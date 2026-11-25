#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "src/gui/streaming/qobuzart.h"

static atomic_int fetches, decodes;
static const char source[] = "https://fixture.invalid/cover.jpg";
static const char qobuz, podcast;

void thread_be_background(const char *name) { (void)name; }

bool http_get(const char *url, char **out, size_t *len, size_t limit, int timeout) {
	(void)limit; (void)timeout;
	assert(!strcmp(url, source));
	fetches++;
	*out = strdup("fixture");
	assert(*out);
	*len = strlen(*out);
	return true;
}

const char *http_last_error(void) { return ""; }

bool cover_load_image_file(const char *path, int w, int h, cover_fit_t fit, cover_image_t *out) {
	assert(access(path, R_OK) == 0 && w == h && fit == COVER_FIT_COVER);
	decodes++;
	out->pixels = malloc(w * h * 2);
	assert(out->pixels);
	memset(out->pixels, 0xa5, w * h * 2);
	out->dsc.data_size = w * h * 2;
	out->dsc.data = out->pixels;
	return true;
}

void cover_free(cover_image_t *image) {
	free(image->pixels);
	memset(image, 0, sizeof(*image));
}

static cover_image_t take(const void *owner, int slot) {
	cover_image_t image = {0};
	bool finished = false;
	for (int i = 0; i < 2000 && !finished; i++) {
		if (qobuzart_take(owner, slot, &image, &finished)) return image;
		nanosleep(&(struct timespec){.tv_nsec = 1000000}, NULL);
	}
	assert(!"artwork request did not return pixels");
	return image;
}

int main(int argc, char **argv) {
	assert(argc == 2);
	qobuzart_set_root(argv[1]);
	qobuzart_request(&qobuz, 0, source, 60);
	qobuzart_request(&qobuz, 1, source, 60);
	cover_image_t first = take(&qobuz, 0), second = take(&qobuz, 1);
	assert(first.pixels != second.pixels && decodes == 1 && fetches == 1);
	cover_free(&first);
	assert(second.dsc.data[0] == 0xa5);
	cover_free(&second);

	qobuzart_request(&podcast, 0, source, 60);
	first = take(&podcast, 0);
	assert(decodes == 1);
	cover_free(&first);
	qobuzart_request(&podcast, 0, source, 180);
	first = take(&podcast, 0);
	assert(decodes == 2 && first.dsc.data_size == 180 * 180 * 2);
	cover_free(&first);

	qobuzart_clear();
	qobuzart_request(&qobuz, 0, source, 60);
	first = take(&qobuz, 0);
	assert(decodes == 3 && fetches == 2);
	cover_free(&first);
	qobuzart_clear();
	return 0;
}
