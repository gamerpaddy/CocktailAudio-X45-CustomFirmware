#define _GNU_SOURCE
/* ext - registry of the screens in this folder, and the drawing helpers they share.
 * To add a screen: write ext/NAME.c with a `const struct ui_ext ext_NAME`, then list it below. */
#include "ext.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern const struct ui_ext ext_wifi;
extern const struct ui_ext ext_search;
extern const struct ui_ext ext_balance;
extern const struct ui_ext ext_art;
extern const struct ui_ext ext_btout;
extern const struct ui_ext ext_clock;
extern const struct ui_ext ext_sleep;
extern const struct ui_ext ext_songlog;
extern const struct ui_ext ext_path;
extern const struct ui_ext ext_heartbeat;
extern const struct ui_ext ext_childlock;

static const struct ui_ext *const all[] = {
	&ext_wifi,
	&ext_search,
	&ext_balance,
	&ext_art,
	&ext_btout,
	&ext_clock,
	&ext_sleep,
	&ext_songlog,
	&ext_path,
	&ext_heartbeat,
	&ext_childlock,
};
#define NEXT ((int)(sizeof all / sizeof *all))

static const struct ui_ext *open_ext;   /* the one whose enter() ran and whose leave() has not */
static enum screen ret_screen;
static int ret_sel;

/* Counts from the start of the process, at 60 s (see now_ms in main.c): a timer that is still 0 reads as long ago. */
long ext_now_ms(void)
{
	static long base_s;
	static int have_base;
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	if (!have_base) {
		base_s = (long)ts.tv_sec - 60;
		have_base = 1;
	}
	return ((long)ts.tv_sec - base_s) * 1000L + ts.tv_nsec / 1000000L;
}

/* The age of a stamp. The clock is a 32-bit long: a stamp that is 0, or was taken more than 24.8 days ago, differs from
 * now by more than 2^31 and reads as negative. A difference below -1 h is such an old stamp (nothing is scheduled that far
 * ahead with it), and it is long ago. */
long ext_since(long now, long t)
{
	long d = (long)((unsigned long)now - (unsigned long)t);

	return d < -3600000L ? LONG_MAX : d;
}

/* ------------------------------------------------------------------ routing --- */

static void left(struct ui *u)
{
	const struct ui_ext *e = open_ext;

	open_ext = NULL;
	if (e && e->leave)
		e->leave(u);
}

void ui_ext_open(struct ui *u, const struct ui_ext *e)
{
	if (!e || !e->draw)   /* a service without a screen (art, heartbeat): there is nothing to show, and no title to draw */
		return;
	if (u->screen != SCR_EXT) {
		ret_screen = u->screen;
		ret_sel = u->sel;
	}
	left(u);
	u->ext = e;
	u->ext_ret = ret_screen;   /* a screen can look at what it was opened from (the search takes the radio list it came from) */
	enter_screen(u, SCR_EXT);
	open_ext = e;
	if (e->enter)
		e->enter(u);
	u->dirty = 1;
}

int ui_ext_open_name(struct ui *u, const char *name)
{
	int i;

	for (i = 0; i < NEXT; i++)
		if (!strcmp(all[i]->name, name) && all[i]->draw) {
			ui_ext_open(u, all[i]);
			return 0;
		}
	return -1;
}

void ui_ext_close(struct ui *u)
{
	left(u);
	u->ext = NULL;
	enter_screen(u, ret_screen == SCR_EXT ? SCR_PLAYER : ret_screen);
	if (ret_screen == SCR_OPTIONS || ret_screen == SCR_OPTCAT || ret_screen == SCR_VISUALS)
		u->sel = ret_sel;   /* a list stays on the entry it was opened from */
}

void ui_ext_draw(struct ui *u)
{
	if (u->ext && u->ext->draw)
		u->ext->draw(u);
}

void ui_ext_ok(struct ui *u)
{
	if (u->ext && u->ext->ok)
		u->ext->ok(u);
	u->dirty = 1;
}

void ui_ext_back(struct ui *u)
{
	if (u->ext && u->ext->back)
		u->ext->back(u);
	else
		ui_ext_close(u);
	u->dirty = 1;
}

void ui_ext_move(struct ui *u, int delta)
{
	if (u->ext && u->ext->move)
		u->ext->move(u, delta);
	u->dirty = 1;
}

void ui_ext_nav(struct ui *u, int dx, int dy)
{
	if (u->ext && u->ext->nav)
		u->ext->nav(u, dx, dy);
	else if (dy)
		ui_ext_move(u, dy);
	u->dirty = 1;
}

int ui_ext_ir(struct ui *u, int code)
{
	int used = u->ext && u->ext->ir && u->ext->ir(u, code);

	if (used)
		u->dirty = 1;
	return used;
}

int ui_ext_wants_hold(const struct ui *u)
{
	return u->ext && u->ext->hold;
}

int ui_ext_hold(struct ui *u, int code)
{
	int used = u->ext && u->ext->hold && u->ext->hold(u, code);

	if (used)
		u->dirty = 1;
	return used;
}

void ui_ext_poll(struct ui *u)
{
	int i;

	/* the screen was left by a way that does not come through here (HOME, MENU hold, the web) */
	if (open_ext && u->screen != SCR_EXT)
		left(u);
	for (i = 0; i < NEXT; i++)
		if (all[i]->poll)
			all[i]->poll(u);
}

