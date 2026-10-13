#define _GNU_SOURCE
/* area - the spectrum as one smooth curve with the area under it filled, instead of separate bars. The curve
 * is drawn between the centres of the bands, the colour follows its height (Options > Bar colours), and a thin
 * line holds the peaks. The number of bands is the owner's, as for the bars (Options > Visualizers). */
#include "vis.h"

#include <string.h>

static float peak[NSB];
static int peak_nb;
static long peak_t;

static void draw(struct ui *u, int x, int y, int w, int h)
{
	struct x45_surface *s = &u->g.back;
	float bb[NSB], f[NSB], dt;
	int nb = u->bands, i, col, prev_top = -1, prev_pk = -1;
	long now = vis_now();

	if (nb < 8 || nb > NSB)
		nb = NSB;
	if (w < 32 || h < 24)
		return;
	vis_rebin(u->sp_b, bb, nb);
	dt = peak_t ? (float)(now - peak_t) / 1000.0f : 0;
	if (dt > 0.25f)
		dt = 0.25f;
	peak_t = now;
	if (peak_nb != nb) {
		memset(peak, 0, sizeof peak);
		peak_nb = nb;
	}
	for (i = 0; i < nb; i++) {
		f[i] = vis_band_f(bb[i]);
		if (f[i] >= peak[i]) {
			peak[i] = f[i];
		} else {
			peak[i] -= 0.30f * dt;
			if (peak[i] < f[i])
				peak[i] = f[i];
		}
	}
	x45_fill_rect(s, x, y, w, h, u->t.barbg);
	for (col = 0; col < w; col++) {
		float pos = ((float)col + 0.5f) / (float)w * (float)nb - 0.5f, v, p;
		int i0 = (int)pos, i1, top, ptop, bh;
		float t;
		uint32_t c;

		if (pos < 0) {
			i0 = 0;
			pos = 0;
		}
		i1 = i0 + 1 < nb ? i0 + 1 : nb - 1;
		t = pos - (float)i0;
		t = t * t * (3.0f - 2.0f * t);   /* smoothstep: the curve has no corners at the band centres */
		v = f[i0] + (f[i1] - f[i0]) * t;
		p = peak[i0] + (peak[i1] - peak[i0]) * t;
		bh = (int)(v * (float)(h - 3) + 0.5f);
		top = y + h - bh;
		ptop = y + h - (int)(p * (float)(h - 3) + 0.5f);
		c = vis_bar_colour(u, v);
		if (bh > 0)
			x45_fill_rect(s, x + col, top, 1, bh, vis_mix(u->t.barbg, c, 40));
		/* the curve and the peak line: a column joins the point before it, so steep parts stay unbroken */
		if (prev_top >= 0 && bh > 0) {
			int a = prev_top < top ? prev_top : top, b = prev_top < top ? top : prev_top;

			x45_fill_rect(s, x + col, a, 1, b - a + 2, c);
		} else if (bh > 0) {
			x45_fill_rect(s, x + col, top, 1, 2, c);
		}
		if (prev_pk >= 0 && ptop < top - 3) {
			int a = prev_pk < ptop ? prev_pk : ptop, b = prev_pk < ptop ? ptop : prev_pk;

			x45_fill_rect(s, x + col, a, 1, b - a + 1, u->t.dim);
		}
		prev_top = bh > 0 ? top : -1;
		prev_pk = ptop;
	}
}

static void reset(void)
{
	memset(peak, 0, sizeof peak);
	peak_nb = 0;
	peak_t = 0;
}

const struct ui_vis vis_area = {
	.name = "area", .label = "Spectrum curve", .bit = 256,   /* a style of the spectrum (spectrum.c); the bit only reads old settings */
	.draw = draw, .reset = reset,
};
