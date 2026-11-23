// Focused §10.3 tests for the M2 WS4 query work (Bob spec §10.1).
//
// Offline mode (default): the network surface is a stubbed http_request() that
// captures the URL, so the EXACT server query mapping is pinned byte for byte
// -- the five sort fields, the desc 0|1 literal, zero-based pages, limit=40,
// minified=0, collapseseries=0, and every group filter built as standard
// padded Base64 that then survives URL encoding ("+", "/" and "=" of the
// alphabet arrive as %2B %2F %3D; this is emphatically NOT Base64URL).
// Response, truncation and ownership behavior run against canned JSON here.
//
// Live mode (-DABS_BROWSE_TEST_LIVE): the real HTTP machinery talks to the
// running fixture mock (tools/audiobookshelf_mock.py), once per checked-in
// fixture alias, exercising pagination/total, all four group kinds through
// filterdata, server-side ordering, and the search contract: request limit
// 101, retain 100, with the honest has_more probe on the "overflow" corpus.
//
// No GUI code and no LVGL here. Credentials flow through the profile store,
// so audiobookshelf_init() and its worker thread stay unreferenced (linker
// pruning keeps this harness light).

#include "src/system/core/config.h"
#include "src/system/net/http.h"
#include "src/system/streaming/abs_profiles.h"
#include "src/system/streaming/audiobookshelf.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *work;          // argv[1]
#define STUB_SERVER "http://stub.invalid"

// Largest caller array any test needs: the full search window.
static audiobookshelf_item_t results[ABS_SEARCH_REQUEST_LIMIT];
static char huge_value[ABS_NAME_MAX + 16];

#ifndef ABS_BROWSE_TEST_LIVE

// ---------------------------------------------------------------------------
// Offline network stub: capture the URL, answer with the queued body.
// ---------------------------------------------------------------------------

static struct {
	char url[1920];
	char body[32768];
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
	// before any URL leaves; the stub pins the Bearer reaching this point so a
	// future credential-path change is caught with the query tests.
	assert(req && req->extra_headers &&
		   strncmp(req->extra_headers, "Authorization: Bearer ", 21) == 0);
	return true;
}

const char *http_last_error(void) { return ""; }
long long http_last_server_time_ms(void) { return 0; }

static const char *captured_path(void) {
	size_t skip = strlen(STUB_SERVER);
	const char *path = spy.url;
	if (strncmp(path, STUB_SERVER, skip) != 0) return "(no request was captured)";
	return path + (skip < strlen(path) ? skip : 0);
}

// Canned items envelope: ids pg-<first>..pg-<first+count-1>, titles Book %03d.
static void spy_items(int total, int first, int count) {
	char *p = spy.body, *end = spy.body + sizeof(spy.body);
	p += snprintf(p, (size_t)(end - p), "{\"total\":%d,\"results\":[", total);
	for (int i = 0; i < count; i++) {
		char id[32], title[ABS_NAME_MAX], author[ABS_NAME_MAX];
		snprintf(id, sizeof(id), "pg-%03d", first + i);
		snprintf(title, sizeof(title), "Book %03d", first + i);
		snprintf(author, sizeof(author), "Author %03d", (first + i) % 7);
		p += snprintf(p, (size_t)(end - p),
					  "%s{\"id\":\"%s\",\"media\":{\"coverPath\":\"/covers/%s-cover\","
					  "\"metadata\":{\"title\":\"%s\",\"authorName\":\"%s\"},"
					  "\"duration\":%d.5,\"numTracks\":3}}",
					  i ? "," : "", id, id, title, author, 100 + first + i);
	}
	snprintf(p, (size_t)(end - p), "]}");
}

// Canned search response: book[].libraryItem, ids sc-<first>..sc-<last>.
static void spy_search(int first, int count) {
	char *p = spy.body, *end = spy.body + sizeof(spy.body);
	p += snprintf(p, (size_t)(end - p), "{\"book\":[");
	for (int i = 0; i < count; i++) {
		char id[32], title[ABS_NAME_MAX];
		snprintf(id, sizeof(id), "sc-%03d", first + i);
		snprintf(title, sizeof(title), "Search %03d", first + i);
		p += snprintf(p, (size_t)(end - p),
					  "%s{\"libraryItem\":{\"id\":\"%s\",\"media\":{\"metadata\":"
					  "{\"title\":\"%s\",\"authorName\":\"Search Author\"},\"duration\":60.0}}}",
					  i ? "," : "", id, title);
	}
	snprintf(p, (size_t)(end - p), "]}");
}

// Canned filterdata body, verbatim.
#define SPY_FILTERDATA(json) snprintf(spy.body, sizeof(spy.body), "%s", json)

#endif // !ABS_BROWSE_TEST_LIVE

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

// One profile store in the work directory; the snapshot is the only credential
// path the browse APIs use (§11.3), so the legacy globals stay untouched here.
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

