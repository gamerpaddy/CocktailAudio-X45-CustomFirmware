#define _GNU_SOURCE
/* colours - the colours the owner chooses, whatever the skin: the accent, the background and the text.
 *
 * A skin brings a palette of its own (skins/NAME.c); on top of it Options offers three choices:
 *   accent     highlights, the selected row, the bars (and a warning colour that goes with it)
 *   background a tone for the whole panel (the panels, lines and cards are derived from it); "skin" keeps the
 *              skin's own, which is pulled a little towards the accent
 *   text       the colour of the writing; "skin" keeps the skin's own
 * A light background gets dark text automatically and a deeper accent, so every combination can be read.
 *
 * To add a colour: one line in the table it belongs to.
 */
#include "skin.h"

#include <string.h>

static const struct { const char *name; uint8_t acc[3], warn[3]; } accents[] = {
	{ "green",   { 108, 198, 134 }, { 226, 128,  96 } },
	{ "blue",    { 104, 170, 230 }, { 230, 150, 100 } },
	{ "amber",   { 232, 176,  84 }, { 226, 110,  96 } },
	{ "violet",  { 170, 140, 230 }, { 230, 130, 110 } },
	{ "rose",    { 232, 128, 160 }, { 232, 176,  90 } },
	{ "ice",     { 150, 220, 226 }, { 232, 140, 100 } },
	{ "red",     { 236,  92,  92 }, { 240, 190,  90 } },
	{ "orange",  { 242, 140,  56 }, { 226,  84,  84 } },
	{ "gold",    { 236, 200,  90 }, { 226, 110,  96 } },
	{ "lime",    { 170, 220,  70 }, { 230, 120,  90 } },
	{ "teal",    {  70, 200, 180 }, { 232, 150, 100 } },
	{ "magenta", { 224, 110, 214 }, { 232, 176,  90 } },
	{ "white",   { 230, 232, 238 }, { 232, 150, 100 } },
};

/* index 0 of the grounds and inks is "the skin's own"; its row here is unused */
static const struct { const char *name; uint8_t rgb[3]; } grounds[] = {
	{ "skin",     {   0,   0,   0 } },
	{ "black",    {   0,   0,   0 } },
	{ "graphite", {  24,  25,  28 } },
	{ "slate",    {  30,  38,  50 } },
	{ "navy",     {  11,  19,  42 } },
	{ "teal",     {  10,  34,  38 } },
	{ "forest",   {  11,  28,  20 } },
	{ "plum",     {  30,  16,  40 } },
	{ "wine",     {  38,  13,  22 } },
	{ "coffee",   {  34,  24,  17 } },
	{ "paper",    { 238, 236, 230 } },
	{ "sand",     { 226, 214, 190 } },
};

static const struct { const char *name; uint8_t rgb[3]; } inks[] = {
	{ "skin",  {   0,   0,   0 } },
	{ "white", { 246, 247, 250 } },
	{ "warm",  { 252, 238, 214 } },
	{ "green", { 150, 255, 160 } },
	{ "amber", { 255, 196,  96 } },
	{ "cyan",  { 160, 232, 244 } },
	{ "rose",  { 252, 196, 214 } },
	{ "grey",  { 200, 204, 212 } },
};

#define N(a) ((int)(sizeof a / sizeof *a))

int colour_count(int kind)
{
	return kind == COL_ACCENT ? N(accents) : kind == COL_GROUND ? N(grounds) : N(inks);
}

const char *colour_name(int kind, int i)
{
	if (i < 0 || i >= colour_count(kind))
		i = 0;
	return kind == COL_ACCENT ? accents[i].name : kind == COL_GROUND ? grounds[i].name : inks[i].name;
}

void colour_swatch(int kind, int i, uint8_t rgb[3])
{
	const uint8_t *src;

	if (i < 0 || i >= colour_count(kind))
		i = 0;
	src = kind == COL_ACCENT ? accents[i].acc : kind == COL_GROUND ? grounds[i].rgb : inks[i].rgb;
	memcpy(rgb, src, 3);
}

void colour_accent(int i, uint8_t acc[3], uint8_t warn[3])
{
	if (i < 0 || i >= N(accents))
		i = 0;
	memcpy(acc, accents[i].acc, 3);
	memcpy(warn, accents[i].warn, 3);
}

/* ------------------------------------------------------------------ palette --- */

