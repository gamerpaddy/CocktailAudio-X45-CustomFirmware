#define _GNU_SOURCE
/* search - find a station by typing a piece of its name; the matches follow every letter.
 *
 * Typing "suns" lists "Sunshine live" and every other station with that in its name: the favourites and the stations
 * of the router, the DAB services or the FM presets, and, on the network input, the community radio directory (asked
 * through x45rb a moment after the typing stops, so the answer comes in while the list is already there).
 *
 * The keyboard is made for the knob: one row of keys that the knob turns through, and the row offers only the letters
 * that lead to a station (after "su" a few of them, not twenty-six; the cursor waits on the likeliest). "all" shows the
 * whole alphabet. The remote types with its number keys, the way a phone did (2: a b c ...), BACKSPACE deletes, CAP/NUM
 * makes the number keys type digits, SEARCH closes the screen. Down (or OK, after typing on the remote) goes to the
 * list, OK plays the station.
 *
 * Remote key codes are the Linux ones the receiver reports (docs/X45-REMOTE.md). */
#include "ext.h"
#include "srchcore.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define KEY_SEARCH 67
#define KEY_BKSP   472
#define KEY_MODE   373
#define KEY_NUM0   512

#define LOCAL_MAX 600
#define WEB_MAX   200
#define WEB_MIN_CHARS    3
#define WEB_DEBOUNCE_MS  650
#define WEB_TIMEOUT_MS   16000
#define TAP_MS           1100   /* the same number key within this long goes on to the next letter */
#define X45RB_DEFAULT    "/home/root/x45new/bin/x45rb"

struct hit {
	char name[72], info[28], norm[SRCH_NAME], url[256];
	int kind, val, rank, idx;
};

static struct hit loc[LOCAL_MAX], web[WEB_MAX];
static int nloc, nweb;
static const struct hit *shown[LOCAL_MAX + WEB_MAX];
static int nshown;

static int is_open, web_enabled;
static char typed[SRCH_MAXQ + 2];
static int qlen;
static long q_t;                         /* when the text last changed */
static int focus;                        /* 0: the keys, 1: the list */
static int sel, top;
static int numeric;                      /* CAP/NUM: the number keys type digits */
static int smart;                        /* the keys offer only what leads to a station */
static int from_remote;                  /* the text came from the number keys: OK goes to the list */
static int tap_key = -1, tap_idx;
static long tap_t;

enum { R_DEL = 128, R_CLEAR, R_ALL, R_LIST };
static unsigned char ring[64], ring_dim[64];
static int nring, cur;

enum { W_IDLE, W_RUN, W_DONE, W_FAIL };
static int wstate;
static pid_t wpid;
static int wfd = -1;
static char *wbuf;
static size_t wlen, wcap;
static char wq[SRCH_MAXQ + 2];           /* what the directory was asked (or is being asked) */
static long wt0;

/* ------------------------------------------------------------ the matches --- */

static int cmp_local(const void *a, const void *b)
{
	const struct hit *x = *(const struct hit *const *)a, *y = *(const struct hit *const *)b;

	if (x->rank != y->rank)
		return x->rank - y->rank;
	if (x->kind != y->kind)
		return x->kind - y->kind;
	if (x->kind == STN_FM || x->kind == STN_DAB)   /* the order the tuner has them in */
		return x->idx - y->idx;
	return strcmp(x->norm, y->norm);
}

static int cmp_web(const void *a, const void *b)
{
	const struct hit *x = *(const struct hit *const *)a, *y = *(const struct hit *const *)b;

	if (x->rank != y->rank)
		return x->rank - y->rank;
	return x->idx - y->idx;
}

static void trimmed(char *out, size_t cap)
{
	size_t n;

	snprintf(out, cap, "%s", typed);
	n = strlen(out);
	while (n > 0 && out[n - 1] == ' ')
		out[--n] = '\0';
}

