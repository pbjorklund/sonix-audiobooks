// Focused WS3 (§9.3/§9.4) host tests for the M4 book-detail waves: the §9.3
// detail model/parser and the pure description helper it shares with local
// ingestion (wave 1), plus the authenticated §9.1/§9.3 cover subsystem
// (wave 2): the 2 MiB encoded cap, the hashed cache name, the LRU ceilings
// of eight files / 16 MiB, quiescence/cancellation, and the real decode path
// the detail and full-screen viewers feed on (cover_load_image_file, linked
// through the objcopy trick below).
//
// Offline mode (default): an http_request() stub with a canned body pins the
// request mapping (GET /api/items/{id}?expanded=1&include=progress,authors
// with the URL-encoded id) byte for byte, and every parse rule runs against
// canned JSON: field extraction, description plain-vs-HTML/entity
// equivalence, chapter bounds 0/1/400/401 with the overflow row, invalid
// times, long strings, malformed JSON, and the ownership invariants (freed
// models, free-of-NULL, exact chapter heap, no inline download tracks in
// the wrapper). A second stub, for the streaming entry points
// abs_cover_fetch() reads, does the same for the cover: pinning the §9.1
// endpoint (GET /api/items/{id}/cover?format=jpeg&width=900&height=1280 with
// the pinned Bearer), serving programmable bodies for the cap/LRU/cancel
// cases, and serving a real JPEG for the decode ones.
//
// Live mode (-DABS_DETAIL_TEST_LIVE): the real HTTP stack against the
// provenance mock, once per checked-in fixture alias, parsing the REAL
// expanded detail bytes the fixtures record (bk-full/bk-minimal, the
// 0/1/400/401-chapter books, mock-book) with every §9.1 field, and fetching
// the cover into the real cache layout (the mock's request log carries the
// authenticated request; the cache hit then asks for nothing).
//
// No GUI and no LVGL here: the §9.2 screen is exercised by the committed
// GUI smokes (audiobookshelf-host-smoke.sh now walks the detail for all of
// its download/link/play modes), and the wrong-server binding truth table
// the detail's Play/Link decisions rest on lives in tools/
// audiobookshelf-absdirs-test.c. ASan's leak check IS the repeated
// fetch/decode/evict leak gate: any cycle that leaks pixels or encoded
// buffers fails the suite at exit.

#include "src/system/core/config.h"
#include "src/system/core/sha256.h"
#include "src/gui/nowplaying/cover.h"
#include "src/system/net/http.h"
#include "src/system/streaming/abs_cover.h"
#include "src/system/streaming/abs_profiles.h"
#include "src/system/streaming/audiobookshelf.h"
#include "src/system/library/description.h"

#include <assert.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static const char *work; // argv[1]
#define STUB_SERVER "http://stub.invalid"

// The §9.3 display chapter: id + two stamps + 192-byte title = exact 216 B
// on every ABI here (the §9.5 budget line multiplies it out).
_Static_assert(sizeof(abs_detail_chapter_t) == 216, "§9.3 chapter record no longer 216 bytes");

// Largest caller-side scratch any test needs.
static audiobookshelf_item_t scratch_item;
static char scratch_path[1280];

#ifndef ABS_DETAIL_TEST_LIVE

// ---------------------------------------------------------------------------
// Offline network stub: capture the URL, answer with the queued body.
// ---------------------------------------------------------------------------

static struct {
	char url[2048];
	char body[98304]; // 401 chapters plus metadata must fit
} spy;

// §9.5 cap-shape emulation (F3): when armed, the stub answers with exactly
// the caller's cap in bytes of the canned body extended with filler — the
// wire result of an oversize Content-Length (and of a close-delimited body at
// cap+1) — with the http-layer truncation signal set. One delivery per
// request, like the fail-status stub.
static bool spy_oversize;
static bool spy_truncated;
static size_t spy_delivered;

bool http_last_truncated(void) { return spy_truncated; }

bool http_request(const char *url, const http_req_t *req, char **out, size_t *out_len,
				  size_t limit, int timeout_secs, int *status_out) {
	(void)timeout_secs;
	snprintf(spy.url, sizeof(spy.url), "%s", url ? url : "");
	if (spy_oversize && limit > 0) {
		spy_oversize = false;
		spy_truncated = true;
		spy_delivered = limit;
		char *body = malloc(limit + 1);
		assert(body);
		size_t prefix = strlen(spy.body);
		if (prefix > limit) prefix = limit;
		memcpy(body, spy.body, prefix);
		memset(body + prefix, 'x', limit - prefix);
		body[limit] = '\0';
		*out = body;
		if (out_len) *out_len = limit;
		if (status_out) *status_out = 200;
		assert(req && req->extra_headers &&
			   strncmp(req->extra_headers, "Authorization: Bearer ", 21) == 0);
		return true;
	}
	spy_truncated = false;
	if (status_out) *status_out = 200;
	size_t len = strlen(spy.body);
	spy_delivered = len;
	char *body = malloc(len + 1);
	assert(body);
	memcpy(body, spy.body, len + 1);
	*out = body;
	if (out_len) *out_len = len;
	// The real api_request() resolves credentials through the profile store
	// before any URL leaves; the stub pins the Bearer reaching this point.
	assert(req && req->extra_headers && strncmp(req->extra_headers, "Authorization: Bearer ", 21) == 0);
	return true;
}

const char *http_last_error(void) { return ""; }
long long http_last_server_time_ms(void) { return 0; }

// ---------------------------------------------------------------------------
// Streaming stub for abs_cover_fetch() (§9.3): cap what is served, drive it
// from a program, and pin the §9.1 endpoint reaching this point.
// ---------------------------------------------------------------------------

static struct {
	char url[2200];
	char headers[1100];
	long long opens;
} stream_spy;

static struct {
	size_t length;               // patterned body length
	unsigned char base;          // first pattern byte; byte i = base + i
	const char *raw;             // when set: exact bytes (a real JPEG)
	size_t raw_len;
	const volatile int *cancel;  // external flag the read stub flips
	size_t cancel_after;         // ... after this many served bytes
	bool die_mid_body;           // read returns -1 after one chunk
	size_t pos;                  // served so far
	bool died;
} stream_body;

static void stream_program(size_t length, unsigned char base) {
	memset(&stream_body, 0, sizeof(stream_body));
	stream_body.length = length;
	stream_body.base = base;
}

static void stream_program_raw(const char *raw, size_t len) {
	memset(&stream_body, 0, sizeof(stream_body));
	stream_body.raw = raw;
	stream_body.raw_len = len;
}

bool http_stream_open_ex(http_stream_t *st, const char *url, const char *extra_headers, int timeout_secs) {
	(void)timeout_secs;
	snprintf(stream_spy.url, sizeof(stream_spy.url), "%s", url ? url : "");
	snprintf(stream_spy.headers, sizeof(stream_spy.headers), "%s", extra_headers ? extra_headers : "");
	stream_spy.opens++;
	memset(st, 0, sizeof(*st));
	st->content_length = (long long)(stream_body.raw ? stream_body.raw_len : stream_body.length);
	if (stream_body.length == 0 && !stream_body.raw) st->content_length = 0;
	// Every open serves the same programmed body from byte zero, the way a
	// fresh HTTP response would.
	stream_body.pos = 0;
	stream_body.died = false;
	return true;
}

int http_stream_read(http_stream_t *st, void *buf, int len) {
	(void)st;
	if (stream_body.die_mid_body && stream_body.pos > 0 && !stream_body.died) {
		// One clean chunk, then the socket dies mid-body.
		stream_body.died = true;
		return -1;
	}
	size_t total = stream_body.raw ? stream_body.raw_len : stream_body.length;
	if (stream_body.pos >= total) return 0;
	if (stream_body.cancel && stream_body.cancel_after > 0 &&
		stream_body.pos + (size_t)len > stream_body.cancel_after) {
		// A pause/cancel from another thread lands mid-transfer.
		__atomic_store_n((volatile int *)stream_body.cancel, 1, __ATOMIC_SEQ_CST);
	}
	size_t want = total - stream_body.pos;
	if (want > (size_t)len) want = (size_t)len;
	unsigned char *out = buf;
	if (stream_body.raw) {
		memcpy(out, stream_body.raw + stream_body.pos, want);
	} else {
		for (size_t i = 0; i < want; i++) out[i] = stream_body.base + (unsigned char)(stream_body.pos + i);
	}
	stream_body.pos += want;
	return (int)want;
}

