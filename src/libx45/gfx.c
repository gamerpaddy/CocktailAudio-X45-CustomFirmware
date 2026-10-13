/* libx45/gfx  -  see gfx.h. */
#define _GNU_SOURCE
#include "gfx.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define FBDEV "/dev/fb0"

/* X45_FBFILE=path: a 1024x600, 32 bit file stands in for the panel (see build/preview.sh) */
static int open_file(struct x45_gfx *g, const char *path)
{
	const int w = 1024, h = 600;
	size_t px = (size_t)w * h;

	g->fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
	if (g->fd < 0)
		return -errno;
	if (ftruncate(g->fd, (off_t)(px * 4)) < 0) {
		int e = -errno;
		close(g->fd);
		return e;
	}
	g->shift_r = 16;
	g->shift_g = 8;
	g->shift_b = 0;
	g->fb_len = px * 4;
	g->fb = mmap(NULL, g->fb_len, PROT_READ | PROT_WRITE, MAP_SHARED, g->fd, 0);
	if (g->fb == MAP_FAILED) {
		int e = -errno;
		close(g->fd);
		g->fb = NULL;
		return e;
	}
	g->back.w = w;
	g->back.h = h;
	g->back.stride = w;
	g->back.pixels = calloc(px, sizeof(uint32_t));
	if (!g->back.pixels)
		return -ENOMEM;
	x45_clip_reset(&g->back);
	return 0;
}

int x45_gfx_open(struct x45_gfx *g)
{
	struct fb_var_screeninfo var;
	struct fb_fix_screeninfo fix;
	size_t px;

	const char *file = getenv("X45_FBFILE");

	memset(g, 0, sizeof *g);
	if (file && *file)
		return open_file(g, file);   /* a preview on a host without the panel: the frame lands in a file */
	g->fd = open(FBDEV, O_RDWR | O_CLOEXEC);
	if (g->fd < 0)
		return -errno;
	if (ioctl(g->fd, FBIOGET_VSCREENINFO, &var) < 0 ||
	    ioctl(g->fd, FBIOGET_FSCREENINFO, &fix) < 0) {
		int e = -errno;
		close(g->fd);
		return e;
	}
	if (var.bits_per_pixel != 32) {
		close(g->fd);
		return -ENOTSUP;
	}
	g->shift_r = var.red.offset;
	g->shift_g = var.green.offset;
	g->shift_b = var.blue.offset;

	g->fb_len = (size_t)fix.line_length * var.yres;
	g->fb = mmap(NULL, g->fb_len, PROT_READ | PROT_WRITE, MAP_SHARED, g->fd, 0);
	if (g->fb == MAP_FAILED) {
		int e = -errno;
		close(g->fd);
		g->fb = NULL;
		return e;
	}

	g->back.w = var.xres;
	g->back.h = var.yres;
	g->back.stride = fix.line_length / 4;
	px = (size_t)g->back.stride * g->back.h;
	g->back.pixels = malloc(px * sizeof(uint32_t));
	if (!g->back.pixels) {
		munmap(g->fb, g->fb_len);
		close(g->fd);
		return -ENOMEM;
	}
	memset(g->back.pixels, 0, px * sizeof(uint32_t));
	x45_clip_reset(&g->back);
	return 0;
}

void x45_gfx_close(struct x45_gfx *g)
{
	free(g->shown);
	g->shown = NULL;
	free(g->back.pixels);
	g->back.pixels = NULL;
	if (g->fb)
		munmap(g->fb, g->fb_len);
	g->fb = NULL;
	if (g->fd >= 0)
		close(g->fd);
	g->fd = -1;
}

/* The panel's memory is slow to write, so only the rows that changed since the last frame are sent;
 * every four seconds the whole frame goes anyway, which also repairs anything another program drew. */
static void present_rows(struct x45_gfx *g, int y0, int y1);

void x45_gfx_present_rows(struct x45_gfx *g, int y0, int y1)
{
	if (!g->shown || y0 < 0 || y1 > g->back.h || y0 >= y1) {
		x45_gfx_present(g);
		return;
	}
	present_rows(g, y0, y1);
}

