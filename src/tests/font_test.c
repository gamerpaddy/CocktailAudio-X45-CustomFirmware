/* Verifies the 5x7 font table.
 *
 * Two layers of checking:
 *   1. x45_font_selfcheck() catches the mechanical authoring mistakes --
 *      blank glyphs, accidental duplicates, bit 7 set.
 *   2. rendering to stdout as ASCII art makes the shapes themselves
 *      checkable without looking at the panel.
 *
 * Runs anywhere; touches no hardware.
 *   font_test            selfcheck + a sample line + the full charset
 *   font_test "TEXT"     render just that string
 */
#include "../libx45/gfx.h"

#include <stdio.h>
#include <string.h>

/* Renders a string as rows of '#' and '.' */
static void art(const char *s)
{
	int row, i;
	size_t n = strlen(s);

	for (row = 0; row < 7; row++) {
		for (i = 0; i < (int)n; i++) {
			const unsigned char *g = x45_glyph((unsigned char)s[i]);
			int col;
			for (col = 0; col < 5; col++)
				putchar((g[col] & (1 << row)) ? '#' : '.');
			putchar(' ');
		}
		putchar('\n');
	}
}

int main(int argc, char **argv)
{
	int problems, c, first, last, shown;

	if (argc > 1) {
		art(argv[1]);
		return 0;
	}

	printf("=== mechanical self-check ===\n");
	problems = x45_font_selfcheck();
	printf("  %d problem%s found\n", problems, problems == 1 ? "" : "s");

	first = x45_font_first();
	last = x45_font_last();
	printf("  coverage: 0x%02X..0x%02X (%d glyphs)\n", first, last,
	       last - first + 1);

	printf("\n=== shape check: readable text ===\n");
	art("Hamburgefonstiv");
	printf("\n");
	art("0123456789 +-.,:");
	printf("\n");
	art("Volume 42 dB");

	printf("\n=== every glyph, 16 per block ===\n");
	shown = 0;
	for (c = first; c <= last; c += 16) {
		char buf[17];
		int k, m = 0;
		for (k = 0; k < 16 && c + k <= last; k++)
			buf[m++] = (char)(c + k);
		buf[m] = 0;
		printf("-- 0x%02X..0x%02X\n", c, c + m - 1);
		art(buf);
		shown += m;
	}
	printf("\n%d glyphs rendered\n", shown);
	return problems ? 1 : 0;
}
