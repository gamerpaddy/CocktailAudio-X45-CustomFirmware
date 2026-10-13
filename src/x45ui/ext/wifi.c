#define _GNU_SOURCE
/* ext/wifi - the WiFi assistant: switch the radio on, see what is in range, join a network (typing the
 * password on the on-screen keyboard), and look after the saved ones (autoconnect, change password, forget).
 *
 * All state lives in wpa_supplicant (libx45/wifi.h speaks to it); this file only keeps what the screen
 * needs: a cached status for the top-bar symbol, the merged list of saved + visible networks, and the
 * small state machine of the views.
 */
#include "ext.h"
#include "osk.h"
#include "../../libx45/wifi.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum view { V_LIST, V_ACTION, V_PASS, V_HIDDEN_SSID };

struct entry {
	char ssid[X45_WIFI_SSID];
	int saved;          /* saved network id, -1 none */
	int auto_join;
	int current;
	int seen;           /* in range now */
	int rssi, freq;
	int secure, unsupported;
	char sec[12];
	char bssid[18];
};

#define MAX_ENT 48
#define SCAN_EVERY_MS 15000
#define PENDING_MS 45000
#define FAIL_SHOW_MS 15000

static struct x45_wifi w = { .req = -1, .mon = -1 };
static struct x45_wifi_status st;
static long st_t, scan_t;
static struct x45_wifi_ap aps[MAX_ENT];
static int nap;
static struct x45_wifi_net nets[16];
static int nnet;
static struct entry ent[MAX_ENT];
static int nent;

static enum view view;
static int sel, top;            /* list view cursor */
static int asel;                /* action view cursor */
static struct entry cur;        /* the entry the action / password views are about */
static struct osk osk;
static char hidden_ssid[X45_WIFI_SSID];
static long forget_armed_t;

/* a join in progress, so that its outcome can be told */
static struct {
	int active, is_new;
	char ssid[X45_WIFI_SSID];
	long t;
	unsigned conn0, fail0, nf0;
} pend;
static char note[96];           /* the last outcome, shown in the status block for a while */
static int note_bad;
static long note_t;

extern const struct ui_ext ext_wifi;

/* ------------------------------------------------------------------ data --- */

static int level_of(int rssi)
{
	return rssi >= -55 ? 4 : rssi >= -65 ? 3 : rssi >= -75 ? 2 : 1;
}

static void set_note(const char *msg, int bad)
{
	snprintf(note, sizeof note, "%s", msg);
	note_bad = bad;
	note_t = ext_now_ms();
}

static int ent_cmp(const void *pa, const void *pb)
{
	const struct entry *a = pa, *b = pb;

	if (a->current != b->current)
		return b->current - a->current;
	if ((a->saved >= 0) != (b->saved >= 0))
		return (b->saved >= 0) - (a->saved >= 0);
	if (a->seen != b->seen)
		return b->seen - a->seen;
	return b->rssi - a->rssi;
}

/* saved networks and the ones heard, one row per name: the one in use, the saved, then by strength */
static void ent_build(void)
{
	int i, k;

	nent = 0;
	for (i = 0; i < nnet && nent < MAX_ENT; i++) {
		struct entry *e = &ent[nent++];

		memset(e, 0, sizeof *e);
		snprintf(e->ssid, sizeof e->ssid, "%.*s", (int)sizeof e->ssid - 1, nets[i].ssid);
		e->saved = nets[i].id;
		e->auto_join = nets[i].auto_join;
		e->current = nets[i].current;
		e->secure = !nets[i].open;
		snprintf(e->sec, sizeof e->sec, "%s", nets[i].open ? "open" : "key");
	}
	for (i = 0; i < nap; i++) {
		struct entry *e = NULL;

		for (k = 0; k < nent; k++)
			if (!strcmp(ent[k].ssid, aps[i].ssid))
				e = &ent[k];
		if (!e) {
			if (nent >= MAX_ENT)
				continue;
			e = &ent[nent++];
			memset(e, 0, sizeof *e);
			snprintf(e->ssid, sizeof e->ssid, "%.*s", (int)sizeof e->ssid - 1, aps[i].ssid);
			e->saved = -1;
			e->auto_join = 1;
		}
		e->seen = 1;
		e->rssi = aps[i].rssi;
		e->freq = aps[i].freq;
		e->secure = aps[i].secure;
		e->unsupported = aps[i].unsupported;
		snprintf(e->sec, sizeof e->sec, "%s", aps[i].sec);
		snprintf(e->bssid, sizeof e->bssid, "%s", aps[i].bssid);
	}
	/* the current one is heard from its connection even when a scan has not listed it */
	for (k = 0; k < nent; k++)
		if (ent[k].current && !ent[k].seen && st.rssi) {
			ent[k].rssi = st.rssi;
			ent[k].freq = st.freq;
			ent[k].seen = 1;
		}
	qsort(ent, nent, sizeof ent[0], ent_cmp);
}

