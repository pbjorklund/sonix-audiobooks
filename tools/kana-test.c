// Unit tests for kana.c: romaji feed/flush/backspace, the editing sink, the
// katakana conversion and its buffer boundaries, the dakuten key's cycles, and
// the kana pad's keys and flick directions.
//
// Pure: no LVGL, no allocation, no config. Run under the sanitizers by
// tools/japanese-host-test.sh, which is what gives an invalid UTF-8 walk or a
// one-byte-too-small buffer somewhere to be caught in.

#include "src/system/input/kana.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
	char text[256]; // the field, caret always at the end
	int erases;		// how many characters the sink has asked to delete, in total
	int inserts;
} sink_t;

// The tests' model of the field: the caret is always at its end, so erase()
// takes its count off the tail and insert() appends -- the same contract
// keyboard.c's sink has against a real textarea.
static void sink_erase(void *ctx, int count) {
	sink_t *s = ctx;
	assert(count >= 0);
	size_t len = strlen(s->text);
	assert(count <= (int)len);
	s->text[len - (size_t)count] = '\0';
	s->erases += count;
}

static void sink_insert(void *ctx, const char *text) {
	sink_t *s = ctx;
	assert(strlen(s->text) + strlen(text) < sizeof(s->text));
	strcat(s->text, text);
	s->inserts++;
}

static const kana_sink_t SINK = {sink_erase, sink_insert};

static sink_t sink_new(void) {
	sink_t s = {.text = {0}};
	return s;
}

// Feeds a whole romaji string through kana_feed, concatenating what it writes.
static void feed_through(kana_t *k, const char *romaji, char *out, size_t cap) {
	kana_reset(k);
	size_t at = 0;
	if (cap) {
		out[0] = '\0';
	}
	for (const char *p = romaji; *p; p++) {
		char piece[64];
		size_t n = kana_feed(k, (unsigned char)*p, piece, sizeof(piece));
		assert(n + 1 < sizeof(piece));
		if (at + n < cap) {
			memcpy(out + at, piece, n + 1);
		}
		at += n;
	}
	if (at < cap) {
		out[at] = '\0';
	}
}

// --- the feed, flush and backspace --------------------------------------------