static void query_init_tests(void) {
	expect_str("sort title", abs_browse_sort_field(ABS_SORT_TITLE), "media.metadata.title");
	expect_str("sort author", abs_browse_sort_field(ABS_SORT_AUTHOR), "media.metadata.authorNameLF");
	expect_str("sort added", abs_browse_sort_field(ABS_SORT_ADDED_AT), "addedAt");
	expect_str("sort duration", abs_browse_sort_field(ABS_SORT_DURATION), "media.duration");
	expect_str("sort series position", abs_browse_sort_field(ABS_SORT_SERIES_POSITION), "sequence");
	expect_bool("sort field refuses out-of-range",
				abs_browse_sort_field((abs_browse_sort_t)99) == NULL, true);

	for (int s = ABS_SORT_TITLE; s <= ABS_SORT_SERIES_POSITION; s++) {
		expect_bool("default desc (§10.1)", abs_browse_query_default_desc((abs_browse_sort_t)s),
					(abs_browse_sort_t)s == ABS_SORT_ADDED_AT);
	}

	abs_browse_query_t q;
	assert(abs_browse_query_init(&q, "lib-1", ABS_SORT_TITLE, ABS_FILTER_NONE, NULL, 3, false));
	expect_int("page kept", q.page, 3);
	// §10.1 pages are zero-based; a negative page is a caller slip, clamped.
	assert(abs_browse_query_init(&q, "lib-1", ABS_SORT_TITLE, ABS_FILTER_NONE, NULL, -5, false));
	expect_int("negative page clamps to zero", q.page, 0);
	assert(abs_browse_query_init(&q, "lib-1", ABS_SORT_TITLE, ABS_FILTER_AUTHOR, "author-1", 0, true));
	expect_str("filter value stored", q.filter_value, "author-1");

	// Series position is the series-filter default only (§10.1).
	assert(!abs_browse_query_init(&q, "lib-1", ABS_SORT_SERIES_POSITION, ABS_FILTER_NONE, NULL, 0, false));
	assert(!abs_browse_query_init(&q, "lib-1", ABS_SORT_SERIES_POSITION, ABS_FILTER_AUTHOR, "a", 0, false));
	assert(!abs_browse_query_init(&q, "lib-1", ABS_SORT_SERIES_POSITION, ABS_FILTER_TAG, "t", 0, false));
	assert(abs_browse_query_init(&q, "lib-1", ABS_SORT_SERIES_POSITION, ABS_FILTER_SERIES, "series-1", 0, false));

	assert(!abs_browse_query_init(&q, NULL, ABS_SORT_TITLE, ABS_FILTER_NONE, NULL, 0, false));
	assert(!abs_browse_query_init(&q, "", ABS_SORT_TITLE, ABS_FILTER_NONE, NULL, 0, false));
	assert(!abs_browse_query_init(&q, "lib-1", (abs_browse_sort_t)77, ABS_FILTER_NONE, NULL, 0, false));
	assert(!abs_browse_query_init(&q, "lib-1", ABS_SORT_TITLE, (abs_browse_filter_t)66, "x", 0, false));
	assert(!abs_browse_query_init(&q, "lib-1", ABS_SORT_TITLE, ABS_FILTER_AUTHOR, "", 0, false));
	// The bounded struct refuses an over-length value instead of truncating.
	memset(huge_value, 'g', sizeof(huge_value) - 1);
	huge_value[sizeof(huge_value) - 1] = '\0';
	assert(!abs_browse_query_init(&q, "lib-1", ABS_SORT_TITLE, ABS_FILTER_GENRE, huge_value, 0, false));
	// Same for an over-length library id.
	char big_lib[ABS_ID_MAX + 4];
	memset(big_lib, 'L', sizeof(big_lib) - 1);
	big_lib[sizeof(big_lib) - 1] = '\0';
	assert(!abs_browse_query_init(&q, big_lib, ABS_SORT_TITLE, ABS_FILTER_NONE, NULL, 0, false));
}

static void build_filter_tests(void) {
	char out[1100];
	// Standard padded Base64, then URL-encoded (§10.1). The GUI's M1 chain
	// (audiobookshelf_group_filter + filtered_items) produces the same bytes
	// for author/series; genres/tags are URL-encoded here once, correctly.
	assert(abs_browse_query_build_filter(ABS_FILTER_AUTHOR, "author-1", out, sizeof(out)));
	expect_str("author filter", out, "authors.YXV0aG9yLTE%3D");
	assert(abs_browse_query_build_filter(ABS_FILTER_SERIES, "series-fa-1", out, sizeof(out)));
	expect_str("series filter", out, "series.c2VyaWVzLWZhLTE%3D");
	// §10.1: Base64 is NOT Base64URL. "+" and "/" belong to the standard
	// alphabet and must survive URL encoding, "=" likewise.
	assert(abs_browse_query_build_filter(ABS_FILTER_GENRE, "Rock/Metal+", out, sizeof(out)));
	expect_str("genre keeps standard Base64", out, "genres.Um9jay9NZXRhbCs%3D");
	assert(abs_browse_query_build_filter(ABS_FILTER_TAG, "Fixture Tag Alpha", out, sizeof(out)));
	expect_str("tag filter", out, "tags.Rml4dHVyZSBUYWcgQWxwaGE%3D");

	assert(!abs_browse_query_build_filter(ABS_FILTER_NONE, "x", out, sizeof(out)));
	assert(!abs_browse_query_build_filter(ABS_FILTER_AUTHOR, "", out, sizeof(out)));
	assert(!abs_browse_query_build_filter(ABS_FILTER_AUTHOR, NULL, out, sizeof(out)));
	assert(!abs_browse_query_build_filter(ABS_FILTER_AUTHOR, "a", out, 4)); // too small
	assert(!abs_browse_query_build_filter(ABS_FILTER_GENRE, huge_value, out, sizeof(out)));
	assert(!abs_browse_query_build_filter((abs_browse_filter_t)42, "x", out, sizeof(out)));
}

#ifndef ABS_BROWSE_TEST_LIVE