static void reload_nets(void)
{
	nnet = x45_wifi_networks(&w, nets, 16);
	if (nnet < 0)
		nnet = 0;
}

static void reload_all(void)
{
	char keep[X45_WIFI_SSID] = "";

	if (sel >= 1 && sel - 1 < nent)
		snprintf(keep, sizeof keep, "%s", ent[sel - 1].ssid);
	reload_nets();
	nap = x45_wifi_aps(&w, aps, MAX_ENT);
	if (nap < 0)
		nap = 0;
	ent_build();
	if (keep[0]) {
		int i;

		for (i = 0; i < nent; i++)
			if (!strcmp(ent[i].ssid, keep))
				sel = i + 1;
	}
}

static int on(void)
{
	return st.state != X45_WIFI_OFF && st.state != X45_WIFI_ERROR;
}

/* the rows of the list: 0 the power switch, then the networks, then "other network" */
static int list_n(void)
{
	return st.state == X45_WIFI_STARTING || !on() ? 1 : 1 + nent + 1;
}

/* ---------------------------------------------------------------- polling --- */

static void refresh_status(struct ui *u)
{
	struct x45_wifi_status old = st;

	if (w.req < 0 && w.mon < 0)
		x45_wifi_open(&w, 1);
	x45_wifi_status(&w, &st);
	st_t = ext_now_ms();
	if (old.state != st.state || strcmp(old.ssid, st.ssid) || strcmp(old.ip, st.ip) ||
	    level_of(old.rssi) != level_of(st.rssi) || strcmp(old.step, st.step) || strcmp(old.error, st.error)) {
		u->dirty = 1;
		if (old.state != st.state || strcmp(old.ssid, st.ssid)) {
			reload_nets();
			ent_build();
		}
	}
}

static void check_pending(struct ui *u)
{
	long now = ext_now_ms();

	if (!pend.active)
		return;
	if (w.conn_seq != pend.conn0 && st.state == X45_WIFI_CONNECTED && !strcmp(st.ssid, pend.ssid)) {
		note[0] = '\0';   /* the status block says it */
		pend.active = 0;
	} else if (w.fail_seq != pend.fail0 && (!strcmp(w.fail, "WRONG_KEY") || !strcmp(w.fail, "AUTH_FAILED") ||
					     !strcmp(w.fail, "CONN_FAILED"))) {   /* a refusal or two is not the end */
		char m[96];
		const char *why = x45_wifi_fail_text(w.fail);

		snprintf(m, sizeof m, "%s: %s", pend.ssid, why);
		set_note(m, 1);
		if (pend.is_new) {   /* a network that never worked is not worth keeping */
			int i;

			reload_nets();
			for (i = 0; i < nnet; i++)
				if (!strcmp(nets[i].ssid, pend.ssid))
					x45_wifi_forget(&w, nets[i].id);
			reload_all();
		}
		pend.active = 0;
	} else if (w.notfound_seq != pend.nf0) {
		char m[96];

		snprintf(m, sizeof m, "%s is not in range", pend.ssid);
		set_note(m, 1);
		pend.active = 0;
	} else if (ext_since(now, pend.t) > PENDING_MS) {
		char m[96];

		snprintf(m, sizeof m, "%s did not answer", pend.ssid);
		set_note(m, 1);
		pend.active = 0;
	} else {
		return;
	}
	u->dirty = 1;
}

