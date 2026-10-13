/* libx45/gfx  -  software rendering onto the X45 front panel.
 *
 * The panel is /dev/fb0: 1024x600, 32 bpp XRGB (R<<16 G<<8 B), line_length
 * exactly 1024*4 so there is no stride padding, smem_len exactly one frame so
 * it is single buffered. Measured on the device: a full-frame memset runs at
 * ~727 fps and a naive per-pixel uint32 loop at ~489 fps, so there is no reason
 * to involve the Vivante GPU - which is what keeps this code free of the
 * galcore-locked EGL blobs x20 depends on.
 *
 * Because the hardware frame is single buffered, drawing straight into it
 * tears. Everything here renders into an off-screen surface and x45_gfx_present()
 * copies it over in one memcpy.
 */
#ifndef X45_GFX_H
#define X45_GFX_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A drawable. `pixels` is w*h uint32, row stride `stride` in pixels. */
struct x45_surface {
	uint32_t *pixels;
	int w, h;
	int stride;
	/* clip rectangle; all drawing is confined to it */
	int clip_x, clip_y, clip_w, clip_h;
};

struct x45_gfx {
	int fd;
	uint32_t *fb;          /* the mapped hardware frame */
	size_t fb_len;
	struct x45_surface back; /* what you draw into */
	int shift_r, shift_g, shift_b;
	uint32_t *shown;        /* what the panel has been sent: present only copies rows that differ */
	long shown_t;           /* when the whole frame was last sent (the panel is refreshed in full now and then) */
};

/* Opens /dev/fb0, maps it, allocates a matching back buffer.
 * Returns 0, or -errno. */
int  x45_gfx_open(struct x45_gfx *g);
void x45_gfx_close(struct x45_gfx *g);

/* Copies the back buffer to the panel. One memcpy when the geometry matches. */
void x45_gfx_present(struct x45_gfx *g);
/* Same, for the rows y0 .. y1-1 only: the caller knows nothing else changed. */
void x45_gfx_present_rows(struct x45_gfx *g, int y0, int y1);

/* Saves / restores the hardware frame, so a UI can hand the panel back to x20
 * exactly as it found it. Returns 0 or -errno. */
int  x45_gfx_save(struct x45_gfx *g, const char *path);
int  x45_gfx_restore(struct x45_gfx *g, const char *path);

/* Pack a colour for this framebuffer's channel order. */
uint32_t x45_rgb(const struct x45_gfx *g, uint8_t r, uint8_t gg, uint8_t b);

/* --- drawing. All clipped to the surface's clip rect. --- */
void x45_clip_reset(struct x45_surface *s);
void x45_clip_set(struct x45_surface *s, int x, int y, int w, int h);

void x45_clear(struct x45_surface *s, uint32_t c);
void x45_fill_rect(struct x45_surface *s, int x, int y, int w, int h, uint32_t c);
void x45_draw_rect(struct x45_surface *s, int x, int y, int w, int h, uint32_t c);
void x45_hline(struct x45_surface *s, int x, int y, int w, uint32_t c);
void x45_vline(struct x45_surface *s, int x, int y, int h, uint32_t c);
void x45_put(struct x45_surface *s, int x, int y, uint32_t c);

/* Horizontal progress/level bar: `frac` is 0..1 of the inner width. */
void x45_bar(struct x45_surface *s, int x, int y, int w, int h,
	     double frac, uint32_t fg, uint32_t bg, uint32_t border);

/* --- text, using the built-in 5x7 font at integer `scale` --- */
/* 1: glyphs at scale >= 2 are drawn with rounded, antialiased edges instead of square blocks */
extern int x45_font_smooth;   /* 0 pixels, 1 smoothed pixels, 2 a TrueType font (when x45_ttf_load worked) */

/* Loads a TrueType font for font mode 2. Sizes follow the pixel font: scale n gives a capital
 * height of about 7*n pixels. Returns 0, or -1 (the pixel font stays in use). */
int  x45_ttf_load(const char *path);
int  x45_ttf_ready(void);
void x45_text(struct x45_surface *s, int x, int y, const char *str,
	      int scale, uint32_t c);
/* Same, but stops at `max_w` pixels and ends with ".." when it has to cut. */
void x45_text_clipped(struct x45_surface *s, int x, int y, const char *str,
		      int scale, int max_w, uint32_t c);
int  x45_text_w(const char *str, int scale);  /* advance width in pixels */
int  x45_text_h(int scale);                   /* 7*scale */

/* The font draws ASCII 0x20-0x7E only. Tags are UTF-8, so run text through this
 * before drawing: Latin accents fold to their base letters, typographic quotes
 * and dashes become ASCII, control characters become spaces, and everything
 * else (CJK, emoji, malformed bytes) becomes one '?' per code point. Writes at
 * most cap-1 bytes plus a NUL and returns the number written; cap 0 writes
 * nothing. Never overruns, whatever the input. */
size_t x45_ascii_fold(char *dst, size_t cap, const char *src);

/* font internals, exposed for the renderer and the self-test */
const unsigned char *x45_glyph(int c);
int x45_font_first(void);
int x45_font_last(void);
int x45_font_selfcheck(void); /* 0 = clean; prints problems to stderr */

#ifdef __cplusplus
}
#endif
#endif /* X45_GFX_H */
