#define _GNU_SOURCE
/* ledger - a table, like a terminal. Small type in columns and the accent colour tinting everything, so that
 * the green theme is a green monitor and the amber one an amber monitor.
 *
 * Lists are numbered tables with dotted leaders to their values; the now-playing page is a sheet of named
 * fields (SOURCE, TITLE, ARTIST ...) with the progress as a row of blocks.
 */
#include "skin.h"

#include <stdio.h>
#include <string.h>

static void bar(struct ui *u, const char *title)
{
	struct x45_surface *s = &u->g.back;
	char buf[96];
	int x;

	snprintf(buf, sizeof buf, "[ %s ]", title);
	x45_text(s, 24, 18, buf, 3, u->t.accent);
	for (x = 12; x < s->w - 12; x += 8)   /* a dotted rule */
		x45_fill_rect(s, x, 53, 2, 2, u->t.line);
}

static void hints(struct ui *u, const char *spec)
{
	struct x45_surface *s = &u->g.back;
	char buf[256], *p, *save = NULL;
	int x = 24, y = s->h - 38;

	snprintf(buf, sizeof buf, "%s", spec);
	for (p = strtok_r(buf, ";", &save); p; p = strtok_r(NULL, ";", &save)) {
		char *bar = strchr(p, '|'), key[40];
		const char *act = "";
		int kw, aw;

		if (bar) {
			*bar = '\0';
			act = bar + 1;
		}
		snprintf(key, sizeof key, "[%s]", p);
		kw = x45_text_w(key, 2);
		aw = x45_text_w(act, 2);
		if (x + kw + 8 + aw > s->w - 12)
			break;
		x45_text(s, x, y, key, 2, u->t.accent);
		x45_text(s, x + kw + 8, y, act, 2, u->t.dim);
		x += kw + 8 + aw + 26;
	}
}

/* ------------------------------------------------------------------- lists --- */

/* a dotted leader from x0 to x1 on the baseline of a row of text at y */
static void leader(struct ui *u, int x0, int x1, int y, uint32_t col)
{
	int x;

	for (x = x0; x + 2 <= x1; x += 8)
		x45_fill_rect(&u->g.back, x, y + 11, 2, 2, col);
}

static void list(struct ui *u, struct ui_list *l)
{
	struct x45_surface *s = &u->g.back;
	const int rh = 30, y0 = l->title ? 92 : 76, vis = l->top ? skin_rows(u) : l->n;
	const int numx = 24, labx = l->kind == LK_ICONS ? 128 : 92;
	int i, first = 0;

	skin_list_title(u, l, 0);
	if (l->n == 0)
		skin_list_msg(u, l, numx, y0, 0);
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
		int idx = first + i, y = y0 + i * rh, sel = idx == l->sel, x = labx, right = s->w - 28, lw;
		struct ui_item it;
		uint32_t fg, vc;
		char num[12], buf[170];

		memset(&it, 0, sizeof it);
		l->item(u, l->ctx, idx, &it);
		fg = skin_item_col(u, &it, sel);
		vc = sel ? u->t.bg : u->t.accent;
		if (sel)
			x45_fill_rect(s, 12, y - 4, s->w - 24, rh - 2, u->t.accent);
		snprintf(num, sizeof num, "%03d", idx + 1);
		x45_text(s, numx, y, num, 2, sel ? u->t.bg : u->t.dim);
		if (l->kind == LK_ICONS && it.icon) {
			const struct x45_icon *ic = icon_get(u, it.icon);

			if (ic)
				x45_icon_draw(s, ic, 82, y - 3 + (rh - 2 - ic->h) / 2, fg);
		}
		if (it.lead[0]) {
			x45_text(s, x, y, it.lead, 2, sel ? u->t.bg : u->t.dim);
			x += l->lead_w * 2 / 3;
		}
		snprintf(buf, sizeof buf, "%s%s", it.label, it.dir && !it.action ? "/" : "");
		if (it.mark[0]) {
			x45_text(s, right - x45_text_w(it.mark, 2), y, it.mark, 2, vc);
			right -= x45_text_w(it.mark, 2) + 14;
		}
		if (it.value[0] || l->kind == LK_ICONS) {
			char val[64];

			if (sel && l->kind != LK_ICONS) {   /* on the bar the value is a dark chip, so that name and value stay apart */
				int cl = skin_value_chip(u, right + 6, y - 4, rh - 8, it.value, 2, s->w / 2, 1, 0);

				right = cl - 14;
				val[0] = '\0';
			} else if (l->kind == LK_ICONS) {
				snprintf(val, sizeof val, "%s", idx == l->current ? "[ACTIVE]" : "");
			} else {
				snprintf(val, sizeof val, "[ %.40s ]", it.value);
			}
			if (val[0]) {
				int vw = x45_text_w(val, 2);

				if (vw > s->w / 2)
					vw = s->w / 2;
				x45_text_clipped(s, right - vw, y, val, 2, vw, vc);
				right -= vw + 14;
			}
		}
		lw = x45_text_w(buf, 2);
		if (right - x < lw)
			lw = right - x;
		if (sel && l->scroll_sel)
			text_scroll(u, 3, x, y, buf, 2, right - x, fg);
		else
			text_fold(s, x, y, buf, 2, right - x, fg);
		if (!sel && (it.value[0] || l->kind == LK_ICONS) && right - (x + lw) > 24)
			leader(u, x + lw + 10, right - 4, y, u->t.line);
		if (idx == l->current && l->kind != LK_ICONS)
			x45_text(s, s->w - 24 - x45_text_w("*", 2), y, "*", 2, sel ? u->t.bg : u->t.accent);
	}
	if (l->top && l->n > vis) {   /* a thin scroll rail at the right edge */
		int th = vis * rh, bh = th * vis / l->n, by = y0 - 4 + th * first / l->n;

		if (bh < 12)
			bh = 12;
		x45_fill_rect(s, s->w - 6, y0 - 4, 1, th, u->t.line);
		x45_fill_rect(s, s->w - 8, by, 4, bh, u->t.dim);
	}
	if (l->hint)
		draw_hints(u, l->hint);
}

