/* x45fb  -  probe and exercise the X45 front panel framebuffer.
 *
 * Purpose: settle whether an x20 replacement can own the display WITHOUT the
 * Vivante GPU stack. x20 renders through Qt5 on EGL against the proprietary
 * libEGL-fb / libGAL-fb / libGLESv2-fb blobs, which are version-locked to the
 * in-kernel `galcore` driver. Depending on those would tie a replacement to
 * the vendor kernel forever.
 *
 * The panel is a plain 1024x600 32bpp linear framebuffer with no stride
 * padding, so software rendering is viable: one frame is 2.4 MB, which a
 * Cortex-A9 at ~1 GHz can fill far faster than any UI needs.
 *
 *   x45fb info              print the fb geometry and pixel layout
 *   x45fb save <file>       dump the current frame
 *   x45fb restore <file>    write a frame back
 *   x45fb test [seconds]    save, draw a test pattern, restore
 *   x45fb bench             measure full-frame fill throughput
 *
 * `test` saves and restores by itself, so x20's screen comes back. SIGSTOP x20
 * first if you want it to stay away: it only repaints on change, so it hands
 * the framebuffer over cleanly and SIGCONT restores it with no service
 * restart.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define FBDEV "/dev/fb0"

struct fb {
	int fd;
	struct fb_var_screeninfo var;
	struct fb_fix_screeninfo fix;
	uint8_t *mem;
	size_t len;
	uint32_t stride; /* bytes per row */
};

static int fb_open(struct fb *f, int for_write)
{
	memset(f, 0, sizeof *f);
	f->fd = open(FBDEV, for_write ? O_RDWR : O_RDONLY);
	if (f->fd < 0) {
		fprintf(stderr, "x45fb: open %s: %s\n", FBDEV, strerror(errno));
		return -1;
	}
	if (ioctl(f->fd, FBIOGET_VSCREENINFO, &f->var) < 0 ||
	    ioctl(f->fd, FBIOGET_FSCREENINFO, &f->fix) < 0) {
		fprintf(stderr, "x45fb: FBIOGET_*SCREENINFO: %s\n", strerror(errno));
		close(f->fd);
		return -1;
	}
	f->stride = f->fix.line_length;
	f->len = (size_t)f->stride * f->var.yres;
	f->mem = mmap(NULL, f->len, for_write ? (PROT_READ | PROT_WRITE) : PROT_READ,
		      MAP_SHARED, f->fd, 0);
	if (f->mem == MAP_FAILED) {
		fprintf(stderr, "x45fb: mmap %zu bytes: %s\n", f->len, strerror(errno));
		close(f->fd);
		return -1;
	}
	return 0;
}

static void fb_close(struct fb *f)
{
	if (f->mem && f->mem != MAP_FAILED)
		munmap(f->mem, f->len);
	if (f->fd >= 0)
		close(f->fd);
}

static void fb_info(struct fb *f)
{
	printf("device        %s  (%s)\n", FBDEV, f->fix.id);
	printf("resolution    %ux%u  virtual %ux%u  offset %u,%u\n",
	       f->var.xres, f->var.yres, f->var.xres_virtual, f->var.yres_virtual,
	       f->var.xoffset, f->var.yoffset);
	printf("bpp           %u\n", f->var.bits_per_pixel);
	printf("layout        R off=%u len=%u  G off=%u len=%u  B off=%u len=%u  A off=%u len=%u\n",
	       f->var.red.offset, f->var.red.length,
	       f->var.green.offset, f->var.green.length,
	       f->var.blue.offset, f->var.blue.length,
	       f->var.transp.offset, f->var.transp.length);
	printf("line_length   %u bytes  (%u = packed, no padding)\n",
	       f->fix.line_length, f->var.xres * (f->var.bits_per_pixel / 8));
	printf("smem_len      %u bytes  (%.2f frames)\n", f->fix.smem_len,
	       (double)f->fix.smem_len / ((double)f->stride * f->var.yres));
	printf("smem_start    0x%08lx\n", (unsigned long)f->fix.smem_start);
	printf("type/visual   %u / %u\n", f->fix.type, f->fix.visual);
	printf("mapped        %zu bytes\n", f->len);
	printf("pixclock      %u  left/right/upper/lower %u/%u/%u/%u  hsync/vsync %u/%u\n",
	       f->var.pixclock, f->var.left_margin, f->var.right_margin,
	       f->var.upper_margin, f->var.lower_margin,
	       f->var.hsync_len, f->var.vsync_len);
}

