/* x45_ascii_fold - see gfx.h.
 *
 * The panel font covers ASCII 0x20-0x7E only, but tags are UTF-8 and a music
 * library is full of accents ("Cafe del Mar", "Bjork"). Rather than show
 * garbage, fold what has an obvious ASCII reading and show '?' for the rest, one
 * '?' per code point (so a row of CJK is a row of '?', not a run of mojibake).
 */
#include "gfx.h"

#include <stdint.h>
#include <string.h>

/* U+00C0..U+00FF */
static const char *const latin1[64] = {
	"A", "A", "A", "A", "A", "A", "AE", "C", "E", "E", "E", "E", "I", "I", "I", "I",
	"D", "N", "O", "O", "O", "O", "O", "x", "O", "U", "U", "U", "U", "Y", "Th", "ss",
	"a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i", "i",
	"d", "n", "o", "o", "o", "o", "o", "/", "o", "u", "u", "u", "u", "y", "th", "y",
};

/* U+0100..U+017F, one letter each (ligatures reduced to their first letter) */
static const char latin_ext_a[129] =
	"AaAaAaCcCcCcCcDd"
	"DdEeEeEeEeEeGgGg"
	"GgGgHhHhIiIiIiIi"
	"IiIiJjKkkLlLlLlL"
	"lLlNnNnNnnNnOoOo"
	"OoOoRrRrRrSsSsSs"
	"SsTtTtTtUuUuUuUu"
	"UuUuWwYyYZzZzZzs";

struct out {
	char *p;
	size_t cap, n;
};

static void emit(struct out *o, const char *s)
{
	for (; *s; s++) {
		if (o->n + 1 < o->cap)
			o->p[o->n] = *s;
		o->n++;
	}
}

static void emit_c(struct out *o, char c)
{
	char s[2] = { c, 0 };
	emit(o, s);
}

size_t x45_ascii_fold(char *dst, size_t cap, const char *src)
{
	struct out o = { dst, cap, 0 };
	const unsigned char *s = (const unsigned char *)src;

	if (!cap)
		return 0;

	while (*s) {
		uint32_t cp;
		int len;

		if (*s < 0x80) {
			/* control characters would draw as the blank glyph anyway */
			emit_c(&o, *s < 0x20 || *s == 0x7f ? ' ' : (char)*s);
			s++;
			continue;
		}
		/* decode one UTF-8 sequence; anything malformed is a single '?' */
		if ((*s & 0xE0) == 0xC0 && *s >= 0xC2) {
			len = 2; cp = *s & 0x1F;
		} else if ((*s & 0xF0) == 0xE0) {
			len = 3; cp = *s & 0x0F;
		} else if ((*s & 0xF8) == 0xF0 && *s <= 0xF4) {
			len = 4; cp = *s & 0x07;
		} else {
			emit_c(&o, '?');
			s++;
			continue;
		}
		{
			int i, ok = 1;
			for (i = 1; i < len; i++) {
				if ((s[i] & 0xC0) != 0x80) {
					ok = 0;
					break;
				}
				cp = (cp << 6) | (s[i] & 0x3F);
			}
			if (!ok) {
				emit_c(&o, '?');
				s++;       /* resync on the next byte */
				continue;
			}
			/* reject overlong 3/4-byte forms and surrogates */
			if ((len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000) ||
			    (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) {
				emit_c(&o, '?');
				s += len;
				continue;
			}
		}
		s += len;

		if (cp == 0xA0)
			emit_c(&o, ' ');
		else if (cp >= 0xC0 && cp <= 0xFF)
			emit(&o, latin1[cp - 0xC0]);
		else if (cp >= 0x100 && cp <= 0x17F)
			emit_c(&o, latin_ext_a[cp - 0x100]);
		else if (cp == 0x2018 || cp == 0x2019 || cp == 0x201A || cp == 0x2032)
			emit_c(&o, '\'');
		else if (cp == 0x201C || cp == 0x201D || cp == 0x201E || cp == 0x2033)
			emit_c(&o, '"');
		else if (cp == 0x2010 || cp == 0x2011 || cp == 0x2012 || cp == 0x2013 ||
			 cp == 0x2014 || cp == 0x2015 || cp == 0x2212)
			emit_c(&o, '-');
		else if (cp == 0x2026)
			emit(&o, "...");
		else if (cp == 0x2022 || cp == 0x00B7)
			emit_c(&o, '*');
		else
			emit_c(&o, '?');
	}
	dst[o.n < cap ? o.n : cap - 1] = '\0';
	return o.n < cap ? o.n : cap - 1;
}
