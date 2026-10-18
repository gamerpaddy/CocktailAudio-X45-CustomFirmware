/* dvd.c - the sound of a video DVD, when it is PCM or Dolby Digital. See dvd.h. */
#define _GNU_SOURCE
#include "dvd.h"
#include "a52/a52.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

static uint32_t be16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }
static uint32_t be32(const uint8_t *p) { return be16(p) << 16 | be16(p + 2); }
static int bcd(uint8_t b) { return (b >> 4) * 10 + (b & 15); }

/* playing time field of the IFO: hours, minutes, seconds, frames (the top two bits of the last byte are the
 * frame rate: 3 = 30, 1 = 25) in BCD */
static int ptime_ms(const uint8_t *p)
{
	int fps = (p[3] >> 6) == 3 ? 30 : 25;

	return (bcd(p[0]) * 3600 + bcd(p[1]) * 60 + bcd(p[2])) * 1000 + bcd(p[3] & 0x3f) * 1000 / fps;
}

/* ---------------------------------------------------------------- IFO --- */

/* the chapters of title `tt` (0-based) of a VTS_nn_0.IFO */
static int ifo_title(const uint8_t *d, size_t len, size_t ptt, size_t pgcit, int tt, int nt,
		     struct dvd_title *t)
{
	size_t from = ptt + be32(d + ptt + 8 + 4 * (size_t)tt), to;
	int npg = (int)be16(d + pgcit), k;

	to = tt + 1 < nt ? ptt + be32(d + ptt + 8 + 4 * (size_t)(tt + 1)) : ptt + be32(d + ptt + 4) + 1;
	if (from + 4 > len || to > len || to < from)
		return -1;
	t->nch = 0;
	t->ms = 0;
	/* a title has 99 chapters at most: a hostile table must not make the loop run over a megabyte of entries */
	for (k = 0; k < 99 && from + 4 * (size_t)(k + 1) <= to && t->nch < DVD_MAXCH; k++) {
		int pgcn = (int)be16(d + from + 4 * (size_t)k), pgn = (int)be16(d + from + 4 * (size_t)k + 2);
		size_t srp, p, pm, cp;
		uint32_t po;
		int nprog, ncell, entry, next, c, ms = 0;
		const uint8_t *first, *last;

		if (pgcn < 1 || pgcn > npg || pgcit + 8 + 8 * (size_t)pgcn > len)
			continue;
		srp = pgcit + 8 + 8 * (size_t)(pgcn - 1);
		po = be32(d + srp + 4);
		if (po > len || pgcit + po + 0xEC > len)   /* no sum of a disc's number and a size that can wrap */
			continue;
		p = pgcit + po;
		nprog = d[p + 2];
		ncell = d[p + 3];
		pm = p + be16(d + p + 0xE6);
		cp = p + be16(d + p + 0xE8);
		if (pgn < 1 || pgn > nprog || pm + (size_t)nprog > len || cp + 24 * (size_t)ncell > len)
			continue;
		entry = d[pm + pgn - 1];
		next = pgn < nprog ? d[pm + pgn] : ncell + 1;
		if (next <= entry)
			next = entry + 1;
		if (next > ncell + 1)
			next = ncell + 1;
		if (entry < 1 || entry > ncell)
			continue;
		first = d + cp + 24 * (size_t)(entry - 1);
		last = d + cp + 24 * (size_t)(next - 2);
		if (be32(last + 20) < be32(first + 8))
			continue;
		for (c = entry; c < next; c++) {
			ms += ptime_ms(d + cp + 24 * (size_t)(c - 1) + 4);
			if (ms > 0x3fffffff)   /* 255 cells of up to 168 hours would wrap an int */
				ms = 0x3fffffff;
		}
		t->ch[t->nch].first = (int32_t)be32(first + 8);
		t->ch[t->nch].last = (int32_t)be32(last + 20);
		t->ch[t->nch].ms = ms;
		t->ms += ms;
		if (t->ms > 0x3fffffff)
			t->ms = 0x3fffffff;
		t->nch++;
	}
	return t->nch > 0 ? 0 : -1;
}

