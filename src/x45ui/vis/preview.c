#define _GNU_SOURCE
/* preview - the visualizers menu (Options > Visualizers, and the RETURN menu of the player): a card for each visual
 * with a still picture of it, so that a choice is made by looking.
 *
 * The picture is the visual's own draw() code, run on a copy of the panel's state that holds made-up music (a
 * spectrum with a bass line and a voice, a wave of two tones, levels, a waterfall history). The copy shares the
 * screen with the original and nothing else: what the real analyzer fed, and what the visuals keep between frames,
 * stay as they were (the few that keep state in the file say how to forget it: ui_vis.reset). So a new visual shows
 * up in the menu with its picture without a line in this file.
 */
#include "vis.h"
#include "../skins/skin.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COLS 3          /* cards in a row */
#define MAXV 16         /* cards the menu holds (main.c: VISSUB_MAX) */

int vis_menu_cols(void)
{
	return COLS;
}

/* made-up music at time step k: a spectrum that falls towards the treble, a bass line that pulses, a voice */
static void spectrum_at(struct ui *p, float k)
{
	int i;

	for (i = 0; i < NSB; i++) {
		float db = -17.0f - 0.27f * (float)i + 5.0f * sinf((float)i * 0.52f + k * 0.4f) + 3.0f * sinf((float)i * 1.43f + 1.0f);

		db += (9.0f + 5.0f * sinf(k * 0.9f)) * expf(-(float)((i - 14) * (i - 14)) / 40.0f);   /* the bass line */
		db += 7.0f * expf(-(float)((i - 58) * (i - 58)) / 70.0f);                              /* a voice */
		p->sp_b[i] = db < -80.0f ? -80.0f : db > -3.0f ? -3.0f : db;
	}
	{   /* an A minor chord and its scale, over the octaves: something for the key wheel to find */
		static const int tone[] = { 57, 60, 64, 69, 72, 76, 81, 84, 88 }, scale[] = { 62, 65, 67, 74, 77, 79 };
		int n;

		for (n = 0; n < (int)(sizeof tone / sizeof *tone); n++)
			p->sp_b[vis_band_of(p, 440.0f * powf(2.0f, (float)(tone[n] - 69) / 12.0f))] += 12.0f;
		for (n = 0; n < (int)(sizeof scale / sizeof *scale); n++)
			p->sp_b[vis_band_of(p, 440.0f * powf(2.0f, (float)(scale[n] - 69) / 12.0f))] += 4.0f;
		for (i = 0; i < NSB; i++)
			if (p->sp_b[i] > -3.0f)
				p->sp_b[i] = -3.0f;
	}
	/* the side signal: the bass is nearly mono, the rest gets wider towards the treble */
	for (i = 0; i < NSB; i++)
		p->sp_sb[i] = p->sp_b[i] - (i < 33 ? 26.0f - 0.2f * (float)i : 9.0f - 6.0f * (float)i / (float)NSB) + 2.0f * sinf((float)i * 0.9f + k);
}

static void demo(struct ui *p, long now)
{
	static unsigned char hist[WF_ROWS][NSB];
	static struct vis_mem mem;   /* what the history visuals keep after the same music */
	static int hist_ready;
	int i, k;

	p->sp_have = 1;
	p->sp_wave = 1;
	p->sp_side = 1;
	p->sp_frate = 44100;
	p->sp_c = 0.55f;
	p->sp_r[0] = -15.0f;
	p->sp_r[1] = -17.0f;
	p->sp_l[0] = -7.0f;
	p->sp_l[1] = -9.0f;
	p->sp_pkh[0] = -6.0f;
	p->sp_pkh[1] = -8.0f;
	for (i = 0; i < 200; i++) {
		float t = (float)i / 200.0f;

		p->sp_wl[i] = (short)(5200.0f * sinf(6.2832f * 3.0f * t) + 1800.0f * sinf(6.2832f * 8.0f * t + 0.7f));
		p->sp_wr[i] = (short)(4600.0f * sinf(6.2832f * 3.0f * t + 0.25f) + 2200.0f * sinf(6.2832f * 8.0f * t + 1.2f));
	}
	if (!hist_ready) {                   /* the waterfall's history: the same music, moving (worked out once) */
		p->wf_head = p->wf_n = 0;
		memset(&p->vm, 0, sizeof p->vm);
		for (k = 0; k < WF_ROWS; k++) {
			float quiet = k < 31 ? 1.0f : 0.0f;   /* a quiet start, as before a record's first song (the oldest waterfall rows, which no card shows) */

			spectrum_at(p, (float)k);
			if (quiet > 0) {   /* a noise floor with a faint 50 Hz line */
				for (i = 0; i < NSB; i++)
					p->sp_b[i] = p->sp_sb[i] = -90.0f + 2.0f * sinf((float)(i * 7 + k));
				p->sp_b[vis_band_of(p, 50.0f)] += 4.0f;
			}
			p->sp_r[0] = quiet > 0 ? -66.0f : -15.0f + 4.0f * sinf((float)k * 0.31f);
			p->sp_r[1] = quiet > 0 ? -67.0f : -16.5f + 4.0f * sinf((float)k * 0.31f + 0.2f);
			p->sp_l[0] = quiet > 0 ? -55.0f : p->sp_r[0] + 8.0f + 2.0f * sinf((float)k);
			p->sp_l[1] = quiet > 0 ? -55.0f : p->sp_r[1] + 8.0f + 2.0f * sinf((float)k + 1.0f);
			p->sp_read_t += 80;
			vis_feed(p);
		}
		memcpy(hist, p->wf, sizeof hist);
		memcpy(&mem, &p->vm, sizeof mem);
		hist_ready = 1;
	} else {
		memcpy(p->wf, hist, sizeof hist);
		memcpy(&p->vm, &mem, sizeof mem);
		p->wf_head = 0;
		p->wf_n = WF_ROWS;
	}
	for (i = 0; i < vis_count(); i++)
		if (vis_at(i)->demo)
			vis_at(i)->demo(p);
	p->sp_r[0] = -15.0f;
	p->sp_r[1] = -17.0f;
	p->sp_l[0] = -7.0f;
	p->sp_l[1] = -9.0f;
	spectrum_at(p, 3.0f);
	memset(p->sp_pkb, 0, sizeof p->sp_pkb);
	p->sp_pk_t = 0;
	p->sp_pk_nb = 0;
	p->vu_n[0] = -30.0f;
	p->vu_n[1] = -30.0f;
	p->vu_t = now;
	for (i = 0; i < vis_count(); i++)    /* the dials' needles swing up to the levels above */
		if (vis_at(i)->frame) {
			vis_at(i)->frame(p, now, 1);
			vis_at(i)->frame(p, now + 600, 1);
		}
}