static void poll(struct ui *u)
{
	long now = ext_now_ms();
	int open = u->screen == SCR_EXT && u->ext == &ext_wifi;
	int events = 0, scanned = 0;
	long every = open || pend.active || st.state == X45_WIFI_STARTING ? 700 : st.state == X45_WIFI_OFF ? 4000 : 3000;

	if (w.req >= 0 || w.mon >= 0) {
		unsigned s0 = w.scan_seq;

		events = x45_wifi_pump(&w);
		scanned = w.scan_seq != s0;
	}
	if (events || ext_since(now, st_t) >= every)
		refresh_status(u);
	check_pending(u);
	if (note[0] && ext_since(now, note_t) > FAIL_SHOW_MS) {
		note[0] = '\0';
		u->dirty = 1;
	}
	if (!open)
		return;
	if (scanned) {
		reload_all();
		u->dirty = 1;
	}
	if (view == V_LIST && sel >= list_n()) {   /* the list got shorter under the cursor (a scan lost a network, the radio went off) */
		sel = list_n() - 1;
		u->dirty = 1;
	}
	if (view == V_LIST && on() && st.state != X45_WIFI_STARTING && st.state != X45_WIFI_CONNECTING &&
	    ext_since(now, scan_t) >= SCAN_EVERY_MS) {
		scan_t = now;
		x45_wifi_scan(&w);
	}
	if (forget_armed_t && ext_since(now, forget_armed_t) > 4000) {
		forget_armed_t = 0;
		u->dirty = 1;
	}
}

/* --------------------------------------------------------------- actions --- */

static void start_join(struct ui *u, const char *ssid, const char *psk, int hidden, int is_new)
{
	int id = x45_wifi_connect(&w, ssid, psk, hidden);

	if (id < 0) {
		set_note(id == -EINVAL ? "that password cannot be right (8 to 63 characters)" : "could not save the network", 1);
		return;
	}
	memset(&pend, 0, sizeof pend);
	pend.active = 1;
	pend.is_new = is_new;
	pend.t = ext_now_ms();
	pend.conn0 = w.conn_seq;
	pend.fail0 = w.fail_seq;
	pend.nf0 = w.notfound_seq;
	snprintf(pend.ssid, sizeof pend.ssid, "%s", ssid);
	w.fail[0] = '\0';
	note[0] = '\0';
	hidden_ssid[0] = '\0';
	reload_all();
	for (id = 0; id < nent; id++)   /* the cursor follows the network that was just joined */
		if (!strcmp(ent[id].ssid, ssid))
			sel = id + 1;
	view = V_LIST;
	u->dirty = 1;
}

static void join_saved(struct ui *u, const struct entry *e)
{
	if (x45_wifi_join(&w, e->saved) < 0) {
		set_note("could not start the connection", 1);
		return;
	}
	memset(&pend, 0, sizeof pend);
	pend.active = 1;
	pend.t = ext_now_ms();
	pend.conn0 = w.conn_seq;
	pend.fail0 = w.fail_seq;
	pend.nf0 = w.notfound_seq;
	snprintf(pend.ssid, sizeof pend.ssid, "%s", e->ssid);
	w.fail[0] = '\0';
	note[0] = '\0';
	view = V_LIST;
	u->dirty = 1;
}

static void open_action(struct ui *u, const struct entry *e)
{
	cur = *e;
	asel = 0;
	forget_armed_t = 0;
	view = V_ACTION;
	u->dirty = 1;
}

/* rows of the action view for `cur` */
enum { A_CONNECT, A_AUTO, A_PASS, A_FORGET, A_NONE };

static int action_rows(int *ids)
{
	int n = 0;

	ids[n++] = A_CONNECT;
	if (cur.saved >= 0) {
		ids[n++] = A_AUTO;
		if (cur.secure && !cur.unsupported)
			ids[n++] = A_PASS;
		ids[n++] = A_FORGET;
	}
	return n;
}

static void power_toggle(struct ui *u)
{
	if (st.state == X45_WIFI_STARTING) {
		say(u, "still starting, one moment");
		return;
	}
	if (on()) {
		x45_wifi_power(0);
		memset(&pend, 0, sizeof pend);
		note[0] = '\0';
	} else {
		x45_wifi_power(1);
		snprintf(st.step, sizeof st.step, "starting");
		st.state = X45_WIFI_STARTING;
	}
	nap = nent = 0;
	st_t = ext_now_ms() - 60000;   /* read the status again at the next pass */
	u->dirty = 1;
}

/* -------------------------------------------------------------- the screen --- */

