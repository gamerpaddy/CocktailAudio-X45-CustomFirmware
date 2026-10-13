/* x45cd - audio CD player for the built-in drive.
 *
 *   x45cd toc [dev]            print the table of contents
 *   x45cd play <track> [dev]   play from <track> to the end of the disc
 *   x45cd text [dev]           CD-Text: "T|P <track> <text>" (0 = album)
 *   x45cd eject|close [dev]    open / close the tray
 *   x45cd id [dev]             "<ntracks> <frames>" of the disc in the drive (exit 3: none)
 *
 * A video DVD (a disc with only a data track and a VIDEO_TS folder whose sound is plain PCM) is listed by
 * toc as "<n> <frames> dvd <chapters> <Hz> <bits>" and the chapters, and play <chapter> plays their sound (dvd.h).
 *   x45cd hdcd <track> [seconds] [dev]   look for HDCD codes, print what was found
 *
 * toc output:   "<ntracks> <total_frames>" then "<n> <audio|data> <frames>"
 * per track (75 frames = 1 second).
 *
 * The drive is read with the kernel's CDROMREADAUDIO ioctl and written to the
 * DAC as 44.1 kHz stereo, the same output path MPD uses. The caller must
 * have MPD stopped (the DAC is single-owner).
 *
 * HDCD: the stream goes through the HDCD decoder (hdcd.c). The DAC is opened
 * with 32-bit samples so that peak extension has room. A plain disc comes out
 * as the exact 16-bit data shifted up by 16 bits. Once HDCD codes that change
 * the sound (peak extend or gain) have been seen, the level drops 6 dB to the
 * decoder's own scale: the stretched peaks of such a disc reach twice the
 * plain full scale. The first track is scanned for 3 s before it plays so
 * the level is right from the first sample; if the codes show up later the
 * level glides down over 3 s. X45CD_HDCD=off turns the decoder off,
 * X45CD_HDCD=loud keeps the plain level (stretched peaks then clip).
 *
 * Control while playing:  SIGUSR1 pause/resume   SIGUSR2 next track
 *                         SIGHUP previous track  SIGTERM/SIGINT stop
 * State: /tmp/x45cd.status, rewritten about four times a second:
 *        "<state> <track> <pos_seconds> <track_seconds> <ntracks>
 *         <hdcd> <peak_extend> <max_gain> <active>"
 *        state = play | pause | stop
 *        hdcd = 0 none seen, 1 codes seen that change nothing, 2 HDCD in effect
 *        peak_extend = 0 never, 1 sometimes, 2 always (in the packets seen)
 *        max_gain = largest attenuation signalled, in 0.5 dB steps
 *        active = 1 while codes are arriving
 */
#define _GNU_SOURCE
#include <alsa/asoundlib.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <sys/mount.h>

#include "../libx45/x45.h"
#include "dvd.h"
#include "hdcd.h"

/* <linux/cdrom.h> pieces, spelled out so the build does not depend on the
 * toolchain's kernel headers */
#define CDROMREADTOCHDR   0x5305
#define CDROMREADTOCENTRY 0x5306
#define CDROMREADAUDIO    0x530e
#define CDROM_MEDIA_CHANGED 0x5325
#define CDROM_DRIVE_STATUS  0x5326
#define CDSL_CURRENT      0x7fffffff
#define CDS_DISC_OK       4
#define CDROM_LBA         0x01
#define CDROM_LEADOUT     0xAA
#define CD_FRAMESIZE_RAW  2352
#define CD_AUDIO_TRACK    0x00
#define CD_DATA_TRACK     0x04

struct cd_tochdr { uint8_t first, last; };
struct cd_tocentry {
	uint8_t track;
	uint8_t adr_ctrl;     /* adr in the low nibble, ctrl in the high */
	uint8_t format;
	int32_t lba;          /* union cdrom_addr: lba or msf, 4 bytes */
	uint8_t datamode;
};
struct cd_readaudio {
	int32_t addr;
	uint8_t addr_format;
	int nframes;
	uint8_t *buf;
};

#define MAXTRK 100
#define CHUNK  13            /* frames per ioctl: 13 x 2352 = 30 KB */
#define STATUS "/tmp/x45cd.status"
#define PCM    "hw:CocktailAudio,0"

static volatile sig_atomic_t g_pause_toggle, g_next, g_prev, g_stop;
static void on_usr1(int s) { (void)s; g_pause_toggle = 1; }
static void on_usr2(int s) { (void)s; g_next = 1; }
static void on_hup(int s)  { (void)s; g_prev = 1; }
static void on_term(int s) { (void)s; g_stop = 1; }

struct toc {
	int first, last, n;
	int32_t start[MAXTRK + 2];   /* lba of track i; start[last+1] = leadout */
	int audio[MAXTRK + 2];
};

/* a number that tells discs apart: the layout of the table of contents */
static uint32_t toc_sig(const struct toc *t)
{
	uint32_t h = 2166136261u;
	int i;

	h = (h ^ (uint32_t)t->n) * 16777619u;
	for (i = t->first; i <= t->last + 1; i++)
		h = (h ^ (uint32_t)t->start[i]) * 16777619u;
	return h ? h : 1;
}