/* Which stream is played: the first linear PCM one (16/20/24 bit, 48/96 kHz), else the Dolby Digital one with the
 * fewest channels (a disc's own stereo mix beats a downmix of the 5.1 one). Neither: with `playable` the title is
 * refused; without it stream 0 is taken, whatever it is, only to be able to say what kind of sound a disc has
 * that cannot be played. */
static int parse_ifo(const uint8_t *d, size_t len, struct dvd_title *out, int playable)
{
	size_t ptt, pgcit;
	int nast, i, nt, tt, best = -1, pcm = -1, ac3 = -1;
	struct dvd_title t, b;

	if (len < 0x300 || memcmp(d, "DVDVIDEO-VTS", 12))
		return -1;
	memset(&t, 0, sizeof t);
	nast = (int)be16(d + 0x202);
	if (nast > 8)
		nast = 8;
	t.astream = -1;
	for (i = 0; i < nast; i++) {
		const uint8_t *a = d + 0x204 + 8 * i;
		int coding = a[0] >> 5, q = a[1] >> 6, fs = (a[1] >> 4) & 3, ch = (a[1] & 7) + 1;

		if (coding == 4 && q != 3 && fs <= 1) {
			if (pcm < 0)
				pcm = i;
		} else if (coding == 0) {
			if (ac3 < 0 || ch < (d[0x204 + 8 * ac3 + 1] & 7) + 1)
				ac3 = i;
		}
	}
	if (pcm >= 0) {
		const uint8_t *a = d + 0x204 + 8 * pcm;
		int q = a[1] >> 6, fs = (a[1] >> 4) & 3;

		t.astream = pcm;
		t.fmt = 4;
		t.sub = 0xA0 + pcm;
		t.bits = q == 0 ? 16 : q == 1 ? 20 : 24;
		t.rate = fs == 0 ? 48000 : 96000;
		t.channels = (a[1] & 7) + 1;
	} else if (ac3 >= 0) {
		t.astream = ac3;
		t.fmt = 0;
		t.sub = 0x80 + ac3;
		t.bits = 0;
		t.rate = 48000;                 /* the only rate Dolby Digital has on a DVD */
		t.channels = (d[0x204 + 8 * ac3 + 1] & 7) + 1;
	} else if (!playable && nast > 0) {
		t.astream = 0;
		t.fmt = d[0x204] >> 5;
	}
	if (t.astream < 0)
		return -1;
	ptt = (size_t)be32(d + 0xC8) * DVD_SECTOR;
	pgcit = (size_t)be32(d + 0xCC) * DVD_SECTOR;
	if (ptt == 0 || pgcit == 0 || ptt + 8 > len || pgcit + 8 > len)
		return -1;
	nt = (int)be16(d + ptt);
	if (nt > 99)   /* a VTS has 99 titles at most */
		nt = 99;
	if (nt < 1 || ptt + 8 + 4 * (size_t)nt > len)
		return -1;
	memset(&b, 0, sizeof b);
	for (tt = 0; tt < nt; tt++) {
		struct dvd_title c = t;

		if (ifo_title(d, len, ptt, pgcit, tt, nt, &c) == 0 && (best < 0 || c.ms > b.ms)) {
			b = c;
			best = tt;
		}
	}
	if (best < 0)
		return -1;
	*out = b;
	return 0;
}

int dvd_parse_ifo(const uint8_t *d, size_t len, struct dvd_title *out)
{
	return parse_ifo(d, len, out, 1);
}

int dvd_parse_ifo_any(const uint8_t *d, size_t len, struct dvd_title *out)
{
	return parse_ifo(d, len, out, 0);
}

