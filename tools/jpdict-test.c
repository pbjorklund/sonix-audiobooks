// Unit tests for jpdict.c: exact and okurigana lookup, candidate extraction
// with exact-fit buffers, and the lookup-before-the-caret walk with its
// UTF-8 and buffer boundaries.
//
// Pure: the dictionary is const data compiled in; there is no LVGL, no
// allocation and no file. Run under the sanitizers by
// tools/japanese-host-test.sh.

#include "src/system/input/jpdict.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

// Every reading below was checked against the generated dictionary
// (jpdict_data.c), so a regeneration that removes one makes the test fail
// loudly rather than testing nothing.

// --- jpdict_lookup ------------------------------------------------------------

static void lookup_tests(void) {
	jpdict_hit_t hit;
	char cand[JPDICT_CAND_MAX];

	// An exact key with three candidates, in order.
	assert(jpdict_lookup("はし", &hit) == true);
	assert(jpdict_candidate(&hit, 0, cand, sizeof(cand)) == 3 && strcmp(cand, "箸") == 0);
	assert(jpdict_candidate(&hit, 1, cand, sizeof(cand)) == 3 && strcmp(cand, "橋") == 0);
	assert(jpdict_candidate(&hit, 2, cand, sizeof(cand)) == 3 && strcmp(cand, "端") == 0);
	assert(hit.count == 3);
	assert(jpdict_candidate(&hit, 3, cand, sizeof(cand)) == 0); // past the list

	// A single candidate, and a long reading.
	assert(jpdict_lookup("みず", &hit) == true && hit.count == 1);
	assert(jpdict_candidate(&hit, 0, cand, sizeof(cand)) == 3 && strcmp(cand, "水") == 0);
	assert(jpdict_lookup("にほんご", &hit) == true);
	assert(jpdict_candidate(&hit, 0, cand, sizeof(cand)) == 9 && strcmp(cand, "日本語") == 0);
	// (ゆき has a verb stem of its own: 行き comes with the exact key's 雪.)

	// Both kinds of entry can answer at once, and the stem comes first. The
	// okurigana goes back as it was typed (the way lv_ime_jp_lite answers its
	// "た" + b-row reading: the stem's kanji, then the kana the user wrote).
	assert(jpdict_lookup("たび", &hit) == true);
	assert(hit.count == 3);
	assert(jpdict_candidate(&hit, 0, cand, sizeof(cand)) == 6 && strcmp(cand, "食び") == 0);
	assert(jpdict_candidate(&hit, 1, cand, sizeof(cand)) == 3 && strcmp(cand, "度") == 0);
	assert(jpdict_candidate(&hit, 2, cand, sizeof(cand)) == 3 && strcmp(cand, "旅") == 0);

	// An okurigana reading: the stem's kanji with the kana after it put back.
	assert(jpdict_lookup("たべ", &hit) == true);
	assert(jpdict_candidate(&hit, 0, cand, sizeof(cand)) == 6 && strcmp(cand, "食べ") == 0);
	assert(jpdict_lookup("みる", &hit) == true);
	assert(hit.count == 2); // 見 and 診
	assert(jpdict_candidate(&hit, 0, cand, sizeof(cand)) == 6 && strcmp(cand, "見る") == 0);
	assert(jpdict_candidate(&hit, 1, cand, sizeof(cand)) == 6 && strcmp(cand, "診る") == 0);

	// ... but only with kana of the right row after the stem.
	assert(jpdict_lookup("たざ", &hit) == false);

	// A whole reading that is not the stem: not answered at all.
	assert(jpdict_lookup("たべる", &hit) == false);
	assert(jpdict_lookup("たべたい", &hit) == false);

	// A stem with an ending only applies to the verbs that have it in the
	// table; a two-kana reading matches nothing with one.
	// Nothing there at all.
	assert(jpdict_lookup(NULL, &hit) == false);
	assert(jpdict_lookup("", &hit) == false); // under the smallest length
	assert(jpdict_lookup("x", &hit) == false); // one byte is still under it
	assert(jpdict_lookup("xyz", &hit) == false);
	assert(jpdict_lookup("はシ", &hit) == false); // katakana is not hiragana
	assert(jpdict_lookup("た" "\xe3\x81", &hit) == false); // cut off by the NUL
	assert(jpdict_lookup("\xe3\x81\xff" "し", &hit) == false); // not even a head

	// Candidate extraction refuses a smaller cap and takes an exact fit.
	assert(jpdict_lookup("みず", &hit) == true);
	memset(cand, '#', sizeof(cand) - 1), cand[sizeof(cand) - 1] = '\0';
	assert(jpdict_candidate(&hit, 0, cand, 3) == 0);
	assert(cand[0] == '\0'); // refused, and left empty
	memset(cand, 0, sizeof(cand));
	assert(jpdict_candidate(&hit, 0, cand, 4) == 3);
	assert(strcmp(cand, "水") == 0); // three bytes and the NUL, exactly
	assert(jpdict_candidate(NULL, 0, cand, sizeof(cand)) == 0);
	assert(jpdict_candidate(&hit, -1, cand, sizeof(cand)) == 0);
	assert(jpdict_candidate(&hit, 0, cand, 0) == 0);
	// A stem's own candidate plus its okurigana, exactly at the boundary.
	assert(jpdict_lookup("たべ", &hit) == true);
	assert(jpdict_candidate(&hit, 0, cand, 6) == 0);   // 食べ is six bytes...
	assert(jpdict_candidate(&hit, 0, cand, 7) == 6);   // ...and six plus the NUL fits
	assert(strcmp(cand, "食べ") == 0);
}

