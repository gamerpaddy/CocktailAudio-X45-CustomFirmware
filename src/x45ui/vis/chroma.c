#define _GNU_SOURCE
/* chroma - which notes are sounding, and the key and the chord they point to.
 *
 * The strength of the twelve pitch classes (C, C#, D ... B; every octave from E3 to G#6 added together), on a wheel in the order of
 * the circle of fifths: a key's notes sit side by side. Over the last ~6 s of music they are matched against the Krumhansl-Kessler key
 * profiles (24 keys) and the best fit is the key; over the last ~0.4 s against the triads, which gives the chord. Beside the key
 * it says how well the profile fits (a track of drums, noise or a dense mix fits nothing; a song with a clear tonal centre fits 70-90 %).
 *
 * The analyzer's bands are 93 cents wide, so a semitone is one band, and the low notes (under ~200 Hz) cannot be told apart
 * with the FFT sizes it uses: the wheel looks at the part of the music from E3 up, which carries the chords and the melody. */
#include "vis.h"

#include <math.h>
#include <stdio.h>

#define LOWEST   52     /* MIDI note E3 ... */
#define HIGHEST  92     /* ... to G#6 */

static const char *const note[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

/* Krumhansl-Kessler profiles, tonic first */
static const float prof[2][12] = {
	{ 6.35f, 2.23f, 3.48f, 2.33f, 4.38f, 4.09f, 2.52f, 5.19f, 2.39f, 3.66f, 2.29f, 2.88f },
	{ 6.33f, 2.68f, 3.52f, 5.38f, 2.60f, 3.53f, 2.54f, 4.75f, 3.98f, 2.69f, 3.34f, 3.17f },
};

/* Pearson correlation of the pitch classes, starting at `root`, with a template */
static float corr(const float *c, int root, const float *t)
{
	float mc = 0, mt = 0, sxy = 0, sxx = 0, syy = 0;
	int i;

	for (i = 0; i < 12; i++) {
		mc += c[(root + i) % 12];
		mt += t[i];
	}
	mc /= 12.0f;
	mt /= 12.0f;
	for (i = 0; i < 12; i++) {
		float a = c[(root + i) % 12] - mc, b = t[i] - mt;

		sxy += a * b;
		sxx += a * a;
		syy += b * b;
	}
	return sxx < 1e-6f || syy < 1e-6f ? 0.0f : sxy / sqrtf(sxx * syy);
}

static void feed(struct ui *u)
{
	static const float tri[2][12] = {
		{ 1, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 0 },   /* major triad */
		{ 1, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0 },   /* minor triad */
	};
	struct vis_chroma *m = &u->vm.chroma;
	float db[HIGHEST - LOWEST + 1], v[12] = { 0 }, mean = 0, a_slow, a_fast, mx = 0, best = -2, bchord = 0.6f, mf = 0;
	long dt;
	int s, i, k, bk = -1, bm = 0, bc = -1, bcm = 0;

	VIS_MEM_STEP(u, m, dt);
	if (dt < 0) {
		m->key = m->chord = -1;
		return;
	}
	if (dt > 400)
		dt = 400;
	if ((u->sp_r[0] > u->sp_r[1] ? u->sp_r[0] : u->sp_r[1]) < -60.0f || dt == 0)
		return;   /* silence leaves the wheel as it is */
	for (s = LOWEST; s <= HIGHEST; s++) {
		db[s - LOWEST] = u->sp_b[vis_band_of(u, 440.0f * powf(2.0f, (float)(s - 69) / 12.0f))];
		mean += db[s - LOWEST];
	}
	mean /= (float)(HIGHEST - LOWEST + 1);
	for (s = LOWEST; s <= HIGHEST; s++) {
		float e = db[s - LOWEST] - (mean + 3.0f);   /* how far the note stands out of the average */

		if (e > 0)
			v[s % 12] += e * (s < 57 ? 0.5f : 1.0f);   /* the lowest octave is blurred: it counts for less */
	}
	a_slow = (float)dt / (float)((m->play_ms < 6000 ? m->play_ms : 6000) + dt);
	a_fast = dt >= 400 ? 1.0f : (float)dt / 400.0f;
	for (i = 0; i < 12; i++) {
		m->slow[i] += (v[i] - m->slow[i]) * a_slow;
		m->fast[i] += (v[i] - m->fast[i]) * a_fast;
		if (m->slow[i] > mx)
			mx = m->slow[i];
		if (m->fast[i] > mf)
			mf = m->fast[i];
	}
	m->play_ms += dt;

	/* the key: the best of 24 fits; the one that is held stays unless another is clearly better */
	if (m->play_ms >= 3000 && mx > 1.0f) {
		float r[2][12];

		for (k = 0; k < 2; k++)
			for (i = 0; i < 12; i++) {
				r[k][i] = corr(m->slow, i, prof[k]);
				if (r[k][i] > best) {
					best = r[k][i];
					bk = i;
					bm = k;
				}
			}
		if (m->key >= 0 && r[m->minor][m->key] >= best - 0.03f) {
			bk = m->key;
			bm = m->minor;
			best = r[bm][bk];
		}
		m->key = bk;
		m->minor = bm;
		m->key_r = best < 0 ? 0 : best;
	}
	/* the chord */
	for (k = 0; k < 2 && mf > 1.0f; k++)
		for (i = 0; i < 12; i++) {
			float c = corr(m->fast, i, tri[k]);

			if (c > bchord) {
				bchord = c;
				bc = i;
				bcm = k;
			}
		}
	m->chord = bc;
	m->chord_minor = bcm;
	m->chord_r = bc >= 0 ? bchord : 0;
	for (i = 0; i < 12; i++) {   /* the picture follows, a little slower than the numbers */
		float target = mx > 1.0f ? m->slow[i] / mx : 0;

		m->shown[i] += (target - m->shown[i]) * 0.3f;
	}
}

/* the wheel's place for a pitch class: the circle of fifths, C at the top, clockwise */
static float angle(int pc)
{
	return ((float)((pc * 7) % 12) * 30.0f - 90.0f) * 0.0174533f;
}

static int in_key(const struct vis_chroma *m, int pc)
{
	static const int major[7] = { 0, 2, 4, 5, 7, 9, 11 }, minor[7] = { 0, 2, 3, 5, 7, 8, 10 };
	const int *sc = m->minor ? minor : major;
	int i, rel = (pc - m->key + 12) % 12;

	for (i = 0; i < 7; i++)
		if (sc[i] == rel)
			return 1;
	return 0;
}

/* a box too low for the wheel: the twelve pitch classes as columns (still in the order of the circle of fifths), the key and the chord in words */
static void draw_compact(struct ui *u, int x, int y, int w, int h)
{
	struct x45_surface *s = &u->g.back;
	const struct vis_chroma *m = &u->vm.chroma;
	int have = m->t_ok && m->key >= 0, clear = have && m->key_r >= 0.5f, tw = w >= 400 ? 190 : 0, lab = h >= 52 ? 18 : 0, slot, ph, cw;
	int ts = h >= 70 ? 3 : 2;
	char buf[48];

	if (h < 28 || w < 120)
		return;
	if (tw) {
		if (have)
			snprintf(buf, sizeof buf, "%s %s", note[m->key], m->minor ? "minor" : "major");
		else
			snprintf(buf, sizeof buf, "--");
		x45_text_clipped(s, x, y + 2, buf, ts, tw - 8, clear ? u->t.accent : u->t.dim);
		if (have)
			snprintf(buf, sizeof buf, "fit %d%%", (int)(m->key_r * 100.0f + 0.5f));
		else
			snprintf(buf, sizeof buf, "listening...");
		if (h >= 50)
			x45_text_clipped(s, x, y + 2 + 7 * ts + 6, buf, 2, tw - 8, u->t.dim);
		if (m->t_ok && m->chord >= 0 && h >= 70) {
			snprintf(buf, sizeof buf, "chord %s%s", note[m->chord], m->chord_minor ? "m" : "");
			x45_text_clipped(s, x, y + 2 + 7 * ts + 6 + 18, buf, 2, tw - 8, u->t.text);
		}
	}
	ph = h - lab;
	cw = (w - tw) / 12;
	if (cw < 6)
		return;
	x45_fill_rect(s, x + tw, y, cw * 12, ph, u->t.barbg);
	for (slot = 0; slot < 12; slot++) {
		int pc = (slot * 7) % 12;   /* the fifth of slot - 1: the circle of fifths read round */
		float f = m->shown[pc] < 0 ? 0 : m->shown[pc] > 1 ? 1 : m->shown[pc];
		int bh = (int)(powf(f, 0.8f) * (float)(ph - 4) + 0.5f), bx = x + tw + slot * cw + cw / 6;
		uint32_t c = !clear ? u->t.barfg : pc == m->key ? u->t.accent : in_key(m, pc) ? u->t.barfg : vis_mix(u->t.barbg, u->t.barfg, 45);

		if (bh > 0)
			x45_fill_rect(s, bx, y + ph - bh, cw - cw / 3, bh, c);
		if (lab)
			x45_text(s, x + tw + slot * cw + (cw - x45_text_w(note[pc], 2)) / 2, y + ph + 3, note[pc], 2, clear && pc == m->key ? u->t.accent : u->t.dim);
	}
}

static void draw(struct ui *u, int x, int y, int w, int h)
{
	struct x45_surface *s = &u->g.back;
	const struct vis_chroma *m = &u->vm.chroma;
	int panel = w >= 360 && w >= h * 3 / 2 && h >= 150;

	if (h < 116 || w < 150) {
		draw_compact(u, x, y, w, h);
		return;
	}
	int r_out, r_in, cx, cy, pc, a, th;
	char buf[48];
	int have = m->t_ok && m->key >= 0, clear = have && m->key_r >= 0.5f, lim = panel ? w * 4 / 10 : w / 2 - 26;

	r_out = h / 2 - 22 < lim ? h / 2 - 22 : lim;
	if (r_out < 36)
		return;
	r_in = r_out * 3 / 10;
	cx = x + (panel ? r_out + 30 : w / 2);
	cy = y + h / 2;
	th = r_out >= 90 ? 6 : r_out >= 60 ? 4 : 3;
	for (a = 0; a < 360; a += 3) {   /* the two rings, dotted */
		float t = (float)a * 0.0174533f;

		x45_fill_rect(s, cx + (int)(cosf(t) * (float)r_out) - 1, cy + (int)(sinf(t) * (float)r_out) - 1, 2, 2, u->t.line);
		x45_fill_rect(s, cx + (int)(cosf(t) * (float)r_in) - 1, cy + (int)(sinf(t) * (float)r_in) - 1, 2, 2, u->t.line);
	}
	for (pc = 0; pc < 12; pc++) {
		float t = angle(pc), f = m->shown[pc] < 0 ? 0 : m->shown[pc] > 1 ? 1 : m->shown[pc], len = (float)r_in + (float)(r_out - r_in) * powf(f, 0.8f);
		int tx = cx + (int)(cosf(t) * len), ty = cy + (int)(sinf(t) * len), lx, ly, tw;
		uint32_t c = !clear ? u->t.barfg : pc == m->key ? u->t.accent : in_key(m, pc) ? u->t.barfg : vis_mix(u->t.barbg, u->t.barfg, 45);

		if (f > 0.02f)
			vis_line(s, cx + (int)(cosf(t) * (float)r_in), cy + (int)(sinf(t) * (float)r_in), tx, ty, th, c);
		x45_fill_rect(s, tx - th, ty - th, 2 * th + 1, 2 * th + 1, c);
		tw = x45_text_w(note[pc], 2);
		lx = cx + (int)(cosf(t) * (float)(r_out + 15)) - tw / 2;
		ly = cy + (int)(sinf(t) * (float)(r_out + 15)) - 7;
		x45_text(s, lx, ly, note[pc], 2, clear && pc == m->key ? u->t.accent : u->t.dim);
	}
	if (have) {   /* in the middle: the tonic */
		snprintf(buf, sizeof buf, "%s%s", note[m->key], m->minor ? "m" : "");
		x45_text(s, cx - x45_text_w(buf, r_in >= 36 ? 3 : 2) / 2, cy - (r_in >= 36 ? 10 : 7), buf, r_in >= 36 ? 3 : 2,
			 clear ? u->t.accent : u->t.dim);
	}
	if (panel) {
		int px = cx + r_out + 44, pw = x + w - px - 8, py = y + h / 2 - 70;

		if (pw < 90)
			return;
		x45_text(s, px, py, "KEY", 2, u->t.dim);
		if (have)
			snprintf(buf, sizeof buf, "%s %s", note[m->key], m->minor ? "minor" : "major");
		else
			snprintf(buf, sizeof buf, "--");
		x45_text_clipped(s, px, py + 19, buf, 3, pw, clear ? u->t.text : u->t.dim);
		if (have) {
			int rel = m->minor ? (m->key + 3) % 12 : (m->key + 9) % 12;

			snprintf(buf, sizeof buf, "fit %d%%", (int)(m->key_r * 100.0f + 0.5f));
			x45_text_clipped(s, px, py + 48, buf, 2, pw, u->t.dim);
			snprintf(buf, sizeof buf, "relative: %s %s", note[rel], m->minor ? "major" : "minor");
			x45_text_clipped(s, px, py + 66, buf, 2, pw, u->t.dim);
		} else {
			x45_text_clipped(s, px, py + 48, "listening...", 2, pw, u->t.dim);
		}
		x45_text(s, px, py + 96, "CHORD", 2, u->t.dim);
		if (m->t_ok && m->chord >= 0)
			snprintf(buf, sizeof buf, "%s%s", note[m->chord], m->chord_minor ? "m" : "");
		else
			snprintf(buf, sizeof buf, "--");
		x45_text_clipped(s, px, py + 115, buf, 3, pw, m->t_ok && m->chord >= 0 ? u->t.text : u->t.dim);
	}
}

const struct ui_vis vis_chroma = {
	.name = "chroma", .label = "Key and chord", .bit = 4096, .weight = 3, .analyzer = 1, .feed = feed, .draw = draw,
};
