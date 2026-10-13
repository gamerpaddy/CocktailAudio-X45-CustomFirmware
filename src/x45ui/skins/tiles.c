#define _GNU_SOURCE
/* tiles - a light skin made of cards.
 *
 * Lists become grids: the inputs are a board of tiles with their symbols, names and folders are cards in two
 * columns, settings are wide cards with the value in a pill. The now-playing page is split in two: the words on
 * the left, on the right a card with a ring that fills with the progress, and the visuals in a card of their own
 * along the foot.
 */
#include "skin.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static void bar(struct ui *u, const char *title)
{
	struct x45_surface *s = &u->g.back;

	x45_fill_rect(s, 0, 0, s->w, 56, u->t.panel);
	x45_fill_rect(s, 0, 52, s->w, 4, u->t.accent);
	x45_text(s, 24, 16, title, 3, u->t.text);
}

/* the keys as pills: the key on a card, the action after it */
static void hints(struct ui *u, const char *spec)
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
		kw = x45_text_w(p, 2) + 26;
		aw = *act ? x45_text_w(act, 2) : 0;
		if (x + kw + 10 + aw > s->w - 12)
			break;
		skin_round_rect(s, x, y, kw, 30, 15, u->t.card);
		skin_round_outline(s, x, y, kw, 30, 15, 1, u->t.line);
		x45_text(s, x + 13, y + 8, p, 2, u->t.text);
		if (aw)
			x45_text(s, x + kw + 10, y + 8, act, 2, u->t.dim);
		x += kw + 10 + aw + 22;
	}
}

/* ------------------------------------------------------------------- lists --- */

/* a card, filled with the accent when it is the one under the cursor */
static void card(struct ui *u, int x, int y, int w, int h, int sel)
{
	struct x45_surface *s = &u->g.back;

	if (sel) {
		skin_round_rect(s, x, y, w, h, 12, u->t.accent);
	} else {
		skin_round_rect(s, x, y, w, h, 12, u->t.card);
		skin_round_outline(s, x, y, w, h, 12, 1, u->t.line);
	}
}

static void list_board(struct ui *u, struct ui_list *l)
{
	struct x45_surface *s = &u->g.back;
	const int cols = 4, tw = 232, th = 132, gap = 16, x0 = (s->w - (cols * tw + (cols - 1) * gap)) / 2;
	const int y0 = l->title ? 96 : 76, rows_fit = 3;
	int selrow = l->sel / cols, first = selrow < rows_fit ? 0 : selrow - rows_fit + 1, i;

	for (i = first * cols; i < l->n && i < (first + rows_fit) * cols; i++) {
		int col = i % cols, row = i / cols - first, x = x0 + col * (tw + gap), y = y0 + row * (th + gap), sel = i == l->sel;
		struct ui_item it;
		uint32_t fg;

		memset(&it, 0, sizeof it);
		l->item(u, l->ctx, i, &it);
		fg = sel ? u->t.bg : u->t.text;
		card(u, x, y, tw, th, sel);
		skin_icon(u, it.icon, x + (tw - 56) / 2, y + 20, 56, sel ? u->t.bg : u->t.accent);
		skin_text_c(u, -1, x + tw / 2, y + th - 36, it.label, 2, tw - 24, fg);
		if (i == l->current)
			skin_round_rect(s, x + tw - 26, y + 12, 12, 12, 6, sel ? u->t.bg : u->t.accent);
	}
}

