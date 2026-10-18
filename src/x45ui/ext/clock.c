#define _GNU_SOURCE
/* ext/clock - the clock and the sleep timer.
 *
 * Two screens share this file and one set of settings:
 *
 *   clock   a large clock (time, date, what plays). The remote's CLOCK key opens it; pressed on it, the backlight goes
 *           down (the manual: "digital clock display / OFF screen"), and any key brings it back.
 *   sleep   the sleep timer and the clock settings, listed in Options as "Clock and sleep timer". The remote's SLEEP
 *           key opens it; pressed on it, it steps through the times. A timer that runs shows in the top bar.
 *
 * When the timer ends the volume has already been faded down over its last 30 s (when the volume is not fixed), and
 *   stop and sleep   what plays is stopped, the output muted and the backlight put down; the first key press brings
 *                    the unit back, unmuted. This works on every input: the ones that play in hardware are muted.
 *   switch off       the unit is shut down (a recording or rip that is running is waited for).
 * A key pressed during the fade ends the timer: somebody is still there.
 *
 * The time comes from the system clock, which the network sets (ntpdate) and the RTC keeps over a power-off. When it
 * was never set (a year before 2025) the clock says so instead of showing a wrong time; the web page can set it.
 *
 * etc/x45ui.conf:  sleep_action = standby|off   clock_bar = 0|1   clock_idle = seconds (0: never)
 *                  clock_24h = 0|1   clock_secs = 0|1
 */
#include "ext.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

extern const struct ui_ext ext_clock;
extern const struct ui_ext ext_sleep;

#define KEY_CLOCK 359
#define KEY_SLEEP 142
#define TIME_FLOOR 1735689600L   /* 2025-01-01: an earlier date means the clock was never set */
#define FADE_MS 30000L

static const int sleep_pre[] = { 0, 15, 30, 45, 60, 90, 120 };
static const int idle_pre[] = { 0, 60, 120, 300, 600, 1800 };
#define NSLEEP ((int)(sizeof sleep_pre / sizeof *sleep_pre))
#define NIDLE ((int)(sizeof idle_pre / sizeof *idle_pre))

static struct {
	int loaded;
	int off;              /* when the timer ends: 0 stop and sleep, 1 switch the unit off */
	int bar, h24, secs;
	int idle;             /* seconds without a key before the clock comes up, 0 never */
	/* the timer */
	int minutes;          /* the length chosen, 0 off */
	int running, fading;
	long end;             /* ext_now_ms() when it ends */
	long act_seen;        /* u->touch_t when the fade began: a key since then ends the timer */
	int last_pct;
	/* asleep */
	int standby;
	long standby_act;
	int off_pending;      /* switching off waits for a recording to end */
	/* what the last frame showed, to redraw when it changes */
	int min_seen, sec_seen;
} C = { .bar = 1, .h24 = 1, .secs = 1 };

