/* ext - screens of the panel UI that live in files of their own.
 *
 * A screen is a `struct ui_ext` in ext/NAME.c; listing it in ext.c's table is the whole of its
 * registration. main.c knows nothing of any single screen: it routes keys, draws and polls through the
 * functions below while u->screen == SCR_EXT. A screen keeps its own state in static variables of its file.
 *
 *   name    key used by the web bridge (`ext wifi`) and by ui_ext_open_name()
 *   title   heading drawn by the screen itself (the header bar is drawn by main.c)
 *   menu    label in the Options list (NULL: not listed there; open it from code instead)
 *   enter   the screen became visible          leave   it is no longer visible (also when left by HOME)
 *   draw    paint the screen below the header  ok      OK / knob push
 *   back    RETURN (NULL: close the screen)    move    knob or up/down by `delta` steps
 *   nav     remote arrows: dx/dy (NULL: dy is passed to move)
 *   ir      any other remote key (a Linux key code): return 1 when the screen used it, 0 to let the UI act on it
 *   hold    a remote key held down (arrives at a person's pace, ~5 per second after half a second): 1 when used
 *   poll   the main loop, every pass, also while the screen is closed (cheap, please)
 *   badge   a symbol in the top bar, drawn whatever screen is up: `right` is the free edge, the return value
 *           the width it took (0: nothing to show)
 *   json    write one JSON value (an object) for the web state; it appears there as "<name>": value
 *   filter  every key, knob turn and remote key, before the UI sees it: return 1 to swallow it (the child lock); the
 *           web bridge sends its events under the name "web" whose first word is `name`: `args` is the rest of the line (the web page's
 *           POST /cmd, docs/X45-API.md); return 1 when it was understood
 */
#ifndef X45UI_EXT_H
#define X45UI_EXT_H

#include "../ui.h"

#include <stdio.h>

struct ui_ext {
	const char *name, *title, *menu;
	void (*enter)(struct ui *u);
	void (*leave)(struct ui *u);
	void (*draw)(struct ui *u);
	void (*ok)(struct ui *u);
	void (*back)(struct ui *u);
	void (*move)(struct ui *u, int delta);
	void (*nav)(struct ui *u, int dx, int dy);
	int (*ir)(struct ui *u, int code);
	int (*hold)(struct ui *u, int code);
	void (*poll)(struct ui *u);
	int (*badge)(struct ui *u, int right);
	void (*json)(struct ui *u, FILE *f);
	int (*cmd)(struct ui *u, const char *args);
	int (*filter)(struct ui *u, const struct x45_event *ev);
};

/* ---- called by main.c ---- */
void ui_ext_open(struct ui *u, const struct ui_ext *e);
int ui_ext_open_name(struct ui *u, const char *name);   /* 0, or -1 when there is no such screen */
void ui_ext_close(struct ui *u);                        /* back to where it was opened from */
void ui_ext_draw(struct ui *u);
void ui_ext_ok(struct ui *u);
void ui_ext_back(struct ui *u);
void ui_ext_move(struct ui *u, int delta);
void ui_ext_nav(struct ui *u, int dx, int dy);
int ui_ext_ir(struct ui *u, int code);                  /* 1: the open screen took the key */
int ui_ext_hold(struct ui *u, int code);                /* 1: the open screen takes the key held down (it has a `hold`) */
int ui_ext_wants_hold(const struct ui *u);              /* the open screen has a `hold` */
void ui_ext_poll(struct ui *u);
int ui_ext_badges(struct ui *u, int right);              /* top bar: returns the new free edge */
void ui_ext_state(struct ui *u, FILE *f);               /* web state: `,"name":{...}` for every screen with a json hook */
int ui_ext_cmd(struct ui *u, const char *name, const char *args);   /* 1: a screen took the web command */
int ui_ext_filter(struct ui *u, const struct x45_event *ev);   /* 1: a screen swallowed the input event */
int ui_ext_menu_count(void);                            /* screens listed in Options */
const struct ui_ext *ui_ext_menu_at(int i);

/* the picture of what plays (ext/art.c), or NULL */
const struct x45_image *ui_art_image(const struct ui *u);
const struct x45_image *ui_art_small(const struct ui *u);   /* the same, in a box of about 160 px */

/* the Bluetooth output (ext/btout.c) */
void ui_btout_release(struct ui *u);            /* give the radio stick up (the Bluetooth input wants it) */
const char *ui_btout_connected_name(void);      /* the headset the sound goes to (its name), NULL when none is connected */
int ui_btout_routed(void);                      /* the sound goes to the device: the DAC (and the SSI capture behind it) hears nothing */
void ui_btout_set_volume(int v);                /* the headset's level, 0..100 (reaches the headset through AVRCP) */
int ui_btout_busy(void);                       /* the output's helper is running */
void ui_btout_web(struct ui *u, const char *cmd);   /* on | off | scan | stopscan | connect ADDR | disconnect | forget ADDR */
void ui_btout_json(struct ui *u, FILE *f);      /* the state, one JSON object */

/* the song log (ext/songlog.c) */
void ui_songlog_mark_now(struct ui *u);         /* put a star on the song that is on air (the remote's RED key) */

/* ---- for the screens ---- */
long ext_now_ms(void);
/* ms from t to now. A long is 32 bits on this machine, so ext_now_ms() wraps (negative after 24.8 days of uptime):
 * compare a deadline through this, never by the size of the two values. */
long ext_since(long now, long t);

/* a setting of etc/x45ui.conf (the file main.c keeps with save_config_key): copies the value of `key` into out, 0 when
 * the key is not there. The screens read their own keys with it; main.c's loader skips what it does not know. */
int ext_conf_get(const char *key, char *out, size_t cap);
int ext_conf_int(const char *key, int def);

#define EXT_ROW_H 40

/* Draws row i inside (x, y, w, EXT_ROW_H); the highlight bar is already there when `selected`, in which case
 * text goes in u->t.bg (see ext_fg) rather than u->t.text. */
typedef void (*ext_row_fn)(struct ui *u, int i, int x, int y, int w, int selected, void *ctx);

/* A scrolling list of n rows in the area x, y, w, h; `sel` is highlighted, *top (kept in range here) is the
 * first row shown. Draws a scroll bar when the rows do not all fit. Returns the number of rows that fit. */
int ext_list(struct ui *u, int x, int y, int w, int h, int n, int sel, int *top, ext_row_fn row, void *ctx);
uint32_t ext_fg(const struct ui *u, int selected);   /* text colour on a row */

/* a block of text lines, wrapped on spaces to w pixels at `scale`; returns the y below it */
int ext_para(struct ui *u, int x, int y, int w, const char *text, int scale, uint32_t col);

/* signal strength as four bars in a 28 x 22 box at (x, y); lvl 0..4 */
void ext_bars(struct ui *u, int x, int y, int lvl, uint32_t on, uint32_t off);

#endif /* X45UI_EXT_H */