void http_stream_close(http_stream_t *st) {
	if (st) st->fd = -1;
}

static const char *captured_path(void) {
	size_t skip = strlen(STUB_SERVER);
	const char *path = spy.url;
	if (strncmp(path, STUB_SERVER, skip) != 0) return "(no request was captured)";
	return path + skip;
}

#endif // !ABS_DETAIL_TEST_LIVE

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------

static void expect_str(const char *what, const char *actual, const char *want) {
	if (strcmp(actual, want) != 0) {
		fprintf(stderr, "FAIL %s:\n  actual: %s\n  want:   %s\n", what, actual, want);
		exit(1);
	}
}

static void expect_bool(const char *what, bool actual, bool want) {
	if (actual != want) {
		fprintf(stderr, "FAIL %s: got %d want %d\n", what, actual, want);
		exit(1);
	}
}

static void expect_int(const char *what, long actual, long want) {
	if (actual != want) {
		fprintf(stderr, "FAIL %s: got %ld want %ld\n", what, actual, want);
		exit(1);
	}
}

static void expect_true(const char *what, bool actual) { expect_bool(what, actual, true); }

// http.c's failure texts go through the translation table; this harness has
// no GUI language layer (the browse-test harness stubs it the same way).
const char *tr(const char *text) { return text; }

// One profile store in the work directory; the snapshot is the only
// credential path the detail fetch uses (§11.3).
static void configure_profile(const char *server, const char *token) {
	char path[1024];
	snprintf(path, sizeof(path), "%s/profiles.ini", work);
	unsetenv("SONIX_AUDIOBOOKSHELF_PROFILES");
	setenv("SONIX_AUDIOBOOKSHELF_PROFILES", path, 1);
	unsetenv("SONIX_AUDIOBOOKSHELF_SERVER");
	unsetenv("SONIX_AUDIOBOOKSHELF_TOKEN");
	assert(abs_profiles_init() == 0);
	char id[ABS_PROFILE_ID_MAX];
	assert(abs_profiles_add("Test", server, token, id, sizeof(id)));
}

// ---------------------------------------------------------------------------
// Cover helpers (wave 2)
// ---------------------------------------------------------------------------

static int dir_file_count(const char *dir) {
	DIR *d = opendir(dir);
	if (!d) return -1;
	int count = 0;
	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		if (e->d_name[0] == '.') continue;
		count++;
	}
	closedir(d);
	return count;
}

static bool dir_has_file(const char *dir, const char *suffix) {
	DIR *d = opendir(dir);
	if (!d) return false;
	bool found = false;
	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		if (e->d_name[0] == '.') continue;
		if (suffix && strstr(e->d_name, suffix)) found = true;
	}
	closedir(d);
	return found;
}

// Stamps BOTH timestamps to `sec` so the eviction order is test-owned: the
// fetch's own touch uses "now", and the crafted stamps sit far in the past.
static void craft_atime(const char *path, long long sec) {
	struct timespec stamps[2] = {{sec, 0}, {sec, 0}};
	assert(utimensat(AT_FDCWD, path, stamps, 0) == 0);
}

static long long file_size(const char *path) {
	struct stat st;
	return stat(path, &st) == 0 ? (long long)st.st_size : -1;
}

static bool file_bytes_equal(const char *path, const char *data, size_t len) {
	if (file_size(path) != (long long)len) return false;
	FILE *f = fopen(path, "rb");
	if (!f) return false;
	char *got = malloc(len + 1);
	assert(got);
	size_t read = fread(got, 1, len + 1, f);
	fclose(f);
	bool same = read == len && memcmp(got, data, len) == 0;
	free(got);
	return same;
}

static bool is_hex(char c) { return strchr("0123456789abcdef", c) != NULL; }

// The pinned browsing-identity snapshot (§11.3) the cover fetch pins.
static void cover_credentials(abs_auth_snapshot_t *snap, const char *token) {
	assert(audiobookshelf_browse_credentials(snap));
	assert(snap->token[0] && snap->server[0]);
	if (token) expect_str("browse credentials token", snap->token, token);
	expect_str("browse credentials server", snap->server, STUB_SERVER);
}

// The base64 of the smallest real baseline RGB JPEG the fixtures use
// (identical policy to tools/abs_fixtures_generate.py: deterministic bytes,
// no runtime dependency). The decode tests prove the fetched cache file is
// decodable by the exact loader the §9.2 screen and the full-screen viewer
// use afterwards.
static const char *const COVER_JPEG_B64 =
	"/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAMCAgMCAgMDAwMEAwMEBQgFBQQEBQoHBwYIDAoM"
	"DAsKCwsNDhIQDQ4RDgsLEBYQERMUFRUVDA8XGBYUGBIUFRT/2wBDAQMEBAUEBQkFBQkUDQsN"
	"FBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBT/wAAR"
	"CAAwACADASIAAhEBAxEB/8QAHwAAAQUBAQEBAQEAAAAAAAAAAAECAwQFBgcICQoL/8QAtRAA"
	"AgEDAwIEAwUFBAQAAAF9AQIDAAQRBRIhMUEGE1FhByJxFDKBkaEII0KxwRVS0fAkM2JyggkK"
	"FhcYGRolJicoKSo0NTY3ODk6Q0RFRkdISUpTVFVWV1hZWmNkZWZnaGlqc3R1dnd4eXqDhIWG"
	"h4iJipKTlJWWl5iZmqKjpKWmp6ipqrKztLW2t7i5usLDxMXGx8jJytLT1NXW19jZ2uHi4+Tl"
	"5ufo6erx8vP09fb3+Pn6/8QAHwEAAwEBAQEBAQEBAQAAAAAAAAECAwQFBgcICQoL/8QAtREA"
	"AgECBAQDBAcFBAQAAQJ3AAECAxEEBSExBhJBUQdhcRMiMoEIFEKRobHBCSMzUvAVYnLRChYk"
	"NOEl8RcYGRomJygpKjU2Nzg5OkNERUZHSElKU1RVVldYWVpjZGVmZ2hpanN0dXZ3eHl6goOE"
	"hYaHiImKkpOUlZaXmJmaoqOkpaanqKmqsrO0tba3uLm6wsPExcbHyMnK0tPU1dbX2Nna4uPk"
	"5ebn6Onq8vP09fb3+Pn6/9oADAMBAAIRAxEAPwD85NF0D7vy13ui6B935at6LoH3flrvdF0D"
	"7vy16lGB7PD2bfDqU9F0D7vy13ui6B935auaLoH3flrvdF0D7vy17lGB/TvD2bfDqeI6LoH3"
	"flrvNF0D7vy1c0XQPu/LXe6LoH3flriowP8APvh3Nvh1Kei6B935a73RdA+78tW9F0D7vy13"
	"ui6B935a9ujA/p3h3Nvh1PEdF0D7vy13ui6B935auaLoH3flrvNF0D7vy1xUYH+ffDubfDqV"
	"NF0D7vy1tzt5H+i2v+t6SSD+D2Hv/L69Lc7eRm1tf9b0kkH8PsPf+X16aOi6B935a/nvxD8R"
	"vq/PkmST9/apUX2e8Yv+b+aX2dl713H+tuEsVflqVH6I/9k=";
static unsigned char cover_jpeg[2048];
static size_t cover_jpeg_len;

static void decode_base64(const char *in, unsigned char *out, size_t out_size, size_t *len_out) {
	size_t o = 0;
	unsigned acc = 0;
	int bits = 0;
	for (; *in; in++) {
		if (*in == '=') break;
		int v;
		if (*in >= 'A' && *in <= 'Z') v = *in - 'A';
		else if (*in >= 'a' && *in <= 'z') v = *in - 'a' + 26;
		else if (*in >= '0' && *in <= '9') v = *in - '0' + 52;
		else if (*in == '+') v = 62;
		else if (*in == '/') v = 63;
		else continue; // ignored padding between lines
		acc = (acc << 6) | (unsigned)v;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			if (o >= out_size) break;
			out[o++] = (unsigned char)(acc >> bits);
		}
	}
	*len_out = o;
}