static const char *const wday[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
static const char *const mon[] = { "January", "February", "March", "April", "May", "June", "July", "August",
				   "September", "October", "November", "December" };

/* ------------------------------------------------------------------ settings --- */

static void load(void)
{
	char v[32];

	C.loaded = 1;
	if (ext_conf_get("sleep_action", v, sizeof v))
		C.off = !strncmp(v, "off", 3);
	C.bar = ext_conf_int("clock_bar", 1) != 0;
	C.h24 = ext_conf_int("clock_24h", 1) != 0;
	C.secs = ext_conf_int("clock_secs", 1) != 0;
	C.idle = ext_conf_int("clock_idle", 0);
	if (C.idle < 0 || C.idle > 7200)
		C.idle = 0;
}

static void keep(const char *key, int v)
{
	char b[16];

	snprintf(b, sizeof b, "%d", v);
	save_config_key(key, b);
}

/* ---------------------------------------------------------------------- time --- */

static int time_ok(void)
{
	return time(NULL) >= TIME_FLOOR;
}

static int local(struct tm *t)
{
	time_t now = time(NULL);

	localtime_r(&now, t);
	return now >= TIME_FLOOR;
}

/* "19:04" or "7:04"; the 12 hour clock's AM / PM goes to *ampm (NULL in the 24 hour one) */
static void fmt_hm(char *out, size_t cap, const struct tm *t, const char **ampm)
{
	if (C.h24) {
		snprintf(out, cap, "%02d:%02d", t->tm_hour, t->tm_min);
		if (ampm)
			*ampm = NULL;
	} else {
		int h = t->tm_hour % 12;

		snprintf(out, cap, "%d:%02d", h ? h : 12, t->tm_min);
		if (ampm)
			*ampm = t->tm_hour < 12 ? "AM" : "PM";
	}
}

/* ---------------------------------------------------------------- sleep timer --- */

static long left_ms(long now)
{
	return (long)((unsigned long)C.end - (unsigned long)now);
}

static void timer_set(struct ui *u, int minutes)
{
	if (C.fading) {
		C.fading = 0;
		ui_level_restore(u);
	}
	C.off_pending = 0;
	C.minutes = minutes;
	C.running = minutes > 0;
	if (C.running) {
		C.end = ext_now_ms() + minutes * 60000L;
		C.act_seen = 0;
		say(u, "Sleep timer %d min", minutes);
	} else {
		say(u, "Sleep timer off");
	}
	u->dirty = 1;
}

static void timer_fire(struct ui *u)
{
	C.running = 0;
	C.fading = 0;
	C.minutes = 0;
	if (C.off) {
		if (u->rec_pid > 0)
			C.off_pending = 1;   /* a recording is not cut off by the timer: the unit goes off when it is finished */
		else
			ui_power_off(u);
		return;
	}
	ui_stop_playing(u);
	ui_set_mute(u, 1);
	ui_level_restore(u);   /* the output is muted; the level the owner set is back for when they unmute */
	C.standby = 1;
	ui_screen_dark(u);
	C.standby_act = u->touch_t;
}

static void timer_tick(struct ui *u, long now)
{
	long rem;

	if (C.standby) {   /* somebody pressed something: the unit wakes up unmuted */
		if (u->touch_t != C.standby_act) {
			C.standby = 0;
			ui_set_mute(u, 0);
			say(u, "Awake");
		} else if (!u->scr_dark) {
			ui_screen_dark(u);   /* the stop that ended the music lit the screen (a new track does that): nobody touched the unit */
		}
		return;
	}
	if (C.off_pending && u->rec_pid <= 0) {
		C.off_pending = 0;
		ui_power_off(u);
		return;
	}
	if (!C.running)
		return;
	rem = left_ms(now);
	if (rem <= 0) {
		timer_fire(u);
		return;
	}
	if (rem > FADE_MS) {
		C.act_seen = u->touch_t;
		return;
	}
	if (!C.fading) {
		C.fading = 1;
		C.act_seen = u->touch_t;
		C.last_pct = 101;
	} else if (u->touch_t != C.act_seen) {   /* a key during the fade: they are still listening */
		C.fading = 0;
		C.running = 0;
		C.minutes = 0;
		ui_level_restore(u);
		say(u, "Sleep timer cancelled");
		u->dirty = 1;
		return;
	}
	{
		int pct = (int)(rem * 100 / FADE_MS);

		if (pct <= C.last_pct - 3 || pct == 0) {
			C.last_pct = pct;
			ui_level_scale(u, pct);
		}
	}
}

/* the next time in the list after the current one (the SLEEP key) */
static void timer_next(struct ui *u)
{
	int i;

	for (i = 0; i < NSLEEP && sleep_pre[i] != C.minutes; i++)
		;
	timer_set(u, sleep_pre[(i + 1) % NSLEEP]);
}

/* -------------------------------------------------------------------- polling --- */

static void idle_clock(struct ui *u)
{
	if (!C.idle || u->scr_dark || C.standby || u->screen == SCR_EXT || u->screen == SCR_CONFIRM || u->bl_edit)
		return;
	if (u->touch_t && ext_since(ext_now_ms(), u->touch_t) > C.idle * 1000L)
		ui_ext_open_name(u, "clock");
}

static void poll(struct ui *u)
{
	long now = ext_now_ms();
	struct tm t;

	if (!C.loaded)
		load();
	timer_tick(u, now);
	idle_clock(u);
	/* a new minute shows in the top bar; the open clock moves every second */
	if (local(&t)) {
		int clock_open = u->screen == SCR_EXT && u->ext == &ext_clock;

		if (t.tm_min != C.min_seen && (C.bar || clock_open))
			u->dirty = 1;
		if (clock_open && C.secs && t.tm_sec != C.sec_seen)
			u->dirty = 1;
		C.min_seen = t.tm_min;
		C.sec_seen = t.tm_sec;
	}
	if (C.running && ((left_ms(now) / 1000) % 60 == 0 || C.fading) && u->screen == SCR_EXT)
		u->dirty = 1;   /* the countdown on the sleep and clock screens */
}

/* "23 min" / "45 s" */
static void fmt_left(char *out, size_t cap)
{
	long s = left_ms(ext_now_ms()) / 1000;

	if (s < 0)
		s = 0;
	if (s >= 90)
		snprintf(out, cap, "%ld min", (s + 30) / 60);
	else
		snprintf(out, cap, "%ld s", s);
}

/* ---------------------------------------------------------------- the clock --- */

static void now_line(struct ui *u, char *out, size_t cap)
{
	out[0] = '\0';
	if (u->input == X45_IN_NETWORK && u->have_song) {
		const char *t = u->song.title[0] ? u->song.title : u->song.name;

		if (t[0])
			snprintf(out, cap, "%s%s%s", t, u->song.artist[0] ? " - " : "", u->song.artist);
	}
	if (!out[0])
		snprintf(out, cap, "%s", ui_input_label(u));
}

static void clock_draw(struct ui *u)
{
	struct x45_surface *s = &u->g.back;
	struct tm t;
	char hm[16], line[200];
	const char *ampm = NULL;
	int ok = local(&t), sc = 22, tw, th, tx, ty = 128;

	if (!ok) {
		const char *m = "time not set";

		x45_text(s, (s->w - x45_text_w(m, 8)) / 2, 200, m, 8, u->t.warn);
		text_fold(s, 40, 300, "The unit has not got the time yet: it needs the network for a moment.", 3, s->w - 80, u->t.dim);
		draw_hints(u, "RETURN|back");
		return;
	}
	fmt_hm(hm, sizeof hm, &t, &ampm);
	while (sc > 6 && x45_text_w(hm, sc) > s->w - 260)
		sc--;
	tw = x45_text_w(hm, sc);
	th = x45_text_h(sc);
	tx = (s->w - tw - (C.secs || ampm ? 120 : 0)) / 2;
	x45_text(s, tx, ty, hm, sc, u->t.text);
	if (C.secs) {
		snprintf(line, sizeof line, "%02d", t.tm_sec);
		x45_text(s, tx + tw + 24, ty + th - x45_text_h(8), line, 8, u->t.accent);
	}
	if (ampm)
		x45_text(s, tx + tw + 24, ty + 8, ampm, 4, u->t.dim);
	snprintf(line, sizeof line, "%s, %d %s %d", wday[t.tm_wday % 7], t.tm_mday, mon[t.tm_mon % 12], t.tm_year + 1900);
	x45_text(s, (s->w - x45_text_w(line, 4)) / 2, ty + th + 36, line, 4, u->t.dim);
	if (C.running) {
		char l[24];

		fmt_left(l, sizeof l);
		snprintf(line, sizeof line, "Sleep timer: %s left", l);
		x45_text(s, (s->w - x45_text_w(line, 3)) / 2, ty + th + 96, line, 3, u->t.accent);
	}
	now_line(u, line, sizeof line);
	text_fold(s, 40, s->h - 112, line, 3, s->w - 80, u->t.dim);
	draw_hints(u, "CLOCK|screen off;SLEEP|sleep timer;RETURN|back");
}

static int clock_ir(struct ui *u, int code)
{
	if (code == KEY_CLOCK) {   /* the key that opened it puts the light out */
		if (!u->ir_bounce)
			ui_screen_dark(u);
		return 1;
	}
	return 0;
}

static void clock_ok(struct ui *u)
{
	ui_ext_close(u);
}

static void clock_move(struct ui *u, int delta)
{
	(void)u;
	(void)delta;
}

/* the time, small, in the top bar */
static int badge(struct ui *u, int right)
{
	struct x45_surface *s = &u->g.back;
	struct tm t;
	char hm[16], b[24];
	const char *ampm;
	int w;

	if (!C.bar || (u->screen == SCR_EXT && u->ext == &ext_clock) || !local(&t))
		return 0;
	fmt_hm(hm, sizeof hm, &t, &ampm);
	snprintf(b, sizeof b, "%s%s%s", hm, ampm ? " " : "", ampm ? ampm : "");
	w = x45_text_w(b, 2);
	x45_text(s, right - w, 24, b, 2, u->t.dim);
	return w;
}

static void clock_json(struct ui *u, FILE *f)
{
	struct tm t;
	int ok = local(&t);
	int i;

	(void)u;
	fprintf(f, "{\"valid\":%d,\"epoch\":%ld,\"offset\":%ld,\"zone\":\"%s\",\"action\":\"%s\",\"bar\":%d,\"idle\":%d,\"h24\":%d,\"secs\":%d,"
		"\"sleep\":{\"minutes\":%d,\"running\":%d,\"fading\":%d,\"left\":%ld,\"asleep\":%d},\"presets\":[",
		ok, (long)time(NULL), ok ? (long)t.tm_gmtoff : 0L, ok && t.tm_zone ? t.tm_zone : "", C.off ? "off" : "standby", C.bar,
		C.idle, C.h24, C.secs, C.minutes, C.running, C.fading, C.running ? left_ms(ext_now_ms()) / 1000 : 0L, C.standby);
	for (i = 0; i < NSLEEP; i++)
		fprintf(f, "%s%d", i ? "," : "", sleep_pre[i]);
	fputs("],\"idles\":[", f);
	for (i = 0; i < NIDLE; i++)
		fprintf(f, "%s%d", i ? "," : "", idle_pre[i]);
	fputs("]}", f);
}

/* clock sleep N | action standby|off | bar 0|1 | idle SECONDS | h24 0|1 | secs 0|1 | open | dark | set EPOCH */
static int clock_cmd(struct ui *u, const char *args)
{
	char w[16] = "", rest[64] = "";
	int n;

	if (!C.loaded)
		load();
	if (sscanf(args, "%15s %63s", w, rest) < 1)
		return 0;
	n = atoi(rest);
	if (!strcmp(w, "sleep")) {
		if (!strcmp(rest, "off"))
			n = 0;
		if (n >= 0 && n <= 600)
			timer_set(u, n);
	} else if (!strcmp(w, "action")) {
		C.off = !strncmp(rest, "off", 3);
		save_config_key("sleep_action", C.off ? "off" : "standby");
	} else if (!strcmp(w, "bar")) {
		C.bar = n != 0;
		keep("clock_bar", C.bar);
	} else if (!strcmp(w, "idle")) {
		C.idle = n < 0 || n > 7200 ? 0 : n;
		keep("clock_idle", C.idle);
	} else if (!strcmp(w, "h24")) {
		C.h24 = n != 0;
		keep("clock_24h", C.h24);
	} else if (!strcmp(w, "secs")) {
		C.secs = n != 0;
		keep("clock_secs", C.secs);
	} else if (!strcmp(w, "open")) {
		ui_ext_open_name(u, "clock");
	} else if (!strcmp(w, "dark")) {
		ui_screen_dark(u);
	} else if (!strcmp(w, "set")) {
		/* only for a clock that was never set: a year before 2025 is not a time anybody kept */
		long e = atol(rest);

		if (!time_ok() && e >= TIME_FLOOR && e < 2147483647L) {   /* time_t is 32 bits here */
			struct timeval tv = { .tv_sec = (time_t)e, .tv_usec = 0 };

			if (settimeofday(&tv, NULL) == 0) {
				if (system("hwclock -w -u >/dev/null 2>&1") != 0)
					say(u, "time set (the RTC was not written)");
				else
					say(u, "time set");
			}
		}
	} else {
		return 0;
	}
	return 1;
}

/* --------------------------------------------------------- sleep / settings screen --- */

enum { R_SLEEP, R_ACTION, R_BAR, R_IDLE, R_FORMAT, R_SECS, R_SHOW, R_N };

static int sel, top;

static void enter(struct ui *u)
{
	(void)u;
	if (!C.loaded)
		load();
	sel = top = 0;
}

static int idx_of(const int *tab, int n, int v)
{
	int i;

	for (i = 0; i < n; i++)
		if (tab[i] == v)
			return i;
	return 0;
}

static void change(struct ui *u, int row, int dir)
{
	switch (row) {
	case R_SLEEP: {
		int i = idx_of(sleep_pre, NSLEEP, C.minutes);

		timer_set(u, sleep_pre[((i + dir) % NSLEEP + NSLEEP) % NSLEEP]);
		break;
	}
	case R_ACTION:
		C.off = !C.off;
		save_config_key("sleep_action", C.off ? "off" : "standby");
		break;
	case R_BAR:
		C.bar = !C.bar;
		keep("clock_bar", C.bar);
		break;
	case R_IDLE: {
		int i = idx_of(idle_pre, NIDLE, C.idle);

		C.idle = idle_pre[((i + dir) % NIDLE + NIDLE) % NIDLE];
		keep("clock_idle", C.idle);
		break;
	}
	case R_FORMAT:
		C.h24 = !C.h24;
		keep("clock_24h", C.h24);
		break;
	case R_SECS:
		C.secs = !C.secs;
		keep("clock_secs", C.secs);
		break;
	default:
		ui_ext_open_name(u, "clock");
		return;
	}
	u->dirty = 1;
}

static void sleep_move(struct ui *u, int d)
{
	(void)u;
	sel = ((sel + d) % R_N + R_N) % R_N;
}

static void sleep_nav(struct ui *u, int dx, int dy)
{
	if (dy)
		sleep_move(u, dy);
	else if (dx && sel != R_SHOW)
		change(u, sel, dx);
}

static void sleep_ok(struct ui *u)
{
	change(u, sel, 1);
}

static int sleep_ir(struct ui *u, int code)
{
	if (code == KEY_SLEEP) {   /* the key that opened it steps through the times */
		if (!u->ir_bounce)
			timer_next(u);
		return 1;
	}
	return 0;
}

static void row(struct ui *u, int i, int x, int y, int rw, int selected, void *ctx)
{
	struct x45_surface *s = &u->g.back;
	uint32_t fg = ext_fg(u, selected), vc = selected ? u->t.bg : u->t.accent;
	const char *label = "", *val = "";
	char v[40] = "";

	(void)ctx;
	switch (i) {
	case R_SLEEP:
		label = "Sleep timer";
		if (C.running) {
			char l[24];

			fmt_left(l, sizeof l);
			snprintf(v, sizeof v, "%s left", l);
		} else {
			snprintf(v, sizeof v, "off");
			vc = selected ? u->t.bg : u->t.dim;
		}
		break;
	case R_ACTION:
		label = "When the timer ends";
		snprintf(v, sizeof v, "%s", C.off ? "switch the unit off" : "stop and sleep");
		break;
	case R_BAR:
		label = "Time in the top bar";
		snprintf(v, sizeof v, "%s", C.bar ? "on" : "off");
		break;
	case R_IDLE:
		label = "Show the clock when idle";
		if (!C.idle)
			snprintf(v, sizeof v, "never");
		else
			snprintf(v, sizeof v, "after %d min", C.idle / 60);
		break;
	case R_FORMAT:
		label = "Time format";
		snprintf(v, sizeof v, "%s", C.h24 ? "24 hour" : "12 hour");
		break;
	case R_SECS:
		label = "Seconds on the clock";
		snprintf(v, sizeof v, "%s", C.secs ? "on" : "off");
		break;
	default:
		label = "Show the clock";
		snprintf(v, sizeof v, ">");
		break;
	}
	val = v;
	x45_text(s, x + 26, y + 9, label, 3, fg);
	x45_text(s, x + rw - 24 - x45_text_w(val, 3), y + 9, val, 3, vc);
}

static void sleep_draw(struct ui *u)
{
	struct x45_surface *s = &u->g.back;
	struct tm t;
	char line[120];

	if (local(&t)) {
		const char *ampm;
		char hm[16];

		fmt_hm(hm, sizeof hm, &t, &ampm);
		snprintf(line, sizeof line, "%s%s%s   %s %d %s", hm, ampm ? " " : "", ampm ? ampm : "", wday[t.tm_wday % 7], t.tm_mday,
			 mon[t.tm_mon % 12]);
	} else {
		snprintf(line, sizeof line, "time not set");
	}
	x45_text(s, 24, 72, line, 3, u->t.text);
	x45_text_clipped(s, 24, 104, "Left / right change a setting; the SLEEP key steps through the timer.", 2, s->w - 48, u->t.dim);
	ext_list(u, 12, 148, s->w - 24, 390, R_N, sel, &top, row, NULL);
	draw_hints(u, "scroll|move;left/right|change;OK|change;RETURN|back");
}

static int sleep_cmd(struct ui *u, const char *args)
{
	int n = !strncmp(args, "off", 3) ? 0 : atoi(args);

	if (n < 0 || n > 600)
		return 0;
	timer_set(u, n);
	return 1;
}

static int sleep_badge(struct ui *u, int right)
{
	struct x45_surface *s = &u->g.back;
	char l[24], b[40];
	int w;

	if (!C.running)
		return 0;
	fmt_left(l, sizeof l);
	snprintf(b, sizeof b, "zz %s", l);
	w = x45_text_w(b, 2);
	x45_text(s, right - w, 24, b, 2, C.fading ? u->t.warn : u->t.accent);
	return w;
}

const struct ui_ext ext_clock = {
	.name = "clock",
	.title = "CLOCK",
	.menu = NULL,
	.draw = clock_draw,
	.ok = clock_ok,
	.move = clock_move,
	.ir = clock_ir,
	.poll = poll,
	.badge = badge,
	.json = clock_json,
	.cmd = clock_cmd,
};

const struct ui_ext ext_sleep = {
	.name = "sleep",
	.title = "SLEEP TIMER",
	.menu = "Clock and sleep timer",
	.enter = enter,
	.draw = sleep_draw,
	.ok = sleep_ok,
	.move = sleep_move,
	.nav = sleep_nav,
	.ir = sleep_ir,
	.badge = sleep_badge,
	.cmd = sleep_cmd,
};