static void browse_construction_tests(void) {
	abs_browse_query_t q;
	char expected[1600];
	int total = 0;

	// Plain title list, zero-based page 2: the §10.1 template, empty filter.
	assert(abs_browse_query_init(&q, "lib-1", ABS_SORT_TITLE, ABS_FILTER_NONE, NULL, 2, false));
	spy_items(58, 81, 3);
	expect_int("title count", audiobookshelf_browse(&q, results, ABS_BROWSE_PAGE_LIMIT, &total), 3);
	expect_int("title total honored", total, 58);
	expect_str("title query", captured_path(),
			   "/api/libraries/lib-1/items?limit=40&page=2&sort=media.metadata.title"
			   "&desc=0&minified=0&collapseseries=0&filter=");
	expect_str("parsed title", results[0].title, "Book 081");
	expect_str("parsed author", results[2].author, "Author 006");
	// The cover route is generated from the item id, as in M1.
	expect_str("parsed cover route", results[0].cover, "/api/items/pg-081/cover");

	// Author sort, newest direction toggle, padded Base64 author filter: the
	// last-name-first field, desc=1 literal, and %3D padding survival.
	assert(abs_browse_query_init(&q, "lib-1", ABS_SORT_AUTHOR, ABS_FILTER_AUTHOR, "author-1", 0, true));
	spy_items(7, 1, 1);
	expect_int("author count", audiobookshelf_browse(&q, results, ABS_BROWSE_PAGE_LIMIT, &total), 1);
	expect_int("author total", total, 7);
	snprintf(expected, sizeof(expected),
			 "/api/libraries/lib-1/items?limit=40&page=0&sort=media.metadata.authorNameLF"
			 "&desc=1&minified=0&collapseseries=0&filter=authors.YXV0aG9yLTE%%3D");
	expect_str("author query", captured_path(), expected);

	// Recently added with the §10.1 default direction (desc=1) from the helper.
	bool desc = abs_browse_query_default_desc(ABS_SORT_ADDED_AT);
	assert(abs_browse_query_init(&q, "lib-1", ABS_SORT_ADDED_AT, ABS_FILTER_NONE, NULL, 0, desc));
	spy_items(2, 4, 2);
	expect_int("added count", audiobookshelf_browse(&q, results, ABS_BROWSE_PAGE_LIMIT, &total), 2);
	snprintf(expected, sizeof(expected),
			 "/api/libraries/lib-1/items?limit=40&page=0&sort=addedAt&desc=1"
			 "&minified=0&collapseseries=0&filter=");
	expect_str("added query", captured_path(), expected);

	// Duration, ascending (toggle permits longest first elsewhere).
	assert(abs_browse_query_init(&q, "lib-1", ABS_SORT_DURATION, ABS_FILTER_NONE, NULL, 0, false));
	spy_items(2, 4, 2);
	expect_int("duration count", audiobookshelf_browse(&q, results, ABS_BROWSE_PAGE_LIMIT, &total), 2);
	snprintf(expected, sizeof(expected),
			 "/api/libraries/lib-1/items?limit=40&page=0&sort=media.duration&desc=0"
			 "&minified=0&collapseseries=0&filter=");
	expect_str("duration query", captured_path(), expected);

	// Series position under a series filter: sequence + padded id.
	assert(abs_browse_query_init(&q, "lib-1", ABS_SORT_SERIES_POSITION,
								 ABS_FILTER_SERIES, "series-fa-1", 0, false));
	spy_items(38, 1, 38);
	expect_int("series count", audiobookshelf_browse(&q, results, ABS_BROWSE_PAGE_LIMIT, &total), 38);
	expect_int("series total", total, 38);
	snprintf(expected, sizeof(expected),
			 "/api/libraries/lib-1/items?limit=40&page=0&sort=sequence&desc=0"
			 "&minified=0&collapseseries=0&filter=series.c2VyaWVzLWZhLTE%%3D");
	expect_str("series query", captured_path(), expected);

	// Genre/tag construction through the request, with the reserved set intact.
	assert(abs_browse_query_init(&q, "lib-1", ABS_SORT_TITLE, ABS_FILTER_GENRE, "Rock/Metal+", 5, false));
	spy_items(1, 1, 1);
	audiobookshelf_browse(&q, results, ABS_BROWSE_PAGE_LIMIT, &total);
	snprintf(expected, sizeof(expected),
			 "/api/libraries/lib-1/items?limit=40&page=5&sort=media.metadata.title&desc=0"
			 "&minified=0&collapseseries=0&filter=genres.Um9jay9NZXRhbCs%%3D");
	expect_str("genre query", captured_path(), expected);

	// The honest bound: a struct-legal tag value (255 bytes) whose padded
	// Base64 is dense in "+"/"/" characters ("///9" groups, three unsafe
	// characters per four bytes of output -- deterministic in the encoder),
	// paired with the struct-legal ceiling of a library id. The request path
	// itself still fits its own buffer; the joined server+path URL exceeds
	// ABS_URL_MAX and api_request refuses it with a message, never a cut.
	char fat_lib[96];
	memset(fat_lib, 'L', 95);
	fat_lib[95] = '\0';
	char fat_tag[256];
	for (int i = 0; i < 255; i += 3) memcpy(fat_tag + i, "\xff\xff\xfd", 3);
	fat_tag[255] = '\0';
	assert(abs_browse_query_init(&q, fat_lib, ABS_SORT_TITLE, ABS_FILTER_TAG, fat_tag, 0, false));
	expect_int("joined-URL bound is the honest refusal",
			   audiobookshelf_browse(&q, results, ABS_BROWSE_PAGE_LIMIT, &total), -1);
	expect_str("joined-URL bound error", audiobookshelf_last_error(),
			   "The request is too long for this server address.");

	// An out-of-range enum cast in a hand-filled struct is refused at the
	// builder too; and a library id with URL-unsafe characters must reach the
	// server encoded.
	abs_browse_query_t raw_q;
	memset(&raw_q, 0, sizeof(raw_q));
	snprintf(raw_q.library_id, sizeof(raw_q.library_id), "a b/c");
	raw_q.sort = ABS_SORT_TITLE;
	raw_q.filter = ABS_FILTER_NONE;
	spy_items(1, 1, 1);
	audiobookshelf_browse(&raw_q, results, ABS_BROWSE_PAGE_LIMIT, &total);
	snprintf(expected, sizeof(expected),
			 "/api/libraries/a%%20b%%2Fc/items?limit=40&page=0&sort=media.metadata.title"
			 "&desc=0&minified=0&collapseseries=0&filter=");
	expect_str("library id encoded", captured_path(), expected);

	memset(&raw_q, 0, sizeof(raw_q));
	snprintf(raw_q.library_id, sizeof(raw_q.library_id), "lib-1");
	raw_q.sort = (abs_browse_sort_t)31;
	raw_q.filter = ABS_FILTER_TAG;
	expect_int("builder refuses a corrupt query",
			   audiobookshelf_browse(&raw_q, results, ABS_BROWSE_PAGE_LIMIT, &total), 0);
	expect_str("builder bound error", audiobookshelf_last_error(),
			   "The browse query is too long for this server address.");

	// build_filter must refuse a too-small output buffer: the output is
	// empty, never a partially encoded value that could look valid.
	char small_out[8] = "keep";
	expect_bool("build_filter refuses small buffer",
				abs_browse_query_build_filter(ABS_FILTER_TAG, "any value", small_out, sizeof(small_out)), false);
	expect_bool("refusal leaves no partial value", small_out[0] == '\0', true);

	// An orphan series-position query (§10.1) is refused at the builder too
	// ("series-filter default only").
	raw_q = (abs_browse_query_t){0};
	snprintf(raw_q.library_id, sizeof(raw_q.library_id), "lib-1");
	raw_q.sort = ABS_SORT_SERIES_POSITION;
	raw_q.filter = ABS_FILTER_AUTHOR;
	snprintf(raw_q.filter_value, sizeof(raw_q.filter_value), "author-1");
	expect_int("orphan series position refused",
			   audiobookshelf_browse(&raw_q, results, ABS_BROWSE_PAGE_LIMIT, &total), 0);
}

