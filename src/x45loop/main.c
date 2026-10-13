/* x45loop - level feed and spectrum for the meters.
 *
 * The analogue sources reach the DAC entirely in hardware (ADC -> NJU72341 ->
 * SRC4392 -> ES9018); the stock firmware plays nothing to the XMOS for them.
 * It still opens the SSI capture (hw:1,0, S24_LE 192000) for its VU meters, and
 * so does this: it reads that capture and never writes to any playback device.
 * (An earlier version looped the capture into the XMOS; that was wrong, and
 * SIGKILLing a 192k USB stream wedges the XMOS until reboot.)
 *
 *   x45loop [-r rate] [-b 16|24] [-f fifo] [-s specfile]
 *
 *   -r rate   sample rate (default 192000)
 *   -b bits   capture format, 24 (default) or 16
 *   -f fifo   read S16_LE stereo from this FIFO (MPD's fifo output) instead of ALSA
 *   -s file   also write the analyzer state there (JSON, replaced atomically ~25x/s)
 *
 * stdout: S16_LE stereo, one frame per ALSA period holding that period's
 * left/right peak (top 16 bits of the 24-bit sample). The reader may be slow or
 * absent; frames are dropped rather than ever blocking the capture.
 *
 * Analyzer file: {"seq":n,"rate":R,"l":[rms,peak],"r":[rms,peak],"c":corr,"b":[128 dB values],
 *                 "wl2":[200 ints],"wr2":[200 ints],"sb":[128 dB values]}
 * levels in dBFS; "b" are 128 log-spaced bands 20 Hz .. 20 kHz (peak of the bins in
 * each band, dBFS of a sine), taken from a Hann-windowed FFT of the mono sum (twelve times a second);
 * "sb" are the same bands of the side signal (L - R) / 2, from the same FFT (see ana_transform): a hard-panned sine of
 * amplitude A is A / 2 in both "b" and "sb", a centred one is A in "b" and silence in "sb".
 * "c" is the correlation of the channels since the last file, -1 (opposite) .. 1 (the same); "wl2"/"wr2"
 * are 25 ms of each channel in hundredths of a percent of full scale (-10000..10000: whole percent drew a
 * lattice of dots at low volume, where the vectorscope magnifies a quiet signal up to 40 times), starting at a rising zero crossing of the
 * sum so that a steady tone stands still: what an oscilloscope and a vectorscope draw.
 *
 * Runs until SIGTERM/SIGINT or an unrecoverable ALSA error.
 */
#define _GNU_SOURCE
#include <alsa/asoundlib.h>

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CAP_DEV "hw:1,0"
#define PERIOD  1920 /* 10 ms at 192 kHz, what the stock firmware uses */
#define NB      128
#define FFT_MAX 16384
#define WAVE_RING 16384   /* frames of each channel kept for the scope (a power of two) */
#define WAVE_COLS 200     /* points of each channel handed on */

static volatile sig_atomic_t stop_now;
static void on_sig(int s) { (void)s; stop_now = 1; }

/* ------------------------------------------------------------- analyzer --- */


struct ana {
	unsigned rate;
	int n;                       /* FFT size */
	float *win, *ring, *sring, *re, *im, *cosv, *sinv;
	float *mm, *ms;              /* magnitude of each bin for the mid and for the side signal (n / 2 + 1 each) */
	int fill, wr;                /* samples in ring, write index */
	double ssq[2];               /* sum of squares and peaks since the last transform */
	float pk[2];
	long cnt;
	int lo[NB], hi[NB];          /* FFT bin range of each band */
	float fb[NB];                /* fractional bin at each band centre */
	unsigned seq;
	const char *path;
	float lastb[NB];
	float lastsb[NB];            /* the same bands of the side signal (L - R) / 2 */
	int estimate;                /* work out the real sample rate from the capture's pace */
	double tx_t, est_t0;         /* last transform; start of the rate measurement */
	long est_frames;
	float *wv[2];                /* the last WAVE_RING frames of each channel */
	unsigned wh;                 /* where the next frame goes (counts on; masked when used) */
	double slr;                  /* sum of l * r since the last file */
	double wr_t;                 /* when the file was written last */
};

