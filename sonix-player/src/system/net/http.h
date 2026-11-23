#ifndef HTTP_H
#define HTTP_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/socket.h>

// A very small HTTP/1.1 client.
//
// http:// goes over a plain socket. https:// goes over the OpenSSL the device
// already ships, opened at run time -- see tls.h for why it is loaded that way
// and where the certificates come from. When OpenSSL or the certificate bundle
// is missing an https:// URL fails with a reason the user can act on, and
// everything http:// carries on working.
//
// A growing share of the stations in the radio directory are https-only.
//
// Two shapes of request:
//
//   http_get()         a whole small response into memory -- the API's JSON.
//   http_stream_open() headers read, body left on the socket -- a radio
//                      stream, which never ends and must not be buffered.
//
// Everything blocks, so nothing here may be called from the UI thread. Both
// calls take a timeout in seconds that bounds the connect and every read, so a
// server that accepts the connection and then says nothing cannot wedge the
// caller's thread forever -- which is exactly what a dead radio station does.

// What the server is told this client is. radio-browser.info asks for a
// descriptive agent of the form appname/version.
#define HTTP_USER_AGENT "SonixPlayer/1.0"

// ---------------------------------------------------------------------------
// The active-operation slot (Bob §8.5 line 1028/1157; §8.2 line 463)
//
// A caller whose HTTP work sits behind a cancellable command (today: the
// Audiobookshelf download queue's pause/cancel, whose pause must not wait out
// a 30 s receive timeout) hands every blocking operation one of these slots.
// The op is the `abs_client_op_t` of the spec under its next-wave name: it
// carries a cancellation flag, a result generation, and a mutex-published
// active connection, and cancellation wakes the exact blocked read through
// the connection itself.
//
// The contract, in the order the spec states it:
//
//   * The OWNER (the worker thread doing the HTTP) publishes the in-flight
//     connection's fd around connect, the TLS handshake, the request write,
//     the header read, and every buffered/streaming body read, and un-publishes
//     before that fd can be closed. Publication takes the slot mutex only for
//     the pointer/state write -- never across a network wait.
//   * Another thread (the GUI) calls http_op_cancel(): the flag is set and the
//     published connection, if any, is shut down. A blocked recv()/SSL_read on
//     that fd returns at once, so a paused download does not ride out its
//     receive timeout. The owner keeps sole read/close ownership: wake never
//     closes, frees or dereferences anything.
//   * Because un-publish happens under the same mutex as publish and always
//     before close, a wake can never shut down an fd that has been closed and
//     re-opened by someone else (fd reuse) -- a wake captures a published fd
//     only while the owner still owns that connection.
//   * `generation` is the result generation: the owner bumps it when an
//     operation begins on the slot, and a cancel records which generation it
//     landed on. An owner that must classify a failed read asks
//     http_op_woken_current(): true means this generation's read was ended by
//     a deliberate wake, false means the network failed on its own.
//   * DNS stays honest (§8.2 line 463): getaddrinfo cannot be interrupted, so
//     a cancel during resolver time is only a flag until the resolver's own
//     bounded timeout returns; the owner then checks the flag and stops BEFORE
//     connecting. The queue reports that phase as "Stopping" rather than
//     pretending DNS can be cut short. Connect itself, the TLS handshake and
//     every read are wakeable.
//
// The slot lives with the owner (one download worker, one active transfer),
// is passed to the op-aware entry points below, and is safe to cancel from
// any other thread at any moment, published or not.
typedef struct {
	pthread_mutex_t mutex; // guards the four fields below, nothing else
	volatile int cancelled;
	volatile unsigned generation;     // bumped at each operation begin
	volatile unsigned wake_generation; // the generation a cancel landed on
	volatile int active_fd;           // published in-flight fd; -1 = none
} http_op_t;

// Lifecycle. Everything is inline and mutex-guarded on purpose: the slot must
// be usable from modules that do not link http.c at all (the download worker
// owns the slot, the queue tests publish into it) while http.c publishes the
// same fields inside its own calls. Every function below is cheap and does
// no I/O.

static inline void http_op_init(http_op_t *op) {
	if (!op) {
		return;
	}
	pthread_mutex_init(&op->mutex, NULL);
	op->cancelled = 0;
	op->generation = 0;
	op->wake_generation = 0;
	op->active_fd = -1;
}

static inline void http_op_destroy(http_op_t *op) {
	if (!op) {
		return;
	}
	pthread_mutex_destroy(&op->mutex);
}

