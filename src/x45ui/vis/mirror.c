#define _GNU_SOURCE
/* mirror - the spectrum as bars that grow up and down from a centre line, like a reflection. The bars are
 * coloured by height (Options > Bar colours); the held peak is a short tick at either end. */
#include "vis.h"

#include <string.h>

static float peak[NSB];
static int peak_nb;
static long peak_t;

static void draw(struct ui *u, int x, int y, int w, int h)
{
	struct x45_surface *s = &u->g.back;
	float bb[NSB], dt;
	int nb = u->bands, i, bw, gap, mid = y + h / 2, half = h / 2 - 2;
	long now = vis_now();

	if (nb < 8 || nb > NSB)
		nb = NSB;
	bw = w / nb;
	if (bw < 2 || h < 24)
		return;
	gap = bw >= 12 ? 2 : bw >= 6 ? 1 : 0;
	vis_rebin(u->sp_b, bb, nb);
	dt = peak_t ? (float)(now - peak_t) / 1000.0f : 0;
	if (dt > 0.25f)
		dt = 0.25f;
	peak_t = now;
	if (peak_nb != nb) {
		memset(peak, 0, sizeof peak);
		peak_nb = nb;
	}
	x45_fill_rect(s, x, y, w, h, u->t.barbg);
	for (i = 0; i < nb; i++) {
		float f = vis_band_f(bb[i]);
		int bh = (int)(f * (float)half + 0.5f), ph, x0 = x + i * w / nb, cw = (i + 1) * w / nb - i * w / nb - gap;
		uint32_t col = vis_bar_colour(u, f);

		if (f >= peak[i]) {
			peak[i] = f;
		} else {
			peak[i] -= 0.35f * dt;
			if (peak[i] < f)
				peak[i] = f;
		}
		if (bh > 0) {
			x45_fill_rect(s, x0, mid - bh, cw, bh, col);
			x45_fill_rect(s, x0, mid + 1, cw, bh, vis_mix(u->t.barbg, col, 55));   /* the reflection is fainter */
		}
		ph = (int)(peak[i] * (float)half + 0.5f);
		if (ph > bh + 2) {
			x45_fill_rect(s, x0, mid - ph - 2, cw, 2, u->t.text);
			x45_fill_rect(s, x0, mid + ph + 1, cw, 2, u->t.dim);
		}
	}
	x45_hline(s, x, mid, w, u->t.line);
}

static void reset(void)
{
	memset(peak, 0, sizeof peak);
	peak_nb = 0;
	peak_t = 0;
}

const struct ui_vis vis_mirror = {
	.name = "mirror", .label = "Mirror spectrum", .bit = 128,   /* a style of the spectrum (spectrum.c); the bit only reads old settings */
	.draw = draw, .reset = reset,
};
