#define _GNU_SOURCE
/* gonio - a vectorscope with the two things it is read for: how alike the channels are, and where the sound sits.
 *
 * Every point of the last 25 ms is drawn at (-left, right), which is the classic mid/side picture turned 45 degrees
 * counter-clockwise: the left channel runs along the horizontal axis (to the left), the right along the vertical
 * (up), mono is the diagonal to the upper left, a wide stereo signal a cloud around it, out-of-phase signals the
 * diagonal to the upper right. The scope is as large as the box it is given. Beside it (when the box is wide
 * enough, and the owner has not switched them off) the correlation of the channels (+1 the same, 0 independent,
 * -1 opposite: it should not stay at the left) and the balance of their levels.
 *
 * Two looks (Options > Visualizers): the Marantz scope, a round green-phosphor tube with a graticule whose trace
 * leaves an afterglow, as the receivers of the seventies had it; and the plain one, a flat square with the trace
 * in the accent colour. The colour of the trace is a choice of its own (gon_col, ui.h).
 */
#include "vis.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define COLS 200
#define PHMAX 272       /* cells of the phosphor in a row: a screen of up to 544 px at two px a cell */

static const char *const look_names[] = { "Marantz scope", "plain" };
static const char *const col_names[] = {
	"look's own", "green phosphor", "amber", "ice blue", "white", "red", "violet", "accent colour", "by distance",
};

int vis_gonio_looks(void)
{
	return (int)(sizeof look_names / sizeof *look_names);
}

const char *vis_gonio_look_name(int i)
{
	return look_names[i < 0 || i >= vis_gonio_looks() ? 0 : i];
}

int vis_gonio_colours(void)
{
	return (int)(sizeof col_names / sizeof *col_names);
}

const char *vis_gonio_colour_name(int i)
{
	return col_names[i < 0 || i >= vis_gonio_colours() ? 0 : i];
}

/* the colour of the trace; f is how far from the middle (0..1), for the choice that follows it */
static uint32_t ink(struct ui *u, float f)
{
	switch (u->gon_col) {
	case 1: return x45_rgb(&u->g, 70, 255, 120);
	case 2: return x45_rgb(&u->g, 255, 178, 40);
	case 3: return x45_rgb(&u->g, 120, 205, 255);
	case 4: return x45_rgb(&u->g, 236, 242, 236);
	case 5: return x45_rgb(&u->g, 255, 74, 60);
	case 6: return x45_rgb(&u->g, 196, 112, 255);
	case 7: return u->t.accent;
	case VIS_GON_BY_DISTANCE: return vis_bar_colour(u, f);
	default: return u->gon_look == 0 ? x45_rgb(&u->g, 70, 255, 120) : u->t.accent;
	}
}

static int cap(int w)
{
	return 480;   /* the largest square the panel has room for beside the readings */
}

static float gain = 1.0f, corr, bal;

/* a horizontal scale from -1 to 1 with a marker; the left half is the warning side when `warn_left` */
static void meter(struct ui *u, int x, int y, int w, const char *name, const char *lo, const char *mid,
		  const char *hi, float v, int warn_left, uint32_t fill)
{
	struct x45_surface *s = &u->g.back;
	int bh = 14, mx, half = w / 2;

	if (v < -1)
		v = -1;
	if (v > 1)
		v = 1;
	x45_text(s, x, y, name, 2, u->t.dim);
	y += 22;
	x45_fill_rect(s, x, y, w, bh, u->t.barbg);
	if (warn_left)
		x45_fill_rect(s, x, y, half, bh, vis_mix(u->t.barbg, u->t.warn, 28));
	x45_draw_rect(s, x, y, w, bh, u->t.line);
	mx = x + half + (int)(v * (float)(half - 3));
	if (v >= 0)
		x45_fill_rect(s, x + half, y + 3, mx - (x + half), bh - 6, v < 0.2f && warn_left ? u->t.dim : fill);
	else
		x45_fill_rect(s, mx, y + 3, x + half - mx, bh - 6, warn_left ? u->t.warn : fill);
	x45_fill_rect(s, mx - 2, y - 3, 4, bh + 6, u->t.text);
	x45_vline(s, x + half, y - 4, bh + 8, u->t.dim);
	x45_text(s, x, y + bh + 6, lo, 2, u->t.dim);
	x45_text(s, x + half - x45_text_w(mid, 2) / 2, y + bh + 6, mid, 2, u->t.dim);
	x45_text(s, x + w - x45_text_w(hi, 2), y + bh + 6, hi, 2, u->t.dim);
}

/* ---- the phosphor: how brightly each cell of the tube glows, fading between frames ---- */

