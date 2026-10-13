#define _GNU_SOURCE
/* skins - the registry of the skins in this folder, and the drawing helpers they share.
 * To add a skin: write skins/NAME.c with a `const struct ui_skin skin_NAME`, then list it below. */
#include "skin.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

extern const struct ui_skin skin_classic, skin_carousel, skin_tiles, skin_ledger, skin_matrix, skin_split, skin_poster;

static const struct ui_skin *const all[] = {
	&skin_classic,      /* the first is the fallback: unknown names and a missing setting mean this one */
	&skin_carousel,
	&skin_tiles,
	&skin_ledger,
	&skin_matrix,
	&skin_split,
	&skin_poster,
};
#define NSKIN ((int)(sizeof all / sizeof *all))

int skin_count(void)
{
	return NSKIN;
}

const struct ui_skin *skin_at(int i)
{
	return all[i >= 0 && i < NSKIN ? i : 0];
}

int skin_find(const char *name)
{
	int i;

	for (i = 0; i < NSKIN; i++)
		if (!strcmp(all[i]->name, name))
			return i;
	return -1;
}

const struct ui_skin *skin_cur(const struct ui *u)
{
	return skin_at(u->skin_id);
}

/* ------------------------------------------------------------------ dispatch --- */

void skin_bar(struct ui *u, const char *title)
{
	const struct ui_skin *k = skin_cur(u);

	if (k->bar)
		k->bar(u, title);
	else
		skin_bar_classic(u, title);
}

void skin_list(struct ui *u, struct ui_list *l)
{
	const struct ui_skin *k = skin_cur(u);

	if (k->list)
		k->list(u, l);
	else
		skin_list_rows(u, l);
}

int skin_has_np(const struct ui *u)
{
	return skin_cur(u)->np != NULL;
}

void skin_np(struct ui *u, struct ui_np *np)
{
	const struct ui_skin *k = skin_cur(u);

	if (k->np)
		k->np(u, np);
}

void skin_hints(struct ui *u, const char *spec)
{
	const struct ui_skin *k = skin_cur(u);

	if (k->hints)
		k->hints(u, spec);
	else
		skin_hints_keycaps(u, spec);
}

int skin_rows(const struct ui *u)
{
	int r = skin_cur(u)->rows;

	return r > 0 ? r : 9;
}

/* ------------------------------------------------------------------- helpers --- */

uint32_t skin_mix(uint32_t a, uint32_t b, int pct_b)
{
	uint32_t out = 0;
	int lane;

	for (lane = 0; lane < 24; lane += 8) {
		int x = (a >> lane) & 0xff, y = (b >> lane) & 0xff;

		out |= (uint32_t)((x * (100 - pct_b) + y * pct_b) / 100) << lane;
	}
	return out | (a & 0xff000000u);
}

/* how far a rounded corner of radius r cuts into row dy (0 = the top row of the shape) */
static int corner(int dy, int r)
{
	float d;

	if (r <= 0 || dy >= r)
		return 0;
	d = (float)(r - dy) - 0.5f;
	return r - (int)(sqrtf((float)r * (float)r - d * d) + 0.5f);
}

void skin_round_rect(struct x45_surface *s, int x, int y, int w, int h, int r, uint32_t col)
{
	int dy;

	if (r > w / 2)
		r = w / 2;
	if (r > h / 2)
		r = h / 2;
	for (dy = 0; dy < h; dy++) {
		int in = corner(dy < h - dy - 1 ? dy : h - dy - 1, r);

		x45_fill_rect(s, x + in, y + dy, w - 2 * in, 1, col);
	}
}

void skin_round_outline(struct x45_surface *s, int x, int y, int w, int h, int r, int th, uint32_t col)
{
	int dy, ri;

	if (r > w / 2)
		r = w / 2;
	if (r > h / 2)
		r = h / 2;
	ri = r > th ? r - th : 0;
	for (dy = 0; dy < h; dy++) {
		int edge = dy < h - dy - 1 ? dy : h - dy - 1, o = corner(edge, r);

		if (edge < th) {
			x45_fill_rect(s, x + o, y + dy, w - 2 * o, 1, col);
		} else {
			int i = th + corner(edge - th, ri);

			if (i < o)
				i = o;
			x45_fill_rect(s, x + o, y + dy, i - o, 1, col);
			x45_fill_rect(s, x + w - i, y + dy, i - o, 1, col);
		}
	}
}