/* the list below the text: own stations first (best match first), then the directory's */
static void rebuild(void)
{
	static const struct hit *lp[LOCAL_MAX], *wp[WEB_MAX];
	char t[SRCH_MAXQ + 2];
	int i, j, nl = 0, nw = 0;

	trimmed(t, sizeof t);
	for (i = 0; i < nloc; i++) {
		loc[i].rank = srch_rank(loc[i].norm, t);
		if (loc[i].rank)
			lp[nl++] = &loc[i];
	}
	for (i = 0; i < nweb; i++) {
		web[i].rank = t[0] ? srch_rank(web[i].norm, t) : 0;
		if (!web[i].rank)
			continue;
		for (j = 0; j < nl; j++)   /* a station of the lists is not listed twice */
			if (lp[j]->url[0] && !strcmp(lp[j]->url, web[i].url))
				break;
		if (j == nl)
			wp[nw++] = &web[i];
	}
	qsort(lp, (size_t)nl, sizeof lp[0], cmp_local);
	qsort(wp, (size_t)nw, sizeof wp[0], cmp_web);
	nshown = 0;
	for (i = 0; i < nl; i++)
		shown[nshown++] = lp[i];
	for (i = 0; i < nw; i++)
		shown[nshown++] = wp[i];
	if (sel >= nshown)
		sel = nshown ? nshown - 1 : 0;
}

/* ------------------------------------------------------------- the keys --- */

static int ring_has(int c)
{
	int i;

	for (i = 0; i < nring; i++)
		if (ring[i] == c)
			return i;
	return -1;
}

/* The row of keys. `jump`: the text just changed, so the cursor goes to the letter that is likeliest to come next;
 * otherwise it stays on the key it was on. */
static void ring_build(int jump)
{
	int cnt[128], i, total = 0, best = -1, bestn = 0, prev = nring ? ring[cur] : -1, space_ok;

	memset(cnt, 0, sizeof cnt);
	for (i = 0; i < nloc; i++)
		srch_next(loc[i].norm, typed, cnt);
	for (i = 0; i < nweb; i++)
		srch_next(web[i].norm, typed, cnt);
	for (i = 0; i < 128; i++)
		total += cnt[i];
	space_ok = qlen > 0 && typed[qlen - 1] != ' ';
	nring = 0;
#define ADD(ch, dim) do { ring[nring] = (unsigned char)(ch); ring_dim[nring] = (unsigned char)(dim); nring++; } while (0)
	for (i = 'a'; i <= 'z'; i++)
		if (cnt[i] > 0 || !smart || total == 0)
			ADD(i, cnt[i] == 0);
	for (i = '0'; i <= '9'; i++)
		if (cnt[i] > 0 || !smart || total == 0)
			ADD(i, cnt[i] == 0);
	if (space_ok && (cnt[' '] > 0 || !smart || total == 0))
		ADD(' ', cnt[' '] == 0);
	if (qlen > 0) {
		ADD(R_DEL, 0);
		ADD(R_CLEAR, 0);
	}
	if (total > 0)
		ADD(R_ALL, 0);
	if (nshown > 0)
		ADD(R_LIST, 0);
#undef ADD
	if (jump) {
		for (i = 0; i < nring; i++)
			if (ring[i] < 128 && cnt[ring[i]] > bestn) {
				bestn = cnt[ring[i]];
				best = i;
			}
		cur = best >= 0 ? best : (qlen > 0 && ring_has(R_DEL) >= 0 ? ring_has(R_DEL) : 0);
	} else {
		cur = prev >= 0 && ring_has(prev) >= 0 ? ring_has(prev) : (cur < nring ? cur : 0);
	}
	if (cur >= nring)
		cur = 0;
}

static void changed(int jump)
{
	q_t = ext_now_ms();
	sel = top = 0;
	rebuild();
	ring_build(jump);
}

/* returns 1 when a character was added */
static int type_char(int c)
{
	if (c == ' ' && (qlen == 0 || typed[qlen - 1] == ' '))
		return 0;
	if (qlen >= SRCH_MAXQ)
		return 0;
	typed[qlen++] = (char)c;
	typed[qlen] = '\0';
	changed(1);
	return 1;
}

static void del_char(void)
{
	if (qlen > 0) {
		typed[--qlen] = '\0';
		changed(1);
	}
}

static void clear_text(void)
{
	typed[0] = '\0';
	qlen = 0;
	changed(1);
}

/* ---------------------------------------------------------- the directory --- */

