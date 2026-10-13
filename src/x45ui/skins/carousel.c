#define _GNU_SOURCE
/* carousel - everything centred, one thing at a time.
 *
 * Lists are not lists: things with a picture (the inputs) are a row of large symbols with the one under the
 * cursor in the middle; every other list is a wheel with the entry under the cursor large and the ones before
 * and after it fading away. The now-playing page is a poster: the source in small capitals, the title big in the
 * middle, a thin line for the progress. No bar behind the title, no keycaps.
 */
#include "skin.h"

#include <stdio.h>
#include <string.h>

static void bar(struct ui *u, const char *title)
{
	struct x45_surface *s = &u->g.back;
	int tw = x45_text_w(title, 2);

	x45_text(s, (s->w - tw) / 2, 20, title, 2, u->t.dim);
	x45_fill_rect(s, s->w / 2 - 24, 44, 48, 3, u->t.accent);
}

/* "KEY action   KEY action", centred along the foot, the keys bright and the actions quiet */
static void hints(struct ui *u, const char *spec)
{
	struct x45_surface *s = &u->g.back;
	char buf[256], *p, *save = NULL, key[8][32], act[8][48];
	int n = 0, i, total = 0, x;

	snprintf(buf, sizeof buf, "%s", spec);
	for (p = strtok_r(buf, ";", &save); p && n < 8; p = strtok_r(NULL, ";", &save)) {
		char *bar = strchr(p, '|');

		if (bar)
			*bar++ = '\0';
		snprintf(key[n], sizeof key[n], "%s", p);
		snprintf(act[n], sizeof act[n], "%s", bar ? bar : "");
		n++;
	}
	for (i = 0; i < n; i++)
		total += x45_text_w(key[i], 2) + 8 + x45_text_w(act[i], 2) + (i ? 30 : 0);
	while (n > 1 && total > s->w - 24) {   /* too many for the width: drop the last ones */
		n--;
		total -= 30 + x45_text_w(key[n], 2) + 8 + x45_text_w(act[n], 2);
	}
	x = (s->w - total) / 2;
	for (i = 0; i < n; i++) {
		x += i ? 30 : 0;
		x45_text(s, x, s->h - 38, key[i], 2, u->t.text);
		x += x45_text_w(key[i], 2) + 8;
		x45_text(s, x, s->h - 38, act[i], 2, u->t.dim);
		x += x45_text_w(act[i], 2);
	}
}

/* ------------------------------------------------------------------- lists --- */

static void dots(struct ui *u, const struct ui_list *l, int y)
{
	struct x45_surface *s = &u->g.back;
	int n = l->n > 24 ? 24 : l->n, i, x0 = s->w / 2 - n * 7 + 3;
	int pos = l->n > 24 ? l->sel * 24 / l->n : l->sel;

	for (i = 0; i < n; i++)
		skin_round_rect(s, x0 + i * 14, y - (i == pos ? 3 : 0), i == pos ? 8 : 6, i == pos ? 8 : 6, 4,
				i == pos ? u->t.accent : u->t.line);
}

/* things with a picture: a row of symbols, the chosen one in the middle and large */
static void list_symbols(struct ui *u, struct ui_list *l)
{
	struct x45_surface *s = &u->g.back;
	const int cx = s->w / 2, size[3] = { 144, 72, 44 }, dist[3] = { 0, 270, 430 };
	int k, side;
	struct ui_item it;

	if (l->n == 0) {
		skin_list_msg(u, l, 0, 250, 1);
		return;
	}
	for (k = 2; k >= 0; k--) {
		for (side = -1; side <= 1; side += 2) {
			int idx, px, py;
			uint32_t col;

			if (k == 0 && side > 0)
				continue;
			if (k > 0 && 2 * k > l->n - (l->n % 2 == 0))
				continue;   /* the list is too short to show another one on this side without repeating */
			idx = ((l->sel + side * k) % l->n + l->n) % l->n;
			memset(&it, 0, sizeof it);
			l->item(u, l->ctx, idx, &it);
			px = cx + side * dist[k] - size[k] / 2;
			py = k == 0 ? 150 : k == 1 ? 196 : 212;
			col = k == 0 ? u->t.accent : k == 1 ? u->t.dim : u->t.line;
			skin_icon(u, it.icon, px, py, size[k], col);
			if (idx == l->current) {
				if (k == 0)
					skin_text_c(u, -1, cx, 118, "ACTIVE", 2, 200, u->t.accent);
				else
					skin_round_rect(s, cx + side * dist[k] - 4, py + size[k] + 8, 8, 8, 4, u->t.accent);
			}
			if (k == 0) {
				int sc = skin_text_scale_fit(it.label, s->w - 80, 5, 3);

				skin_text_c(u, 3, cx, 318, it.label, sc, s->w - 80, u->t.text);
			} else if (k == 1) {
				skin_text_c(u, -1, cx + side * dist[k], py + size[k] + 24, it.label, 2, 200, u->t.dim);
			}
		}
	}
	dots(u, l, 470);
}

