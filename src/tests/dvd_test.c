/* dvd_test.c - the IFO reader and the LPCM unpacker of x45cd/dvd.c.
 *
 * The IFO / VOB samples are real files from a disc (build/dvd/); without them only the synthetic checks run. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../x45cd/dvd.h"

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint8_t *slurp(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	uint8_t *b;
	long n;

	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	b = malloc((size_t)n);
	if (b && fread(b, 1, (size_t)n, f) != (size_t)n) {
		free(b);
		b = NULL;
	}
	fclose(f);
	*len = (size_t)n;
	return b;
}

/* a program stream sector with one private-stream-1 packet carrying `n` bytes of LPCM */
static void make_sector(uint8_t *s, int sub, int qfsch, const uint8_t *pcm, int n)
{
	int pes_len;

	memset(s, 0xff, DVD_SECTOR);
	memcpy(s, "\0\0\1\xba\x44\0\4\0\4\1\1\x89\xc3\xf8", 14);      /* pack header, no stuffing */
	s[14] = 0; s[15] = 0; s[16] = 1; s[17] = 0xbd;
	pes_len = 3 + 0 + 7 + n;                                    /* flags, no PES header data, LPCM header */
	s[18] = (uint8_t)(pes_len >> 8); s[19] = (uint8_t)pes_len;
	s[20] = 0x81; s[21] = 0; s[22] = 0;
	s[23] = (uint8_t)sub; s[24] = 1; s[25] = 0; s[26] = 4; s[27] = 0; s[28] = (uint8_t)qfsch; s[29] = 0xff;
	memcpy(s + 30, pcm, (size_t)n);
	/* the rest of the sector is padding (0xBE) */
	{
		int off = 30 + n;

		if (off + 6 <= DVD_SECTOR) {
			s[off] = 0; s[off + 1] = 0; s[off + 2] = 1; s[off + 3] = 0xbe;
			s[off + 4] = (uint8_t)((DVD_SECTOR - off - 6) >> 8);
			s[off + 5] = (uint8_t)(DVD_SECTOR - off - 6);
		}
	}
}

/* a PCM stream with substream 0xA0 + stream */
static void pcm_init(struct dvd_pcm *p, int stream)
{
	static struct dvd_title t;

	t.fmt = 4;
	t.sub = 0xA0 + stream;
	dvd_pcm_init(p, &t);
}

static void test_synthetic(void)
{
	struct dvd_pcm p;
	uint8_t sec[DVD_SECTOR], pcm[16];
	int32_t out[64];
	int n;

	/* 16 bit, 48 kHz, 2 channels: 0x1234 / 0xedcb, then 0x0001 / 0x8000 */
	pcm_init(&p, 0);
	pcm[0] = 0x12; pcm[1] = 0x34; pcm[2] = 0xed; pcm[3] = 0xcb;
	pcm[4] = 0x00; pcm[5] = 0x01; pcm[6] = 0x80; pcm[7] = 0x00;
	make_sector(sec, 0xA0, 0x01, pcm, 8);
	n = dvd_pcm_sector(&p, sec, out, 32);
	CHECK(n == 2, "16 bit: %d frames", n);
	CHECK(out[0] == 0x12340000 && out[1] == (int32_t)0xedcb0000 && out[2] == 0x00010000 &&
	      out[3] == (int32_t)0x80000000, "16 bit values %08x %08x %08x %08x", out[0], out[1], out[2], out[3]);

	/* another substream is not ours */
	pcm_init(&p, 1);
	n = dvd_pcm_sector(&p, sec, out, 32);
	CHECK(n == 0, "wrong substream gave %d frames", n);

	/* a sample frame cut in two by a packet boundary is put together again */
	pcm_init(&p, 0);
	make_sector(sec, 0xA0, 0x01, pcm, 6);       /* 1.5 frames */
	n = dvd_pcm_sector(&p, sec, out, 32);
	CHECK(n == 1, "split frame, first part: %d", n);
	make_sector(sec, 0xA0, 0x01, pcm + 6, 2);
	n = dvd_pcm_sector(&p, sec, out + 2, 30);
	CHECK(n == 1 && out[2] == 0x00010000 && out[3] == (int32_t)0x80000000, "split frame joined: n=%d %08x %08x",
	      n, out[2], out[3]);

	/* 24 bit stereo: one group is 2 frames, 12 bytes: hi16 of L0 R0 L1 R1, then the low byte of each */
	{
		uint8_t g[12] = { 0x12, 0x34, 0xab, 0xcd, 0x01, 0x02, 0xfe, 0xff, 0x56, 0x78, 0x9a, 0xbc };

		pcm_init(&p, 0);
		make_sector(sec, 0xA0, 0x81, g, 12);        /* quantization 2 (24 bit) is bits 7-6 = 10 -> 0x80 */
		sec[28] = 0x81;
		n = dvd_pcm_sector(&p, sec, out, 32);
		/* qfsch 0x81 has q = 2 -> 24 bit, 2 channels */
		CHECK(n == 2, "24 bit: %d frames", n);
		CHECK(out[0] == 0x12345600 && out[1] == (int32_t)0xabcd7800 && out[2] == 0x01029a00 &&
		      out[3] == (int32_t)0xfeffbc00, "24 bit values %08x %08x %08x %08x", out[0], out[1], out[2], out[3]);
	}
}