static int read_toc(int fd, struct toc *t)
{
	struct cd_tochdr h;
	struct cd_tocentry e;
	int i;

	if (ioctl(fd, CDROMREADTOCHDR, &h) < 0)
		return -errno;
	if (h.last >= MAXTRK)
		return -EINVAL;
	t->first = h.first;
	t->last = h.last;
	t->n = h.last - h.first + 1;
	for (i = h.first; i <= h.last + 1; i++) {
		memset(&e, 0, sizeof e);
		e.track = (uint8_t)(i <= h.last ? i : CDROM_LEADOUT);
		e.format = CDROM_LBA;
		if (ioctl(fd, CDROMREADTOCENTRY, &e) < 0)
			return -errno;
		t->start[i] = e.lba;
		t->audio[i] = !(e.adr_ctrl & (CD_DATA_TRACK << 4));
	}
	return 0;
}

/* ---------------------------------------------------------------- HDCD --- */
#define SECTOR_PCM_FRAMES (CD_FRAMESIZE_RAW / 4)    /* 588 stereo frames per sector */
#define PRESCAN_SECTORS   (3 * 75)
#define K_PLAIN    65536     /* out = decoded * K >> 15: 2 is the plain CD level */
#define K_HEADROOM 32768     /* 1 is the decoder's own scale, one bit of headroom */
#define RAMP_STEP  63        /* Q8 per frame: 6 dB (2 -> 1) in 3 s at 44.1 kHz */

static struct hdcd g_hd;
static struct hdcd_info g_hi;
static int g_hdcd_on = 1, g_hdcd_loud;
static int g_disc_hdcd;                 /* HDCD in effect has been seen on this disc */
static int g_kq = K_PLAIN << 8;         /* current output scale, Q8 */

static void hdcd_env(void)
{
	const char *e = getenv("X45CD_HDCD");

	if (e && !strcmp(e, "off"))
		g_hdcd_on = 0;
	else if (e && !strcmp(e, "loud"))
		g_hdcd_loud = 1;
}

static int hdcd_target(void)
{
	return (g_disc_hdcd && !g_hdcd_loud ? K_HEADROOM : K_PLAIN) << 8;
}

/* a new track (or a restart of one): fresh decoder state, level as the disc had it */
static void hdcd_track_start(void)
{
	hdcd_init(&g_hd, 44100, 2000);
	memset(&g_hi, 0, sizeof g_hi);
	g_kq = hdcd_target();
}

/* One chunk of CD data (16-bit little endian stereo, `sectors` x 588 frames) to
 * the DAC's 32-bit format: decoded, then scaled to the level policy above. */
static int hdcd_convert(const int16_t *in, int sectors, int32_t *out)
{
	int pf = sectors * SECTOR_PCM_FRAMES, i, c;
	int tgt;

	for (i = 0; i < pf * 2; i++)
		out[i] = in[i];
	if (g_hdcd_on) {
		hdcd_decode(&g_hd, out, pf);
		hdcd_info(&g_hd, &g_hi);
		if (g_hi.detected == HDCD_EFFECTUAL && !g_disc_hdcd) {
			g_disc_hdcd = 1;
			fprintf(stderr, "x45cd: HDCD in effect (peak extend %d, gain %d.%d dB), level %s\n",
				g_hi.peak_extend, g_hi.max_gain / 2, g_hi.max_gain % 2 * 5,
				g_hdcd_loud ? "kept" : "gliding down 6 dB");
		}
	} else {
		for (i = 0; i < pf * 2; i++)
			out[i] *= 32768;
	}
	tgt = hdcd_target();
	for (i = 0; i < pf; i++) {
		int k;

		if (g_kq > tgt) {
			g_kq -= RAMP_STEP;
			if (g_kq < tgt)
				g_kq = tgt;
		}
		k = g_kq >> 8;
		if (k == K_HEADROOM)
			continue;
		for (c = 0; c < 2; c++) {
			int64_t v = ((int64_t)out[i * 2 + c] * k) >> 15;

			out[i * 2 + c] = v > INT32_MAX ? INT32_MAX : v < INT32_MIN ? INT32_MIN : (int32_t)v;
		}
	}
	return pf;
}

static void status(const char *state, int track, long pos, long len, int n)
{
	char tmp[] = STATUS ".tmp";
	FILE *f = fopen(tmp, "w");

	if (!f)
		return;
	int det = g_hi.detected;

	if (g_disc_hdcd && det < HDCD_EFFECTUAL)
		det = HDCD_EFFECTUAL;       /* a track that has not shown its codes yet */
	fprintf(f, "%s %d %ld %ld %d %d %d %d %d\n", state, track, pos, len, n,
		det, g_hi.peak_extend, g_hi.max_gain, g_hi.active);
	fclose(f);
	rename(tmp, STATUS);
}

