/* x45play  -  play WAV and FLAC to the X45's XMOS output, bit-perfect.
 *
 * This is the proof that the whole cross-build pipeline works: static musl ARM
 * binary, linking libFLAC and alsa-lib built the same way, with no dependency
 * on the device's eglibc rootfs and no vendor code in the path.
 *
 *   x45play [options] <file.wav|file.flac>
 *     -D <dev>   ALSA device (default: the XMOS, found by usbid)
 *     -v <0-100> set volume through /dev/x45 before playing
 *     -b         bring the audio board up first (power cycle + unmute)
 *     -i         print the file's format and the negotiated ALSA params, then exit
 *
 * The XMOS is resolved by **usbid 152a:86c1**, never by card index: power-cycling
 * the audio board re-enumerates it on USB and the index moves.
 *
 * Output format follows the source. The XMOS offers S32_LE (altset 1), S16_LE
 * (altset 2) and DSD_U32_BE (altset 3) at 44.1k-384k, so 16-bit sources go out
 * as S16_LE and anything deeper is left-justified into S32_LE, which is what the
 * hardware wants and avoids any resampling.
 */
#define _GNU_SOURCE
#include "../libx45/x45.h"

#include <FLAC/stream_decoder.h>
#include <alsa/asoundlib.h>

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define XMOS_USBID "152a:86c1"

static volatile sig_atomic_t stop_now;
static void on_sig(int s) { (void)s; stop_now = 1; }

static int info_only;

/* ------------------------------------------------- find the XMOS by usbid --- */

/* Returns a malloc'd "hw:N,0" for the card whose usbid matches, or NULL. */
static char *find_xmos(void)
{
	int card;

	for (card = 0; card < 8; card++) {
		char path[64], buf[64];
		FILE *f;
		size_t n;

		snprintf(path, sizeof path, "/proc/asound/card%d/usbid", card);
		f = fopen(path, "r");
		if (!f)
			continue;
		n = fread(buf, 1, sizeof buf - 1, f);
		fclose(f);
		buf[n] = 0;
		while (n && (buf[n - 1] == '\n' || buf[n - 1] == ' '))
			buf[--n] = 0;
		if (strcmp(buf, XMOS_USBID))
			continue;
		{
			char *dev = NULL;
			if (asprintf(&dev, "hw:%d,0", card) < 0)
				return NULL;
			return dev;
		}
	}
	return NULL;
}

/* ------------------------------------------------------------------- ALSA --- */

struct out {
	snd_pcm_t *pcm;
	unsigned rate;
	int channels;
	snd_pcm_format_t fmt;
	int bytes_per_sample;
	int opened;
	const char *devname;
	long long frames_written;
};

static int out_open(struct out *o, const char *dev, unsigned rate,
		    int channels, int bits)
{
	snd_pcm_hw_params_t *hw;
	unsigned exact = rate;
	int dir = 0, r;

	o->devname = dev;
	o->rate = rate;
	o->channels = channels;
	o->fmt = (bits <= 16) ? SND_PCM_FORMAT_S16_LE : SND_PCM_FORMAT_S32_LE;
	o->bytes_per_sample = (bits <= 16) ? 2 : 4;

	/* The XMOS master clock has two families and nothing else in the stack
	 * selects between them: left on the wrong one, 96 kHz plays at 88.2 kHz.
	 * Must happen before the PCM opens. */
	r = x45_clock_select(rate);
	if (r < 0)
		fprintf(stderr, "x45play: warning: cannot select the master-clock "
			"family for %u Hz: %s\n", rate, strerror(-r));
	else
		printf("clock    %u kHz family\n",
		       x45_clock_family(rate) == X45_CLOCK_48 ? 48 : 44);

	r = snd_pcm_open(&o->pcm, dev, SND_PCM_STREAM_PLAYBACK, 0);
	if (r < 0) {
		fprintf(stderr, "x45play: open %s: %s\n", dev, snd_strerror(r));
		return r;
	}
	snd_pcm_hw_params_alloca(&hw);
	snd_pcm_hw_params_any(o->pcm, hw);
	if ((r = snd_pcm_hw_params_set_access(o->pcm, hw,
					     SND_PCM_ACCESS_RW_INTERLEAVED)) < 0)
		goto fail;
	if ((r = snd_pcm_hw_params_set_format(o->pcm, hw, o->fmt)) < 0) {
		fprintf(stderr, "x45play: format %s rejected: %s\n",
			snd_pcm_format_name(o->fmt), snd_strerror(r));
		goto fail;
	}
	if ((r = snd_pcm_hw_params_set_channels(o->pcm, hw,
						(unsigned)channels)) < 0)
		goto fail;
	if ((r = snd_pcm_hw_params_set_rate_near(o->pcm, hw, &exact, &dir)) < 0)
		goto fail;
	if (exact != rate)
		fprintf(stderr, "x45play: warning: asked %u Hz, got %u Hz\n",
			rate, exact);
	o->rate = exact;
	if ((r = snd_pcm_hw_params(o->pcm, hw)) < 0)
		goto fail;
	o->opened = 1;

	printf("output   %s\n", dev);
	printf("         %u Hz, %d ch, %s\n", o->rate, o->channels,
	       snd_pcm_format_name(o->fmt));
	return 0;
fail:
	fprintf(stderr, "x45play: hw_params: %s\n", snd_strerror(r));
	snd_pcm_close(o->pcm);
	o->pcm = NULL;
	return r;
}