static void test_real(void)
{
	size_t ilen = 0, vlen = 0;
	uint8_t *ifo = slurp("../build/dvd/vts01.ifo", &ilen);
	uint8_t *vob = slurp("../build/dvd/chunk1.vob", &vlen);
	struct dvd_title t;

	if (!ifo)
		ifo = slurp("build/dvd/vts01.ifo", &ilen);
	if (!vob)
		vob = slurp("build/dvd/chunk1.vob", &vlen);
	if (!ifo) {
		printf("dvd_test: no sample IFO, real-disc checks skipped\n");
		return;
	}
	CHECK(dvd_parse_ifo(ifo, ilen, &t) == 0, "the IFO does not parse");
	if (failures)
		return;
	CHECK(t.nch == 18, "chapters %d", t.nch);
	CHECK(t.bits == 16 && t.rate == 48000 && t.channels == 2 && t.astream == 0,
	      "stream %d: %d bit, %d Hz, %d ch", t.astream, t.bits, t.rate, t.channels);
	CHECK(t.ch[0].first == 0 && t.ch[0].last == 13801, "chapter 1 sectors %d..%d", t.ch[0].first, t.ch[0].last);
	CHECK(t.ch[17].first == 1933129 && t.ch[17].last == 1946886, "chapter 18 sectors %d..%d",
	      t.ch[17].first, t.ch[17].last);
	CHECK(t.ms / 1000 > 4780 && t.ms / 1000 < 4800, "title time %d s", t.ms / 1000);
	printf("dvd_test: %d chapters, %d s, stream %d: %d bit %d Hz %d ch\n", t.nch, t.ms / 1000, t.astream,
	       t.bits, t.rate, t.channels);
	if (vob) {
		struct dvd_pcm p;
		static int32_t out[1200 * 2];
		long frames = 0, jumps = 0;
		int s, prev[2] = { 0, 0 }, have = 0;
		double sum = 0;
		long maxd = 0;

		dvd_pcm_init(&p, &t);
		for (s = 0; s < (int)(vlen / DVD_SECTOR); s++) {
			int n = dvd_pcm_sector(&p, vob + (size_t)s * DVD_SECTOR, out, 1200), i;

			for (i = 0; i < n; i++) {
				int l = out[2 * i] >> 16, r = out[2 * i + 1] >> 16;

				sum += (double)l * l + (double)r * r;
				if (have) {
					long d = abs(l - prev[0]);

					if (d > maxd)
						maxd = d;
					if (d > 20000)
						jumps++;
				}
				prev[0] = l;
				prev[1] = r;
				have = 1;
			}
			frames += n;
		}
		printf("dvd_test: %ld frames (%.2f s) from %zu sectors, rms %.0f, largest step %ld, jumps %ld\n", frames,
		       frames / 48000.0, vlen / DVD_SECTOR, frames ? __builtin_sqrt(sum / (2.0 * (double)frames)) : 0.0,
		       maxd, jumps);
		CHECK(frames > 0, "no PCM in the sample");
		/* 3000 sectors hold about 3000 * 2008 / 4 frames if all were audio; video takes most */
		CHECK(jumps < 5, "%ld jumps in the waveform: packets joined wrongly", jumps);
		free(vob);
	}
	free(ifo);
}