static void browse_response_tests(void) {
	abs_browse_query_t q;
	assert(abs_browse_query_init(&q, "lib-1", ABS_SORT_TITLE, ABS_FILTER_NONE, NULL, 0, false));

	// Bounded slice: the server offers more results than the caller's array.
	spy_items(58, 41, 2);
	int total = 0;
	expect_int("sliced count", audiobookshelf_browse(&q, results, 1, &total), 1);
	expect_int("total survives slicing", total, 58);
	// total_out is optional.
	expect_int("no-total count", audiobookshelf_browse(&q, results, 1, NULL), 1);
	// Field extraction.
	spy_items(3, 8, 2);
	expect_int("two rows", audiobookshelf_browse(&q, results, ABS_BROWSE_PAGE_LIMIT, &total), 2);
	expect_str("first id", results[0].id, "pg-008");
	expect_str("second author", results[1].author, "Author 002");
	expect_str("second cover route", results[1].cover, "/api/items/pg-009/cover");

	// Malformed JSON is an honest failure, with the M1 error text preserved.
	snprintf(spy.body, sizeof(spy.body), "{\"results\":");
	spy.url[0] = '\0';
	int c = audiobookshelf_browse(&q, results, ABS_BROWSE_PAGE_LIMIT, &total);
	expect_int("invalid body count", c, -1);
	expect_str("invalid body error", audiobookshelf_last_error(),
			   "Audiobookshelf returned invalid book data.");

	// Empty result set for a filter the server rejects (fail closed).
	snprintf(spy.body, sizeof(spy.body), "{\"total\":0,\"results\":[]}");
	expect_int("empty results", audiobookshelf_browse(&q, results, ABS_BROWSE_PAGE_LIMIT, &total), 0);

	// Bad arguments short-circuit without touching the network.
	spy.url[0] = '\0';
	audiobookshelf_browse(&q, results, 0, &total);
	audiobookshelf_browse(NULL, results, ABS_BROWSE_PAGE_LIMIT, &total);
	expect_str("no request for bad args", captured_path(), "(no request was captured)");
}

static void search_tests(void) {
	// §10.1: the request line carries limit=101, whatever the caller keeps.
	spy_search(1, 101);
	bool has_more = false;
	int c = audiobookshelf_search_ex("lib-1", "war peace", results, ABS_SEARCH_DISPLAY_LIMIT, &has_more);
	expect_int("101 -> 100 retained", c, 100);
	expect_bool("101st is has_more evidence", has_more, true);
	expect_str("search query", captured_path(),
			   "/api/libraries/lib-1/search?q=war%20peace&limit=101");
	expect_str("first retained id", results[0].id, "sc-001");
	expect_str("last retained id", results[99].id, "sc-100");

	// Exactly 100 retained and the probe absent: an honest "no more".
	spy_search(1, 100);
	has_more = true;
	c = audiobookshelf_search_ex("lib-1", "hundred", results, ABS_SEARCH_DISPLAY_LIMIT, &has_more);
	expect_int("100 retained", c, 100);
	expect_bool("no honest overflow", has_more, false);

	// Fewer than 100, and zero, are plain.
	spy_search(1, 40);
	c = audiobookshelf_search_ex("lib-1", "forty", results, ABS_SEARCH_DISPLAY_LIMIT, &has_more);
	expect_int("40 retained", c, 40);
	expect_bool("40 no overflow", has_more, false);
	spy_search(1, 0);
	expect_int("zero retained", audiobookshelf_search_ex("lib-1", "zero", results,
														 ABS_SEARCH_DISPLAY_LIMIT, &has_more), 0);

	// The display ceiling: a caller passing more than 100 still keeps 100,
	// and the probe is independent of how few rows the caller had room for.
	spy_search(1, 101);
	has_more = false;
	c = audiobookshelf_search_ex("lib-1", "many", results, ABS_SEARCH_DISPLAY_LIMIT + 40, &has_more);
	expect_int("ceiling holds at 100", c, 100);
	expect_bool("ceiling retains probe", has_more, true);
	spy_search(1, 101);
	c = audiobookshelf_search_ex("lib-1", "many", results, 40, &has_more);
	expect_int("40-room keeps 40", c, 40);
	expect_bool("probe is server-side evidence", has_more, true);

	// Over-long query is refused rather than silently shortened.
	memset(huge_value, 'q', ABS_NAME_MAX + 8);
	huge_value[ABS_NAME_MAX + 8] = '\0';
	expect_int("long query refused",
			   audiobookshelf_search_ex("lib-1", huge_value, results, 4, &has_more), 0);
	expect_str("long query error", audiobookshelf_last_error(), "The search text is too long.");

	// Malformed body and bad arguments.
	snprintf(spy.body, sizeof(spy.body), "{\"book\":");
	expect_int("invalid search body",
			   audiobookshelf_search_ex("lib-1", "x", results, 4, &has_more), -1);
	expect_str("invalid search error", audiobookshelf_last_error(),
			   "Audiobookshelf returned invalid search data.");
	// Degenerate arguments never claim more matches from a network failure.
	expect_int("no query",
			   audiobookshelf_search_ex("lib-1", "", results, 4, &has_more), 0);
	expect_bool("failed search is not " "more", has_more, false);
}

