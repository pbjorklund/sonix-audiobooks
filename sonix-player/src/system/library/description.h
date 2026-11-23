#ifndef DESCRIPTION_H
#define DESCRIPTION_H

#include <stddef.h>

// The description cleanup the audiobook library and the Audiobookshelf detail
// page share (spec §9.1/§9.3/§9.4). Publishers paste the store page's HTML
// into a description, so the policy is one place, pure and allocation-free:
//
//   * HTML markup is dropped -- it is never rendered as LVGL markup and never
//     left as raw publisher HTML. <p>, </p>, <br>, <li> and <div> end a line,
//     every other tag disappears.
//   * The common entities turn back into their characters
//     (&amp; &quot; &#39; &apos; &lt; &gt; &nbsp;).
//   * Control characters normalize: '\r' becomes '\n'; runs of blank lines
//     collapse to one; tabs become spaces; a space that starts a line goes;
//     trailing whitespace is trimmed. The first embedded NUL ends the scan.
//
// Everything is bounded: the scan stops when one byte short of the end of
// `out` is written, and a cleanup can never grow the text (tags and entities
// only shrink or keep one byte), so a caller that supplies the input length as
// the capacity never truncates.

// The shared 8 KiB description ceiling (§9.3 ABS_DETAIL_DESCRIPTION_MAX,
// metadata.c's local DESCRIPTION_MAX_BYTES, one constant for all of them).
#define DESCRIPTION_TEXT_MAX 8192

// Full cleanup for raw publisher HTML/markup. At most `len` bytes of `text`
// are read; the cleaned text is written NUL-terminated into `out`. Returns
// the number of bytes written excluding the terminator (0 when nothing
// usable survived).
size_t description_from_html(const char *text, size_t len, char *out, size_t out_size);

// The same newline/control policy for already-plain text (ABS sends
// `descriptionPlain` pre-cleaned): '<' and '&' are kept literally, because
// plain prose is free to contain them ("a < b", "Rock & Roll").
size_t description_from_plain(const char *text, size_t len, char *out, size_t out_size);

// The §9.1 selection: prefer `plain` (descriptionPlain); when it is absent,
// empty or whitespace-only, clean `raw` with the HTML policy. Returns the
// bytes written (0 when neither source has usable text).
size_t description_pick(const char *plain, const char *raw, char *out, size_t out_size);

#endif /* DESCRIPTION_H */
