/* pinsnoop - watch the digital audio port that feeds the i.MX6 SSI (AUDMUX port 3: CS8422 serial output).
 *
 * The SSI capture (hw:1,0) is how the meters see the signal, and when it delivers nothing there is no way to
 * tell "no clocks" from "no data". This looks at the pads themselves: it forces the input path of the four
 * AUD3 pads to the GPIO block (the SION bit, which leaves the AUDMUX function alone), samples the pad levels
 * as fast as the bus allows, and reports per pad how often it toggled and what frequency that makes.
 *
 *   pad              pin          GPIO
 *   BCLK  (TXC)      CSI0_DAT4    GPIO5_IO22
 *   TXD              CSI0_DAT5    GPIO5_IO23   (to the codec; unused here)
 *   LRCK  (TXFS)     CSI0_DAT6    GPIO5_IO24
 *   RXD              CSI0_DAT7    GPIO5_IO25   (audio data from the CS8422)
 *
 * The LRCK frequency is the sample rate of whatever the CS8422 sends; RXD toggling means audio data
 * (silence is a constant line). The mux registers are put back afterwards. Run as root, takes ~1 s.
 *
 *   pinsnoop [samples]     default 4,000,000
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define IOMUXC  0x020E0000u
#define GPIO5   0x020AC000u
#define PSR     2

static const struct { const char *name; unsigned mux; int bit; } pad[] = {
	{ "BCLK", 0x268, 22 }, { "TXD ", 0x26C, 23 }, { "LRCK", 0x270, 24 }, { "RXD ", 0x274, 25 },
};

int main(int argc, char **argv)
{
	int fd = open("/dev/mem", O_RDWR | O_SYNC), i;
	long n = argc > 1 ? atol(argv[1]) : 4000000, k;
	volatile uint32_t *mux, *gpio;
	uint32_t orig[4], *buf;
	struct timespec t0, t1;
	double secs;
	long edges[4] = { 0 }, ones[4] = { 0 };

	if (fd < 0) {
		perror("/dev/mem");
		return 2;
	}
	if (n < 1000) {
		fprintf(stderr, "pinsnoop: at least 1000 samples are needed\n");
		return 2;
	}
	mux = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, IOMUXC);
	gpio = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, GPIO5);
	buf = malloc(n * sizeof *buf);
	if (mux == MAP_FAILED || gpio == MAP_FAILED || !buf) {
		perror("mmap");
		return 2;
	}
	for (i = 0; i < 4; i++) {
		orig[i] = mux[pad[i].mux / 4];
		if ((orig[i] & 7) != 4) {
			fprintf(stderr, "%s is not muxed to the AUDMUX (0x%x): not touching it\n", pad[i].name, orig[i]);
			return 3;
		}
	}
	for (i = 0; i < 4; i++)
		mux[pad[i].mux / 4] = orig[i] | 0x10;       /* SION: the pad is also readable through the GPIO block */
	usleep(100);
	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (k = 0; k < n; k++)
		buf[k] = gpio[PSR];
	clock_gettime(CLOCK_MONOTONIC, &t1);
	for (i = 0; i < 4; i++)
		mux[pad[i].mux / 4] = orig[i];
	secs = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) * 1e-9;
	for (k = 1; k < n; k++)
		for (i = 0; i < 4; i++) {
			uint32_t m = 1u << pad[i].bit;

			edges[i] += !!((buf[k] ^ buf[k - 1]) & m);
			ones[i] += !!(buf[k] & m);
		}
	printf("%ld samples in %.1f ms (%.1f Msamples/s)\n", n, secs * 1e3, n / secs / 1e6);
	for (i = 0; i < 4; i++)
		printf("%s  edges %-8ld  high %5.1f%%  ~%.0f Hz\n", pad[i].name, edges[i], 100.0 * ones[i] / n,
		       edges[i] / 2.0 / secs);
	return 0;
}