static void enter(struct ui *u)
{
	view = V_LIST;
	sel = top = 0;
	forget_armed_t = 0;
	refresh_status(u);
	reload_all();
	scan_t = ext_now_ms() - SCAN_EVERY_MS;   /* look around at once */
}

static void leave(struct ui *u)
{
	view = V_LIST;
	memset(&osk, 0, sizeof osk);   /* a half-typed password is not kept */
}

static void move(struct ui *u, int d)
{
	int n;

	if (view == V_PASS || view == V_HIDDEN_SSID) {
		osk_move(&osk, d);
		return;
	}
	if (view == V_ACTION) {
		int ids[4];

		n = action_rows(ids);
		asel = ((asel + d) % n + n) % n;
		forget_armed_t = 0;
		return;
	}
	n = list_n();
	sel = ((sel + d) % n + n) % n;
}

static void nav(struct ui *u, int dx, int dy)
{
	if (view == V_PASS || view == V_HIDDEN_SSID)
		osk_nav(&osk, dx, dy);
	else if (dy)
		move(u, dy);
}

static void ok(struct ui *u)
{
	if (view == V_PASS || view == V_HIDDEN_SSID) {
		if (osk_press(&osk) != OSK_DONE)
			return;
		if (view == V_HIDDEN_SSID) {
			if (!osk.len) {
				view = V_LIST;
				return;
			}
			snprintf(hidden_ssid, sizeof hidden_ssid, "%.*s", (int)sizeof hidden_ssid - 1, osk.buf);
			snprintf(cur.ssid, sizeof cur.ssid, "%s", hidden_ssid);
			cur.saved = -1;
			cur.secure = 1;
			osk_open(&osk, "Password (leave empty for an open network)", "", 63, 1);
			view = V_PASS;
			return;
		}
		{
			int hidden = cur.saved < 0 && !cur.seen;   /* typed in by hand and not heard: ask for it by name */

			if (cur.saved >= 0 && !osk.len) {
				view = V_LIST;
				return;
			}
			start_join(u, cur.ssid, osk.len ? osk.buf : NULL, hidden, cur.saved < 0);
			memset(&osk, 0, sizeof osk);
		}
		return;
	}
	if (view == V_ACTION) {
		int ids[4], n = action_rows(ids);

		if (asel >= n)
			return;
		switch (ids[asel]) {
		case A_CONNECT:
			if (cur.current) {
				x45_wifi_disconnect(&w);
				memset(&pend, 0, sizeof pend);
				view = V_LIST;
			} else {
				join_saved(u, &cur);
			}
			break;
		case A_AUTO:
			x45_wifi_set_auto(&w, cur.saved, !cur.auto_join);
			cur.auto_join = !cur.auto_join;
			reload_all();
			break;
		case A_PASS:
			osk_open(&osk, "New password", "", 63, 1);
			view = V_PASS;
			break;
		case A_FORGET:
			if (!forget_armed_t) {
				forget_armed_t = ext_now_ms();   /* OK again, within 4 s */
			} else {
				x45_wifi_forget(&w, cur.saved);
				forget_armed_t = 0;
				set_note("network forgotten", 0);
				reload_all();
				view = V_LIST;
				sel = top = 0;
			}
			break;
		}
		return;
	}
	/* the list */
	if (sel == 0) {
		power_toggle(u);
		return;
	}
	if (!on() || st.state == X45_WIFI_STARTING)
		return;
	if (sel - 1 >= nent) {   /* "other network" */
		memset(&cur, 0, sizeof cur);
		cur.saved = -1;
		osk_open(&osk, "Network name", "", 32, 1);
		view = V_HIDDEN_SSID;
		return;
	}
	{
		const struct entry *e = &ent[sel - 1];

		if (e->saved >= 0) {
			open_action(u, e);
		} else if (e->unsupported) {
			say(u, "%s networks are not supported yet", e->sec);
		} else if (!e->secure) {
			start_join(u, e->ssid, NULL, 0, 1);
		} else {
			cur = *e;
			osk_open(&osk, "Password", "", 63, 1);
			snprintf(osk.prompt, sizeof osk.prompt, "Password for %.40s", e->ssid);
			view = V_PASS;
		}
	}
}