static inline uint32_t pack(struct fb *f, uint8_t r, uint8_t g, uint8_t b)
{
	return ((uint32_t)r << f->var.red.offset) |
	       ((uint32_t)g << f->var.green.offset) |
	       ((uint32_t)b << f->var.blue.offset);
}

static double now_s(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int save_to(struct fb *f, const char *path)
{
	FILE *o = fopen(path, "wb");
	if (!o) {
		fprintf(stderr, "x45fb: %s: %s\n", path, strerror(errno));
		return -1;
	}
	if (fwrite(f->mem, 1, f->len, o) != f->len) {
		fprintf(stderr, "x45fb: short write to %s\n", path);
		fclose(o);
		return -1;
	}
	fclose(o);
	return 0;
}

static int restore_from(struct fb *f, const char *path)
{
	FILE *i = fopen(path, "rb");
	size_t got;
	if (!i) {
		fprintf(stderr, "x45fb: %s: %s\n", path, strerror(errno));
		return -1;
	}
	got = fread(f->mem, 1, f->len, i);
	fclose(i);
	if (got != f->len) {
		fprintf(stderr, "x45fb: %s held %zu bytes, framebuffer is %zu\n",
			path, got, f->len);
		return -1;
	}
	return 0;
}

/* A 5x7 bitmap font, enough to prove text rendering works. */
static const uint8_t font5x7[][5] = {
	{0x7E,0x11,0x11,0x11,0x7E}, /* A */
	{0x7F,0x49,0x49,0x49,0x36}, /* B */
	{0x3E,0x41,0x41,0x41,0x22}, /* C */
	{0x7F,0x41,0x41,0x22,0x1C}, /* D */
	{0x7F,0x49,0x49,0x49,0x41}, /* E */
	{0x7F,0x09,0x09,0x09,0x01}, /* F */
	{0x3E,0x41,0x49,0x49,0x7A}, /* G */
	{0x7F,0x08,0x08,0x08,0x7F}, /* H */
	{0x00,0x41,0x7F,0x41,0x00}, /* I */
	{0x20,0x40,0x41,0x3F,0x01}, /* J */
	{0x7F,0x08,0x14,0x22,0x41}, /* K */
	{0x7F,0x40,0x40,0x40,0x40}, /* L */
	{0x7F,0x02,0x0C,0x02,0x7F}, /* M */
	{0x7F,0x04,0x08,0x10,0x7F}, /* N */
	{0x3E,0x41,0x41,0x41,0x3E}, /* O */
	{0x7F,0x09,0x09,0x09,0x06}, /* P */
	{0x3E,0x41,0x51,0x21,0x5E}, /* Q */
	{0x7F,0x09,0x19,0x29,0x46}, /* R */
	{0x46,0x49,0x49,0x49,0x31}, /* S */
	{0x01,0x01,0x7F,0x01,0x01}, /* T */
	{0x3F,0x40,0x40,0x40,0x3F}, /* U */
	{0x1F,0x20,0x40,0x20,0x1F}, /* V */
	{0x7F,0x20,0x18,0x20,0x7F}, /* W */
	{0x63,0x14,0x08,0x14,0x63}, /* X */
	{0x03,0x04,0x78,0x04,0x03}, /* Y */
	{0x61,0x51,0x49,0x45,0x43}, /* Z */
};
static const uint8_t digits5x7[][5] = {
	{0x3E,0x51,0x49,0x45,0x3E}, /* 0 */
	{0x00,0x42,0x7F,0x40,0x00}, /* 1 */
	{0x42,0x61,0x51,0x49,0x46}, /* 2 */
	{0x21,0x41,0x45,0x4B,0x31}, /* 3 */
	{0x18,0x14,0x12,0x7F,0x10}, /* 4 */
	{0x27,0x45,0x45,0x45,0x39}, /* 5 */
	{0x3C,0x4A,0x49,0x49,0x30}, /* 6 */
	{0x01,0x71,0x09,0x05,0x03}, /* 7 */
	{0x36,0x49,0x49,0x49,0x36}, /* 8 */
	{0x06,0x49,0x49,0x29,0x1E}, /* 9 */
};

static void draw_char(struct fb *f, int x, int y, char c, int scale, uint32_t col)
{
	const uint8_t *glyph = NULL;
	int cx, cy, sx, sy;

	if (c >= 'A' && c <= 'Z')
		glyph = font5x7[c - 'A'];
	else if (c >= 'a' && c <= 'z')
		glyph = font5x7[c - 'a'];
	else if (c >= '0' && c <= '9')
		glyph = digits5x7[c - '0'];
	else
		return; /* space and punctuation: just advance */

	for (cx = 0; cx < 5; cx++)
		for (cy = 0; cy < 7; cy++)
			if (glyph[cx] & (1 << cy))
				for (sx = 0; sx < scale; sx++)
					for (sy = 0; sy < scale; sy++) {
						int px = x + cx * scale + sx;
						int py = y + cy * scale + sy;
						if (px < 0 || py < 0 ||
						    px >= (int)f->var.xres ||
						    py >= (int)f->var.yres)
							continue;
						*(uint32_t *)(f->mem + (size_t)py * f->stride +
							      (size_t)px * 4) = col;
					}
}

static void draw_text(struct fb *f, int x, int y, const char *s, int scale, uint32_t col)
{
	for (; *s; s++, x += 6 * scale)
		draw_char(f, x, y, *s, scale, col);
}

static void draw_pattern(struct fb *f)
{
	uint32_t y, x;

	/* vertical colour ramps across the top half: proves the channel offsets */
	for (y = 0; y < f->var.yres / 2; y++) {
		uint32_t *row = (uint32_t *)(f->mem + (size_t)y * f->stride);
		for (x = 0; x < f->var.xres; x++) {
			uint8_t v = (uint8_t)(x * 255 / (f->var.xres - 1));
			uint32_t band = y / (f->var.yres / 8 ? f->var.yres / 8 : 1);
			switch (band) {
			case 0: row[x] = pack(f, v, 0, 0); break;
			case 1: row[x] = pack(f, 0, v, 0); break;
			case 2: row[x] = pack(f, 0, 0, v); break;
			default: row[x] = pack(f, v, v, v); break;
			}
		}
	}
	/* bottom half: dark field, a 1 px white border, and labels */
	for (y = f->var.yres / 2; y < f->var.yres; y++) {
		uint32_t *row = (uint32_t *)(f->mem + (size_t)y * f->stride);
		for (x = 0; x < f->var.xres; x++)
			row[x] = pack(f, 16, 16, 24);
	}
	for (x = 0; x < f->var.xres; x++) {
		*(uint32_t *)(f->mem + (size_t)(f->var.yres / 2) * f->stride + (size_t)x * 4) =
			pack(f, 255, 255, 255);
		*(uint32_t *)(f->mem + (size_t)(f->var.yres - 1) * f->stride + (size_t)x * 4) =
			pack(f, 255, 255, 255);
	}
	draw_text(f, 40, f->var.yres / 2 + 60,  "X45 SOFTWARE RENDER", 5, pack(f, 255, 255, 255));
	draw_text(f, 40, f->var.yres / 2 + 130, "NO GPU NO EGL NO QT", 3, pack(f, 120, 220, 120));
	draw_text(f, 40, f->var.yres / 2 + 185, "1024 X 600 32BPP", 3, pack(f, 160, 160, 200));
}

static void bench(struct fb *f)
{
	int i, n = 60;
	double t0, t1;
	size_t bytes;

	printf("filling %ux%u (%zu bytes) %d times...\n",
	       f->var.xres, f->var.yres, f->len, n);
	t0 = now_s();
	for (i = 0; i < n; i++)
		memset(f->mem, i & 0xFF, f->len);
	t1 = now_s();
	bytes = f->len * (size_t)n;
	printf("memset  : %.1f fps   %.0f MB/s\n",
	       n / (t1 - t0), bytes / (t1 - t0) / 1e6);

	t0 = now_s();
	for (i = 0; i < n; i++) {
		uint32_t *p = (uint32_t *)f->mem;
		size_t j, px = f->len / 4;
		uint32_t c = pack(f, (uint8_t)i, 0, (uint8_t)(255 - i));
		for (j = 0; j < px; j++)
			p[j] = c;
	}
	t1 = now_s();
	printf("per-px  : %.1f fps   %.0f MB/s\n",
	       n / (t1 - t0), bytes / (t1 - t0) / 1e6);
	printf("\nA music-player UI repaints a few hundred KB per frame, not 2.4 MB.\n");
}

static void usage(void)
{
	puts("usage: x45fb info | save <file> | restore <file> | test [seconds] | bench");
}

int main(int argc, char **argv)
{
	struct fb f;
	const char *cmd = argc > 1 ? argv[1] : "info";

	if (!strcmp(cmd, "info")) {
		if (fb_open(&f, 0) < 0)
			return 1;
		fb_info(&f);
		fb_close(&f);
		return 0;
	}
	if (!strcmp(cmd, "save")) {
		int r;
		if (argc < 3) { usage(); return 2; }
		if (fb_open(&f, 0) < 0) return 1;
		r = save_to(&f, argv[2]);
		if (r == 0) printf("saved %zu bytes to %s\n", f.len, argv[2]);
		fb_close(&f);
		return r ? 1 : 0;
	}
	if (!strcmp(cmd, "restore")) {
		int r;
		if (argc < 3) { usage(); return 2; }
		if (fb_open(&f, 1) < 0) return 1;
		r = restore_from(&f, argv[2]);
		if (r == 0) printf("restored %zu bytes from %s\n", f.len, argv[2]);
		fb_close(&f);
		return r ? 1 : 0;
	}
	if (!strcmp(cmd, "test")) {
		int secs = argc > 2 ? atoi(argv[2]) : 4;
		const char *tmp = "/tmp/x45fb-saved.raw";
		double t0, t1;

		if (fb_open(&f, 1) < 0) return 1;
		if (save_to(&f, tmp) < 0) { fb_close(&f); return 1; }
		printf("saved current frame (%zu bytes) to %s\n", f.len, tmp);

		t0 = now_s();
		draw_pattern(&f);
		t1 = now_s();
		printf("drew test pattern in %.1f ms\n", (t1 - t0) * 1000.0);
		printf("holding %d s, then restoring...\n", secs);
		fflush(stdout);
		sleep(secs);

		if (restore_from(&f, tmp) < 0) { fb_close(&f); return 1; }
		puts("restored");
		fb_close(&f);
		unlink(tmp);
		return 0;
	}
	if (!strcmp(cmd, "bench")) {
		const char *tmp = "/tmp/x45fb-saved.raw";
		if (fb_open(&f, 1) < 0) return 1;
		if (save_to(&f, tmp) < 0) { fb_close(&f); return 1; }
		bench(&f);
		restore_from(&f, tmp);
		fb_close(&f);
		unlink(tmp);
		return 0;
	}
	usage();
	return 2;
}
