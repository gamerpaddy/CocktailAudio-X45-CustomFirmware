/* skin - the look of the panel: its colours, the bar at the top, the lists, the now-playing page and the key hints.
 *
 * A skin is a `const struct ui_skin` in skins/NAME.c; listing it in skins/skins.c is the whole of its registration.
 * What a skin leaves NULL falls back to the classic look, so a skin may change one thing (only the palette) or
 * everything (a list that is a carousel). main.c draws none of these itself any more: it says what there is to
 * show (a struct ui_list, a struct ui_np) and the skin lays it out.
 *
 * A skin is a look, not a colour: the accent, the background and the text colour are the owner's own choice (Options,
 * skins/colours.c) and every skin works with every combination; a skin only brings the colours it starts from.
 *
 *   palette   bg panel line text dim barbg card, as 0..255 triples; `tint` pulls them towards the accent colour
 *   bar       the title bar across the top (56 px high; the symbols at its right edge are drawn by main.c)
 *   list      any list of rows: browser, stations, tracks, inputs, settings
 *   np        the now-playing page: the same facts about the source in another arrangement
 *   hints     the keys that work, along the foot
 *
 * To add a skin: skins/NAME.c, one line in skins/skins.c. To add a way of showing lists, write the function in
 * the skin file that needs it; other skins can use it by naming it (see the extern block below).
 */
#ifndef X45UI_SKIN_H
#define X45UI_SKIN_H

#include "../ui.h"

/* ---------------------------------------------------------------- what a list is --- */

struct ui_item {
	char lead[24];         /* a short fixed column before the label ("01  3:42") */
	char label[128];       /* UTF-8 as it came; draw it through text_fold / text_scroll */
	char value[48];        /* a setting's value, drawn apart from the label */
	const char *icon;      /* ICON_DIR name, or NULL */
	uint8_t dir;           /* a folder */
	uint8_t action;        /* a command rather than content: quiet */
	char mark[4];          /* a short mark at the right edge: the track that runs (">" or "||") */
};

enum { LK_TEXT, LK_ICONS, LK_SETTINGS };   /* names, things with a picture, labels with values */

struct ui_list {
	int kind;
	const char *title;     /* a small heading under the bar (a path, a count), or NULL */
	int title_kind;        /* 0 quiet, 1 warning, 2 accent */
	const char *note;      /* a remark at the right of the heading, warning colour, or NULL */
	const char *msg;       /* what to say instead of rows when there are none, or NULL */
	int msg_kind;          /* 0 quiet, 1 warning, 2 accent */
	int n, sel;
	int current;           /* the row that is in force (the input that plays, the option being set), or -1 */
	int *top;              /* the first row of the window, which the skin keeps in range; NULL when every row fits */
	int lead_w;            /* width of the lead column, 0 when the items have none */
	int marks;             /* folders are marked, so every label leaves room for the mark */
	int scroll_sel;        /* the selected label may run when it is too long */
	void (*item)(struct ui *u, void *ctx, int i, struct ui_item *it);   /* fills row i; called for the visible ones only */
	void *ctx;
	const char *hint;      /* "KEY|what;..." for the foot */
};

/* ------------------------------------------------------------ what is playing --- */

struct ui_np {
	const char *kicker;    /* where it comes from, in capitals: "INTERNET RADIO", "CD", "DAB+" */
	const char *lab[3];    /* names for the three lines where a skin names its fields (NULL: TITLE, ARTIST, ALBUM) */
	char title[160];       /* what is playing / the station; UTF-8 as it came */
	char sub[160];         /* the artist / what the station says: the accent line */
	char extra[160];       /* the album / the ensemble / the radiotext: the quiet line */
	int big_title;         /* the title is a number (an FM frequency): set it large and not running */
	char fmt[96];          /* the audio format */
	char time[48];         /* "1:23 / 4:56", "LIVE   3:53" or "" */
	char state[64];        /* "PLAYING   3 / 12" */
	char flags[48];        /* "shuffle  repeat" */
	int playing;           /* the state is in the accent colour */
	double frac;           /* progress 0..1, below 0 when there is none (a live stream) */
	const struct x45_image *slide;   /* the DAB slideshow picture, or NULL */
	const char *msg;       /* an error or status line, or NULL */
	int msg_warn;
	const char *hint;      /* the keys, for the foot; drawn by main.c when no analyzer has taken the place */
};