static unsigned char ph[PHMAX * PHMAX];
static int ph_n;
static long ph_t;

static void ph_add(int n, int px, int py, int v)
{
	int i, s;

	if (px < 0 || py < 0 || px >= n || py >= n)
		return;
	i = py * n + px;
	s = ph[i] + v;
	ph[i] = (unsigned char)(s > 255 ? 255 : s);
}

/* a line of the beam into the phosphor, a little glow on the cells beside it */
static void ph_line(int n, int x0, int y0, int x1, int y1, int v)
{
	int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
	int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
	int err = dx + dy, e2;

	for (;;) {
		ph_add(n, x0, y0, v);
		ph_add(n, x0 + 1, y0, v / 4);
		ph_add(n, x0 - 1, y0, v / 4);
		ph_add(n, x0, y0 + 1, v / 4);
		ph_add(n, x0, y0 - 1, v / 4);
		if (x0 == x1 && y0 == y1)
			break;
		e2 = 2 * err;
		if (e2 >= dy) {
			err += dy;
			x0 += sx;
		}
		if (e2 <= dx) {
			err += dx;
			y0 += sy;
		}
	}
}

static void disc(struct x45_surface *s, int cx, int cy, int rad, uint32_t c)
{
	int dy;

	for (dy = -rad; dy <= rad; dy++) {
		int half = (int)sqrtf((float)(rad * rad - dy * dy));

		x45_hline(s, cx - half, cy + dy, 2 * half + 1, c);
	}
}

static void ring(struct x45_surface *s, int cx, int cy, int rad, uint32_t c)
{
	int i, steps = (int)(6.4f * (float)rad);

	for (i = 0; i < steps; i++) {
		float a = 6.2832f * (float)i / (float)steps;

		x45_fill_rect(s, cx + (int)lroundf((float)rad * cosf(a)), cy + (int)lroundf((float)rad * sinf(a)), 1, 1, c);
	}
}

