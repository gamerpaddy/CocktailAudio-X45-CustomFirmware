#define _GNU_SOURCE
/* split - a list on the left and, on the right, the row under the cursor in full: the whole name of a track that the
 * row can only cut off, the picture of an input, a setting's value in large type. Nothing needs to run to be read.
 *
 * Only the bar and the lists are its own; the now-playing page is the classic one.
 */
#include "skin.h"

#include <stdio.h>
#include <string.h>

static void bar(struct ui *u, const char *title)
{
	struct x45_surface *s = &u->g.back;

	x45_fill_rect(s, 0, 0, s->w, 56, u->t.panel);
	x45_fill_rect(s, 0, 0, 10, 56, u->t.accent);
	x45_hline(s, 0, 56, s->w, u->t.line);
	x45_text(s, 28, 18, title, 3, u->t.text);
}

/* `utf8` broken at spaces into lines of at most w px, from (x, y) at line height lh; returns the lines written. A
 * word longer than a line is broken where it has to be. */
static int wrap(struct ui *u, int x, int y, const char *utf8, int scale, int w, int max_lines, int lh, uint32_t col)
{
	struct x45_surface *s = &u->g.back;
	char buf[320], seg[200];
	const char *p = buf;
	int lines = 0;

	x45_ascii_fold(buf, sizeof buf, utf8);
	while (*p == ' ')
		p++;
	while (*p && lines < max_lines) {
		int k = 0, brk = -1;

		while (p[k] && k < (int)sizeof seg - 1) {
			seg[k] = p[k];
			seg[k + 1] = '\0';
			if (x45_text_w(seg, scale) > w)
				break;
			if (p[k] == ' ')
				brk = k;
			k++;
		}
		if (p[k] && brk > 0)   /* more follows: end the line at the last space that fitted */
			k = brk;
		else if (k == 0)
			k = 1;
		memcpy(seg, p, (size_t)k);
		seg[k] = '\0';
		while (k > 0 && seg[k - 1] == ' ')
			seg[--k] = '\0';
		x45_text_clipped(s, x, y + lines * lh, seg, scale, w, col);
		lines++;
		p += strlen(seg);
		while (*p == ' ')
			p++;
	}
	return lines;
}

static void pill(struct ui *u, int x, int y, const char *text, int on)
{
	struct x45_surface *s = &u->g.back;
	int w = x45_text_w(text, 2) + 28;

	if (on) {
		skin_round_rect(s, x, y, w, 30, 15, u->t.accent);
		x45_text(s, x + 14, y + 8, text, 2, u->t.bg);
	} else {
		skin_round_outline(s, x, y, w, 30, 15, 1, u->t.line);
		x45_text(s, x + 14, y + 8, text, 2, u->t.dim);
	}
}

static void detail(struct ui *u, struct ui_list *l, int x, int y, int w, int h)
{
	struct x45_surface *s = &u->g.back;
	struct ui_item it;
	const int cx = x + w / 2, pad = 28;
	char pos[32];
	int lines;

	skin_round_rect(s, x, y, w, h, 14, u->t.card);
	skin_round_outline(s, x, y, w, h, 14, 1, u->t.line);
	if (l->n == 0)
		return;
	memset(&it, 0, sizeof it);
	l->item(u, l->ctx, l->sel, &it);
	if (l->kind == LK_ICONS) {
		skin_icon(u, it.icon, cx - 56, y + 44, 112, u->t.accent);
		skin_text_c(u, 4, cx, y + 190, it.label, skin_text_scale_fit(it.label, w - 2 * pad, 5, 3), w - 2 * pad, u->t.text);
		if (l->sel == l->current)
			pill(u, cx - (x45_text_w("ACTIVE", 2) + 28) / 2, y + 250, "ACTIVE", 1);
	} else if (l->kind == LK_SETTINGS) {
		int sc;

		lines = wrap(u, x + pad, y + pad, it.label, 4, w - 2 * pad, 3, 40, u->t.text);
		sc = skin_text_scale_fit(it.value, w - 2 * pad - 40, 6, 3);
		if (it.value[0]) {
			int ch = 7 * sc + 40, cy = y + pad + lines * 40 + 24;

			skin_round_rect(s, x + pad, cy, w - 2 * pad, ch, 12, skin_mix(u->t.bg, u->t.accent, 20));
			skin_text_c(u, 5, cx, cy + 20, it.value, sc, w - 2 * pad - 40, u->t.accent);
		}
	} else {
		int ty = y + pad;

		const char *tag = it.dir && !it.action ? "FOLDER" : it.action ? "COMMAND" :
				  l->sel == l->current || it.mark[0] ? (!strcmp(it.mark, "||") ? "PAUSED" : "PLAYING") : NULL;

		if (tag) {
			pill(u, x + pad, ty, tag, tag[0] == 'P');
			ty += 44;
		}
		if (it.lead[0]) {
			x45_text_clipped(s, x + pad, ty, it.lead, 3, w - 2 * pad, u->t.dim);
			ty += 36;
		}
		wrap(u, x + pad, ty, it.label, 3, w - 2 * pad, 8, 34, it.dir ? u->t.accent : u->t.text);
	}
	if (l->n > 1) {
		snprintf(pos, sizeof pos, "%d / %d", l->sel + 1, l->n);
		skin_text_r(u, x + w - 20, y + h - 34, pos, 2, 160, u->t.dim);
	}
}

