#define _GNU_SOURCE
/* tonal - the tonal balance of the music: the long-term spectrum (the last ~10 s of music, so a single beat does not move it) in
 * eight regions from sub-bass to air, each as how far it sits above or below what ordinary music does. "Ordinary" is a spectrum
 * that falls 4.5 dB per octave, lined up to the middle regions (150 Hz .. 5 kHz): a guide, not a law. A record that is dull,
 * a speaker that is boomy, a master that is bright show up as bars away from the line.
 *
 * Every band of the analyzer holds the strongest bin of its stretch; a wide treble band holds many bins, so noise-like sound reaches higher
 * in it than the same sound does in a narrow bass band. That is corrected for (the expected maximum of that many bins), as for noise. */
#include "vis.h"

#include <math.h>
#include <stdio.h>

#define SLOPE   4.5f    /* dB per octave of the guide */
#define RANGE   12.0f   /* the bars reach this far each way, dB */
#define NREG    8

static const struct { float lo, hi; const char *name; } reg[NREG] = {
	{ 20, 60, "sub" }, { 60, 150, "bass" }, { 150, 400, "lo-mid" }, { 400, 1000, "mid" },
	{ 1000, 2500, "hi-mid" }, { 2500, 5000, "presence" }, { 5000, 10000, "treble" }, { 10000, 20000, "air" },
};

/* how many times a band is wider than the analyzer's bins; the FFT size follows the sample rate as in x45loop */
static float bias(const struct ui *u, int i)
{
	float rate = u->sp_frate ? (float)u->sp_frate : 44100.0f, f0 = vis_band_hz(u, i), n, width;
	int fft = rate >= 96000 ? 16384 : rate >= 48000 ? 8192 : 4096;

	width = f0 * (powf(vis_band_hz(u, NSB - 1) / vis_band_hz(u, 0), 1.0f / (float)(NSB - 1)) - 1.0f);
	n = width / (rate / (float)fft);
	return n > 1.5f ? 1.0f / (logf(n) + 0.58f) : 1.0f;   /* the mean of n noise bins against the largest of them */
}

static void feed(struct ui *u)
{
	struct vis_tonal *m = &u->vm.tonal;
	long dt;
	float a;
	int i;

	VIS_MEM_STEP(u, m, dt);
	if (dt <= 0)
		return;
	if (dt > 400)
		dt = 400;
	if ((u->sp_r[0] > u->sp_r[1] ? u->sp_r[0] : u->sp_r[1]) < -60.0f)
		return;   /* silence does not dilute it */
	a = (float)dt / (float)((m->warm_ms < 10000 ? m->warm_ms : 10000) + dt);   /* a plain mean for the first 10 s, then a moving one */
	for (i = 0; i < NSB; i++)
		m->pw[i] += (vis_pow(u->sp_b[i]) - m->pw[i]) * a;
	m->warm_ms += dt;
}

/* how far each region is from the guide, dB; returns whether there is anything to show */
static int deviations(const struct ui *u, float dev[NREG])
{
	const struct vis_tonal *m = &u->vm.tonal;
	float raw[NREG], mid = 0;
	int r, i, n;

	if (m->warm_ms < 1500)
		return 0;
	for (r = 0; r < NREG; r++) {
		int a = vis_band_of(u, reg[r].lo), z = vis_band_of(u, reg[r].hi * 0.999f);
		double p = 0;

		for (i = a, n = 0; i <= z; i++, n++)
			p += (double)m->pw[i] * bias(u, i);
		raw[r] = 10.0f * log10f((float)(p / (double)n) + 1e-20f) +
			 SLOPE * log2f(sqrtf(reg[r].lo * reg[r].hi) / 1000.0f);   /* against the guide */
	}
	for (r = 2; r <= 5; r++)
		mid += raw[r];
	mid /= 4.0f;
	for (r = 0; r < NREG; r++)
		dev[r] = raw[r] - mid;
	return 1;
}

