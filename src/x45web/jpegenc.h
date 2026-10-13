/* A small baseline JPEG encoder: 4:4:4, one quantisation table, fixed-length Huffman codes (every AC symbol 8 bits,
 * every DC category 4 bits; valid JPEG, a little bigger than the optimised tables, nothing to look up). Written for
 * the panel's screen stream: flat colours and text, a few frames a second. */
#ifndef X45_JPEGENC_H
#define X45_JPEGENC_H

#include <math.h>
#include <stdint.h>
#include <string.h>

struct jpg_out {
	uint8_t *p;
	size_t n, cap;
	int over;
	uint32_t acc;
	int nacc;
};

static void jpg_byte(struct jpg_out *o, int b)
{
	if (o->n < o->cap)
		o->p[o->n++] = (uint8_t)b;
	else
		o->over = 1;
}

static void jpg_bits(struct jpg_out *o, unsigned code, int len)
{
	o->acc = (o->acc << len) | (code & ((1u << len) - 1));
	o->nacc += len;
	while (o->nacc >= 8) {
		int b = (int)((o->acc >> (o->nacc - 8)) & 0xff);

		jpg_byte(o, b);
		if (b == 0xff)
			jpg_byte(o, 0);
		o->nacc -= 8;
	}
}

static void jpg_w16(struct jpg_out *o, int v)
{
	jpg_byte(o, v >> 8);
	jpg_byte(o, v & 0xff);
}

static const uint8_t jpg_zz[64] = {
	0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
	35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63
};
static const uint8_t jpg_q0[64] = {
	16, 11, 10, 16, 24, 40, 51, 61, 12, 12, 14, 19, 26, 58, 60, 55, 14, 13, 16, 24, 40, 57, 69, 56, 14, 17, 22, 29, 51, 87, 80, 62,
	18, 22, 37, 56, 68, 109, 103, 77, 24, 35, 55, 64, 81, 104, 113, 92, 49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99
};

/* the AC symbols in table order: EOB, ZRL, then run 0..15 x size 1..10 */
static void jpg_ac_syms(uint8_t *syms, int *idx)
{
	int n = 0, r, s;

	memset(idx, -1, 256 * sizeof *idx);
	syms[n] = 0x00; idx[0x00] = n++;
	syms[n] = 0xf0; idx[0xf0] = n++;
	for (r = 0; r < 16; r++)
		for (s = 1; s <= 10; s++) {
			syms[n] = (uint8_t)(r << 4 | s);
			idx[r << 4 | s] = n++;
		}
}

static int jpg_cat(int v)
{
	int a = v < 0 ? -v : v, c = 0;

	while (a) {
		c++;
		a >>= 1;
	}
	return c;
}