static void out_close(struct out *o)
{
	if (o->pcm) {
		if (o->opened)
			snd_pcm_drain(o->pcm);
		snd_pcm_close(o->pcm);
		o->pcm = NULL;
	}
}

/* Writes `frames` interleaved frames, recovering from underruns. */
static int out_write(struct out *o, const void *buf, snd_pcm_uframes_t frames)
{
	const char *p = buf;

	while (frames > 0 && !stop_now) {
		snd_pcm_sframes_t n = snd_pcm_writei(o->pcm, p, frames);
		if (n == -EPIPE) {
			fprintf(stderr, "x45play: underrun, recovering\n");
			snd_pcm_prepare(o->pcm);
			continue;
		}
		if (n == -ESTRPIPE) {
			while (snd_pcm_resume(o->pcm) == -EAGAIN)
				usleep(10000);
			snd_pcm_prepare(o->pcm);
			continue;
		}
		if (n < 0)
			return (int)n;
		p += (size_t)n * o->channels * o->bytes_per_sample;
		frames -= (snd_pcm_uframes_t)n;
		o->frames_written += n;
	}
	return 0;
}

/* -------------------------------------------------------------------- WAV --- */

static uint32_t rd32(const unsigned char *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd16(const unsigned char *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

static int play_wav(const char *path, const char *dev, struct out *o)
{
	unsigned char hdr[12], ch[8];
	FILE *f = fopen(path, "rb");
	int channels = 0, bits = 0;
	unsigned rate = 0;
	long data_len = 0;
	unsigned char *buf;
	size_t frame_bytes;
	int r = -1;

	if (!f) {
		fprintf(stderr, "x45play: %s: %s\n", path, strerror(errno));
		return -1;
	}
	if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "RIFF", 4) ||
	    memcmp(hdr + 8, "WAVE", 4)) {
		fprintf(stderr, "x45play: %s: not a RIFF/WAVE file\n", path);
		goto out;
	}
	/* walk the chunks; we need fmt then data */
	while (fread(ch, 1, 8, f) == 8) {
		uint32_t sz = rd32(ch + 4);
		if (!memcmp(ch, "fmt ", 4)) {
			unsigned char fmt[40];
			uint32_t want = sz > sizeof fmt ? sizeof fmt : sz;
			if (fread(fmt, 1, want, f) != want)
				goto out;
			if (sz > want) {
				if (sz - want > 0x7ffffff0u)   /* a long is 32 bits: a huge size would seek backwards, into this very header */
					goto out;
				fseek(f, (long)(sz - want), SEEK_CUR);
			}
			if (want < 16 || (rd16(fmt) != 1 && rd16(fmt) != 0xFFFE)) {   /* plain PCM (or its extensible form) only */
				fprintf(stderr, "x45play: %s: not PCM audio\n", path);
				goto out;
			}
			channels = rd16(fmt + 2);
			rate = rd32(fmt + 4);
			bits = rd16(fmt + 14);
		} else if (!memcmp(ch, "data", 4)) {
			data_len = sz > 0x7fffffffu ? 0x7fffffff : (long)sz;   /* long is 32 bits here */
			break;
		} else {
			if (sz > 0x7ffffff0u)
				goto out;
			fseek(f, (long)((sz + 1) & ~1u), SEEK_CUR);
		}
	}
	if (!channels || !rate || !bits || !data_len) {
		fprintf(stderr, "x45play: %s: incomplete WAV header\n", path);
		goto out;
	}
	if (bits != 16 && bits != 24 && bits != 32) {   /* the loops below work in whole 2, 3 or 4 byte samples */
		fprintf(stderr, "x45play: %s: %d bit samples are not supported\n", path, bits);
		goto out;
	}
	printf("source   %s\n", path);
	printf("         WAV %u Hz, %d ch, %d bit, %ld bytes of audio\n",
	       rate, channels, bits, data_len);

	if (info_only) { r = 0; goto out; }
	if (out_open(o, dev, rate, channels, bits) < 0)
		goto out;

	/* WAV 16-bit goes out as-is; 24/32-bit is already little-endian and
	 * left-justifiable into S32 by the ALSA format choice. For 24-bit packed
	 * (3 bytes) we widen to 4 bytes here. */
	frame_bytes = (size_t)channels * (size_t)(bits / 8);
	buf = malloc(4096 * (frame_bytes > 16 ? frame_bytes : 16));
	if (!buf)
		goto out;

	if (bits == 24) {
		unsigned char *wide = malloc(4096 * (size_t)channels * 4);
		if (!wide) { free(buf); goto out; }
		while (data_len > 0 && !stop_now) {
			size_t want = (size_t)(data_len < (long)(4096 * frame_bytes)
					       ? (size_t)data_len : 4096 * frame_bytes);
			size_t got = fread(buf, 1, want, f);
			size_t frames, i, c;
			if (!got)
				break;
			frames = got / frame_bytes;
			for (i = 0; i < frames; i++)
				for (c = 0; c < (size_t)channels; c++) {
					const unsigned char *s =
						buf + (i * frame_bytes) + c * 3;
					unsigned char *d = wide +
						((i * (size_t)channels) + c) * 4;
					d[0] = 0;
					d[1] = s[0];
					d[2] = s[1];
					d[3] = s[2];
				}
			if (out_write(o, wide, frames) < 0)
				break;
			data_len -= (long)got;
		}
		free(wide);
	} else {
		while (data_len > 0 && !stop_now) {
			size_t want = (size_t)(data_len < (long)(4096 * frame_bytes)
					       ? (size_t)data_len : 4096 * frame_bytes);
			size_t got = fread(buf, 1, want, f);
			if (!got)
				break;
			if (out_write(o, buf, got / frame_bytes) < 0)
				break;
			data_len -= (long)got;
		}
	}
	free(buf);
	printf("played   %lld frames\n", o->frames_written);
	r = 0;
out:
	fclose(f);
	return r;
}