static void groups_tests(void) {
	audiobookshelf_library_t *list = NULL;
	int n = 0;
	bool truncated = false;
	int oversized = 0;

	// Authors: {id,name} objects, both required, alphabetical output.
	SPY_FILTERDATA("{\"authors\":[{\"id\":\"b-id\",\"name\":\"B Author\"},"
				   "{\"id\":\"a-id\",\"name\":\"A Author\"},{\"id\":\"empty\",\"name\":\"\"}]}");
	n = audiobookshelf_groups_ex("lib-1", ABS_FILTER_AUTHOR, &list, &truncated, &oversized);
	expect_int("author groups", n, 2);
	expect_bool("small group not truncated", truncated, false);
	expect_int("no oversized values", oversized, 0);
	expect_str("sorted first", list[0].name, "A Author");
	expect_str("id kept distinct from name", list[0].id, "a-id");
	expect_bool("entry not oversized", list[0].oversized, false);
	free(list);
	list = NULL;

	// Genres as bare strings (§10.1 real-server shape): the string is the
	// display and the filter value; no object id exists.
	SPY_FILTERDATA("{\"genres\":[\"Nonfiction\",\"Fiction\"]}");
	n = audiobookshelf_groups_ex("lib-1", ABS_FILTER_GENRE, &list, &truncated, &oversized);
	expect_int("genre groups", n, 2);
	expect_str("genre sorted", list[0].name, "Fiction");
	expect_bool("genre has no object id", list[0].id[0] == '\0', true);
	free(list);
	list = NULL;

	// Genre/tag entries as {id,name} objects (legacy fixture shape): the NAME
	// is the filter value and the object id is not carried (§10.1).
	SPY_FILTERDATA("{\"tags\":[{\"id\":\"ta-9\",\"name\":\"Zed Tag\"},"
				   "{\"id\":\"ta-1\",\"name\":\"Alpha Tag\"}]}");
	n = audiobookshelf_groups_ex("lib-1", ABS_FILTER_TAG, &list, &truncated, &oversized);
	expect_int("tag groups", n, 2);
	expect_str("tag display", list[0].name, "Alpha Tag");
	expect_bool("tag object id not carried", list[0].id[0] == '\0', true);
	free(list);
	list = NULL;

	// An empty group reports zero and keeps the output pointer NULL.
	SPY_FILTERDATA("{\"tags\":[]}");
	n = audiobookshelf_groups_ex("lib-1", ABS_FILTER_TAG, &list, &truncated, &oversized);
	expect_int("empty group", n, 0);
	assert(list == NULL);

	// Malformed body is -1 with the M1 error text.
	SPY_FILTERDATA("{\"authors\":");
	n = audiobookshelf_groups_ex("lib-1", ABS_FILTER_AUTHOR, &list, &truncated, &oversized);
	expect_int("malformed filterdata", n, -1);
	expect_str("filterdata error", audiobookshelf_last_error(), "Invalid library filters.");

	// §10.6 boundary: a 255-byte genre fits the 256-byte query contract and
	// stays usable; one byte more is oversized -- kept visible, flagged, counted.
	char name255[ABS_NAME_MAX], name256[ABS_NAME_MAX + 8];
	memset(name255, 'a', sizeof(name255) - 1); // 255 bytes
	name255[sizeof(name255) - 1] = '\0';
	snprintf(name256, sizeof(name256), "%sb", name255); // 256 bytes
	snprintf(spy.body, sizeof(spy.body), "{\"genres\":[\"%s\",\"%s\"]}", name255, name256);
	n = audiobookshelf_groups_ex("lib-1", ABS_FILTER_GENRE, &list, &truncated, &oversized);
	expect_int("boundary group count", n, 2);
	expect_int("one oversized value", oversized, 1);
	expect_str("255-byte value usable", list[0].name, name255);
	expect_bool("255-byte value not oversized", list[0].oversized, false);
	expect_bool("256-byte value oversized", list[1].oversized, true);
	free(list);
	list = NULL;
	truncated = false; oversized = 0;

	// The same rule protects author/series ids (the encoded filter value):
	// an id longer than the id contract is flagged, never silently truncated.
	char long_id[200];
	memset(long_id, 'i', sizeof(long_id) - 1);
	long_id[sizeof(long_id) - 1] = '\0';
	snprintf(spy.body, sizeof(spy.body), "{\"authors\":[{\"id\":\"%s\",\"name\":\"Long Id\"}]}",
			 long_id);
	n = audiobookshelf_groups_ex("lib-1", ABS_FILTER_AUTHOR, &list, &truncated, &oversized);
	expect_int("long id author kept", n, 1);
	expect_bool("long id flagged oversized", list[0].oversized, true);
	expect_int("long id counted", oversized, 1);
	free(list);
	list = NULL;
	oversized = 0;

	// §10.6: filter groups at 512 and 513 entries. The projection is hard
	// bounded to ABS_FILTER_GROUP_LIMIT entries BEFORE allocation; above that
	// only the bounded first set is retained and TRUNCATED is reported.
	char *p = spy.body;
	char *end = spy.body + sizeof(spy.body);
	p += snprintf(p, (size_t)(end - p), "{\"genres\":[");
	for (int i = 0; i < ABS_FILTER_GROUP_LIMIT; i++)
		p += snprintf(p, (size_t)(end - p), "%s\"Genre %04d\"", i ? "," : "", i);
	snprintf(p, (size_t)(end - p), "]}");
	n = audiobookshelf_groups_ex("lib-1", ABS_FILTER_GENRE, &list, &truncated, &oversized);
	expect_int("exactly 512 entries", n, ABS_FILTER_GROUP_LIMIT);
	expect_bool("exactly 512 not truncated", truncated, false);
	expect_str("sorted first of 512", list[0].name, "Genre 0000");
	free(list);
	list = NULL;
	truncated = false;

	p = spy.body;
	p += snprintf(p, (size_t)(end - p), "{\"genres\":[");
	for (int i = 0; i < ABS_FILTER_GROUP_LIMIT + 1; i++)
		p += snprintf(p, (size_t)(end - p), "%s\"Genre %04d\"", i ? "," : "", i);
	snprintf(p, (size_t)(end - p), "]}");
	n = audiobookshelf_groups_ex("lib-1", ABS_FILTER_GENRE, &list, &truncated, &oversized);
	expect_int("513 entries cap to 512", n, ABS_FILTER_GROUP_LIMIT);
	expect_bool("513 entries truncated", truncated, true);
	expect_str("bounded set sorted first", list[0].name, "Genre 0000");
	expect_str("bounded set sorted last", list[ABS_FILTER_GROUP_LIMIT - 1].name, "Genre 0511");
	free(list);
	list = NULL;

	// The legacy series entry point still resolves through ids.
	SPY_FILTERDATA("{\"series\":[{\"id\":\"s-2\",\"name\":\"S2\"},"
				   "{\"id\":\"s-1\",\"name\":\"S1\"}]}");
	n = audiobookshelf_groups("lib-1", true, &list);
	expect_int("legacy series groups", n, 2);
	expect_str("legacy series id", list[0].id, "s-1");
	free(list);

	// M1 filter value chain: group_filter stays raw; filtered_items encodes.
	char filter[ABS_ID_MAX * 3];
	audiobookshelf_group_filter(false, "author-1", filter, sizeof(filter));
	expect_str("raw author filter", filter, "authors.YXV0aG9yLTE=");
	audiobookshelf_group_filter(true, "series-fa-1", filter, sizeof(filter));
	expect_str("raw series filter", filter, "series.c2VyaWVzLWZhLTE=");
}