/* one thing under the cursor, large, with the ones before and after it fading away: four on each side, so that a
 * folder or a track list reads as a list and not as one entry. A list too short to fill both sides is not wrapped
 * round (the same entry would sit above and below): it shows what there is on each side of the cursor. */
#define WHEEL_REACH 4
static void list_wheel(struct ui *u, struct ui_list *l)
{
	struct x45_surface *s = &u->g.back;
	const int cx = s->w / 2, w = s->w - 80, y0 = 285;
	/* distance of the entries before the cursor's, then of the ones after it past the cursor's own block (px) */
	static const int up[WHEEL_REACH + 1] = { 0, 62, 104, 140, 172 }, dn[WHEEL_REACH + 1] = { 0, 0, 42, 78, 110 };
	static const int fade[WHEEL_REACH + 1] = { 0, 0, 30, 50, 65 };   /* percent towards the background */
	const int wrap = l->n >= 2 * WHEEL_REACH + 1;
	struct ui_item it;
	char pos[32];
	int k, y, has_val;

	if (l->n == 0) {
		skin_list_msg(u, l, 0, 250, 1);
		return;
	}
	memset(&it, 0, sizeof it);
	l->item(u, l->ctx, l->sel, &it);
	has_val = it.value[0] || it.lead[0];

	/* the cursor's entry */
	{
		int sc = skin_text_scale_fit(it.label, w, 5, 3);
		uint32_t col = it.action ? u->t.dim : it.dir ? u->t.accent : u->t.text;

		y = y0;
		if (l->scroll_sel)
			skin_text_c(u, 3, cx, y, it.label, sc, w, col);
		else
			skin_text_c(u, -1, cx, y, it.label, sc, w, col);
		if (has_val)
			skin_text_c(u, -1, cx, y + 54, it.value[0] ? it.value : it.lead, 3, w, u->t.accent);
		if (l->sel == l->current)
			x45_fill_rect(s, cx - 30, y + (has_val ? 92 : 56), 60, 4, u->t.accent);
		if (it.mark[0])
			skin_text_c(u, -1, cx, y + (has_val ? 100 : 66), it.mark, 3, 80, u->t.accent);
	}
	/* before and after */
	for (k = 1; k <= WHEEL_REACH; k++) {
		int side;

		for (side = -1; side <= 1; side += 2) {
			int idx = l->sel + side * k;
			uint32_t col;
			int sc = k <= 2 ? 3 : 2;
			char buf[200];

			if (wrap)
				idx = (idx % l->n + l->n) % l->n;   /* the knob wraps around the ends */
			else if (idx < 0 || idx >= l->n)
				continue;
			memset(&it, 0, sizeof it);
			l->item(u, l->ctx, idx, &it);
			if (it.lead[0] && !it.value[0])
				snprintf(buf, sizeof buf, "%s  %s", it.lead, it.label);
			else
				snprintf(buf, sizeof buf, "%s", it.label);
			col = it.dir ? skin_mix(u->t.dim, u->t.accent, 55) : u->t.dim;
			col = skin_mix(col, u->t.bg, fade[k]);
			if (side < 0)
				y = y0 - up[k];
			else
				y = y0 + (has_val ? 110 : 78) + dn[k];
			skin_text_c(u, -1, cx, y, buf, sc, w, col);
			if (idx == l->current) {   /* the one in force, when it is not under the cursor */
				int tw = x45_text_w(buf, sc);

				skin_round_rect(s, cx - (tw > w ? w : tw) / 2 - 20, y + 7 * sc / 2 - 4, 8, 8, 4, u->t.accent);
			}
		}
	}
	if (l->n > 1) {
		snprintf(pos, sizeof pos, "%d / %d", l->sel + 1, l->n);
		skin_text_r(u, s->w - 24, 74, pos, 2, 200, u->t.dim);
	}
}

