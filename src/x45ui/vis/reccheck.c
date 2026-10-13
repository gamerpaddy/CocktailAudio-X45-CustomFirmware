#define _GNU_SOURCE
/* reccheck - eight lamps for judging a recording, or a transfer that is about to become one (a record, a tape, a line-in
 * level): the highest peak and the clips, the dynamics, the noise floor, the level of the two channels against each other, the
 * phase between them, subsonic rumble and mains hum. Each reads green, amber or red.
 *
 * Everything counts from the moment the playback began (another input, or the analyzer away for more than 3 s, starts it afresh).
 * The noise floor and the hum are measured in the quietest second, so they say something once there has been a quiet passage:
 * the lead-in groove of a record, the silence before the music. */
#include "vis.h"

#include <math.h>
#include <stdio.h>

enum { OK, WARN, BAD, NA };

static void init(struct vis_rec *m)
{
	m->peak = -140.0f;
	m->floor_db = 1000.0f;   /* no second yet */
	m->top_db = -140.0f;
	m->corr_min = 1.0f;
	m->sub = -140.0f;
	m->hum = -140.0f;
	m->blk_pk = -140.0f;
}

static float db_of(double z)
{
	return z < 1e-14 ? -140.0f : 10.0f * log10f((float)z);
}

/* the mean level of the bands that make up lo .. hi Hz, in dB of power */
static float band_mean(const struct ui *u, float lo, float hi)
{
	int a = vis_band_of(u, lo), z = vis_band_of(u, hi), i;
	double p = 0;

	for (i = a; i <= z; i++)
		p += vis_pow(u->sp_b[i]);
	return db_of(p / (z - a + 1));
}

/* a mains line (f Hz) against two places beside it, dB */
static float prominence(const struct ui *u, float f, float below, float above)
{
	float ref = 0.5f * (u->sp_b[vis_band_of(u, below)] + u->sp_b[vis_band_of(u, above)]);

	return u->sp_b[vis_band_of(u, f)] - ref;
}

static void block_done(struct vis_rec *m)
{
	float rms = db_of((m->blk_z[0] + m->blk_z[1]) * 0.5 / (double)m->blk_ms), c = m->blk_c / (float)m->blk_ms;

	m->b_rms[m->bh] = rms;
	m->b_pk[m->bh] = m->blk_pk;
	m->bh = (m->bh + 1) % REC_BLOCKS;
	if (m->bn < REC_BLOCKS)
		m->bn++;
	if (rms > -100.0f) {
		if (rms < m->floor_db)
			m->floor_db = rms;
		if (rms > m->top_db)
			m->top_db = rms;
	}
	if (rms > -60.0f) {   /* the correlation of silence says nothing */
		if (c < m->corr_min)
			m->corr_min = c;
		m->corr_sum += (double)c * (double)m->blk_ms;
		m->corr_ms += m->blk_ms;
	}
	m->blk_ms = 0;
	m->blk_z[0] = m->blk_z[1] = 0;
	m->blk_pk = -140.0f;
	m->blk_c = 0;
}

static void feed(struct ui *u)
{
	struct vis_rec *m = &u->vm.rec;
	long dt;
	float pk, lvl, k;
	double zl, zr;

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
	if (pk > m->peak)
		m->peak = pk;
	if (pk >= -0.1f) {
		if (!m->clip_run)
			m->clips++;
		m->clip_run = 1;
	} else {
		m->clip_run = 0;
	}
	zl = vis_pow(u->sp_r[0]);
	zr = vis_pow(u->sp_r[1]);
	m->blk_z[0] += zl * (double)dt;
	m->blk_z[1] += zr * (double)dt;
	if (pk > m->blk_pk)
		m->blk_pk = pk;
	m->blk_c += u->sp_c * (float)dt;
	m->blk_ms += dt;
	lvl = db_of((zl + zr) * 0.5);
	if (lvl > -60.0f) {   /* music: the channels' energy, and the low end against the bass */
		m->lr_z[0] += zl * (double)dt;
		m->lr_z[1] += zr * (double)dt;
		k = band_mean(u, 20.0f, 32.0f) - band_mean(u, 100.0f, 250.0f);
		if (!m->sub_ok) {
			m->sub = k;
			m->sub_ok = 1;
		} else {
			m->sub += (k - m->sub) * (dt >= 4000 ? 1.0f : (float)dt / 4000.0f);
		}
	}
	if (lvl < -50.0f && lvl > -110.0f) {   /* quiet, but not dead: is there a mains line in it? */
		float p50 = prominence(u, 50.0f, 30.0f, 85.0f), p60 = prominence(u, 60.0f, 38.0f, 95.0f), p = p50 > p60 ? p50 : p60;

		if (m->hum <= -139.0f)
			m->hum = p;
		else
			m->hum += (p - m->hum) * (dt >= 2000 ? 1.0f : (float)dt / 2000.0f);
	}
	if (m->blk_ms >= 1000)
		block_done(m);
}