static void feed_tests(void) {
	kana_t k;
	char out[64];

	// Complete rules become their kana as soon as the spelling is done.
	feed_through(&k, "ko", out, sizeof(out));
	assert(strcmp(out, "こ") == 0);
	feed_through(&k, "shi", out, sizeof(out));
	assert(strcmp(out, "し") == 0);
	feed_through(&k, "kya", out, sizeof(out));
	assert(strcmp(out, "きゃ") == 0);
	feed_through(&k, "xtu", out, sizeof(out));
	assert(strcmp(out, "っ") == 0);

	// A pending letter waits: nothing out, and there to be shown as pre-edit.
	kana_reset(&k);
	assert(kana_feed(&k, 'k', out, sizeof(out)) == 0);
	assert(strcmp(kana_pending(&k), "k") == 0);
	assert(strcmp(out, "") == 0);
	assert(kana_feed(&k, 'a', out, sizeof(out)) == 3);
	assert(strcmp(out, "か") == 0);
	assert(strcmp(kana_pending(&k), "") == 0);

	// The lone n: "konnichi" has one n for ん and one that opens the に.
	feed_through(&k, "kon',", out, sizeof(out));
	assert(strcmp(out, "こん,") == 0); // what is not romaji passes through
	feed_through(&k, "konniti", out, sizeof(out));
	assert(strcmp(out, "こんにち") == 0);
	feed_through(&k, "hon'ya", out, sizeof(out));
	assert(strcmp(out, "ほんや") == 0);
	feed_through(&k, "nn", out, sizeof(out));
	assert(strcmp(out, "") == 0);
	assert(strcmp(kana_pending(&k), "nn") == 0);
	assert(kana_feed(&k, 'a', out, sizeof(out)) == 6); // んな
	assert(strcmp(out, "んな") == 0);

	// Doubled consonants, and the "tch" of "match".
	feed_through(&k, "kitte", out, sizeof(out));
	assert(strcmp(out, "きって") == 0);
	feed_through(&k, "gakkou", out, sizeof(out));
	assert(strcmp(out, "がっこう") == 0);
	feed_through(&k, "otto", out, sizeof(out));
	assert(strcmp(out, "おっと") == 0);

	// Small kana, in both of their spellings.
	feed_through(&k, "xa", out, sizeof(out));
	assert(strcmp(out, "ぁ") == 0);
	feed_through(&k, "lyo", out, sizeof(out));
	assert(strcmp(out, "ょ") == 0);
	feed_through(&k, "ltsu", out, sizeof(out));
	assert(strcmp(out, "っ") == 0);

	// The long-vowel mark, and letters that are not romaji at all pass through.
	feed_through(&k, "ko-", out, sizeof(out));
	assert(strcmp(out, "こー") == 0);
	feed_through(&k, "qz1", out, sizeof(out));
	assert(strcmp(out, "qz1") == 0);

	// A buffer that exactly fits one kana takes it; a byte less cannot.
	kana_reset(&k);
	char small[4];
	memset(small, 0, sizeof(small));
	assert(kana_feed(&k, 'k', small, sizeof(small)) == 0);	// waiting gives nothing
	assert(kana_feed(&k, 'o', small, sizeof(small)) == 3);	// こ and its NUL in exactly four
	assert(strcmp(small, "こ") == 0);
	memset(small, 0, sizeof(small));
	assert(kana_feed(&k, 'k', small, sizeof(small) - 1) == 0); // cap 3 still waits
	assert(kana_feed(&k, 'o', small, sizeof(small) - 1) == 0); // こ without its NUL does not fit
	assert(strcmp(small, "") == 0);

	// Flush: the lone n is written as its kana, anything else as typed.
	feed_through(&k, "k", out, sizeof(out));
	assert(kana_flush(&k, out, sizeof(out)) == 1);
	assert(strcmp(out, "k") == 0);
	feed_through(&k, "nn", out, sizeof(out));
	assert(kana_flush(&k, out, sizeof(out)) == 3);
	assert(strcmp(out, "ん") == 0);
	kana_reset(&k);
	assert(kana_flush(&k, out, sizeof(out)) == 0);

	// The same flush against its own boundary: cap 4 holds ん, cap 3 does not.
	feed_through(&k, "n", out, sizeof(out));
	char tiny[4];
	memset(tiny, 0, sizeof(tiny));
	assert(kana_flush(&k, tiny, sizeof(tiny)) == 3);
	assert(strcmp(tiny, "ん") == 0);
	memset(tiny, 0, sizeof(tiny));
	feed_through(&k, "n", out, sizeof(out));
	assert(kana_flush(&k, tiny, sizeof(tiny) - 1) == 0);
	assert(strcmp(tiny, "") == 0);

	// Backspace: takes back waiting letters, reports none when clean.
	feed_through(&k, "kya", out, sizeof(out));
	kana_reset(&k);
	assert(kana_feed(&k, 'k', out, sizeof(out)) == 0);
	assert(kana_backspace(&k) == true);
	assert(strcmp(kana_pending(&k), "") == 0);
	assert(kana_backspace(&k) == false);
}

// --- typing into a field ------------------------------------------------------

static void sink_tests(void) {
	kana_t k;
	sink_t s;

	// A letter writes its waiting prefix in, and replaces it when it settles.
	s = sink_new();
	kana_reset(&k);
	kana_edit_key(&k, false, 'k', &SINK, &s);
	assert(strcmp(s.text, "k") == 0 && s.erases == 0 && s.inserts == 1);
	kana_edit_key(&k, false, 'y', &SINK, &s);
	assert(strcmp(s.text, "ky") == 0 && s.erases == 1);
	kana_edit_key(&k, false, 'o', &SINK, &s);
	assert(strcmp(s.text, "きょ") == 0 && s.erases == 3);

	// Flush: the waiting letters are taken back and settled in their kana.
	s = sink_new();
	kana_reset(&k);
	kana_edit_key(&k, false, 'n', &SINK, &s);
	assert(strcmp(s.text, "n") == 0);
	kana_edit_flush(&k, false, &SINK, &s);
	assert(strcmp(s.text, "ん") == 0 && s.erases == 1);

	// Flush with nothing waiting is a no-op.
	kana_reset(&k);
	int erases = s.erases;
	int inserts = s.inserts;
	kana_edit_flush(&k, false, &SINK, &s);
	assert(s.erases == erases && s.inserts == inserts);

	// Backspace: waiting letters first, one at a time; the field deletes when
	// nothing is waiting.
	s = sink_new();
	kana_reset(&k);
	assert(kana_edit_backspace(&k, &SINK, &s) == false);
	kana_edit_key(&k, false, 'k', &SINK, &s);
	assert(strcmp(s.text, "k") == 0);
	assert(kana_edit_backspace(&k, &SINK, &s) == true);
	assert(strcmp(s.text, "") == 0 && s.erases == 1);
	assert(kana_edit_backspace(&k, &SINK, &s) == false);

	// Katakana mode converts what goes in.
	s = sink_new();
	kana_reset(&k);
	kana_edit_key(&k, true, 'k', &SINK, &s);
	kana_edit_key(&k, true, 'i', &SINK, &s);
	kana_edit_key(&k, true, 't', &SINK, &s);
	kana_edit_key(&k, true, 'a', &SINK, &s);
	assert(strcmp(s.text, "キタ") == 0);
}

