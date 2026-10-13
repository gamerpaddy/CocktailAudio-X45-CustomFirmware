#define _GNU_SOURCE
/* loudness - how loud the music is, in LUFS, and how that moved over the last four minutes.
 *
 * Short-term loudness (the last 3 s) once a second as columns, 14 LUFS (streaming) and 23 LUFS (EBU R128) marked; beside them the
 * momentary (0.4 s), short-term, integrated (everything since the playback began, gated as in ITU-R BS.1770) loudness, the
 * loudness range (EBU 3342) and the highest sample peak.
 *
 * The numbers are an ESTIMATE: the analyzer hands over the RMS of each channel and 128 bands, not the signal, so the K-weighting
 * (a 4 dB shelf above 1.7 kHz and a high pass at 38 Hz) is applied as a gain worked out from the band spectrum, between the value
 * for tonal and for noise-like content. Within a LU or two of a meter for ordinary music; not a reading to certify with. */
#include "vis.h"

#include <math.h>
#include <stdio.h>

#define LOW   (-40.0f)   /* the bottom of the graph, LUFS */
#define HIGH  (-6.0f)    /* the top */

/* the K-weighting's power gain at f Hz: ITU-R BS.1770 stage 1 (high shelf, +4 dB, 1681.97 Hz, Q 0.70717) and stage 2
 * (high pass, 38.1355 Hz, Q 0.50033), as analog prototypes */
static float kgain(float f)
{
	const float A = 1.2589f;   /* 10^(4 dB / 40) */
	float w = f / 1681.97f, q = 0.70717f, w2 = f / 38.1355f, q2 = 0.50033f;
	float num_r = 1.0f - A * w * w, num_i = w * sqrtf(A) / q, den_r = A - w * w;
	float shelf = A * A * (num_r * num_r + num_i * num_i) / (den_r * den_r + num_i * num_i);
	float hp = w2 * w2 * w2 * w2 / ((1.0f - w2 * w2) * (1.0f - w2 * w2) + w2 * w2 / (q2 * q2));

	return shelf * hp;
}

/* The gain the K-weighting gives the spectrum on the screen now. Each band carries the strongest bin of its stretch: for a
 * tone that is the tone's level, for noise it is the level per bin and the band holds as many bins as it is wide. Which of the two
 * the music is we cannot tell, so the gain is the mean (in dB) of the two readings. */
static float kfactor(const struct ui *u)
{
	static float f[NSB], g[NSB];
	static unsigned rate = 1;
	float st = 0, sg = 0, sf = 0, sgf = 0;
	int i;

	if (rate != u->sp_frate) {
		rate = u->sp_frate;
		for (i = 0; i < NSB; i++) {
			f[i] = vis_band_hz(u, i);
			g[i] = kgain(f[i]);
		}
	}
	for (i = 0; i < NSB; i++) {
		float p = vis_pow(u->sp_b[i]);

		st += p;
		sg += g[i] * p;
		sf += f[i] * p;
		sgf += g[i] * f[i] * p;
	}
	if (st < 1e-12f || sf < 1e-9f)
		return 1.0f;
	st = sqrtf((sg / st) * (sgf / sf));
	return st < 0.25f ? 0.25f : st > 4.0f ? 4.0f : st;
}

static float lufs(float z)
{
	return z < 1e-10f ? -100.0f : -0.691f + 10.0f * log10f(z);
}

static int bin_of(float l)
{
	int b = (int)floorf((l + 70.0f) * 2.0f);

	return b < 0 ? 0 : b >= LOUD_BINS ? LOUD_BINS - 1 : b;
}

static float bin_l(int b)   /* the loudness in the middle of bin b */
{
	return -70.0f + ((float)b + 0.5f) * 0.5f;
}

/* the loudness over the last `ms` of frames; -100 while fewer than three quarters of that are there */
static float window(const struct vis_loud *m, int ms, float *zmean)
{
	double sum = 0;
	int span = 0, k = m->fh, n;

	for (n = 0; n < m->fn && span < ms; n++) {
		k = (k + LOUD_FEEDS - 1) % LOUD_FEEDS;
		sum += (double)m->z[k] * m->dt[k];
		span += m->dt[k];
	}
	if (span < ms * 3 / 4)
		return -100.0f;
	*zmean = (float)(sum / span);
	return lufs(*zmean);
}