static void list(struct ui *u, struct ui_list *l)
{
	struct x45_surface *s = &u->g.back;
	const int lw = 452, y0 = l->title ? 100 : 80, vis = l->top ? skin_rows(u) : l->n;
	const int rh = !l->top && l->n * 42 > 540 - y0 ? (540 - y0) / l->n : 42;   /* a list that always fits is squeezed to fit */
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
		const int idx = first + i, y = y0 + i * rh, ty = y + (rh - 21) / 2 - 2, sel = idx == l->sel;
		int x = 28, right = lw - 14;
		struct ui_item it;
		uint32_t fg;
		char buf[170];

		memset(&it, 0, sizeof it);
		l->item(u, l->ctx, idx, &it);
		fg = skin_item_col(u, &it, sel);
		if (sel)
			skin_round_rect(s, 12, y - 3, lw, rh - 2, 8, u->t.accent);
		if (l->kind == LK_ICONS && it.icon) {
			const struct x45_icon *ic = icon_get(u, it.icon);

			if (ic && ic->h <= rh)
				x45_icon_draw(s, ic, x, y - 3 + (rh - 2 - ic->h) / 2, fg);
			x += 40;
		}
		if (it.lead[0]) {
			x45_text(s, x, ty, it.lead, 3, fg);
			x += l->lead_w;
		}
		if (idx == l->current)
			skin_round_rect(s, lw - 14, y + (rh - 2) / 2 - 8, 10, 10, 5, sel ? u->t.bg : u->t.accent);
		if (idx == l->current)
			right -= 20;
		if (it.mark[0]) {
			skin_text_r(u, right, ty + 3, it.mark, 2, 40, sel ? u->t.bg : u->t.accent);
			right -= 40;
		}
		if (it.value[0])
			right = skin_value_chip(u, right + 4, y - 1, rh - 6, it.value, 2, lw / 2, sel, 4) - 12;
		snprintf(buf, sizeof buf, "%s%s", it.label, it.dir && !it.action ? "/" : "");
		text_fold(s, x, ty, buf, 3, right - x, fg);
	}
	detail(u, l, lw + 28, y0 - 20, s->w - lw - 28 - 12, 440);
	if (l->hint)
		draw_hints(u, l->hint);
}

const struct ui_skin skin_split = {
	.name = "split",
	.label = "Split",
	.about = "A list on the left; the row under the cursor in full on the right (whole names, large pictures and values)",
	.bg = { 12, 14, 20 }, .panel = { 24, 28, 40 }, .line = { 62, 72, 94 },
	.text = { 234, 238, 246 }, .dim = { 134, 146, 170 }, .barbg = { 34, 40, 54 }, .card = { 18, 21, 31 },
	.tint = 30,
	.rows = 9,
	.bar = bar,
	.list = list,
};