static void legacy_compat_tests(void) {
	int total = 0;

	// Byte-for-byte M1 items request through the legacy path, including the
	// URL-encoded '=' of the GUI's padded Base64 (the GUI smoke's grep).
	snprintf(spy.body, sizeof(spy.body),
			 "{\"total\":1,\"results\":[{\"id\":\"x\",\"media\":{\"metadata\":{\"title\":\"t\"}}}]}");
	expect_int("legacy count", audiobookshelf_filtered_items("lib-1", 1, "authors.YXV0aG9yLTE=",
															 results, ABS_PAGE_LIMIT, &total), 1);
	expect_str("legacy request", captured_path(),
			   "/api/libraries/lib-1/items?limit=40&page=1&sort=media.metadata.title"
			   "&minified=0&collapseseries=0&filter=authors.YXV0aG9yLTE%3D");

	// Legacy search pins the M1 request shape (limit = min(max, 40)).
	spy_search(1, 2);
	expect_int("legacy search", audiobookshelf_search("lib-1", "m", results, ABS_PAGE_LIMIT), 2);
	expect_str("legacy search request", captured_path(),
			   "/api/libraries/lib-1/search?q=m&limit=40");
	expect_int("legacy search clamps", audiobookshelf_search("lib-1", "m", results, 500), 2);

	// Degenerate arguments stay inert (no network, no crash).
	spy.url[0] = '\0';
	expect_int("no library id", audiobookshelf_filtered_items("", 0, "", results, 4, &total), 0);
	expect_str("no request on empty library", captured_path(), "(no request was captured)");
}

int main(int argc, char **argv) {
	if (argc != 2) {
		fprintf(stderr, "usage: %s <workdir>\n", argv[0]);
		return 2;
	}
	work = argv[1];

	configure_profile(STUB_SERVER, "browse-test-token");
	query_init_tests();
	build_filter_tests();
	browse_construction_tests();
	browse_response_tests();
	search_tests();
	groups_tests();
	legacy_compat_tests();

	printf("audiobookshelf-browse-test: offline construction/response/ownership tests passed\n");
	return 0;
}

#else // ABS_BROWSE_TEST_LIVE

// The real network stack labels its failures through the language module
// (lang.c pulls in LVGL, which this harness must not). Identity is faithful
// enough here: no test asserts on the transport's translated error strings.
const char *tr(const char *text) { return text; }

// ---------------------------------------------------------------------------
// Live matrix against the checked-in fixture mock, one pass per alias. The
// three aliases ship byte-identical datasets, so every expected value below is
// pinned from tools/fixtures/audiobookshelf/*/scenario/dataset.json (§14.6).
// ---------------------------------------------------------------------------

static const char *list_lib;  // lib-list-1, resolved by name from filterdata
static const char *search_lib; // lib-search-1: the 40/100/101 search corpora

static void live_configure(void) {
	const char *server = getenv("SONIX_AUDIOBOOKSHELF_SERVER");
	const char *token = getenv("SONIX_AUDIOBOOKSHELF_TOKEN");
	assert(server && server[0] && token && token[0]);
	configure_profile(server, token);
}

static void live_resolve_library(void) {
	audiobookshelf_library_t libs[16];
	int n = audiobookshelf_libraries(libs, 16);
	assert(n > 0);
	list_lib = NULL;
	search_lib = NULL;
	for (int i = 0; i < n; i++) {
		if (strcmp(libs[i].name, "Fixture Fiction Shelf") == 0) list_lib = strdup(libs[i].id);
		if (strcmp(libs[i].name, "Fixture Search Range") == 0) search_lib = strdup(libs[i].id);
	}
	assert(list_lib && strcmp(list_lib, "lib-list-1") == 0);
	assert(search_lib && strcmp(search_lib, "lib-search-1") == 0);
}

static bool titles_ascending(const audiobookshelf_item_t *out, int count) {
	for (int i = 1; i < count; i++)
		if (strcmp(out[i - 1].title, out[i].title) >= 0) return false;
	return true;
}