static int open_pcm(snd_pcm_t **pcm, unsigned rate)
{
	snd_pcm_hw_params_t *hw;
	snd_pcm_uframes_t buf = rate / 2; /* half a second */
	int e, tries;

	x45_clock_select(rate);
	for (tries = 0; tries < 25; tries++) {
		e = snd_pcm_open(pcm, PCM, SND_PCM_STREAM_PLAYBACK, 0);
		if (e != -EBUSY)
			break;
		usleep(200000);
	}
	if (e < 0)
		return e;
	snd_pcm_hw_params_alloca(&hw);
	snd_pcm_hw_params_any(*pcm, hw);
	if ((e = snd_pcm_hw_params_set_access(*pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0 ||
	    (e = snd_pcm_hw_params_set_format(*pcm, hw, SND_PCM_FORMAT_S32_LE)) < 0 ||
	    (e = snd_pcm_hw_params_set_channels(*pcm, hw, 2)) < 0 ||
	    (e = snd_pcm_hw_params_set_rate_near(*pcm, hw, &rate, 0)) < 0)
		goto fail;
	snd_pcm_hw_params_set_buffer_size_near(*pcm, hw, &buf);
	if ((e = snd_pcm_hw_params(*pcm, hw)) < 0)
		goto fail;
	return 0;
fail:
	snd_pcm_close(*pcm);
	*pcm = NULL;
	return e;
}

static int any_audio(const struct toc *t)
{
	int i;

	for (i = t->first; i <= t->last; i++)
		if (t->audio[i])
			return 1;
	return 0;
}

/* Where the sound of a track ends. On an Enhanced CD the data track is a second session, and the table of contents
 * counts the 11400 sectors between the two (lead-out, lead-in, pre-gap) to the last audio track: nothing can be read
 * there. An audio track that is followed by a data track ends that far before it, as the CD rippers have it. */
#define SESSION_GAP 11400

static int32_t track_end(const struct toc *t, int trk)
{
	int32_t end = t->start[trk + 1];

	if (t->audio[trk] && trk < t->last && !t->audio[trk + 1] && end - t->start[trk] > SESSION_GAP + 75)
		end -= SESSION_GAP;
	return end;
}

static int cmd_toc(const char *dev)
{
	struct toc t;
	int fd = open(dev, O_RDONLY | O_NONBLOCK), i, r;

	if (fd < 0) {
		perror(dev);
		return 2;
	}
	if ((r = read_toc(fd, &t)) < 0) {
		fprintf(stderr, "x45cd: no disc / no toc: %s\n", strerror(-r));
		return 3;
	}
	if (!any_audio(&t)) {
		/* a video DVD whose sound is plain PCM plays as a list of chapters:
		 * "<n> <frames> dvd <chapters> <Hz> <bits>", then "<k> audio <75 x seconds>" for each */
		struct dvd d;
		int nch = dvd_open(&d, dev, toc_sig(&t));

		if (nch > 0) {
			printf("%d %d dvd %d %d %d\n", t.n, (int)(t.start[t.last + 1] - t.start[t.first]), nch,
			       d.t.rate, d.t.bits);
			for (i = 0; i < nch; i++)
				printf("%d audio %d\n", i + 1, (d.t.ch[i].ms + 500) / 1000 * 75);
			dvd_close(&d);
			close(fd);
			return 0;
		}
		if (nch == -ENOTSUP) {
			/* a video DVD whose sound is DTS / MPEG audio: "<n> <frames> dvdx <name>", then the data
			 * track as usual. The name has underscores for spaces. */
			char name[24], *q;

			snprintf(name, sizeof name, "%s", dvd_fmt_name(d.other_fmt));
			for (q = name; *q; q++)
				if (*q == ' ')
					*q = '_';
			printf("%d %d dvdx %s\n", t.n, (int)(t.start[t.last + 1] - t.start[t.first]), name);
			for (i = t.first; i <= t.last; i++)
				printf("%d %s %d\n", i, t.audio[i] ? "audio" : "data",
				       (int)(track_end(&t, i) - t.start[i]));
			close(fd);
			return 0;
		}
	}
	printf("%d %d\n", t.n, (int)(t.start[t.last + 1] - t.start[t.first]));
	for (i = t.first; i <= t.last; i++)
		printf("%d %s %d\n", i, t.audio[i] ? "audio" : "data",
		       (int)(track_end(&t, i) - t.start[i]));
	close(fd);
	return 0;
}

/* Read `n` sectors at `lba` into buf; a sector the drive cannot read is silence
 * (and the call returns -1). */
static int read_sectors(int fd, int32_t lba, int n, uint8_t *buf)
{
	struct cd_readaudio ra;

	ra.addr = lba;
	ra.addr_format = CDROM_LBA;
	ra.nframes = n;
	ra.buf = buf;
	if (ioctl(fd, CDROMREADAUDIO, &ra) < 0) {
		fprintf(stderr, "x45cd: read error at %d: %s\n", (int)lba, strerror(errno));
		memset(buf, 0, (size_t)n * CD_FRAMESIZE_RAW);
		return -1;
	}
	return 0;
}

/* what the disc that was started looked like: number of tracks and length in frames */
static int g_sig_n, g_sig_total;

/* The disc that was playing is gone: the tray is open or empty, or another disc is in. */
static int disc_gone(int fd)
{
	int st = ioctl(fd, CDROM_DRIVE_STATUS, CDSL_CURRENT), r;
	struct toc t;

	if (st >= 0 && st != CDS_DISC_OK) {
		fprintf(stderr, "x45cd: drive status %d\n", st);
		return 1;
	}
	if (ioctl(fd, CDROM_MEDIA_CHANGED, CDSL_CURRENT) != 1)
		return 0;
	/* The flag also comes up for a disc that stayed (a unit attention after the drive idled, or a mount),
	 * so it only counts when the disc's table of contents is not the one that was started. */
	if ((r = read_toc(fd, &t)) < 0) {
		fprintf(stderr, "x45cd: drive reports a media change, no table of contents: %s\n", strerror(-r));
		return 1;
	}
	if (g_sig_n && (t.n != g_sig_n || t.start[t.last + 1] - t.start[t.first] != g_sig_total)) {
		fprintf(stderr, "x45cd: drive reports a media change, and it is another disc\n");
		return 1;
	}
	fprintf(stderr, "x45cd: drive reports a media change, same disc: ignored\n");
	return 0;
}

/* Run the first seconds of a track through the decoder, without playing them.
 * Returns 1 as soon as the HDCD codes are seen to change the sound. */
static int hdcd_prescan(int fd, int32_t lba, int32_t end, int sectors)
{
	static uint8_t raw[CHUNK * CD_FRAMESIZE_RAW] __attribute__((aligned(4)));
	static int32_t out[CHUNK * SECTOR_PCM_FRAMES * 2];
	int done = 0;

	hdcd_init(&g_hd, 44100, 2000);
	if (end - lba < sectors)
		sectors = (int)(end - lba);
	while (done < sectors) {
		int n = sectors - done < CHUNK ? sectors - done : CHUNK;
		const int16_t *in = (const int16_t *)raw;
		int i, pf = n * SECTOR_PCM_FRAMES;

		read_sectors(fd, lba + done, n, raw);
		for (i = 0; i < pf * 2; i++)
			out[i] = in[i];
		hdcd_decode(&g_hd, out, pf);
		done += n;
		hdcd_info(&g_hd, &g_hi);
		if (g_hi.detected == HDCD_EFFECTUAL)
			return 1;
	}
	return 0;
}

static int sg_prevent(int fd, int prevent);
static int cmd_play_dvd(int track, const char *dev, int fd, uint32_t sig);

static int cmd_play(int track, const char *dev)
{
	struct toc t;
	snd_pcm_t *pcm = NULL;
	static uint8_t buf[CHUNK * CD_FRAMESIZE_RAW] __attribute__((aligned(4)));
	static int32_t out32[CHUNK * SECTOR_PCM_FRAMES * 2];
	int fd, r, paused = 0, rc = 0, bad = 0;
	long lastst = 0, gone_t = 0;
	pid_t parent = getppid();
	int follow = getenv("X45CD_FOLLOW_PARENT") != NULL;

	fd = open(dev, O_RDONLY | O_NONBLOCK);
	if (fd < 0) {
		perror(dev);
		return 2;
	}
	if ((r = read_toc(fd, &t)) < 0) {
		fprintf(stderr, "x45cd: no disc / no toc: %s\n", strerror(-r));
		return 3;
	}
	g_sig_n = t.n;
	g_sig_total = (int)(t.start[t.last + 1] - t.start[t.first]);
	if (!any_audio(&t)) {
		int dr = cmd_play_dvd(track, dev, fd, toc_sig(&t));

		close(fd);
		return dr;
	}
	if (track < t.first || track > t.last)
		track = t.first;
	while (track <= t.last && !t.audio[track])
		track++;
	if (track > t.last) {
		fprintf(stderr, "x45cd: no audio track\n");
		return 4;
	}
	/* The door stays unlocked while the disc plays, so the drive's own eject button works:
	 * the drive ignores it while the host has asked it to prevent medium removal. */
	sg_prevent(fd, 0);
	status("play", track, 0, (track_end(&t, track) - t.start[track]) / 75, t.n);   /* not the last disc's numbers */
	hdcd_env();
	if (g_hdcd_on && !g_hdcd_loud &&
	    hdcd_prescan(fd, t.start[track], track_end(&t, track), PRESCAN_SECTORS)) {
		g_disc_hdcd = 1;
		fprintf(stderr, "x45cd: HDCD found in the first seconds of track %d\n", track);
	}
	if ((r = open_pcm(&pcm, 44100)) < 0) {
		fprintf(stderr, "x45cd: open %s: %s\n", PCM, snd_strerror(r));
		return 5;
	}

	signal(SIGUSR1, on_usr1);
	signal(SIGUSR2, on_usr2);
	signal(SIGHUP, on_hup);
	signal(SIGTERM, on_term);
	signal(SIGINT, on_term);

	while (!g_stop && track <= t.last) {
		int32_t lba = t.start[track], end = track_end(&t, track);
		long len = (end - lba) / 75;
		int jump = 0;

		if (!t.audio[track]) {
			track++;
			continue;
		}
		hdcd_track_start();
		sg_prevent(fd, 0);      /* whoever else opened the drive may have locked it again */
		while (lba < end && !g_stop && !jump) {
			int n = end - lba < CHUNK ? (int)(end - lba) : CHUNK;
			snd_pcm_sframes_t w;
			uint8_t *p = (uint8_t *)out32;
			long now_s;

			if (follow && getppid() != parent)
				g_stop = 1;
			if (g_stop)
				break;
			if (g_next) {
				g_next = 0;
				jump = 1;
				break;
			}
			if (g_prev) {
				g_prev = 0;
				/* more than 3 s in: restart this track, else go back one */
				if (lba - t.start[track] > 3 * 75 || track == t.first)
					lba = t.start[track];
				else
					track--;
				jump = -1;
				break;
			}
			if (g_pause_toggle) {
				g_pause_toggle = 0;
				paused = !paused;
				if (paused) {
					snd_pcm_drop(pcm);
				} else {
					snd_pcm_prepare(pcm);
				}
			}
			if (paused) {
				status("pause", track, (lba - t.start[track]) / 75, len, t.n);
				usleep(100000);
				if (time(NULL) != gone_t) {      /* nothing is read while paused: look once a second */
					gone_t = time(NULL);
					if (disc_gone(fd)) {
						fprintf(stderr, "x45cd: disc removed\n");
						g_stop = 1;
						rc = 7;
					}
				}
				continue;
			}

			if (read_sectors(fd, lba, n, buf) < 0) {
				/* one bad sector is a scratch (silence); a drive with no disc fails every read */
				if (++bad >= 2 && disc_gone(fd)) {
					fprintf(stderr, "x45cd: disc removed\n");
					g_stop = 1;
					rc = 7;
					break;
				}
				if (bad >= 40) {
					fprintf(stderr, "x45cd: the disc cannot be read any more\n");
					g_stop = 1;
					rc = 7;
					break;
				}
			} else {
				bad = 0;
			}
			lba += n;
			r = hdcd_convert((const int16_t *)buf, n, out32);

			while (r > 0 && !g_stop) {
				w = snd_pcm_writei(pcm, p, (snd_pcm_uframes_t)r);
				if (w < 0) {
					if (w == -EINTR)
						continue;
					w = snd_pcm_recover(pcm, (int)w, 1);
					if (w < 0) {
						fprintf(stderr, "x45cd: write: %s\n",
							snd_strerror((int)w));
						rc = 6;
						goto out;
					}
					continue;
				}
				p += w * 8;
				r -= (int)w;
			}
			{
				/* something flips the clock-family pin behind our back
				 * (right after the stream opens, and sometimes later);
				 * put it right after every chunk and note it */
				int before = x45_clock_get();

				x45_clock_select(44100);
				if (before >= 0 && before != x45_clock_get())
					fprintf(stderr, "x45cd: clock family had flipped (was %d) at track %d, frame %d\n",
						before, track, (int)(lba - t.start[track]));
			}
			now_s = (lba - t.start[track]) / 75;
			if (now_s != lastst) {
				lastst = now_s;
				status("play", track, now_s, len, t.n);
			}
		}
		if (g_hi.packets || g_hi.errors)
			fprintf(stderr, "x45cd: track %d: hdcd %d, %d packets, peak extend %d, gain %d.%d dB, %d errors\n",
				track, g_hi.detected, g_hi.packets, g_hi.peak_extend,
				g_hi.max_gain / 2, g_hi.max_gain % 2 * 5, g_hi.errors);
		if (jump == 1)
			track++;
		else if (jump == 0 && !g_stop)
			track++;
		/* jump == -1: track/lba already positioned by the SIGHUP branch;
		 * restart that track from its start */
	}
out:
	if (pcm) {
		if (g_stop)
			snd_pcm_drop(pcm);
		else
			snd_pcm_drain(pcm);
		snd_pcm_close(pcm);
	}
	status("stop", 0, 0, 0, t.n);
	close(fd);
	return rc;
}

/* ------------------------------------------------------------ video DVD --- */
#define DVD_CHUNK       16          /* sectors per read: 32 KB */
#define DVD_FRAMES_MAX  1100        /* PCM frames one sector can hold (mono, 16 bit: 1007) */

/* The sound of a video DVD whose audio is plain PCM (dvd.h), chapter `track` to the end.
 * `fd` is the open drive, for the door and the disc check. */
static int cmd_play_dvd(int track, const char *dev, int fd, uint32_t sig)
{
	static uint8_t buf[DVD_CHUNK * DVD_SECTOR];
	static int32_t out[DVD_FRAMES_MAX * 2];
	struct dvd d;
	struct dvd_pcm pc;
	snd_pcm_t *pcm = NULL;
	int r, paused = 0, rc = 0, bad = 0, ch, nch;
	long gone_t = 0, lastst = -1;
	unsigned rate;
	pid_t parent = getppid();
	int follow = getenv("X45CD_FOLLOW_PARENT") != NULL;

	if (dvd_open(&d, dev, sig) <= 0) {
		fprintf(stderr, "x45cd: no audio track\n");
		return 4;
	}
	nch = d.t.nch;
	rate = (unsigned)d.t.rate;
	sg_prevent(fd, 0);      /* the mount locked the door; the drive's own button should still work */
	ch = track >= 1 && track <= nch ? track : 1;
	status("play", ch, 0, (d.t.ch[ch - 1].ms + 500) / 1000, nch);
	if ((r = open_pcm(&pcm, rate)) < 0) {
		fprintf(stderr, "x45cd: open %s: %s\n", PCM, snd_strerror(r));
		dvd_close(&d);
		return 5;
	}
	signal(SIGUSR1, on_usr1);
	signal(SIGUSR2, on_usr2);
	signal(SIGHUP, on_hup);
	signal(SIGTERM, on_term);
	signal(SIGINT, on_term);
	if (d.t.fmt == 0)
		fprintf(stderr, "x45cd: DVD, %d chapters, Dolby Digital stream %d: %d ch, mixed down to stereo\n", nch,
			d.t.astream, d.t.channels);
	else
		fprintf(stderr, "x45cd: DVD, %d chapters, PCM stream %d: %d bit %u Hz %d ch\n", nch, d.t.astream,
			d.t.bits, rate, d.t.channels);
	memset(&pc, 0, sizeof pc);

	while (!g_stop && ch >= 1 && ch <= nch) {
		const struct dvd_chapter *c = &d.t.ch[ch - 1];
		int32_t sec = c->first;
		long played = 0, len_s = (c->ms + 500) / 1000;
		int jump = 0;

		dvd_pcm_init(&pc, &d.t);
		while (sec <= c->last && !g_stop && !jump) {
			int n = c->last - sec + 1 < DVD_CHUNK ? (int)(c->last - sec + 1) : DVD_CHUNK;
			int got, i, frames = 0;
			long now_s;

			if (follow && getppid() != parent)
				g_stop = 1;
			if (g_stop)
				break;
			if (g_next) {
				g_next = 0;
				jump = 1;
				break;
			}
			if (g_prev) {
				g_prev = 0;
				/* more than 3 s in: restart this chapter, else go back one */
				if (played > 3 * (long)rate || ch == 1) {
					sec = c->first;
					played = 0;
					jump = -2;      /* same chapter again */
				} else {
					ch--;
					jump = -1;
				}
				break;
			}
			if (g_pause_toggle) {
				g_pause_toggle = 0;
				paused = !paused;
				if (paused)
					snd_pcm_drop(pcm);
				else
					snd_pcm_prepare(pcm);
			}
			if (paused) {
				status("pause", ch, played / (long)rate, len_s, nch);
				usleep(100000);
				if (time(NULL) != gone_t) {
					gone_t = time(NULL);
					if (disc_gone(fd)) {
						fprintf(stderr, "x45cd: disc removed\n");
						g_stop = 1;
						rc = 7;
					}
				}
				continue;
			}

			got = dvd_read(&d, sec, n, buf);
			if (got <= 0) {
				fprintf(stderr, "x45cd: read error at sector %d: %s\n", (int)sec,
					got < 0 ? strerror(-got) : "end of the files");
				if (++bad >= 2 && disc_gone(fd)) {
					fprintf(stderr, "x45cd: disc removed\n");
					g_stop = 1;
					rc = 7;
					break;
				}
				if (bad >= 20) {
					fprintf(stderr, "x45cd: the disc cannot be read any more\n");
					g_stop = 1;
					rc = 7;
					break;
				}
				sec += n;       /* a sector the drive cannot read is skipped */
				continue;
			}
			bad = 0;
			for (i = 0; i < got && !g_stop; i++) {
				/* a sector holds at most 1007 PCM frames; a Dolby Digital frame is 1536 and what
				 * does not fit stays in the decoder until the next call */
				int nf = dvd_pcm_sector(&pc, buf + (size_t)i * DVD_SECTOR, out, DVD_FRAMES_MAX);

				while (nf > 0 && !g_stop) {
					const int32_t *p = out;

					r = nf;
					while (r > 0 && !g_stop) {
						snd_pcm_sframes_t w = snd_pcm_writei(pcm, p, (snd_pcm_uframes_t)r);

						if (w < 0) {
							if (w == -EINTR)
								continue;
							w = snd_pcm_recover(pcm, (int)w, 1);
							if (w < 0) {
								fprintf(stderr, "x45cd: write: %s\n", snd_strerror((int)w));
								rc = 6;
								goto out;
							}
							continue;
						}
						p += w * 2;
						r -= (int)w;
					}
					frames += nf;
					nf = dvd_pcm_flush(&pc, out, DVD_FRAMES_MAX);
				}
			}
			sec += got;
			played += frames;
			x45_clock_select(rate);
			now_s = played / (long)rate;
			if (now_s != lastst) {
				lastst = now_s;
				status("play", ch, now_s, len_s, nch);
			}
		}
		dvd_pcm_done(&pc);
		if (jump == 1)
			ch++;
		else if (jump == 0 && !g_stop)
			ch++;
		lastst = -1;
		/* jump -1 / -2: ch is already the chapter to start again */
	}
out:
	if (pcm) {
		if (g_stop)
			snd_pcm_drop(pcm);
		else
			snd_pcm_drain(pcm);
		snd_pcm_close(pcm);
	}
	dvd_pcm_done(&pc);
	dvd_close(&d);
	status("stop", 0, 0, 0, nch);
	return rc;
}

/* x45cd hdcd <track> [seconds] [dev]: run the start of a track through the
 * decoder without playing it and say what the codes look like. */
static int cmd_hdcd(int track, int seconds, const char *dev)
{
	static uint8_t raw[CHUNK * CD_FRAMESIZE_RAW] __attribute__((aligned(4)));
	static int32_t out[CHUNK * SECTOR_PCM_FRAMES * 2];
	struct toc t;
	int fd = open(dev, O_RDONLY | O_NONBLOCK), r, done = 0, sectors, first = -1;
	int32_t lba;

	if (fd < 0) {
		perror(dev);
		return 2;
	}
	if ((r = read_toc(fd, &t)) < 0) {
		fprintf(stderr, "x45cd: no disc / no toc: %s\n", strerror(-r));
		return 3;
	}
	if (track < t.first || track > t.last || !t.audio[track]) {
		fprintf(stderr, "x45cd: no such audio track\n");
		return 4;
	}
	lba = t.start[track];
	sectors = seconds * 75;
	if (track_end(&t, track) - lba < sectors)
		sectors = (int)(track_end(&t, track) - lba);
	hdcd_init(&g_hd, 44100, 2000);
	while (done < sectors) {
		int n = sectors - done < CHUNK ? sectors - done : CHUNK;
		const int16_t *in = (const int16_t *)raw;
		int i, pf = n * SECTOR_PCM_FRAMES;

		read_sectors(fd, lba + done, n, raw);
		for (i = 0; i < pf * 2; i++)
			out[i] = in[i];
		hdcd_decode(&g_hd, out, pf);
		hdcd_info(&g_hd, &g_hi);
		if (first < 0 && g_hi.packets)
			first = done * 1000 / 75;
		done += n;
	}
	hdcd_info(&g_hd, &g_hi);
	printf("track %d, %d s: hdcd %d, %d packets (first at %d ms), peak extend %d, max gain %d.%d dB, "
	       "transient filter %d, errors %d, active %d\n",
	       track, sectors / 75, g_hi.detected, g_hi.packets, first, g_hi.peak_extend,
	       g_hi.max_gain / 2, g_hi.max_gain % 2 * 5, g_hi.transient_filter, g_hi.errors,
	       g_hi.active);
	close(fd);
	return 0;
}

/* ---------------------------------------------------------- tray + CD-Text --- */
#define CDROMEJECT      0x5309
#define CDROMCLOSETRAY  0x5319
#define CDROM_LOCKDOOR  0x5329
#define SG_IO           0x2285
#define SG_DXFER_FROM_DEV (-3)

struct sg_io_hdr {
	int interface_id, dxfer_direction;
	unsigned char cmd_len, mx_sb_len;
	unsigned short iovec_count;
	unsigned int dxfer_len;
	void *dxferp;
	unsigned char *cmdp, *sbp;
	unsigned int timeout, flags;
	int pack_id;
	void *usr_ptr;
	unsigned char status, masked_status, msg_status, sb_len_wr;
	unsigned short host_status, driver_status;
	int resid;
	unsigned int duration, info;
};

/* START STOP UNIT with LoEj: the drive's own tray command, for when the kernel's
 * CDROMEJECT / CDROMCLOSETRAY ioctls answer EIO. */
static int sg_start_stop(int fd, int load)
{
	uint8_t cdb[6] = { 0x1b, 0, 0, 0, load ? 0x03 : 0x02, 0 };
	uint8_t sense[32];
	struct sg_io_hdr h;

	memset(&h, 0, sizeof h);
	h.interface_id = 'S';
	h.dxfer_direction = -1;               /* SG_DXFER_NONE */
	h.cmd_len = sizeof cdb;
	h.mx_sb_len = sizeof sense;
	h.cmdp = cdb;
	h.sbp = sense;
	h.timeout = 20000;
	if (ioctl(fd, SG_IO, &h) < 0)
		return -1;
	return h.status == 0 ? 0 : -1;
}

/* PREVENT ALLOW MEDIUM REMOVAL: 1 locks the door (the drive's own button does nothing),
 * 0 lets it open. */
static int sg_prevent(int fd, int prevent)
{
	uint8_t cdb[6] = { 0x1e, 0, 0, 0, prevent ? 1 : 0, 0 };
	uint8_t sense[32];
	struct sg_io_hdr h;

	memset(&h, 0, sizeof h);
	h.interface_id = 'S';
	h.dxfer_direction = -1;               /* SG_DXFER_NONE */
	h.cmd_len = sizeof cdb;
	h.mx_sb_len = sizeof sense;
	h.cmdp = cdb;
	h.sbp = sense;
	h.timeout = 5000;
	if (ioctl(fd, SG_IO, &h) < 0)
		return -1;
	return h.status == 0 ? 0 : -1;
}

/* x45cd id: "<ntracks> <frames>" like the first line of toc, from the header and the lead-out only,
 * so the panel can ask often whether the disc is still the one it listed. Prints nothing and
 * exits 3 when there is no disc. */
static int cmd_id(const char *dev)
{
	struct cd_tochdr h;
	struct cd_tocentry first, lead;
	int fd = open(dev, O_RDONLY | O_NONBLOCK), st;

	if (fd < 0) {
		perror(dev);
		return 2;
	}
	st = ioctl(fd, CDROM_DRIVE_STATUS, CDSL_CURRENT);
	if (st > 0 && st != CDS_DISC_OK) {      /* tray open, no disc, still spinning up: do not wait for it */
		close(fd);
		return 3;
	}
	memset(&first, 0, sizeof first);
	memset(&lead, 0, sizeof lead);
	first.format = lead.format = CDROM_LBA;
	if (ioctl(fd, CDROMREADTOCHDR, &h) < 0) {
		close(fd);
		return 3;
	}
	first.track = h.first;
	lead.track = CDROM_LEADOUT;
	if (ioctl(fd, CDROMREADTOCENTRY, &first) < 0 || ioctl(fd, CDROMREADTOCENTRY, &lead) < 0) {
		close(fd);
		return 3;
	}
	printf("%d %d\n", h.last - h.first + 1, (int)(lead.lba - first.lba));
	close(fd);
	return 0;
}

static int cmd_tray(const char *dev, int open_it)
{
	int fd = open(dev, O_RDONLY | O_NONBLOCK);

	if (fd < 0) {
		perror(dev);
		return 2;
	}
	if (open_it) {
		if (umount2(DVD_MNT, 0) < 0 && errno == EBUSY)    /* a DVD that a killed player left mounted */
			umount2(DVD_MNT, MNT_DETACH);
		ioctl(fd, CDROM_LOCKDOOR, 0);   /* the kernel keeps the door locked while open */
	}
	if (ioctl(fd, open_it ? CDROMEJECT : CDROMCLOSETRAY, 0) < 0 &&
	    sg_start_stop(fd, !open_it) < 0) {
		perror(open_it ? "eject" : "close tray");
		close(fd);
		return 3;
	}
	close(fd);
	return 0;
}

/* CD-Text: READ TOC/PMA/ATIP, format 5. The disc stores 18-byte packs: type
 * (0x80 title, 0x81 performer), the track the first string belongs to, a
 * sequence number, a flags byte, 12 text bytes and a CRC. Strings are NUL
 * terminated and run on across packs; each NUL moves to the next track. Output:
 *   "T <track> <text>" for titles (track 0 = the album), "P <track> <text>" for
 * performers. Nothing is printed for a disc without CD-Text (exit 4). */
static int cmd_text(const char *dev)
{
	static uint8_t buf[4096];
	uint8_t cdb[10] = { 0x43, 0, 5, 0, 0, 0, 0, sizeof buf >> 8, sizeof buf & 0xff, 0 };
	uint8_t sense[32];
	struct sg_io_hdr h;
	char text[2][MAXTRK + 2][256];
	int pos[2] = { 0, 0 }, trk[2] = { 0, 0 }, seen = 0;
	int fd = open(dev, O_RDONLY | O_NONBLOCK), len, i, k;

	if (fd < 0) {
		perror(dev);
		return 2;
	}
	memset(&h, 0, sizeof h);
	memset(text, 0, sizeof text);
	h.interface_id = 'S';
	h.dxfer_direction = SG_DXFER_FROM_DEV;
	h.cmd_len = sizeof cdb;
	h.mx_sb_len = sizeof sense;
	h.dxfer_len = sizeof buf;
	h.dxferp = buf;
	h.cmdp = cdb;
	h.sbp = sense;
	h.timeout = 10000;
	if (ioctl(fd, SG_IO, &h) < 0 || h.status != 0) {
		fprintf(stderr, "x45cd: no CD-Text\n");
		close(fd);
		return 4;
	}
	close(fd);
	len = (buf[0] << 8 | buf[1]) + 2;
	if (len > (int)sizeof buf)
		len = sizeof buf;
	for (i = 4; i + 18 <= len; i += 18) {
		const uint8_t *pk = buf + i;
		int t = pk[0] == 0x80 ? 0 : pk[0] == 0x81 ? 1 : -1;
		int trk0 = pk[1] & 0x7f;

		if (t < 0)
			continue;
		if (pos[t] == 0)
			trk[t] = trk0;
		for (k = 0; k < 12; k++) {
			uint8_t c = pk[4 + k];

			if (c == 0) {
				if (trk[t] <= MAXTRK)
					text[t][trk[t]][pos[t]] = '\0';
				trk[t]++;
				pos[t] = 0;
			} else if (trk[t] <= MAXTRK && pos[t] < 255) {
				text[t][trk[t]][pos[t]++] = (char)c;
				seen = 1;
			}
		}
	}
	if (!seen) {
		fprintf(stderr, "x45cd: no CD-Text\n");
		return 4;
	}
	for (k = 0; k < 2; k++)
		for (i = 0; i <= MAXTRK; i++)
			if (text[k][i][0])
				printf("%c %d %s\n", k ? 'P' : 'T', i, text[k][i]);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc >= 2 && !strcmp(argv[1], "toc"))
		return cmd_toc(argc > 2 ? argv[2] : "/dev/sr0");
	if (argc >= 2 && !strcmp(argv[1], "id"))
		return cmd_id(argc > 2 ? argv[2] : "/dev/sr0");
	if (argc >= 3 && !strcmp(argv[1], "play"))
		return cmd_play(atoi(argv[2]), argc > 3 ? argv[3] : "/dev/sr0");
	if (argc >= 3 && !strcmp(argv[1], "hdcd"))
		return cmd_hdcd(atoi(argv[2]), argc > 3 ? atoi(argv[3]) : 10,
				argc > 4 ? argv[4] : "/dev/sr0");
	if (argc >= 2 && !strcmp(argv[1], "text"))
		return cmd_text(argc > 2 ? argv[2] : "/dev/sr0");
	if (argc >= 2 && !strcmp(argv[1], "eject"))
		return cmd_tray(argc > 2 ? argv[2] : "/dev/sr0", 1);
	if (argc >= 2 && !strcmp(argv[1], "close"))
		return cmd_tray(argc > 2 ? argv[2] : "/dev/sr0", 0);
	fprintf(stderr, "usage: x45cd toc|text|eject|close [dev] | x45cd play <track> [dev] | "
		"x45cd hdcd <track> [seconds] [dev]\n");
	return 1;
}