// Owner, between attempts on a reusable slot: clears a consumed/overrun
// cancellation flag so a stop directed at the previous attempt cannot leak
// into the next one. wake_generation is re-anchored to the current generation
// so no stale wake can ever look like this attempt's own.
static inline void http_op_clear(http_op_t *op) {
	if (!op) {
		return;
	}
	pthread_mutex_lock(&op->mutex);
	op->cancelled = 0;
	op->wake_generation = op->generation;
	op->active_fd = -1;
	pthread_mutex_unlock(&op->mutex);
}

// Owner, at the start of an op-aware HTTP call: the result generation moves,
// and a wake from here on belongs to this operation.
static inline void http_op_begin(http_op_t *op) {
	if (!op) {
		return;
	}
	pthread_mutex_lock(&op->mutex);
	op->generation++;
	pthread_mutex_unlock(&op->mutex);
}

static inline bool http_op_cancelled(http_op_t *op) {
	if (!op) {
		return false;
	}
	pthread_mutex_lock(&op->mutex);
	bool c = op->cancelled != 0;
	pthread_mutex_unlock(&op->mutex);
	return c;
}

// True when a cancel/wake is directed at the operation the owner is currently
// running (§8.5 "result generation"): the owner's read failed and THIS
// generation is the one the wake hit. The cheap, non-classifying form is
// http_op_cancelled().
static inline bool http_op_woken_current(http_op_t *op) {
	if (!op) {
		return false;
	}
	pthread_mutex_lock(&op->mutex);
	bool w = op->cancelled != 0 && op->wake_generation == op->generation;
	pthread_mutex_unlock(&op->mutex);
	return w;
}

static inline unsigned http_op_generation(http_op_t *op) {
	if (!op) {
		return 0;
	}
	pthread_mutex_lock(&op->mutex);
	unsigned g = op->generation;
	pthread_mutex_unlock(&op->mutex);
	return g;
}

// Another thread stops the operation: the flag rises and the published
// connection -- if one is in flight right now -- is shut down so the blocked
// read/write returns immediately. The owner still owns read/close; this only
// wakes the connection, like http_stream_wake() but through the slot and only
// while an fd is genuinely published.
static inline void http_op_cancel(http_op_t *op) {
	if (!op) {
		return;
	}
	pthread_mutex_lock(&op->mutex);
	op->cancelled = 1;
	op->wake_generation = op->generation;
	int fd = op->active_fd;
	if (fd >= 0) {
		shutdown(fd, SHUT_RDWR); // wakes recv()/SSL_read at once; owner still closes
	}
	pthread_mutex_unlock(&op->mutex);
}

// Owner: publish the in-flight connection's fd for the duration of one
// blocking network phase (connect, handshake, request write, header read, one
// buffered/streaming body read), then un-publish before that fd can be closed.
// Both under the slot mutex, so a concurrent wake captures a published fd only
// while the owner still owns the connection -- never a closed-reused number.
static inline void http_op_publish_fd(http_op_t *op, int fd) {
	if (!op) {
		return;
	}
	pthread_mutex_lock(&op->mutex);
	op->active_fd = fd;
	pthread_mutex_unlock(&op->mutex);
}

static inline void http_op_unpublish(http_op_t *op) {
	if (!op) {
		return;
	}
	pthread_mutex_lock(&op->mutex);
	op->active_fd = -1;
	pthread_mutex_unlock(&op->mutex);
}

// Test/diagnostic observation (§8.5 line 588's optional hooks): true while the
// owner is INSIDE one published network phase, i.e. at an instant where a
// cancel would wake a genuinely blocked wait. *fd_out (may be NULL) receives
// the published fd. The wake tests use it to synchronize a cancellation onto
// the blocked window without wall-clock guesses.
static inline bool http_op_published(http_op_t *op, int *fd_out) {
	if (!op) {
		return false;
	}
	pthread_mutex_lock(&op->mutex);
	bool p = op->active_fd >= 0;
	if (p && fd_out) {
		*fd_out = op->active_fd;
	}
	pthread_mutex_unlock(&op->mutex);
	return p;
}

// Fetches `url` into a freshly malloc'd, NUL-terminated buffer. The caller
// frees it. `out_len` may be NULL. Redirects (301/302/303/307/308) are
// followed, up to HTTP_MAX_REDIRECTS, and may cross between http and https in
// either direction -- which real redirects do constantly.
//
// Returns false on any failure, leaving *out untouched. `limit` caps how much
// body is kept; a longer response is truncated rather than allowed to eat the
// heap.
#define HTTP_MAX_REDIRECTS 5
bool http_get(const char *url, char **out, size_t *out_len, size_t limit, int timeout_secs);