/* settings: a column of rows with the name at the left and the value in a chip at the right, so that the two can
 * not be mixed up; the row under the cursor is a card with an accent chip, the ones around it fade away */
static void list_settings(struct ui *u, struct ui_list *l)
{
	struct x45_surface *s = &u->g.back;
	const int cx = s->w / 2, cw = 760, x0 = cx - cw / 2, x1 = cx + cw / 2;
	const int dy[4] = { 0, 78, 128, 170 };   /* distance of the rows from the cursor's, in px */
	int k, side;
	char pos[32];

	if (l->n == 0) {
		skin_list_msg(u, l, 0, 250, 1);
		return;
	}
	for (k = 3; k >= 0; k--) {
		for (side = -1; side <= 1; side += 2) {
			int idx, y, h, sc, vsc, chw, cl, sel = k == 0;
			struct ui_item it;
			uint32_t fg, bd;
			const char *val;

			if (k == 0 && side > 0)
				continue;
			if (k > 0 && l->n < 2 * k + 1)
				continue;   /* too short to show another one on this side without repeating one */
			idx = ((l->sel + side * k) % l->n + l->n) % l->n;   /* the knob wraps around the ends */
			memset(&it, 0, sizeof it);
			l->item(u, l->ctx, idx, &it);
			val = it.value[0] ? it.value : it.lead;
			h = sel ? 76 : k == 1 ? 50 : 42;
			y = 300 + side * dy[k] - h / 2;
			sc = sel ? 4 : 3;
			vsc = sel ? 4 : 3;
			fg = sel ? u->t.text : k == 1 ? u->t.dim : u->t.line;
			bd = sel ? u->t.accent : u->t.line;
			if (it.action && !sel)
				fg = k == 1 ? skin_mix(u->t.dim, u->t.bg, 40) : u->t.line;
			if (sel) {
				skin_round_rect(s, x0, y, cw, h, 16, skin_mix(u->t.bg, u->t.accent, 12));
				skin_round_outline(s, x0, y, cw, h, 16, 2, bd);
			}
			chw = 0;
			cl = x1 - 24;
			if (val[0]) {
				char buf[160];
				int tw;

				vsc = skin_text_scale_fit(val, cw / 2 - 40, vsc, 2);
				x45_ascii_fold(buf, sizeof buf, val);
				tw = x45_text_w(buf, vsc);
				if (tw > cw / 2 - 40)
					tw = cw / 2 - 40;
				chw = tw + 2 * (10 + vsc * 2);
				if (sel) {
					cl = x1 - 18 - chw;
					skin_round_rect(s, cl, y + (h - 7 * vsc - 16) / 2, chw, 7 * vsc + 16, (7 * vsc + 16) / 2, u->t.accent);
					x45_text_clipped(s, cl + 10 + vsc * 2, y + (h - 7 * vsc) / 2, buf, vsc, tw, u->t.bg);
				} else {
					int ch = 7 * vsc + 12;

					cl = x1 - 18 - chw;
					skin_round_outline(s, cl, y + (h - ch) / 2, chw, ch, ch / 2, 1, u->t.line);
					x45_text_clipped(s, cl + 10 + vsc * 2, y + (h - 7 * vsc) / 2, buf, vsc, tw, k == 1 ? u->t.dim : u->t.line);
				}
			}
			if (l->current >= 0 && idx == l->current)
				skin_round_rect(s, x0 + 14, y + h / 2 - 4, 8, 8, 4, u->t.accent);
			{
				int lx = x0 + (l->current >= 0 ? 36 : 28), lw = cl - 20 - lx;

				if (sel && l->scroll_sel)
					text_scroll(u, 3, lx, y + (h - 7 * sc) / 2, it.label, sc, lw, fg);
				else
					text_fold(s, lx, y + (h - 7 * sc) / 2, it.label, sc, lw, fg);
			}
		}
	}
	if (l->n > 1) {
		snprintf(pos, sizeof pos, "%d / %d", l->sel + 1, l->n);
		skin_text_r(u, s->w - 24, 74, pos, 2, 200, u->t.dim);
	}
}