static void web_stop(void)
{
	if (wpid > 0) {
		kill(-wpid, SIGKILL);   /* x45rb, the shell and curl it starts: one group */
		kill(wpid, SIGKILL);
		waitpid(wpid, NULL, 0);
	}
	wpid = 0;
	if (wfd >= 0)
		close(wfd);
	wfd = -1;
	wlen = 0;
}

static void web_start(const char *q)
{
	int p[2];
	pid_t pid;
	const char *bin = getenv("X45RB") ? getenv("X45RB") : X45RB_DEFAULT;

	if (pipe2(p, O_CLOEXEC) < 0)
		return;
	pid = fork();
	if (pid < 0) {
		close(p[0]);
		close(p[1]);
		return;
	}
	if (pid == 0) {
		setpgid(0, 0);
		dup2(p[1], 1);
		dup2(p[1], 2);   /* "the radio directory did not answer" comes through the same pipe */
		execl(bin, "x45rb", "search", q, (char *)NULL);
		_exit(127);
	}
	setpgid(pid, pid);
	close(p[1]);
	fcntl(p[0], F_SETFL, fcntl(p[0], F_GETFL) | O_NONBLOCK);
	wpid = pid;
	wfd = p[0];
	wlen = 0;
	wt0 = ext_now_ms();
	wstate = W_RUN;
	snprintf(wq, sizeof wq, "%s", q);
}

static void web_parse(void)
{
	char *line = wbuf, *end;
	int failed = 0;

	nweb = 0;
	if (!wbuf) {   /* the tool printed nothing (it is missing, or died at once): that is an answer too */
		wstate = W_FAIL;
		return;
	}
	wbuf[wlen] = '\0';
	for (; line && *line; line = end ? end + 1 : NULL) {
		char *url, *name, *tail;
		struct hit *h;

		end = strchr(line, '\n');
		if (end)
			*end = '\0';
		if (strstr(line, "did not answer"))
			failed = 1;
		if (line[0] != 'S' || line[1] != '\t' || nweb >= WEB_MAX)
			continue;
		url = line + 2;
		name = strchr(url, '\t');
		if (!name || (size_t)(name - url) >= sizeof web[0].url)   /* an address cut short would play something else */
			continue;
		*name++ = '\0';
		h = &web[nweb];
		memset(h, 0, sizeof *h);
		snprintf(h->url, sizeof h->url, "%s", url);
		tail = strstr(name, "  [");
		if (tail) {
			char *close_br;

			*tail = '\0';
			snprintf(h->info, sizeof h->info, "%s", tail + 3);
			close_br = strchr(h->info, ']');
			if (close_br)
				*close_br = '\0';
		}
		snprintf(h->name, sizeof h->name, "%s", name);
		srch_norm(h->norm, sizeof h->norm, h->name);
		h->kind = STN_WEB;
		h->idx = nweb;
		nweb++;
	}
	wstate = failed && nweb == 0 ? W_FAIL : W_DONE;
}

static void web_finish(struct ui *u)
{
	int st;

	if (wfd >= 0)
		close(wfd);
	wfd = -1;
	waitpid(wpid, &st, 0);
	wpid = 0;
	web_parse();
	rebuild();
	ring_build(0);
	u->dirty = 1;
}

static void web_poll(struct ui *u, long now)
{
	char t[SRCH_MAXQ + 2];

	if (wpid > 0) {
		char buf[4096];
		ssize_t r;
		int eof = 0;

		while ((r = read(wfd, buf, sizeof buf)) > 0) {
			if (wlen + (size_t)r + 1 > wcap) {
				size_t nc = wcap ? wcap * 2 : 1 << 16;
				char *nb = realloc(wbuf, nc);

				if (!nb)
					break;
				wbuf = nb;
				wcap = nc;
			}
			if (wlen + (size_t)r + 1 <= wcap) {
				memcpy(wbuf + wlen, buf, (size_t)r);
				wlen += (size_t)r;
			}
		}
		if (r == 0 || (r < 0 && errno != EAGAIN && errno != EINTR))
			eof = 1;
		if (eof) {
			web_finish(u);
		} else if (ext_since(now, wt0) > WEB_TIMEOUT_MS) {
			web_stop();
			wstate = W_FAIL;
			u->dirty = 1;
		}
	}
	if (!web_enabled)
		return;
	trimmed(t, sizeof t);
	if (strlen(t) < WEB_MIN_CHARS) {
		if (!t[0] && (nweb > 0 || wstate != W_IDLE)) {   /* the text is gone, so are its answers */
			web_stop();
			nweb = 0;
			wstate = W_IDLE;
			wq[0] = '\0';
			rebuild();
			ring_build(0);
			u->dirty = 1;
		}
		return;
	}
	if (strcmp(t, wq) && ext_since(now, q_t) >= WEB_DEBOUNCE_MS) {
		web_stop();
		web_start(t);
		u->dirty = 1;
	}
}

