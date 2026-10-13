#define _GNU_SOURCE
/* matrix - an LED dot-matrix display. Every word is a grid of lit dots over the dim ones that are off, the way a
 * hi-fi display or a train indicator writes; text too long for its place runs past like a ticker.
 *
 * Lists are one line of dots per row, the row under the cursor lit in the accent colour on a field of off dots; a
 * setting's value sits in a framed window of its own. Now playing is the title in large dots, the artist under it,
 * the time and a row of LEDs for the progress.
 */
#include "skin.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* ----------------------------------------------------------------- the dots --- */

static void led(struct x45_surface *s, int x, int y, int d, uint32_t col)
{
	if (d >= 4)
		skin_round_rect(s, x, y, d, d, d / 2, col);
	else
		x45_fill_rect(s, x, y, d, d, col);
}

/* the width of `str` written with dots every p px: 5 columns and a gap for each character */
static int dots_w(const char *str, int p)
{
	int n = (int)strlen(str);

	return n ? n * 6 * p - p : 0;
}

static void dots(struct x45_surface *s, int x, int y, const char *str, int p, uint32_t col)
{
	const int d = p - 1;

	for (; *str; str++, x += 6 * p) {
		const unsigned char *g = x45_glyph((unsigned char)*str);
		int c, r;

		for (c = 0; c < 5; c++)
			for (r = 0; r < 7; r++)
				if (g[c] & (1 << r))
					led(s, x + c * p, y + r * p, d, col);
	}
}

/* the dots that are off: a field of w x 7 rows, where the lit ones will go */
static void field(struct x45_surface *s, int x, int y, int w, int p, uint32_t col)
{
	const int d = p - 1;
	int cx, r;

	for (cx = 0; cx + d <= w; cx += p)
		for (r = 0; r < 7; r++)
			led(s, x + cx, y + r * p, d, col);
}

/* `utf8` in capitals, folded to what the font has */
static void caps(char *out, size_t n, const char *utf8)
{
	size_t i;

	x45_ascii_fold(out, n, utf8);
	for (i = 0; out[i]; i++)
		out[i] = (char)toupper((unsigned char)out[i]);
}

/* the largest dot pitch in [lo, hi] at which `str` fits w px (lo when none does) */
static int pitch_fit(const char *str, int w, int hi, int lo)
{
	int p;

	for (p = hi; p > lo; p--)
		if (dots_w(str, p) <= w)
			return p;
	return lo;
}

static long mono_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

/* A line of dots in a window of w px. Too long for it: it is cut at the edge, or with `run` it travels like a
 * ticker (a rest at the start, then left until the end shows, then back to the start). `slot` keeps the clock of
 * each running line apart; a new text starts again from the left. */
#define TK_SLOTS 4
static void ticker(struct ui *u, int slot, int x, int y, const char *utf8, int p, int w, uint32_t col, int run)
{
	static unsigned hash[TK_SLOTS];
	static long t0[TK_SLOTS];
	struct x45_surface *s = &u->g.back;
	char buf[320];
	int tw, off = 0;

	caps(buf, sizeof buf, utf8);
	tw = dots_w(buf, p);
	if (tw > w && run && slot >= 0 && slot < TK_SLOTS) {
		const long now = mono_ms(), rest = 1800;
		const int travel = tw + 6 * p - w, speed = 80;   /* px per second */
		long run_ms = (long)travel * 1000 / speed, t;
		unsigned h = 5381;
		const char *c;

		for (c = buf; *c; c++)
			h = h * 33 + (unsigned char)*c;
		if (hash[slot] != h || !t0[slot]) {
			hash[slot] = h;
			t0[slot] = now;
		}
		t = (now - t0[slot]) % (rest + run_ms + rest);
		off = t < rest ? 0 : t < rest + run_ms ? (int)((t - rest) * speed / 1000) : travel;
		u->scrolling = 1;
	}
	x45_clip_set(s, x, y - 2, w < tw ? w : tw, 7 * p + 4);
	dots(s, x - off, y, buf, p, col);
	x45_clip_reset(s);
}

/* -------------------------------------------------------------- bar and keys --- */

