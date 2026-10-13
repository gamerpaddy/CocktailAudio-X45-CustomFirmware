/* osk - an on-screen keyboard for the panel: the knob steps through the keys, OK presses the one
 * under the cursor, the remote's arrows move in two dimensions. Four rows of ten characters (letters,
 * capitals or symbols, digits always on the last row) and a row of function keys. */
#ifndef X45UI_OSK_H
#define X45UI_OSK_H

#include "../ui.h"

#define OSK_MAX 72

struct osk {
	char buf[OSK_MAX];
	char prompt[64];
	int len, max;       /* max: longest text accepted (< OSK_MAX) */
	int layer;          /* 0 small letters, 1 capitals, 2 symbols */
	int cur;            /* the key under the cursor: 0..39 characters, 40..45 function keys */
	int show;           /* the text is shown (1) or hidden behind dots (0) */
};

enum { OSK_EDIT, OSK_DONE };

void osk_open(struct osk *o, const char *prompt, const char *initial, int max, int show);
void osk_draw(struct ui *u, struct osk *o);
void osk_move(struct osk *o, int delta);          /* the knob */
void osk_nav(struct osk *o, int dx, int dy);      /* the remote's arrows */
int osk_press(struct osk *o);                     /* OK: OSK_DONE when the Done key was pressed */

#endif