int ui_ext_badges(struct ui *u, int right)
{
	int i, w;

	for (i = 0; i < NEXT; i++)
		if (all[i]->badge && (w = all[i]->badge(u, right)) > 0)
			right -= w + 20;
	return right;
}

void ui_ext_state(struct ui *u, FILE *f)
{
	int i;

	for (i = 0; i < NEXT; i++)
		if (all[i]->json) {
			fprintf(f, ",\"%s\":", all[i]->name);
			all[i]->json(u, f);
		}
}

int ui_ext_cmd(struct ui *u, const char *name, const char *args)
{
	int i;

	for (i = 0; i < NEXT; i++)
		if (all[i]->cmd && !strcmp(all[i]->name, name) && all[i]->cmd(u, args)) {
			u->dirty = 1;
			return 1;
		}
	return 0;
}

int ui_ext_filter(struct ui *u, const struct x45_event *ev)
{
	int i;

	for (i = 0; i < NEXT; i++)
		if (all[i]->filter && all[i]->filter(u, ev))
			return 1;
	return 0;
}

int ext_conf_get(const char *key, char *out, size_t cap)
{
	char line[1100];
	size_t kl = strlen(key);
	FILE *f = fopen("/home/root/x45new/etc/x45ui.conf", "r");
	int found = 0;

	if (!f)
		return 0;
	while (fgets(line, sizeof line, f)) {
		char *k = line, *v, *e;

		while (*k == ' ' || *k == '\t')
			k++;
		if (*k == '#' || strncmp(k, key, kl))
			continue;
		v = k + kl;
		while (*v == ' ' || *v == '\t')
			v++;
		if (*v != '=')
			continue;
		v++;
		while (*v == ' ' || *v == '\t')
			v++;
		e = v + strcspn(v, "\r\n");
		while (e > v && (e[-1] == ' ' || e[-1] == '\t'))
			e--;
		*e = '\0';
		snprintf(out, cap, "%s", v);
		found = 1;   /* the last one wins, as in main.c */
	}
	fclose(f);
	return found;
}

int ext_conf_int(const char *key, int def)
{
	char v[32];

	return ext_conf_get(key, v, sizeof v) ? atoi(v) : def;
}

int ui_ext_menu_count(void)
{
	int i, n = 0;

	for (i = 0; i < NEXT; i++)
		n += all[i]->menu != NULL;
	return n;
}

const struct ui_ext *ui_ext_menu_at(int k)
{
	int i;

	for (i = 0; i < NEXT; i++)
		if (all[i]->menu && k-- == 0)
			return all[i];
	return NULL;
}

/* ------------------------------------------------------------------ drawing --- */

uint32_t ext_fg(const struct ui *u, int selected)
{
	return selected ? u->t.bg : u->t.text;
}

int ext_list(struct ui *u, int x, int y, int w, int h, int n, int sel, int *top, ext_row_fn row, void *ctx)
{
	struct x45_surface *s = &u->g.back;
	int vis = h / EXT_ROW_H, i;

	if (vis < 1)
		vis = 1;
	if (n <= vis) {
		*top = 0;
	} else {
		if (sel >= 0 && sel < *top)
			*top = sel;
		if (sel >= *top + vis)
			*top = sel - vis + 1;
		if (*top > n - vis)
			*top = n - vis;
		if (*top < 0)
			*top = 0;
	}
	for (i = 0; i < vis && *top + i < n; i++) {
		int idx = *top + i, ry = y + i * EXT_ROW_H;

		if (idx == sel)
			x45_fill_rect(s, x, ry, w, EXT_ROW_H - 2, u->t.accent);
		row(u, idx, x, ry, w, idx == sel, ctx);
	}
	if (n > vis) {
		int th = vis * EXT_ROW_H, bh = th * vis / n, by = y + th * *top / n;

		if (bh < 12)
			bh = 12;
		x45_fill_rect(s, x + w - 3, y, 1, th, u->t.line);
		x45_fill_rect(s, x + w - 5, by, 5, bh, u->t.dim);
	}
	return vis;
}

int ext_para(struct ui *u, int x, int y, int w, const char *text, int scale, uint32_t col)
{
	struct x45_surface *s = &u->g.back;
	char line[160];
	const char *p = text;
	int lh = x45_text_h(scale) + 2 * scale;

	while (*p) {
		int len = 0, last_sp = -1;

		if (*p == '\n') {   /* an empty line: nothing to draw, and no stale text in `line` */
			y += lh;
			p++;
			continue;
		}
		/* the longest run of words that fits */
		while (p[len] && p[len] != '\n' && len < (int)sizeof line - 1) {
			line[len] = p[len];
			line[len + 1] = '\0';
			if (p[len] == ' ')
				last_sp = len;
			if (x45_text_w(line, scale) > w) {
				if (last_sp > 0)
					len = last_sp;
				break;
			}
			len++;
		}
		if (len == 0)
			len = 1;   /* one glyph wider than the area: draw it anyway */
		line[len] = '\0';
		x45_text(s, x, y, line, scale, col);
		y += lh;
		p += len;
		while (*p == ' ')
			p++;
		if (*p == '\n')
			p++;
	}
	return y;
}

void ext_bars(struct ui *u, int x, int y, int lvl, uint32_t on, uint32_t off)
{
	int b;

	for (b = 0; b < 4; b++) {
		int bh = 6 + b * 5;

		x45_fill_rect(&u->g.back, x + b * 7, y + 22 - bh, 5, bh, b < lvl ? on : off);
	}
}