/* the Marantz tube: bezel, glass, graticule, and the trace with its afterglow */
static void draw_tube(struct ui *u, int x, int cx, int cy, int side)
{
	struct x45_surface *s = &u->g.back;
	const int R = side / 2 - 3, rs = R - 6, cs = side > 260 ? 2 : 1, half = rs / cs, n = 2 * half;
	const uint32_t white = x45_rgb(&u->g, 255, 255, 255), black = x45_rgb(&u->g, 0, 0, 0);
	const uint32_t base = ink(u, 0.5f);
	const uint32_t glass = vis_mix(black, base, 7), grat = vis_mix(glass, base, 22), grat2 = vis_mix(glass, base, 11);
	long now = vis_now(), dt = now - ph_t;
	int f256, i, xi, yi, ox = cx - half * cs, oy = cy - half * cs, px = 0, py = 0, tick, v0;

	if (!vis_big(u))   /* the large one stands on the page itself */
		x45_fill_rect(s, x, cy - side / 2, side, side, u->t.barbg);
	disc(s, cx, cy, R, vis_mix(u->t.barbg, u->t.line, 70));          /* the bezel */
	disc(s, cx, cy, R - 3, vis_mix(black, u->t.line, 25));
	disc(s, cx, cy, rs, glass);                                       /* the glass */
	ring(s, cx, cy, rs + 1, vis_mix(black, u->t.dim, 40));

	/* the graticule: two rings, the cross with its ticks, the diagonals of mono and anti-phase */
	ring(s, cx, cy, rs / 3, grat2);
	ring(s, cx, cy, rs * 2 / 3, grat2);
	vis_line(s, cx - rs * 7 / 10, cy + rs * 7 / 10, cx + rs * 7 / 10, cy - rs * 7 / 10, 1, grat2);
	vis_line(s, cx - rs * 7 / 10, cy - rs * 7 / 10, cx + rs * 7 / 10, cy + rs * 7 / 10, 1, grat2);
	x45_hline(s, cx - rs, cy, 2 * rs + 1, grat);
	x45_vline(s, cx, cy - rs, 2 * rs + 1, grat);
	for (tick = 1; tick < 10; tick++) {
		int d = rs * tick / 10, len = tick % 5 == 0 ? 7 : 3;

		x45_vline(s, cx - d, cy - len, 2 * len + 1, grat);
		x45_vline(s, cx + d, cy - len, 2 * len + 1, grat);
		x45_hline(s, cx - len, cy - d, 2 * len + 1, grat);
		x45_hline(s, cx - len, cy + d, 2 * len + 1, grat);
	}
	x45_text(s, cx - rs + 10, cy - 20, "L", 2, grat);       /* above the horizontal line */
	x45_text(s, cx + 9, cy - rs + 12, "R", 2, grat);         /* beside the vertical line */
	if (side >= 230) {   /* on the diagonals, well inside the glass, with the line cut out behind the word */
		const int mw = x45_text_w("MONO", 2), aw = x45_text_w("ANTI", 2);
		int d = rs * 60 / 100;

		while (d > rs / 3 && (d + mw / 2 + 3) * (d + mw / 2 + 3) + (d + 9) * (d + 9) > (rs - 8) * (rs - 8))
			d--;   /* a small tube: as far out as the whole word still lies on the glass */
		x45_fill_rect(s, cx - d - mw / 2 - 3, cy - d - 9, mw + 6, 18, glass);
		x45_text(s, cx - d - mw / 2, cy - d - 7, "MONO", 2, grat);
		x45_fill_rect(s, cx + d - aw / 2 - 3, cy - d - 9, aw + 6, 18, glass);
		x45_text(s, cx + d - aw / 2, cy - d - 7, "ANTI", 2, grat);
	}

	/* the phosphor: what glowed a moment ago fades, then this frame's trace is written into it */
	if (n != ph_n || ph_t == 0 || dt > 600 || dt < 0) {
		memset(ph, 0, sizeof ph);
		ph_n = n;
		f256 = 0;
	} else {
		f256 = (int)(256.0f * expf(-(float)dt / 110.0f));
	}
	if (f256 < 256)
		for (i = 0; i < n * n; i++)
			ph[i] = (unsigned char)((ph[i] * f256) >> 8);
	ph_t = now;
	v0 = f256 == 0 ? 170 : 56;   /* a first frame (the menu's picture) has no afterglow behind it: write it brighter */
	for (i = 0; i < COLS; i++) {
		float l = (float)u->sp_wl[i] * 0.0001f, rr = (float)u->sp_wr[i] * 0.0001f;
		float dx = -l * gain, dy = rr * gain, len = sqrtf(dx * dx + dy * dy);
		int qx, qy;

		if (len > 1.0f) {   /* the beam stays on the glass */
			dx /= len;
			dy /= len;
		}
		qx = half + (int)(dx * (float)(half - 1));
		qy = half - (int)(dy * (float)(half - 1));
		if (i)
			ph_line(n, px, py, qx, qy, v0);
		px = qx;
		py = qy;
	}
	for (yi = 0; yi < n; yi++)
		for (xi = 0; xi < n; xi++) {
			int v = ph[yi * n + xi];
			uint32_t c;

			if (v < 7)
				continue;
			if (u->gon_col == VIS_GON_BY_DISTANCE) {   /* the colour follows the distance from the middle */
				float ddx = (float)(xi - half), ddy = (float)(yi - half);

				c = ink(u, sqrtf(ddx * ddx + ddy * ddy) / (float)half);
			} else {
				c = base;
			}
			c = vis_mix(glass, c, v * 100 / 255);
			if (v > 190)   /* where it is hottest the glow turns white */
				c = vis_mix(c, white, (v - 190) * 55 / 65);
			x45_fill_rect(s, ox + xi * cs, oy + yi * cs, cs, cs, c);
		}
	x45_fill_rect(s, ox + px * cs - cs, oy + py * cs - cs, 3 * cs, 3 * cs, vis_mix(base, white, 70));   /* the spot */
}