// --- katakana, and its buffer edges -------------------------------------------

static void katakana_tests(void) {
	char out[64];

	// Whole strings, with the ASCII left as it is.
	kana_to_katakana("あいうえおかき", out, sizeof(out));
	assert(strcmp(out, "アイウエオカキ") == 0);
	kana_to_katakana("っ、ぁゖ", out, sizeof(out));
	assert(strcmp(out, "ッ、ァヶ") == 0);
	kana_to_katakana("がぱ", out, sizeof(out));
	assert(strcmp(out, "ガパ") == 0);

	// The exact fit this test exists for: one kana in exactly four bytes.
	char exact[4];
	kana_to_katakana("か", exact, sizeof(exact));
	assert(strcmp(exact, "カ") == 0);
	kana_to_katakana("か", exact, 3); // kana and NUL cannot both fit
	assert(strcmp(exact, "") == 0);
	kana_to_katakana("か", exact, 0);
	kana_to_katakana("か", exact, 1);
	kana_to_katakana("あa", exact, sizeof(exact)); // cap 4: the kana takes the room
	assert(strcmp(exact, "ア") == 0);
	kana_to_katakana("あa", out, 5);
	assert(strcmp(out, "アa") == 0);

	// Invalid UTF-8 passes through exactly, not dropped or made up.
	kana_to_katakana("\xff" "k", out, sizeof(out));
	assert(strcmp(out, "\xff" "k") == 0);
	kana_to_katakana("\xc3\x28", out, sizeof(out)); // a two-byte lead with no continuation
	assert(strcmp(out, "\xc3\x28") == 0);

	// The long-vowel mark (already katakana) is untouched.
	kana_to_katakana("ー", out, sizeof(out));
	assert(strcmp(out, "ー") == 0);
}

// --- the dakuten key and the character before the caret ------------------------

static void cycle_tests(void) {
	char out[8];
	char prev[8];

	// Dakuten and handakuten, each cycle round to the plain form.
	assert(kana_cycle_mark("か", out, sizeof(out)) && strcmp(out, "が") == 0);
	assert(kana_cycle_mark("が", out, sizeof(out)) && strcmp(out, "か") == 0);
	assert(kana_cycle_mark("は", out, sizeof(out)) && strcmp(out, "ば") == 0);
	assert(kana_cycle_mark("ば", out, sizeof(out)) && strcmp(out, "ぱ") == 0);
	assert(kana_cycle_mark("ぱ", out, sizeof(out)) && strcmp(out, "は") == 0);
	assert(kana_cycle_mark("ふ", out, sizeof(out)) && strcmp(out, "ぶ") == 0);
	assert(kana_cycle_mark("つ", out, sizeof(out)) && strcmp(out, "っ") == 0); // small tsu
	assert(kana_cycle_mark("っ", out, sizeof(out)) && strcmp(out, "づ") == 0);
	assert(kana_cycle_mark("づ", out, sizeof(out)) && strcmp(out, "つ") == 0);
	assert(kana_cycle_mark("う", out, sizeof(out)) && strcmp(out, "ぅ") == 0); // the small form
	assert(kana_cycle_mark("ぅ", out, sizeof(out)) && strcmp(out, "ゔ") == 0);
	assert(kana_cycle_mark("ゔ", out, sizeof(out)) && strcmp(out, "う") == 0);
	assert(kana_cycle_mark("ゎ", out, sizeof(out)) && strcmp(out, "わ") == 0);
	assert(kana_cycle_mark("ゖ", out, sizeof(out)) == false); // no cycle it knows

	// Katakana in, katakana out.
	assert(kana_cycle_mark("ハ", out, sizeof(out)) && strcmp(out, "バ") == 0);
	assert(kana_cycle_mark("バ", out, sizeof(out)) && strcmp(out, "パ") == 0);
	assert(kana_cycle_mark("パ", out, sizeof(out)) && strcmp(out, "ハ") == 0);
	assert(kana_cycle_mark("ツ", out, sizeof(out)) && strcmp(out, "ッ") == 0);

	// Not a kana, not three bytes, no room, nothing at all.
	assert(kana_cycle_mark("a", out, sizeof(out)) == false);
	assert(kana_cycle_mark("あa", out, sizeof(out)) == false);
	assert(kana_cycle_mark("", out, sizeof(out)) == false);
	assert(kana_cycle_mark(NULL, out, sizeof(out)) == false);
	memcpy(prev, "あ", 3), prev[3] = '\0';
	assert(kana_cycle_mark(prev, out, 4) && strcmp(out, "ぁ") == 0); // the small form, in exact room
	assert(kana_cycle_mark(prev, out, 3) == false);

	// The character before the caret, counted in characters not bytes.
	assert(kana_prev_char("たべ", 2, out, sizeof(out)) && strcmp(out, "べ") == 0);
	assert(kana_prev_char("たべ", 1, out, sizeof(out)) && strcmp(out, "た") == 0);
	assert(kana_prev_char("たべ", 0, out, sizeof(out)) == false);
	assert(kana_prev_char("たべ", 3, out, sizeof(out)) == false); // the caret is past the end
	assert(kana_prev_char("たべ", 1, out, 2) == false);			  // three bytes do not fit
	// A sequence cut off by the NUL ends the walk instead of running on.
	assert(kana_prev_char("\xe3\x81", 1, out, sizeof(out)) == false);
}

