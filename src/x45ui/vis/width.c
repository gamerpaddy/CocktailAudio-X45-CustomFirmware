#define _GNU_SOURCE
/* width - the stereo image by frequency: the mid signal (what both channels share, (L + R) / 2) rising from the middle line, the
 * side signal (what makes them differ, (L - R) / 2) hanging below it, on the same dB scale and over the same frequencies as the
 * spectrum. Where the lower half is long, the sound is wide; where it is empty, mono. Above, the share of the side signal in three
 * ranges (side power against mid + side power): the bass should be nearly mono (a record cuts it that way, and a wide bass
 * loses its punch on a small speaker), the rest is a matter of taste.
 *
 * Needs the analyzer's "sb" (x45loop of 2026-10-10 on): with an older one the visual stays out of the way. */
#include "vis.h"

#include <math.h>
#include <stdio.h>

static const float edge[2] = { 120.0f, 2000.0f };

static void feed(struct ui *u)
{
	struct vis_width *m = &u->vm.width;
	long dt;
	double pm[3] = { 0, 0, 0 }, ps[3] = { 0, 0, 0 };
	float a;
	int i, r, hz1, hz2;

	VIS_MEM_STEP(u, m, dt);
	if (dt <= 0 || !u->sp_side)
		return;
	if ((u->sp_r[0] > u->sp_r[1] ? u->sp_r[0] : u->sp_r[1]) < -70.0f)
		return;   /* silence has no width */
	hz1 = vis_band_of(u, edge[0]);
	hz2 = vis_band_of(u, edge[1]);
	for (i = 0; i < NSB; i++) {
		r = i <= hz1 ? 0 : i <= hz2 ? 1 : 2;
		pm[r] += vis_pow(u->sp_b[i]);
		ps[r] += vis_pow(u->sp_sb[i]);
	}
	a = dt >= 600 ? 1.0f : (float)dt / 600.0f;
	for (r = 0; r < 3; r++) {
		float v = (float)(ps[r] / (pm[r] + ps[r] + 1e-20));

		if (!m->ok)
			m->reg[r] = v;
		else
			m->reg[r] += (v - m->reg[r]) * a;
	}
	m->ok = 1;
}

static void draw(struct ui *u, int x, int y, int w, int h)
{
	static const struct { int f; const char *l; } fm[] = {
		{ 50, "50" }, { 100, "100" }, { 200, "200" }, { 500, "500" }, { 1000, "1k" },
		{ 2000, "2k" }, { 5000, "5k" }, { 10000, "10k" }, { 20000, "20k" }
	};
	static const char *const rname[3] = { "BASS", "MIDS", "HIGHS" };
	struct x45_surface *s = &u->g.back;
	const struct vis_width *m = &u->vm.width;
	float mid[NSB], side[NSB];
	int nb = u->bands, i, bw, gap, head = h >= 100 ? 22 : 0, lab = h >= 90 ? 20 : 0, py = y + head, ph = h - head - lab, zero, half, mx;

	if (nb < 8 || nb > NSB)
		nb = NSB;
	bw = w / nb;
	if (bw < 2 || ph < 30)
		return;
	gap = bw >= 12 ? 2 : bw >= 6 ? 1 : 0;
	zero = py + ph / 2;
	half = ph / 2 - 1;
	vis_rebin(u->sp_b, mid, nb);
	vis_rebin(u->sp_sb, side, nb);
	x45_fill_rect(s, x, py, w, ph, u->t.barbg);
	for (i = 0; i < nb; i++) {
		float fm_ = vis_band_f(mid[i]), fs = vis_band_f(side[i]);
		int hm = (int)(fm_ * (float)half + 0.5f), hs = (int)(fs * (float)half + 0.5f), bx = x + i * w / nb, bwid = (i + 1) * w / nb - i * w / nb - gap;

		if (hm > 0)
			x45_fill_rect(s, bx, zero - hm, bwid, hm, vis_bar_colour(u, fm_));
		if (hs > 0)
			x45_fill_rect(s, bx, zero + 1, bwid, hs, vis_bar_colour(u, fs));
	}
	x45_hline(s, x, zero, w, u->t.dim);
	mx = x + (int)((float)w * logf(edge[0] / 20.0f) / logf(1000.0f));   /* the 120 Hz mark: the bass should be mono */
	for (i = py; i < py + ph; i += 8)
		x45_vline(s, mx, i, 4, u->t.text);
	if (ph >= 60) {
		x45_text(s, x + 6, py + 4, "MID", 2, u->t.dim);
		x45_text(s, x + 6, py + ph - 18, "SIDE", 2, u->t.dim);
	}
	if (lab)
		for (i = 0; i < (int)(sizeof fm / sizeof *fm); i++) {
			int fx = x + (int)((float)w * logf((float)fm[i].f / 20.0f) / logf(1000.0f)), tw = x45_text_w(fm[i].l, 2);

			x45_vline(s, fx, py + ph, 5, u->t.dim);
			fx -= tw / 2;
			if (fx < x)
				fx = x;
			if (fx + tw > x + w)
				fx = x + w - tw;
			x45_text(s, fx, py + ph + 6, fm[i].l, 2, u->t.dim);
		}
	if (head && m->ok) {
		int tx = x;

		x45_text(s, tx, y, "SIDE SHARE", 2, u->t.dim);
		tx += x45_text_w("SIDE SHARE", 2) + 16;
		for (i = 0; i < 3; i++) {
			char buf[40];
			float v = m->reg[i] * 100.0f;
			uint32_t c = u->t.text;

			if (i == 0)   /* the bass: nearly mono is right */
				c = v <= 10.0f ? x45_rgb(&u->g, 92, 204, 122) : v <= 25.0f ? x45_rgb(&u->g, 240, 190, 62) : x45_rgb(&u->g, 238, 86, 74);
			snprintf(buf, sizeof buf, "%s %.0f%%", rname[i], (double)v);
			if (tx + x45_text_w(buf, 2) > x + w)
				break;
			x45_text(s, tx, y, buf, 2, c);
			tx += x45_text_w(buf, 2) + 18;
		}
	}
}

const struct ui_vis vis_width = {
	.name = "width", .label = "Stereo width", .bit = 8192, .weight = 3, .bands = 1, .side = 1, .look = VIS_LOOK_COLOURS,
	.feed = feed, .draw = draw,
};
