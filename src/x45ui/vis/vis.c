#define _GNU_SOURCE
/* vis - the registry of the visuals, and how what is on shares a box. See vis.h. */
#include "vis.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern const struct ui_vis vis_bars, vis_dials, vis_spectrum, vis_waterfall, vis_scope, vis_gonio;
extern const struct ui_vis vis_loudness, vis_reccheck, vis_tonal, vis_chroma, vis_width;
/* vis_led, vis_mirror and vis_area (vis.h) are styles of the spectrum, drawn by vis/spectrum.c: not in this list */

/* the order is the order of the menu and of the layout, top to bottom */
static const struct ui_vis *const all[] = {
	&vis_bars, &vis_dials, &vis_scope, &vis_gonio, &vis_spectrum, &vis_waterfall,
	&vis_loudness, &vis_reccheck, &vis_tonal, &vis_chroma, &vis_width,
};
#define NVIS ((int)(sizeof all / sizeof *all))

int vis_count(void)
{
	return NVIS;
}

const struct ui_vis *vis_at(int i)
{
	return all[i < 0 || i >= NVIS ? 0 : i];
}

/* what the panel shows as visuals: one setting for all inputs, or the input's own */
unsigned vis_get(const struct ui *u)
{
	return u->vis_all ? u->vis_shared.on : u->vis[u->input].on;
}

struct vis_sel *vis_sel_of(struct ui *u)
{
	return u->vis_all ? &u->vis_shared : &u->vis[u->input];
}

/* The look options of the visuals that are on, so that Options > Visualizers (and the web) list only what would
 * change something on the screen. A visual that cannot draw yet (no analyzer) still counts: it is switched on. */
unsigned vis_looks(const struct ui *u)
{
	unsigned mask = vis_get(u), looks = 0;
	int i;

	for (i = 0; i < NVIS; i++)
		if (mask & all[i]->bit)
			looks |= all[i]->look | (all[i]->bands ? VIS_LOOK_BANDS : 0);
	if ((looks & VIS_LOOK_FIELD) && u->gon_col == VIS_GON_BY_DISTANCE)
		looks |= VIS_LOOK_COLOURS;   /* its trace is coloured like the bars */
	return looks;
}

/* saved as one number: the bits that are on, and above them (<< 16) the bit of the older one */
unsigned long vis_pack(const struct vis_sel *s)
{
	return (unsigned long)s->on | ((unsigned long)s->first << 16);
}

static int bits_in(unsigned v)
{
	int n = 0;

	for (; v; v >>= 1)
		n += v & 1;
	return n;
}

/* from a saved number; an older setting with more than VIS_MAX_ON bits keeps the lowest ones. LED, mirror and curve
 * (once visuals of their own) now are styles of the spectrum: a setting that has one of them has the spectrum on, and
 * the return value says which style that was (the first of them in the setting), or -1 */
int vis_unpack(struct vis_sel *s, unsigned long v)
{
	unsigned on = (unsigned)(v & 0xffffu), first = (unsigned)((v >> 16) & 0xffffu), kept = 0, bit;
	const unsigned old[3] = { vis_led.bit, vis_mirror.bit, vis_area.bit };   /* styles 1, 2, 3 */
	int n = 0, style = -1, k;

	for (k = 0; k < 3; k++) {
		if (style < 0 && ((on | first) & old[k]))
			style = k + 1;
		if (on & old[k])
			on = (on & ~old[k]) | vis_spectrum.bit;
		if (first & old[k])
			first = (first & ~old[k]) | vis_spectrum.bit;
	}
	for (bit = 1; bit <= 0x8000u && n < VIS_MAX_ON; bit <<= 1)
		if (on & bit) {
			kept |= bit;
			n++;
		}
	s->on = (unsigned short)kept;
	s->first = (unsigned short)((n == VIS_MAX_ON && (kept & first) && bits_in(first) == 1) ? first : n == VIS_MAX_ON ? (kept & -kept) : 0);
	return style;
}

unsigned vis_switch(struct vis_sel *s, unsigned bit)
{
	unsigned off = 0;

	if (s->on & bit) {
		s->on &= (unsigned short)~bit;
		s->first = 0;
	} else if (bits_in(s->on) >= VIS_MAX_ON) {
		off = s->first;                       /* a third one: the oldest goes */
		s->on = (unsigned short)((s->on & ~off) | bit);
		s->first = (unsigned short)(s->on & ~bit);   /* what stayed is now the older of the two */
	} else {
		s->first = s->on;                     /* none or one on: that one (if any) is the older */
		s->on |= (unsigned short)bit;
	}
	return off;
}

/* ---- the colours of bars, columns and cells: the owner's choice (Options > Bar colours) ---- */

