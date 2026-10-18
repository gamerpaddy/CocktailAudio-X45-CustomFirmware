#define _GNU_SOURCE
/* ext/childlock - the child lock of the panel and the remote.
 *
 * Locked, the unit ignores its buttons, the knob and the remote: nothing can be stopped, switched or powered off by
 * accident. The web page and the apps (the bridge's events carry the name "web") keep working, so whoever has the
 * page can still do everything and unlock. On the unit itself the lock ends when OK (the knob's push or the remote's
 * OK) is held for three seconds.
 *
 * The lock is a file (etc/childlock), so it outlives a restart of the UI and a reboot. Options, Child lock asks and
 * locks; the web page and `childlock 1|0` do the same.
 *
 * etc/x45ui.conf:  childlock_volume = 1   keep the volume knob and the remote's volume keys working while locked
 */
#include "ext.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../libx45/x45in.h"

extern const struct ui_ext ext_childlock;

#define LOCK_FILE "/home/root/x45new/etc/childlock"
#define HOLD_MS 3000L
#define REPEAT_MS 700L   /* a remote key held down repeats faster than this */
#define IR_ENTER 28
#define IR_MUTE 60
#define IR_VOLDOWN 61
#define IR_VOLUP 62

static int loaded, locked, vol_ok;
static long ok_since, ok_last;
static int ok_is_key;

static void load(void)
{
	loaded = 1;
	locked = access(LOCK_FILE, F_OK) == 0;
	vol_ok = ext_conf_int("childlock_volume", 0) != 0;
}

static void set_lock(struct ui *u, int on)
{
	if (!loaded)
		load();
	on = on != 0;
	if (on == locked)
		return;
	locked = on;
	ok_since = 0;
	if (on) {
		FILE *f = fopen(LOCK_FILE, "w");

		if (f)
			fclose(f);
		u->menu_down = u->stop_down = 0;   /* a button held while locking must not act on its release */
		vol_ok = ext_conf_int("childlock_volume", 0) != 0;
	} else {
		unlink(LOCK_FILE);
	}
	u->dirty = 1;
}

static int filter(struct ui *u, const struct x45_event *ev)
{
	long now = ext_now_ms();
	int is_ok_key = ev->kind == X45_EV_KEY && ev->code == X45_KEY_OK;
	int is_ok_ir = ev->kind == X45_EV_IR && ev->code == IR_ENTER;

	if (!loaded)
		load();
	if (!locked || (ev->name && !strcmp(ev->name, "web")))
		return 0;
	if (vol_ok) {
		if (ev->kind == X45_EV_ENCODER && ev->code == X45_KNOB_VOLUME)
			return 0;
		if (ev->kind == X45_EV_IR && (ev->code == IR_VOLUP || ev->code == IR_VOLDOWN || ev->code == IR_MUTE))
			return 0;
	}
	if (is_ok_key) {
		if (ev->pressed) {
			if (!ok_since || !ok_is_key) {
				ok_since = now;
				ok_is_key = 1;
			}
			ok_last = now;
		} else {
			ok_since = 0;
		}
	} else if (is_ok_ir) {
		if (ev->pressed == 1) {
			ok_since = now;
			ok_is_key = 0;
			ok_last = now;
		} else if (ev->pressed == 2 && ok_since && !ok_is_key) {
			ok_last = now;
		} else if (ev->pressed == 0) {
			ok_since = 0;
		}
	} else {
		ok_since = 0;
	}
	/* one reminder per press (the knob gives one event per click: a reminder per second is enough) */
	if ((ev->kind == X45_EV_ENCODER) || ((ev->kind == X45_EV_KEY || ev->kind == X45_EV_IR) && ev->pressed == 1)) {
		static long said;

		if (ext_since(now, said) > 1000 || !said) {
			said = now ? now : 1;
			say(u, "Child lock - hold OK for 3 seconds to unlock");
		}
	}
	return 1;
}