void x45_gfx_present(struct x45_gfx *g)
{
	struct timespec ts;
	long now;
	size_t stride_b = (size_t)g->back.stride * 4;
	int rows = g->back.h;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	now = ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
	if (!g->shown) {
		g->shown = malloc(g->fb_len);
		g->shown_t = now - 10000;   /* nothing was sent yet: the whole frame goes (the clock is a 32-bit count, so
					     * "long ago" is made from now, not taken to be 0) */
	}
	if (!g->shown || (long)((unsigned long)now - (unsigned long)g->shown_t) > 4000 ||
	    stride_b * (size_t)rows > g->fb_len) {
		memcpy(g->fb, g->back.pixels, g->fb_len);
		if (g->shown)
			memcpy(g->shown, g->back.pixels, g->fb_len);
		g->shown_t = now;
		return;
	}
	present_rows(g, 0, rows);
}

static void present_rows(struct x45_gfx *g, int y0, int y1)
{
	int y;

	for (y = y0; y < y1; y++) {
		/* the panel's memory is the slow part (about 40 MB/s): send only the pixels from the first to the
		 * last one that differs in this row */
		const uint32_t *nw = g->back.pixels + (size_t)y * g->back.stride;
		uint32_t *old = g->shown + (size_t)y * g->back.stride;
		uint32_t *dst = g->fb + (size_t)y * g->back.stride;
		int w = g->back.w, i = 0;

		/* runs of differing pixels; runs closer than 24 pixels are sent as one */
		while (i < w) {
			int lo, hi, gap;

			while (i < w && nw[i] == old[i])
				i++;
			if (i >= w)
				break;
			lo = hi = i;
			for (gap = 0; i < w && gap < 24; i++) {
				if (nw[i] != old[i]) {
					hi = i;
					gap = 0;
				} else {
					gap++;
				}
			}
			memcpy(dst + lo, nw + lo, (size_t)(hi - lo + 1) * 4);
			memcpy(old + lo, nw + lo, (size_t)(hi - lo + 1) * 4);
		}
	}
}

int x45_gfx_save(struct x45_gfx *g, const char *path)
{
	FILE *f = fopen(path, "wb");
	size_t n;

	if (!f)
		return -errno;
	n = fwrite(g->fb, 1, g->fb_len, f);
	fclose(f);
	return n == g->fb_len ? 0 : -EIO;
}

int x45_gfx_restore(struct x45_gfx *g, const char *path)
{
	FILE *f = fopen(path, "rb");
	size_t n;

	if (!f)
		return -errno;
	n = fread(g->fb, 1, g->fb_len, f);
	fclose(f);
	free(g->shown);   /* the panel no longer shows what was sent last: the next frame goes out whole */
	g->shown = NULL;
	return n == g->fb_len ? 0 : -EIO;
}

uint32_t x45_rgb(const struct x45_gfx *g, uint8_t r, uint8_t gg, uint8_t b)
{
	return ((uint32_t)r << g->shift_r) |
	       ((uint32_t)gg << g->shift_g) |
	       ((uint32_t)b << g->shift_b);
}

/* ------------------------------------------------------------------ clip --- */

void x45_clip_reset(struct x45_surface *s)
{
	s->clip_x = 0;
	s->clip_y = 0;
	s->clip_w = s->w;
	s->clip_h = s->h;
}

void x45_clip_set(struct x45_surface *s, int x, int y, int w, int h)
{
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > s->w) w = s->w - x;
	if (y + h > s->h) h = s->h - y;
	if (w < 0) w = 0;
	if (h < 0) h = 0;
	s->clip_x = x;
	s->clip_y = y;
	s->clip_w = w;
	s->clip_h = h;
}

/* --------------------------------------------------------------- drawing --- */

void x45_put(struct x45_surface *s, int x, int y, uint32_t c)
{
	if (x < s->clip_x || y < s->clip_y ||
	    x >= s->clip_x + s->clip_w || y >= s->clip_y + s->clip_h)
		return;
	s->pixels[(size_t)y * s->stride + x] = c;
}

void x45_fill_rect(struct x45_surface *s, int x, int y, int w, int h, uint32_t c)
{
	int yy, xx;

	/* clamp to the clip rect once, then run tight rows */
	if (x < s->clip_x) { w -= s->clip_x - x; x = s->clip_x; }
	if (y < s->clip_y) { h -= s->clip_y - y; y = s->clip_y; }
	if (x + w > s->clip_x + s->clip_w) w = s->clip_x + s->clip_w - x;
	if (y + h > s->clip_y + s->clip_h) h = s->clip_y + s->clip_h - y;
	if (w <= 0 || h <= 0)
		return;

	for (yy = y; yy < y + h; yy++) {
		uint32_t *row = s->pixels + (size_t)yy * s->stride + x;
		for (xx = 0; xx < w; xx++)
			row[xx] = c;
	}
}

