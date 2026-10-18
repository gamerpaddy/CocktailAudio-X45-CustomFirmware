/* JPEG / PNG loading for the UI (DAB slideshow pictures, cover art): decode with
 * stb_image, box-filter down to the requested size, pack for the panel. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#pragma GCC diagnostic ignored "-Wcast-function-type"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF   /* station logos are often GIFs (the first frame is used) */
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_MAX_DIMENSIONS 1024   /* what the panel shows is at most 300 px (x45art shrinks covers first, DAB slides are 320x240): a bigger header is refused before any pixel is allocated, so a hostile 200 KB file cannot cost 300 MB */
#include "stb_image.h"
#pragma GCC diagnostic pop

#include "image.h"

int x45_image_load(const struct x45_gfx *g, const char *path, int maxw, int maxh,
		   struct x45_image *im)
{
	return x45_image_load_bg(g, path, maxw, maxh, -1, im);
}

int x45_image_load_bg(const struct x45_gfx *g, const char *path, int maxw, int maxh, int bg_rgb,
		      struct x45_image *im)
{
	FILE *f = fopen(path, "rb");
	unsigned char *buf, *rgb;
	long len;
	int sw, sh, comp, dw, dh, x, y;

	memset(im, 0, sizeof *im);
	if (!f)
		return -1;
	fseek(f, 0, SEEK_END);
	len = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (len <= 0 || len > 4 * 1024 * 1024 || !(buf = malloc(len))) {
		fclose(f);
		return -1;
	}
	if (fread(buf, 1, len, f) != (size_t)len) {
		free(buf);
		fclose(f);
		return -1;
	}
	fclose(f);
	rgb = stbi_load_from_memory(buf, (int)len, &sw, &sh, &comp, 4);
	free(buf);
	if (!rgb)
		return -1;

	/* fit inside maxw x maxh, never enlarge more than 2x */
	dw = sw;
	dh = sh;
	if (dw * maxh > dh * maxw) {
		dh = dh * maxw / dw;
		dw = maxw;
	} else {
		dw = dw * maxh / dh;
		dh = maxh;
	}
	if (dw > sw * 2) {
		dw = sw * 2;
		dh = sh * 2;
	}
	if (dw < 1)
		dw = 1;
	if (dh < 1)
		dh = 1;
	im->px = malloc((size_t)dw * dh * sizeof *im->px);
	if (!im->px) {
		stbi_image_free(rgb);
		return -1;
	}
	im->w = dw;
	im->h = dh;
	for (y = 0; y < dh; y++) {
		int y0 = (int)((long)y * sh / dh), y1 = (int)((long)(y + 1) * sh / dh);

		if (y1 <= y0)
			y1 = y0 + 1;
		for (x = 0; x < dw; x++) {
			int x0 = (int)((long)x * sw / dw), x1 = (int)((long)(x + 1) * sw / dw);
			unsigned long r = 0, gg = 0, b = 0, n = 0;
			int xx, yy;

			if (x1 <= x0)
				x1 = x0 + 1;
			for (yy = y0; yy < y1 && yy < sh; yy++)
				for (xx = x0; xx < x1 && xx < sw; xx++) {
					const unsigned char *p = rgb + ((size_t)yy * sw + xx) * 4;

					if (bg_rgb >= 0 && p[3] != 255) {   /* a see-through pixel takes the background's share */
						int a = p[3], ba = 255 - a;

						r += (p[0] * a + ((bg_rgb >> 16) & 255) * ba) / 255;
						gg += (p[1] * a + ((bg_rgb >> 8) & 255) * ba) / 255;
						b += (p[2] * a + (bg_rgb & 255) * ba) / 255;
					} else {
						r += p[0];
						gg += p[1];
						b += p[2];
					}
					n++;
				}
			if (!n)
				n = 1;
			im->px[(size_t)y * dw + x] = x45_rgb(g, r / n, gg / n, b / n);
		}
	}
	stbi_image_free(rgb);
	return 0;
}

int x45_icon_load(const char *path, struct x45_icon *ic)
{
	FILE *f = fopen(path, "rb");
	unsigned char *buf, *px;
	long len;
	int w, h, comp, i;

	memset(ic, 0, sizeof *ic);
	if (!f)
		return -1;
	fseek(f, 0, SEEK_END);
	len = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (len <= 0 || len > 1024 * 1024 || !(buf = malloc(len))) {
		fclose(f);
		return -1;
	}
	if (fread(buf, 1, len, f) != (size_t)len) {
		free(buf);
		fclose(f);
		return -1;
	}
	fclose(f);
	px = stbi_load_from_memory(buf, (int)len, &w, &h, &comp, 4);
	free(buf);
	if (!px)
		return -1;
	if (w < 1 || h < 1 || w > 128 || h > 128) {
		stbi_image_free(px);
		return -1;
	}
	ic->a = malloc((size_t)w * h);
	if (!ic->a) {
		stbi_image_free(px);
		return -1;
	}
	ic->w = w;
	ic->h = h;
	for (i = 0; i < w * h; i++) {
		const unsigned char *p = px + (size_t)i * 4;

		/* a file without an alpha channel (a BMP): white on black, so the brightness is the coverage */
		ic->a[i] = comp == 4 || comp == 2 ? p[3] : (unsigned char)((p[0] * 3 + p[1] * 6 + p[2]) / 10);
	}
	stbi_image_free(px);
	return 0;
}

