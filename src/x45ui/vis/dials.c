#define _GNU_SOURCE
/* dials - two analog VU dials: a scale, the held peak as a mark, a needle with a quick rise and a slow fall.
 * The scale never changes, so it is the part that is drawn once (two_pass): the frames only redraw what moves. */
#include "vis.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define VU_FLOOR -50.0f
#define VU_CURVE 1.7f      /* the needle follows (dB position)^1.7: loud music sits mid-scale, not in the red */
#define VU_SPAN  0.87f     /* needle swing either side of vertical, radians (50 degrees) */

static void line(struct x45_surface *s, int x0, int y0, int x1, int y1, int th, uint32_t c)
{
	int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
	int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
	int err = dx + dy, e2;

	for (;;) {
		x45_fill_rect(s, x0 - th / 2, y0 - th / 2, th, th, c);
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

static float angle(float db)
{
	if (db < VU_FLOOR)
		db = VU_FLOOR;
	if (db > 0)
		db = 0;
	return (-1.0f + powf((db - VU_FLOOR) / -VU_FLOOR, VU_CURVE) * 2.0f) * VU_SPAN;
}

/* one dial in the box x,y,w,h */
static void dial(struct ui *u, int x, int y, int w, int h, int ch, float needle, float pk)
{
	static const int marks[] = { -40, -30, -20, -10, -6, -3, 0 };
	struct x45_surface *s = &u->g.back;
	int px = x + w / 2, py = y + h - 14, i, sc;
	float r = (float)(h - 30), rx = ((float)w / 2 - 16) / sinf(VU_SPAN), a;

	if (rx < r)
		r = rx;
	if (r < 24)
		return;
	if (u->dial_pass != 2) {
		x45_fill_rect(s, x, y, w, h, u->t.barbg);
		x45_draw_rect(s, x, y, w, h, u->t.line);
		/* scale arc, the last 3 dB heavier in the warning colour */
		for (i = 0; i <= 160; i++) {
			float t = (float)i / 160.0f, db = VU_FLOOR + powf(t, 1.0f / VU_CURVE) * -VU_FLOOR;

			a = (-1.0f + 2.0f * t) * VU_SPAN;
			if (db >= -3.0f)
				x45_fill_rect(s, px + (int)(sinf(a) * r) - 2, py - (int)(cosf(a) * r) - 2, 5, 5, u->t.warn);
			else
				x45_fill_rect(s, px + (int)(sinf(a) * r) - 1, py - (int)(cosf(a) * r) - 1, 3, 3, u->t.dim);
		}
		sc = r > 150 ? 2 : 1;
		for (i = 0; i < 7; i++) {
			char lab[12];
			int tw, th = x45_text_h(sc), len = r > 70 ? 14 : 8;

			a = angle((float)marks[i]);
			line(s, px + (int)(sinf(a) * r), py - (int)(cosf(a) * r),
			     px + (int)(sinf(a) * (r - len)), py - (int)(cosf(a) * (r - len)), 3,
			     marks[i] >= -3 ? u->t.warn : u->t.text);
			if (r < 90)
				continue;
			snprintf(lab, sizeof lab, "%d", -marks[i]);
			tw = x45_text_w(lab, sc);
			x45_text(s, px + (int)(sinf(a) * (r - len - 14 - tw / 3)) - tw / 2,
				 py - (int)(cosf(a) * (r - len - 14 - tw / 3)) - th / 2, lab, sc, u->t.dim);
		}
		x45_text(s, x + 10, y + h - 26, ch ? "R" : "L", 2, u->t.dim);
	}
	if (u->dial_pass == 1)
		return;
	if (pk > VU_FLOOR) {
		a = angle(pk);
		line(s, px + (int)(sinf(a) * (r + 3)), py - (int)(cosf(a) * (r + 3)),
		     px + (int)(sinf(a) * (r + 12)), py - (int)(cosf(a) * (r + 12)), 4, u->t.accent);
	}
	a = angle(needle);
	line(s, px, py, px + (int)(sinf(a) * (r - 6)), py - (int)(cosf(a) * (r - 6)), 4, u->t.text);
	x45_fill_rect(s, px - 7, py - 7, 14, 14, u->t.text);
}

static int cap(int w)
{
	return (w - 16) / 2 * 7 / 10;
}

/* needle ballistics: quick rise, slower fall; the clock runs on whether or not the dials are shown */
static void frame(struct ui *u, long now, int on)
{
	float dt = u->vu_t ? (float)(now - u->vu_t) : 0;
	int ch;

	for (ch = 0; on && ch < 2; ch++) {
		float lvl, pk, tau;

		vis_level(u, ch, &lvl, &pk);
		if (lvl < VU_FLOOR)
			lvl = VU_FLOOR;
		tau = lvl > u->vu_n[ch] ? 90.0f : 300.0f;
		u->vu_n[ch] += (lvl - u->vu_n[ch]) * (1.0f - expf(-dt / tau));
	}
	u->vu_t = now;
}

static void draw(struct ui *u, int x, int y, int w, int h)
{
	int dw = (w - 16) / 2;
	float l0, p0, l1, p1;

	vis_level(u, 0, &l0, &p0);
	vis_level(u, 1, &l1, &p1);
	dial(u, x, y, dw, h, 0, u->vu_n[0], p0);
	dial(u, x + w - dw, y, dw, h, 1, u->vu_n[1], p1);
}

const struct ui_vis vis_dials = {
	.name = "dials", .label = "Analog VU", .bit = 4, .weight = 5, .cap = cap, .two_pass = 1,
	.frame = frame, .draw = draw,
};