/* --------------------------------------------------------------- the screen --- */

static struct ui_stn collected[LOCAL_MAX];

static void enter(struct ui *u)
{
	int i, n;

	typed[0] = '\0';
	qlen = 0;
	focus = 0;
	sel = top = 0;
	numeric = 0;
	smart = 1;
	from_remote = 0;
	tap_key = -1;
	nloc = nweb = 0;
	wstate = W_IDLE;
	wq[0] = '\0';
	web_enabled = u->input == X45_IN_NETWORK;
	n = stations_collect(u, collected, LOCAL_MAX);
	for (i = 0; i < n; i++) {
		struct hit *h = &loc[nloc];
		int j, dup = 0;

		for (j = 0; collected[i].url[0] && j < nloc; j++)   /* a favourite that the router also has: once */
			dup |= !strcmp(loc[j].url, collected[i].url);
		if (dup)
			continue;
		memset(h, 0, sizeof *h);
		snprintf(h->name, sizeof h->name, "%s", collected[i].name);
		snprintf(h->url, sizeof h->url, "%s", collected[i].url);
		snprintf(h->info, sizeof h->info, "%s", collected[i].info);
		srch_norm(h->norm, sizeof h->norm, collected[i].name);
		h->kind = collected[i].kind;
		h->val = collected[i].val;
		h->idx = nloc++;
	}
	is_open = 1;
	nring = 0;
	cur = 0;
	q_t = ext_now_ms();
	rebuild();
	ring_build(1);
}

static void leave(struct ui *u)
{
	(void)u;
	web_stop();
	wstate = W_IDLE;
	is_open = 0;
	nloc = nweb = nshown = 0;
	free(wbuf);
	wbuf = NULL;
	wcap = wlen = 0;
}

static void poll(struct ui *u)
{
	if (is_open)
		web_poll(u, ext_now_ms());
}

static void play(struct ui *u)
{
	const struct hit *h;
	struct ui_stn s;

	if (sel < 0 || sel >= nshown)
		return;
	h = shown[sel];
	memset(&s, 0, sizeof s);
	snprintf(s.name, sizeof s.name, "%s", h->name);
	snprintf(s.url, sizeof s.url, "%s", h->url);
	s.kind = h->kind;
	s.val = h->val;
	station_play(u, &s);
}

static void to_list(void)
{
	if (nshown > 0) {
		focus = 1;
		sel = 0;
		from_remote = 0;
	}
}

static void press(void)
{
	int c = ring[cur];

	tap_key = -1;
	switch (c) {
	case R_DEL:
		del_char();
		break;
	case R_CLEAR:
		clear_text();
		break;
	case R_ALL:
		smart = !smart;
		ring_build(0);
		break;
	case R_LIST:
		to_list();
		break;
	default:
		type_char(c);
		break;
	}
}

static void move(struct ui *u, int d)
{
	(void)u;
	if (focus == 1) {
		if (nshown > 0)
			sel = ((sel + d) % nshown + nshown) % nshown;
		return;
	}
	from_remote = 0;
	tap_key = -1;
	if (nring > 0)
		cur = ((cur + d) % nring + nring) % nring;
}

static void nav(struct ui *u, int dx, int dy)
{
	if (focus == 1) {
		if (dx || (dy < 0 && sel == 0))
			focus = 0;
		else if (dy)
			move(u, dy);
		return;
	}
	if (dx)
		move(u, dx);
	else if (dy > 0)
		to_list();
}

static void ok(struct ui *u)
{
	if (focus == 1) {
		play(u);
		return;
	}
	if (from_remote) {   /* the text was typed on the remote: OK takes it to the list */
		to_list();
		return;
	}
	press();
}