/* px: BGRA words (blue in the low byte), stride in pixels. Returns the size, or 0 if out is too small. */
static size_t jpeg_encode_bgra(const uint32_t *px, int w, int h, int stride, int quality, uint8_t *out, size_t cap)
{
	struct jpg_out o = { out, 0, cap, 0, 0, 0 };
	uint8_t q[64], syms[162];
	int acidx[256], i, x, y, c, pred[3] = { 0, 0, 0 }, scale;
	float cs[8][8];

	if (quality < 1)
		quality = 1;
	if (quality > 100)
		quality = 100;
	scale = quality < 50 ? 5000 / quality : 200 - quality * 2;
	for (i = 0; i < 64; i++) {
		int v = (jpg_q0[i] * scale + 50) / 100;

		q[i] = (uint8_t)(v < 1 ? 1 : v > 255 ? 255 : v);
	}
	for (x = 0; x < 8; x++)
		for (y = 0; y < 8; y++)
			cs[x][y] = (float)(cos((2 * y + 1) * x * 3.14159265358979 / 16.0) * (x ? 0.5 : 0.35355339059327373));
	jpg_ac_syms(syms, acidx);

	jpg_w16(&o, 0xffd8);
	jpg_w16(&o, 0xffe0); jpg_w16(&o, 16);
	jpg_byte(&o, 'J'); jpg_byte(&o, 'F'); jpg_byte(&o, 'I'); jpg_byte(&o, 'F'); jpg_byte(&o, 0);
	jpg_w16(&o, 0x0101); jpg_byte(&o, 0); jpg_w16(&o, 1); jpg_w16(&o, 1); jpg_byte(&o, 0); jpg_byte(&o, 0);
	jpg_w16(&o, 0xffdb); jpg_w16(&o, 67); jpg_byte(&o, 0);
	for (i = 0; i < 64; i++)
		jpg_byte(&o, q[jpg_zz[i]]);
	jpg_w16(&o, 0xffc0); jpg_w16(&o, 17); jpg_byte(&o, 8); jpg_w16(&o, h); jpg_w16(&o, w); jpg_byte(&o, 3);
	for (c = 1; c <= 3; c++) {
		jpg_byte(&o, c); jpg_byte(&o, 0x11); jpg_byte(&o, 0);
	}
	/* DC table: 12 codes of 4 bits */
	jpg_w16(&o, 0xffc4); jpg_w16(&o, 2 + 1 + 16 + 12); jpg_byte(&o, 0x00);
	for (i = 1; i <= 16; i++)
		jpg_byte(&o, i == 4 ? 12 : 0);
	for (i = 0; i < 12; i++)
		jpg_byte(&o, i);
	/* AC table: 162 codes of 8 bits */
	jpg_w16(&o, 0xffc4); jpg_w16(&o, 2 + 1 + 16 + 162); jpg_byte(&o, 0x10);
	for (i = 1; i <= 16; i++)
		jpg_byte(&o, i == 8 ? 162 : 0);
	for (i = 0; i < 162; i++)
		jpg_byte(&o, syms[i]);
	jpg_w16(&o, 0xffda); jpg_w16(&o, 12); jpg_byte(&o, 3);
	for (c = 1; c <= 3; c++) {
		jpg_byte(&o, c); jpg_byte(&o, 0x00);
	}
	jpg_byte(&o, 0); jpg_byte(&o, 63); jpg_byte(&o, 0);

	for (int by = 0; by < h; by += 8) {
		for (int bx = 0; bx < w; bx += 8) {
			float comp[3][64];

			for (y = 0; y < 8; y++) {
				for (x = 0; x < 8; x++) {
					int xx = bx + x < w ? bx + x : w - 1, yy = by + y < h ? by + y : h - 1;
					uint32_t p = px[(size_t)yy * (size_t)stride + (size_t)xx];
					float b = (float)(p & 0xff), g = (float)((p >> 8) & 0xff), r = (float)((p >> 16) & 0xff);

					comp[0][y * 8 + x] = 0.299f * r + 0.587f * g + 0.114f * b - 128.0f;
					comp[1][y * 8 + x] = -0.168736f * r - 0.331264f * g + 0.5f * b;
					comp[2][y * 8 + x] = 0.5f * r - 0.418688f * g - 0.081312f * b;
				}
			}
			for (c = 0; c < 3; c++) {
				float tmp[64], dct[64];
				int zq[64], last = 0, dc, run = 0;

				/* rows then columns */
				for (y = 0; y < 8; y++)
					for (int u = 0; u < 8; u++) {
						float a = 0;

						for (x = 0; x < 8; x++)
							a += comp[c][y * 8 + x] * cs[u][x];
						tmp[y * 8 + u] = a;
					}
				for (int u = 0; u < 8; u++)
					for (int v = 0; v < 8; v++) {
						float a = 0;

						for (y = 0; y < 8; y++)
							a += tmp[y * 8 + u] * cs[v][y];
						dct[v * 8 + u] = a;
					}
				for (i = 0; i < 64; i++) {
					float f = dct[jpg_zz[i]] / (float)q[jpg_zz[i]];

					zq[i] = (int)(f < 0 ? f - 0.5f : f + 0.5f);
					if (zq[i])
						last = i;
				}
				dc = zq[0] - pred[c];
				pred[c] = zq[0];
				{
					int cat = jpg_cat(dc);

					jpg_bits(&o, (unsigned)cat, 4);
					if (cat)
						jpg_bits(&o, (unsigned)(dc < 0 ? dc - 1 : dc), cat);
				}
				for (i = 1; i <= last; i++) {
					int v = zq[i];

					if (v == 0) {
						run++;
						continue;
					}
					while (run > 15) {
						jpg_bits(&o, (unsigned)acidx[0xf0], 8);
						run -= 16;
					}
					{
						int cat = jpg_cat(v);

						jpg_bits(&o, (unsigned)acidx[run << 4 | cat], 8);
						jpg_bits(&o, (unsigned)(v < 0 ? v - 1 : v), cat);
					}
					run = 0;
				}
				if (last < 63)
					jpg_bits(&o, (unsigned)acidx[0x00], 8);
			}
		}
	}
	if (o.nacc)
		jpg_bits(&o, 0x7f, 8 - o.nacc);   /* pad with ones */
	jpg_w16(&o, 0xffd9);
	return o.over ? 0 : o.n;
}

#endif