static double mono_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* Tables for a sample rate: FFT size, window, twiddles, which bins make up each band. */
static void ana_setup(struct ana *a, unsigned rate)
{
	int i;
	double fmax = rate * 0.45 < 20000.0 ? rate * 0.45 : 20000.0, fmin = 20.0;

	a->rate = rate;
	a->n = rate >= 96000 ? 16384 : rate >= 48000 ? 8192 : 4096;
	if (a->n > FFT_MAX)
		a->n = FFT_MAX;
	memset(a->ring, 0, FFT_MAX * sizeof(float));
	memset(a->sring, 0, FFT_MAX * sizeof(float));
	a->fill = a->wr = 0;
	for (i = 0; i < a->n; i++)
		a->win[i] = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * i / a->n);
	for (i = 0; i < a->n / 2; i++) {
		a->cosv[i] = cosf(2.0f * (float)M_PI * i / a->n);
		a->sinv[i] = -sinf(2.0f * (float)M_PI * i / a->n);
	}
	for (i = 0; i < NB; i++) {
		double f0 = fmin * pow(fmax / fmin, (double)i / NB);
		double f1 = fmin * pow(fmax / fmin, (double)(i + 1) / NB);

		a->lo[i] = (int)floor(f0 * a->n / rate);
		a->hi[i] = (int)ceil(f1 * a->n / rate);
		if (a->hi[i] <= a->lo[i])
			a->hi[i] = a->lo[i] + 1;
		if (a->hi[i] > a->n / 2)
			a->hi[i] = a->n / 2;
		if (a->lo[i] >= a->hi[i])
			a->lo[i] = a->hi[i] - 1;
		a->fb[i] = (float)(fmin * pow(fmax / fmin, ((double)i + 0.5) / NB) * a->n / rate);
		a->lastb[i] = -120.0f;
		a->lastsb[i] = -120.0f;
	}
}

static void ana_init(struct ana *a, unsigned rate, const char *path)
{
	memset(a, 0, sizeof *a);
	a->path = path;
	a->win = malloc(FFT_MAX * sizeof(float));
	a->ring = calloc(FFT_MAX, sizeof(float));
	a->sring = calloc(FFT_MAX, sizeof(float));
	a->mm = calloc(FFT_MAX / 2 + 1, sizeof(float));
	a->ms = calloc(FFT_MAX / 2 + 1, sizeof(float));
	a->re = malloc(FFT_MAX * sizeof(float));
	a->im = malloc(FFT_MAX * sizeof(float));
	a->cosv = malloc(FFT_MAX / 2 * sizeof(float));
	a->sinv = malloc(FFT_MAX / 2 * sizeof(float));
	a->wv[0] = calloc(WAVE_RING, sizeof(float));
	a->wv[1] = calloc(WAVE_RING, sizeof(float));
	if (!a->win || !a->ring || !a->sring || !a->mm || !a->ms || !a->re || !a->im || !a->cosv || !a->sinv ||
	    !a->wv[0] || !a->wv[1]) {
		fprintf(stderr, "x45loop: out of memory\n");
		exit(5);
	}
	ana_setup(a, rate);
	a->est_t0 = mono_s();
}
static void fft(struct ana *a)
{
	int n = a->n, i, j, k, m;

	for (i = 1, j = 0; i < n; i++) {
		int bit = n >> 1;

		for (; j & bit; bit >>= 1)
			j ^= bit;
		j ^= bit;
		if (i < j) {
			float t = a->re[i]; a->re[i] = a->re[j]; a->re[j] = t;
			t = a->im[i]; a->im[i] = a->im[j]; a->im[j] = t;
		}
	}
	for (m = 2; m <= n; m <<= 1) {
		int half = m >> 1, step = n / m;

		for (k = 0; k < n; k += m)
			for (j = 0; j < half; j++) {
				float wr = a->cosv[j * step], wi = a->sinv[j * step];
				int p = k + j, q = p + half;
				float tr = a->re[q] * wr - a->im[q] * wi;
				float ti = a->re[q] * wi + a->im[q] * wr;

				a->re[q] = a->re[p] - tr;
				a->im[q] = a->im[p] - ti;
				a->re[p] += tr;
				a->im[p] += ti;
			}
	}
}