/* ------------------------------------------------------------------- FLAC --- */

struct flacctx {
	struct out *o;
	const char *dev;
	unsigned rate;
	int channels, bits;
	int started;
	int32_t *ibuf;
	size_t ibuf_frames;
};

static void flac_meta(const FLAC__StreamDecoder *dec,
		      const FLAC__StreamMetadata *m, void *v)
{
	struct flacctx *c = v;

	(void)dec;
	if (m->type != FLAC__METADATA_TYPE_STREAMINFO)
		return;
	c->rate = m->data.stream_info.sample_rate;
	c->channels = (int)m->data.stream_info.channels;
	c->bits = (int)m->data.stream_info.bits_per_sample;
	printf("         FLAC %u Hz, %d ch, %d bit, %llu frames\n",
	       c->rate, c->channels, c->bits,
	       (unsigned long long)m->data.stream_info.total_samples);
}

static void flac_err(const FLAC__StreamDecoder *dec,
		     FLAC__StreamDecoderErrorStatus st, void *v)
{
	(void)dec; (void)v;
	fprintf(stderr, "x45play: FLAC error: %s\n",
		FLAC__StreamDecoderErrorStatusString[st]);
}

static FLAC__StreamDecoderWriteStatus
flac_write(const FLAC__StreamDecoder *dec, const FLAC__Frame *fr,
	   const FLAC__int32 *const buf[], void *v)
{
	struct flacctx *c = v;
	unsigned n = fr->header.blocksize;
	unsigned i;
	int ch;

	(void)dec;
	if (stop_now)
		return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;

	if (!c->started) {
		if (out_open(c->o, c->dev, c->rate, c->channels, c->bits) < 0)
			return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
		c->started = 1;
	}
	if (n > c->ibuf_frames) {
		free(c->ibuf);
		c->ibuf = malloc((size_t)n * (size_t)c->channels * 4);
		if (!c->ibuf)
			return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
		c->ibuf_frames = n;
	}

	/* libFLAC hands us planar, right-justified samples. Interleave, and for
	 * anything deeper than 16 bit left-justify into 32 so the DAC gets the
	 * full word without resampling. */
	if (c->bits <= 16) {
		int16_t *d = (int16_t *)c->ibuf;
		for (i = 0; i < n; i++)
			for (ch = 0; ch < c->channels; ch++)
				d[i * c->channels + ch] = (int16_t)buf[ch][i];
	} else {
		int32_t *d = c->ibuf;
		int shift = 32 - c->bits;
		for (i = 0; i < n; i++)
			for (ch = 0; ch < c->channels; ch++)
				d[i * c->channels + ch] =
					(int32_t)((uint32_t)buf[ch][i] << shift);
	}
	if (out_write(c->o, c->ibuf, n) < 0)
		return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
	return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

static int play_flac(const char *path, const char *dev, struct out *o)
{
	FLAC__StreamDecoder *dec = FLAC__stream_decoder_new();
	struct flacctx c;
	int rc = -1;

	if (!dec) {
		fprintf(stderr, "x45play: cannot create a FLAC decoder\n");
		return -1;
	}
	memset(&c, 0, sizeof c);
	c.o = o;
	c.dev = dev;

	printf("source   %s\n", path);
	if (FLAC__stream_decoder_init_file(dec, path, flac_write, flac_meta,
					   flac_err, &c)
	    != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
		fprintf(stderr, "x45play: %s: cannot open as FLAC\n", path);
		goto out;
	}
	if (info_only) {
		FLAC__stream_decoder_process_until_end_of_metadata(dec);
		rc = 0;
		goto out;
	}
	if (!FLAC__stream_decoder_process_until_end_of_stream(dec)) {
		fprintf(stderr, "x45play: decode stopped: %s\n",
			FLAC__StreamDecoderStateString[
				FLAC__stream_decoder_get_state(dec)]);
	}
	printf("played   %lld frames\n", o->frames_written);
	rc = 0;
out:
	free(c.ibuf);
	FLAC__stream_decoder_delete(dec);
	return rc;
}

/* ------------------------------------------------------------------- main --- */

static int ends_with(const char *s, const char *suf)
{
	size_t ls = strlen(s), lf = strlen(suf);
	return ls >= lf && !strcasecmp(s + ls - lf, suf);
}

int main(int argc, char **argv)
{
	const char *file = NULL, *dev = NULL;
	char *found = NULL;
	struct out o;
	int i, vol = -1, bringup = 0, rc;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-D") && i + 1 < argc)
			dev = argv[++i];
		else if (!strcmp(argv[i], "-v") && i + 1 < argc)
			vol = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-b"))
			bringup = 1;
		else if (!strcmp(argv[i], "-i"))
			info_only = 1;
		else if (argv[i][0] == '-') {
			puts("usage: x45play [-D dev] [-v 0-100] [-b] [-i] <file.wav|file.flac>");
			return 2;
		} else
			file = argv[i];
	}
	if (!file) {
		puts("usage: x45play [-D dev] [-v 0-100] [-b] [-i] <file.wav|file.flac>");
		return 2;
	}

	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);
	memset(&o, 0, sizeof o);

	if (!dev) {
		found = find_xmos();
		if (!found) {
			fprintf(stderr,
				"x45play: no card with usbid %s; pass -D\n",
				XMOS_USBID);
			return 1;
		}
		dev = found;
		printf("xmos     %s (usbid %s)\n", dev, XMOS_USBID);
	}

	if (bringup || vol >= 0) {
		int fd = x45_open();
		if (fd < 0) {
			fprintf(stderr, "x45play: /dev/x45: %s\n", strerror(-fd));
		} else {
			if (bringup) {
				fprintf(stderr, "x45play: bringing the board up, ~2 s\n");
				x45_bringup(fd, X45_IN_NETWORK);
			}
			if (vol >= 0) {
				x45_volume_mode(fd, 0);   /* the driver starts with the volume fixed: a level would change nothing */
				x45_volume(fd, vol);
			}
			x45_close(fd);
		}
	}

	if (ends_with(file, ".flac"))
		rc = play_flac(file, dev, &o);
	else
		rc = play_wav(file, dev, &o);

	out_close(&o);
	free(found);
	return rc < 0 ? 1 : 0;
}
