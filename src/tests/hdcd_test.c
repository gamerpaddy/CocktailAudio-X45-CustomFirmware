/* Unit test for src/x45cd/hdcd.c, the HDCD decoder.
 *
 * There is no reference decoder on the build machine, so the test builds HDCD
 * streams itself: it hides control packets in the LSBs of a synthetic signal the
 * way an encoder does (the decoder descrambles w ^ w>>5 ^ w>>23, so the encoder
 * runs that recursion forwards) and checks what the decoder makes of them.
 *
 *   gcc -std=gnu11 -Wall -Wextra -fsanitize=address,undefined \
 *       -o hdcd_test tests/hdcd_test.c x45cd/hdcd.c -lm && ./hdcd_test
 */
#include "../x45cd/hdcd.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N 44100          /* one second */

static int failures;

static void check(const char *what, long got, long want)
{
	int ok = got == want;

	if (!ok)
		failures++;
	printf("  %-58s got %-12ld want %-12ld %s\n", what, got, want, ok ? "ok" : "FAIL");
}

static void check_near(const char *what, double got, double want, double tol)
{
	int ok = fabs(got - want) <= tol;

	if (!ok)
		failures++;
	printf("  %-58s got %-12.4f want %-12.4f %s\n", what, got, want, ok ? "ok" : "FAIL");
}

/* Write a packet whose descrambled bit string is `bits` (nbits long, first bit
 * first) into the LSBs of channel `ch`, starting at frame `at`. The bits that
 * come before the packet are whatever the signal has there, the recursion uses
 * them as they are. */
static void put_bits(int32_t *x, int ch, int at, uint64_t bits, int nbits)
{
	int m;

	for (m = 0; m < nbits; m++) {
		int d = (int)(bits >> (nbits - 1 - m) & 1);
		int s5 = at + m - 5 >= 0 ? (x[(at + m - 5) * 2 + ch] & 1) : 0;
		int s23 = at + m - 23 >= 0 ? (x[(at + m - 23) * 2 + ch] & 1) : 0;

		x[(at + m) * 2 + ch] = (x[(at + m) * 2 + ch] & ~1) | (d ^ s5 ^ s23);
	}
}

/* type A packet (prefix 0x7e0fa005 then the code byte) in both channels */
static void put_packet_a(int32_t *x, int at, int code)
{
	uint64_t bits = 0x7e0fa005ULL << 8 | (unsigned)code;

	put_bits(x, 0, at, bits, 40);
	put_bits(x, 1, at, bits, 40);
}

static void fill(int32_t *x, int n, double amp, double hz)
{
	int i;

	for (i = 0; i < n; i++) {
		int v = (int)lrint(amp * 32767.0 * sin(2 * M_PI * hz * i / 44100.0));

		/* LSBs random so that no accidental packet depends on the signal */
		v = (v & ~1) | (rand() & 1);
		x[i * 2] = v;
		x[i * 2 + 1] = v;
	}
}