/* integrated loudness and range from the histograms (BS.1770 / EBU 3342: an absolute gate at -70, a relative one 10 LU / 20 LU below) */
static void slow(struct vis_loud *m)
{
	double esum = 0, e2 = 0, lsum = 0;
	unsigned n = 0, n2 = 0, nl = 0, nl2 = 0, acc, want;
	int b, b0;
	float rel, lo = 0, hi = 0;

	for (b = 0; b < LOUD_BINS; b++) {
		esum += m->ienergy[b];
		n += m->icount[b];
	}
	if (!n) {
		m->integ = -100.0f;
	} else {
		rel = lufs((float)(esum / n)) - 10.0f;
		b0 = bin_of(rel);
		if (bin_l(b0) < rel && b0 < LOUD_BINS - 1)
			b0++;
		for (b = b0; b < LOUD_BINS; b++) {
			e2 += m->ienergy[b];
			n2 += m->icount[b];
		}
		m->integ = n2 ? lufs((float)(e2 / n2)) : -100.0f;
	}
	for (b = 0; b < LOUD_BINS; b++) {
		lsum += (double)m->lcount[b] * vis_pow(bin_l(b) + 0.691f);
		nl += m->lcount[b];
	}
	m->lra = -1.0f;
	if (nl < 10)
		return;
	rel = lufs((float)(lsum / nl)) - 20.0f;
	b0 = bin_of(rel);
	for (b = b0; b < LOUD_BINS; b++)
		nl2 += m->lcount[b];
	if (nl2 < 10)
		return;
	want = (unsigned)(0.10 * nl2);
	for (b = b0, acc = 0; b < LOUD_BINS; b++) {
		acc += m->lcount[b];
		if (acc > want) {
			lo = bin_l(b);
			break;
		}
	}
	want = (unsigned)(0.95 * nl2);
	for (b = b0, acc = 0; b < LOUD_BINS; b++) {
		acc += m->lcount[b];
		if (acc > want) {
			hi = bin_l(b);
			break;
		}
	}
	m->lra = hi > lo ? hi - lo : 0.0f;
}

static void init(struct vis_loud *m)
{
	m->mom = m->st = m->integ = -100.0f;
	m->lra = -1.0f;
	m->tpk = -140.0f;
}

static void feed(struct ui *u)
{
	struct vis_loud *m = &u->vm.loud;
	long dt;
	float z, zm = 0, pk;

	VIS_MEM_STEP(u, m, dt);
	if (dt < 0) {
		init(m);
		return;
	}
	if (dt == 0)
		return;
	if (dt > 400)
		dt = 400;
	pk = u->sp_l[0] > u->sp_l[1] ? u->sp_l[0] : u->sp_l[1];
	if (pk > m->tpk)
		m->tpk = pk;
	z = (vis_pow(u->sp_r[0]) + vis_pow(u->sp_r[1])) * kfactor(u);
	m->z[m->fh] = z;
	m->dt[m->fh] = (short)dt;
	m->fh = (m->fh + 1) % LOUD_FEEDS;
	if (m->fn < LOUD_FEEDS)
		m->fn++;
	m->mom = window(m, 400, &zm);
	if (m->mom > -70.0f) {
		int b = bin_of(m->mom);

		m->icount[b]++;
		m->ienergy[b] += zm;
	}
	m->st = window(m, 3000, &zm);
	m->hist_ms += dt;
	if (m->hist_ms >= 1000) {
		m->hist_ms -= 1000;
		m->hist[m->hh] = m->st;
		m->hh = (m->hh + 1) % LOUD_HIST;
		if (m->hn < LOUD_HIST)
			m->hn++;
		if (m->st > -70.0f)
			m->lcount[bin_of(m->st)]++;
		slow(m);
	}
}

/* a four-minute history for the picture in the menu: music that swells and falls back, around 14 LUFS */
static void demo(struct ui *u)
{
	struct vis_loud *m = &u->vm.loud;
	int i;

	memset(m, 0, sizeof *m);
	init(m);
	m->t_ok = 1;
	m->input = u->input;
	for (i = 0; i < LOUD_HIST; i++) {
		float t = (float)i;

		m->hist[i] = -15.5f + 3.5f * sinf(t * 0.11f) + 2.0f * sinf(t * 0.43f + 1.0f) - (i > 150 && i < 170 ? 7.0f : 0.0f);
	}
	m->hh = 0;
	m->hn = LOUD_HIST;
	m->mom = -12.6f;
	m->st = m->hist[LOUD_HIST - 1];
	m->integ = -14.1f;
	m->lra = 7.4f;
	m->tpk = -0.8f;
}

static void fmt(char *out, size_t n, float v)
{
	if (v <= -70.0f)
		snprintf(out, n, "--");
	else
		snprintf(out, n, "%.1f", (double)v);
}