const char *dvd_fmt_name(int fmt)
{
	switch (fmt) {
	case 0:  return "Dolby Digital";
	case 2:
	case 3:  return "MPEG audio";
	case 4:  return "PCM";
	case 6:  return "DTS";
	default: return "unknown";
	}
}

/* ------------------------------------------------------------- the disc --- */

static int read_file(const char *path, uint8_t **buf, size_t *len)
{
	FILE *f = fopen(path, "rb");
	struct stat st;
	size_t n;

	if (!f)
		return -errno;
	if (fstat(fileno(f), &st) < 0 || st.st_size < 1 || st.st_size > (4 << 20)) {
		fclose(f);
		return -EINVAL;
	}
	*buf = malloc((size_t)st.st_size);
	if (!*buf) {
		fclose(f);
		return -ENOMEM;
	}
	n = fread(*buf, 1, (size_t)st.st_size, f);
	fclose(f);
	*len = n;
	return 0;
}

void dvd_close(struct dvd *d)
{
	int i;

	for (i = 0; i < d->nvob; i++)
		close(d->vobfd[i]);
	d->nvob = 0;
	if (d->mounted) {
		if (umount2(DVD_MNT, 0) < 0)
			umount2(DVD_MNT, MNT_DETACH);   /* something still holds it; let go when it does */
		d->mounted = 0;
	}
}

/* Every IFO of the disc costs about 0.6 s on the drive (two seeks), so the verdict of a scan is kept in
 * DVD_CACHE for the next start of the same disc: "<sig> <vts> <other_fmt>". The table of contents of a DVD says
 * next to nothing (this drive reports the same size for every disc), so the key also hashes the disc's own
 * VIDEO_TS.IFO, which lists all titles and where they are. */
static uint32_t disc_key(uint32_t sig)
{
	uint8_t *buf = NULL;
	size_t len = 0, i;
	uint32_t h = sig;

	if (!sig || read_file(DVD_MNT "/VIDEO_TS/VIDEO_TS.IFO", &buf, &len) < 0)
		return 0;
	for (i = 0; i < len; i++)
		h = (h ^ buf[i]) * 16777619u;
	free(buf);
	return h ? h : 1;
}

static int cache_get(uint32_t sig, int *vts, int *other_fmt)
{
	FILE *f = fopen(DVD_CACHE, "r");
	unsigned s = 0;
	int ok;

	if (!f)
		return 0;
	ok = fscanf(f, "%x %d %d", &s, vts, other_fmt) == 3 && s == sig && *vts >= 0 && *vts <= 99;
	fclose(f);
	return ok;
}

static void cache_put(uint32_t sig, int vts, int other_fmt)
{
	FILE *f = fopen(DVD_CACHE, "w");

	if (!f)
		return;
	fprintf(f, "%x %d %d\n", (unsigned)sig, vts, other_fmt);
	fclose(f);
}

/* The title with the longest playing time among VTS_`from`_0.IFO .. VTS_`to`_0.IFO that has a playable sound
 * (`best`), and the longest one whose sound is something else (`other`). Returns the number of the best VTS or 0. */
static int scan(int from, int to, struct dvd_title *best, struct dvd_title *other)
{
	int vts, bestvts = 0;
	char path[96];

	for (vts = from; vts <= to; vts++) {
		uint8_t *buf = NULL;
		size_t len = 0;
		struct dvd_title t;

		snprintf(path, sizeof path, DVD_MNT "/VIDEO_TS/VTS_%02d_0.IFO", vts);
		if (read_file(path, &buf, &len) < 0)
			continue;
		if (dvd_parse_ifo(buf, len, &t) == 0 && t.ms > best->ms) {
			*best = t;
			bestvts = vts;
		}
		if (dvd_parse_ifo_any(buf, len, &t) == 0 && t.fmt != 4 && t.fmt != 0 && t.ms > other->ms)
			*other = t;
		free(buf);
	}
	return bestvts;
}