int main(void)
{
	static int32_t x[N * 2], ref[N * 2];
	struct hdcd h;
	struct hdcd_info in;
	int i, bad = 0;

	srand(1);

	printf("plain audio, random LSBs: no packets, exactly sample << 15\n");
	fill(x, N, 0.9, 440);
	memcpy(ref, x, sizeof x);
	hdcd_init(&h, 44100, 2000);
	for (i = 0; i < N; i += 7644)
		hdcd_decode(&h, x + i * 2, N - i < 7644 ? N - i : 7644);
	for (i = 0; i < N * 2; i++)
		if (x[i] != (int32_t)((int64_t)ref[i] * 32768))
			bad++;
	hdcd_info(&h, &in);
	check("samples that differ from sample << 15", bad, 0);
	check("detected", in.detected, HDCD_NONE);
	check("packets", in.packets, 0);

	printf("packet A, peak extend on, gain code 4 (-4.0 dB) at frame 1000\n");
	/* code byte 0x14: bit 4 peak extend, low three bits 4 -> control 0x18:
	 * peak extend, gain field 8 (x -0.5 dB each = -4.0 dB) */
	fill(x, N, 0.5, 440);
	memcpy(ref, x, sizeof x);
	put_packet_a(x, 1000, 0x14);
	/* a second packet shortly before the end so the code timer is still
	 * running when the block ends */
	put_packet_a(x, N - 3000, 0x14);
	memcpy(ref, x, sizeof x);
	hdcd_init(&h, 44100, 2000);
	for (i = 0; i < N; i += 7644)
		hdcd_decode(&h, x + i * 2, N - i < 7644 ? N - i : 7644);
	hdcd_info(&h, &in);
	check("packets found (both channels, two packets each)", in.packets, 4);
	check("detected", in.detected, HDCD_EFFECTUAL);
	check("peak extend", in.peak_extend, HDCD_PE_PERMANENT);
	check("max gain in half dB", in.max_gain, 8);
	check("errors", in.errors, 0);
	check("codes active in both channels at the end", in.active, 1);
	/* before the packet: plain << 15; far after it the gain has settled:
	 * attenuation of 4.0 dB. Samples are a 440 Hz sine at 0.5 FS, below the
	 * peak extend threshold. */
	check("sample 500 untouched", x[500 * 2], ref[500 * 2] * 32768);
	{
		/* find a sample well after the packet with a large value */
		int k = 20000;
		double want = (double)(ref[k * 2] >> 0) * 32768.0 * pow(10.0, -4.0 / 20.0);
		double got = x[k * 2];

		check_near("sample 20000 attenuated by 4.0 dB (ratio)", got / want, 1.0, 0.01);
	}

	printf("the chunk size does not change the result\n");
	{
		static int32_t a[N * 2], b[N * 2], raw[N * 2];
		static const int sizes[] = { 7644, 997, 588, 1 };
		int s;

		fill(raw, N, 0.8, 330);
		/* packets with different codes, some close together, some far apart */
		put_packet_a(raw, 2000, 0x14);
		put_packet_a(raw, 9000, 0x10);
		put_packet_a(raw, 9400, 0x12);
		put_packet_a(raw, 30000, 0x00);
		memcpy(a, raw, sizeof a);
		hdcd_init(&h, 44100, 2000);
		hdcd_decode(&h, a, N);
		for (s = 0; s < 4; s++) {
			char what[80];
			int diff = 0, size = sizes[s];

			memcpy(b, raw, sizeof b);
			hdcd_init(&h, 44100, 2000);
			for (i = 0; i < N; i += size)
				hdcd_decode(&h, b + i * 2, N - i < size ? N - i : size);
			for (i = 0; i < N * 2; i++)
				if (b[i] != a[i])
					diff++;
			snprintf(what, sizeof what, "differences, chunks of %d frames vs. all at once", size);
			check(what, diff, 0);
		}
	}

	printf("peak extend: a loud sample is stretched above 2^30\n");
	{
		static int32_t y[4000 * 2];
		int j;

		for (j = 0; j < 4000; j++) {
			int v = j % 2 ? 32000 : -32000;

			y[j * 2] = y[j * 2 + 1] = v & ~1;
		}
		put_packet_a(y, 100, 0x10);   /* peak extend only, no gain */
		/* put_packet_a overwrote LSBs only */
		hdcd_init(&h, 44100, 2000);
		hdcd_decode(&h, y, 4000);
		check("loud sample after the packet is above 32000 << 15",
		      y[3000 * 2 + 2] > (int32_t)(32000LL << 15), 1);
		check("negative loud sample is below -32000 << 15",
		      y[3000 * 2] < -(int32_t)(32000LL << 15), 1);
		check("loud sample stays inside int32", y[3000 * 2 + 2] > 0, 1);
	}

	printf("timer: the control resets after 2 s without a packet\n");
	{
		static int32_t z[44100 * 3 * 2];
		int n = 44100 * 3;

		fill(z, n, 0.5, 220);
		put_packet_a(z, 1000, 0x14);
		hdcd_init(&h, 44100, 2000);
		hdcd_decode(&h, z, 1500);
		check("control set after the packet", h.ch[0].control, 0x18);
		hdcd_decode(&h, z + 1500 * 2, n - 1500);
		check("control back to 0 after the timer ran out", h.ch[0].control, 0);
		check("timer expiry counted", h.ch[0].count_sustain_expired, 1);
	}

	printf("%s\n", failures ? "FAILED" : "all ok");
	return failures != 0;
}
