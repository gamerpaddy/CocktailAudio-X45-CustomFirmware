#define _GNU_SOURCE
#include "osk.h"

#include <stdio.h>
#include <string.h>

#define NCH 40      /* character keys */
#define NFN 6       /* function keys: shift, symbols, space, delete, show, done */
#define KEY_SHIFT  (NCH + 0)
#define KEY_SYM    (NCH + 1)
#define KEY_SPACE  (NCH + 2)
#define KEY_DEL    (NCH + 3)
#define KEY_SHOW   (NCH + 4)
#define KEY_DONE   (NCH + 5)

static const char *const layers[3] = {
	"qwertyuiop" "asdfghjkl-" "zxcvbnm,._" "1234567890",
	"QWERTYUIOP" "ASDFGHJKL-" "ZXCVBNM,._" "1234567890",
	"!@#$%^&*()" "=+[]{};:/?" "'\"`~<>|\\-_" "1234567890",
};

void osk_open(struct osk *o, const char *prompt, const char *initial, int max, int show)
{
	memset(o, 0, sizeof *o);
	snprintf(o->prompt, sizeof o->prompt, "%s", prompt);
	o->max = max > 0 && max < OSK_MAX ? max : OSK_MAX - 1;
	if (initial)
		snprintf(o->buf, sizeof o->buf, "%.*s", o->max, initial);
	o->len = (int)strlen(o->buf);
	o->show = show;
}

void osk_move(struct osk *o, int delta)
{
	int n = NCH + NFN;

	o->cur = ((o->cur + delta) % n + n) % n;
}

static void rowcol(int cur, int *row, int *col)
{
	if (cur < NCH) {
		*row = cur / 10;
		*col = cur % 10;
	} else {
		*row = 4;
		*col = cur - NCH;
	}
}

void osk_nav(struct osk *o, int dx, int dy)
{
	int row, col;

	rowcol(o->cur, &row, &col);
	if (dx) {
		int w = row == 4 ? NFN : 10;

		col = ((col + dx) % w + w) % w;
	}
	if (dy) {
		int nr = ((row + dy) % 5 + 5) % 5;

		/* the function row has six wide keys under ten narrow ones */
		if (nr == 4 && row != 4)
			col = col * NFN / 10;
		else if (nr != 4 && row == 4)
			col = col * 10 / NFN + 10 / NFN / 2;
		if (nr != 4 && col > 9)
			col = 9;
		row = nr;
	}
	o->cur = row == 4 ? NCH + col : row * 10 + col;
}

static void put(struct osk *o, char c)
{
	if (o->len < o->max) {
		o->buf[o->len++] = c;
		o->buf[o->len] = '\0';
	}
}

int osk_press(struct osk *o)
{
	switch (o->cur) {
	case KEY_SHIFT:
		o->layer = o->layer == 0 ? 1 : 0;
		break;
	case KEY_SYM:
		o->layer = o->layer == 2 ? 0 : 2;
		break;
	case KEY_SPACE:
		put(o, ' ');
		break;
	case KEY_DEL:
		if (o->len > 0)
			o->buf[--o->len] = '\0';
		break;
	case KEY_SHOW:
		o->show = !o->show;
		break;
	case KEY_DONE:
		return OSK_DONE;
	default:
		put(o, layers[o->layer][o->cur]);
		break;
	}
	return OSK_EDIT;
}

void osk_draw(struct ui *u, struct osk *o)
{
	struct x45_surface *s = &u->g.back;
	static const char *const fn_lbl[NFN] = { "Aa", "#+=", "space", "del", "show", "done" };
	static const int fn_w[NFN] = { 116, 116, 292, 116, 116, 156 };
	char shown[OSK_MAX + 1];
	const int kx = 32, ky = 196, kw = 88, kh = 56, gap = 8;
	int i, x, y, tw;
	const char *t = shown;

	x45_text(s, 32, 76, o->prompt, 3, u->t.dim);

	/* the text box: the end of the text, with a block cursor */
	x45_fill_rect(s, 32, 112, s->w - 64, 60, u->t.panel);
	x45_draw_rect(s, 32, 112, s->w - 64, 60, u->t.line);
	if (o->show) {
		snprintf(shown, sizeof shown, "%s", o->buf);
	} else {
		for (i = 0; i < o->len; i++)
			shown[i] = '*';
		shown[o->len] = '\0';
	}
	while (*t && x45_text_w(t, 4) > s->w - 64 - 52)
		t++;
	tw = x45_text_w(t, 4);
	x45_text(s, 46, 128, t, 4, u->t.text);
	x45_fill_rect(s, 46 + tw + 2, 126, 14, 30, u->t.accent);

	for (i = 0; i < NCH; i++) {
		int sel = i == o->cur;
		char c[2] = { layers[o->layer][i], 0 };

		x = kx + (i % 10) * (kw + gap);
		y = ky + (i / 10) * (kh + gap);
		x45_fill_rect(s, x, y, kw, kh, sel ? u->t.accent : u->t.panel);
		x45_draw_rect(s, x, y, kw, kh, u->t.line);
		x45_text(s, x + (kw - x45_text_w(c, 3)) / 2, y + (kh - x45_text_h(3)) / 2, c, 3, sel ? u->t.bg : u->t.text);
	}
	y = ky + 4 * (kh + gap);
	x = kx;
	for (i = 0; i < NFN; i++) {
		int sel = NCH + i == o->cur;
		const char *lbl = fn_lbl[i];
		uint32_t fg = sel ? u->t.bg : u->t.text;

		if (i == 0 && o->layer == 1)
			lbl = "aa";
		if (i == 1 && o->layer == 2)
			lbl = "abc";
		if (i == 4 && o->show)
			lbl = "hide";
		x45_fill_rect(s, x, y, fn_w[i], kh, sel ? u->t.accent : u->t.panel);
		x45_draw_rect(s, x, y, fn_w[i], kh, u->t.line);
		x45_text(s, x + (fn_w[i] - x45_text_w(lbl, 2)) / 2, y + (kh - x45_text_h(2)) / 2, lbl, 2,
			 i == 5 && !sel ? u->t.accent : fg);
		x += fn_w[i] + gap;
	}
	draw_hints(u, "scroll|key;OK|press;RETURN|cancel");
}