static float db(float x)
{
	return x < 1e-7f ? -140.0f : 20.0f * log10f(x);
}

/* The last 25 ms of both channels as WAVE_COLS points each, in hundredths of a percent of full scale. The picture starts at
 * a rising zero crossing of the sum (looked for in the older half of the 50 ms kept) and each point is the
 * strongest sample of its stretch, so a steady tone stands still and a short peak is not lost. */
static void wave_cut(const struct ana *a, int *out_l, int *out_r)
{
	const unsigned mask = WAVE_RING - 1;
	int win = (int)(a->rate / 40), start, i, c;
	unsigned base;

	if (win < 400)
		win = 400;
	if (win > WAVE_RING / 2)
		win = WAVE_RING / 2;
	base = a->wh - 2 * (unsigned)win;
	start = 0;
	for (i = 1; i < win; i++) {
		float p = a->wv[0][(base + i - 1) & mask] + a->wv[1][(base + i - 1) & mask];
		float q = a->wv[0][(base + i) & mask] + a->wv[1][(base + i) & mask];

		if (p < -0.004f && q >= 0.0f) {
			start = i;
			break;
		}
	}
	for (c = 0; c < WAVE_COLS; c++) {
		int s0 = start + c * win / WAVE_COLS, s1 = start + (c + 1) * win / WAVE_COLS, k;
		float bl = 0, br = 0;

		if (s1 <= s0)
			s1 = s0 + 1;
		for (k = s0; k < s1; k++) {
			float l = a->wv[0][(base + k) & mask], r = a->wv[1][(base + k) & mask];

			if (fabsf(l) > fabsf(bl))
				bl = l;
			if (fabsf(r) > fabsf(br))
				br = r;
		}
		out_l[c] = (int)lrintf(bl * 10000.0f);
		out_r[c] = (int)lrintf(br * 10000.0f);
		if (out_l[c] > 10000) out_l[c] = 10000;
		if (out_l[c] < -10000) out_l[c] = -10000;
		if (out_r[c] > 10000) out_r[c] = 10000;
		if (out_r[c] < -10000) out_r[c] = -10000;
	}
}

static void ana_write(struct ana *a)
{
	char tmp[512], buf[8192];
	FILE *f;
	int i, o = 0, wl[WAVE_COLS], wr[WAVE_COLS];
	float lr[2], lp[2], corr = 0;
	double den = sqrt(a->ssq[0] * a->ssq[1]);

	for (i = 0; i < 2; i++) {
		lr[i] = a->cnt ? db(sqrtf((float)(a->ssq[i] / a->cnt))) : -140.0f;
		lp[i] = db(a->pk[i]);
	}
	if (den > 1e-9)
		corr = (float)(a->slr / den);
	if (corr > 1)
		corr = 1;
	if (corr < -1)
		corr = -1;
	wave_cut(a, wl, wr);
	o += snprintf(buf + o, sizeof buf - o, "{\"seq\":%u,\"rate\":%u,\"l\":[%.1f,%.1f],\"r\":[%.1f,%.1f],\"c\":%.2f,\"b\":[",
		      ++a->seq, a->rate, lr[0], lp[0], lr[1], lp[1], corr);
	for (i = 0; i < NB; i++)
		o += snprintf(buf + o, sizeof buf - o, "%s%.1f", i ? "," : "", a->lastb[i]);
	o += snprintf(buf + o, sizeof buf - o, "],\"wl2\":[");
	for (i = 0; i < WAVE_COLS; i++)
		o += snprintf(buf + o, sizeof buf - o, "%s%d", i ? "," : "", wl[i]);
	o += snprintf(buf + o, sizeof buf - o, "],\"wr2\":[");
	for (i = 0; i < WAVE_COLS; i++)
		o += snprintf(buf + o, sizeof buf - o, "%s%d", i ? "," : "", wr[i]);
	o += snprintf(buf + o, sizeof buf - o, "],\"sb\":[");
	for (i = 0; i < NB; i++)
		o += snprintf(buf + o, sizeof buf - o, "%s%.1f", i ? "," : "", a->lastsb[i]);
	o += snprintf(buf + o, sizeof buf - o, "]}\n");
	snprintf(tmp, sizeof tmp, "%s.tmp", a->path);
	f = fopen(tmp, "w");
	if (!f)
		return;
	fwrite(buf, 1, (size_t)o, f);
	if (fclose(f) == 0)
		rename(tmp, a->path);
	a->ssq[0] = a->ssq[1] = 0;
	a->pk[0] = a->pk[1] = 0;
	a->slr = 0;
	a->cnt = 0;
	a->wr_t = mono_s();
}