/* ---- the lamps ---- */

struct lamp {
	const char *label;
	char value[24];
	char hint[40];
	int state;
};

static int cmp_f(const void *a, const void *b)
{
	float x = *(const float *)a, y = *(const float *)b;

	return x < y ? 1 : x > y ? -1 : 0;   /* largest first */
}

/* the crest factor (peak against RMS, dB) over the loudest fifth of the seconds of the last minute */
static int crest(const struct vis_rec *m, float *out)
{
	float rms[REC_BLOCKS], c[REC_BLOCKS], order[REC_BLOCKS], sum = 0;
	int n = 0, i, top, k;

	for (i = 0; i < m->bn; i++)
		if (m->b_rms[i] > -60.0f) {
			rms[n] = m->b_rms[i];
			c[n] = m->b_pk[i] - m->b_rms[i];
			order[n] = rms[n];
			n++;
		}
	if (n < 5)
		return 0;
	qsort(order, (size_t)n, sizeof *order, cmp_f);
	top = n / 5 > 0 ? n / 5 : 1;
	for (i = 0, k = 0; i < n; i++)
		if (rms[i] >= order[top - 1] && k < top) {
			sum += c[i];
			k++;
		}
	*out = sum / (float)k;
	return 1;
}

static void lamps(const struct vis_rec *m, struct lamp l[8])
{
	float v, t;
	int i;

	for (i = 0; i < 8; i++) {
		l[i].value[0] = l[i].hint[0] = 0;
		l[i].state = NA;
	}
	l[0].label = "PEAK";
	l[1].label = "CLIPS";
	l[2].label = "DYNAMICS";
	l[3].label = "NOISE FLOOR";
	l[4].label = "L / R LEVEL";
	l[5].label = "PHASE";
	l[6].label = "SUBSONIC";
	l[7].label = "MAINS HUM";
	snprintf(l[0].value, sizeof l[0].value, "--");
	snprintf(l[1].value, sizeof l[1].value, "--");
	if (m->peak > -139.0f) {
		snprintf(l[0].value, sizeof l[0].value, "%.1f dB", (double)m->peak);
		if (m->peak >= -18.0f)
			snprintf(l[0].hint, sizeof l[0].hint, "headroom %.1f dB", (double)-m->peak);
		else
			snprintf(l[0].hint, sizeof l[0].hint, "level is low");
		l[0].state = m->peak >= -0.1f ? BAD : m->peak >= -1.0f ? WARN : m->peak >= -18.0f ? OK : WARN;
		snprintf(l[1].value, sizeof l[1].value, "%d", m->clips);
		snprintf(l[1].hint, sizeof l[1].hint, "stretches at full scale");
		l[1].state = m->clips == 0 ? OK : m->clips < 3 ? WARN : BAD;
	}
	snprintf(l[2].value, sizeof l[2].value, "--");
	snprintf(l[2].hint, sizeof l[2].hint, "peak against average");
	if (crest(m, &v)) {
		snprintf(l[2].value, sizeof l[2].value, "%.0f dB", (double)v);
		l[2].state = v >= 8.0f ? OK : WARN;
		snprintf(l[2].hint, sizeof l[2].hint, v >= 14.0f ? "open" : v >= 8.0f ? "normal" : "squashed");
	}
	snprintf(l[3].value, sizeof l[3].value, "--");
	snprintf(l[3].hint, sizeof l[3].hint, "needs a quiet moment");
	if (m->bn >= 3 && m->floor_db <= 0 && m->floor_db <= m->top_db - 12.0f) {
		snprintf(l[3].value, sizeof l[3].value, "%.0f dB", (double)m->floor_db);
		snprintf(l[3].hint, sizeof l[3].hint, "quietest second");
		l[3].state = m->floor_db <= -60.0f ? OK : m->floor_db <= -45.0f ? WARN : BAD;
	}
	snprintf(l[4].value, sizeof l[4].value, "--");
	if (m->lr_z[0] > 1e-9 && m->lr_z[1] > 1e-9) {
		t = db_of(m->lr_z[0]) - db_of(m->lr_z[1]);
		snprintf(l[4].value, sizeof l[4].value, "%c %.1f dB", t >= 0 ? 'L' : 'R', (double)fabsf(t));
		snprintf(l[4].hint, sizeof l[4].hint, "the louder channel");
		l[4].state = fabsf(t) < 1.0f ? OK : fabsf(t) < 2.5f ? WARN : BAD;
	}
	snprintf(l[5].value, sizeof l[5].value, "--");
	if (m->corr_ms >= 3000) {
		v = (float)(m->corr_sum / (double)m->corr_ms);
		snprintf(l[5].value, sizeof l[5].value, "%+.2f", (double)v);
		snprintf(l[5].hint, sizeof l[5].hint, "worst second %+.2f", (double)m->corr_min);
		l[5].state = v >= 0.3f ? OK : v >= 0.0f ? WARN : BAD;
	}
	snprintf(l[6].value, sizeof l[6].value, "--");
	if (m->sub_ok) {
		snprintf(l[6].value, sizeof l[6].value, "%.0f dB", (double)m->sub);
		snprintf(l[6].hint, sizeof l[6].hint, "rumble, warp: under 32 Hz");
		l[6].state = m->sub <= -10.0f ? OK : m->sub <= 0.0f ? WARN : BAD;
	}
	snprintf(l[7].value, sizeof l[7].value, "--");
	snprintf(l[7].hint, sizeof l[7].hint, "needs silence");
	if (m->hum > -139.0f) {
		snprintf(l[7].value, sizeof l[7].value, "%.0f dB", (double)m->hum);
		snprintf(l[7].hint, sizeof l[7].hint, "50 / 60 Hz line");
		l[7].state = m->hum < 6.0f ? OK : m->hum < 12.0f ? WARN : BAD;
	}
}

