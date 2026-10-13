#define _GNU_SOURCE
/* classic - the look the panel was made with: a dark bar on top, plain rows with a bar on the one under the
 * cursor, keycaps along the foot, and a now-playing page that main.c draws itself (no `np` hook). The pieces
 * are public so that other skins can keep the ones they like. */
#include "skin.h"

#include <stdio.h>
#include <string.h>

void skin_bar_classic(struct ui *u, const char *title)
{
	struct x45_surface *s = &u->g.back;

	x45_fill_rect(s, 0, 0, s->w, 56, u->t.panel);
	x45_hline(s, 0, 56, s->w, u->t.line);
	x45_text(s, 24, 18, title, 3, u->t.text);
}

/* The keys that work on a screen, along its foot: "KEY|what;KEY|what". A key is a small keycap with
 * the action beside it, spaced the same on every screen, so they all read the same way. */
void skin_hints_keycaps(struct ui *u, const char *spec)
{
	struct x45_surface *s = &u->g.back;
	char buf[256], *p, *save = NULL;
	int x = 24, y = s->h - 46;

	snprintf(buf, sizeof buf, "%s", spec);
	for (p = strtok_r(buf, ";", &save); p; p = strtok_r(NULL, ";", &save)) {
		char *bar = strchr(p, '|');
		const char *act = "";
		int kw, aw;

		if (bar) {
			*bar = '\0';
			act = bar + 1;
		}
		kw = x45_text_w(p, 2) + 18;
		aw = *act ? x45_text_w(act, 2) : 0;
		if (x + kw + 10 + aw > s->w - 12)
			break;
		x45_fill_rect(s, x, y, kw, 30, u->t.panel);
		x45_draw_rect(s, x, y, kw, 30, u->t.line);
		x45_text(s, x + 9, y + 8, p, 2, u->t.text);
		if (aw)
			x45_text(s, x + kw + 10, y + 8, act, 2, u->t.dim);
		x += kw + 10 + aw + 24;
	}
}

/* Rows, one under the other, the row under the cursor on a bar. A list that scrolls shows 9 rows of 40 px; one
 * that always fits is spread by its length. */
void skin_list_rows(struct ui *u, struct ui_list *l)
{
	struct x45_surface *s = &u->g.back;
	const int y0 = l->title ? 100 : 92, vis = l->top ? skin_rows(u) : l->n;
	const int rh = l->top ? 40 : l->n > 13 ? 31 : l->n > 10 ? 34 : 40;
	const int tx = l->kind == LK_ICONS ? 84 : 32;
	int i;

	skin_list_title(u, l, 0);
	if (l->n == 0)
		skin_list_msg(u, l, 32, y0, 0);
	if (l->top) {
		if (l->sel < *l->top)
			*l->top = l->sel;
		if (l->sel >= *l->top + vis)
			*l->top = l->sel - vis + 1;
		if (*l->top < 0)
			*l->top = 0;
	}
	for (i = 0; i < vis && (l->top ? *l->top : 0) + i < l->n; i++) {
		int idx = (l->top ? *l->top : 0) + i, ry = y0 + i * rh, sel = idx == l->sel, x = tx, avail;
		struct ui_item it;
		uint32_t fg, vc;
		char buf[160];

		memset(&it, 0, sizeof it);
		l->item(u, l->ctx, idx, &it);
		fg = skin_item_col(u, &it, sel);
		vc = sel ? u->t.bg : u->t.accent;
		if (sel)
			x45_fill_rect(s, 12, ry - 6, s->w - 24, rh - 2, u->t.accent);
		if (l->kind == LK_ICONS && it.icon) {
			const struct x45_icon *ic = icon_get(u, it.icon);

			if (ic && ic->h <= rh)
				x45_icon_draw(s, ic, 32 + (40 - ic->w) / 2, ry - 6 + (rh - 2 - ic->h) / 2, fg);
		}
		if (it.lead[0]) {
			x45_text(s, 32, ry, it.lead, 3, fg);
			x = 32 + l->lead_w;
		}
		if (l->marks) {
			snprintf(buf, sizeof buf, "%s%s", it.action ? "  " : it.dir ? "> " : "  ", it.label);
			x = 24;
		} else {
			snprintf(buf, sizeof buf, "%s", it.label);
		}
		avail = s->w - x - 32;
		if (it.value[0] && l->kind == LK_SETTINGS) {   /* the value is a chip of its own: never mistaken for the name */
			int cl = skin_value_chip(u, s->w - (l->current >= 0 ? 56 : 24), ry - 5, rh - 10, it.value, 3, s->w / 2, sel, 4);

			avail = cl - x - 20;
			if (!sel)
				x45_hline(s, 12, ry - 6 + rh - 2, s->w - 24, skin_mix(u->t.bg, u->t.line, 55));
		} else if (it.value[0]) {
			int vw = x45_text_w(it.value, 3), vmax = s->w - tx - 300, vr = s->w - (l->current >= 0 ? 70 : 44);

			if (vw > vmax)
				vw = vmax;
			x45_text_clipped(s, vr - vw, ry, it.value, 3, vw, vc);
			avail = vr - vw - x - 20;
		}
		if (sel && l->scroll_sel)
			text_scroll(u, 3, x, ry, buf, 3, avail, fg);
		else
			text_fold(s, x, ry, buf, 3, avail, fg);
		if (it.mark[0])
			x45_text(s, s->w - 60, ry, it.mark, 3, sel ? u->t.bg : u->t.accent);
		if (idx == l->current)
			x45_fill_rect(s, s->w - 40, ry + 4, 14, 14, sel ? u->t.bg : u->t.accent);
	}
	if (l->hint)
		draw_hints(u, l->hint);
}

const struct ui_skin skin_classic = {
	.name = "classic",
	.label = "Classic",
	.about = "Plain rows on a dark panel, keycaps along the foot",
	.bg = { 10, 12, 18 }, .panel = { 22, 26, 36 }, .line = { 60, 70, 90 },
	.text = { 232, 236, 244 }, .dim = { 130, 142, 165 }, .barbg = { 32, 38, 50 }, .card = { 16, 19, 27 },
	.rows = 9,
};