static void back(struct ui *u)
{
	if (focus == 1) {
		focus = 0;
		return;
	}
	ui_ext_close(u);
}

/* the number keys write letters like an old phone did: again within a moment, the next letter on the key */
static int ir(struct ui *u, int code)
{
	long now = ext_now_ms();

	if (code == KEY_SEARCH) {
		ui_ext_close(u);
		return 1;
	}
	if (code == KEY_BKSP) {
		from_remote = 1;
		focus = 0;
		tap_key = -1;
		del_char();
		return 1;
	}
	if (code == KEY_MODE) {
		numeric = !numeric;
		return 1;
	}
	if (code >= KEY_NUM0 && code <= KEY_NUM0 + 9) {
		int d = code - KEY_NUM0;
		const char *set = srch_tap_chars(d);
		size_t n = strlen(set);

		if (u->ir_bounce)   /* the same button twice within a blink is one press */
			return 1;
		from_remote = 1;
		focus = 0;
		if (numeric) {
			type_char('0' + d);
			tap_key = -1;
			return 1;
		}
		if (tap_key == d && qlen > 0 && ext_since(now, tap_t) < TAP_MS) {
			tap_idx = (tap_idx + 1) % (int)n;
			if (set[tap_idx] == ' ' && (qlen == 1 || typed[qlen - 2] == ' ')) {
				del_char();      /* a space cannot stand there: the cycle ends with the letter before it gone */
				tap_key = -1;
				return 1;
			}
			typed[qlen - 1] = set[tap_idx];
			changed(1);
		} else {
			tap_idx = 0;
			tap_key = type_char(set[0]) ? d : -1;
		}
		tap_t = now;
		return 1;
	}
	return 0;
}

/* ------------------------------------------------------------------ drawing --- */

static void row(struct ui *u, int i, int x, int y, int w, int selected, void *ctx)
{
	static const char *const tags[] = { "this list", "favourite", "router", "DAB+", "FM", "web" };
	const struct hit *h = shown[i];
	const char *tag = (h->kind == STN_WEB || h->kind == STN_LIST) && h->info[0] ? h->info : tags[h->kind];
	uint32_t fg = selected ? u->t.bg : u->t.text, dim = selected ? u->t.bg : u->t.dim;
	int tw = x45_text_w(tag, 2);

	(void)ctx;
	text_fold(&u->g.back, x + 12, y + 8, h->name, 3, w - tw - 44, fg);
	x45_text(&u->g.back, x + w - tw - 16, y + 13, tag, 2, dim);
}

static void draw_ring(struct ui *u, int y)
{
	struct x45_surface *s = &u->g.back;
	enum { CW = 64, CH = 60, GAP = 4, SLOTS = 13 };
	int x0 = (s->w - (SLOTS * CW + (SLOTS - 1) * GAP)) / 2, k;
	int rotate = nring >= SLOTS, start = rotate ? 0 : (SLOTS - nring) / 2;

	for (k = 0; k < SLOTS; k++) {
		int idx, c, on, x = x0 + k * (CW + GAP), tw, scale;
		char lbl[8];
		uint32_t fg;

		if (rotate)
			idx = ((cur + k - SLOTS / 2) % nring + nring) % nring;
		else if (k >= start && k < start + nring)
			idx = k - start;
		else
			continue;
		c = ring[idx];
		on = idx == cur;
		if (on && focus == 0) {
			x45_fill_rect(s, x, y, CW, CH, u->t.accent);
			fg = u->t.bg;
		} else {
			x45_fill_rect(s, x, y, CW, CH, u->t.panel);
			fg = ring_dim[idx] ? u->t.dim : (c == R_LIST ? u->t.accent : u->t.text);
		}
		x45_draw_rect(s, x, y, CW, CH, on ? u->t.accent : u->t.line);
		scale = 2;
		switch (c) {
		case R_DEL:   snprintf(lbl, sizeof lbl, "del"); break;
		case R_CLEAR: snprintf(lbl, sizeof lbl, "clr"); break;
		case R_ALL:   snprintf(lbl, sizeof lbl, "%s", smart ? "all" : "less"); break;
		case R_LIST:  snprintf(lbl, sizeof lbl, "list"); break;
		case ' ':     snprintf(lbl, sizeof lbl, "spc"); break;
		default:
			lbl[0] = (char)(c >= 'a' && c <= 'z' ? c - 32 : c);
			lbl[1] = '\0';
			scale = 4;
			break;
		}
		tw = x45_text_w(lbl, scale);
		x45_text(s, x + (CW - tw) / 2, y + (CH - x45_text_h(scale)) / 2, lbl, scale, fg);
	}
}