/* ------------------------------------------------------------- now playing --- */

static void field(struct ui *u, int y, const char *name, uint32_t nc)
{
	char buf[24];

	snprintf(buf, sizeof buf, "%s", name);
	x45_text(&u->g.back, 24, y + 2, buf, 2, nc);
}

static void np(struct ui *u, struct ui_np *n)
{
	struct x45_surface *s = &u->g.back;
	const int fx = 190;
	int w = s->w - fx - 24, y = 80;
	char line[128];

	if (n->slide) {
		x45_image_draw(s, n->slide, s->w - 24 - n->slide->w, 116);
		w -= n->slide->w + 20;
	}
	field(u, y, "SOURCE", u->t.dim);
	x45_text(s, fx, y + 2, n->kicker ? n->kicker : "", 2, u->t.accent);
	y += n->big_title ? 46 : 36;
	field(u, y, n->lab[0] ? n->lab[0] : "TITLE", u->t.dim);
	if (n->big_title)
		x45_text(s, fx, y - 10, n->title, 5, u->t.text);
	else
		text_scroll(u, 0, fx, y, n->title, 3, w, u->t.text);
	y += n->big_title ? 58 : 40;
	if (n->sub[0]) {
		field(u, y, n->lab[1] ? n->lab[1] : "ARTIST", u->t.dim);
		text_scroll(u, 1, fx, y, n->sub, 3, w, u->t.accent);
		y += 40;
	}
	if (n->extra[0]) {
		field(u, y, n->lab[2] ? n->lab[2] : "ALBUM", u->t.dim);
		text_scroll(u, 2, fx, y + 2, n->extra, 2, w, u->t.text);
		y += 32;
	}
	if (n->fmt[0]) {
		field(u, y, "FORMAT", u->t.dim);
		text_fold(s, fx, y + 2, n->fmt, 2, w, u->t.text);
		y += 32;
	}
	if (n->time[0] || n->frac >= 0) {
		int tw = n->time[0] ? x45_text_w(n->time, 3) : 0;

		field(u, y, "TIME", u->t.dim);
		x45_text(s, fx, y, n->time, 3, u->t.text);
		if (n->frac >= 0) {   /* the progress as blocks, 14 px each */
			int bx = fx + tw + 28, nb = (s->w - 24 - bx) / 16, on = (int)(nb * (n->frac > 1 ? 1 : n->frac) + 0.5), i;

			for (i = 0; i < nb; i++)
				x45_fill_rect(s, bx + i * 16, y + 2, 12, 18, i < on ? u->t.accent : u->t.barbg);
		}
		y += 38;
	}
	snprintf(line, sizeof line, "%s%s%s", n->state, n->flags[0] && n->state[0] ? "   " : "", n->flags);
	if (line[0]) {
		field(u, y, "STATE", u->t.dim);
		text_fold(s, fx, y + 2, line, 2, w, n->playing ? u->t.accent : u->t.dim);
		y += 32;
	}
	if (n->msg)
		text_fold(s, fx, y + 2, n->msg, 2, w, n->msg_warn ? u->t.warn : u->t.dim);
	/* the visuals under a rule */
	x45_hline(s, 12, 394, s->w - 24, u->t.line);
	draw_visuals_rect(u, 12, 402, s->w - 24, s->h - 12 - 402);
}

const struct ui_skin skin_ledger = {
	.name = "ledger",
	.label = "Ledger",
	.about = "A table like a terminal: numbered rows with dotted leaders, now playing as named fields",
	.bg = { 5, 8, 7 }, .panel = { 10, 16, 14 }, .line = { 40, 54, 48 },
	.text = { 214, 224, 216 }, .dim = { 110, 128, 116 }, .barbg = { 20, 30, 26 }, .card = { 10, 16, 14 },
	.tint = 60,
	.rows = 13,
	.bar = bar,
	.list = list,
	.np = np,
	.hints = hints,
};