/* one visual in its box, the way the screen would give it room */
static void paint(struct ui *p, int i, int x, int y, int w, int h)
{
	const struct ui_vis *v = vis_at(i);
	int cap = v->cap ? v->cap(w) : 100000;

	if (h > cap) {
		y += (h - cap) / 2;
		h = cap;
	}
	if (v->square && w > h) {
		x += (w - h) / 2;
		w = h;
	}
	p->dial_pass = 0;
	v->draw(p, x, y, w, h);
}

/* a copy of the panel's state to draw on; the screen itself is shared */
static struct ui *scratch;

static struct ui *preview_begin(const struct ui *u)
{
	if (!scratch)
		scratch = malloc(sizeof *scratch);
	if (!scratch)
		return NULL;
	memcpy(scratch, u, sizeof *scratch);
	demo(scratch, vis_now());
	return scratch;
}

static void preview_end(void)
{
	int i;

	for (i = 0; i < vis_count(); i++)
		if (vis_at(i)->reset)
			vis_at(i)->reset();
}

void vis_menu_draw(struct ui *u)
{
	struct x45_surface *s = &u->g.back;
	const struct vis_sel *sel = u->vis_all ? &u->vis_shared : &u->vis[u->input];
	const int mx = 16, gap = 12, y0 = 64, y1 = s->h - 92, lh = x45_text_h(2);
	int n = vis_count() < MAXV ? vis_count() : MAXV, rows = (n + COLS - 1) / COLS, i;
	int cw = (s->w - 2 * mx - gap * (COLS - 1)) / COLS, ch = (y1 - y0 - gap * (rows - 1)) / rows;
	int th = ch - 8 - 6 - lh - 8;   /* the picture: what a card leaves between its margins and the label */
	struct ui *p = th >= 24 ? preview_begin(u) : NULL;

	for (i = 0; i < n; i++) {
		const struct ui_vis *v = vis_at(i);
		int cx = mx + (i % COLS) * (cw + gap), cy = y0 + (i / COLS) * (ch + gap);
		int cur = i == u->sel, on = (sel->on & v->bit) != 0, ly = cy + 8 + th + 6, stw;
		const char *st = !on ? "off" : sel->first == v->bit ? "on, first" : "on";

		skin_round_rect(s, cx, cy, cw, ch, 10, cur ? skin_mix(u->t.card, u->t.accent, 22) : u->t.card);
		x45_fill_rect(s, cx + 8, cy + 8, cw - 16, th, u->t.barbg);
		if (p)
			paint(p, i, cx + 8, cy + 8, cw - 16, th);
		if (cur)
			skin_round_outline(s, cx, cy, cw, ch, 10, 3, u->t.accent);
		stw = x45_text_w(st, 2);
		x45_text(s, cx + cw - 12 - stw, ly, st, 2, on ? u->t.accent : u->t.dim);
		x45_text_clipped(s, cx + 12, ly, v->label, 2, cw - 24 - stw - 10, cur || on ? u->t.text : u->t.dim);
	}
	if (p)
		preview_end();
}