static void draw(struct ui *u)
{
	struct x45_surface *s = &u->g.back;
	const int bx = 32, bw = s->w - 64, by = 64, bh = 50, ly = 122, rows = 6, ry = ly + rows * EXT_ROW_H + 14;
	char status[64], t[SRCH_MAXQ + 2];
	const char *shown_txt = typed;
	int tw, sw, i;

	/* the text being typed, with a block for the cursor */
	x45_fill_rect(s, bx, by, bw, bh, u->t.panel);
	x45_draw_rect(s, bx, by, bw, bh, u->t.line);
	while (*shown_txt && x45_text_w(shown_txt, 4) > bw - 300)
		shown_txt++;
	tw = x45_text_w(shown_txt, 4);
	if (qlen == 0)
		x45_text(s, bx + 14, by + (bh - x45_text_h(3)) / 2, "type a station name", 3, u->t.dim);
	x45_text(s, bx + 14, by + (bh - x45_text_h(4)) / 2, shown_txt, 4, u->t.text);
	x45_fill_rect(s, bx + 14 + tw + 2, by + 9, 12, bh - 18, u->t.accent);

	/* what the search is up to, at the right of the box */
	trimmed(t, sizeof t);
	if (web_enabled && wstate == W_RUN)
		snprintf(status, sizeof status, "%d found, asking the web", nshown);
	else if (web_enabled && wstate == W_FAIL && strlen(t) >= WEB_MIN_CHARS)
		snprintf(status, sizeof status, "%d found, web did not answer", nshown);
	else if (web_enabled && strlen(t) < WEB_MIN_CHARS && nshown > 0)
		snprintf(status, sizeof status, "%d stations", nshown);
	else
		snprintf(status, sizeof status, "%d found", nshown);
	sw = x45_text_w(status, 2);
	x45_text(s, bx + bw - sw - 14, by + (bh - x45_text_h(2)) / 2, status, 2,
		 web_enabled && wstate == W_FAIL && strlen(t) >= WEB_MIN_CHARS ? u->t.warn : u->t.dim);

	/* the matches */
	if (nshown > 0) {
		ext_list(u, bx, ly, bw, rows * EXT_ROW_H, nshown, focus == 1 ? sel : -1, &top, row, NULL);
	} else {
		x45_text(s, bx + 12, ly + 12, qlen ? "no station matches" : "no stations yet", 3, u->t.dim);
		if (web_enabled && qlen && wstate == W_RUN)
			x45_text(s, bx + 12, ly + 48, "asking the radio directory ...", 2, u->t.dim);
	}

	/* the keys, and what the number keys of the remote write */
	draw_ring(u, ry);
	{
		char lg[160] = "";

		for (i = 1; i <= 10; i++) {
			int d = i % 10;
			const char *set = srch_tap_chars(d);
			char one[24];

			if (numeric)
				snprintf(one, sizeof one, "%d ", d);
			else if (d == 0)
				snprintf(one, sizeof one, "0 space");
			else
				snprintf(one, sizeof one, "%d %.3s  ", d, set);
			strncat(lg, one, sizeof lg - strlen(lg) - 1);
		}
		x45_text(s, bx, ry + 60 + 16, numeric ? "numbers: " : lg, 2, u->t.dim);
		if (numeric)
			x45_text(s, bx + x45_text_w("numbers: ", 2), ry + 60 + 16, lg, 2, u->t.text);
	}
	draw_hints(u, focus == 1 ? "scroll|move;OK|play;RETURN|keys" : "scroll|letter;OK|type;RETURN|back");
}

const struct ui_ext ext_search = {
	.name = "search",
	.title = "SEARCH",
	.menu = NULL,
	.enter = enter,
	.leave = leave,
	.draw = draw,
	.ok = ok,
	.back = back,
	.move = move,
	.nav = nav,
	.ir = ir,
	.poll = poll,
};