void x45_clear(struct x45_surface *s, uint32_t c)
{
	x45_fill_rect(s, 0, 0, s->w, s->h, c);
}

void x45_hline(struct x45_surface *s, int x, int y, int w, uint32_t c)
{
	x45_fill_rect(s, x, y, w, 1, c);
}

void x45_vline(struct x45_surface *s, int x, int y, int h, uint32_t c)
{
	x45_fill_rect(s, x, y, 1, h, c);
}

void x45_draw_rect(struct x45_surface *s, int x, int y, int w, int h, uint32_t c)
{
	if (w <= 0 || h <= 0)
		return;
	x45_hline(s, x, y, w, c);
	x45_hline(s, x, y + h - 1, w, c);
	x45_vline(s, x, y, h, c);
	x45_vline(s, x + w - 1, y, h, c);
}

void x45_bar(struct x45_surface *s, int x, int y, int w, int h,
	     double frac, uint32_t fg, uint32_t bg, uint32_t border)
{
	int inner;

	if (frac < 0.0)
		frac = 0.0;
	if (frac > 1.0)
		frac = 1.0;
	x45_fill_rect(s, x, y, w, h, bg);
	x45_draw_rect(s, x, y, w, h, border);
	inner = (int)((w - 4) * frac + 0.5);
	if (inner > 0)
		x45_fill_rect(s, x + 2, y + 2, inner, h - 4, fg);
}

/* ------------------------------------------------------------------ text --- */

static int ttf_ok;
static int ttf_text_w(const char *str, int scale);
static void ttf_text(struct x45_surface *s, int x, int y, const char *str, int scale, uint32_t col, int max_chars);
#define USE_TTF() (x45_font_smooth >= 2 && ttf_ok)

int x45_text_h(int scale)
{
	return 7 * scale;
}

int x45_text_w(const char *str, int scale)
{
	size_t n = str ? strlen(str) : 0;
	if (n && USE_TTF() && scale >= 1 && scale <= 9)
		return ttf_text_w(str, scale);
	if (!n)
		return 0;
	/* 5 columns per glyph plus a 1-column gap, no gap after the last */
	return (int)(n * 6 * scale) - scale;
}

int x45_font_smooth;

/* ------------------------------------------------------------ TrueType --- */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"
#pragma GCC diagnostic pop

static stbtt_fontinfo ttf;
static unsigned char *ttf_data;
static int ttf_ok;

struct ttf_glyph {
	unsigned char *a;     /* coverage, w x h */
	int w, h, xoff, yoff; /* yoff: from the baseline (negative = above) */
	float adv;
	int have;
};
static struct ttf_glyph ttf_cache[128][10];

int x45_ttf_load(const char *path)
{
	FILE *f = fopen(path, "rb");
	long len;

	if (!f)
		return -1;
	fseek(f, 0, SEEK_END);
	len = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (len <= 0 || len > 16 * 1024 * 1024 || !(ttf_data = malloc((size_t)len))) {
		fclose(f);
		return -1;
	}
	if (fread(ttf_data, 1, (size_t)len, f) != (size_t)len ||
	    !stbtt_InitFont(&ttf, ttf_data, stbtt_GetFontOffsetForIndex(ttf_data, 0))) {
		free(ttf_data);
		ttf_data = NULL;
		fclose(f);
		return -1;
	}
	fclose(f);
	memset(ttf_cache, 0, sizeof ttf_cache);
	ttf_ok = 1;
	return 0;
}

int x45_ttf_ready(void)
{
	return ttf_ok;
}

#define USE_TTF() (x45_font_smooth >= 2 && ttf_ok)

static struct ttf_glyph *ttf_glyph(int c, int scale)
{
	struct ttf_glyph *gl;
	float sf;
	int adv, lsb, x0, y0, x1, y1;

	if (c < 32 || c > 126 || scale < 1 || scale > 9)
		c = '?';
	gl = &ttf_cache[c][scale];
	if (gl->have)
		return gl;
	sf = stbtt_ScaleForMappingEmToPixels(&ttf, 10.0f * (float)scale);
	stbtt_GetCodepointHMetrics(&ttf, c, &adv, &lsb);
	gl->adv = (float)adv * sf;
	stbtt_GetCodepointBitmapBox(&ttf, c, sf, sf, &x0, &y0, &x1, &y1);
	gl->w = x1 - x0;
	gl->h = y1 - y0;
	gl->xoff = x0;
	gl->yoff = y0;
	if (gl->w > 0 && gl->h > 0 && (gl->a = malloc((size_t)gl->w * (size_t)gl->h))) {
		memset(gl->a, 0, (size_t)gl->w * (size_t)gl->h);
		stbtt_MakeCodepointBitmap(&ttf, gl->a, gl->w, gl->h, gl->w, sf, sf, c);
	}
	gl->have = 1;
	return gl;
}

