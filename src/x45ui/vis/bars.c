#define _GNU_SOURCE
/* bars - two segmented level bars (RMS on a dB scale, the held peak as a marker) with a dB scale under them.
 * Runs on the analyzer's levels, or on the panel meter of the external inputs. */
#include "vis.h"

static int cap(int w)
{
	return 108;
}

static void draw(struct ui *u, int x, int y, int w, int h)
{
	static const int marks[] = { -60, -40, -30, -20, -10, -6, -3, 0 };
	static const char *const labels[] = { "60", "40", "30", "20", "10", "6", "3", "0" };
	struct x45_surface *s = &u->g.back;
	const int seg = 8, gap = 2;
	int scale_h = h >= 96 ? 24 : 0;
	int chh = (h - scale_h - 8) / 2, ch, i, ts, lw, bw, bx;

	if (chh > 44)
		chh = 44;
	if (chh < 8)
		return;
	ts = chh >= 30 ? 3 : (chh >= 16 ? 2 : 1);
	lw = 6 * ts + 12;
	bx = x + lw;
	bw = w - lw;
	for (ch = 0; ch < 2; ch++) {
		float lvl, pk;
		int by = y + ch * (chh + 8), fill, pkx;

		vis_level(u, ch, &lvl, &pk);
		fill = vis_db_x(lvl, bw);
		pkx = vis_db_x(pk, bw);
		x45_text(s, x, by + (chh - x45_text_h(ts)) / 2, ch ? "R" : "L", ts, u->t.dim);
		x45_fill_rect(s, bx, by, bw, chh, u->t.barbg);
		for (i = 0; i + seg <= bw; i += seg + gap) {
			float db = METER_FLOOR + (float)i / (float)bw * -METER_FLOOR;

			if (i + seg > fill)
				continue;
			x45_fill_rect(s, bx + i, by + 3, seg, chh - 6,
				      db >= -3 ? u->t.warn : db >= -12 ? u->t.accent : u->t.barfg);
		}
		if (pk > METER_FLOOR)
			x45_fill_rect(s, bx + pkx - 2, by, 4, chh, u->t.text);
		x45_draw_rect(s, bx, by, bw, chh, u->t.line);
	}
	for (i = 0; i < (scale_h ? 8 : 0); i++) {
		int mx = bx + vis_db_x((float)marks[i], bw);
		int tw = x45_text_w(labels[i], 2);

		x45_vline(s, mx, y + 2 * chh + 8, 6, u->t.dim);
		x45_text(s, mx - tw / 2 < bx ? bx : mx - tw / 2, y + 2 * chh + 12, labels[i], 2, u->t.dim);
	}
}

const struct ui_vis vis_bars = {
	.name = "bars", .label = "Level bars", .bit = 1, .weight = 2, .cap = cap, .draw = draw,
};