static void list(struct ui *u, struct ui_list *l)
{
	skin_list_title(u, l, 1);
	if (l->kind == LK_ICONS)
		list_symbols(u, l);
	else if (l->kind == LK_SETTINGS)
		list_settings(u, l);
	else
		list_wheel(u, l);
	if (l->hint)
		draw_hints(u, l->hint);
}

/* ------------------------------------------------------------- now playing --- */

static void np(struct ui *u, struct ui_np *n)
{
	struct x45_surface *s = &u->g.back;
	int x0 = 24, x1 = s->w - 24, cx, w, y = 76, sc;
	char line[128];

	if (n->slide) {   /* the picture on the left, the text centred in what is left */
		x45_image_draw(s, n->slide, 24, 84);
		x0 = 24 + n->slide->w + 24;
	}
	cx = (x0 + x1) / 2;
	w = x1 - x0;
	if (n->kicker)
		skin_text_c(u, -1, cx, y, n->kicker, 2, w, u->t.dim);
	y += 38;
	if (n->big_title) {
		skin_text_c(u, -1, cx, y - 6, n->title, skin_text_scale_fit(n->title, w, 9, 5), w, u->t.text);
		y += 80;
	} else {
		sc = skin_text_scale_fit(n->title, w, 5, 3);
		skin_text_c(u, 0, cx, y, n->title, sc, w, u->t.text);
		y += 7 * sc + 16;
	}
	if (n->sub[0]) {
		skin_text_c(u, 1, cx, y, n->sub, 3, w, u->t.accent);
		y += 36;
	}
	if (n->extra[0]) {
		skin_text_c(u, 2, cx, y, n->extra, 2, w, u->t.dim);
		y += 28;
	}
	if (n->fmt[0])
		skin_text_c(u, -1, cx, y, n->fmt, 2, w, u->t.dim);
	y = 292;
	if (n->frac >= 0) {
		int bw = w > 700 ? 640 : w - 48, bx = cx - bw / 2;

		x45_fill_rect(s, bx, y, bw, 6, u->t.barbg);
		x45_fill_rect(s, bx, y, (int)(bw * (n->frac > 1 ? 1 : n->frac)), 6, u->t.accent);
		y += 20;
	}
	if (n->time[0])
		skin_text_c(u, -1, cx, y, n->time, 3, w, u->t.text);
	y = 352;
	snprintf(line, sizeof line, "%s%s%s", n->state, n->flags[0] && n->state[0] ? "     " : "", n->flags);
	skin_text_c(u, -1, cx, y, line, 2, w, n->playing ? u->t.accent : u->t.dim);
	if (n->msg)
		skin_text_c(u, -1, cx, y + 32, n->msg, 2, w, n->msg_warn ? u->t.warn : u->t.dim);
	draw_visuals_rect(u, 12, 410, s->w - 24, s->h - 12 - 410);
}

const struct ui_skin skin_carousel = {
	.name = "carousel",
	.label = "Carousel",
	.about = "One thing at a time: inputs as a row of symbols, lists as a wheel, a poster for what plays",
	.bg = { 5, 6, 9 }, .panel = { 5, 6, 9 }, .line = { 44, 48, 62 },
	.text = { 242, 244, 250 }, .dim = { 118, 126, 146 }, .barbg = { 28, 31, 40 }, .card = { 14, 16, 22 },
	.tint = 10,
	.rows = 7,
	.bar = bar,
	.list = list,
	.np = np,
	.hints = hints,
};