static void poll(struct ui *u)
{
	long now;

	if (!loaded)
		load();
	if (!locked || !ok_since)
		return;
	now = ext_now_ms();
	if (ext_since(now, ok_since) < HOLD_MS)
		return;
	if (!ok_is_key && ext_since(now, ok_last) > REPEAT_MS) {   /* the remote key was let go: no release arrives */
		ok_since = 0;
		return;
	}
	set_lock(u, 0);
	say(u, "Unlocked");
}

static int badge(struct ui *u, int right)
{
	int w;

	if (!locked)
		return 0;
	w = x45_text_w("LOCK", 2);
	x45_text(&u->g.back, right - w, 24, "LOCK", 2, u->t.warn);
	return w;
}

static void enter(struct ui *u)
{
	(void)u;
	if (!loaded)
		load();
}

static void draw(struct ui *u)
{
	struct x45_surface *s = &u->g.back;

	x45_text(s, 24, 70, "Lock the panel and the remote?", 3, u->t.text);
	ext_para(u, 24, 150, s->w - 48,
		 "The buttons, the knob and the remote do nothing until the lock is taken off: hold OK for three seconds. "
		 "The web page keeps working and can unlock too.", 3, u->t.dim);
	draw_hints(u, "OK|lock;RETURN|cancel");
}

static void ok(struct ui *u)
{
	set_lock(u, 1);
	ui_ext_close(u);
	say(u, "Locked - hold OK for 3 seconds to unlock");
}

static void json(struct ui *u, FILE *f)
{
	(void)u;
	if (!loaded)
		load();
	fprintf(f, "{\"locked\":%d,\"volume\":%d}", locked, vol_ok);
}

/* childlock 1 | 0 | volume 1 | volume 0 */
static int cmd(struct ui *u, const char *args)
{
	char w[16] = "";
	int v = 0;

	if (!loaded)
		load();
	if (sscanf(args, "%15s %d", w, &v) < 1)
		return 0;
	if (!strcmp(w, "1") || !strcmp(w, "0")) {
		set_lock(u, w[0] == '1');
		return 1;
	}
	if (!strcmp(w, "probe")) {   /* test aid: runs an event of the unit's own hardware (not "web") through the filter */
		char kind[12] = "";
		int pressed = 1;
		struct x45_event ev;
		FILE *f;
		int r;

		sscanf(args, "%*s %11s %d", kind, &pressed);
		memset(&ev, 0, sizeof ev);
		ev.pressed = pressed;
		ev.ticks = 1;
		if (!strcmp(kind, "okkey")) {
			ev.kind = X45_EV_KEY; ev.code = X45_KEY_OK;
		} else if (!strcmp(kind, "okir")) {
			ev.kind = X45_EV_IR; ev.code = IR_ENTER;
		} else if (!strcmp(kind, "power")) {
			ev.kind = X45_EV_KEY; ev.code = X45_KEY_POWER;
		} else if (!strcmp(kind, "knob")) {
			ev.kind = X45_EV_ENCODER; ev.code = X45_KNOB_SCROLL;
		} else if (!strcmp(kind, "vol")) {
			ev.kind = X45_EV_ENCODER; ev.code = X45_KNOB_VOLUME;
		} else if (!strcmp(kind, "volir")) {
			ev.kind = X45_EV_IR; ev.code = IR_VOLUP;
		} else if (!strcmp(kind, "web")) {
			ev.kind = X45_EV_KEY; ev.code = X45_KEY_POWER; ev.name = "web";
		} else {
			return 0;
		}
		r = filter(u, &ev);
		f = fopen("/tmp/x45childlock.probe", "a");
		if (f) {
			fprintf(f, "%s %d %s locked=%d\n", kind, pressed, r ? "swallowed" : "passed", locked);
			fclose(f);
		}
		return 1;
	}
	if (!strcmp(w, "volume")) {
		char b[4];

		vol_ok = v != 0;
		snprintf(b, sizeof b, "%d", vol_ok);
		save_config_key("childlock_volume", b);
		return 1;
	}
	return 0;
}

const struct ui_ext ext_childlock = {
	.name = "childlock",
	.title = "CHILD LOCK",
	.menu = "Child lock",
	.enter = enter,
	.draw = draw,
	.ok = ok,
	.poll = poll,
	.badge = badge,
	.json = json,
	.cmd = cmd,
	.filter = filter,
};