static int ypos(int y, int h, float l)
{
	if (l < LOW)
		l = LOW;
	if (l > HIGH)
		l = HIGH;
	return y + h - (int)((l - LOW) / (HIGH - LOW) * (float)h + 0.5f);
}

static void dotted(struct x45_surface *s, int x, int y, int w, uint32_t c)
{
	int i;

	for (i = 0; i < w; i += 8)
		x45_hline(s, x + i, y, w - i < 4 ? w - i : 4, c);
}

static void draw(struct ui *u, int x, int y, int w, int h)
{
	struct x45_surface *s = &u->g.back;
	const struct vis_loud *m = &u->vm.loud;
	int rw = w >= 700 ? 214 : w >= 440 ? 170 : 0;
	int gw = w - (rw ? rw + 12 : 0), title = h >= 90 ? 20 : 0, gy = y + title, gh = h - title, lw = 40, px, i, npts;
	char buf[40];

	if (h < 40 || gw < 120)
		return;
	if (title)
		x45_text(s, x, y, "LOUDNESS, LUFS (est.)", 2, u->t.dim);
	x45_fill_rect(s, x, gy, gw, gh, u->t.barbg);
	px = x + gw - 6;   /* the newest column ends here */
	npts = (px - (x + lw)) / 2;
	if (npts > m->hn)
		npts = m->hn;
	for (i = 0; i < npts; i++) {
		float v = m->hist[(m->hh - npts + i + 2 * LOUD_HIST) % LOUD_HIST];
		int top, cx = px - (npts - i) * 2;
		uint32_t c = v > -9.0f ? u->t.warn : v > -14.5f ? u->t.accent : u->t.barfg;

		if (v <= LOW)
			continue;
		top = ypos(gy, gh, v);
		x45_fill_rect(s, cx, top, 2, gy + gh - top, vis_mix(u->t.barbg, c, 55));
		x45_fill_rect(s, cx, top, 2, 2, c);
	}
	{   /* the marks, written over the columns */
		static const struct { float l; const char *t; } mark[] = { { -14.0f, "-14" }, { -23.0f, "-23" }, { -30.0f, "-30" } };
		int k;

		for (k = 0; k < 3; k++) {
			int my = ypos(gy, gh, mark[k].l);

			if (my < gy + 2 || my > gy + gh - 2)
				continue;
			dotted(s, x + lw - 4, my, gw - lw + 4 - 6, u->t.dim);
			x45_text(s, x + 2, my - 7, mark[k].t, 2, u->t.dim);
		}
	}
	if (m->mom > LOW)
		x45_fill_rect(s, x + gw - 6, ypos(gy, gh, m->mom) - 1, 6, 3, u->t.text);   /* the momentary value, at the right edge */
	if (m->hn < 3)
		x45_text(s, x + lw + 10, gy + gh / 2 - 7, "collecting...", 2, u->t.dim);
	x45_draw_rect(s, x, gy, gw, gh, u->t.line);

	if (rw) {
		static const char *const names[] = { "SHORT-TERM", "MOMENTARY", "INTEGRATED", "RANGE (LU)", "PEAK (dBFS)" };
		float val[5] = { m->st, m->mom, m->integ, m->lra < 0 ? -100.0f : m->lra, m->tpk <= -139.0f ? -100.0f : m->tpk };
		int rx = x + gw + 12, rh = h / 5, two = rh >= 40, n = 5, row;

		if (!two && rh < 18)
			n = h / 18;
		if (!two)
			rh = h / (n ? n : 1) > 26 ? 26 : h / (n ? n : 1);
		for (row = 0; row < n; row++) {
			int ry = y + row * rh;

			fmt(buf, sizeof buf, val[row]);
			if (two) {
				x45_text(s, rx, ry + 2, names[row], 2, u->t.dim);
				x45_text(s, rx, ry + 2 + 15, buf, 3, row == 0 ? u->t.accent : (row == 4 && m->tpk >= -0.1f ? u->t.warn : u->t.text));
			} else {
				x45_text(s, rx, ry + (rh - 14) / 2, names[row], 2, u->t.dim);
				x45_text(s, rx + rw - x45_text_w(buf, 2), ry + (rh - 14) / 2, buf, 2, row == 0 ? u->t.accent : u->t.text);
			}
		}
	} else {
		fmt(buf, sizeof buf, m->st);
		x45_text(s, x + gw - 8 - x45_text_w(buf, 3), gy + 6, buf, 3, u->t.accent);
	}
}

const struct ui_vis vis_loudness = {
	.name = "loudness", .label = "Loudness history", .bit = 512, .weight = 3, .analyzer = 1, .feed = feed, .draw = draw, .demo = demo,
};