// --- the kana pad --------------------------------------------------------------

static void key_tests(void) {
	// Key rows: lengths, caps and steps.
	assert(kana_key_len(0) == 5);
	assert(kana_key_len(7) == 3);  // やゆよ
	assert(kana_key_len(10) == 0); // the marks key is not a cycle
	assert(kana_key_len(11) == 4);
	assert(kana_key_len(12) == 0);
	assert(kana_key_len(-1) == 0);

	assert(strcmp(kana_key_cap(0, false), "あ") == 0);
	assert(strcmp(kana_key_cap(0, true), "ア") == 0);
	assert(strcmp(kana_key_cap(9, false), "、。") == 0); // its first two marks
	assert(strcmp(kana_key_cap(10, false), "") == 0);

	assert(strcmp(kana_key_step(0, 0, false), "あ") == 0);
	assert(strcmp(kana_key_step(0, 4, false), "お") == 0);
	assert(strcmp(kana_key_step(7, 2, false), "よ") == 0);
	assert(strcmp(kana_key_step(7, 3, false), "") == 0); // no fourth
	assert(strcmp(kana_key_step(0, 1, true), "イ") == 0);
	assert(strcmp(kana_key_step(0, 5, false), "") == 0);
	assert(strcmp(kana_key_step(0, -1, false), "") == 0);

	// Flick directions: the longer axis wins, a short drag stays a tap, ties
	// go vertical.
	assert(kana_flick_dir(-100, 0, 22) == KANA_DIR_LEFT);
	assert(kana_flick_dir(100, 0, 22) == KANA_DIR_RIGHT);
	assert(kana_flick_dir(0, -100, 22) == KANA_DIR_UP);
	assert(kana_flick_dir(0, 100, 22) == KANA_DIR_DOWN);
	assert(kana_flick_dir(0, 0, 22) == KANA_DIR_CENTER);
	assert(kana_flick_dir(10, 10, 22) == KANA_DIR_CENTER); // both under the threshold
	assert(kana_flick_dir(30, 30, 22) == KANA_DIR_DOWN);   // a tie on the axes
	assert(kana_flick_dir(21, 0, 22) == KANA_DIR_CENTER);
	assert(kana_flick_dir(22, 0, 22) == KANA_DIR_RIGHT);
	assert(kana_flick_dir(-1, -2, 2) == KANA_DIR_UP);

	// The flicked kana, centre included; katakana follows the mode.
	assert(strcmp(kana_flick_step(0, KANA_DIR_CENTER, false), "あ") == 0);
	assert(strcmp(kana_flick_step(0, KANA_DIR_UP, false), "う") == 0);
	assert(strcmp(kana_flick_step(0, KANA_DIR_LEFT, true), "イ") == 0);
	assert(strcmp(kana_flick_step(11, KANA_DIR_RIGHT, false), "ー") == 0);
	assert(strcmp(kana_flick_step(11, KANA_DIR_DOWN, false), "") == 0); // the wa row has four
	assert(strcmp(kana_flick_step(7, KANA_DIR_LEFT, false), "") == 0);  // ya flicks up and down only
	assert(strcmp(kana_flick_step(7, KANA_DIR_UP, false), "ゆ") == 0);
	assert(strcmp(kana_flick_step(10, KANA_DIR_UP, false), "") == 0); // the marks key flicks nothing
	assert(strcmp(kana_flick_step(-1, KANA_DIR_UP, false), "") == 0);
}

int main(void) {
	feed_tests();
	sink_tests();
	katakana_tests();
	cycle_tests();
	key_tests();
	printf("kana-test: all checks passed\n");
	return 0;
}
