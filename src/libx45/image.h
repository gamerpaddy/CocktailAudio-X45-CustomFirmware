#ifndef X45_IMAGE_H
#define X45_IMAGE_H

#include <stdint.h>

#include "gfx.h"

/* A decoded picture, already packed for this framebuffer's channel order. */
struct x45_image {
	uint32_t *px;
	int w, h;
};

/* Loads a JPEG, PNG, GIF or BMP and scales it to fit maxw x maxh (never more than doubled). Returns 0, or -1. */
int  x45_image_load(const struct x45_gfx *g, const char *path, int maxw, int maxh,
		    struct x45_image *im);
/* The same, with the see-through parts of a PNG / GIF laid over the colour bg_rgb (0xRRGGBB); -1 ignores the
 * alpha channel, as x45_image_load does. */
int  x45_image_load_bg(const struct x45_gfx *g, const char *path, int maxw, int maxh, int bg_rgb,
		       struct x45_image *im);
void x45_image_free(struct x45_image *im);
void x45_image_draw(struct x45_surface *s, const struct x45_image *im, int x, int y);

/* A small glyph used as a mask: only its coverage matters, the UI tints it with a theme colour.
 * PNG (or BMP): the alpha channel when the file has one, else the picture's brightness. */
struct x45_icon {
	uint8_t *a;
	int w, h;
};

int  x45_icon_load(const char *path, struct x45_icon *ic);
void x45_icon_free(struct x45_icon *ic);
void x45_icon_draw(struct x45_surface *s, const struct x45_icon *ic, int x, int y, uint32_t col);
/* The same, enlarged (or shrunk) to a square of `size` pixels with smooth edges. */
void x45_icon_draw_scaled(struct x45_surface *s, const struct x45_icon *ic, int x, int y, int size, uint32_t col);

#endif