static void back(struct ui *u)
{
	if (view == V_PASS && cur.saved >= 0 && hidden_ssid[0] == '\0') {
		view = V_ACTION;
	} else if (view == V_PASS || view == V_HIDDEN_SSID || view == V_ACTION) {
		view = V_LIST;
	} else {
		ui_ext_close(u);
		return;
	}
	hidden_ssid[0] = '\0';
	memset(&osk, 0, sizeof osk);
	forget_armed_t = 0;
}

/* ---- drawing ---- */

static void status_block(struct ui *u)
{
	struct x45_surface *s = &u->g.back;
	char l1[96], l2[128] = "";
	uint32_t c1 = u->t.text, c2 = u->t.dim;

	switch (st.state) {
	case X45_WIFI_OFF:
		snprintf(l1, sizeof l1, "WiFi is off");
		snprintf(l2, sizeof l2, "push to switch it on");
		break;
	case X45_WIFI_STARTING:
		snprintf(l1, sizeof l1, "Starting WiFi...");
		snprintf(l2, sizeof l2, "%s", st.step);
		break;
	case X45_WIFI_ERROR:
		snprintf(l1, sizeof l1, "WiFi did not start");
		snprintf(l2, sizeof l2, "%s", st.error);
		c1 = u->t.warn;
		break;
	case X45_WIFI_CONNECTED:
		snprintf(l1, sizeof l1, "Connected to %s", st.ssid);
		snprintf(l2, sizeof l2, "%s   %s   %d dBm", st.ip, st.freq >= 4900 ? "5 GHz" : "2.4 GHz", st.rssi);
		c1 = u->t.accent;
		break;
	case X45_WIFI_CONNECTING:
		snprintf(l1, sizeof l1, "Connecting%s%s...", pend.active ? " to " : "", pend.active ? pend.ssid : "");
		snprintf(l2, sizeof l2, "%s", st.wpa_state);
		break;
	default:
		snprintf(l1, sizeof l1, "Not connected");
		snprintf(l2, sizeof l2, "%s", nnet ? "push a network to join it" : "choose a network below");
		break;
	}
	if (note[0]) {   /* the outcome of what was just tried wins the first line */
		snprintf(l2, sizeof l2, "%s", l1);
		snprintf(l1, sizeof l1, "%s", note);
		c2 = u->t.dim;
		c1 = note_bad ? u->t.warn : u->t.accent;
	}
	x45_text_clipped(s, 24, 72, l1, 3, s->w - 48, c1);
	x45_text_clipped(s, 24, 102, l2, 2, s->w - 48, c2);
}

static void row_list(struct ui *u, int i, int x, int y, int rw, int selected, void *ctx)
{
	struct x45_surface *s = &u->g.back;
	uint32_t fg = ext_fg(u, selected), dim = selected ? u->t.bg : u->t.dim;

	if (i == 0) {
		const char *v = st.state == X45_WIFI_STARTING ? "starting" : on() ? "on" : "off";

		x45_text(s, x + 26, y + 9, "WiFi", 3, fg);
		x45_text(s, x + rw - 24 - x45_text_w(v, 3), y + 9, v, 3, selected ? u->t.bg : u->t.accent);
	} else if (i - 1 >= nent) {
		x45_text(s, x + 26, y + 9, "Other network...", 3, fg);
		x45_text(s, x + rw - 24 - x45_text_w(">", 3), y + 9, ">", 3, dim);
	} else {
		const struct entry *e = &ent[i - 1];
		int rx = x + rw - 28;
		uint32_t nc = e->seen || e->current ? fg : dim;

		if (e->current)
			x45_fill_rect(s, x + 6, y + 14, 10, 10, selected ? u->t.bg : u->t.accent);
		if (e->seen) {
			rx -= 30;
			ext_bars(u, rx, y + 6, level_of(e->rssi), selected ? u->t.bg : u->t.accent,
				 selected ? u->t.accent : u->t.line);
		}
		if (e->secure && e->sec[0]) {
			rx -= 18 + x45_text_w(e->sec, 2);
			x45_text(s, rx, y + 12, e->sec, 2, dim);
		}
		if (e->saved >= 0) {
			const char *m = !e->auto_join ? "manual" : "saved";

			rx -= 18 + x45_text_w(m, 2);
			x45_text(s, rx, y + 12, m, 2, selected ? u->t.bg : u->t.accent);
		}
		x45_text_clipped(s, x + 26, y + 9, e->ssid, 3, rx - x - 40, nc);
	}
}

