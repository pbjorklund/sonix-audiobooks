#define _XOPEN_SOURCE 700
#include "abs_show_reader.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "src/system/core/json.h"

#define WIRE_LIMIT (16u * 1024 * 1024)
#define ROW_LIMIT 4096
_Static_assert(ROW_LIMIT * sizeof(abs_episode_row_t) <= 2u * 1024 * 1024,
	"show index exceeds the R1 memory budget");

typedef struct {
	int (*read)(void *, void *, int);
	void *user;
	unsigned char bytes[8192];
	int i, end;
	size_t received;
	bool failed;
} reader_t;

static int peek(reader_t *r) {
	if (r->end < 0) return -1;
	if (r->i == r->end) {
		int got = r->read(r->user, r->bytes, sizeof(r->bytes));
		if (got <= 0 || got > (int)sizeof(r->bytes) || (size_t)got > WIRE_LIMIT - r->received) {
			r->failed = got != 0;
			r->end = -1;
			return -1;
		}
		r->received += (size_t)got;
		r->i = 0;
		r->end = got;
	}
	return r->bytes[r->i];
}

static int next(reader_t *r) { int c = peek(r); if (c >= 0) r->i++; return c; }
static bool take(reader_t *r, int c) { if (peek(r) != c) return false; next(r); return true; }
static void space(reader_t *r) {
	int c;
	while ((c = peek(r)) == ' ' || c == '\n' || c == '\r' || c == '\t') next(r);
}
static bool digit(int c) { return c >= '0' && c <= '9'; }
static bool hex(int c) { return digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

static void save(char *buf, size_t cap, size_t *n, int c) {
	if (*n + 1 < cap) buf[*n] = (char)c;
	(*n)++;
}

// If a string is needed, capture a bounded raw prefix, then decode escapes
// via the existing JSON string extractor. Large skipped values use no buffer.
static bool string(reader_t *r, char *raw, size_t cap, size_t *len) {
	if (!take(r, '"')) return false;
	size_t n = 0;
	for (;;) {
		int c = next(r);
		if (c < 32) return false;
		if (c == '"') break;
		save(raw, cap, &n, c);
		if (c != '\\') continue;
		c = next(r);
		if (c <= 0 || !strchr("\"\\/bfnrtu", c)) return false;
		save(raw, cap, &n, c);
		if (c == 'u') for (int j = 0; j < 4; j++) {
			c = next(r);
			if (!hex(c)) return false;
			save(raw, cap, &n, c);
		}
	}
	if (cap) raw[n < cap ? n : cap - 1] = '\0';
	if (len) *len = n;
	return true;
}

static bool number(reader_t *r, char *buf, size_t cap) {
	size_t n = 0;
	if (take(r, '-')) save(buf, cap, &n, '-');
	if (!digit(peek(r))) return false;
	int c = next(r);
	save(buf, cap, &n, c);
	if (c != '0') while (digit(peek(r))) save(buf, cap, &n, next(r));
	if (take(r, '.')) {
		save(buf, cap, &n, '.');
		if (!digit(peek(r))) return false;
		while (digit(peek(r))) save(buf, cap, &n, next(r));
	}
	c = peek(r);
	if (c == 'e' || c == 'E') {
		save(buf, cap, &n, next(r));
		c = peek(r);
		if (c == '-' || c == '+') save(buf, cap, &n, next(r));
		if (!digit(peek(r))) return false;
		while (digit(peek(r))) save(buf, cap, &n, next(r));
	}
	if (cap) {
		buf[n < cap ? n : 0] = '\0';
	}
	return true;
}

static bool skip(reader_t *r, int depth) {
	if (depth >= JSON_MAX_DEPTH) return false;
	space(r);
	int c = peek(r);
	if (c == '"') return string(r, NULL, 0, NULL);
	if (c == '-' || digit(c)) return number(r, NULL, 0);
	if (c == '{' || c == '[') {
		next(r);
		int end = c == '{' ? '}' : ']';
		space(r);
		if (take(r, end)) return true;
		for (;;) {
			if (c == '{') {
				if (!string(r, NULL, 0, NULL)) return false;
				space(r);
				if (!take(r, ':')) return false;
			}
			if (!skip(r, depth + 1)) return false;
			space(r);
			if (take(r, end)) return true;
			if (!take(r, ',')) return false;
			space(r);
		}
	}
	const char *literal = c == 't' ? "true" : c == 'f' ? "false" : c == 'n' ? "null" : NULL;
	if (!literal) return false;
	while (*literal) if (!take(r, *literal++)) return false;
	return true;
}

static bool key(reader_t *r, char *out, size_t cap) {
	char raw[384];
	size_t n;
	if (!string(r, raw, sizeof(raw), &n)) return false;
	out[0] = '\0';
	if (n < sizeof(raw)) {
		json_tok_t tok = {.type = JSON_STRING, .end = (int)n};
		json_doc_t doc = {.text = raw, .toks = &tok, .count = 1};
		json_str(&doc, 0, out, cap);
	}
	space(r);
	if (!take(r, ':')) return false;
	space(r);
	return true;
}

static bool identity(reader_t *r, char *out, size_t cap) {
	size_t n;
	return string(r, out, cap, &n) && n > 0 && n < cap && !strchr(out, '\\') &&
		!strchr(out, '/') && strcmp(out, ".") && strcmp(out, "..");
}

static bool close_or_comma(reader_t *r, int close, bool *end) {
	space(r);
	if (take(r, close)) { *end = true; return true; }
	if (!take(r, ',')) return false;
	space(r);
	*end = false;
	return true;
}

static long long pub_date(const char *s) {
	struct tm tm = {0};
	// ABS emits RSS dates. Only compare dates with an unambiguous UTC offset.
	char *end = strptime(s, "%a, %d %b %Y %H:%M:%S", &tm);
	if (!end) return 0;
	while (*end == ' ') end++;
	int offset = 0;
	if (strcmp(end, "GMT") && strcmp(end, "UTC")) {
		if (strlen(end) != 5 || (*end != '+' && *end != '-') ||
			!digit(end[1]) || !digit(end[2]) || !digit(end[3]) || !digit(end[4])) return 0;
		int hours = (end[1] - '0') * 10 + end[2] - '0', mins = (end[3] - '0') * 10 + end[4] - '0';
		if (hours >= 24 || mins >= 60) return 0;
		offset = (hours * 60 + mins) * 60 * (*end == '-' ? -1 : 1);
	}
	// Avoid duplicate dependence on the system zone and on time_t size.
	int month = tm.tm_mon + 1, year = tm.tm_year + 1900 - (month < 3);
	if (year < 1969 || tm.tm_mday < 1 || month < 1 || month > 12) return 0;
	int era = year / 400, y = year - era * 400;
	int day = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + tm.tm_mday - 1;
	long long days = (long long)era * 146097 + y * 365 + y / 4 - y / 100 + day - 719468;
	long long sec = days * 86400 + tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec - offset;
	return sec > 0 ? sec * 1000 : 0;
}

static bool episode(reader_t *r, abs_episode_row_t *row, const char *show) {
	if (!take(r, '{')) return false;
	space(r);
	long long fallback_date = 0;
	bool end = false;
	do {
		char name[32];
		if (!key(r, name, sizeof(name))) return false;
		int c = peek(r);
		if (!strcmp(name, "id")) {
			if (row->episode_id[0] || !identity(r, row->episode_id, sizeof(row->episode_id))) return false;
		} else if (!strcmp(name, "libraryItemId")) {
			if (row->show_id[0] || !identity(r, row->show_id, sizeof(row->show_id))) return false;
		} else if (c == '"' && (!strcmp(name, "title") || !strcmp(name, "pubDate"))) {
			char raw[1536];
			size_t len;
			if (!string(r, raw, sizeof(raw), &len)) return false;
			if (!strcmp(name, "pubDate")) fallback_date = pub_date(raw);
			else {
				json_tok_t tok = {.type = JSON_STRING, .end = (int)(len < sizeof(raw) ? len : sizeof(raw) - 1)};
				json_doc_t doc = {.text = raw, .toks = &tok, .count = 1};
				json_str(&doc, 0, row->title, sizeof(row->title));
			}
		} else if ((digit(c) || c == '-') && (!strcmp(name, "publishedAt") ||
				!strcmp(name, "addedAt") || !strcmp(name, "duration"))) {
			char raw[40];
			if (!number(r, raw, sizeof(raw))) return false;
			if (!strcmp(name, "duration")) {
				double value = strtod(raw, NULL);
				if (isfinite(value) && value > 0) row->duration = value;
			} else {
				double stamp = strtod(raw, NULL);
				if (isfinite(stamp) && stamp > 0 && stamp < 253402300800000.) {
					if (!strcmp(name, "publishedAt")) row->published_ms = (long long)stamp;
					else row->added_ms = (long long)stamp;
				}
			}
		} else if (!skip(r, 4)) return false;
		if (!close_or_comma(r, '}', &end)) return false;
	} while (!end);
	if (!row->published_ms) row->published_ms = fallback_date;
	return row->episode_id[0] && !strcmp(row->show_id, show);
}

typedef struct { abs_episode_row_t *rows; int count, cap; bool found; } rows_t;

static bool episodes(reader_t *r, rows_t *rows, const char *show) {
	if (!take(r, '[')) return false;
	space(r);
	if (take(r, ']')) return true;
	bool end = false;
	do {
		if (rows->count == ROW_LIMIT) return false;
		if (rows->count == rows->cap) {
			int cap = rows->cap ? rows->cap * 2 : 32;
			abs_episode_row_t *grown = realloc(rows->rows, (size_t)cap * sizeof(*grown));
			if (!grown) return false;
			rows->rows = grown;
			rows->cap = cap;
		}
		abs_episode_row_t *row = &rows->rows[rows->count++];
		memset(row, 0, sizeof(*row));
		if (!episode(r, row, show) || !close_or_comma(r, ']', &end)) return false;
	} while (!end);
	return true;
}

static bool media(reader_t *r, rows_t *rows, const char *show) {
	if (!take(r, '{')) return false;
	space(r);
	if (take(r, '}')) return true;
	bool end = false;
	do {
		char name[32];
		if (!key(r, name, sizeof(name))) return false;
		if (!strcmp(name, "episodes")) {
			if (rows->found || !episodes(r, rows, show)) return false;
			rows->found = true;
		} else if (!skip(r, 2)) return false;
		if (!close_or_comma(r, '}', &end)) return false;
	} while (!end);
	return true;
}

static bool show_doc(reader_t *r, rows_t *rows, const char *show) {
	space(r);
	if (!take(r, '{')) return false;
	space(r);
	char id[ABS_ID_MAX] = "", type[24] = "";
	bool end = false, media_seen = false;
	do {
		char name[32];
		if (!key(r, name, sizeof(name))) return false;
		if (!strcmp(name, "id")) {
			if (id[0] || !identity(r, id, sizeof(id))) return false;
		} else if (!strcmp(name, "mediaType")) {
			if (type[0] || !identity(r, type, sizeof(type))) return false;
		} else if (!strcmp(name, "media")) {
			if (media_seen || !media(r, rows, show)) return false;
			media_seen = true;
		} else if (!skip(r, 1)) return false;
		if (!close_or_comma(r, '}', &end)) return false;
	} while (!end);
	space(r);
	return !r->failed && peek(r) < 0 && !r->failed && rows->found && !strcmp(id, show) && !strcmp(type, "podcast");
}

static int id_order(const void *a, const void *b) {
	return strcmp(((const abs_episode_row_t *)a)->episode_id, ((const abs_episode_row_t *)b)->episode_id);
}

static int date_order(const void *a, const void *b) {
	const abs_episode_row_t *x = a, *y = b;
	if (x->published_ms != y->published_ms) return x->published_ms > y->published_ms ? -1 : 1;
	if (x->added_ms != y->added_ms) return x->added_ms > y->added_ms ? -1 : 1;
	return id_order(a, b);
}

int abs_show_read(int (*read)(void *, void *, int), void *user,
	const char *show_id, abs_episode_row_t **out) {
	if (!out) return -1;
	*out = NULL;
	if (!read || !show_id) return -1;
	reader_t r = {.read = read, .user = user};
	rows_t rows = {0};
	if (!show_doc(&r, &rows, show_id)) { free(rows.rows); return -1; }
	if (rows.count > 1) {
		qsort(rows.rows, (size_t)rows.count, sizeof(*rows.rows), id_order);
		for (int i = 1; i < rows.count; i++) if (!id_order(&rows.rows[i - 1], &rows.rows[i])) {
			free(rows.rows);
			return -1;
		}
		qsort(rows.rows, (size_t)rows.count, sizeof(*rows.rows), date_order);
	}
	*out = rows.rows;
	return rows.count;
}
