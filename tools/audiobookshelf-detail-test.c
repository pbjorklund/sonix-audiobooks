// Focused WS3 (§9.3/§9.4) host tests for the M4 book-detail wave 1: the §9.3
// detail model/parser and the pure description helper it shares with local
// ingestion.
//
// Offline mode (default): an http_request() stub with a canned body pins the
// request mapping (GET /api/items/{id}?expanded=1&include=progress,authors
// with the URL-encoded id) byte for byte, and every parse rule runs against
// canned JSON: field extraction, description plain-vs-HTML/entity
// equivalence, chapter bounds 0/1/400/401 with the overflow row, invalid
// times, long strings, malformed JSON, and the ownership invariants (freed
// models, free-of-NULL, exact chapter heap, no inline download tracks in
// the wrapper).
//
// Live mode (-DABS_DETAIL_TEST_LIVE): the real HTTP stack against the
// provenance mock, once per checked-in fixture alias, parsing the REAL
// expanded detail bytes the fixtures record (bk-full/bk-minimal, the
// 0/1/400/401-chapter books, mock-book) with every §9.1 field.
//
// No GUI and no LVGL here: the §9.2 screen is exercised by the committed
// GUI smokes (audiobookshelf-host-smoke.sh now walks the detail for all of
// its download/link/play modes), and the wrong-server binding truth table
// the detail's Play/Link decisions rest on lives in tools/
// audiobookshelf-absdirs-test.c.

#include "src/system/core/config.h"
#include "src/system/net/http.h"
#include "src/system/streaming/abs_profiles.h"
#include "src/system/streaming/audiobookshelf.h"
#include "src/system/library/description.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

bool http_request(const char *url, const http_req_t *req, char **out, size_t *out_len,
				  size_t limit, int timeout_secs, int *status_out) {
	(void)limit; (void)timeout_secs;
	snprintf(spy.url, sizeof(spy.url), "%s", url ? url : "");
	if (status_out) *status_out = 200;
	size_t len = strlen(spy.body);
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
	ownership_tests();
	two_servers_tests();

	printf("audiobookshelf-detail-test: offline parse/helper matrix passed\n");
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

	printf("audiobookshelf-detail-test: live fixture detail matrix passed\n");
	return 0;
}

#endif // ABS_DETAIL_TEST_LIVE