/* An icon at `size` px. The files are 24 px glyphs (the lists, the top bar) and NAME@128 pictures of the same
 * drawing; anything larger than a glyph is drawn from the picture, and shrunk with every pixel counted, so it stays
 * sharp. A picture that is missing falls back on the glyph. */
void skin_icon(struct ui *u, const char *name, int x, int y, int size, uint32_t col)
{
	const struct x45_icon *ic = NULL;

	if (name && size > 30) {
		char big[48];

		snprintf(big, sizeof big, "%s@128", name);
		ic = icon_get(u, big);
	}
	if (!ic && name)
		ic = icon_get(u, name);
	if (ic)
		x45_icon_draw_scaled(&u->g.back, ic, x, y, size, col);
}

void skin_text_c(struct ui *u, int slot, int cx, int y, const char *utf8, int scale, int max_w, uint32_t col)
{
	char buf[320];
	int tw;

	x45_ascii_fold(buf, sizeof buf, utf8);
	tw = x45_text_w(buf, scale);
	if (tw <= max_w)
		x45_text(&u->g.back, cx - tw / 2, y, buf, scale, col);
	else
		text_scroll(u, slot, cx - max_w / 2, y, utf8, scale, max_w, col);
}

void skin_text_r(struct ui *u, int right, int y, const char *utf8, int scale, int max_w, uint32_t col)
{
	char buf[320];
	int tw;

	x45_ascii_fold(buf, sizeof buf, utf8);
	tw = x45_text_w(buf, scale);
	if (tw > max_w)
		tw = max_w;
	x45_text_clipped(&u->g.back, right - tw, y, buf, scale, tw, col);
}

int skin_text_scale_fit(const char *s, int max_w, int from, int to)
{
	char buf[320];
	int sc;

	x45_ascii_fold(buf, sizeof buf, s);
	for (sc = from; sc > to; sc--)
		if (x45_text_w(buf, sc) <= max_w)
			return sc;
	return to;
}

uint32_t skin_item_col(const struct ui *u, const struct ui_item *it, int selected)
{
	if (selected)
		return u->t.bg;
	return it->action ? u->t.dim : it->dir ? u->t.accent : u->t.text;
}

static uint32_t kind_col(const struct ui *u, int kind)
{
	return kind == 1 ? u->t.warn : kind == 2 ? u->t.accent : u->t.dim;
}

void skin_list_msg(struct ui *u, const struct ui_list *l, int x, int y, int center)
{
	int w = u->g.back.w - 48, sc;

	if (!l->msg || !l->msg[0])
		return;
	sc = skin_text_scale_fit(l->msg, w, 3, 2);
	if (center)
		skin_text_c(u, -1, u->g.back.w / 2, y, l->msg, sc, w, kind_col(u, l->msg_kind));
	else
		text_fold(&u->g.back, x, y, l->msg, sc, w, kind_col(u, l->msg_kind));
}

void skin_list_title(struct ui *u, const struct ui_list *l, int center)
{
	struct x45_surface *s = &u->g.back;

	if (l->title && l->title[0]) {
		if (center)
			skin_text_c(u, -1, s->w / 2, 68, l->title, 2, s->w - 48, kind_col(u, l->title_kind));
		else
			text_fold(s, 24, 68, l->title, 2, s->w - 48 - (l->note ? 260 : 0), kind_col(u, l->title_kind));
	}
	if (l->note && l->note[0])
		skin_text_r(u, s->w - 24, 68, l->note, 2, 400, u->t.warn);
}

/* A setting's value in a chip of its own, the chip's right edge at `right`, its top at y: so that the name of a
 * setting and its value can never be taken for one another. Text in the accent colour; the chip is the darker
 * patch (dark on the accent bar when `sel`, a tint of the accent otherwise). Returns the chip's left edge. */
int skin_value_chip(struct ui *u, int right, int y, int h, const char *utf8, int scale, int max_w, int sel, int r)
{
	struct x45_surface *s = &u->g.back;
	char buf[160];
	int tw, pad = 10 + scale * 2, x;

	x45_ascii_fold(buf, sizeof buf, utf8);
	tw = x45_text_w(buf, scale);
	if (tw > max_w - 2 * pad)
		tw = max_w - 2 * pad;
	x = right - tw - 2 * pad;
	skin_round_rect(s, x, y, tw + 2 * pad, h, r, sel ? u->t.bg : skin_mix(u->t.bg, u->t.accent, 20));
	x45_text_clipped(s, x + pad, y + (h - 7 * scale) / 2, buf, scale, tw, u->t.accent);
	return x;
}
