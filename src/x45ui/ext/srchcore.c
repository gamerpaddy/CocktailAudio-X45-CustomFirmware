#define _GNU_SOURCE
#include "srchcore.h"

#include "../../libx45/gfx.h"

#include <stdio.h>
#include <string.h>

void srch_norm(char *out, size_t cap, const char *utf8)
{
	char tmp[320];
	size_t n = 0, i;
	int sp = 1;

	if (cap == 0)
		return;
	x45_ascii_fold(tmp, sizeof tmp, utf8);
	for (i = 0; tmp[i] && n + 1 < cap; i++) {
		unsigned char c = (unsigned char)tmp[i];

		if (c >= 'A' && c <= 'Z')
			c = (unsigned char)(c + 32);
		if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
			out[n++] = (char)c;
			sp = 0;
		} else if (!sp) {
			out[n++] = ' ';
			sp = 1;
		}
	}
	while (n > 0 && out[n - 1] == ' ')
		n--;
	out[n] = '\0';
}

/* is `word` in `name` where a word begins (boundary) or anywhere (!boundary) */
static int has_word(const char *name, const char *word, int boundary)
{
	const char *p = name;

	while ((p = strstr(p, word)) != NULL) {
		if (!boundary || p == name || p[-1] == ' ')
			return 1;
		p++;
	}
	return 0;
}

int srch_rank(const char *name, const char *query)
{
	const char *w = query;
	size_t qn = strlen(query);
	int all_boundary = 1;

	if (qn == 0)
		return 1;
	while (*w) {
		char word[SRCH_MAXQ + 1];
		size_t len = strcspn(w, " ");

		if (len > 0) {
			if (len > SRCH_MAXQ)
				len = SRCH_MAXQ;
			memcpy(word, w, len);
			word[len] = '\0';
			if (!has_word(name, word, 0))
				return 0;
			if (!has_word(name, word, 1))
				all_boundary = 0;
		}
		w += strcspn(w, " ");
		w += strspn(w, " ");
	}
	if (strncmp(name, query, qn) == 0)
		return 1;
	return all_boundary ? 2 : 3;
}

void srch_next(const char *name, const char *typed, int cnt[128])
{
	char q[SRCH_MAXQ + 2];
	const char *lw;
	size_t lwn;
	const char *p;

	snprintf(q, sizeof q, "%s", typed);
	/* the name has to match everything typed so far (trailing space or not) */
	{
		char t[SRCH_MAXQ + 2];
		size_t n;

		snprintf(t, sizeof t, "%s", q);
		n = strlen(t);
		while (n > 0 && t[n - 1] == ' ')
			t[--n] = '\0';
		if (!srch_rank(name, t))
			return;
	}
	lw = strrchr(q, ' ');
	lw = lw ? lw + 1 : q;
	lwn = strlen(lw);
	if (lwn == 0) {                          /* a word is about to start: its first letter */
		for (p = name; *p; p++)
			if ((p == name || p[-1] == ' ') && *p != ' ')
				cnt[(unsigned char)*p & 127]++;
		return;
	}
	for (p = name; (p = strstr(p, lw)) != NULL; p++) {
		char c = p[lwn];

		if (c)
			cnt[(unsigned char)c & 127]++;
	}
}

const char *srch_tap_chars(int d)
{
	static const char *const t[10] = { " 0", "qz1", "abc2", "def3", "ghi4", "jkl5", "mno6", "prs7", "tuv8", "wxy9" };

	return d >= 0 && d <= 9 ? t[d] : "";
}