/* The level of band b from the bin magnitudes mag[]. */
static float band_peak(const struct ana *a, const float *mag, int b)
{
	float m = 0;
	int i;

	if (a->hi[b] - a->lo[b] <= 1) {
		/* narrower than one bin (the low end): read between the neighbouring bins, so
		 * adjacent bands differ instead of repeating one value */
		int i0 = (int)a->fb[b];
		float fr = a->fb[b] - (float)i0;

		if (i0 < 0)
			i0 = 0;
		if (i0 + 1 >= a->n / 2)
			i0 = a->n / 2 - 2;
		return mag[i0] * (1.0f - fr) + mag[i0 + 1] * fr;
	}
	for (i = a->lo[b]; i < a->hi[b]; i++)
		if (mag[i] > m)
			m = mag[i];
	return m;
}

/* One FFT gives the bands of the mono sum and of the side signal. The ring holds M = (L + R) / 2 and the second ring
 * S = (L - R) / 2; z = M + jS is transformed, and since M and S are real, the two spectra are told apart by the symmetry
 * of the transform: M(k) = (Z(k) + conj(Z(n-k))) / 2 and S(k) = (Z(k) - conj(Z(n-k))) / 2j. A second transform is not needed. */
static void ana_transform(struct ana *a)
{
	int i, b;
	float norm = 0, wsum = 0;

	for (i = 0; i < a->n; i++) {
		int k = (a->wr + i) % a->n;

		a->re[i] = a->ring[k] * a->win[i];
		a->im[i] = a->sring[k] * a->win[i];
		wsum += a->win[i];
	}
	norm = 2.0f / wsum;
	fft(a);
	for (i = 0; i <= a->n / 2; i++) {
		int j = (a->n - i) & (a->n - 1);   /* n is a power of two: the mirror bin, bin 0 being its own */
		float pr = a->re[i] + a->re[j], pi = a->im[i] - a->im[j];   /* 2 Re M, 2 Im M */
		float qr = a->im[i] + a->im[j], qi = a->re[i] - a->re[j];   /* 2 Re S, -2 Im S */

		a->mm[i] = 0.5f * sqrtf(pr * pr + pi * pi) * norm;
		a->ms[i] = 0.5f * sqrtf(qr * qr + qi * qi) * norm;
	}
	for (b = 0; b < NB; b++) {
		a->lastb[b] = db(band_peak(a, a->mm, b));
		a->lastsb[b] = db(band_peak(a, a->ms, b));
	}
	ana_write(a);
}