static void live_browse(void) {
	abs_browse_query_t q;
	int total = 0;

	// 75 book rows in two pages of up to 40, ordered by title.
	assert(abs_browse_query_init(&q, list_lib, ABS_SORT_TITLE, ABS_FILTER_NONE, NULL, 0, false));
	expect_int("title page 0 count", audiobookshelf_browse(&q, results, ABS_BROWSE_PAGE_LIMIT, &total), 40);
	expect_int("title total", total, 75);
	expect_str("title page 0 first", results[0].id, "pg-bk-001");
	expect_str("title page 0 last", results[39].id, "pg-bk-040");
	assert(titles_ascending(results, 40));

	// §10.1: returned total drives the previous/next answer, page 1 is final.
	assert(abs_browse_query_init(&q, list_lib, ABS_SORT_TITLE, ABS_FILTER_NONE, NULL, 1, false));
	expect_int("title page 1 count", audiobookshelf_browse(&q, results, ABS_BROWSE_PAGE_LIMIT, &total), 35);
	expect_str("title page 1 first", results[0].id, "pg-bk-041");
	expect_str("title page 1 last", results[34].id, "pg-bk-075");

	// Direction toggle: descending title order.
	assert(abs_browse_query_init(&q, list_lib, ABS_SORT_TITLE, ABS_FILTER_NONE, NULL, 0, true));
	audiobookshelf_browse(&q, results, ABS_BROWSE_PAGE_LIMIT, &total);
	expect_str("title desc first", results[0].id, "pg-bk-075");

	// Recently added: added_at is monotonic in the fixture, asc/desc differ.
	assert(abs_browse_query_init(&q, list_lib, ABS_SORT_ADDED_AT, ABS_FILTER_NONE,
								 NULL, 0, abs_browse_query_default_desc(ABS_SORT_ADDED_AT)));
	audiobookshelf_browse(&q, results, ABS_BROWSE_PAGE_LIMIT, &total);
	expect_str("added desc (default) first", results[0].id, "pg-bk-075");
	assert(abs_browse_query_init(&q, list_lib, ABS_SORT_ADDED_AT, ABS_FILTER_NONE, NULL, 0, false));
	audiobookshelf_browse(&q, results, ABS_BROWSE_PAGE_LIMIT, &total);
	expect_str("added asc first", results[0].id, "pg-bk-001");

	// Duration: the fixture durations are NOT monotonic with item ids, so
	// server-side ordering is observable: asc starts at Book 007, desc at Book 069.
	assert(abs_browse_query_init(&q, list_lib, ABS_SORT_DURATION, ABS_FILTER_NONE, NULL, 0, false));
	audiobookshelf_browse(&q, results, ABS_BROWSE_PAGE_LIMIT, &total);
	expect_str("duration asc first", results[0].id, "pg-bk-007");
	assert(abs_browse_query_init(&q, list_lib, ABS_SORT_DURATION, ABS_FILTER_NONE, NULL, 0, true));
	audiobookshelf_browse(&q, results, ABS_BROWSE_PAGE_LIMIT, &total);
	expect_str("duration desc first", results[0].id, "pg-bk-069");

	// Author last-name-first field: the request mapping is pinned offline and
	// in the mock log; this fixture's ordering helper falls back to title
	// order for the LF key, so only the envelope is checked here (the mock
	// gains that key with the §10.3 fixture wave).
	assert(abs_browse_query_init(&q, list_lib, ABS_SORT_AUTHOR, ABS_FILTER_NONE, NULL, 0, false));
	expect_int("author sort count",
			   audiobookshelf_browse(&q, results, ABS_BROWSE_PAGE_LIMIT, &total), 40);
	expect_int("author sort total", total, 75);

	// Author id filter: 25 rows, title-ordered inside the filter.
	assert(abs_browse_query_init(&q, list_lib, ABS_SORT_TITLE, ABS_FILTER_AUTHOR, "author-fa-1", 0, false));
	audiobookshelf_browse(&q, results, ABS_BROWSE_PAGE_LIMIT, &total);
	expect_int("author filter total", total, 25);
	expect_str("author filter first", results[0].id, "pg-bk-001");
	expect_str("author filter last", results[24].id, "pg-bk-073");

	// Series filter with the series-position default: 38 rows, sequence then
	// title tie-break (fixture: odds at position 1, evens at 2).
	assert(abs_browse_query_init(&q, list_lib, ABS_SORT_SERIES_POSITION,
								 ABS_FILTER_SERIES, "series-fa-1", 0, false));
	audiobookshelf_browse(&q, results, ABS_BROWSE_PAGE_LIMIT, &total);
	expect_int("series filter total", total, 38);
	expect_str("series position first", results[0].id, "pg-bk-001");
	expect_str("series position second", results[1].id, "pg-bk-003");
	expect_str("series position last", results[37].id, "pg-bk-075");

	// §10.1 genre/tag filters encode the NAME, not an id. The fixture serves
	// name-shaped genres/tags (bare strings in filterdata, name strings on
	// the rows), so a name-based filter round-trips to real rows: the same
	// row set the id shape used to pin (Alpha covers rows 1..40, Fiction is
	// every 5th row starting at 1).
	assert(abs_browse_query_init(&q, list_lib, ABS_SORT_TITLE, ABS_FILTER_TAG, "Fixture Tag Alpha", 0, false));
	audiobookshelf_browse(&q, results, ABS_BROWSE_PAGE_LIMIT, &total);
	expect_int("tag name filter total", total, 40);
	expect_str("tag filter first", results[0].id, "pg-bk-001");
	expect_str("tag filter last", results[39].id, "pg-bk-040");

	assert(abs_browse_query_init(&q, list_lib, ABS_SORT_TITLE, ABS_FILTER_GENRE, "Fiction", 0, false));
	audiobookshelf_browse(&q, results, ABS_BROWSE_PAGE_LIMIT, &total);
	expect_int("genre name filter total", total, 15);
	expect_str("genre filter first", results[0].id, "pg-bk-001");
	expect_str("genre filter last", results[14].id, "pg-bk-071");

	// The §10.1 negative: the former fixture-internal ids are NOT filter
	// values. An id-spelled genre/tag selects no rows at all -- failing
	// closed, never by name/id coincidence.
	assert(abs_browse_query_init(&q, list_lib, ABS_SORT_TITLE, ABS_FILTER_TAG, "ta-fa-1", 0, false));
	audiobookshelf_browse(&q, results, ABS_BROWSE_PAGE_LIMIT, &total);
	expect_int("id-spelled tag selects nothing", total, 0);

	assert(abs_browse_query_init(&q, list_lib, ABS_SORT_TITLE, ABS_FILTER_GENRE, "ge-fa-1", 0, false));
	audiobookshelf_browse(&q, results, ABS_BROWSE_PAGE_LIMIT, &total);
	expect_int("id-spelled genre selects nothing", total, 0);
}

