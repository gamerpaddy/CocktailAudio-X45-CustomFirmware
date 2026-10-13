#define _GNU_SOURCE
/* scope - an oscilloscope: the last 25 ms of the left and right channel as two traces over a graticule.
 *
 * The analyzer cuts the picture at a rising zero crossing, so a steady tone stands still. Music is quiet next to
 * full scale (its peaks sit around -12 dB), so the gain follows the picture: slowly down, quickly up, so that
 * the trace uses the height of the box. The gain is written in the corner.
 */
#include "vis.h"

#include <stdio.h>

#define COLS 200

static int cap(int w)
{
	return 230;
}

static float gain = 1.0f;   /* the trace is drawn this many times larger than the signal */

static void draw(struct ui *u, int x, int y, int w, int h)
{
	struct x45_surface *s = &u->g.back;
	const uint32_t grid = vis_mix(u->t.barbg, u->t.line, 70), centre = u->t.line;
	int ch, i, mid = y + h / 2, amp = h / 2 - 3, peak = 100;
	char lab[16];

	if (w < 64 || h < 32)
		return;
	x45_fill_rect(s, x, y, w, h, u->t.barbg);
	for (i = 1; i < 8; i++)
		x45_vline(s, x + i * w / 8, y, h, grid);
	for (i = 1; i < 4; i++)
		if (i != 2)
			x45_hline(s, x, y + i * h / 4, w, grid);
	x45_hline(s, x, mid, w, centre);
	x45_draw_rect(s, x, y, w, h, u->t.line);

	for (i = 0; i < COLS; i++) {
		int a = u->sp_wl[i] < 0 ? -u->sp_wl[i] : u->sp_wl[i], b = u->sp_wr[i] < 0 ? -u->sp_wr[i] : u->sp_wr[i];

		if (a > peak)
			peak = a;
		if (b > peak)
			peak = b;
	}
	{   /* aim for the loudest point at 85 % of the half height; at most 40 times, at least 1 */
		float want = 8500.0f / (float)peak;

		if (want > 40.0f)
			want = 40.0f;
		if (want < 1.0f)
			want = 1.0f;
		gain += (want - gain) * (want < gain ? 0.5f : 0.08f);
	}
	for (ch = 1; ch >= 0; ch--) {   /* the left channel on top */
		const short *wv = ch ? u->sp_wr : u->sp_wl;
		uint32_t col = ch ? u->t.warn : u->t.accent;
		int px = x, py = mid;

		for (i = 0; i < COLS; i++) {
			int cx = x + 1 + i * (w - 3) / (COLS - 1);
			int cy = mid - (int)((float)wv[i] * 0.0001f * gain * (float)amp);

			if (cy < y + 1)
				cy = y + 1;
			if (cy > y + h - 2)
				cy = y + h - 2;
			if (i)
				vis_line(s, px, py, cx, cy, 2, col);
			px = cx;
			py = cy;
		}
	}
	snprintf(lab, sizeof lab, "x%.1f", (double)gain);
	x45_text(s, x + 8, y + 6, "L", 2, u->t.accent);
	x45_text(s, x + 8 + x45_text_w("L ", 2), y + 6, "R", 2, u->t.warn);
	x45_text(s, x + w - 8 - x45_text_w(lab, 2), y + 6, lab, 2, u->t.dim);
}

static void reset(void)
{
	gain = 1.0f;
}

const struct ui_vis vis_scope = {
	.name = "scope", .label = "Oscilloscope", .bit = 16, .weight = 4, .cap = cap, .wave = 1, .draw = draw, .reset = reset,
};
