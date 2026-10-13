/* Feeds recorded `cb` frames (from the 2026-10-04 x20 trace) to the tuner's MOT
 * reassembly and checks that whole JPEG pictures come out. Native build only. */
#define main x45tuner_main
#include "../x45tuner/main.c"
#undef main

int main(int argc, char **argv)
{
	FILE *f = fopen(argc > 1 ? argv[1] : "out/cbframes.bin", "rb");
	uint8_t buf[2048];
	uint16_t len;
	int pics = 0, fails = 0, last_n = 0;

	if (!f) {
		perror("frames");
		return 1;
	}
	while (fread(&len, 2, 1, f) == 1 && len <= sizeof buf && fread(buf, 1, len, f) == len) {
		mot_frame(buf, len);
		if (T.slide_n != last_n) {
			FILE *g = fopen(SLIDE_FILE, "rb");
			uint8_t h[2], t[2];
			long sz;

			last_n = T.slide_n;
			if (!g) {
				fails++;
				continue;
			}
			fseek(g, 0, SEEK_END);
			sz = ftell(g);
			fseek(g, 0, SEEK_SET);
			fread(h, 1, 2, g);
			fseek(g, -2, SEEK_END);
			fread(t, 1, 2, g);
			fclose(g);
			printf("picture %d: %ld bytes, starts %02x%02x ends %02x%02x\n", ++pics, sz, h[0], h[1], t[0], t[1]);
			if (h[0] != 0xff || h[1] != 0xd8 || t[0] != 0xff || t[1] != 0xd9)
				fails++;
			if (pics == 1)
				rename(SLIDE_FILE, "out/slide1.jpg");
		}
	}
	printf("mot_test: %d pictures, %d failures\n", pics, fails);
	return pics > 0 && !fails ? 0 : 1;
}
