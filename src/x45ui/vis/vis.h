/* vis - the visuals of the panel: level bars, analog dials, spectrum, waterfall ... one file each.
 *
 * A visual is a `const struct ui_vis` in vis/NAME.c; listing it in vis/vis.c is the whole of its registration.
 * The owner switches them on and off per input (or for all inputs) in the RETURN menu, two at a time at most
 * (a third takes the place of the oldest); whatever is on shares the box a screen gives them (draw_visuals_rect):
 * one alone fills all of it, two split the height by weight, each within the tallest it needs. The Options row,
 * the menu, the saved masks and the layout all read the registry, so a new visual is a new file and one line.
 *
 * To add a visual: vis/NAME.c with the struct and a draw function, `extern` + one pointer in vis/vis.c.
 * Give it a bit of its own in `bit` (the masks are saved as numbers: never reuse or change one that exists).
 */
#ifndef X45UI_VIS_H
#define X45UI_VIS_H

#include "../ui.h"

#include <stdlib.h>
#include <string.h>

#define NSB          128     /* bands in the analyzer file */
#define SPEC_FLOOR   -84.0f  /* bottom of the spectrum bars, dBFS */
#define METER_FLOOR  -60.0f  /* bottom of the level bars, dBFS */

struct ui_vis {
	const char *name;        /* "bars" */
	const char *label;       /* what the menu says: "Level bars" */
	unsigned short bit;      /* its bit in the saved masks (vis_<input> keys); unique, never changed */
	int weight;              /* its share of the height among the visuals that are on */
	int (*cap)(int w);       /* the tallest it needs in a box this wide (px); NULL: whatever it gets */
	int bands;               /* only with the analyzer's bands (sp_have); the level bars also run on the panel meter. Brings the
	                          * Options row for the number of bands: a visual that does not use it sets `analyzer` instead */
	int analyzer;            /* needs the analyzer (sp_have) but not the choice of how many bands are shown */
	int wave;                /* needs the analyzer's picture of the wave (sp_wave): the scopes */
	int side;                /* needs the bands of the side signal (sp_side): an analyzer older than 2026-10-10 does not write them */
	int two_pass;            /* has a part that never changes (a dial's scale): draw() is called first with
	                          * u->dial_pass 1 (only that part), then each frame with 2 (only what moves) */
	void (*frame)(struct ui *u, long now, int on);   /* once per drawn frame before drawing, in pass 0 and 2 only */
	void (*feed)(struct ui *u);                      /* once per analyzer frame, shown or not (keeps a history) */
	void (*draw)(struct ui *u, int x, int y, int w, int h);
	void (*reset)(void);     /* forget what draw() keeps in the file between frames (peak holds, gains): the previews of
	                          * the menu draw it on made-up music and call this afterwards. Not needed when it keeps none */
	void (*demo)(struct ui *u);   /* for the picture in the menu: fill what it keeps in u->vm with made-up music, when feeding
	                               * the demo music through feed() for a few seconds is not enough (a four-minute history) */
	int square;              /* its picture is a square at the left of its box: a preview puts it in the middle */
	unsigned look;           /* the look options it has of its own (VIS_LOOK_*); the analyzer's bands come from `bands` */
};

/* The look options (Options > Visualizers) that belong to some visuals. The list shows only those of the visuals that
 * are switched on right now: vis_looks() is their union. */
#define VIS_LOOK_BANDS    1      /* the number of bands: every visual with `bands` */
#define VIS_LOOK_COLOURS  2      /* the bar colours */
#define VIS_LOOK_FIELD    4      /* the stereo field's look, trace colour, bars and size */
#define VIS_LOOK_STYLE    8      /* the spectrum's style (bars, LED cells, mirrored, curve) */
#define VIS_GON_BY_DISTANCE 8    /* gon_col: the trace takes the bar colours, by its distance from the middle */

/* The other styles of the spectrum (led.c, mirror.c, area.c): the same bands drawn differently, drawn by spectrum.c and not
 * listed in vis.c. Their bits (64, 128, 256) are what older saved settings hold, so they stay taken. */
extern const struct ui_vis vis_led, vis_mirror, vis_area;

int vis_count(void);
const struct ui_vis *vis_at(int i);
/* what is switched on is a struct vis_sel (ui.h): at most VIS_MAX_ON visuals, and a third switched on takes the
 * place of the oldest. It is kept per input, or once for all of them (Options > Visualizers). */