static void draw(struct ui *u, int x, int y, int w, int h)
{
	static const char *const shortl[8] = { "PEAK", "CLIPS", "DYN.", "NOISE", "L / R", "PHASE", "SUB", "HUM" };
	struct x45_surface *s = &u->g.back;
	struct lamp l[8];
	const int gap = 8;
	int cols = w >= 560 ? 4 : 2, rows = 8 / cols, tw = (w - gap * (cols - 1)) / cols, th = (h - gap * (rows - 1)) / rows, i;
	uint32_t col[4] = { x45_rgb(&u->g, 92, 204, 122), x45_rgb(&u->g, 240, 190, 62), x45_rgb(&u->g, 238, 86, 74), u->t.dim };

	if (th < 26 || tw < 90) {
		rows = 1;   /* a strip: four lamps in a row */
		cols = 4;
		tw = (w - gap * 3) / 4;
		th = h;
		if (th < 26 || tw < 60)
			return;
	}
	lamps(&u->vm.rec, l);
	for (i = 0; i < (rows == 1 ? 4 : 8); i++) {
		int tx = x + (i % cols) * (tw + gap), ty = y + (i / cols) * (th + gap), ts = th >= 56 ? 3 : 2, vw;
		uint32_t c = col[l[i].state], vc = l[i].state == NA ? u->t.dim : u->t.text;
		const char *label = tw >= 150 ? l[i].label : shortl[i];
		char v[24], *unit;

		snprintf(v, sizeof v, "%s", l[i].value);
		if (tw < 130 && (unit = strstr(v, " dB")))   /* a narrow tile has room for the number only */
			*unit = 0;
		x45_fill_rect(s, tx, ty, tw, th, u->t.barbg);
		x45_fill_rect(s, tx, ty, 4, th, c);   /* the lamp: a bar down the tile's edge */
		if (th >= 44) {
			x45_text_clipped(s, tx + 12, ty + 5, label, 2, tw < 130 ? tw - 15 : tw - 20, u->t.dim);
			x45_text_clipped(s, tx + 12, ty + 5 + 19, v, ts, tw - 20, vc);
			if (th >= 5 + 19 + 8 * ts + 22 && l[i].hint[0] && tw >= 150)
				x45_text_clipped(s, tx + 12, ty + th - 19, l[i].hint, 2, tw - 20, u->t.dim);
		} else {
			vw = x45_text_w(v, 2);
			x45_text_clipped(s, tx + 12, ty + (th - 14) / 2, label, 2, tw - 28 - vw, u->t.dim);
			x45_text(s, tx + tw - 8 - vw, ty + (th - 14) / 2, v, 2, vc);
		}
	}
}

static int cap(int w)
{
	return 250;
}

const struct ui_vis vis_reccheck = {
	.name = "reccheck", .label = "Record check", .bit = 1024, .weight = 3, .cap = cap, .analyzer = 1, .feed = feed, .draw = draw,
};