static const char *verdict(const float dev[NREG], char *buf, size_t n)
{
	float tilt = 0.5f * (dev[6] + dev[7]) - 0.5f * (dev[0] + dev[1]), worst = 0;
	int r, wi = -1;

	for (r = 0; r < NREG; r++)
		if (fabsf(dev[r]) > fabsf(worst)) {
			worst = dev[r];
			wi = r;
		}
	if (tilt > 5.0f)
		snprintf(buf, n, "bright");
	else if (tilt < -5.0f)
		snprintf(buf, n, "dark");
	else if (wi >= 0 && fabsf(worst) >= 3.0f)
		snprintf(buf, n, "%s %+.0f dB", reg[wi].name, (double)worst);
	else
		snprintf(buf, n, "balanced");
	return buf;
}

static void draw(struct ui *u, int x, int y, int w, int h)
{
	struct x45_surface *s = &u->g.back;
	float dev[NREG];
	char buf[40];
	int title = h >= 100 ? 20 : 0, lab = h >= 70 ? 20 : 0, py = y + title, ph = h - title - lab, zero, half, cw = w / NREG, r;
	uint32_t amber = x45_rgb(&u->g, 240, 190, 62);

	if (ph < 30 || cw < 24)
		return;
	zero = py + ph / 2;
	half = ph / 2 - (ph >= 120 ? 16 : 2);
	x45_fill_rect(s, x, py, w, ph, u->t.barbg);
	x45_hline(s, x, zero - (int)(6.0f / RANGE * (float)half), w, vis_mix(u->t.barbg, u->t.line, 60));
	x45_hline(s, x, zero + (int)(6.0f / RANGE * (float)half), w, vis_mix(u->t.barbg, u->t.line, 60));
	x45_hline(s, x, zero, w, u->t.dim);
	if (title)
		x45_text(s, x, y, "TONAL BALANCE", 2, u->t.dim);
	if (!deviations(u, dev)) {
		x45_text(s, x + w / 2 - x45_text_w("collecting...", 2) / 2, zero - 7, "collecting...", 2, u->t.dim);
		return;
	}
	if (title) {
		verdict(dev, buf, sizeof buf);
		x45_text(s, x + w - x45_text_w(buf, 2), y, buf, 2, u->t.text);
	}
	for (r = 0; r < NREG; r++) {
		float d = dev[r], c = d > RANGE ? RANGE : d < -RANGE ? -RANGE : d;
		int bh = (int)(fabsf(c) / RANGE * (float)half + 0.5f), bx = x + r * cw + (cw >= 60 ? 7 : cw / 6), bw = cw - 2 * (cw >= 60 ? 7 : cw / 6);
		uint32_t col = fabsf(d) < 3.0f ? u->t.barfg : fabsf(d) < 6.0f ? amber : u->t.warn;

		if (bh > 0)
			x45_fill_rect(s, bx, c >= 0 ? zero - bh : zero + 1, bw, bh, col);
		if (ph >= 80) {
			int ty = c >= 0 ? zero - bh - 16 : zero + bh + 3;

			snprintf(buf, sizeof buf, "%+.0f", (double)d);
			if (ty < py)
				ty = py + 2;
			if (ty + 14 > py + ph)
				ty = py + ph - 16;
			x45_text(s, x + r * cw + (cw - x45_text_w(buf, 2)) / 2, ty, buf, 2, u->t.text);
		}
		if (lab) {
			int ts = x45_text_w(reg[r].name, 2) <= cw - 4 ? 2 : 1;

			x45_text(s, x + r * cw + (cw - x45_text_w(reg[r].name, ts)) / 2, py + ph + 6, reg[r].name, ts, u->t.dim);
		}
	}
}

const struct ui_vis vis_tonal = {
	.name = "tonal", .label = "Tonal balance", .bit = 2048, .weight = 3, .analyzer = 1, .feed = feed, .draw = draw,
};
