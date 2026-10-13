/* Pure-logic test for x45_balance_levels(): the left/right volume indexes for a master volume and a balance.
 * Touches no hardware. The reference is the stock firmware's own arithmetic (read from its volume-apply function),
 * written out below as stock_levels(); the one deliberate difference is that a side is never left at 0 (mute)
 * by the balance alone - see the comment in libx45/x45.c. */
#include "../libx45/x45.h"

#include <stdio.h>

/* the stock x20 routine, verbatim in effect: b = -24..24, vol = 1..100 */
static void stock_levels(int vol, int b, int *left, int *right)
{
	int l = vol, r = vol;

	if (b > 0) {
		r = vol + (b + 1) / 2;
		l = vol - b / 2;
		if (r > 100) {
			l -= r - 100;
			r = 100;
		} else if (l < 0) {
			r += 1 - l;
			l = 1;
		}
	} else if (b < 0) {
		int t = b / 2;   /* truncates toward zero, as the stock code does */

		r = vol + t;
		l = vol + (b & 1) - t;
		if (l > 100) {
			r -= l - 100;
			l = 100;
		} else if (r < 0) {
			l += 1 - r;
			r = 1;
		}
	}
	*left = l;
	*right = r;
}

int main(void)
{
	int vol, b, fails = 0, same = 0, lifted = 0;

	for (vol = 0; vol <= 100; vol++) {
		for (b = -X45_BALANCE_MAX; b <= X45_BALANCE_MAX; b++) {
			int l, r, sl, sr;

			x45_balance_levels(vol, b, &l, &r);
			if (vol == 0) {   /* mute stays mute whatever the balance */
				if (l != 0 || r != 0) {
					printf("FAIL vol 0 bal %d -> %d/%d\n", b, l, r);
					fails++;
				}
				continue;
			}
			if (l < 1 || r < 1 || l > 100 || r > 100) {
				printf("FAIL range vol %d bal %d -> %d/%d\n", vol, b, l, r);
				fails++;
			}
			if (r - l != b) {   /* the whole point: the right side is `b` indexes above the left */
				printf("FAIL difference vol %d bal %d -> %d/%d\n", vol, b, l, r);
				fails++;
			}
			if (b == 0 && l != vol) {
				printf("FAIL centre vol %d -> %d/%d\n", vol, l, r);
				fails++;
			}
			stock_levels(vol, b, &sl, &sr);
			if (sl == l && sr == r) {
				same++;
			} else if (sl == 0 || sr == 0) {
				/* the stock firmware leaves the quieter side at 0 when the balance only just reaches it; this one
				 * keeps it at 1 and lets the louder side follow, so the difference still holds */
				int ql = sl, qr = sr;

				if (sl == 0) {
					ql = 1;
					qr = sr + 1;
				} else {
					qr = 1;
					ql = sl + 1;
				}
				if (ql != l || qr != r) {
					printf("FAIL lifted vol %d bal %d: stock %d/%d, here %d/%d\n", vol, b, sl, sr, l, r);
					fails++;
				} else {
					lifted++;
				}
			} else {
				printf("FAIL vs stock vol %d bal %d: stock %d/%d, here %d/%d\n", vol, b, sl, sr, l, r);
				fails++;
			}
		}
	}
	/* a few by hand, taken from the stock behaviour: +1 raises the right by one, +2 raises it and lowers the left */
	{
		static const struct { int vol, b, l, r; } k[] = {
			{ 50, 0, 50, 50 }, { 50, 1, 50, 51 }, { 50, 2, 49, 51 }, { 50, 3, 49, 52 }, { 50, -1, 51, 50 },
			{ 50, -2, 51, 49 }, { 50, -3, 52, 49 }, { 50, 24, 38, 62 }, { 50, -24, 62, 38 },
			{ 100, 4, 96, 100 }, { 100, -4, 100, 96 }, { 99, 5, 95, 100 }, { 2, 24, 1, 25 },
		};
		unsigned i;

		for (i = 0; i < sizeof k / sizeof *k; i++) {
			int l, r;

			x45_balance_levels(k[i].vol, k[i].b, &l, &r);
			if (l != k[i].l || r != k[i].r) {
				printf("FAIL case vol %d bal %d: want %d/%d got %d/%d\n", k[i].vol, k[i].b, k[i].l, k[i].r, l, r);
				fails++;
			}
		}
	}
	/* out-of-range balance is held to the range, NULL outputs are tolerated */
	{
		int l, r, l2, r2;

		x45_balance_levels(50, 99, &l, &r);
		x45_balance_levels(50, X45_BALANCE_MAX, &l2, &r2);
		if (l != l2 || r != r2) {
			printf("FAIL clamp\n");
			fails++;
		}
		x45_balance_levels(50, 3, NULL, NULL);
		x45_balance_levels(-5, 3, &l, &r);
		if (l != 0 || r != 0) {
			printf("FAIL negative volume\n");
			fails++;
		}
	}
	printf("balance_test: %d identical to the stock routine, %d with the quiet side lifted off 0, %d failures\n",
	       same, lifted, fails);
	return fails != 0;
}