int dvd_open(struct dvd *d, const char *dev, uint32_t sig)
{
	struct dvd_title best, other;
	int bestvts = 0, i, cached_vts = 0, cached_other = -1;
	char path[96];
	int64_t cum = 0;

	memset(d, 0, sizeof *d);
	memset(&best, 0, sizeof best);
	memset(&other, 0, sizeof other);
	d->other_fmt = -1;
	umount2(DVD_MNT, 0);          /* a player that was killed may have left it mounted */
	mkdir(DVD_MNT, 0755);
	if (mount(dev, DVD_MNT, "udf", MS_RDONLY | MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) < 0 &&
	    mount(dev, DVD_MNT, "iso9660", MS_RDONLY | MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) < 0)
		return -errno;
	d->mounted = 1;
	sig = disc_key(sig);
	if (sig && cache_get(sig, &cached_vts, &cached_other)) {
		if (cached_vts == 0) {
			/* the scan found nothing playable last time */
			if (cached_other >= 0)
				d->other_fmt = cached_other;
			dvd_close(d);
			return d->other_fmt >= 0 ? -ENOTSUP : -ENOENT;
		}
		bestvts = scan(cached_vts, cached_vts, &best, &other);
		if (!bestvts)
			unlink(DVD_CACHE);      /* the disc is not what the cache says: look at all of it */
		else
			sig = 0;
	}
	if (!bestvts)
		bestvts = scan(1, 99, &best, &other);
	if (!bestvts) {
		if (other.ms > 0)
			d->other_fmt = other.fmt;     /* a video DVD, but its sound is not PCM: say what it is */
		if (sig)
			cache_put(sig, 0, d->other_fmt);
		dvd_close(d);
		return d->other_fmt >= 0 ? -ENOTSUP : -ENOENT;
	}
	if (sig)
		cache_put(sig, bestvts, -1);
	d->t = best;
	d->vts = bestvts;
	for (i = 0; i < 9; i++) {
		struct stat st;
		int fd;

		snprintf(path, sizeof path, DVD_MNT "/VIDEO_TS/VTS_%02d_%d.VOB", bestvts, i + 1);
		fd = open(path, O_RDONLY | O_CLOEXEC);
		if (fd < 0)
			break;
		if (fstat(fd, &st) < 0) {
			close(fd);
			break;
		}
		d->vobfd[i] = fd;
		d->vobsec[i] = cum;
		cum += st.st_size / DVD_SECTOR;
		d->nvob = i + 1;
	}
	d->vobsec[d->nvob] = cum;
	if (d->nvob == 0) {
		dvd_close(d);
		return -ENOENT;
	}
	return d->t.nch;
}

int dvd_read(struct dvd *d, int32_t sector, int n, uint8_t *buf)
{
	int done = 0;

	while (done < n) {
		int64_t s = (int64_t)sector + done, avail;
		int i, m;
		ssize_t r;

		for (i = 0; i < d->nvob; i++)
			if (s < d->vobsec[i + 1])
				break;
		if (i == d->nvob)
			break;
		avail = d->vobsec[i + 1] - s;
		m = n - done < avail ? n - done : (int)avail;
		r = pread(d->vobfd[i], buf + (size_t)done * DVD_SECTOR, (size_t)m * DVD_SECTOR,
			  (off_t)((s - d->vobsec[i]) * DVD_SECTOR));
		if (r < 0)
			return done ? done : -errno;
		if (r < DVD_SECTOR)
			break;
		done += (int)(r / DVD_SECTOR);
		if (r % DVD_SECTOR)
			break;
	}
	return done;
}

/* ------------------------------------------------------------ LPCM ----- */

void dvd_pcm_init(struct dvd_pcm *p, const struct dvd_title *t)
{
	memset(p, 0, sizeof *p);
	p->sub = t->sub;
	p->fmt = t->fmt;
	if (p->fmt == 0)
		p->a52 = a52_init(0);
}

void dvd_pcm_done(struct dvd_pcm *p)
{
	if (p->a52)
		a52_free(p->a52);
	p->a52 = NULL;
}