// ---------------------------------------------------------------------------
// Offline: the §9.1/§9.3 parse rules against canned JSON
// ---------------------------------------------------------------------------

#ifndef ABS_DETAIL_TEST_LIVE

static void mkdir_s(const char *path) {
	char cmd[1200];
	snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", path);
	assert(system(cmd) == 0);
}

static void write_file(const char *path, const char *text) {
	FILE *f = fopen(path, "w");
	assert(f);
	fputs(text, f);
	fclose(f);
}

// The pure helper (§9.4, description.h/.c): the exact local policy
// metadata.c used to own, now shared with the detail parse. The first two
// cases pin the strings the committed audiobook catalog smoke checks in the
// scanned database, so helper and scanner can never drift apart quietly.
static void description_helper_tests(void) {
	char out[DESCRIPTION_TEXT_MAX + 8];
	size_t n;

	n = description_from_html("<p>A host-test <b>description</b> &amp; details.</p>", 48, out, sizeof(out));
	expect_bool("local-smoke description identical", n == strlen("A host-test description & details."), true);
	expect_str("local-smoke description identical", out, "A host-test description & details.");

	n = description_from_html("<p>A host-test summary for the standalone M4B.</p>", 50, out, sizeof(out));
	expect_str("local-smoke standalone identical", out, "A host-test summary for the standalone M4B.");

	// Paragraph/line tags break a line; style wrappers vanish silently.
	n = description_from_html("One<p>Two</p><br/>Three<li>Four<div>Five</div>", 46, out, sizeof(out));
	expect_str("paragraph/line breaks", out, "One\nTwo\nThree\nFour\nFive");

	// Entities decode; an unknown entity stays literal byte-for-byte.
	const char *entities = "&amp;&quot;&#39;&apos;&lt;&gt;&nbsp;X&yuml;";
	char decoded[64];
	n = description_from_html(entities, strlen(entities), decoded, sizeof(decoded));
	expect_str("entity set", decoded, "&\"''<> X&yuml;");
	expect_bool("entity set length", n == strlen("&\"''<> X&yuml;"), true);
	(void)n;

	// CR becomes LF; blank-line runs collapse; tabs become spaces (a literal
	// space after one stays -- the policy only drops a space at a line
	// start); trailing whitespace trims; embedded NUL ends the scan.
	n = description_from_html("A\r\n B\t C\n\n\nD\0E  ", 14, out, sizeof(out));
	expect_str("control/newline policy", out, "A\nB  C\nD");

	// A tag with no '>' anywhere stays literal (no closed tag is invented) --
	// while a '<' with a '>' later DOES swallow the range in between, exactly
	// as the local loop always did ("a <3 <b>bold" arrives as "a bold": the
	// scanner cannot tell prose from markup; publishers' HTML is the input).
	n = description_from_html("a <3 and <b", 11, out, sizeof(out));
	expect_str("unclosed < stays literal", out, "a <3 and <b");
	n = description_from_html("a <3 <b>bold", 12, out, sizeof(out));
	expect_str("range to the next > is dropped", out, "a bold");

	// The plain policy (for descriptionPlain): no tag or entity reading at
	// all, only the control/newline rules.
	n = description_from_plain("A & B < C\r\n\r\n\tD", 15, out, sizeof(out));
	expect_str("plain keeps & and <", out, "A & B < C\nD");
	n = description_from_plain("<p>&amp;</p>", strlen("<p>&amp;</p>"), out, sizeof(out));
	expect_str("plain never decodes", out, "<p>&amp;</p>");

	// §9.1 selection: prefer plain; fall back when it is empty or whitespace.
	n = description_pick("From plain", "<p>From raw</p>", out, sizeof(out));
	expect_bool("pick prefers plain", n == 10, true);
	expect_str("pick prefers plain", out, "From plain");
	n = description_pick("", "<p>From raw</p>", out, sizeof(out));
	expect_str("pick falls back", out, "From raw");
	n = description_pick("   ", "<b>Fall</b>back", out, sizeof(out));
	expect_str("pick whitespace falls back", out, "Fallback");
	n = description_pick(NULL, NULL, out, sizeof(out));
	expect_bool("pick nothing", n == 0, true);
	expect_str("pick nothing", out, "");

	// Bounding: the scan stops when out is one byte short of full; nothing
	// overruns its buffer and nothing is read past what the caller allows.
	char small[8];
	n = description_from_html("abcdefghij", 10, small, sizeof(small));
	expect_int("bounded html write", (long)n, 7); // terminator reserved
	expect_str("bounded html write", small, "abcdefg");
	n = description_from_plain("abcdef&gh", 9, small, sizeof(small));
	expect_int("bounded plain write", (long)n, 7);
	// Zero-length and zero-room callers are refused, nothing written.
	expect_bool("zero room refused", description_from_html("x", 1, out, 0) == 0, true);
	expect_bool("zero len refused", description_from_html("", 0, out, sizeof(out)) == 0, true);
	// A capacity that covers the input NEVER truncates: the cleanup cannot
	// grow the text (one byte in, at most one byte out).
	char exact[10 + 3];
	n = description_from_html("xx<br>yy</p>&amp;", strlen("xx<br>yy</p>&amp;"), exact, 10);
	expect_str("exact-capacity result", exact, "xx\nyy\n&");

	// The detail-parse-level description equivalence (§9.7 "plain vs
	// HTML/entities"): the same book through both wire fields arrives
	// byte-identically after the cleanup.
	char html_out[DESCRIPTION_TEXT_MAX + 1], plain_out[DESCRIPTION_TEXT_MAX + 1];
	const char *html = "Line one &amp;&nbsp;two<br><p>Line &lt;three&gt;&#39;s</p>";
	const char *plain = "Line one & two\nLine <three>'s";
	description_from_html(html, strlen(html), html_out, sizeof(html_out));
	description_from_plain(plain, strlen(plain), plain_out, sizeof(plain_out));
	expect_str("plain-vs-HTML equivalence", html_out, plain_out);
}