// --- jpdict_lookup_before ------------------------------------------------------

static void lookup_before_tests(void) {
	jpdict_hit_t hit;
	char cand[JPDICT_CAND_MAX];
	char reading[3 * 8 + 1];
	unsigned chars = 0;

	// The hiragana before the caret, the whole run looked up first.
	assert(jpdict_lookup_before("これはし", 4, 8, &hit, reading, sizeof(reading), &chars) == true);
	assert(chars == 4 && strcmp(reading, "これはし") == 0); // the whole run happens to be a key
	assert(jpdict_lookup_before("xはし", 3, 8, &hit, reading, sizeof(reading), &chars) == true);
	assert(chars == 2 && strcmp(reading, "はし") == 0); // letters stop the run

	// A caret one character in sees only what is before it.
	assert(jpdict_lookup_before("xた", 2, 8, &hit, reading, sizeof(reading), &chars) == true);
	assert(chars == 1 && strcmp(reading, "た") == 0);

	// A verb: the stem and its okurigana are looked up, the ending rides along.
	assert(jpdict_lookup_before("たべるは", 3, 8, &hit, reading, sizeof(reading), &chars) == true);
	assert(chars == 3 && strcmp(reading, "たべる") == 0);
	assert(jpdict_candidate(&hit, 0, cand, sizeof(cand)) == 9 && strcmp(cand, "食べる") == 0);

	// A longer ending, up to the three the table allows.
	assert(jpdict_lookup_before("たべたいは", 4, 8, &hit, reading, sizeof(reading), &chars) == true);
	assert(chars == 4);
	assert(jpdict_candidate(&hit, 0, cand, sizeof(cand)) == 12 && strcmp(cand, "食べたい") == 0);

	// The run only reaches through what is actually there: the pieces a run
	// without a whole-word hit is tried in are its own last n kana, so
	// "はしを" falls through はしを, しを and を and answers nothing.
	assert(jpdict_lookup_before("はしを", 3, 8, &hit, reading, sizeof(reading), &chars) == false);
	assert(jpdict_lookup_before("たべ、", 3, 8, &hit, reading, sizeof(reading), &chars) == false);

	// The run stops at the caret's max_chars before anything else.
	assert(jpdict_lookup_before("おおおおおお", 6, 2, &hit, reading, sizeof(reading), &chars) == true);
	assert(chars == 2 && strcmp(reading, "おお") == 0);

	// The run stops at what is before the caret: katakana and letters are not
	// part of it.
	assert(jpdict_lookup_before("ハシ", 2, 8, &hit, reading, sizeof(reading), &chars) == false);
	assert(jpdict_lookup_before("abc", 3, 8, &hit, reading, sizeof(reading), &chars) == false);
	// A caret that has walked into a half-written kana stops the walk.
	assert(jpdict_lookup_before("\xe3\x81", 1, 8, &hit, reading, sizeof(reading), &chars) == false);
	assert(jpdict_lookup_before("たべ", 3, 8, &hit, reading, sizeof(reading), &chars) == false);
	assert(jpdict_lookup_before("", 1, 8, &hit, reading, sizeof(reading), &chars) == false);

	// The reading buffer has to be as long as the run it may hold.
	assert(jpdict_lookup_before("はし", 2, 2, &hit, reading, 3 * 2 + 1, &chars) == true);
	assert(strcmp(reading, "はし") == 0);
	char small[3 * 2 + 1 - 1];
	assert(jpdict_lookup_before("はし", 2, 2, &hit, small, sizeof(small), &chars) == false);
	assert(jpdict_lookup_before("はし", 2, 0, &hit, reading, sizeof(reading), &chars) == false);
	assert(jpdict_lookup_before(NULL, 0, 8, &hit, reading, sizeof(reading), &chars) == false);
	assert(jpdict_lookup_before("はし", 2, 8, NULL, reading, sizeof(reading), &chars) == false);
	assert(jpdict_lookup_before("はし", 2, 8, &hit, NULL, sizeof(reading), &chars) == false);
	assert(jpdict_lookup_before("はし", 2, 8, &hit, reading, sizeof(reading), NULL) == false);
}

int main(void) {
	lookup_tests();
	lookup_before_tests();
	printf("jpdict-test: all checks passed\n");
	return 0;
}