static void bar(struct ui *u, const char *title)
{
	struct x45_surface *s = &u->g.back;
	char buf[64];
	int x, w;

	caps(buf, sizeof buf, title);
	w = dots_w(buf, 4);
	field(s, 24, 13, w, 4, u->t.barbg);
	dots(s, 24, 13, buf, 4, u->t.accent);
	for (x = 12; x < s->w - 12; x += 6)   /* a rule of dots */
		x45_fill_rect(s, x, 53, 2, 2, u->t.line);
}

/* ------------------------------------------------------------------- lists --- */

static void list(struct ui *u, struct ui_list *l)
{
	struct x45_surface *s = &u->g.back;
	const int rh = 38, p = 4, y0 = l->title ? 94 : 76, vis = l->top ? skin_rows(u) : l->n;
	int i, first = 0;

	skin_list_title(u, l, 0);
	if (l->n == 0)
		skin_list_msg(u, l, 24, y0, 0);
	if (l->top) {
		if (l->sel < *l->top)
			*l->top = l->sel;
		if (l->sel >= *l->top + vis)
			*l->top = l->sel - vis + 1;
		if (*l->top < 0)
			*l->top = 0;
		first = *l->top;
	}
	for (i = 0; i < vis && first + i < l->n; i++) {
		const int idx = first + i, y = y0 + i * rh, ty = y + (rh - 7 * p) / 2 - 1, sel = idx == l->sel;
		int x = 52, right = s->w - 30;
		struct ui_item it;
		uint32_t lit;
		char buf[170];

		memset(&it, 0, sizeof it);
		l->item(u, l->ctx, idx, &it);
		lit = sel ? u->t.accent : it.action ? u->t.dim : it.dir ? skin_mix(u->t.accent, u->t.bg, 30) :
		      skin_mix(u->t.text, u->t.bg, 12);
		if (sel) {
			x45_fill_rect(s, 12, y, s->w - 24, rh - 2, u->t.panel);
			dots(s, 24, ty, ">", p, u->t.accent);
		}
		if (l->kind == LK_ICONS && it.icon) {
			const struct x45_icon *ic = icon_get(u, it.icon);

			if (ic)
				x45_icon_draw(s, ic, 52, y + (rh - 2 - ic->h) / 2, lit);
			x += 40;
		}
		if (it.lead[0]) {
			x45_text(s, x, y + (rh - 14) / 2 - 1, it.lead, 2, sel ? u->t.text : u->t.dim);
			x += l->lead_w * 2 / 3 + 8;
		}
		if (idx == l->current)
			led(s, s->w - 34, y + (rh - 2) / 2 - 5, 10, sel ? u->t.accent : u->t.dim);
		if (idx == l->current)
			right -= 22;
		if (it.mark[0]) {
			int mw = x45_text_w(it.mark, 2);

			x45_text(s, right - mw, y + (rh - 14) / 2 - 1, it.mark, 2, lit);
			right -= mw + 14;
		}
		if (it.value[0]) {   /* a window for the value, so that it is never mistaken for the name */
			char val[64];
			int vw, cw, ch = rh - 8, cx;

			caps(val, sizeof val, it.value);
			vw = dots_w(val, p);
			if (vw > s->w / 2 - 40)
				vw = s->w / 2 - 40;
			cw = vw + 24;
			cx = right - cw;
			skin_round_outline(s, cx, y + 2, cw, ch, 3, 1, sel ? u->t.accent : u->t.line);
			ticker(u, 1, cx + 12, ty, it.value, p, vw, sel ? u->t.text : u->t.accent, sel);
			right = cx - 16;
		}
		snprintf(buf, sizeof buf, "%s%s", it.label, it.dir && !it.action ? "/" : "");
		if (sel)
			field(s, x, ty, right - x < dots_w(buf, p) ? right - x : dots_w(buf, p), p, u->t.barbg);
		ticker(u, 0, x, ty, buf, p, right - x, lit, sel && l->scroll_sel);
	}
	if (l->top && l->n > vis) {   /* a column of dots at the right edge: where the window is */
		const int th = vis * rh, dots_n = th / 8;
		int k, a = dots_n * first / l->n, b = dots_n * (first + vis) / l->n;

		for (k = 0; k < dots_n; k++)
			x45_fill_rect(s, s->w - 7, y0 + k * 8, 3, 3, k >= a && k < b ? u->t.accent : u->t.line);
	}
	if (l->hint)
		draw_hints(u, l->hint);
}