/* One group of samples: 16 bit is one frame (a sample for every channel, big endian); 20 and 24 bit come in
 * pairs of frames, the top 16 bits of all of them first, then the rest (a byte for 24 bit, a nibble for 20). */
static int group_decode(const uint8_t *g, int bits, int ch, int32_t *out)
{
	int fr = bits == 16 ? 1 : 2, f, c;

	for (f = 0; f < fr; f++) {
		int32_t v[8] = { 0 };

		for (c = 0; c < ch; c++) {
			uint32_t hi = be16(g + 2 * ((size_t)f * ch + c)), s;

			if (bits == 16) {
				s = hi << 16;
			} else if (bits == 24) {
				s = hi << 16 | (uint32_t)g[4 * ch + f * ch + c] << 8;
			} else {
				int idx = f * ch + c;
				uint32_t nib = g[4 * ch + idx / 2];

				nib = idx & 1 ? nib & 15 : nib >> 4;
				s = hi << 16 | nib << 12;
			}
			v[c] = (int32_t)s;
		}
		out[2 * f] = v[0];
		out[2 * f + 1] = ch > 1 ? v[1] : v[0];
	}
	return fr;
}

static int lpcm_feed(struct dvd_pcm *p, const uint8_t *in, int n, int32_t *out, int cap)
{
	int ch = p->channels, bits = p->bits;
	int grp = bits == 16 ? 2 * ch : bits == 20 ? 5 * ch : 6 * ch;
	int fr = bits == 16 ? 1 : 2, nout = 0;

	if (p->npend) {
		int take = grp - p->npend;

		if (take > n)
			take = n;
		memcpy(p->pend + p->npend, in, (size_t)take);
		p->npend += take;
		in += take;
		n -= take;
		if (p->npend < grp)
			return 0;
		if (cap < fr)
			return 0;
		nout += group_decode(p->pend, bits, ch, out);
		p->npend = 0;
	}
	while (n >= grp && nout + fr <= cap) {
		nout += group_decode(in, bits, ch, out + 2 * nout);
		in += grp;
		n -= grp;
	}
	if (n > 0 && n < grp) {
		memcpy(p->pend, in, (size_t)n);
		p->npend = n;
	}
	return nout;
}

/* ------------------------------------------------------- Dolby Digital ----- */

static int32_t f2s32(float x)
{
	double v = (double)x * 2147483648.0;

	return v >= 2147483647.0 ? INT32_MAX : v <= -2147483648.0 ? INT32_MIN : (int32_t)v;
}

/* One complete frame in p->in: decode its 6 blocks into p->q (stereo; a bad frame is a frame of silence, so the
 * timing holds). Dynamic range compression is off: this is for listening in a room, not at night. */
static void ac3_decode(struct dvd_pcm *p)
{
	a52_state_t *st = p->a52;
	int flags = A52_STEREO | A52_ADJUST_LEVEL, blk, i;
	sample_t level = 1;

	p->qn = DVD_AC3_FRAMES;
	p->qpos = 0;
	memset(p->q, 0, sizeof p->q);
	if (a52_frame(st, p->in, &flags, &level, 0) != 0)
		return;
	a52_dynrng(st, NULL, NULL);
	for (blk = 0; blk < 6; blk++) {
		const sample_t *s;

		if (a52_block(st) != 0)
			return;
		s = a52_samples(st);
		for (i = 0; i < 256; i++) {
			p->q[2 * (blk * 256 + i)] = f2s32(s[i]);
			p->q[2 * (blk * 256 + i) + 1] = f2s32(s[256 + i]);
		}
	}
}

/* Decodes the next whole frame of p->in into p->q; 0 when there is none yet. */
static int ac3_next(struct dvd_pcm *p)
{
	int flags, sr, br, size;

	if (!p->synced || p->nin < 7)
		return 0;
	size = a52_syncinfo(p->in, &flags, &sr, &br);
	if (size == 0) {                    /* not the start of a frame: wait for the next packet's pointer */
		p->synced = 0;
		p->nin = 0;
		return 0;
	}
	if (p->nin < size)
		return 0;
	ac3_decode(p);
	memmove(p->in, p->in + size, (size_t)(p->nin - size));
	p->nin -= size;
	return 1;
}