// The same, for callers talking to an API rather than a radio station.
//
//   extra_headers   lines already terminated with "\r\n", one per header, or
//                   NULL. Qobuz wants the app credentials in there.
//   want_error_body true keeps the body of a 4xx instead of discarding it: that
//                   is where an API says why it refused, which is what should
//                   be shown instead of a generic failure. 5xx and empty bodies
//                   remain failures.
//   status_out      the status code, or NULL.
bool http_get_ex(const char *url, const char *extra_headers, bool want_error_body, char **out, size_t *out_len,
				 size_t limit, int timeout_secs, int *status_out);

// Where the request ended up, which is not always where it started: radio CDNs
// answer an HLS manifest with a 302 to an edge (msvdn.net does it every
// session), and the relative URLs inside the manifest resolve against the edge,
// not the requested host. After a successful http_get()/http_request() this
// holds the URL of the last hop -- per thread, like http_last_error(), and
// overwritten by the next request on the same thread, so copy it immediately.
const char *http_last_final_url(void);

// HTTP Date from the latest response on this thread, in UTC milliseconds (0 if absent).
long long http_last_server_time_ms(void);

// ---------------------------------------------------------------------------
// Requests that are not GET
//
// Qobuz gets by with GET alone -- even adding a favourite is a GET with query
// parameters, which is their choice. Tidal does not: login is a POST with a
// form body, favourites are written with POST and removed with DELETE, and a
// successful DELETE answers 204 with no body.
//
// That last point is why a dedicated flag exists. Everywhere else in the player
// a 200 with no body is a failure -- a radio station that accepts the
// connection and sends nothing is broken, and treating it as success would hand
// the decoder an empty buffer. But 204 is the right answer to removing a
// favourite, and the caller must be able to say so without changing behaviour
// for everyone else.
typedef struct {
	// "GET", "POST", "DELETE". NULL means GET.
	const char *method;

	// Lines already terminated with "\r\n", one per header, or NULL.
	const char *extra_headers;

	// The body, or NULL. `body_len` 0 with a non-NULL body means strlen(),
	// which is the normal case since these bodies are text.
	const char *body;
	size_t body_len;

	// The body's type. NULL with a body present means
	// "application/x-www-form-urlencoded", which is what both Tidal's login
	// endpoint and its writes expect.
	const char *content_type;

	// Keep the body of a 4xx instead of discarding it: that is where the API
	// says why it refused. See http_get_ex().
	bool want_error_body;

	// Read success from the status code rather than the content. Two effects,
	// both needed by the same kind of caller:
	//
	//   * any 2xx counts as success, not just 200. Creating something answers
	//     201, which without this would read as a failure;
	//   * an empty body counts as a response. A successful DELETE answers 204
	//     and sends nothing; and a 4xx with no body -- which happens -- would
	//     otherwise surface as "no response" instead of its code, hiding a 401
	//     from a caller waiting for it to refresh the token.
	//
	// With this set *out is still allocated and terminated: an empty string,
	// never NULL, so the caller always frees the same way.
	bool allow_empty_body;

	// The caller's active-operation slot (above), when this request must be
	// cancellable mid-flight. NULL (the default for every zero-initialised
	// request) keeps the historical, non-wakeable behavior of every existing
	// caller. The owner keeps the slot valid for the whole call; http.c
	// publishes the connection through it around connect, handshake, request
	// write, headers and every body read, and honors the cancellation flag.
	http_op_t *op;

} http_req_t;

// Like http_get_ex(), but with the method and body chosen by the caller.
// Redirects are followed as usual; a 303, and a 301/302 on a POST, become a GET
// to the new destination, which is what browsers do and what servers expect.
bool http_request(const char *url, const http_req_t *req, char **out, size_t *out_len, size_t limit, int timeout_secs,
				  int *status_out);

// How much is read at once while looking for the end of the headers. The
// release-download redirect from github.com carries over 5 KB of them, with
// the Location past the fourth kilobyte and close to a kilobyte long.
#define HTTP_HEADER_BUFFER 8192