static void live_groups(void) {
	audiobookshelf_library_t *list = NULL;
	expect_int("authors", audiobookshelf_groups_ex(list_lib, ABS_FILTER_AUTHOR, &list, NULL, NULL), 3);
	expect_str("author one sorted first", list[0].name, "Fixture Author One");
	expect_str("author id distinct", list[0].id, "author-fa-1");
	// Alphabetical: "One" < "Three" < "Two".
	expect_str("author three sorted second", list[1].name, "Fixture Author Three");
	expect_str("author two sorted last", list[2].name, "Fixture Author Two");
	free(list);
	list = NULL;
	expect_int("series", audiobookshelf_groups_ex(list_lib, ABS_FILTER_SERIES, &list, NULL, NULL), 2);
	expect_str("series one", list[0].name, "Fixture Series One");
	free(list);
	list = NULL;
	// Genre/tag groups: the returned `name` IS the filter value (§10.1) and
	// the object id is not carried, whatever wire shape the server picked.
	// The fixture serves the real-server bare-string shape now.
	expect_int("genres", audiobookshelf_groups_ex(list_lib, ABS_FILTER_GENRE, &list, NULL, NULL), 2);
	expect_str("genre name", list[0].name, "Fiction");
	expect_bool("genre id empty", list[0].id[0] == '\0', true);
	expect_bool("genre not oversized", list[0].oversized, false);
	free(list);
	list = NULL;
	expect_int("tags", audiobookshelf_groups_ex(list_lib, ABS_FILTER_TAG, &list, NULL, NULL), 2);
	expect_str("tag name", list[0].name, "Fixture Tag Alpha");
	expect_bool("tag id empty", list[0].id[0] == '\0', true);
	free(list);
	list = NULL;

	// §10.3 wave gate: the name-based genre filter round-trips to rows. The
	// fixture's groups return NAME values (§10.1), the mock matches rows by
	// name, so this browse is the positive end-to-end proof; the fixture gap
	// from the M2 backend report (genres/tags served as ids) is closed.
	abs_browse_query_t q;
	assert(abs_browse_query_init(&q, list_lib, ABS_SORT_TITLE, ABS_FILTER_GENRE, "Fiction", 0, false));
	int total = 0;
	int count = audiobookshelf_browse(&q, results, ABS_BROWSE_PAGE_LIMIT, &total);
	expect_int("name filter round-trip count", count, 15);
	expect_int("name filter round-trip total", total, 15);
	expect_str("name filter round-trip first", results[0].id, "pg-bk-001");
	expect_str("name filter round-trip last", results[14].id, "pg-bk-071");
	// And the tag side, from the group value the GUI would be handed:
	audiobookshelf_library_t *tags = NULL;
	expect_int("tag groups refetch", audiobookshelf_groups_ex(list_lib, ABS_FILTER_TAG, &tags, NULL, NULL), 2);
	assert(abs_browse_query_init(&q, list_lib, ABS_SORT_TITLE, ABS_FILTER_TAG, tags[0].name, 0, false));
	count = audiobookshelf_browse(&q, results, ABS_BROWSE_PAGE_LIMIT, &total);
	expect_int("tag name round-trip count", count, 40);
	expect_str("tag name round-trip first", results[0].id, "pg-bk-001");
	expect_str("tag name round-trip last", results[39].id, "pg-bk-040");
	free(tags);
}

static void live_search(void) {
	bool has_more;
	int c;

	// "zero": no corpus entry, empty but a valid search.
	has_more = true;
	c = audiobookshelf_search_ex(search_lib, "zero", results, ABS_SEARCH_DISPLAY_LIMIT, &has_more);
	expect_int("search zero", c, 0);
	expect_bool("search zero has_more", has_more, false);

	// "fourty": 40 corpus matches, well under the ceiling.
	c = audiobookshelf_search_ex(search_lib, "fourty", results, ABS_SEARCH_DISPLAY_LIMIT, &has_more);
	expect_int("search fourty", c, 40);
	expect_bool("search fourty has_more", has_more, false);

	// "hundred": exactly 100 usable matches and NO 101st: retained 100 with
	// an honest false -- "showing up to 100", never "more matches found".
	c = audiobookshelf_search_ex(search_lib, "hundred", results, ABS_SEARCH_DISPLAY_LIMIT, &has_more);
	expect_int("search hundred", c, 100);
	expect_bool("search hundred has_more", has_more, false);
	expect_str("search hundred first", results[0].id, "sc-001");
	expect_str("search hundred last", results[99].id, "sc-100");

	// "overflow": the 101st usable match exists server-side: probe evidence.
	c = audiobookshelf_search_ex(search_lib, "overflow", results, ABS_SEARCH_DISPLAY_LIMIT, &has_more);
	expect_int("search overflow", c, 100);
	expect_bool("search overflow has_more", has_more, true);
	expect_str("search overflow first", results[0].id, "sc-001");
	expect_str("search overflow last", results[99].id, "sc-100");

	// The legacy M1 entry point keeps its old request limit for the GUI.
	c = audiobookshelf_search(search_lib, "overflow", results, ABS_PAGE_LIMIT);
	expect_int("legacy search still M1", c, 40);
}

int main(int argc, char **argv) {
	if (argc != 2) {
		fprintf(stderr, "usage: %s <workdir> (against the fixture mock)\n", argv[0]);
		return 2;
	}
	work = argv[1];

	live_configure();
	live_resolve_library();
	live_browse();
	live_groups();
	live_search();
	free((void *)list_lib);
	free((void *)search_lib);

	printf("audiobookshelf-browse-test: live fixture matrix passed\n");
	return 0;
}

#endif // ABS_BROWSE_TEST_LIVE