static int ac3_drain(struct dvd_pcm *p, int32_t *out, int cap)
{
	int n = 0;

	if (!p->a52)
		return 0;
	while (n < cap) {
		if (p->qpos >= p->qn && !ac3_next(p))
			break;
		{
			int take = p->qn - p->qpos < cap - n ? p->qn - p->qpos : cap - n;

			memcpy(out + 2 * (size_t)n, p->q + 2 * (size_t)p->qpos, (size_t)take * 2 * sizeof(int32_t));
			p->qpos += take;
			n += take;
		}
	}
	return n;
}

/* A packet of the Dolby Digital substream: 4 header bytes (substream, number of frames starting in it, and a
 * pointer to the first of them, counted from the last byte of the pointer), then the bytes of the frames. */
static void ac3_feed(struct dvd_pcm *p, const uint8_t *h, int n)
{
	int fau = (int)be16(h + 2);
	const uint8_t *data = h + 4;

	n -= 4;
	if (n <= 0)
		return;
	if (!p->synced) {
		if (fau < 1 || fau - 1 >= n)    /* no frame starts here: nothing to join the bytes to */
			return;
		data += fau - 1;
		n -= fau - 1;
		p->nin = 0;
		p->synced = 1;
	}
	if (p->nin + n > DVD_AC3_IN) {         /* far behind: start over at the next frame start */
		p->synced = 0;
		p->nin = 0;
		return;
	}
	memcpy(p->in + p->nin, data, (size_t)n);
	p->nin += n;
}

int dvd_pcm_flush(struct dvd_pcm *p, int32_t *out, int cap)
{
	return p->fmt == 0 ? ac3_drain(p, out, cap) : 0;
}

int dvd_pcm_sector(struct dvd_pcm *p, const uint8_t *s, int32_t *out, int cap)
{
	int pos, nout = 0;

	if (memcmp(s, "\0\0\1\xba", 4) || (s[4] & 0xc0) != 0x40)      /* an MPEG-2 pack */
		return 0;
	pos = 14 + (s[13] & 7);
	while (pos + 6 <= DVD_SECTOR && s[pos] == 0 && s[pos + 1] == 0 && s[pos + 2] == 1) {
		int id = s[pos + 3], len = (int)be16(s + pos + 4), end = pos + 6 + len;

		if (end > DVD_SECTOR)
			end = DVD_SECTOR;
		if (id == 0xBD && len > 9 && pos + 9 <= DVD_SECTOR && p->fmt == 0) {
			int d0 = pos + 9 + s[pos + 8];

			if (d0 + 4 <= end && s[d0] == p->sub)
				ac3_feed(p, s + d0, end - d0);
		} else if (id == 0xBD && len > 9 && pos + 9 <= DVD_SECTOR) {
			int d0 = pos + 9 + s[pos + 8];      /* the byte after the PES header: the substream */

			if (d0 + 7 <= end && s[d0] == p->sub) {
				int q = s[d0 + 5] >> 6, ch = (s[d0 + 5] & 7) + 1;
				int bits = q == 0 ? 16 : q == 1 ? 20 : 24;

				if (q == 3 || ch > 8) {
					pos = end;
					continue;
				}
				if (bits != p->bits || ch != p->channels)
					p->npend = 0;       /* a different layout: what was kept is not part of it */
				p->bits = bits;
				p->channels = ch;
				nout += lpcm_feed(p, s + d0 + 7, end - (d0 + 7), out + 2 * nout, cap - nout);
			}
		}
		pos = end;
	}
	if (p->fmt == 0)
		return ac3_drain(p, out, cap);
	return nout;
}
