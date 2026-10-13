#define _GNU_SOURCE
/* poster - large type, for reading the panel from across the room. Few rows to a page, each in big letters; now
 * playing is the title as large as it will go, the artist under it and the time with a thick progress bar.
 */
#include "skin.h"

#include <stdio.h>
#include <string.h>

static void bar(struct ui *u, const char *title)
{
	struct x45_surface *s = &u->g.back;

	x45_text(s, 24, 12, title, 4, u->t.text);
	x45_fill_rect(s, 24, 48, 140, 5, u->t.accent);
}

/* ------------------------------------------------------------------- lists --- */

static void list(struct ui *u, struct ui_list *l)
{
	struct x45_surface *s = &u->g.back;
	const int rh = 74, y0 = l->title ? 104 : 80, vis = 6;
	int i, first = 0;

	skin_list_title(u, l, 0);
	if (l->n == 0)
		skin_list_msg(u, l, 32, y0, 0);
	if (l->n > vis) {   /* the cursor stays about in the middle of the page */
		first = l->sel - vis / 2;
		if (first > l->n - vis)
			first = l->n - vis;
		if (first < 0)
			first = 0;
	}
	if (l->top)
		*l->top = first;
	for (i = 0; i < vis && first + i < l->n; i++) {
		const int idx = first + i, y = y0 + i * rh, sel = idx == l->sel;
		int x = 36, right = s->w - 36, sc;
		struct ui_item it;
		uint32_t fg;
		char buf[170];

		memset(&it, 0, sizeof it);
		l->item(u, l->ctx, idx, &it);
		fg = skin_item_col(u, &it, sel);
		if (sel)
			skin_round_rect(s, 12, y, s->w - 24, rh - 6, 14, u->t.accent);
		else
			x45_hline(s, 36, y + rh - 4, s->w - 72, skin_mix(u->t.bg, u->t.line, 60));
		if (l->kind == LK_ICONS && it.icon) {
			skin_icon(u, it.icon, x, y + (rh - 6 - 52) / 2, 52, fg);
			x += 80;
		}
		if (it.lead[0]) {
			x45_text(s, x, y + (rh - 6 - 28) / 2, it.lead, 4, fg);
			x += l->lead_w * 4 / 3;
		}
		if (idx == l->current) {
			skin_round_rect(s, right - 18, y + (rh - 6) / 2 - 9, 18, 18, 9, sel ? u->t.bg : u->t.accent);
			right -= 40;
		}
		if (it.mark[0]) {
			skin_text_r(u, right, y + (rh - 6 - 21) / 2, it.mark, 3, 80, sel ? u->t.bg : u->t.accent);
			right -= 70;
		}
		if (it.value[0])
			right = skin_value_chip(u, right + 6, y + 9, rh - 6 - 18, it.value, 3, s->w / 2, sel, 8) - 20;
		snprintf(buf, sizeof buf, "%s%s", it.label, it.dir && !it.action ? "/" : "");
		sc = skin_text_scale_fit(buf, right - x, 5, 3);
		if (sel && l->scroll_sel)
			text_scroll(u, 3, x, y + (rh - 6 - 7 * sc) / 2, buf, sc, right - x, fg);
		else
			text_fold(s, x, y + (rh - 6 - 7 * sc) / 2, buf, sc, right - x, fg);
	}
	if (l->n > vis) {   /* where the page is */
		const int th = vis * rh - 6, bh = th * vis / l->n < 16 ? 16 : th * vis / l->n;

		x45_fill_rect(s, s->w - 6, y0 + (th - bh) * first / (l->n - vis), 3, bh, u->t.dim);
	}
	if (l->hint)
		draw_hints(u, l->hint);
}

/* ------------------------------------------------------------- now playing --- */

static void np(struct ui *u, struct ui_np *n)
{
	struct x45_surface *s = &u->g.back;
	const int x = 28;
	int w = s->w - 2 * x, y = 70, sc;
	char row[160];

	if (n->slide) {
		x45_image_draw(s, n->slide, s->w - 24 - n->slide->w, 84);
		w -= n->slide->w + 24;
	}
	if (n->kicker)
		x45_text(s, x, y, n->kicker, 3, u->t.accent);
	y += 36;
	if (n->big_title) {
		x45_text(s, x, y, n->title, 9, u->t.text);
		y += 9 * 7 + 16;
	} else {
		sc = skin_text_scale_fit(n->title, w, 8, 5);
		text_scroll(u, 0, x, y, n->title, sc, w, u->t.text);
		y += 7 * sc + 16;
	}
	if (n->sub[0]) {
		sc = skin_text_scale_fit(n->sub, w, 5, 4);
		text_scroll(u, 1, x, y, n->sub, sc, w, u->t.accent);
		y += 7 * sc + 12;
	}
	if (n->extra[0]) {
		text_scroll(u, 2, x, y, n->extra, 3, w, u->t.dim);
		y += 34;
	}
	if (n->time[0] || n->frac >= 0) {
		if (n->time[0])
			x45_text_clipped(s, x, y, n->time, 5, w, u->t.text);
		y += 46;
		if (n->frac >= 0) {
			const double f = n->frac > 1 ? 1 : n->frac;

			skin_round_rect(s, x, y, s->w - 2 * x, 18, 9, u->t.barbg);
			if (f > 0.005)
				skin_round_rect(s, x, y, (int)((s->w - 2 * x) * f) < 18 ? 18 : (int)((s->w - 2 * x) * f), 18, 9, u->t.accent);
			y += 30;
		}
	}
	snprintf(row, sizeof row, "%s%s%s", n->state, n->flags[0] && n->state[0] ? "     " : "", n->flags);
	if (row[0]) {
		text_fold(s, x, y, row, 3, w, n->playing ? u->t.accent : u->t.dim);
		y += 32;
	}
	if (n->fmt[0] && y < 372) {
		text_fold(s, x, y, n->fmt, 2, w, u->t.dim);
		y += 26;
	}
	if (n->msg)
		text_fold(s, x, y, n->msg, 2, w, n->msg_warn ? u->t.warn : u->t.dim);
	draw_visuals_rect(u, 24, 428, s->w - 48, s->h - 12 - 428);
}

const struct ui_skin skin_poster = {
	.name = "poster",
	.label = "Poster",
	.about = "Large type for reading across the room: six big rows to a page, the title as large as it will go",
	.bg = { 6, 6, 8 }, .panel = { 16, 16, 20 }, .line = { 70, 70, 80 },
	.text = { 250, 250, 252 }, .dim = { 150, 152, 162 }, .barbg = { 34, 34, 42 }, .card = { 14, 14, 18 },
	.tint = 15,
	.rows = 6,
	.bar = bar,
	.list = list,
	.np = np,
};
