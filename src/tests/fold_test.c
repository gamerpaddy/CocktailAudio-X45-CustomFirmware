/* x45_ascii_fold: UTF-8 -> what the 5x7 ASCII font can draw. Pure logic. */
#include "../libx45/gfx.h"

#include <stdio.h>
#include <string.h>

static int fails, checks;

#define EXPECT(in, want) do { \
	char out[128]; \
	size_t n = x45_ascii_fold(out, sizeof out, in); \
	checks++; \
	if (strcmp(out, want) || n != strlen(want)) { \
		fails++; \
		printf("FAIL line %d: got '%s' (%zu), want '%s'\n", __LINE__, out, n, want); \
	} \
} while (0)

int main(void)
{
	EXPECT("plain ascii stays", "plain ascii stays");
	EXPECT("", "");
	EXPECT("Caf\xc3\xa9 del Mar", "Cafe del Mar");
	EXPECT("Bj\xc3\xb6rk \xe2\x80\x93 J\xc3\xb3ga", "Bjork - Joga");
	EXPECT("\xc3\x86ther \xc3\x98l", "AEther Ol");
	EXPECT("Stra\xc3\x9f" "e", "Strasse");
	EXPECT("\xc5\x81\xc3\xb3" "d\xc5\xba", "Lodz");                 /* Lodz */
	EXPECT("\xe2\x80\x9cquoted\xe2\x80\x9d \xe2\x80\x99n\xe2\x80\x99", "\"quoted\" 'n'");
	EXPECT("wait\xe2\x80\xa6", "wait...");
	EXPECT("a\xc2\xa0" "b", "a b");                                 /* NBSP */
	EXPECT("a\tb\nc", "a b c");                                     /* controls */
	EXPECT("\xc3\x9c\xc3\xbc", "Uu");
	EXPECT("\xc5\x92uvre", "Ouvre");                                /* ligature reduced */

	/* no reading of the font beyond what it can draw: CJK and emoji -> '?' */
	EXPECT("\xe6\x97\xa5\xe6\x9c\xac", "??");
	EXPECT("x\xf0\x9f\x98\x80y", "x?y");                            /* one '?' per code point */

	/* malformed input: never crash, never swallow the following text */
	EXPECT("a\xff" "b", "a?b");
	EXPECT("a\xc3", "a?");                                          /* truncated sequence */
	EXPECT("a\xc3" "b", "a?b");                                     /* bad continuation */
	EXPECT("a\xc0\x80" "b", "a??b");                                /* overlong NUL */
	EXPECT("a\xe0\x80\x80" "b", "a?b");                             /* overlong 3-byte */
	/* overlong encodings of characters we DO map must still be refused, or
	 * "e" could be smuggled past a filter as three bytes */
	EXPECT("a\xe0\x81\x81" "b", "a?b");                             /* 'A' in 3 bytes */
	EXPECT("a\xe0\x83\xa9" "b", "a?b");                             /* e-acute in 3 bytes */
	EXPECT("a\xf0\x82\x80\x99" "b", "a?b");                         /* U+2019 in 4 bytes */
	EXPECT("a\xed\xa0\x80" "b", "a?b");                             /* UTF-16 surrogate */
	EXPECT("a\xf5\x80\x80\x80" "b", "a????b");                      /* beyond U+10FFFF */

	/* output bound: always NUL-terminated, never past cap, returns length written */
	{
		char small[6];
		size_t n = x45_ascii_fold(small, sizeof small, "abcdefghij");
		checks++;
		if (strcmp(small, "abcde") || n != 5) {
			fails++;
			printf("FAIL truncation: '%s' (%zu)\n", small, n);
		}
		n = x45_ascii_fold(small, 1, "abc");
		checks++;
		if (small[0] != '\0' || n != 0) {
			fails++;
			printf("FAIL cap 1: %zu\n", n);
		}
		checks++;
		if (x45_ascii_fold(small, 0, "abc") != 0) {
			fails++;
			printf("FAIL cap 0\n");
		}
		/* a 2-letter expansion that does not fit must not overrun */
		n = x45_ascii_fold(small, 3, "\xc3\x86\xc3\x86");           /* AE AE, cap 3 */
		checks++;
		if (strcmp(small, "AE") || n != 2) {
			fails++;
			printf("FAIL partial expansion: '%s' (%zu)\n", small, n);
		}
	}

	printf("fold_test: %d/%d pass\n", checks - fails, checks);
	return fails ? 1 : 0;
}