// The §9.1 expanded request, byte for byte, and the exact model fields off
// the modern wire shape.
static void request_and_field_tests(void) {
	// §9.1: narrator/series/description/duration/author/progress, with the
	// modern narratorName key.
	snprintf(spy.body, sizeof(spy.body),
			 "{\"id\":\"it-full\","
			 "\"media\":{\"coverPath\":\"/metadata/Cache/items/it-full/cover.jpg\","
			 "\"metadata\":{\"title\":\"Detail Book\",\"authorName\":"
			 "\"Fixture Detail Author\",\"narratorName\":\"Fixture Narrator\","
			 "\"descriptionPlain\":\"Plain summary.\",\"description\":"
			 "\"<p>Never seen: raw</p>\",\"series\":[{\"name\":\"Series A\",\"sequence\":\"1.5\"}],"
			 "\"seriesName\":\"Series A\"},\"duration\":3000.0,"
			 "\"chapters\":[{\"id\":1,\"start\":0.0,\"end\":100.0,\"title\":\"One\"}]},"
			 "\"userMediaProgress\":{\"currentTime\":150.0,\"isFinished\":false,\"lastUpdate\":7}}");
	audiobookshelf_detail_t *d = audiobookshelf_detail("it-full");
	assert(d);
	expect_str("§9.1 request path", captured_path(),
			  "/api/items/it-full?expanded=1&include=progress,authors");
	expect_str("title", d->item.title, "Detail Book");
	expect_str("author", d->item.author, "Fixture Detail Author");
	expect_str("narrator", d->narrator, "Fixture Narrator");
	expect_str("descriptionPlain preferred (raw HTML never shown)", d->description, "Plain summary.");
	expect_str("series name", d->item.series, "Series A");
	// §9.1: the sequence is a STRING; "1.5" must survive verbatim.
	expect_str("series sequence verbatim", d->series_part, "1.5");
	expect_bool("duration", d->item.duration == 3000.0, true);
	expect_bool("progress", d->item.progress == 150.0, true);
	expect_bool("finished flag", d->item.finished, false);
	expect_int("chapter count", d->chapter_count, 1);
	expect_bool("no overflow", d->chapters_overflow, false);
	expect_bool("cover slot parsed for wave 2", d->item.cover[0] != '\0', true);
	audiobookshelf_detail_free(d);

	// The v2.3x wire names the checked-in fixtures record: `narrator`,
	// `index`/`startOffset`/`endOffset` chapters, and the authors[0] author
	// fallback. The modern names always win when both are present.
	snprintf(spy.body, sizeof(spy.body),
			 "{\"id\":\"it-old\","
			 "\"media\":{\"metadata\":{\"title\":\"Old Shape\",\"description\":\"desc\","
			 "\"narrator\":\"Narrator Old\",\"authors\":[{\"name\":\"Author Old\"}],"
			 "\"series\":[{\"name\":\"Series B\",\"sequence\":\"12\"}]},"
			 "\"chapters\":[{\"index\":1,\"startOffset\":5.0,\"endOffset\":9.0,\"title\":\"One\"},"
			 "{\"index\":2,\"startOffset\":9.0,\"endOffset\":12.0,\"title\":\"Two\"}]}}");
	d = audiobookshelf_detail("it-old");
	assert(d);
	expect_str("narrator v23x fallback", d->narrator, "Narrator Old");
	expect_str("author authors[0] fallback", d->item.author, "Author Old");
	expect_str("series v23x", d->item.series, "Series B");
	expect_str("sequence verbatim (integer-looking)", d->series_part, "12");
	expect_int("v23x chapter fallback shape", d->chapter_count, 2);
	expect_bool("v23x start", d->chapters[0].start == 5.0, true);
	expect_bool("v23x end", d->chapters[0].end == 9.0, true);
	expect_bool("v23x id falls to index", d->chapters[1].id == 2, true);
	audiobookshelf_detail_free(d);

	// §9.1: no narrator -> the row hides (the model leaves it empty and the
	// GUI decides); no series, no description, no progress: all empty.
	snprintf(spy.body, sizeof(spy.body),
			 "{\"id\":\"it-bare\",\"media\":{\"metadata\":{\"title\":\"Bare\"},"
			 "\"duration\":45.5,\"chapters\":[]}}");
	d = audiobookshelf_detail("it-bare");
	assert(d);
	expect_str("no narrator", d->narrator, "");
	expect_str("no series", d->item.series, "");
	expect_str("no sequence", d->series_part, "");
	expect_str("no description", d->description, "");
	expect_bool("no progress", d->item.progress == 0, true);
	expect_int("no chapters", d->chapter_count, 0);
	expect_bool("no overflow for an empty list", d->chapters_overflow, false);
	expect_bool("no chapters heap allocated", d->chapters == NULL, true);
	audiobookshelf_detail_free(d);
}

// §9.1's description rule: descriptionPlain preferred; otherwise the raw
// description cleaned with the SAME policy local descriptions run through.
static void description_tests(void) {
	snprintf(spy.body, sizeof(spy.body),
			 "{\"id\":\"it-d\",\"media\":{\"metadata\":{\"title\":\"D\","
			 "\"description\":\"<p>First &amp; line</p><br>Second&nbsp;&lt;item&gt;"
			 "&#39;s</p>\\r\\n\\r\\n\\tThird\"}}}");
	audiobookshelf_detail_t *d = audiobookshelf_detail("it-d");
	assert(d);
	expect_str("HTML description cleaned", d->description,
			   "First & line\nSecond <item>'s\nThird");
	audiobookshelf_detail_free(d);

	snprintf(spy.body, sizeof(spy.body),
			 "{\"id\":\"it-d2\",\"media\":{\"metadata\":{\"title\":\"D2\","
			 "\"descriptionPlain\":\"A & B < C already plain\",\"description\":\"<p>ignored</p>\"}}}");
	d = audiobookshelf_detail("it-d2");
	assert(d);
	expect_str("plain wins and keeps & <", d->description, "A & B < C already plain");
	audiobookshelf_detail_free(d);

	// All-whitespace plainText still falls back to the raw HTML (§9.1
	// "otherwise"): a whitespace-only plain is nothing to show.
	snprintf(spy.body, sizeof(spy.body),
			 "{\"id\":\"it-d3\",\"media\":{\"metadata\":{\"title\":\"D3\","
			 "\"descriptionPlain\":\"   \",\"description\":\"<b>Fall</b>back\"}}}");
	d = audiobookshelf_detail("it-d3");
	assert(d);
	expect_str("whitespace plain falls back", d->description, "Fallback");
	audiobookshelf_detail_free(d);
}

// §9.1 chapter bounds: 0 / 1 / 400 / 401, the overflow row, numChapters as
// presentation evidence, the (start,id) display sort, and the invalid-time
// untimed title.
static void chapter_tests(void) {
	char body[98304];

	// A helper-rendered corpus: `count` chapters, ids 1..count, each an
	// ordered 300 s slice. The ids are deliberately reversed server-side to
	// prove the (start,id) display sort.
	const int counts[] = {0, 1, 400, 401};
	for (int ti = 0; ti < 4; ti++) {
		int count = counts[ti];
		snprintf(body, sizeof(body), "{\"id\":\"it-c\",\"media\":{\"metadata\":{\"title\":\"C\"},");
		size_t off = strlen(body);
		snprintf(body + off, sizeof(body) - off, "\"chapters\":[");
		for (int i = 0; i < count; i++) {
			int index = count - i - 1; // server sends end-to-start
			off = strlen(body);
			snprintf(body + off, sizeof(body) - off,
					 "%s{\"id\":%d,\"start\":%.1f,\"end\":%.1f,\"title\":\"Chapter %d\"}",
					 i ? "," : "", index + 1, index * 300.0, (index + 1) * 300.0, index + 1);
		}
		off = strlen(body);
		snprintf(body + off, sizeof(body) - off, "]}}");
		snprintf(spy.body, sizeof(spy.body), "%s", body);
		audiobookshelf_detail_t *d = audiobookshelf_detail("it-c");
		assert(d);
		int kept = count < 400 ? count : 400;
		char what[64];
		snprintf(what, sizeof(what), "chapters kept of %d", count);
		expect_int(what, d->chapter_count, kept);
		snprintf(what, sizeof(what), "overflow for %d", count);
		expect_bool(what, d->chapters_overflow, count > 400);
		if (kept > 1) {
			// Kept = the first `kept` in SERVER order (here end-to-start);
			// sorted ascending once parsed, whatever order that was (§9.1:
			// the server's order is not trusted on screen).
			expect_bool("display sorted ascending(ish)",
						d->chapters[0].start < d->chapters[kept - 1].start, true);
			for (int k = 1; k < kept; k++) {
				if (d->chapters[k].start < d->chapters[k - 1].start) {
					expect_bool("display strictly ascending", false, true);
				}
			}
			// The kept window's smallest start: the last one the server sent.
			snprintf(what, sizeof(what), "sort window first of %d", count);
			expect_bool(what, d->chapters[0].start == (count - kept) * 300.0, true);
		}
		audiobookshelf_detail_free(d);
	}

	// numChapters (§9.1) only ever feeds the overflow presentation: the
	// list is the source and a server counting more than it sent is honest
	// overflow evidence for the GUI's overflow row.
	snprintf(spy.body, sizeof(spy.body),
			 "{\"id\":\"it-nc\",\"media\":{\"metadata\":{\"title\":\"NC\"},\"numChapters\":5,"
			 "\"chapters\":[{\"id\":1,\"start\":0.0,\"end\":300.0,\"title\":\"One\"}]}}");
	audiobookshelf_detail_t *d = audiobookshelf_detail("it-nc");
	assert(d);
	expect_int("numChapters never fabricates rows", d->chapter_count, 1);
	expect_bool("numChapters evidence", d->chapters_overflow, true);
	audiobookshelf_detail_free(d);

	// §9.1 invalid ordering: end before start -> the title stays, the times
	// zero out (an untimed row), the valid part of the list is untouched,
	// and nothing in chapter parsing ever reaches local playback.
	snprintf(spy.body, sizeof(spy.body),
			 "{\"id\":\"it-bad\",\"media\":{\"metadata\":{\"title\":\"Bad\"},"
			 "\"chapters\":[{\"id\":3,\"start\":600.0,\"end\":900.0,\"title\":\"Valid\"},"
			 "{\"id\":1,\"start\":300.0,\"end\":100.0,\"title\":\"Reversed\"},"
			 "{\"id\":2,\"start\":50.0,\"end\":50.0,\"title\":\"Zero Length\"},"
			 "{\"id\":4,\"start\":900.0,\"end\":900.0,\"title\":\"Still Zero\"}]}}");
	d = audiobookshelf_detail("it-bad");
	assert(d);
	expect_int("all titles kept", d->chapter_count, 4);
	expect_bool("no overflow flag from a bad pair", d->chapters_overflow, false);
	bool reversed_untimed = false;
	for (int i = 0; i < d->chapter_count; i++) {
		if (strcmp(d->chapters[i].title, "Reversed") == 0) {
			reversed_untimed = d->chapters[i].start == 0.0 && d->chapters[i].end == 0.0;
		}
	}
	expect_true("§9.1 invalid pair is an untimed title", reversed_untimed);
	// Sorted by (start,id): the untimed row leads, and a zero-DURATION
	// (legitimately ordered) chapter keeps its stamps -- only end<start is
	// an invalid ordering.
	expect_bool("sort keeps untimed first", d->chapters[0].id == 1, true);
	bool stamps_kept = false;
	for (int i = 0; i < d->chapter_count; i++) {
		if (strcmp(d->chapters[i].title, "Zero Length") == 0)
			stamps_kept = d->chapters[i].start == 50.0 && d->chapters[i].end == 50.0;
	}
	expect_true("zero-length keeps its stamps", stamps_kept);
	audiobookshelf_detail_free(d);
}