/* all of a VOB's sound of one stream, the top 16 bits of both channels; returns the frames */
static long decode_all(const uint8_t *vob, size_t vlen, const struct dvd_title *t, int16_t *L, int16_t *R, long cap)
{
	struct dvd_pcm p;
	static int32_t out[1100 * 2];
	long n = 0;
	size_t s;

	dvd_pcm_init(&p, t);
	for (s = 0; s < vlen / DVD_SECTOR; s++) {
		int k = dvd_pcm_sector(&p, vob + s * DVD_SECTOR, out, 1100), i;

		while (k > 0) {
			for (i = 0; i < k && n < cap; i++, n++) {
				L[n] = (int16_t)(out[2 * i] >> 16);
				R[n] = (int16_t)(out[2 * i + 1] >> 16);
			}
			k = dvd_pcm_flush(&p, out, 1100);
		}
	}
	dvd_pcm_done(&p);
	return n;
}

static void wave_stats(const char *what, const int16_t *L, const int16_t *R, long n, double *rms_out, long *jumps_out)
{
	double sum = 0;
	long i, jumps = 0, maxd = 0;

	for (i = 0; i < n; i++) {
		sum += (double)L[i] * L[i] + (double)R[i] * R[i];
		if (i) {
			long d = labs((long)L[i] - L[i - 1]);

			if (d > maxd)
				maxd = d;
			if (d > 20000)
				jumps++;
		}
	}
	*rms_out = n ? __builtin_sqrt(sum / (2.0 * (double)n)) : 0.0;
	*jumps_out = jumps;
	printf("dvd_test: %s: %ld frames (%.2f s), rms %.0f, largest step %ld, jumps %ld\n", what, n, n / 48000.0,
	       *rms_out, maxd, jumps);
}