/* frames: interleaved samples scaled to +-1.0 */
static void ana_feed(struct ana *a, const float *s, int frames)
{
	int i;
	double t;

	for (i = 0; i < frames; i++) {
		float l = s[2 * i], r = s[2 * i + 1], m = 0.5f * (l + r);

		a->ssq[0] += (double)l * l;
		a->ssq[1] += (double)r * r;
		if (fabsf(l) > a->pk[0]) a->pk[0] = fabsf(l);
		if (fabsf(r) > a->pk[1]) a->pk[1] = fabsf(r);
		a->slr += (double)l * r;
		a->cnt++;
		a->ring[a->wr] = m;
		a->sring[a->wr] = 0.5f * (l - r);
		a->wr = (a->wr + 1) % a->n;
		if (a->fill < a->n)
			a->fill++;
		a->wv[0][a->wh & (WAVE_RING - 1)] = l;
		a->wv[1][a->wh & (WAVE_RING - 1)] = r;
		a->wh++;
	}
	if (a->estimate)
		a->est_frames += frames;
	t = mono_s();
	/* The display rate follows the clock, not the sample count: a capture that runs at the
	 * stream's own rate (44.1 kHz from a PC) would otherwise update only 3 times a second.
	 * The bands are worked out twelve times a second, the file (levels, picture of the wave) written
	 * twice as often. */
	if (a->fill >= a->n && t - a->tx_t >= 0.08) {
		a->tx_t = t;
		ana_transform(a);
	} else if (a->fill >= a->n && t - a->wr_t >= 0.04) {
		ana_write(a);
	}
	/* The capture delivers frames at the rate of whatever is playing, whatever it was opened
	 * with; measure it, so the bands are laid out for the real rate. */
	if (a->estimate && t - a->est_t0 >= 1.5) {
		static const unsigned std[] = { 32000, 44100, 48000, 88200, 96000, 176400, 192000 };
		double est = (double)a->est_frames / (t - a->est_t0);
		unsigned best = 0;
		unsigned k;

		for (k = 0; k < sizeof std / sizeof *std; k++)
			if (fabs(est - std[k]) < std[k] * 0.05)
				best = std[k];
		a->est_t0 = t;
		a->est_frames = 0;
		if (best && best != a->rate)
			ana_setup(a, best);
	}
}

/* ------------------------------------------------------------- capture --- */