// §9.3 bounds under hostile strings: nothing overruns its field, the text
// never exceeds its ceiling, and truncation never produces raw HTML.
static void long_string_tests(void) {
	char long_value[12000];
	memset(long_value, 'x', sizeof(long_value) - 1);
	long_value[sizeof(long_value) - 1] = '\0';
	char body[56000];
	snprintf(body, sizeof(body),
			 "{\"id\":\"it-long\",\"media\":{\"metadata\":{\"title\":\"%s\",\"narratorName\":\"%s\","
			 "\"description\":\"%s\",\"series\":[{\"name\":\"%s\",\"sequence\":\"%s\"}]}}}",
			 long_value, long_value, "<b>x</b>", long_value, long_value);
	snprintf(spy.body, sizeof(spy.body), "%s", body);

	audiobookshelf_detail_t *d = audiobookshelf_detail("it-long");
	assert(d);
	expect_int("title bounded", (long)strlen(d->item.title), ABS_NAME_MAX - 1);
	expect_int("narrator bounded", (long)strlen(d->narrator), ABS_NAME_MAX - 1);
	// The raw publisher markup never survives the cleanup, whatever the
	// truncation did to it: "<b>x</b>" arrives as "x".
	expect_str("truncated description carries no raw markup", d->description, "x");
	expect_int("series name bounded", (long)strlen(d->item.series), ABS_NAME_MAX - 1);
	expect_int("sequence bounded", (long)strlen(d->series_part), 31);
	audiobookshelf_detail_free(d);

	// A long PLAIN description: normalized output stops at the ceiling; and
	// the model's own description field is exactly the §9.3 byte shape.
	snprintf(body, sizeof(body),
			 "{\"id\":\"it-long2\",\"media\":{\"metadata\":{\"title\":\"T\",\"descriptionPlain\":\"%s\"}}}",
			 long_value);
	snprintf(spy.body, sizeof(spy.body), "%s", body);
	d = audiobookshelf_detail("it-long2");
	assert(d);
	expect_int("plain description bounded", (long)strlen(d->description), ABS_DETAIL_DESCRIPTION_MAX);
	audiobookshelf_detail_free(d);
}

static void failure_tests(void) {
	// Malformed JSON: NULL with a sanitized engine error, never a crash.
	snprintf(spy.body, sizeof(spy.body), "{\"id\":\"it-x\",\"media\":{\"metadata\":{\"title\":");
	audiobookshelf_detail_t *d = audiobookshelf_detail("it-x");
	expect_bool("malformed JSON refused", d == NULL, true);
	expect_str("sanitized error", audiobookshelf_last_error(),
			   "Audiobookshelf returned invalid book details.");

	expect_bool("NULL id refused", audiobookshelf_detail(NULL) == NULL, true);
	expect_bool("empty id refused", audiobookshelf_detail("") == NULL, true);
	// The refusal messages carry no request material at all.
	expect_bool("no URL in the refusal", strstr(audiobookshelf_last_error(), "/api") == NULL, true);
	audiobookshelf_detail_free(NULL); // the free contract is NULL-safe
}

// §9.5 (F3): a detail body cut at the 3 MiB cap is a bounded display outcome,
// not a parse failure. The stub delivers cap bytes of a longer body with the
// truncation signal set — byte for byte what http.c does for an oversize
// Content-Length — and the detail becomes the note model: non-NULL, flagged,
// no invented fields, no refusal error. A complete body under the cap keeps
// its generic refusal (the two must never be confused).
static void detail_overflow_tests(void) {
	// The cap-cut shape: the truncation signal, then the note model. The API
	// layer passes the shared 3 MiB bound down; the stub's delivered record
	// pins that handoff.
	spy_delivered = 0;
	spy_oversize = true;
	audiobookshelf_detail_t *d = audiobookshelf_detail("it-overflow");
	expect_true("§9.5 cap cut delivers the note model", d != NULL);
	expect_bool("details_too_large set", d->details_too_large, true);
	expect_bool("no refusal error on the note model", audiobookshelf_last_error()[0] == '\0', true);
	// The JSON never terminated, so nothing was invented: the fields stay
	// empty and the GUI falls back to the list row identity.
	expect_str("no parsed title on the note model", d->item.title, "");
	expect_int("no fabricated chapter rows", d->chapter_count, 0);
	expect_bool("no chapters allocated on the note model", d->chapters == NULL, true);
	expect_bool("cap handoff: the transport got exactly the documented bound",
				spy_delivered == 3u * 1024u * 1024u, true);
	audiobookshelf_detail_free(d);

	// The generic refusal for a COMPLETE invalid body is unchanged: the two
	// outcomes stay distinguishable.
	snprintf(spy.body, sizeof(spy.body), "{\"id\":\"it-bad\",\"media\":");
	d = audiobookshelf_detail("it-bad");
	expect_bool("complete invalid body still refused", d == NULL, true);
	expect_str("still the generic error", audiobookshelf_last_error(),
			   "Audiobookshelf returned invalid book details.");
	expect_bool("generic refusal carries no truncation signal", http_last_truncated(), false);
}

// What the detail view never carries: no inline track array, and the heap
// the chapters use is a single exact block of the §9.3 record size.
static void ownership_tests(void) {
	char body[98304];
	snprintf(body, sizeof(body),
			 "{\"id\":\"it-own\",\"media\":{\"metadata\":{\"title\":\"Own\"},"
			 "\"numTracks\":3,\"tracks\":["
			 "{\"index\":1,\"startOffset\":0,\"duration\":10,\"title\":\"T1\","
			 "\"contentUrl\":\"/api/items/it-own/file/1\",\"mimeType\":\"audio/mpeg\",\"metadata\":{\"filename\":\"T1.mp3\"}},"
			 "{\"index\":2,\"startOffset\":10,\"duration\":10,\"title\":\"T2\","
			 "\"contentUrl\":\"/api/items/it-own/file/2\",\"mimeType\":\"audio/mpeg\",\"metadata\":{\"filename\":\"T2.mp3\"}},"
			 "{\"index\":3,\"startOffset\":20,\"duration\":10,\"title\":\"T3\","
			 "\"contentUrl\":\"/api/items/it-own/file/3\",\"mimeType\":\"audio/mpeg\",\"metadata\":{\"filename\":\"T3.mp3\"}}],"
			 "\"chapters\":[{\"id\":1,\"start\":0.0,\"end\":30.0,\"title\":\"Whole\"}]}}");
	snprintf(spy.body, sizeof(spy.body), "%s", body);
	audiobookshelf_detail_t *d = audiobookshelf_detail("it-own");
	assert(d);
	// The wrapper's item keeps the LIST-facing fields read_item fills; the
	// model itself adds NO track storage to reach for (compile time proves
	// the shape; this pins the runtime surface the GUI can see stays small).
	_Static_assert(sizeof(audiobookshelf_detail_t) < 96 * 1024,
				   "§9.3 detail wrapper left the §9.5 budget on this ABI");
	printf("detail-test: size of the detail wrapper on this ABI: %zu bytes, "
		   "full model with 400 chapters: %zu bytes (%.1f KiB of the 96 KiB target)\n",
		   sizeof(audiobookshelf_detail_t),
		   sizeof(audiobookshelf_detail_t) + 400 * sizeof(abs_detail_chapter_t),
		   (double)(sizeof(audiobookshelf_detail_t) + 400 * sizeof(abs_detail_chapter_t)) / 1024.0);
	expect_int("chapters are exact heap, not inline", d->chapter_count, 1);
	audiobookshelf_detail_free(d); // chapters + object die here (ASan checks)
}

