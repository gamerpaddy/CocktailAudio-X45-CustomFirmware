/* srchcore: the matching of the station search. Pure logic. */
#include "../x45ui/ext/srchcore.h"

#include <stdio.h>
#include <string.h>

static int fails, checks;

#define CHECK(cond) do { checks++; if (!(cond)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

static void norm_is(const char *in, const char *want)
{
	char out[SRCH_NAME];

	srch_norm(out, sizeof out, in);
	checks++;
	if (strcmp(out, want)) {
		fails++;
		printf("FAIL norm '%s': got '%s', want '%s'\n", in, out, want);
	}
}

static int rank_of(const char *name, const char *typed)
{
	char n[SRCH_NAME], q[SRCH_MAXQ + 2];

	srch_norm(n, sizeof n, name);
	srch_norm(q, sizeof q, typed);
	return srch_rank(n, q);
}

int main(void)
{
	int cnt[128];
	char n[SRCH_NAME];

	norm_is("Sunshine live", "sunshine live");
	norm_is("  SUNSHINE   LIVE - 90s  ", "sunshine live 90s");
	norm_is("Caf\xc3\xa9 del Mar", "cafe del mar");
	norm_is("1LIVE diggi", "1live diggi");
	norm_is("***", "");
	norm_is("", "");

	CHECK(rank_of("Sunshine Live", "suns") == 1);        /* begins with it */
	CHECK(rank_of("Radio Sunshine", "suns") == 2);       /* a word begins with it */
	CHECK(rank_of("Radio Moonsun", "suns") == 0);
	CHECK(rank_of("Radio Moonsunshine", "suns") == 3);      /* inside a word */
	CHECK(rank_of("sunshine live 90s", "live sunshine") == 2);   /* words in any order */
	CHECK(rank_of("sunshine live 90s", "sunshine 90") == 2);
	CHECK(rank_of("sunshine live", "sunshine rock") == 0);
	CHECK(rank_of("Anything", "") == 1);
	CHECK(rank_of("Bj\xc3\xb6rk FM", "bjork") == 1);
	CHECK(rank_of("sunshine live", "sunshine live") == 1);
	CHECK(rank_of("sunshine", "sunshine live") == 0);

	/* what can come next: the letter after each occurrence of the last word, a space where a word ends */
	srch_norm(n, sizeof n, "Sunshine Live");
	memset(cnt, 0, sizeof cnt);
	srch_next(n, "suns", cnt);
	CHECK(cnt['h'] == 1);
	CHECK(cnt['u'] == 0);
	memset(cnt, 0, sizeof cnt);
	srch_next(n, "sunshine", cnt);
	CHECK(cnt[' '] == 1);
	memset(cnt, 0, sizeof cnt);
	srch_next(n, "sunshine ", cnt);     /* a word is about to start: its first letter */
	CHECK(cnt['l'] == 1);
	memset(cnt, 0, sizeof cnt);
	srch_next(n, "", cnt);              /* nothing typed: the first letter of every word */
	CHECK(cnt['s'] == 1 && cnt['l'] == 1 && cnt['u'] == 0);
	memset(cnt, 0, sizeof cnt);
	srch_next(n, "sunshine live", cnt); /* the name ends here: nothing can follow */
	{
		int i, sum = 0;

		for (i = 0; i < 128; i++)
			sum += cnt[i];
		CHECK(sum == 0);
	}
	memset(cnt, 0, sizeof cnt);
	srch_next(n, "moon", cnt);          /* not a match: nothing counted */
	CHECK(cnt['s'] == 0);
	memset(cnt, 0, sizeof cnt);
	srch_next(n, "e l", cnt);           /* a name with the last word later on: "live" after "sunshine" ends in e */
	CHECK(cnt['i'] == 1);

	CHECK(!strcmp(srch_tap_chars(2), "abc2"));
	CHECK(!strcmp(srch_tap_chars(7), "prs7"));
	CHECK(!strcmp(srch_tap_chars(0), " 0"));
	CHECK(!strcmp(srch_tap_chars(1), "qz1"));
	CHECK(!strcmp(srch_tap_chars(10), ""));

	printf("srch_test: %d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;
}