static void list_cards(struct ui *u, struct ui_list *l)
{
	struct x45_surface *s = &u->g.back;
	const int cols = 2, cw = 488, ch = 52, gap = 10, x0 = (s->w - (cols * cw + gap)) / 2, y0 = l->title ? 96 : 80;
	const int per = cols * 6;
	int first, i, pages = (l->n + per - 1) / per;
	char note[24];

	if (l->n == 0) {
		skin_list_msg(u, l, 32, y0 + 6, 0);
		return;
	}
	first = (l->sel / per) * per;
	if (l->top)
		*l->top = first;
	for (i = first; i < l->n && i < first + per; i++) {
		int k = i - first, x = x0 + (k % cols) * (cw + gap), y = y0 + (k / cols) * (ch + gap), sel = i == l->sel;
		int tx = x + 18, avail = cw - 36;
		struct ui_item it;
		uint32_t fg;
		char buf[170];

		memset(&it, 0, sizeof it);
		l->item(u, l->ctx, i, &it);
		fg = it.action ? (sel ? u->t.bg : u->t.dim) : sel ? u->t.bg : u->t.text;
		card(u, x, y, cw, ch, sel);
		if (it.dir) {   /* a folder: its tab */
			uint32_t fc = sel ? u->t.bg : u->t.accent;

			skin_round_rect(s, tx, y + 19, 12, 5, 2, fc);
			skin_round_rect(s, tx, y + 22, 24, 14, 3, fc);
			tx += 36;
			avail -= 36;
		}
		if (it.lead[0] && it.label[0])
			snprintf(buf, sizeof buf, "%s  %s", it.lead, it.label);
		else
			snprintf(buf, sizeof buf, "%s", it.lead[0] ? it.lead : it.label);
		if (i == l->current) {   /* the one that is in force */
			skin_round_rect(s, x + cw - 30, y + (ch - 12) / 2, 12, 12, 6, sel ? u->t.bg : u->t.accent);
			avail -= 28;
		}
		if (it.mark[0]) {
			skin_text_r(u, x + cw - 16 - (i == l->current ? 28 : 0), y + 16, it.mark, 3, 60, sel ? u->t.bg : u->t.accent);
			avail -= 50;
		}
		if (sel && l->scroll_sel)
			text_scroll(u, 3, tx, y + 16, buf, 3, avail, fg);
		else
			text_fold(s, tx, y + 16, buf, 3, avail, fg);
	}
	if (pages > 1) {
		snprintf(note, sizeof note, "page %d / %d", first / per + 1, pages);
		skin_text_r(u, s->w - 24, 68, note, 2, 160, u->t.dim);
	}
}

/* labels with values: a card each, the value in a pill at the right */
static void list_settings(struct ui *u, struct ui_list *l)
{
	struct x45_surface *s = &u->g.back;
	const int y0 = l->title ? 96 : 80, gap = 8, x = 24, w = s->w - 48;
	int ch = l->n ? (440 - gap * (l->n - 1)) / l->n : 50, i;

	if (ch > 56)
		ch = 56;
	if (ch < 36)
		ch = 36;
	if (l->n == 0)
		skin_list_msg(u, l, 32, y0, 0);
	for (i = 0; i < l->n; i++) {
		int y = y0 + i * (ch + gap), sel = i == l->sel, ty = y + (ch - 21) / 2;
		struct ui_item it;
		int avail = w - 48;

		memset(&it, 0, sizeof it);
		l->item(u, l->ctx, i, &it);
		card(u, x, y, w, ch, sel);
		if (l->kind == LK_SETTINGS && l->current >= 0 && i == l->current)
			skin_round_rect(s, x + 14, y + ch / 2 - 6, 12, 12, 6, sel ? u->t.bg : u->t.accent);
		if (it.value[0]) {
			int vw = x45_text_w(it.value, 3), vmax = w / 2;

			if (vw > vmax)
				vw = vmax;
			skin_round_rect(s, x + w - vw - 44, y + (ch - 34) / 2, vw + 28, 34, 17, sel ? u->t.bg : u->t.panel);
			x45_text_clipped(s, x + w - vw - 30, ty, it.value, 3, vw, sel ? u->t.accent : u->t.text);
			avail -= vw + 40;
		}
		text_fold(s, x + (l->current >= 0 ? 40 : 22), ty, it.label, 3, avail, sel ? u->t.bg : u->t.text);
	}
}

static void list(struct ui *u, struct ui_list *l)
{
	skin_list_title(u, l, 0);
	if (l->kind == LK_ICONS)
		list_board(u, l);
	else if (l->kind == LK_SETTINGS)
		list_settings(u, l);
	else
		list_cards(u, l);
	if (l->hint)
		draw_hints(u, l->hint);
}

/* ------------------------------------------------------------- now playing --- */

/* a ring of radius r and thickness th around (cx, cy), filled clockwise from the top up to `frac` */
static void ring(struct ui *u, int cx, int cy, int r, int th, double frac, uint32_t on, uint32_t off, uint32_t back)
{
	struct x45_surface *s = &u->g.back;
	int dx, dy;

	for (dy = -r - 1; dy <= r + 1; dy++)
		for (dx = -r - 1; dx <= r + 1; dx++) {
			float d = sqrtf((float)(dx * dx + dy * dy)), cov;
			float a;
			uint32_t c;

			if (d > (float)r + 1 || d < (float)(r - th) - 1)
				continue;
			cov = (float)r + 0.5f - d;
			if (d - (float)(r - th) < 1)
				cov = d - (float)(r - th) + 0.5f;
			if (cov <= 0)
				continue;
			if (cov > 1)
				cov = 1;
			a = atan2f((float)dx, (float)-dy) / 6.2831853f;
			if (a < 0)
				a += 1;
			c = frac >= 0 && a <= frac ? on : off;
			x45_put(s, cx + dx, cy + dy, skin_mix(back, c, (int)(cov * 100)));
		}
}