// The two-server rule underneath the detail's Play/Link decisions: one
// local manifest, two profiles -- the §11.5 truth the worker's mapping runs
// on (the full truth table lives in audiobookshelf-absdirs-test.c; this is
// the detail-shaped slice: same item id, two servers).
// ---------------------------------------------------------------------------
// Wave 2 (§9.1/§9.3): the authenticated cover cache
// ---------------------------------------------------------------------------

// §9.1 endpoint, pinned Bearer, hashed cache name with no token in it, byte
// round-trip, and the cache hit that asks for nothing.
static void cover_basic_tests(void) {
	abs_cover_set_root(work);
	char err[256] = "", path[512] = "";
	abs_auth_snapshot_t snap;
	cover_credentials(&snap, "stub-token");

	stream_program(8192, 0x41);
	expect_true("cover fetch succeeds", abs_cover_fetch(&snap, "it-cover", NULL, path, sizeof(path), err, sizeof(err)));
	expect_bool("cover error empty on success", err[0] == '\0', true);
	expect_str("§9.1 cover request path", stream_spy.url,
			  STUB_SERVER "/api/items/it-cover/cover?format=jpeg&width=900&height=1280");
	expect_str("§9.1 cover Bearer pinned", stream_spy.headers, "Authorization: Bearer stub-token\r\n");

	const char *name = strrchr(path, '/');
	assert(name);
	name++;
	expect_bool("hashed cache name length", strlen(name) == 12 + 1 + 12 + 4, true);
	expect_bool("hashed cache name separator", name[12] == '-', true);
	for (int i = 0; i < 25; i++) {
		if (i == 12) continue;
		expect_bool("hashed cache name is hex", is_hex(name[i]), true);
	}
	expect_bool("hashed cache name extension", strcmp(name + 25, ".jpg") == 0, true);
	expect_bool("cache layout root", strstr(path, "/.local/abs-cover-cache/") != NULL, true);
	// The name carries two hashes and nothing else: neither the token, nor
	// any fragment of the request, ever reaches the card.
	expect_bool("no token in the cached path", strstr(path, "stub-token") == NULL, true);
	expect_bool("no header material in the path", strstr(path, "Bearer") == NULL, true);

	char expected[8192];
	for (size_t i = 0; i < sizeof(expected); i++) expected[i] = (char)(0x41 + (unsigned char)i);
	expect_int("cover bytes complete", (long)file_size(path), 8192);
	expect_true("cover bytes round-trip", file_bytes_equal(path, expected, 8192));
	{
		char dir[600];
		snprintf(dir, sizeof(dir), "%s/.local/abs-cover-cache", work);
		bool part = false;
		DIR *d = opendir(dir);
		if (d) {
			struct dirent *e;
			while ((e = readdir(d)) != NULL)
				if (strstr(e->d_name, ".part")) part = true;
			closedir(d);
		}
		expect_bool("no .part in the cache after success", part, false);
	}

	// The cache hit answers from the card and asks for nothing (§9.3).
	long long opens_after_fetch = stream_spy.opens;
	char hit[512] = "";
	expect_true("cover cache hit succeeds", abs_cover_fetch(&snap, "it-cover", NULL, hit, sizeof(hit), err, sizeof(err)));
	expect_str("cache hit returns the same file", hit, path);
	expect_bool("cache hit made no request", stream_spy.opens == opens_after_fetch, true);
}

// §9.1's 2 MiB encoded cap: a stated-oversize response is refused before one
// byte lands; a body that grows past the cap is stopped mid-stream; exactly
// the cap stores. Both refusals are sanitized.
static void cover_cap_tests(void) {
	abs_auth_snapshot_t snap;
	cover_credentials(&snap, NULL);

	char err[256] = "", path[512] = "";
	stream_program((size_t)ABS_COVER_ENCODED_MAX + 1, 0x20);
	expect_bool("encode cap refuses oversize", !abs_cover_fetch(&snap, "it-big", NULL, path, sizeof(path), err, sizeof(err)), true);
	expect_bool("oversize refusal is sanitized", err[0] != 0, true);
	expect_bool("no URL in the oversize refusal", strstr(err, "/api") == NULL, true);
	expect_bool("no token in the oversize refusal", strstr(err, "stub-token") == NULL, true);
	expect_bool("oversize leaves no file", file_size(path) < 0, true);

	stream_program((size_t)ABS_COVER_ENCODED_MAX, 0x7E);
	expect_true("encode exactly 2 MiB stores", abs_cover_fetch(&snap, "it-exact", NULL, path, sizeof(path), err, sizeof(err)));
	expect_int("exactly-cap file size", (long)file_size(path), ABS_COVER_ENCODED_MAX);
}

// Cancellation and a dead socket: both stop between read units, both leave
// no file, both are sanitized.
static void cover_cancel_tests(void) {
	abs_auth_snapshot_t snap;
	cover_credentials(&snap, NULL);
	char err[256] = "", path[512] = "";

	volatile int cancel = 0;
	stream_program(60000, 0x33);
	memset(&stream_body.cancel_after, 0, sizeof(stream_body.cancel_after));
	stream_body.cancel = &cancel;
	stream_body.cancel_after = 30000;
	expect_bool("cancellation stops the fetch",
				!abs_cover_fetch(&snap, "it-cancel", &cancel, path, sizeof(path), err, sizeof(err)), true);
	expect_bool("cancelled refusal names it", strstr(err, "cancel") != NULL, true);
	expect_bool("cancel leaves no file", file_size(path) < 0, true);
	stream_body.cancel = NULL;
	stream_body.cancel_after = 0;

	// stream_program() resets the whole program: death is armed AFTER it.
	stream_program(90000, 0x44);
	stream_body.die_mid_body = true;
	expect_bool("a dead socket interrupts the fetch",
				!abs_cover_fetch(&snap, "it-died", NULL, path, sizeof(path), err, sizeof(err)), true);
	expect_bool("interrupt refusal is sanitized", err[0] != 0 && strstr(err, "stub-token") == NULL, true);
	stream_body.die_mid_body = false;
}

