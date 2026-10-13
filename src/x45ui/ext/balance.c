#define _GNU_SOURCE
/* ext/balance - the balance control: how much louder one side plays than the other.
 *
 * The setting (u->balance, -24..+24, positive = right) and the arithmetic live elsewhere (main.c applies it with
 * every volume change, libx45 turns it into the two channel levels); this is only the screen. It opens from
 * Options > Sound > Balance, from the player menu (RETURN on the front panel, MENU on the remote) and from the
 * remote's "Vol balance" key, which also closes it again. Left/right and the knob step it, 0 centres it, OK or
 * RETURN is done; the volume keys keep adjusting the volume so that the effect can be heard while it is set.
 */
#include "ext.h"
#include "../../libx45/x45.h"

#include <stdio.h>
#include <stdlib.h>

#define KEY_BALANCE 466   /* the remote's "Vol balance" button (docs/X45-REMOTE.md) */
#define KEY_LEFT    105
#define KEY_RIGHT   106
#define KEY_NUM0    512

extern const struct ui_ext ext_balance;

/* the side and the number of steps, as words */
static void words(int b, char *out, size_t cap)
{
	if (b == 0)
		snprintf(out, cap, "CENTRE");
	else
		snprintf(out, cap, "%s %d", b > 0 ? "RIGHT" : "LEFT", abs(b));
}

static void step(struct ui *u, int d)
{
	ui_set_balance(u, u->balance + d, 0);
}

static void leave(struct ui *u)
{
	ui_set_balance(u, u->balance, 1);   /* kept once, however many steps were taken */
}

static void draw(struct ui *u)
{
	struct x45_surface *s = &u->g.back;
	const int bx = 24, bw = s->w - 48, by = 232, bh = 44;
	int cx = bx + bw / 2, half = bw / 2 - 4, len, i, l, r, vol;
	char buf[96];
	uint32_t col = u->balance ? u->t.accent : u->t.text;

	x45_text(s, 24, 72, "BALANCE", 3, u->t.text);
	x45_text_clipped(s, 24, 104, "One step is one volume click, taken from one side and given to the other.", 2,
			 s->w - 48, u->t.dim);

	words(u->balance, buf, sizeof buf);
	x45_text(s, (s->w - x45_text_w(buf, 6)) / 2, 146, buf, 6, col);

	/* the scale: a bar from the middle towards the louder side, a tick for each step */
	x45_fill_rect(s, bx, by, bw, bh, u->t.barbg);
	x45_draw_rect(s, bx, by, bw, bh, u->t.line);
	len = half * abs(u->balance) / X45_BALANCE_MAX;
	if (len > 0)
		x45_fill_rect(s, u->balance > 0 ? cx : cx - len, by + 3, len, bh - 6, u->t.barfg);
	for (i = -X45_BALANCE_MAX; i <= X45_BALANCE_MAX; i++) {
		int tx = cx + half * i / X45_BALANCE_MAX, th = i % 6 == 0 ? 14 : 6;

		x45_fill_rect(s, tx, by + bh + 4, 1, th, i == u->balance ? u->t.accent : u->t.line);
	}
	x45_fill_rect(s, cx - 1, by - 8, 3, bh + 16, u->t.text);
	x45_text(s, bx, by + bh + 28, "L", 3, u->balance < 0 ? u->t.accent : u->t.dim);
	x45_text(s, bx + bw - x45_text_w("R", 3), by + bh + 28, "R", 3, u->balance > 0 ? u->t.accent : u->t.dim);

	/* what the two sides get at the volume the unit is playing now */
	vol = ui_volume_locked(u) ? 100 : u->volume;
	x45_balance_levels(vol, u->balance, &l, &r);
	snprintf(buf, sizeof buf, "left %d    right %d    (volume %d)", l, r, vol);
	x45_text(s, (s->w - x45_text_w(buf, 2)) / 2, by + bh + 78, buf, 2, u->t.dim);

	if (ui_volume_locked(u))
		x45_text_clipped(s, 24, by + bh + 112, "Fixed volume: the louder side stays at full level, the other is turned down.", 2,
				 s->w - 48, u->t.dim);
	draw_hints(u, "scroll|adjust;0|centre;OK|done;RETURN|done");
}

static void ok(struct ui *u)
{
	ui_ext_close(u);
}

static void move(struct ui *u, int delta)
{
	step(u, delta);
}

static void nav(struct ui *u, int dx, int dy)
{
	if (dx)
		step(u, dx);
	(void)dy;
}

static int ir(struct ui *u, int code)
{
	if (code == KEY_BALANCE) {   /* the key that opened it closes it */
		if (!u->ir_bounce)
			ui_ext_close(u);
		return 1;
	}
	if (code == KEY_NUM0) {
		ui_set_balance(u, 0, 0);
		return 1;
	}
	return 0;
}

/* a held left/right goes on (main.c has already slowed the repeat down to a rate a person can follow) */
static int hold(struct ui *u, int code)
{
	if (code != KEY_LEFT && code != KEY_RIGHT)
		return 0;
	step(u, code == KEY_LEFT ? -1 : 1);
	return 1;
}

const struct ui_ext ext_balance = {
	.name = "balance",
	.title = "BALANCE",
	.menu = NULL,   /* listed under Sound, not at the top of Options */
	.leave = leave,
	.draw = draw,
	.ok = ok,
	.move = move,
	.nav = nav,
	.ir = ir,
	.hold = hold,
};