/* ------------------------------------------------------------- now playing --- */

/* a line of text in dots with its field of off dots behind, at the largest pitch in [lo, hi] that fits w */
static void line(struct ui *u, int slot, int x, int y, const char *utf8, int hi, int lo, int w, uint32_t col)
{
	char buf[320];
	int p;

	caps(buf, sizeof buf, utf8);
	p = pitch_fit(buf, w, hi, lo);
	field(&u->g.back, x, y, w < dots_w(buf, p) ? w : dots_w(buf, p), p, u->t.barbg);
	ticker(u, slot, x, y, buf, p, w, col, 1);
}

static void np(struct ui *u, struct ui_np *n)
{
	struct x45_surface *s = &u->g.back;
	const int x = 24;
	int w = s->w - 2 * x, y = 76, ty;
	char row[160];

	if (n->slide) {
		x45_image_draw(s, n->slide, s->w - 24 - n->slide->w, 84);
		w -= n->slide->w + 24;
	}
	if (n->kicker)
		x45_text(s, x, y, n->kicker, 2, u->t.accent);
	y += 28;
	if (n->big_title) {
		char big[160];

		caps(big, sizeof big, n->title);
		field(s, x, y, dots_w(big, 7), 7, u->t.barbg);
		dots(s, x, y, big, 7, u->t.text);
		y += 7 * 7 + 18;
	} else {
		line(u, 0, x, y, n->title, 6, 4, w, u->t.text);
		y += 6 * 7 + 18;   /* room for the largest pitch, so that the rest stays in place */
	}
	if (n->sub[0]) {
		line(u, 1, x, y, n->sub, 4, 3, w, u->t.accent);
		y += 4 * 7 + 12;
	}
	if (n->extra[0]) {
		line(u, 2, x, y, n->extra, 3, 3, w, skin_mix(u->t.text, u->t.bg, 30));
		y += 3 * 7 + 12;
	}
	if (n->fmt[0]) {
		text_fold(s, x, y, n->fmt, 2, w, u->t.dim);
		y += 26;
	}
	if (n->time[0] || n->frac >= 0) {
		char t[64];
		int p = 4, tw = 0;

		caps(t, sizeof t, n->time);
		tw = dots_w(t, p);
		if (t[0]) {
			field(s, x, y, tw, p, u->t.barbg);
			dots(s, x, y, t, p, u->t.text);
		}
		if (n->frac >= 0) {   /* the progress as a row of LEDs */
			const int bx = x + tw + (tw ? 32 : 0), pitch = 12, nb = (w - (bx - x)) / pitch;
			int on = (int)(nb * (n->frac > 1 ? 1 : n->frac) + 0.5), k;

			for (k = 0; k < nb; k++)
				led(s, bx + k * pitch, y + 8, 9, k < on ? u->t.accent : u->t.barbg);
		}
		y += 7 * p + 14;
	}
	ty = y;
	snprintf(row, sizeof row, "%s%s%s", n->state, n->flags[0] && n->state[0] ? "   " : "", n->flags);
	if (row[0]) {
		char up[200];

		caps(up, sizeof up, row);
		ticker(u, 3, x, ty, up, 3, w, n->playing ? u->t.accent : u->t.dim, 1);
		ty += 28;
	}
	if (n->msg)
		text_fold(s, x, ty, n->msg, 2, w, n->msg_warn ? u->t.warn : u->t.dim);
	x45_hline(s, 12, 392, s->w - 24, u->t.line);
	draw_visuals_rect(u, 12, 400, s->w - 24, s->h - 12 - 400);
}

const struct ui_skin skin_matrix = {
	.name = "matrix",
	.label = "Matrix",
	.about = "An LED dot-matrix display: words in lit dots on a field of off dots, long text runs like a ticker",
	.bg = { 3, 4, 3 }, .panel = { 9, 13, 9 }, .line = { 36, 50, 36 },
	.text = { 226, 234, 224 }, .dim = { 100, 118, 100 }, .barbg = { 17, 25, 17 }, .card = { 9, 13, 9 },
	.tint = 55,
	.rows = 11,
	.bar = bar,
	.list = list,
	.np = np,
};