// §9.3 LRU: eight files kept, the ninth evicts the oldest USED; a re-open
// refreshes a file's recency; the byte ceiling evicts exactly the same way at
// exactly/over 16 MiB.
static void cover_lru_tests(void) {
	char err[256] = "", path[512], item[32];
	abs_auth_snapshot_t snap;
	cover_credentials(&snap, NULL);

	char lru_root[600];
	snprintf(lru_root, sizeof(lru_root), "%s/lru", work);
	mkdir_s(lru_root);
	abs_cover_set_root(lru_root);

	long long base = (long long)time(NULL) - 100000;
	char files[10][600];

	// Nine fetches; each cached file is stamped with its own, increasing
	// "last use" so the eviction order is deterministic.
	stream_program(8192, 0x10);
	for (int i = 0; i < 9; i++) {
		snprintf(item, sizeof(item), "it-l%d", i);
		expect_true("lru fetch stores", abs_cover_fetch(&snap, item, NULL, path, sizeof(path), err, sizeof(err)));
		snprintf(files[i], sizeof(files[i]), "%s", path);
		craft_atime(path, base + i);
	}
	char dir[700];
	snprintf(dir, sizeof(dir), "%s/.local/abs-cover-cache", lru_root);
	expect_int("nine fetches keep eight files", dir_file_count(dir), 8);
	expect_bool("the oldest file was evicted", file_size(files[0]) < 0, true);
	for (int i = 1; i < 9; i++) expect_bool("the kept eight survived", file_size(files[i]) > 0, true);

	// LRU is by use, not by age: re-opening the second cache file makes IT
	// untouchable and the next fetch evicts the then-oldest (l2).
	long long opens_before = stream_spy.opens;
	expect_true("re-open of l1 is a cache hit", abs_cover_fetch(&snap, "it-l1", NULL, path, sizeof(path), err, sizeof(err)));
	expect_bool("re-open made no request", stream_spy.opens == opens_before, true);
	expect_true("fetched l9 stores", abs_cover_fetch(&snap, "it-l9", NULL, path, sizeof(path), err, sizeof(err)));
	expect_int("still eight files after the tenth", dir_file_count(dir), 8);
	expect_bool("the then-oldest (l2) was evicted", file_size(files[2]) < 0, true);
	expect_bool("recently-used l1 survived", file_size(files[1]) > 0, true);
	expect_bool("the fresh file survived pruning", file_size(path) > 0, true);

	// Byte ceiling, exactly 16 MiB: each cached cover is at most the §9.1
	// 2 MiB encoded cap, so the ceiling shape is 8 × 2 MiB. Eight fills
	// WITHOUT eviction (exactly the ceiling); nothing is dropped at the edge.
	char byte_root[600];
	snprintf(byte_root, sizeof(byte_root), "%s/bytes", work);
	mkdir_s(byte_root);
	abs_cover_set_root(byte_root);
	char byte_dir[700];
	snprintf(byte_dir, sizeof(byte_dir), "%s/.local/abs-cover-cache", byte_root);
	stream_program((size_t)ABS_COVER_ENCODED_MAX, 0x50);
	char byte_files[10][600];
	for (int i = 0; i < 8; i++) {
		snprintf(item, sizeof(item), "it-b%d", i);
		expect_true("exactly-16 fetch stores", abs_cover_fetch(&snap, item, NULL, path, sizeof(path), err, sizeof(err)));
		expect_int("capped cover file size", (long)file_size(path), ABS_COVER_ENCODED_MAX);
		snprintf(byte_files[i], sizeof(byte_files[i]), "%s", path);
		craft_atime(path, base + 200 + i);
	}
	expect_bool("the exactly-16-MiB ceiling keeps all eight", dir_file_count(byte_dir) == 8, true);

	// One more capped cover pushes to 18 MiB (over 16): the oldest by use
	// goes, exactly one, and the fresh file is never its own eviction.
	expect_true("ninth fetch stores", abs_cover_fetch(&snap, "it-b9", NULL, path, sizeof(path), err, sizeof(err)));
	snprintf(byte_files[8], sizeof(byte_files[8]), "%s", path);
	craft_atime(path, base + 300);
	expect_bool("over 16 MiB evicts back to eight", dir_file_count(byte_dir) == 8, true);
	expect_bool("evicted the oldest by use", file_size(byte_files[0]) < 0, true);
	expect_bool("the fresh oversize ceiling cover survived", file_size(byte_files[8]) > 0, true);
	(void)byte_root;
}

// Card close and unconfigured snapshots refuse before any network or card
// write, and re-attach restores them.
static void cover_lifecycle_tests(void) {
	abs_auth_snapshot_t snap;
	cover_credentials(&snap, NULL);
	char err[256] = "", path[512] = "";

	abs_cover_quiesce();
	stream_program(1024, 0x21);
	expect_bool("card close refuses the fetch",
				!abs_cover_fetch(&snap, "it-card", NULL, path, sizeof(path), err, sizeof(err)), true);
	expect_bool("card-close refusal is sanitized", err[0] != 0 && strstr(err, "stub-token") == NULL, true);
	expect_bool("card close wrote nothing", file_size(path) < 0, true);

	abs_cover_set_root(work); // card back: same restore shape card_databases_attach() runs
	expect_true("card re-attach restores the cache", abs_cover_fetch(&snap, "it-card", NULL, path, sizeof(path), err, sizeof(err)));

	abs_auth_snapshot_t broken = {.server = "", .token = ""};
	expect_bool("no login refuses the fetch",
				!abs_cover_fetch(&broken, "it-x", NULL, path, sizeof(path), err, sizeof(err)), true);
	expect_bool("no-login refusal is sanitized", err[0] != 0 && strstr(err, "stub-token") == NULL, true);
}

// The decode the GUI runs on a fetched file: a real cover decodes through
// cover_load_image_file (the exact loader slot 108 and the full-screen
// viewer use, with its real transient encoded allocation), and undecodable
// bytes arrive as a fetch success that refuses to decode -- which §9.2 maps
// to the text-only detail with the "Cover unavailable" note. The leak gate:
// forty fetch → decode → release cycles must leave the heap empty (ASan's
// leak check) and the cache bounded.
static void cover_decode_tests(void) {
	char err[256] = "", path[512] = "";
	abs_auth_snapshot_t snap;
	cover_credentials(&snap, NULL);
	mkdir_s(work);
	abs_cover_set_root(work);

	if (!cover_jpeg_len) decode_base64(COVER_JPEG_B64, cover_jpeg, sizeof(cover_jpeg), &cover_jpeg_len);
	expect_int("embedded test jpeg decodes to bytes", (long)cover_jpeg_len, 899);

	// A real JPEG cover: fetched, then decoded into the §9.5 contain shape.
	stream_program_raw((const char *)cover_jpeg, cover_jpeg_len);
	expect_true("jpeg cover fetched", abs_cover_fetch(&snap, "it-jpg", NULL, path, sizeof(path), err, sizeof(err)));
	cover_image_t image;
	expect_true("the decoded cover the detail shows", cover_load_image_file(path, 90, 128, COVER_FIT_CONTAIN, &image));
	expect_bool("decoded cover carries pixels", image.pixels != NULL, true);
	expect_bool("decoded cover fits the box", image.dsc.header.w <= 90 && image.dsc.header.h <= 128, true);
	cover_free(&image);

	// Undecodable bytes: the cache stores what the server sent (opaque),
	// and the decoder refuses -- the screen's text-only fallback contract.
	stream_program_raw("\x00\x11\x22\x33not an image at all\x00\x00", 22);
	expect_bool("garbage cover bytes still cached (server is authoritative)",
				abs_cover_fetch(&snap, "it-garbage", NULL, path, sizeof(path), err, sizeof(err)), true);
	cover_image_t junk;
	expect_bool("garbage bytes never decode", !cover_load_image_file(path, 90, 128, COVER_FIT_CONTAIN, &junk), true);
	cover_free(&junk);

	// Repeated detail/fullscreen entry-exit: fetch → decode → release, forty
	// times. The cache stays at its §9.3 ceilings and nothing leaks.
	stream_program_raw((const char *)cover_jpeg, cover_jpeg_len);
	for (int c = 0; c < 40; c++) {
		char item[32];
		snprintf(item, sizeof(item), "it-mm%d", c);
		char cycle_path[512] = "";
		expect_true("cycle fetch", abs_cover_fetch(&snap, item, NULL, cycle_path, sizeof(cycle_path), err, sizeof(err)));
		cover_image_t cycle;
		if (cover_load_image_file(cycle_path, 90, 128, COVER_FIT_CONTAIN, &cycle)) {
			cover_free(&cycle);
		}
	}
	char dir[700];
	snprintf(dir, sizeof(dir), "%s/.local/abs-cover-cache", work);
	int files = dir_file_count(dir);
	expect_bool("cycles leave the cache bounded", files >= 1 && files <= ABS_COVER_FILES_MAX, true);
	printf("cover-test: cache settled at %d of %d files after the leak cycles\n", files,
		   ABS_COVER_FILES_MAX);
}

