#define _GNU_SOURCE
/* spectrum - the analyzer's bands, in the style the owner chose (Options > Visualizers > Spectrum style): bars with a
 * falling peak line and the frequencies written underneath (this file), LED cells (led.c), a mirrored picture
 * (mirror.c) or a curve (area.c). The number of bands is the owner's too (Spectrum bands), and the colours (Bar colours). */
#include "vis.h"

#include <math.h>
#include <string.h>

/* u->bands log-spaced bands (20 Hz .. 20 kHz) as bars in the box x,y,w,h */
static void draw_bars(struct ui *u, int x, int y, int w, int h)
{
	static const struct { int f; const char *l; } fm[] = {
		{ 50, "50" }, { 100, "100" }, { 200, "200" }, { 500, "500" }, { 1000, "1k" },
		{ 2000, "2k" }, { 5000, "5k" }, { 10000, "10k" }, { 20000, "20k" }
	};
	struct x45_surface *s = &u->g.back;
	float bb[NSB], dt;
	int nb = u->bands, i, bw, gap, total, lab = h >= 70 ? 20 : 0;
	long now = vis_now();

	if (nb < 8 || nb > NSB)
		nb = NSB;
	bw = w / nb;
	if (bw < 2)
		return;
	gap = bw >= 12 ? 2 : bw >= 6 ? 1 : 0;
	total = w;   /* the bars share the whole width: each one's edges are rounded from i * w / nb */
	h -= lab;
	vis_rebin(u->sp_b, bb, nb);
	dt = u->sp_pk_t ? (float)(now - u->sp_pk_t) / 1000.0f : 0;
	if (dt > 0.25f)
		dt = 0.25f;
	u->sp_pk_t = now;
	if (u->sp_pk_nb != nb) {
		memset(u->sp_pkb, 0, sizeof u->sp_pkb);
		u->sp_pk_nb = nb;
	}
	x45_fill_rect(s, x, y, total, h, u->t.barbg);
	for (i = 0; i < nb; i++) {
		float f = vis_band_f(bb[i]);
		int bh, ph;

		bh = (int)(f * (float)h + 0.5f);
		if (f >= u->sp_pkb[i]) {
			u->sp_pkb[i] = f;
		} else {
			u->sp_pkb[i] -= 0.35f * dt;
			if (u->sp_pkb[i] < f)
				u->sp_pkb[i] = f;
		}
		if (bh > 0)
			x45_fill_rect(s, x + i * w / nb, y + h - bh, (i + 1) * w / nb - i * w / nb - gap, bh, vis_bar_colour(u, f));
		ph = (int)(u->sp_pkb[i] * (float)h + 0.5f);
		if (ph > bh + 2 && ph < h)
			x45_fill_rect(s, x + i * w / nb, y + h - ph - 2, (i + 1) * w / nb - i * w / nb - gap, 2, u->t.text);
	}
	for (i = 0; lab && i < (int)(sizeof fm / sizeof *fm); i++) {
		int fx = x + (int)((float)total * logf((float)fm[i].f / 20.0f) / logf(1000.0f));
		int tw = x45_text_w(fm[i].l, 2);

		x45_vline(s, fx, y + h, 5, u->t.dim);
		fx -= tw / 2;
		if (fx < x)
			fx = x;
		if (fx + tw > x + total)
			fx = x + total - tw;
		x45_text(s, fx, y + h + 6, fm[i].l, 2, u->t.dim);
	}
}

/* the styles, in the order of u->spec_style (the saved number: new ones only at the end) */
static const struct { const char *name; const struct ui_vis *v; } style[] = {
	{ "bars with peaks", NULL },
	{ "LED cells", &vis_led },
	{ "mirrored", &vis_mirror },
	{ "curve", &vis_area },
};
#define NSTYLE ((int)(sizeof style / sizeof *style))

int vis_spec_styles(void)
{
	return NSTYLE;
}

const char *vis_spec_style_name(int i)
{
	return style[i < 0 || i >= NSTYLE ? 0 : i].name;
}

static void draw(struct ui *u, int x, int y, int w, int h)
{
	int k = u->spec_style < 0 || u->spec_style >= NSTYLE ? 0 : u->spec_style;

	if (style[k].v)
		style[k].v->draw(u, x, y, w, h);
	else
		draw_bars(u, x, y, w, h);
}

/* the styles keep peak holds in their files: the menu's pictures forget them after drawing */
static void reset(void)
{
	int k;

	for (k = 0; k < NSTYLE; k++)
		if (style[k].v && style[k].v->reset)
			style[k].v->reset();
}

const struct ui_vis vis_spectrum = {
	.name = "spectrum", .label = "Spectrum", .bit = 2, .weight = 3, .bands = 1, .look = VIS_LOOK_COLOURS | VIS_LOOK_STYLE,
	.draw = draw, .reset = reset,
};