// A stream whose body is read a piece at a time.
typedef struct {
	int fd;

	// The TLS session riding on that fd, or NULL for a plain http:// stream.
	// Opaque here on purpose: http.h is included in places that have no
	// business knowing OpenSSL exists.
	void *tls;

	// From the response headers, when the server sent them.
	char content_type[64]; // e.g. "audio/mpeg"
	char icy_name[128];	   // the station's own name for itself
	char icy_genre[128];
	int icy_metaint; // bytes of audio between metadata blocks, 0 when none

	// Content-Length, or 0 when the server does not send it -- always the case
	// for a radio station, and almost never for a downloaded file. 64-bit on
	// purpose (§8.4): the target's `long` is 32 bits, and a >2 GB audiobook
	// file must not wrap into a wrong progress bar or a wrong length check.
	long long content_length;

	// Body bytes already read while parsing the headers. The first read has
	// to hand these back before touching the socket again.
	//
	// This MUST be at least as big as the buffer the header parser reads
	// into: one TCP segment can carry the headers and several kilobytes of
	// body, and a lead buffer too small to hold the overshoot loses those
	// bytes. On a radio stream that loses an ICY block boundary -- the
	// metadata counter desynchronises and the decoder is handed metadata as if
	// it were audio.
	char lead[HTTP_HEADER_BUFFER];
	int lead_len;
	int lead_pos;

	// Where the ICY metadata counter is within the current block. Only
	// meaningful when icy_metaint > 0.
	int meta_countdown;

	// The last "StreamTitle" the station sent, or empty. Updated in place by
	// http_stream_read(); the caller polls it.
	char stream_title[256];
	bool title_changed;
} http_stream_t;

// Connects, sends the request (asking for ICY metadata), reads the response
// headers and stops there. Returns false if the URL is neither http:// nor
// https://, the host does not resolve, the connection or the TLS handshake
// fails, or the status is not 200.
bool http_stream_open(http_stream_t *st, const char *url, int timeout_secs);

// The authenticated form. `extra_headers` is the same CRLF-terminated list
// accepted by http_get_ex(); it is repeated after same-origin redirects and
// dropped if a redirect changes scheme, host or port. Needed for media
// endpoints such as Audiobookshelf, where the body is too large to buffer.
bool http_stream_open_ex(http_stream_t *st, const char *url, const char *extra_headers, int timeout_secs);

// The same, cancellable. `op` is the caller's active-operation slot: the open
// publishes the connection around connect, handshake, request write and the
// header read, so http_op_cancel() wakes a blocked open at once. When the
// server answers with a non-2xx status, `status_out` (if given) receives it --
// a download retry policy can tell a refused token (401/403) from missing
// media (404) without re-reading headers. Redirection and auth stripping are
// exactly the non-op form's.
bool http_stream_open_ex_op(http_stream_t *st, const char *url, const char *extra_headers, int timeout_secs,
							http_op_t *op, int *status_out);

// Reads up to `len` bytes of *audio* into buf: any ICY metadata blocks in the
// way are consumed and parsed out, never handed to the caller. Returns the
// number of bytes read, 0 at end of stream, -1 on error.
int http_stream_read(http_stream_t *st, void *buf, int len);

// The cancellable form: each blocking body read is wrapped in the slot's
// publish/unpublish window and preceded by an http_op_cancelled() check, so a
// cancel both wakes a blocked read at once and stops the loop before the next
// one. The stream (and its op) stay the owner's: nothing here closes anything.
int http_stream_read_op(http_stream_t *st, void *buf, int len, http_op_t *op);

void http_stream_close(http_stream_t *st);

// Wakes a reader blocked in http_stream_read() from another thread: shuts the
// socket down so the pending recv() returns at once instead of sitting out the
// receive timeout. The stream stays valid (its owner still has to close it);
// every read from here on simply fails.
//
// It works the same under TLS, and for the same reason: SSL_read is sitting in
// a recv() on that fd like everything else, and a shut-down socket ends it.
//
// This is what makes stopping a station immediate: without it a station whose
// server has gone quiet holds its thread for the whole receive timeout.
void http_stream_wake(http_stream_t *st);

// Why the last http_get() or http_stream_open() on THIS thread failed, when
// the reason is one a person could act on -- a device clock so wrong that
// every certificate looks expired, a missing certificate bundle, a name that
// does not match. NULL when there is nothing useful to say, which is the usual
// case for an ordinary network failure.
//
// Per thread on purpose: the radio's directory worker and its playback thread
// both use this file and must not overwrite each other's answer.
const char *http_last_error(void);

// Percent-encodes `in` into `out` for use inside a URL path segment or query
// value. Returns out.
char *http_url_encode(const char *in, char *out, size_t out_size);

#endif /* HTTP_H */
