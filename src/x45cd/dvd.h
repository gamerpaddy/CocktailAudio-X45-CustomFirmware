/* dvd.h - the sound of a video DVD, when it is plain PCM or Dolby Digital.
 *
 * The disc is mounted read-only (UDF, or ISO 9660) and read as files: VIDEO_TS/VTS_nn_0.IFO says where the
 * chapters are and what the audio streams are; the VOB files hold the MPEG program stream. Only one audio
 * stream of the longest title is played: the first linear PCM stream (16, 20 or 24 bit, 48 or 96 kHz), else
 * the Dolby Digital stream with the fewest channels (decoded with liba52, a52/, and mixed down to stereo).
 * DTS, MPEG audio and scrambled (CSS) discs are not handled: there is no decoder and no descrambler here. */
#ifndef X45_DVD_H
#define X45_DVD_H

#include <stddef.h>
#include <stdint.h>

#define DVD_MAXCH  99
#define DVD_MNT    "/tmp/x45dvd"
#define DVD_CACHE  "/tmp/x45dvd.cache"
#define DVD_SECTOR 2048

struct dvd_chapter {
	int32_t first, last;        /* sectors in the VOB set of the title set (VTS_nn_1.VOB, _2, ...) */
	int ms;                     /* playing time */
};

struct dvd_title {
	int nch;
	struct dvd_chapter ch[DVD_MAXCH];
	int ms;                     /* the whole title */
	int astream;                /* number of the audio stream in the IFO */
	int fmt;                    /* coding of that stream: 0 Dolby Digital, 2/3 MPEG, 4 PCM, 6 DTS */
	int sub;                    /* its substream id in private stream 1: 0xA0 + astream (PCM), 0x80 + astream (AC3) */
	int bits, rate, channels;   /* of that stream, from the IFO (bits: PCM only, 0 for Dolby Digital) */
};

/* Reads the title with the longest playing time that has a playable audio stream (PCM, else Dolby Digital) out
 * of one VTS_nn_0.IFO (`d`, `len` bytes). Returns 0, or -1 when there is no such title. Pure: no files, no mount. */
int dvd_parse_ifo(const uint8_t *d, size_t len, struct dvd_title *t);
/* The same, but a title whose first stream is neither also counts (fmt says what it is). */
int dvd_parse_ifo_any(const uint8_t *d, size_t len, struct dvd_title *t);
const char *dvd_fmt_name(int fmt);

struct dvd {
	struct dvd_title t;
	int other_fmt;              /* after a failed dvd_open that returned -ENOTSUP: the coding of the sound */
	int vts;
	int mounted;
	int nvob;
	int vobfd[9];
	int64_t vobsec[10];         /* first sector of each VOB file in the set; [nvob] is the end */
};

/* Mounts `dev`, finds the best title. Returns the chapter count, or a negative errno.
 * `sig` is a hint for the disc (0: none; the disc's own VIDEO_TS.IFO is mixed in); the verdict of a full scan is kept for the same `sig`, which saves
 * the second start of a disc a few seconds. */
int dvd_open(struct dvd *d, const char *dev, uint32_t sig);
void dvd_close(struct dvd *d);      /* closes the files and unmounts */
/* `n` sectors from `sector` on (set numbering) into buf; returns the number read, or a negative errno. */
int dvd_read(struct dvd *d, int32_t sector, int n, uint8_t *buf);

#define DVD_AC3_FRAMES 1536     /* samples per channel in a Dolby Digital frame */
#define DVD_AC3_IN     8192     /* input kept for a frame that is not complete yet */

struct dvd_pcm {
	int sub;                    /* substream id wanted */
	int fmt;                    /* 4 PCM, 0 Dolby Digital */
	int bits, channels;         /* PCM: of the packets seen last */
	uint8_t pend[64];           /* PCM: the start of a sample group that a packet ended in */
	int npend;
	void *a52;                  /* Dolby Digital: the liba52 state */
	uint8_t in[DVD_AC3_IN];     /* Dolby Digital: bytes of the frame being collected */
	int nin, synced;
	int32_t q[DVD_AC3_FRAMES * 2];  /* Dolby Digital: the last decoded frame, stereo, not yet handed out */
	int qn, qpos;
};

/* Starts a stream of `t` (a chapter): call it for every chapter, and dvd_pcm_done when finished with it. */
void dvd_pcm_init(struct dvd_pcm *p, const struct dvd_title *t);
void dvd_pcm_done(struct dvd_pcm *p);
/* Feeds one 2048-byte sector of the program stream. Decoded stereo frames (32-bit, left justified, channels
 * beyond the second dropped or mixed down, mono doubled) go to `out`, at most `cap`. Returns the frames written.
 * A Dolby Digital frame is 1536 frames long: what does not fit stays inside, so call dvd_pcm_flush after it
 * until it returns 0. */
int dvd_pcm_sector(struct dvd_pcm *p, const uint8_t *sector, int32_t *out, int cap);
int dvd_pcm_flush(struct dvd_pcm *p, int32_t *out, int cap);

#endif