static const char *const bar_names[] = {
	"accent colour", "green-yellow-red, soft", "green-yellow-red, bright", "fire", "ocean", "rainbow",
	/* the saved number is the index: new ones only at the end */
	"amber", "green phosphor", "violet", "sunset", "neon", "ice", "white", "red",
};

int vis_bar_colours(void)
{
	return (int)(sizeof bar_names / sizeof *bar_names);
}

const char *vis_bar_colour_name(int i)
{
	return bar_names[i < 0 || i >= vis_bar_colours() ? 0 : i];
}

/* a blend of three stops at 0, .5 and 1 */
static uint32_t ramp3(struct ui *u, float f, const int a[3], const int b[3], const int c[3])
{
	const int *p = f < 0.5f ? a : b, *q = f < 0.5f ? b : c;
	float t = f < 0.5f ? f * 2.0f : (f - 0.5f) * 2.0f;

	return x45_rgb(&u->g, (int)((float)p[0] + (float)(q[0] - p[0]) * t), (int)((float)p[1] + (float)(q[1] - p[1]) * t),
		       (int)((float)p[2] + (float)(q[2] - p[2]) * t));
}

uint32_t vis_bar_colour(struct ui *u, float f)
{
	int r, g, b;
	float t;

	if (f < 0)
		f = 0;
	if (f > 1)
		f = 1;
	switch (u->spec_col) {
	case 1:   /* soft: a pastel green -> cream -> red */
		if (f < 0.55f) {
			t = f / 0.55f;
			r = 148 + (int)(88 * t);
			g = 208 + (int)(14 * t);
			b = 164 - (int)(18 * t);
		} else {
			t = (f - 0.55f) / 0.45f;
			r = 236 + (int)(2 * t);
			g = 222 - (int)(88 * t);
			b = 146 - (int)(10 * t);
		}
		return x45_rgb(&u->g, r, g, b);
	case 2:   /* vivid: saturated green -> yellow -> red */
		if (f < 0.6f) {
			t = f / 0.6f;
			r = (int)(20 + 220 * t);
			g = 205;
			b = (int)(60 - 60 * t);
		} else {
			t = (f - 0.6f) / 0.4f;
			r = 240;
			g = (int)(205 - 175 * t);
			b = (int)(10 * t);
		}
		return x45_rgb(&u->g, r, g, b);
	case 3: {   /* fire: dark red -> orange -> pale yellow */
		static const int a[3] = { 120, 14, 8 }, m[3] = { 238, 112, 20 }, z[3] = { 255, 238, 150 };

		return ramp3(u, f, a, m, z);
	}
	case 4: {   /* ocean: deep blue -> teal -> foam */
		static const int a[3] = { 20, 56, 160 }, m[3] = { 28, 170, 190 }, z[3] = { 214, 248, 244 };

		return ramp3(u, f, a, m, z);
	}
	case 5: {   /* rainbow: blue at the foot, red at the top */
		float h = (1.0f - f) * 4.0f;   /* 4 .. 0: blue, cyan, green, yellow, red */
		int seg = (int)h;
		float k = h - (float)seg;

		switch (seg) {
		case 4: r = 0; g = 0; b = 255; break;
		case 3: r = 0; g = (int)(255 * (1.0f - k)); b = 255; break;
		case 2: r = 0; g = 255; b = (int)(255 * k); break;
		case 1: r = (int)(255 * (1.0f - k)); g = 255; b = 0; break;
		default: r = 255; g = (int)(255 * k); b = 0; break;
		}
		return x45_rgb(&u->g, 40 + r * 215 / 255, 40 + g * 200 / 255, 40 + b * 215 / 255);
	}
	case 6: {   /* amber: a dark glow to a pale gold, as a vacuum display has it */
		static const int a[3] = { 110, 56, 4 }, m[3] = { 255, 176, 30 }, z[3] = { 255, 236, 150 };

		return ramp3(u, f, a, m, z);
	}
	case 7: {   /* green phosphor: the tube of an oscilloscope */
		static const int a[3] = { 6, 90, 30 }, m[3] = { 60, 235, 100 }, z[3] = { 200, 255, 205 };

		return ramp3(u, f, a, m, z);
	}
	case 8: {   /* violet: indigo, magenta, pink */
		static const int a[3] = { 70, 40, 170 }, m[3] = { 190, 60, 220 }, z[3] = { 255, 170, 230 };

		return ramp3(u, f, a, m, z);
	}
	case 9: {   /* sunset: purple, orange, yellow */
		static const int a[3] = { 96, 30, 130 }, m[3] = { 240, 90, 60 }, z[3] = { 255, 214, 90 };

		return ramp3(u, f, a, m, z);
	}
	case 10: {  /* neon: cyan, magenta, yellow */
		static const int a[3] = { 0, 226, 255 }, m[3] = { 255, 60, 220 }, z[3] = { 255, 240, 60 };

		return ramp3(u, f, a, m, z);
	}
	case 11: {  /* ice: steel blue to white */
		static const int a[3] = { 50, 90, 150 }, m[3] = { 130, 190, 240 }, z[3] = { 245, 250, 255 };

		return ramp3(u, f, a, m, z);
	}
	case 12: {  /* white: grey to white */
		static const int a[3] = { 90, 94, 100 }, m[3] = { 180, 184, 190 }, z[3] = { 250, 250, 250 };

		return ramp3(u, f, a, m, z);
	}
	case 13: {  /* red: dark to bright to pale */
		static const int a[3] = { 100, 8, 12 }, m[3] = { 240, 40, 36 }, z[3] = { 255, 170, 150 };

		return ramp3(u, f, a, m, z);
	}
	default:
		return f > 0.95f ? u->t.warn : f > 0.8f ? u->t.accent : u->t.barfg;
	}
}