static void row_action(struct ui *u, int i, int x, int y, int rw, int selected, void *ctx)
{
	struct x45_surface *s = &u->g.back;
	uint32_t fg = ext_fg(u, selected);
	char l[48], v[32] = "";
	int ids[4];

	action_rows(ids);
	switch (ids[i]) {
	case A_CONNECT:
		snprintf(l, sizeof l, "%s", cur.current ? "Disconnect" : "Connect");
		break;
	case A_AUTO:
		snprintf(l, sizeof l, "Join automatically");
		snprintf(v, sizeof v, "%s", cur.auto_join ? "on" : "off");
		break;
	case A_PASS:
		snprintf(l, sizeof l, "Change the password");
		break;
	default:
		if (forget_armed_t) {
			snprintf(l, sizeof l, "Press OK again to forget");
			fg = selected ? u->t.bg : u->t.warn;
		} else {
			snprintf(l, sizeof l, "Forget this network");
		}
		break;
	}
	x45_text(s, x + 26, y + 9, l, 3, fg);
	if (v[0])
		x45_text(s, x + rw - 24 - x45_text_w(v, 3), y + 9, v, 3, selected ? u->t.bg : u->t.accent);
}

static void draw(struct ui *u)
{
	struct x45_surface *s = &u->g.back;

	if (view == V_PASS || view == V_HIDDEN_SSID) {
		osk_draw(u, &osk);
		return;
	}
	if (view == V_ACTION) {
		char info[128];
		int ids[4], n = action_rows(ids);

		x45_text_clipped(s, 24, 72, cur.ssid, 3, s->w - 48, u->t.text);
		if (cur.seen)
			snprintf(info, sizeof info, "%s   %s   %d dBm", cur.sec, cur.freq >= 4900 ? "5 GHz" : "2.4 GHz", cur.rssi);
		else
			snprintf(info, sizeof info, "not in range");
		x45_text_clipped(s, 24, 102, info, 2, s->w - 48, u->t.dim);
		ext_list(u, 12, 150, s->w - 24, n * EXT_ROW_H, n, asel, &(int){ 0 }, row_action, NULL);
		draw_hints(u, "scroll|move;OK|choose;RETURN|back");
		return;
	}
	status_block(u);
	x45_hline(s, 12, 136, s->w - 24, u->t.line);
	ext_list(u, 12, 148, s->w - 24, 390, list_n(), sel, &top, row_list, NULL);
	draw_hints(u, "scroll|move;OK|select;RETURN|back");
}

/* the top bar: the usual symbol, an arc for each strength step, a dot when it is still connecting */
static int badge(struct ui *u, int right)
{
	struct x45_surface *s = &u->g.back;
	int lit, a, cx = right - 18, cy = 40;
	uint32_t on_c;

	if (st.state == X45_WIFI_CONNECTED) {
		lit = level_of(st.rssi) - 1;   /* arcs lit besides the dot: 0..3 */
		on_c = u->t.accent;
	} else if (st.state == X45_WIFI_CONNECTING) {
		lit = 0;
		on_c = u->t.dim;
	} else {
		return 0;
	}
	x45_fill_rect(s, cx - 2, cy - 3, 5, 5, on_c);
	for (a = 0; a < 3; a++) {
		int r = 8 + a * 7, dy, dx;
		uint32_t c = a < lit ? on_c : u->t.line;

		for (dy = -r - 1; dy <= 0; dy++)
			for (dx = -r - 1; dx <= r + 1; dx++) {
				int d2 = dx * dx + dy * dy;

				if (d2 >= (r - 1) * (r - 1) && d2 <= (r + 1) * (r + 1) && -dy >= (dx < 0 ? -dx : dx))
					x45_put(s, cx + dx, cy - 6 + dy, c);
			}
	}
	return 36;
}

const struct ui_ext ext_wifi = {
	.name = "wifi",
	.title = "WIFI",
	.menu = "Network",
	.enter = enter,
	.leave = leave,
	.draw = draw,
	.ok = ok,
	.back = back,
	.move = move,
	.nav = nav,
	.poll = poll,
	.badge = badge,
};
