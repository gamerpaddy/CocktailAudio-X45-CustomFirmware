#define _GNU_SOURCE
/* scopepage - the player with the stereo field almost fullscreen (Options > Visualizers > Stereo field size).
 *
 * The scope takes the height of the panel under the bar, as a square at the right edge; what the skin would have put
 * across the page (kicker, title, artist, album, format, time, progress, state) becomes a narrow column of smaller
 * type at the left, so that nothing runs under the picture. The page is the same for every skin: it uses the
 * skin's colours and bar, not its arrangement. The scope itself is vis/gonio.c, drawn through draw_visuals_rect
 * (vis_big() in vis.c hands it the whole box), so the frames in which only the trace moves are made as for any visual:
 * the column is drawn first and kept as the background of the box. It needs the bars left out (gon_nobars): with them
 * the scope is the 440 px picture the box of a skin gives it. */
#include "vis.h"
#include "../skins/skin.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define TOP 62          /* under the bar (56 px) */
#define MARGIN 12

void scope_page(struct ui *u, const struct ui_np *np)
{
	struct x45_surface *s = &u->g.back;
	const int side = s->h - TOP - MARGIN, sx = s->w - MARGIN - side, x0 = 24, cw = sx - 16 - x0, bottom = TOP + side;
	char up[48];
	int y = TOP + 10, i;

	if (np->kicker) {
		for (i = 0; np->kicker[i] && i < (int)sizeof up - 1; i++)
			up[i] = (char)toupper((unsigned char)np->kicker[i]);
		up[i] = '\0';
		x45_text_clipped(s, x0, y, up, 2, cw, u->t.dim);
	}
	y += 30;
	if (np->big_title) {   /* a frequency: large and not running */
		int sc = skin_text_scale_fit(np->title, cw, 5, 2);

		x45_text(s, x0, y, np->title, sc, u->t.text);
		y += x45_text_h(sc) + 14;
	} else {
		text_scroll(u, 0, x0, y, np->title, 3, cw, u->t.text);
		y += 36;
	}
	if (np->sub[0]) {
		text_scroll(u, 1, x0, y, np->sub, 2, cw, u->t.accent);
		y += 26;
	}
	if (np->extra[0]) {
		text_scroll(u, 2, x0, y, np->extra, 2, cw, u->t.dim);
		y += 26;
	}
	if (np->fmt[0])
		x45_text_clipped(s, x0, y + 6, np->fmt, 2, cw, u->t.dim);

	/* the lower part keeps its place from the foot, whatever is above it */
	if (np->msg && np->msg[0])
		text_fold(s, x0, bottom - 188, np->msg, 2, cw, np->msg_warn ? u->t.warn : u->t.dim);
	if (np->time[0])
		x45_text_clipped(s, x0, bottom - 138, np->time, 3, cw, u->t.text);
	if (np->frac >= 0)
		x45_bar(s, x0, bottom - 98, cw, 16, np->frac, u->t.barfg, u->t.barbg, u->t.line);
	if (np->state[0])
		x45_text_clipped(s, x0, bottom - 64, np->state, 2, cw, np->playing ? u->t.accent : u->t.dim);
	if (np->flags[0])
		x45_text_clipped(s, x0, bottom - 36, np->flags, 2, cw, u->t.dim);

	draw_visuals_rect(u, sx, TOP, side, side);
}
