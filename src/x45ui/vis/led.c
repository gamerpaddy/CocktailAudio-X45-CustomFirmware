#define _GNU_SOURCE
/* led - the spectrum as a graphic-equaliser display: columns of separate cells, lit up to the level, the held
 * peak a lone brighter cell that falls back slowly. Cells are coloured by height as chosen in Options > Bar
 * colours. */
#include "vis.h"

#include <string.h>

static float peak[NSB];
static int peak_nb;
static long peak_t;

static void draw(struct ui *u, int x, int y, int w, int h)
{
	struct x45_surface *s = &u->g.back;
	float bb[NSB], dt;
	int nb = u->bands, i, k, cell, rows, bw, gap;
	long now = vis_now();
	const uint32_t off = vis_mix(u->t.barbg, u->t.line, 45);

	if (nb < 8 || nb > NSB)
		nb = NSB;
	if (nb > 64)
		nb = 64;   /* cells need width: more than 64 columns would be slivers */
	bw = w / nb;
	if (bw < 3 || h < 24)
		return;
	gap = bw >= 10 ? 3 : 2;
	cell = h >= 120 ? 8 : 6;
	rows = h / cell;
	if (rows < 4)
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
	x45_fill_rect(s, x, y, w, h, u->t.barbg);
	for (i = 0; i < nb; i++) {
		float f = vis_band_f(bb[i]);
		int lit = (int)(f * (float)rows + 0.5f), pk, x0 = x + i * w / nb, cw = (i + 1) * w / nb - i * w / nb - gap;

		if (f >= peak[i]) {
			peak[i] = f;
		} else {
			peak[i] -= 0.30f * dt;
			if (peak[i] < f)
				peak[i] = f;
		}
		pk = (int)(peak[i] * (float)rows + 0.5f);
		for (k = 0; k < rows; k++) {
			int cy = y + h - (k + 1) * cell;
			float at = (float)(k + 1) / (float)rows;

			x45_fill_rect(s, x0, cy + 1, cw, cell - 2, k < lit ? vis_bar_colour(u, at) : off);
		}
		if (pk > lit && pk <= rows)
			x45_fill_rect(s, x0, y + h - pk * cell + 1, cw, cell - 2, u->t.text);
	}
}

static void reset(void)
{
	memset(peak, 0, sizeof peak);
	peak_nb = 0;
	peak_t = 0;
}

const struct ui_vis vis_led = {
	.name = "led", .label = "LED spectrum", .bit = 64,   /* a style of the spectrum (spectrum.c); the bit only reads old settings */
	.draw = draw, .reset = reset,
};
