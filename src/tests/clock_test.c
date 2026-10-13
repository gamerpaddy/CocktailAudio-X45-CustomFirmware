/* Pure-logic test for x45_clock_family(): which master-clock family a rate
 * belongs to. Touches no hardware; builds as ARM and runs on the device like
 * quad_test. The hardware side (does the line really change the clock?) is
 * checked separately by measuring the USB feedback rate. */
#include "../libx45/x45.h"

#include <stdio.h>

static const struct { unsigned rate; int want; const char *why; } cases[] = {
	{ 8000, X45_CLOCK_48, "telephony" },
	{ 11025, X45_CLOCK_44, "" },
	{ 12000, X45_CLOCK_48, "" },
	{ 16000, X45_CLOCK_48, "" },
	{ 22050, X45_CLOCK_44, "" },
	{ 24000, X45_CLOCK_48, "" },
	{ 32000, X45_CLOCK_48, "broadcast" },
	{ 44100, X45_CLOCK_44, "CD" },
	{ 48000, X45_CLOCK_48, "" },
	{ 88200, X45_CLOCK_44, "" },
	{ 96000, X45_CLOCK_48, "" },
	{ 176400, X45_CLOCK_44, "" },
	{ 192000, X45_CLOCK_48, "" },
	{ 352800, X45_CLOCK_44, "DXD / 8x44.1" },
	{ 384000, X45_CLOCK_48, "8x48" },
	{ 705600, X45_CLOCK_44, "16x44.1" },
	{ 768000, X45_CLOCK_48, "16x48" },
	/* DSD expressed as a byte rate per channel, which is how MPD names it */
	{ 352800, X45_CLOCK_44, "DSD64" },
	{ 705600, X45_CLOCK_44, "DSD128" },
	{ 1411200, X45_CLOCK_44, "DSD256" },
	{ 384000, X45_CLOCK_48, "DSD64 on the 48k grid" },
	/* not audio rates: must be refused rather than guessed */
	{ 0, -1, "zero" },
	{ 12345, -1, "odd" },
	{ 100000, -1, "between families" },
	{ 44101, -1, "one off 44.1k" },
};

int main(void)
{
	size_t i, fails = 0, n = sizeof(cases) / sizeof(cases[0]);

	for (i = 0; i < n; i++) {
		int got = x45_clock_family(cases[i].rate);
		if (got != cases[i].want) {
			printf("FAIL %7u Hz: got %d, want %d  %s\n", cases[i].rate,
			       got, cases[i].want, cases[i].why);
			fails++;
		}
	}
	printf("clock_test: %zu/%zu pass\n", n - fails, n);
	return fails ? 1 : 0;
}