static void two_servers_tests(void) {
	char folder[1024], manifest[1200], audio[1300];
	snprintf(folder, sizeof(folder), "%s/sd/Audiobooks/Author/Book", work);
	mkdir_s(folder);
	snprintf(manifest, sizeof(manifest), "%s/.audiobookshelf", folder);
	snprintf(audio, sizeof(audio), "%s/Part One.mp3", folder);
	FILE *f = fopen(audio, "wb");
	assert(f);
	fputs("x", f);
	fclose(f);
	write_file(manifest, "version=1\nitem_id=same-id\nserver=http://abs-a.example\nduration=120\n"
						 "track=1\t0\t120\tPart One.mp3\n");

	// Two profiles on one store: the same §9.3 manifest examined through
	// both browsing identities, exactly the "same ID on two servers" ask.
	char id_a[ABS_PROFILE_ID_MAX], id_b[ABS_PROFILE_ID_MAX];
	assert(abs_profiles_add("Server A", "http://abs-a.example", "ta", id_a, sizeof(id_a)));
	assert(abs_profiles_add("Server B", "http://abs-b.example", "tb", id_b, sizeof(id_b)));

	assert(abs_profiles_select(id_a));
	expect_true("same server + same id: local Play applies",
				audiobookshelf_local_linked(audio, "same-id"));
	expect_true("own server: not owned elsewhere", !audiobookshelf_local_owned_elsewhere(audio));
	assert(abs_profiles_select(id_b));
	expect_true("§9.2: same item id on another server never links", !audiobookshelf_local_linked(audio, "same-id"));
	expect_true("the copy is owned elsewhere: download only, never re-link",
				audiobookshelf_local_owned_elsewhere(audio));
	assert(abs_profiles_select(id_a));
}

int main(int argc, char **argv) {
	if (argc != 2) {
		fprintf(stderr, "usage: %s <workdir>\n", argv[0]);
		return 2;
	}
	work = argv[1];
	configure_profile(STUB_SERVER, "stub-token");

	description_helper_tests();
	request_and_field_tests();
	description_tests();
	chapter_tests();
	long_string_tests();
	failure_tests();
	detail_overflow_tests();
	ownership_tests();
	decode_base64(COVER_JPEG_B64, cover_jpeg, sizeof(cover_jpeg), &cover_jpeg_len);
	cover_basic_tests();
	cover_cap_tests();
	cover_cancel_tests();
	cover_lru_tests();
	cover_lifecycle_tests();
	cover_decode_tests();
	two_servers_tests();

	printf("audiobookshelf-detail-test: offline parse/helper/cover matrix passed\n");
	return 0;
}

#else // ABS_DETAIL_TEST_LIVE

// ---------------------------------------------------------------------------
// Live: the REAL expanded detail bytes of every checked-in fixture alias.
// ---------------------------------------------------------------------------

static void live_configure(void) {
	const char *server = getenv("SONIX_AUDIOBOOKSHELF_SERVER");
	const char *token = getenv("SONIX_AUDIOBOOKSHELF_TOKEN");
	assert(server && server[0] && token && token[0]);
	configure_profile(server, token);
}

static void live_detail_expectations(const char *id, int chapters_want, bool overflow_want,
									 const char *narrator_want, const char *series_want,
									 const char *sequence_want, const char *description_want) {
	audiobookshelf_detail_t *d = audiobookshelf_detail(id);
	assert(d);
	char what[96];
	snprintf(what, sizeof(what), "%s chapters", id);
	expect_int(what, d->chapter_count, chapters_want);
	snprintf(what, sizeof(what), "%s overflow", id);
	expect_bool(what, d->chapters_overflow, overflow_want);
	snprintf(what, sizeof(what), "%s narrator", id);
	expect_str(what, d->narrator, narrator_want);
	snprintf(what, sizeof(what), "%s series", id);
	expect_str(what, d->item.series, series_want);
	snprintf(what, sizeof(what), "%s sequence", id);
	expect_str(what, d->series_part, sequence_want);
	snprintf(what, sizeof(what), "%s description", id);
	expect_str(what, d->description, description_want);
	if (chapters_want > 1) {
		// Sorted for display by (start,id), whatever order the fixture sent.
		snprintf(what, sizeof(what), "%s sorted first", id);
		expect_bool(what, d->chapters[0].start == 0.0, true);
		if (chapters_want >= 400) {
			// The §9.1 stamps the fixture generator writes: 300 s slices.
			expect_bool("fixture last chapter start", d->chapters[chapters_want - 1].start == 119700.0, true);
		}
	}
	audiobookshelf_detail_free(d);
}

// Wave 2, live: the cover endpoint against the provenance mock. The fetch
// runs through the real HTTP stack (Bearer required by the mock), lands in
// the real §9.3 cache layout, carries the fixture's exact bytes, and the
// second fetch is a cache hit that asks the server for nothing. The mock's
// request log assertions (the .sh) pin the §9.1 endpoint on the wire.
static void live_cover_tests(void) {
	const char *fixture = getenv("SONIX_DETAIL_FIXTURE_COVER");
	assert(fixture && fixture[0]);
	long long fixture_size = file_size(fixture);
	assert(fixture_size > 0);
	char fixture_bytes[2048];
	{
		FILE *f = fopen(fixture, "rb");
		assert(f);
		size_t got = fread(fixture_bytes, 1, sizeof(fixture_bytes), f);
		fclose(f);
		assert((long long)got == fixture_size);
	}

	abs_cover_set_root(work);
	abs_auth_snapshot_t snap;
	assert(audiobookshelf_browse_credentials(&snap));
	assert(snap.server[0] && snap.token[0]);

	char err[256] = "", path[512] = "";
	expect_true("live cover fetched", abs_cover_fetch(&snap, "bk-full", NULL, path, sizeof(path), err, sizeof(err)));
	expect_bool("cover error empty on success", err[0] == '\0', true);
	expect_bool("cover lands in the §9.3 cache layout", strstr(path, "/.local/abs-cover-cache/") != NULL, true);
	expect_bool("cover names carry no credential", strstr(path, "Bearer") == NULL && strstr(path, snap.token) == NULL, true);
	expect_int("cover bytes are the fixture's", (long)file_size(path), (long)fixture_size);
	expect_true("cover bytes round-trip", file_bytes_equal(path, fixture_bytes, (size_t)fixture_size));

	char hit[512] = "";
	expect_true("live cover cache hit", abs_cover_fetch(&snap, "bk-full", NULL, hit, sizeof(hit), err, sizeof(err)));
	expect_str("cache hit returns the same file", hit, path);

	// Every alias must serve its own book: a different item id maps to a
	// different cache name (fetch it, keep both files).
	char second[512] = "";
	expect_true("second item's cover fetched",
				abs_cover_fetch(&snap, "bk-minimal", NULL, second, sizeof(second), err, sizeof(err)));
	expect_bool("per-item cache names", strcmp(second, path) != 0, true);
}

int main(int argc, char **argv) {
	if (argc != 2) {
		fprintf(stderr, "usage: %s <workdir> (against the fixture mock)\n", argv[0]);
		return 2;
	}
	work = argv[1];
	live_configure();

	// §9.1 request shape reaches the mock as-is; the fixture bytes below are
	// the verified v2.3x wire shapes (the modern `id/start/end` names are
	// pinned offline above).
	live_detail_expectations("bk-full", 10, false, "Fixture Narrator",
							 "Fixture Detail Series", "1",
							 "A deterministically generated book with every optional "
							 "detail field populated, used by detail-page suites.");
	live_detail_expectations("bk-minimal", 0, false, "", "", "", "");
	live_detail_expectations("bk-chapters-0", 0, false, "", "", "", "");
	live_detail_expectations("bk-chapters-1", 1, false, "", "", "", "");
	live_detail_expectations("bk-chapters-400", 400, false, "", "", "", "");
	live_detail_expectations("bk-chapters-401", 400, true, "", "", "", "");
	live_detail_expectations("mock-book", 0, false, "", "Fixture Series", "2",
							 "A downloaded fixture used to prove resume import.");

	live_cover_tests();

	printf("audiobookshelf-detail-test: live fixture detail matrix passed\n");
	return 0;
}

#endif // ABS_DETAIL_TEST_LIVE