long vis_now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* x45loop lays its bands out from 20 Hz to 20 kHz, or to 0.45 x the sample rate when that is lower */
static float band_fmax(const struct ui *u)
{
	float r = u->sp_frate ? (float)u->sp_frate : 44100.0f, f = r * 0.45f;

	return f < 20000.0f ? f : 20000.0f;
}

float vis_band_hz(const struct ui *u, int i)
{
	return 20.0f * powf(band_fmax(u) / 20.0f, ((float)i + 0.5f) / (float)NSB);
}

int vis_band_of(const struct ui *u, float hz)
{
	int b;

	if (hz < 20.0f)
		return 0;
	b = (int)(logf(hz / 20.0f) / logf(band_fmax(u) / 20.0f) * (float)NSB);
	return b < 0 ? 0 : b >= NSB ? NSB - 1 : b;
}

float vis_pow(float db)
{
	return expf(db * 0.2302585f);
}

long vis_step(const struct ui *u, int *input, int *t_ok, long *t)
{
	long now = u->sp_read_t, dt;

	if (!*t_ok || *input != u->input) {
		*input = u->input;
		*t = now;
		*t_ok = 1;
		return -1;
	}
	dt = now - *t;
	*t = now;
	return dt < 0 || dt > 3000 ? -1 : dt;
}

void vis_level(const struct ui *u, int ch, float *lvl, float *pk)
{
	*lvl = u->sp_have ? u->sp_r[ch] : u->m_lvl[ch];   /* RMS when the analyzer runs */
	*pk = u->sp_have ? u->sp_pkh[ch] : u->m_pk[ch];
}

int vis_db_x(float db, int w)
{
	if (db < METER_FLOOR)
		db = METER_FLOOR;
	if (db > 0)
		db = 0;
	return (int)((db - METER_FLOOR) / -METER_FLOOR * (float)w + 0.5f);
}

void vis_feed(struct ui *u)
{
	int i;

	for (i = 0; i < NVIS; i++)
		if (all[i]->feed)
			all[i]->feed(u);
}

/* The large stereo field (Options > Visualizers): on the player it takes the box when it is the only visual switched on,
 * and it is drawn even while the analyzer has nothing to say (a tube at rest, the text beside it keeps its place).
 * With a second visual on, the page is the normal one and the stereo field shares the box with it, smaller. */
int vis_big(const struct ui *u)
{
	unsigned mask = vis_get(u);

	return u->gon_big && u->gon_nobars && u->screen == SCR_PLAYER && (mask & vis_gonio.bit) != 0 && (mask & ~vis_gonio.bit) == 0;
}

/* Whatever is switched on shares the box: one alone fills all of it, several split it by weight (each within
 * the tallest it needs; what one cannot use goes to the others). */
void draw_visuals(struct ui *u, int x, int y, int w, int h)
{
	unsigned mask = vis_get(u);
	int on[NVIS], cap[NVIS], fixed[NVIS], hh[NVIS], n = 0, i, rem, pass;
	long now = vis_now();

	if (vis_big(u)) {
		if (u->dial_pass != 1)
			vis_gonio.draw(u, x, y, w, h);
		return;
	}

	for (i = 0; i < NVIS; i++) {
		on[i] = (mask & all[i]->bit) && (!(all[i]->bands || all[i]->analyzer) || u->sp_have) && (!all[i]->wave || u->sp_wave) &&
			(!all[i]->side || u->sp_side);
		cap[i] = all[i]->cap ? all[i]->cap(w) : 100000;
		fixed[i] = hh[i] = 0;
		n += on[i];
	}
	if (!n || h < 24)
		return;
	rem = h - 12 * (n - 1);
	for (pass = 0; pass < NVIS; pass++) {
		int sumw = 0, clamped = 0;

		for (i = 0; i < NVIS; i++)
			if (on[i] && !fixed[i])
				sumw += all[i]->weight;
		if (!sumw)
			break;
		for (i = 0; i < NVIS; i++) {
			int share;

			if (!on[i] || fixed[i])
				continue;
			share = rem * all[i]->weight / sumw;
			if (share > cap[i]) {
				hh[i] = cap[i];
				fixed[i] = 1;
				rem -= cap[i];
				clamped = 1;
			} else {
				hh[i] = share;
			}
		}
		if (!clamped)
			break;
	}
	if (u->dial_pass != 1)
		for (i = 0; i < NVIS; i++)
			if (all[i]->frame)
				all[i]->frame(u, now, on[i]);
	for (i = 0; i < NVIS; i++) {
		if (!on[i])
			continue;
		if (u->dial_pass == 1 && !all[i]->two_pass) {
			y += hh[i] + 12;
			continue;
		}
		all[i]->draw(u, x, y, w, hh[i]);
		y += hh[i] + 12;
	}
}