/* the second sample disc: Dolby Digital 2.0 (stream 0) and 5.1 (stream 1), unscrambled */
static void test_ac3_disc(void)
{
	size_t ilen = 0, vlen = 0;
	uint8_t *ifo = slurp("../build/dvd2/vts01.ifo", &ilen);
	uint8_t *vob = slurp("../build/dvd2/chunk1.vob", &vlen);
	struct dvd_title t, t1;
	int16_t *L0, *R0, *L1, *R1;
	long n0, n1, i;
	double rms0, rms1, ab = 0, aa = 0, bb = 0;
	long j0, j1;
	clock_t t0;
	enum { CAP = 3000000 };

	if (!ifo)
		ifo = slurp("build/dvd2/vts01.ifo", &ilen);
	if (!vob)
		vob = slurp("build/dvd2/chunk1.vob", &vlen);
	if (!ifo) {
		printf("dvd_test: no Dolby Digital sample IFO, check skipped\n");
		return;
	}
	CHECK(dvd_parse_ifo(ifo, ilen, &t) == 0, "the Dolby Digital IFO does not parse as playable");
	if (failures)
		return;
	CHECK(t.fmt == 0 && t.nch == 23, "format %d, %d chapters", t.fmt, t.nch);
	CHECK(t.sub == 0x80 && t.channels == 2 && t.rate == 48000, "stereo stream wanted: sub 0x%02x, %d ch, %d Hz",
	      t.sub, t.channels, t.rate);
	CHECK(!strcmp(dvd_fmt_name(t.fmt), "Dolby Digital"), "name %s", dvd_fmt_name(t.fmt));
	printf("dvd_test: second disc: %s, %d chapters, %d s, stream %d (%d ch)\n", dvd_fmt_name(t.fmt), t.nch,
	       t.ms / 1000, t.astream, t.channels);
	free(ifo);
	if (!vob)
		return;
	L0 = malloc(CAP * sizeof *L0);
	R0 = malloc(CAP * sizeof *R0);
	L1 = malloc(CAP * sizeof *L1);
	R1 = malloc(CAP * sizeof *R1);
	t1 = t;
	t1.sub = 0x81;
	t1.astream = 1;
	t1.channels = 6;
	t0 = clock();
	n0 = decode_all(vob, vlen, &t, L0, R0, CAP);
	n1 = decode_all(vob, vlen, &t1, L1, R1, CAP);
	t0 = clock() - t0;
	printf("dvd_test: decoding both streams took %.2f s of CPU for %.2f s of sound each\n",
	       (double)t0 / CLOCKS_PER_SEC, n0 / 48000.0);
	CHECK(n0 > 48000 && n0 % 1536 == 0, "stereo stream: %ld frames", n0);
	CHECK(n1 > 48000 && n1 % 1536 == 0, "5.1 stream: %ld frames", n1);
	CHECK(n0 == n1, "the two streams differ in length: %ld and %ld", n0, n1);
	wave_stats("stereo stream", L0, R0, n0, &rms0, &j0);
	wave_stats("5.1 downmix  ", L1, R1, n1, &rms1, &j1);
	CHECK(rms0 > 200 && rms1 > 200, "silent: rms %.0f / %.0f", rms0, rms1);
	CHECK(j0 < 5 && j1 < 5, "%ld / %ld jumps in the waveform: frames joined wrongly", j0, j1);
	for (i = 0; i < n0 && i < n1; i++) {
		ab += (double)L0[i] * L1[i];
		aa += (double)L0[i] * L0[i];
		bb += (double)L1[i] * L1[i];
	}
	printf("dvd_test: stereo vs 5.1 downmix, left channel correlation %.3f\n",
	       aa > 0 && bb > 0 ? ab / __builtin_sqrt(aa * bb) : 0.0);
	{
		/* are the two streams the same programme, at what offset: best correlation over +-1600 frames */
		long lag, best_lag = 0, m = 40000;
		double best = 0, lr0 = 0, l0 = 0, r0 = 0;

		for (lag = -1600; lag <= 1600; lag++) {
			double c = 0, x = 0, y = 0;

			for (i = 8000; i < 8000 + m && i + lag < n1 && i < n0; i++) {
				c += (double)L0[i] * L1[i + lag];
				x += (double)L0[i] * L0[i];
				y += (double)L1[i + lag] * L1[i + lag];
			}
			c = x > 0 && y > 0 ? c / __builtin_sqrt(x * y) : 0;
			if (c > best || -c > best) {
				best = c < 0 ? -c : c;
				best_lag = lag;
			}
		}
		for (i = 0; i < n0; i++) {
			lr0 += (double)L0[i] * R0[i];
			l0 += (double)L0[i] * L0[i];
			r0 += (double)R0[i] * R0[i];
		}
		printf("dvd_test: best |correlation| %.3f at lag %ld; stereo stream left vs right %.3f\n", best, best_lag,
		       l0 > 0 && r0 > 0 ? lr0 / __builtin_sqrt(l0 * r0) : 0.0);
	}
	free(L0); free(R0); free(L1); free(R1);
	free(vob);
}

int main(void)
{
	test_synthetic();
	test_real();
	test_ac3_disc();
	if (failures) {
		printf("dvd_test: %d failures\n", failures);
		return 1;
	}
	printf("dvd_test: ok\n");
	return 0;
}