static int open_capture(snd_pcm_t **pcm, unsigned rate, int bits)
{
	snd_pcm_hw_params_t *hw;
	snd_pcm_uframes_t p = PERIOD, buf;
	unsigned r = rate;
	int e;

	if (bits == 16)
		p = rate / 100;
	if ((e = snd_pcm_open(pcm, CAP_DEV, SND_PCM_STREAM_CAPTURE, 0)) < 0)
		return e;
	snd_pcm_hw_params_alloca(&hw);
	snd_pcm_hw_params_any(*pcm, hw);
	if ((e = snd_pcm_hw_params_set_access(*pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0 ||
	    (e = snd_pcm_hw_params_set_format(*pcm, hw, bits == 16 ? SND_PCM_FORMAT_S16_LE
								   : SND_PCM_FORMAT_S24_LE)) < 0 ||
	    (e = snd_pcm_hw_params_set_channels(*pcm, hw, 2)) < 0 ||
	    (e = snd_pcm_hw_params_set_rate_near(*pcm, hw, &r, 0)) < 0)
		goto fail;
	snd_pcm_hw_params_set_period_size_near(*pcm, hw, &p, 0);
	buf = p * 4;
	snd_pcm_hw_params_set_buffer_size_near(*pcm, hw, &buf);
	if ((e = snd_pcm_hw_params(*pcm, hw)) < 0)
		goto fail;
	return 0;
fail:
	snd_pcm_close(*pcm);
	*pcm = NULL;
	return e;
}

/* MPD's fifo output: S16_LE stereo, only the analyzer is fed */
static int run_fifo(const char *path, unsigned rate, const char *spec, pid_t parent)
{
	int fd = open(path, O_RDONLY | O_NONBLOCK);
	struct ana a;
	int16_t buf[4096];
	float fl[4096];
	int carry = 0;

	if (fd < 0) {
		fprintf(stderr, "x45loop: open %s: %s\n", path, strerror(errno));
		return 4;
	}
	ana_init(&a, rate, spec);
	while (!stop_now && getppid() == parent) {
		struct pollfd p = { fd, POLLIN, 0 };
		ssize_t n;
		int frames, i;

		if (poll(&p, 1, 200) <= 0)
			continue;
		n = read(fd, (char *)buf + carry, sizeof buf - (size_t)carry);
		if (n <= 0) {
			if (n == 0)
				usleep(20000);   /* every writer gone: poll answers at once, forever, until one comes back */
			continue;
		}
		n += carry;
		frames = (int)(n / 4);
		for (i = 0; i < frames * 2; i++)
			fl[i] = buf[i] / 32768.0f;
		ana_feed(&a, fl, frames);
		carry = (int)(n - frames * 4);
		if (carry)
			memmove(buf, (char *)buf + frames * 4, (size_t)carry);
	}
	close(fd);
	return 0;
}

int main(int argc, char **argv)
{
	unsigned rate = 192000;
	int bits = 24, i;
	const char *fifo = NULL, *spec = NULL;
	snd_pcm_t *cap = NULL;
	int32_t *in;
	float *fl;
	struct ana a;
	int e, fl_;
	pid_t parent;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-r") && i + 1 < argc)
			rate = (unsigned)atoi(argv[++i]);
		else if (!strcmp(argv[i], "-b") && i + 1 < argc)
			bits = atoi(argv[++i]) == 16 ? 16 : 24;
		else if (!strcmp(argv[i], "-f") && i + 1 < argc)
			fifo = argv[++i];
		else if (!strcmp(argv[i], "-s") && i + 1 < argc)
			spec = argv[++i];
	}

	if (rate < 8000 || rate > 384000)   /* the buffers and the band tables are made for rates up to this */
		rate = 192000;
	parent = getppid();
	signal(SIGPIPE, SIG_IGN);
	signal(SIGTERM, on_sig);
	signal(SIGINT, on_sig);
	if (fifo)
		return run_fifo(fifo, rate, spec ? spec : "/tmp/x45meter.json", parent);

	fl_ = fcntl(1, F_GETFL);
	if (fl_ >= 0)
		fcntl(1, F_SETFL, fl_ | O_NONBLOCK);

	if ((e = open_capture(&cap, rate, bits)) < 0) {
		fprintf(stderr, "x45loop: open %s: %s\n", CAP_DEV, snd_strerror(e));
		return 4;
	}
	in = malloc(PERIOD * 4 * 2 * sizeof *in);
	fl = malloc(PERIOD * 4 * 2 * sizeof *fl);
	if (!in || !fl)
		return 5;
	if (spec)
	{
		ana_init(&a, rate, spec);
		a.estimate = 1;
	}

	while (!stop_now && getppid() == parent) { /* leave when the UI is gone */
		snd_pcm_uframes_t want = bits == 16 ? rate / 100 : PERIOD;
		snd_pcm_sframes_t n = snd_pcm_readi(cap, in, want);
		int pk[2] = { 0, 0 };
		int16_t lv[2];

		if (n < 0) {
			if (n == -EINTR)
				continue;
			n = snd_pcm_recover(cap, (int)n, 1);
			if (n < 0) {
				fprintf(stderr, "x45loop: capture: %s\n", snd_strerror((int)n));
				return 6;
			}
			continue;
		}
		for (i = 0; i < n * 2; i++) {
			int32_t s, v;
			int a16;

			if (bits == 16) {
				v = ((int16_t *)in)[i];
				fl[i] = v / 32768.0f;
				a16 = v < 0 ? -v : v;
			} else {
				/* 24 bit in the low three bytes: sign-extend, keep the top 16 */
				s = (int32_t)((uint32_t)in[i] << 8) >> 8;
				fl[i] = s / 8388608.0f;
				a16 = (s < 0 ? -s : s) >> 8;
			}
			if (a16 > pk[i & 1])
				pk[i & 1] = a16;
		}
		if (spec)
			ana_feed(&a, fl, (int)n);
		lv[0] = (int16_t)(pk[0] > 32767 ? 32767 : pk[0]);
		lv[1] = (int16_t)(pk[1] > 32767 ? 32767 : pk[1]);
		(void)!write(1, lv, sizeof lv); /* pipe full or no reader: drop it */
	}
	snd_pcm_drop(cap);
	snd_pcm_close(cap);
	return 0;
}
