#define _GNU_SOURCE
/* waterfall - the spectrum over time: the newest row on top, scrolling down, two pixels a row. A row is kept
 * for every analyzer frame (feed), shown or not, so the picture is there the moment it is switched on. */
#include "vis.h"

#include <math.h>

/* one row of history per analyzer frame */
static void feed(struct ui *u)
{
	unsigned char *row = u->wf[u->wf_head];
	int i;

	for (i = 0; i < NSB; i++) {
		float f = (u->sp_b[i] - SPEC_FLOOR) / -SPEC_FLOOR;

		if (f < 0)
			f = 0;
		if (f > 1)
			f = 1;
		row[i] = (unsigned char)(powf(f, 1.4f) * 255.0f + 0.5f);
	}
	u->wf_head = (u->wf_head + 1) % WF_ROWS;
	if (u->wf_n < WF_ROWS)
		u->wf_n++;
}

/* history level 0..255 -> dark, blue, teal, yellow, soft red */
static uint32_t color(struct ui *u, int v)
{
	static uint32_t tab[64];
	static int ready;

	if (!ready) {
		int i;

		for (i = 0; i < 64; i++) {
			float t = (float)i / 63.0f, k, r, g, b;

			if (t < 0.25f) {
				k = t / 0.25f;
				r = 10 + 10 * k; g = 12 + 40 * k; b = 30 + 110 * k;
			} else if (t < 0.5f) {
				k = (t - 0.25f) / 0.25f;
				r = 20 + 40 * k; g = 52 + 130 * k; b = 140 - 20 * k;
			} else if (t < 0.75f) {
				k = (t - 0.5f) / 0.25f;
				r = 60 + 170 * k; g = 182 + 30 * k; b = 120 - 50 * k;
			} else {
				k = (t - 0.75f) / 0.25f;
				r = 230 + 6 * k; g = 212 - 100 * k; b = 70 + 10 * k;
			}
			tab[i] = x45_rgb(&u->g, (int)r, (int)g, (int)b);
		}
		ready = 1;
	}
	return tab[v >> 2];
}

static void draw(struct ui *u, int x, int y, int w, int h)
{
	struct x45_surface *s = &u->g.back;
	int nb = u->bands, rows, r, c, bw, total;

	if (nb < 8 || nb > NSB)
		nb = NSB;
	bw = w / nb;
	if (bw < 2 || h < 16)
		return;
	total = w;
	rows = h / 2;
	if (rows > WF_ROWS)
		rows = WF_ROWS;
	x45_fill_rect(s, x, y, total, rows * 2, u->t.barbg);
	for (r = 0; r < rows && r < u->wf_n; r++) {
		const unsigned char *row = u->wf[(u->wf_head - 1 - r + 2 * WF_ROWS) % WF_ROWS];
		int run = 0, runcol = -1;

		for (c = 0; c <= nb; c++) {
			int col = -1;

			if (c < nb) {
				int a = c * NSB / nb, z = (c + 1) * NSB / nb, k, sum = 0;

				if (z <= a)
					z = a + 1;
				for (k = a; k < z; k++)
					sum += row[k];
				col = (sum / (z - a)) >> 2;
				if (col < 2)
					col = -1;   /* silence: the background shows */
			}
			if (col != runcol) {
				if (runcol >= 0)
					x45_fill_rect(s, x + (c - run) * w / nb, y + r * 2, c * w / nb - (c - run) * w / nb, 2, color(u, runcol << 2));
				runcol = col;
				run = 0;
			}
			run++;
		}
	}
}

const struct ui_vis vis_waterfall = {
	.name = "waterfall", .label = "Waterfall", .bit = 8, .weight = 4, .bands = 1, .feed = feed, .draw = draw,
};
