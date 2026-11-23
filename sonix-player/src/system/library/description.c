#include "description.h"

// The shared description→plain-text policy (see description.h). Pure and
// allocation-free on purpose: metadata.c owns the candidate ranks and the
// thread-local offer state, the ABS detail parser owns its caller-owned
// model, and both call this file for every byte-level decision so remote and
// local ingestion can never disagree.

#include <ctype.h>
#include <stdbool.h>
#include <string.h>

// The HTML tags that end a paragraph or a line: <p>, </p>, <br>, <li>, <div>.
static bool description_tag_breaks(const char *tag, size_t len) {
	while (len && isspace((unsigned char)*tag)) {
		tag++;
		len--;
	}
	if (len && *tag == '/') {
		tag++;
		len--;
	}
	if (len >= 1 && tolower((unsigned char)tag[0]) == 'p' && (len == 1 || !isalpha((unsigned char)tag[1]))) {
		return true;
	}
	if (len >= 2 && tolower((unsigned char)tag[0]) == 'b' && tolower((unsigned char)tag[1]) == 'r') {
		return true;
	}
	if (len >= 2 && tolower((unsigned char)tag[0]) == 'l' && tolower((unsigned char)tag[1]) == 'i') {
		return true;
	}
	return len >= 3 && strncasecmp(tag, "div", 3) == 0;
}

static const struct {
	const char *entity;
	char value;
} ENTITIES[] = {
	{"&amp;", '&'}, {"&quot;", '"'}, {"&#39;", '\''}, {"&apos;", '\''}, {"&lt;", '<'}, {"&gt;", '>'}, {"&nbsp;", ' '},
};

// One scanner for both policies. `markup` selects the HTML reading (tags
// dropped, entities decoded); without it, '<' and '&' are ordinary text for
// the already-plain sources. Every write checks the room first, so the scan
// is bounded by out_size whatever comes in; each rule below replaces at most
// one byte per input byte, so a caller whose out_size covers the input never
// truncates.
static size_t description_normalize(const char *text, size_t len, bool markup,
									char *out, size_t out_size) {
	if (!out || out_size == 0) {
		return 0;
	}
	out[0] = '\0';
	if (!text || len == 0) {
		return 0;
	}
	size_t written = 0;
	for (size_t i = 0; i < len && text[i];) {
		if (markup && text[i] == '<') {
			const char *end = memchr(text + i + 1, '>', len - i - 1);
			if (end) {
				if (description_tag_breaks(text + i + 1, (size_t)(end - text - i - 1)) &&
					written && out[written - 1] != '\n') {
					if (written + 1 >= out_size) break;
					out[written++] = '\n';
				}
				i = (size_t)(end - text) + 1;
				continue;
			}
		}
		bool entity = false;
		if (markup) {
			for (size_t e = 0; e < sizeof(ENTITIES) / sizeof(ENTITIES[0]); e++) {
				size_t n = strlen(ENTITIES[e].entity);
				if (n <= len - i && memcmp(text + i, ENTITIES[e].entity, n) == 0) {
					if (written + 1 >= out_size) break;
					out[written++] = ENTITIES[e].value;
					i += n;
					entity = true;
					break;
				}
			}
			if (entity) {
				continue;
			}
		}
		char c = text[i++];
		if (c == '\r') {
			c = '\n';
		}
		if (c == '\n' && (!written || out[written - 1] == '\n')) {
			continue;
		}
		if (c == '\t') {
			c = ' ';
		}
		if (c == ' ' && (!written || out[written - 1] == '\n')) {
			continue;
		}
		if (written + 1 >= out_size) break;
		out[written++] = c;
	}
	while (written && isspace((unsigned char)out[written - 1])) {
		written--;
	}
	out[written] = '\0';
	return written;
}

size_t description_from_html(const char *html, size_t len, char *out, size_t out_size) {
	return description_normalize(html, len, true, out, out_size);
}

size_t description_from_plain(const char *plain, size_t len, char *out, size_t out_size) {
	return description_normalize(plain, len, false, out, out_size);
}

size_t description_pick(const char *plain, const char *raw, char *out, size_t out_size) {
	if (out && out_size) {
		out[0] = '\0';
	}
	// §9.1: descriptionPlain is the preferred source; a plain value that is
	// empty or all whitespace falls back to the raw HTML description.
	size_t written = plain ? description_from_plain(plain, strlen(plain), out, out_size) : 0;
	if (!written) {
		written = raw ? description_from_html(raw, strlen(raw), out, out_size) : 0;
	}
	return written;
}