static int luma(const uint8_t c[3])
{
	return (c[0] * 3 + c[1] * 6 + c[2]) / 10;
}

static uint8_t clamp8(int v)
{
	return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

static void shifted(uint8_t out[3], const uint8_t base[3], int d)
{
	int i;

	for (i = 0; i < 3; i++)
		out[i] = clamp8(base[i] + d);
}

static void mixed(uint8_t out[3], const uint8_t a[3], const uint8_t b[3], int pct_b)
{
	int i;

	for (i = 0; i < 3; i++)
		out[i] = (uint8_t)((a[i] * (100 - pct_b) + b[i] * pct_b) / 100);
}

/* `base` with the hue of `acc`, at the brightness of `base`, `tint` percent of the way */
static void tinted(uint8_t out[3], const uint8_t base[3], const uint8_t acc[3], int tint)
{
	int lb = luma(base), la = luma(acc), i;

	if (la < 1)
		la = 1;
	for (i = 0; i < 3; i++) {
		int target = acc[i] * lb / la;

		if (target > 255)
			target = 255;
		out[i] = (uint8_t)(base[i] + (target - base[i]) * tint / 100);
	}
}

void skin_palette(struct ui *u)
{
	const struct ui_skin *k = skin_cur(u);
	struct x45_gfx *g = &u->g;
	uint8_t acc[3], warn[3], a[3], w[3], pal[7][3];   /* bg panel line text dim barbg card */
	int i, light, pct = k->accent_pct ? k->accent_pct : 100;

	colour_accent(u->theme_id, acc, warn);
	if (u->ground_id > 0 && u->ground_id < N(grounds)) {
		const uint8_t *g0 = grounds[u->ground_id].rgb, *text;
		static const uint8_t dark_text[3] = { 30, 32, 38 }, light_text[3] = { 238, 240, 246 };

		light = luma(g0) > 140;
		memcpy(pal[0], g0, 3);
		shifted(pal[1], g0, light ? -14 : 13);
		shifted(pal[2], g0, light ? -50 : 46);
		shifted(pal[5], g0, light ? -24 : 22);
		shifted(pal[6], g0, light ? 12 : 6);
		text = light ? dark_text : light_text;
		memcpy(pal[3], text, 3);
		mixed(pal[4], text, g0, light ? 42 : 48);
		pct = light ? 62 : 100;
	} else {
		const uint8_t *src[7] = { k->bg, k->panel, k->line, k->text, k->dim, k->barbg, k->card };
		uint8_t sa[3];

		for (i = 0; i < 3; i++)   /* the skin pulls its palette towards its own (maybe deeper) accent */
			sa[i] = (uint8_t)(acc[i] * pct / 100);
		for (i = 0; i < 7; i++)
			tinted(pal[i], src[i], sa, k->tint);
		light = luma(pal[0]) > 140;
	}
	if (u->ink_id > 0 && u->ink_id < N(inks)) {
		uint8_t t[3];

		memcpy(t, inks[u->ink_id].rgb, 3);
		if (light)   /* the same hue, dark enough to read on a light ground */
			for (i = 0; i < 3; i++)
				t[i] = (uint8_t)(t[i] * 26 / 100);
		memcpy(pal[3], t, 3);
		mixed(pal[4], t, pal[0], 46);
	}
	for (i = 0; i < 3; i++) {
		a[i] = (uint8_t)(acc[i] * pct / 100);
		w[i] = (uint8_t)(warn[i] * pct / 100);
	}
	u->t.bg = x45_rgb(g, pal[0][0], pal[0][1], pal[0][2]);
	u->t.panel = x45_rgb(g, pal[1][0], pal[1][1], pal[1][2]);
	u->t.line = x45_rgb(g, pal[2][0], pal[2][1], pal[2][2]);
	u->t.text = x45_rgb(g, pal[3][0], pal[3][1], pal[3][2]);
	u->t.dim = x45_rgb(g, pal[4][0], pal[4][1], pal[4][2]);
	u->t.barbg = x45_rgb(g, pal[5][0], pal[5][1], pal[5][2]);
	u->t.card = x45_rgb(g, pal[6][0], pal[6][1], pal[6][2]);
	u->t.accent = x45_rgb(g, a[0], a[1], a[2]);
	u->t.barfg = u->t.accent;
	u->t.warn = x45_rgb(g, w[0], w[1], w[2]);
}