static void np(struct ui *u, struct ui_np *n)
{
	struct x45_surface *s = &u->g.back;
	const int lx = 24, lw = 570, rx = 620, ry = 76, rw = 380, rh = 304;
	int y = 76, sc, kw;
	char big[48], small[48] = "", line[128], *sl;

	/* the words */
	if (n->kicker) {
		kw = x45_text_w(n->kicker, 2) + 28;
		skin_round_rect(s, lx, y, kw, 30, 15, u->t.card);
		skin_round_outline(s, lx, y, kw, 30, 15, 1, u->t.line);
		x45_text(s, lx + 14, y + 8, n->kicker, 2, u->t.accent);
	}
	y += 52;
	if (n->big_title) {
		x45_text(s, lx, y, n->title, 7, u->t.text);
		y += 72;
	} else {
		sc = skin_text_scale_fit(n->title, lw, 4, 3);
		text_scroll(u, 0, lx, y, n->title, sc, lw, u->t.text);
		y += 7 * sc + 14;
	}
	if (n->sub[0]) {
		text_scroll(u, 1, lx, y, n->sub, 3, lw, u->t.accent);
		y += 36;
	}
	if (n->extra[0]) {
		text_scroll(u, 2, lx, y, n->extra, 2, lw, u->t.dim);
		y += 28;
	}
	if (n->fmt[0])
		text_fold(s, lx, y + 8, n->fmt, 2, lw, u->t.text);
	snprintf(line, sizeof line, "%s%s%s", n->state, n->flags[0] && n->state[0] ? "     " : "", n->flags);
	text_fold(s, lx, 316, line, 2, lw, n->playing ? u->t.accent : u->t.dim);
	if (n->msg)
		text_fold(s, lx, 346, n->msg, 2, lw, n->msg_warn ? u->t.warn : u->t.dim);

	/* the card on the right: the picture, or the ring with the time in it */
	skin_round_rect(s, rx, ry, rw, rh, 18, u->t.card);
	skin_round_outline(s, rx, ry, rw, rh, 18, 1, u->t.line);
	if (n->slide) {
		x45_image_draw(s, n->slide, rx + (rw - n->slide->w) / 2, ry + (rh - n->slide->h) / 2);
	} else {
		int cx = rx + rw / 2, cy = ry + rh / 2 - 8;

		ring(u, cx, cy, 104, 12, n->frac < 0 && n->playing ? 1 : n->frac, u->t.accent, u->t.barbg, u->t.card);   /* on air: a full ring */
		snprintf(big, sizeof big, "%s", n->time[0] ? n->time : n->playing ? "LIVE" : "--");
		sl = strstr(big, " / ");
		if (sl) {
			snprintf(small, sizeof small, "%s", sl + 3);
			*sl = '\0';
		}
		if (strlen(big) == 19 && big[4] == '-' && big[10] == ' ') {   /* the DAB clock "2026-10-08 17:07:05": the time, the date under it */
			snprintf(small, sizeof small, "%.10s", big);
			memmove(big, big + 11, 9);
		}
		if (!strncmp(big, "LIVE", 4) && big[4]) {   /* "LIVE   3:53": the words above, the clock in the ring */
			char *clock = big + 4;

			while (*clock == ' ')
				clock++;
			snprintf(small, sizeof small, "LIVE");
			memmove(big, clock, strlen(clock) + 1);
		}
		sc = skin_text_scale_fit(big, 150, 5, 2);
		skin_text_c(u, -1, cx, cy - 7 * sc / 2 - (small[0] ? 8 : 0), big, sc, 150, u->t.text);
		if (small[0])
			skin_text_c(u, -1, cx, cy + 7 * sc / 2 + 2, small, 2, 150, u->t.dim);
	}

	/* the visuals in a card of their own */
	skin_round_rect(s, 12, 392, s->w - 24, s->h - 12 - 392, 14, u->t.card);
	draw_visuals_rect(u, 24, 400, s->w - 48, s->h - 12 - 400 - 8);
}

const struct ui_skin skin_tiles = {
	.name = "tiles",
	.label = "Tiles",
	.about = "Light cards: inputs as a board of tiles, a progress ring beside the words",
	.bg = { 238, 235, 228 }, .panel = { 224, 220, 210 }, .line = { 196, 190, 178 },
	.text = { 32, 34, 42 }, .dim = { 112, 110, 104 }, .barbg = { 214, 209, 198 }, .card = { 250, 248, 243 },
	.tint = 5,
	.accent_pct = 62,
	.rows = 12,
	.bar = bar,
	.list = list,
	.np = np,
	.hints = hints,
};