static void blend_mask(struct x45_surface *s, int x, int y, const unsigned char *m, int w, int h, uint32_t col)
{
	int px, py;

	for (py = 0; py < h; py++) {
		int yy = y + py;

		if (yy < s->clip_y || yy >= s->clip_y + s->clip_h)
			continue;
		for (px = 0; px < w; px++) {
			int a = m[py * w + px], xx = x + px, lane;
			uint32_t *d, out = 0;

			if (!a || xx < s->clip_x || xx >= s->clip_x + s->clip_w)
				continue;
			d = &s->pixels[(size_t)yy * s->stride + xx];
			if (a >= 250) {
				*d = col;
				continue;
			}
			for (lane = 0; lane < 24; lane += 8) {
				int c1 = (col >> lane) & 0xff, c0 = (*d >> lane) & 0xff;

				out |= (uint32_t)((c0 * (255 - a) + c1 * a) / 255) << lane;
			}
			*d = (*d & 0xff000000u) | out;
		}
	}
}

/* the pen moves by the glyph's advance (and the pair's kerning) in fractions of a pixel */
static float ttf_step(const char *p, int scale)
{
	float adv = ttf_glyph((unsigned char)*p, scale)->adv;

	if (p[1]) {
		float sf = stbtt_ScaleForMappingEmToPixels(&ttf, 10.0f * (float)scale);

		adv += (float)stbtt_GetCodepointKernAdvance(&ttf, (unsigned char)*p, (unsigned char)p[1]) * sf;
	}
	return adv;
}

static int ttf_text_w(const char *str, int scale)
{
	float w = 0;

	for (; *str; str++)
		w += ttf_step(str, scale);
	return (int)(w + 0.5f);
}

/* y is the top of a capital letter, like the pixel font's row 0 */
static void ttf_text(struct x45_surface *s, int x, int y, const char *str, int scale, uint32_t col, int max_chars)
{
	float pen = 0;
	int baseline = y + 7 * scale;

	for (; *str && max_chars != 0; str++, max_chars--) {
		struct ttf_glyph *gl = ttf_glyph((unsigned char)*str, scale);

		if (gl->a)
			blend_mask(s, x + (int)(pen + 0.5f) + gl->xoff, baseline + gl->yoff, gl->a, gl->w, gl->h, col);
		pen += ttf_step(str, scale);
	}
}

/* Smooth glyphs: the 5x7 bitmap is enlarged by bilinear interpolation and the result is cut
 * with a soft threshold, which rounds corners and diagonals and leaves antialiased edges. The
 * coverage masks are built once per (character, scale) and kept. */
#define SM_MAXSCALE 8
static unsigned char *sm_cache[128][SM_MAXSCALE + 1];

static float sm_bit(const unsigned char *g, int cx, int cy)
{
	if (cx < 0 || cx > 4 || cy < 0 || cy > 6)
		return 0.0f;
	return (g[cx] >> cy) & 1 ? 1.0f : 0.0f;
}

static const unsigned char *sm_mask(int c, int scale)
{
	unsigned char **slot;
	const unsigned char *g;
	int w = 5 * scale, h = 7 * scale, px, py;

	if (c < 0 || c >= 128 || scale < 1 || scale > SM_MAXSCALE)
		return NULL;
	slot = &sm_cache[c][scale];
	if (*slot)
		return *slot;
	g = x45_glyph(c);
	*slot = malloc((size_t)(w * h));
	if (!*slot)
		return NULL;
	for (py = 0; py < h; py++) {
		for (px = 0; px < w; px++) {
			float u = ((float)px + 0.5f) / (float)scale - 0.5f;
			float v = ((float)py + 0.5f) / (float)scale - 0.5f;
			int x0 = (int)(u < 0 ? u - 1 : u), y0 = (int)(v < 0 ? v - 1 : v);
			float fx = u - (float)x0, fy = v - (float)y0, val, t;

			val = sm_bit(g, x0, y0) * (1 - fx) * (1 - fy) + sm_bit(g, x0 + 1, y0) * fx * (1 - fy) +
			      sm_bit(g, x0, y0 + 1) * (1 - fx) * fy + sm_bit(g, x0 + 1, y0 + 1) * fx * fy;
			t = (val - 0.30f) / 0.40f;
			t = t < 0 ? 0 : t > 1 ? 1 : t;
			(*slot)[py * w + px] = (unsigned char)(t * t * (3 - 2 * t) * 255.0f + 0.5f);
		}
	}
	return *slot;
}