void x45_icon_free(struct x45_icon *ic)
{
	free(ic->a);
	memset(ic, 0, sizeof *ic);
}

void x45_icon_draw(struct x45_surface *s, const struct x45_icon *ic, int x, int y, uint32_t col)
{
	int yy, xx, lane;

	if (!ic || !ic->a)
		return;
	for (yy = 0; yy < ic->h; yy++) {
		int py = y + yy;

		if (py < s->clip_y || py >= s->clip_y + s->clip_h)
			continue;
		for (xx = 0; xx < ic->w; xx++) {
			int a = ic->a[(size_t)yy * ic->w + xx], px = x + xx;
			uint32_t *d, out = 0;

			if (!a || px < s->clip_x || px >= s->clip_x + s->clip_w)
				continue;
			d = &s->pixels[(size_t)py * s->stride + px];
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

void x45_icon_draw_scaled(struct x45_surface *s, const struct x45_icon *ic, int x, int y, int size, uint32_t col)
{
	int dy, dx, lane;

	if (!ic || !ic->a || size < 1)
		return;
	for (dy = 0; dy < size; dy++) {
		int py = y + dy, y0, y1;
		float fy, wy;

		if (py < s->clip_y || py >= s->clip_y + s->clip_h)
			continue;
		/* bilinear: the centre of the target pixel, mapped into the icon (a larger icon is averaged, below) */
		fy = ((float)dy + 0.5f) * (float)ic->h / (float)size - 0.5f;
		y0 = (int)floorf(fy);
		wy = fy - (float)y0;
		y1 = y0 + 1;
		if (y0 < 0)
			y0 = 0;
		if (y1 < 0)
			y1 = 0;
		if (y0 >= ic->h)
			y0 = ic->h - 1;
		if (y1 >= ic->h)
			y1 = ic->h - 1;
		for (dx = 0; dx < size; dx++) {
			int px = x + dx, x0, x1, a;
			float fx, wx, v;
			uint32_t *d, out = 0;

			if (px < s->clip_x || px >= s->clip_x + s->clip_w)
				continue;
			if (ic->w > size + size / 4 || ic->h > size + size / 4) {
				/* shrinking: every source pixel under the target pixel counts, by how much of it is under it */
				float sx0 = (float)dx * (float)ic->w / (float)size, sx1 = (float)(dx + 1) * (float)ic->w / (float)size;
				float sy0 = (float)dy * (float)ic->h / (float)size, sy1 = (float)(dy + 1) * (float)ic->h / (float)size;
				float sum = 0, wsum = 0;
				int ix, iy;

				for (iy = (int)sy0; iy < (int)ceilf(sy1) && iy < ic->h; iy++) {
					float oy = (float)(iy + 1) < sy1 ? (float)(iy + 1) : sy1;

					oy -= (float)iy > sy0 ? (float)iy : sy0;
					for (ix = (int)sx0; ix < (int)ceilf(sx1) && ix < ic->w; ix++) {
						float ox = (float)(ix + 1) < sx1 ? (float)(ix + 1) : sx1, wgt;

						ox -= (float)ix > sx0 ? (float)ix : sx0;
						wgt = ox * oy;
						sum += wgt * (float)ic->a[(size_t)iy * ic->w + ix];
						wsum += wgt;
					}
				}
				a = wsum > 0 ? (int)(sum / wsum + 0.5f) : 0;
				goto blend;
			}
			fx = ((float)dx + 0.5f) * (float)ic->w / (float)size - 0.5f;
			x0 = (int)floorf(fx);
			wx = fx - (float)x0;
			x1 = x0 + 1;
			if (x0 < 0)
				x0 = 0;
			if (x1 < 0)
				x1 = 0;
			if (x0 >= ic->w)
				x0 = ic->w - 1;
			if (x1 >= ic->w)
				x1 = ic->w - 1;
			v = (ic->a[(size_t)y0 * ic->w + x0] * (1 - wx) + ic->a[(size_t)y0 * ic->w + x1] * wx) * (1 - wy) +
			    (ic->a[(size_t)y1 * ic->w + x0] * (1 - wx) + ic->a[(size_t)y1 * ic->w + x1] * wx) * wy;
			a = (int)(v + 0.5f);
blend:
			if (a <= 0)
				continue;
			d = &s->pixels[(size_t)py * s->stride + px];
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

void x45_image_free(struct x45_image *im)
{
	free(im->px);
	memset(im, 0, sizeof *im);
}

void x45_image_draw(struct x45_surface *s, const struct x45_image *im, int x, int y)
{
	int yy, xx;

	for (yy = 0; yy < im->h; yy++)
		for (xx = 0; xx < im->w; xx++)
			x45_put(s, x + xx, y + yy, im->px[(size_t)yy * im->w + xx]);
}