/* The visuals the owner switched on, laid out in a rectangle, for every source the unit can tap (radio,
 * network playback); the external inputs have their own screen. On the player screen what was behind the
 * rectangle is kept, so that the next frames can put it back and redraw just the visuals. */
void draw_visuals_rect(struct ui *u, int x, int y, int w, int h)
{
	struct x45_surface *s = &u->g.back;
	unsigned mask = vis_get(u);
	int i, two = 0, big = vis_big(u);

	if (!big && (!u->sp_have || meter_wanted(u) || !mask))
		return;
	for (i = 0; i < NVIS && !big; i++)
		two |= all[i]->two_pass && (mask & all[i]->bit);
	if (u->screen == SCR_PLAYER && y > 0 && h > 0 && y + h <= s->h) {
		size_t bytes = (size_t)h * (size_t)s->stride * 4;
		uint32_t *bg = realloc(u->vf_bg, bytes);

		if (bg) {
			u->vf_bg = bg;
			memcpy(bg, s->pixels + (size_t)y * s->stride, bytes);
			u->vf_x = x;
			u->vf_y = y;
			u->vf_w = w;
			u->vf_h = h;
			u->vf_ok = 1;
			u->vf_dials = 0;
			if (two) {
				/* what never changes (the dials' scales): drawn once and kept as the background */
				uint32_t *bg2 = realloc(u->vf_bg2, bytes);

				if (bg2) {
					u->vf_bg2 = bg2;
					u->dial_pass = 1;
					draw_visuals(u, x, y, w, h);
					u->dial_pass = 0;
					memcpy(bg2, s->pixels + (size_t)y * s->stride, bytes);
					u->vf_dials = 1;
				}
			}
		}
	}
	u->dial_pass = u->vf_dials && u->screen == SCR_PLAYER ? 2 : 0;
	draw_visuals(u, x, y, w, h);
	u->dial_pass = 0;
}

/* ---- what the band visuals share ---- */

/* the analyzer's NSB bands folded into nb display bands (power mean: a loud narrow peak survives) */
void vis_rebin(const float *src, float *dst, int nb)
{
	int i;

	for (i = 0; i < nb; i++) {
		int a = i * NSB / nb, z = (i + 1) * NSB / nb, k;
		float p = 0;

		if (z <= a)
			z = a + 1;
		for (k = a; k < z; k++)
			p += expf(src[k] * 0.2302585f);
		dst[i] = 4.3429448f * logf(p / (float)(z - a) + 1e-12f);
	}
}

/* a band level in dB as the fraction of a bar's height: the dB scale over a wide range with a mild curve,
 * so loud, dense music does not sit against the top all the time */
float vis_band_f(float db)
{
	float f = (db - SPEC_FLOOR) / -SPEC_FLOOR;

	if (f < 0)
		f = 0;
	if (f > 1)
		f = 1;
	return powf(f, 1.6f);
}

/* a line of thickness th, for traces and vectors */
void vis_line(struct x45_surface *s, int x0, int y0, int x1, int y1, int th, uint32_t c)
{
	int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
	int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
	int err = dx + dy, e2;

	for (;;) {
		x45_fill_rect(s, x0 - th / 2, y0 - th / 2, th, th, c);
		if (x0 == x1 && y0 == y1)
			break;
		e2 = 2 * err;
		if (e2 >= dy) {
			err += dy;
			x0 += sx;
		}
		if (e2 <= dx) {
			err += dx;
			y0 += sy;
		}
	}
}

/* a with b mixed in, pct percent of b, for the faint things (a graticule, a fill, a lamp that is out) */
uint32_t vis_mix(uint32_t a, uint32_t b, int pct)
{
	uint32_t out = 0;
	int lane;

	for (lane = 0; lane < 24; lane += 8) {
		int x = (int)((a >> lane) & 0xff), y = (int)((b >> lane) & 0xff);

		out |= (uint32_t)((x * (100 - pct) + y * pct) / 100) << lane;
	}
	return out | (a & 0xff000000u);
}