static void draw_glyph_smooth(struct x45_surface *s, int x, int y, int c, int scale, uint32_t col)
{
	const unsigned char *m = sm_mask(c, scale);
	int w = 5 * scale, h = 7 * scale, px, py;

	if (!m)
		return;
	for (py = 0; py < h; py++) {
		int yy = y + py;

		if (yy < s->clip_y || yy >= s->clip_y + s->clip_h)
			continue;
		for (px = 0; px < w; px++) {
			int a = m[py * w + px], xx = x + px, lane;
			uint32_t *d, out = 0;

			if (!a || xx < s->clip_x || xx >= s->clip_x + s->clip_w)
				continue;
			d = &s->pixels[(size_t)yy * s->stride + xx];
			if (a >= 250) {
				*d = col;
				continue;
			}
			for (lane = 0; lane < 24; lane += 8) {
				int c1 = (col >> lane) & 0xff, c0 = (*d >> lane) & 0xff;

				out |= (uint32_t)((c0 * (255 - a) + c1 * a) / 255) << lane;
			}
			*d = (*d & 0xff000000u) | out;
		}
	}
}

static void draw_glyph(struct x45_surface *s, int x, int y, int c,
		       int scale, uint32_t col)
{
	const unsigned char *g = x45_glyph(c);
	int cx, cy;

	if (x45_font_smooth && scale >= 2 && scale <= SM_MAXSCALE) {
		draw_glyph_smooth(s, x, y, c, scale, col);
		return;
	}

	for (cx = 0; cx < 5; cx++) {
		if (!g[cx])
			continue;
		for (cy = 0; cy < 7; cy++) {
			if (!(g[cx] & (1 << cy)))
				continue;
			if (scale == 1)
				x45_put(s, x + cx, y + cy, col);
			else
				x45_fill_rect(s, x + cx * scale, y + cy * scale,
					      scale, scale, col);
		}
	}
}

void x45_text(struct x45_surface *s, int x, int y, const char *str,
	      int scale, uint32_t c)
{
	if (!str || scale < 1)
		return;
	if (USE_TTF() && scale <= 9) {
		ttf_text(s, x, y, str, scale, c, -1);
		return;
	}
	for (; *str; str++, x += 6 * scale) {
		if (*str == ' ')
			continue;
		draw_glyph(s, x, y, (unsigned char)*str, scale, c);
	}
}

void x45_text_clipped(struct x45_surface *s, int x, int y, const char *str,
		      int scale, int max_w, uint32_t c)
{
	int adv = 6 * scale;
	int used = 0;

	if (!str || scale < 1 || max_w <= 0)
		return;
	if (x45_text_w(str, scale) <= max_w) {
		x45_text(s, x, y, str, scale, c);
		return;
	}
	if (USE_TTF() && scale <= 9) {
		/* proportional: take characters while they fit with ".." after them */
		int dots = ttf_text_w("..", scale), n = 0, wd = 0;
		float pen = 0;

		/* one pass: the pen advances glyph by glyph (re-measuring every prefix made this quadratic,
		 * which a 300-character title paid for on every redraw) */
		while (str[n]) {
			float next = pen + ttf_step(str + n, scale);

			if ((int)(next + 0.5f) + dots > max_w)
				break;
			pen = next;
			wd = (int)(pen + 0.5f);
			n++;
		}
		ttf_text(s, x, y, str, scale, c, n);
		ttf_text(s, x + wd, y, "..", scale, c, -1);
		return;
	}
	/* leave room for ".." */
	for (; *str; str++) {
		if (used + adv + 2 * adv > max_w)
			break;
		if (*str != ' ')
			draw_glyph(s, x + used, y, (unsigned char)*str, scale, c);
		used += adv;
	}
	draw_glyph(s, x + used, y, '.', scale, c);
	draw_glyph(s, x + used + adv, y, '.', scale, c);
}