/* the plain scope: a flat square, the trace as dots and lines in the chosen colour */
static void draw_flat(struct ui *u, int x, int cx, int cy, int r, int side)
{
	struct x45_surface *s = &u->g.back;
	const uint32_t faint = vis_mix(u->t.barbg, u->t.line, 70);
	const int dot = side > 300 ? 2 : 1;
	int i, px = 0, py = 0;

	x45_fill_rect(s, x, cy - side / 2, side, side, u->t.barbg);
	x45_draw_rect(s, x, cy - side / 2, side, side, u->t.line);
	x45_vline(s, cx, cy - side / 2 + 1, side - 2, faint);
	x45_hline(s, x + 1, cy, side - 2, faint);
	vis_line(s, cx - r * 7 / 10, cy + r * 7 / 10, cx + r * 7 / 10, cy - r * 7 / 10, 1, faint);
	vis_line(s, cx - r * 7 / 10, cy - r * 7 / 10, cx + r * 7 / 10, cy + r * 7 / 10, 1, faint);
	x45_text(s, x + 6, cy - 20, "L", 2, u->t.dim);
	x45_text(s, cx + 8, cy - side / 2 + 5, "R", 2, u->t.dim);
	if (side >= 230) {   /* the diagonals: the picture of a mono signal, and of one that cancels */
		x45_text(s, x + 6, cy - side / 2 + 5, "MONO", 2, u->t.dim);
		x45_text(s, x + side - 6 - x45_text_w("ANTI", 2), cy - side / 2 + 5, "ANTI", 2, u->t.dim);
	}
	for (i = 0; i < COLS; i++) {
		float l = (float)u->sp_wl[i] * 0.0001f, rr = (float)u->sp_wr[i] * 0.0001f;
		int dx = (int)(-l * gain * (float)r), dy = (int)(rr * gain * (float)r);
		int qx, qy;
		uint32_t c;

		if (dx > r) dx = r;
		if (dx < -r) dx = -r;
		if (dy > r) dy = r;
		if (dy < -r) dy = -r;
		qx = cx + dx;
		qy = cy - dy;
		c = ink(u, sqrtf((float)(dx * dx + dy * dy)) / (float)r);
		if (i)
			vis_line(s, px, py, qx, qy, side > 300 ? 2 : 1, vis_mix(u->t.barbg, c, 45));
		x45_fill_rect(s, qx - dot, qy - dot, 2 * dot + 1, 2 * dot + 1, c);
		px = qx;
		py = qy;
	}
}

static void draw(struct ui *u, int x, int y, int w, int h)
{
	int side = h < w ? h : w, cx, cy, r, i, peak = 100, bars;

	if (side < 48)
		return;
	if (side > (vis_big(u) ? 544 : 440))
		side = vis_big(u) ? 544 : 440;
	if (!u->sp_have || !u->sp_wave) {   /* nothing is analyzed (paused, between two tracks): a tube at rest */
		memset(u->sp_wl, 0, sizeof u->sp_wl);
		memset(u->sp_wr, 0, sizeof u->sp_wr);
	}
	bars = !u->gon_nobars && w >= side + 260 && h >= 132;
	if (!bars)
		x += (w - side) / 2;   /* alone, the scope sits in the middle of its box */
	r = side / 2 - 4;
	cx = x + side / 2;
	cy = y + h / 2;

	for (i = 0; i < COLS; i++) {
		int a = u->sp_wl[i] < 0 ? -u->sp_wl[i] : u->sp_wl[i], b = u->sp_wr[i] < 0 ? -u->sp_wr[i] : u->sp_wr[i];

		if (a > peak)
			peak = a;
		if (b > peak)
			peak = b;
	}
	{
		float want = 9000.0f / (float)peak;

		if (want > 40.0f)
			want = 40.0f;
		if (want < 1.0f)
			want = 1.0f;
		gain += (want - gain) * (want < gain ? 0.5f : 0.08f);
	}
	if (u->gon_look == 0)
		draw_tube(u, x, cx, cy, side);
	else
		draw_flat(u, x, cx, cy, r, side);

	/* the readings, when there is room beside the scope */
	if (bars) {
		int mx = x + side + 36, mw = w - side - 36 - 12, my = y + (h - 124) / 2;
		float lv, pk, rv, c = u->sp_c, d;
		char big[24];
		uint32_t fill = ink(u, 0.5f);

		vis_level(u, 0, &lv, &pk);
		vis_level(u, 1, &rv, &pk);
		d = (lv < -120.0f && rv < -120.0f) ? 0.0f : (rv - lv) / 12.0f;
		corr += (c - corr) * 0.25f;
		bal += (d - bal) * 0.25f;
		if (mw > 520)
			mw = 520;
		meter(u, mx, my, mw, "CORRELATION", "-1", "0", "+1", corr, 1, fill);
		meter(u, mx, my + 76, mw, "BALANCE", "L", "centre", "R", bal, 0, fill);
		snprintf(big, sizeof big, "%+.2f", (double)corr);
		x45_text(&u->g.back, mx + mw - x45_text_w(big, 2), my, big, 2, corr < 0 ? u->t.warn : u->t.text);
		snprintf(big, sizeof big, "%+.1f dB", (double)(bal * 12.0f));
		x45_text(&u->g.back, mx + mw - x45_text_w(big, 2), my + 76, big, 2, u->t.text);
	}
}

static void reset(void)
{
	gain = 1.0f;
	corr = bal = 0;
	memset(ph, 0, sizeof ph);
	ph_n = 0;
	ph_t = 0;
}

const struct ui_vis vis_gonio = {
	.name = "gonio", .label = "Stereo field", .bit = 32, .weight = 4, .cap = cap, .wave = 1, .draw = draw, .reset = reset,
	.square = 1, .look = VIS_LOOK_FIELD,
};