/* --------------------------------------------------------------------- a skin --- */

struct ui_skin {
	const char *name;      /* the key in x45ui.conf ("skin = tiles") and for the web bridge */
	const char *label;     /* what Options shows */
	const char *about;     /* one line for the web page: what is different about it (no quotes) */
	uint8_t bg[3], panel[3], line[3], text[3], dim[3], barbg[3], card[3];
	uint8_t tint;          /* 0..100: how far the palette is pulled towards the accent colour */
	uint8_t accent_pct;    /* 100: the accent as chosen; less darkens it (a light skin needs a deeper one) */
	int rows;              /* rows of a scrolling list that fit on a page; 0 = 9 */
	void (*bar)(struct ui *u, const char *title);
	void (*list)(struct ui *u, struct ui_list *l);
	void (*np)(struct ui *u, struct ui_np *np);
	void (*hints)(struct ui *u, const char *spec);
};

/* ---- called by main.c ---- */
void skin_palette(struct ui *u);                  /* the skin's colours with the owner's choices (skins/colours.c) */
void skin_bar(struct ui *u, const char *title);
void skin_list(struct ui *u, struct ui_list *l);
int skin_has_np(const struct ui *u);           /* the skin lays out the now-playing page itself */
void skin_np(struct ui *u, struct ui_np *np);
void skin_hints(struct ui *u, const char *spec);
int skin_rows(const struct ui *u);

int skin_count(void);
const struct ui_skin *skin_at(int i);
int skin_find(const char *name);               /* its index, or -1 */
const struct ui_skin *skin_cur(const struct ui *u);

/* ---- the colours the owner chooses (skins/colours.c) ---- */
enum { COL_ACCENT, COL_GROUND, COL_INK };
int colour_count(int kind);                    /* ground and ink: entry 0 is "the skin's own" */
const char *colour_name(int kind, int i);
void colour_swatch(int kind, int i, uint8_t rgb[3]);   /* the colour itself, for the web page */
void colour_accent(int i, uint8_t acc[3], uint8_t warn[3]);

/* ---- what skins share (skins/skins.c) ---- */
uint32_t skin_mix(uint32_t a, uint32_t b, int pct_b);          /* a blended with b, pct_b percent of b */
void skin_round_rect(struct x45_surface *s, int x, int y, int w, int h, int r, uint32_t col);
void skin_round_outline(struct x45_surface *s, int x, int y, int w, int h, int r, int th, uint32_t col);
void skin_icon(struct ui *u, const char *name, int x, int y, int size, uint32_t col);   /* an icon, size px square */
/* text centred on cx; one too wide for max_w starts at its left edge and runs (slot as for text_scroll) */
void skin_text_c(struct ui *u, int slot, int cx, int y, const char *utf8, int scale, int max_w, uint32_t col);
void skin_text_r(struct ui *u, int right, int y, const char *utf8, int scale, int max_w, uint32_t col);
int skin_value_chip(struct ui *u, int right, int y, int h, const char *utf8, int scale, int max_w, int sel, int r);   /* a setting's value in a chip; returns its left edge */
uint32_t skin_item_col(const struct ui *u, const struct ui_item *it, int selected);   /* the colour of a label */
void skin_list_msg(struct ui *u, const struct ui_list *l, int x, int y, int center);  /* the empty-list message */
void skin_list_title(struct ui *u, const struct ui_list *l, int center);              /* heading and note under the bar */
int skin_text_scale_fit(const char *s, int max_w, int from, int to);                  /* the largest scale in [to, from] that fits */

/* ---- the classic look, for skins that keep a part of it (skins/classic.c) ---- */
void skin_bar_classic(struct ui *u, const char *title);
void skin_list_rows(struct ui *u, struct ui_list *l);
void skin_hints_keycaps(struct ui *u, const char *spec);

#endif /* X45UI_SKIN_H */