#define VIS_MAX_ON 2
unsigned vis_get(const struct ui *u);                          /* the bits that are on: for all inputs or this one */
unsigned vis_looks(const struct ui *u);                        /* the VIS_LOOK_* options that the ones on make use of */
struct vis_sel *vis_sel_of(struct ui *u);                      /* the setting the menu edits */
unsigned vis_switch(struct vis_sel *s, unsigned bit);          /* flip one; returns the bit switched off to make room, or 0 */
unsigned long vis_pack(const struct vis_sel *s);               /* as saved in x45ui.conf */
/* Returns the spectrum style an older setting implies, or -1: LED, mirror and curve were once visuals of their own
 * (bits 64, 128, 256) and are now styles of the spectrum; such a setting is read as "spectrum on". */
int vis_unpack(struct vis_sel *s, unsigned long v);
/* ---- called by main.c ---- */
void vis_feed(struct ui *u);                                   /* a new analyzer frame is in u->sp_b */
void draw_visuals(struct ui *u, int x, int y, int w, int h);   /* what is on for the input, laid out in the box */

/* ---- the large stereo field (vis/scopepage.c) ---- */
struct ui_np;
int vis_big(const struct ui *u);                               /* the player shows the stereo field almost fullscreen (gon_big, no bars) */
void scope_page(struct ui *u, const struct ui_np *np);         /* that page: a small text column and the scope */

/* ---- the menu of the visuals, each with a still picture (vis/preview.c) ---- */
void vis_menu_draw(struct ui *u);                              /* the cards; u->sel is the one under the cursor */
int vis_menu_cols(void);                                       /* cards in a row, for the cursor keys */

/* ---- what visuals share (vis/vis.c) ---- */
long vis_now(void);                                            /* a monotonic millisecond clock */
void vis_level(const struct ui *u, int ch, float *lvl, float *pk);   /* RMS and held peak, dBFS */
int vis_db_x(float db, int w);                                 /* a level as a position on a bar w px wide */
/* the colour of a bar, column or cell that reaches f (0..1) of its height: the owner's choice, Options > Bar colours */
uint32_t vis_bar_colour(struct ui *u, float f);
int vis_bar_colours(void);
const char *vis_bar_colour_name(int i);
/* the spectrum's styles (vis/spectrum.c): the same bands as bars, LED cells, a mirrored picture or a curve */
int vis_spec_styles(void);
const char *vis_spec_style_name(int i);
/* the stereo field's own choices (vis/gonio.c): its look, and the colour of its trace; Options > Visualizers */
int vis_gonio_looks(void);
const char *vis_gonio_look_name(int i);
int vis_gonio_colours(void);
const char *vis_gonio_colour_name(int i);
void vis_rebin(const float *src, float *dst, int nb);       /* the analyzer's NSB bands folded into nb */
/* where the analyzer's bands are: band i covers 20 Hz .. fmax in NSB log-spaced steps (fmax is 20 kHz, less on a low sample rate) */
float vis_band_hz(const struct ui *u, int i);                  /* the centre of band i, Hz */
int vis_band_of(const struct ui *u, float hz);                 /* the band that holds hz */
float vis_pow(float db);                                       /* dB to a power ratio */
/* The visuals that keep a history (vis/mem.h) call this once per analyzer frame with the fields input, t_ok and t of their memory.
 * Returns the ms since their last frame, or -1 when they must start afresh: the first frame, another input, or the analyzer
 * was away for more than 3 s (the playback stopped and was started again). */
long vis_step(const struct ui *u, int *input, int *t_ok, long *t);
#define VIS_MEM_STEP(u, m, dt) do { \
		(dt) = vis_step((u), &(m)->input, &(m)->t_ok, &(m)->t); \
		if ((dt) < 0) { \
			int in_ = (m)->input; \
			long t_ = (m)->t; \
			memset((m), 0, sizeof *(m)); \
			(m)->input = in_; \
			(m)->t = t_; \
			(m)->t_ok = 1; \
		} \
	} while (0)
float vis_band_f(float db);                                    /* a band level as the height of a bar, 0..1 */
void vis_line(struct x45_surface *s, int x0, int y0, int x1, int y1, int th, uint32_t c);
uint32_t vis_mix(uint32_t a, uint32_t b, int pct);             /* a with pct percent of b */

#endif /* X45UI_VIS_H */
