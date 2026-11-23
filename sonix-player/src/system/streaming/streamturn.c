#include "streamturn.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static bool busy;

void streamturn_acquire(void) {
	pthread_mutex_lock(&lock);
	while (busy) {
		pthread_cond_wait(&changed, &lock);
	}
	busy = true;
	pthread_mutex_unlock(&lock);
}

bool streamturn_try_acquire(void) {
	// One instant, no waiting: the caller parks on its own cancellation-aware
	// clock instead of holding a thread inside this lock behind somebody
	// else's transfer. Same mutex discipline as acquire, just without the
	// wait loop.
	if (pthread_mutex_trylock(&lock) != 0) {
		return false; // somebody else holds the turn mutex: the turn is taken
	}
	if (busy) {
		pthread_mutex_unlock(&lock);
		return false;
	}
	busy = true;
	pthread_mutex_unlock(&lock);
	return true;
}

void streamturn_release(void) {
	pthread_mutex_lock(&lock);
	busy = false;
	// Broadcast, not signal: a waiter may be another download or somebody
	// parked in streamturn_wait_idle(), and waking just one leaves the other
	// stuck until someone else comes through.
	pthread_cond_broadcast(&changed);
	pthread_mutex_unlock(&lock);
}

// Four registrants in this tree -- Qobuz, Tidal, podcasts, the ABS download
// queue -- with decided margin (see streamturn.h).
//
// The abandon table: STREAMTURN_ABANDON_MAX slots for the registrants named
// in streamturn.h (qobuzcache, tidalcache, podcastcache, the ABS download
// queue) plus decided margin. Formerly exactly four -- which is also exactly
// how many services register -- so adding a fourth registrant would have
// silently dropped its abandon (register's overflow was a quiet no-op), and a
// user tap during a queue download would have waited out the whole file. The
// capacity is now raised deliberately and pinned to the registrant count by a
// build-time assertion, so both under-capacity changes fail the build.
//
// The table has its own lock: registration comes from the main thread at
// startup, from the card supervisor thread (inserting a card redoes set_root)
// and from the UI thread, while the readers are the pages' workers. Without it
// the count can become visible before the function it counts -- one preemption
// between two instructions -- and the call lands on a null pointer.
_Static_assert(STREAMTURN_ABANDON_MAX >= STREAMTURN_ABANDON_KNOWN,
			   "streamturn abandon table must hold every registered downloader");
#define ABANDON_MAX STREAMTURN_ABANDON_MAX
static pthread_mutex_t abandon_lock = PTHREAD_MUTEX_INITIALIZER;
static streamturn_abandon_fn abandon_fns[ABANDON_MAX];
static int abandon_count;

void streamturn_register_abandon(streamturn_abandon_fn fn) {
	if (!fn) {
		return;
	}
	pthread_mutex_lock(&abandon_lock);
	bool known = false;
	for (int i = 0; i < abandon_count && !known; i++) {
		known = abandon_fns[i] == fn;
	}
	if (!known && abandon_count >= ABANDON_MAX) {
		// The build-time assertion keeps the known registrants inside the
		// table; a caller past that (a new downloader that skipped the
		// constants) must be loud, not silently dropped -- a lost abandon
		// means one service's downloads can stall another's start for
		// minutes.
		fprintf(stderr, "streamturn: abandon table full (%d); '%p' is NOT registered\n",
				abandon_count, (void *)fn);
	}
	if (!known && abandon_count < ABANDON_MAX) {
		abandon_fns[abandon_count] = fn;
		abandon_count++;
	}
	pthread_mutex_unlock(&abandon_lock);
}

int streamturn_abandon_count(void) {
	pthread_mutex_lock(&abandon_lock);
	int count = abandon_count;
	pthread_mutex_unlock(&abandon_lock);
	return count;
}

void streamturn_abandon_all(void) {
	// The table is copied and the calls made outside the lock: an abandon
	// closes a socket under a blocked recv(), and must not do that while
	// holding off anyone trying to register.
	streamturn_abandon_fn copy[ABANDON_MAX];
	int count;

	pthread_mutex_lock(&abandon_lock);
	count = abandon_count;
	for (int i = 0; i < count; i++) {
		copy[i] = abandon_fns[i];
	}
	pthread_mutex_unlock(&abandon_lock);

	for (int i = 0; i < count; i++) {
		copy[i]();
	}
}

void streamturn_wait_idle(void) {
	pthread_mutex_lock(&lock);
	while (busy) {
		pthread_cond_wait(&changed, &lock);
	}
	pthread_mutex_unlock(&lock);
}
