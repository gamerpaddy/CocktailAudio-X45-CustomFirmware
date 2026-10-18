/* x45rec - record to the attached disk as FLAC (a CD can also be ripped to MP3).
 *
 *   x45rec in  [-r rate] [-b 16|24] [-n name] [-d dir]   record the capture (hw:1,0)
 *                                                        until SIGTERM
 *   x45rec cd  [track] [-d dir] [-f flac|mp3|mp3v0]      rip the audio CD (all tracks
 *                                                        or one) with its CD-Text tags; a video
 *                                                        DVD: the sound, a FLAC per chapter
 *   x45rec dir                                           print where recordings go
 *
 * `in` records whatever the selected input feeds the SSI capture (the analogue and
 * digital inputs at 192 kHz / 24 bit, the radio at 44.1 kHz / 16 bit). The caller
 * must have stopped anything else that holds the capture device (the level meter).
 * `cd` reads the disc digitally (CDROMREADAUDIO), so the DAC is not involved.
 * -f picks the file format of a CD rip: flac (default, lossless), mp3 (320 kbps constant) or
 * mp3v0 (LAME -V0, variable, about 220-260 kbps). MP3s get an ID3v2 tag and the LAME gapless header.
 *
 * Target: the first read-write mount under /mnt, in "My Music/Recordings" when the
 * disk has a "My Music" folder, else "Recordings" at its top.
 *
 * A CD rip never overwrites: every disc gets a folder of its own (a second disc with the same name becomes "name (2)";
 * a disc without any tags is "Unknown Artist - Audio CD <id>", the id being the number x45cd discid prints), a track
 * is written as <name>.part and renamed when whole, a track already ripped whole in that format is kept, and a file
 * that is in the way gets a "(2)". The folder holds "rip log.txt": per track the CRC-32 of the PCM (the same for
 * FLAC and MP3 of one disc, so two rips can be compared), the unreadable sectors (a sector the drive cannot give
 * after three tries is silence, and says so there and in the status), and the peak level. Tags come from the CD-Text
 * and from the user's own entries for the disc (x45cd text, x45cd setmeta).
 *
 * Status (rewritten twice a second): /tmp/x45rec.status
 *   state=rec|ripping|done|error  secs=<elapsed>  bytes=<written>  file=<path>
 *   track=<n>  tracks=<total>  bad=<unreadable sectors>  dir=<album folder>  msg=<text>
 */
#define _GNU_SOURCE
#include <alsa/asoundlib.h>
#include "../x45cd/dvd.h"
#include <FLAC/metadata.h>
#include <FLAC/stream_encoder.h>
#include <lame/lame.h>

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define CAP_DEV "hw:1,0"
#define CDDEV   "/dev/sr0"
#define X45CD   "/home/root/x45new/bin/x45cd"
#define STATUS  "/tmp/x45rec.status"

static volatile sig_atomic_t g_stop;
static void on_term(int s) { (void)s; g_stop = 1; }

static long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* ------------------------------------------------------------------ status --- */

struct stat_info {
	const char *state;
	long secs, bytes;
	char file[300], msg[120], dir[300];
	int track, tracks;
	int bad;        /* unreadable CD sectors so far (they are written as silence and logged) */
};

static void write_status(const struct stat_info *s)
{
	FILE *f = fopen(STATUS ".tmp", "w");

	if (!f)
		return;
	fprintf(f, "state=%s\nsecs=%ld\nbytes=%ld\nfile=%s\ntrack=%d\ntracks=%d\nbad=%d\ndir=%s\nmsg=%s\n",
		s->state, s->secs, s->bytes, s->file, s->track, s->tracks, s->bad, s->dir, s->msg);
	fclose(f);
	rename(STATUS ".tmp", STATUS);
}

static void fail(struct stat_info *s, const char *msg)
{
	s->state = "error";
	snprintf(s->msg, sizeof s->msg, "%s", msg);
	write_status(s);
	fprintf(stderr, "x45rec: %s\n", msg);
}

/* ------------------------------------------------------------------ target --- */

static void mkdir_p(const char *path)
{
	char tmp[400];
	char *p;

	snprintf(tmp, sizeof tmp, "%s", path);
	for (p = tmp + 1; *p; p++)
		if (*p == '/') {
			*p = '\0';
			mkdir(tmp, 0777);
			*p = '/';
		}
	mkdir(tmp, 0777);
}

/* A disk the music player mounted read-only (it never writes otherwise) is switched to read-write for
 * the recording and back when the recorder ends. */
static char remounted[300];

static void restore_ro(void)
{
	char cmd[400];

	if (!remounted[0])
		return;
	sync();
	snprintf(cmd, sizeof cmd, "mount -o remount,ro '%s' >/dev/null 2>&1", remounted);
	if (system(cmd) != 0) {
		/* a file still open: try again once it has been closed */
		sleep(1);
		(void)system(cmd);
	}
	remounted[0] = '\0';
}

/* first attached disk under /mnt that can be written */
static int find_target(char *out, size_t cap)
{
	FILE *f = fopen("/proc/mounts", "r");
	char line[512];
	struct stat st;
	int found = 0;

	if (!f)
		return -1;
	while (!found && fgets(line, sizeof line, f)) {
		char dev[128], mp[256], type[32], opts[128];

		if (sscanf(line, "%127s %255s %31s %127s", dev, mp, type, opts) != 4)
			continue;
		if (strncmp(mp, "/mnt/", 5) != 0 || strncmp(dev, "/dev/sd", 7) != 0)
			continue;   /* only an attached disk (USB / SATA), never the internal memory */
		if (strncmp(opts, "rw", 2) != 0) {
			char cmd[400];

			if (strpbrk(mp, "'\\"))
				continue;
			snprintf(cmd, sizeof cmd, "mount -o remount,rw '%s' >/dev/null 2>&1", mp);
			if (system(cmd) != 0)
				continue;
			snprintf(remounted, sizeof remounted, "%s", mp);
			atexit(restore_ro);
		}
		{
			char mm[300];

			snprintf(mm, sizeof mm, "%s/My Music", mp);
			if (stat(mm, &st) == 0 && S_ISDIR(st.st_mode))
				snprintf(out, cap, "%s/Recordings", mm);
			else
				snprintf(out, cap, "%s/Recordings", mp);
		}
		found = 1;
	}
	fclose(f);
	return found ? 0 : -1;
}

/* a name that is safe on FAT and NTFS */
static void sanitize(char *s)
{
	char *p;

	for (p = s; *p; p++)
		if (strchr("/\\:*?\"<>|", *p) || (unsigned char)*p < 32)
			*p = '_';
	while (p > s && (p[-1] == ' ' || p[-1] == '.'))
		*--p = '\0';
}

/* -------------------------------------------------------------------- FLAC --- */

static FLAC__StreamEncoder *enc_open(const char *path, int ch, int bits, unsigned rate,
				     const char *const tags[][2], int ntags)
{
	FLAC__StreamEncoder *e = FLAC__stream_encoder_new();
	FLAC__StreamMetadata *vc = NULL, *meta[1];
	int i;

	if (!e)
		return NULL;
	FLAC__stream_encoder_set_channels(e, ch);
	FLAC__stream_encoder_set_bits_per_sample(e, bits);
	FLAC__stream_encoder_set_sample_rate(e, rate);
	FLAC__stream_encoder_set_compression_level(e, 1);   /* light on the A9 at 192 kHz */
	if (ntags > 0 && (vc = FLAC__metadata_object_new(FLAC__METADATA_TYPE_VORBIS_COMMENT))) {
		for (i = 0; i < ntags; i++) {
			FLAC__StreamMetadata_VorbisComment_Entry ent;

			if (tags[i][1][0] &&
			    FLAC__metadata_object_vorbiscomment_entry_from_name_value_pair(
				    &ent, tags[i][0], tags[i][1]))
				FLAC__metadata_object_vorbiscomment_append_comment(vc, ent, false);
		}
		meta[0] = vc;
		FLAC__stream_encoder_set_metadata(e, meta, 1);
	}
	if (FLAC__stream_encoder_init_file(e, path, NULL, NULL) != FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
		FLAC__stream_encoder_delete(e);
		if (vc)
			FLAC__metadata_object_delete(vc);
		return NULL;
	}
	/* the encoder keeps pointers into vc until finish: leak it for the life of
	 * this short-lived process rather than track it */
	return e;
}

static long file_size(const char *path)
{
	struct stat st;

	if (stat(path, &st) != 0)
		return 0;
	return st.st_size > 0x7fffffffL ? 0x7fffffffL : (long)st.st_size;   /* a long is 32 bits here: stay positive */
}

/* ------------------------------------------------------------------- input --- */

static int cmd_in(int rate, int bits, const char *name, const char *dir_arg)
{
	struct stat_info st;
	char dir[300], path[400], stamp[40];
	snd_pcm_t *pcm = NULL;
	snd_pcm_hw_params_t *hw;
	FLAC__StreamEncoder *enc;
	const char *tags[3][2];
	snd_pcm_uframes_t period = rate >= 96000 ? 1920 : 1024, buf;
	unsigned r = rate;
	int32_t *pcm32;
	void *raw;
	int e, i, ntags = 0;
	long t0, last = now_ms() - 1000;   /* (not 0: now_ms() is negative for half of a 49.7 day cycle) */
	time_t t = time(NULL);
	struct tm tm;
	long long frames = 0, pre_frames = 0;
	int32_t *pre = NULL;

	memset(&st, 0, sizeof st);
	if (dir_arg)
		snprintf(dir, sizeof dir, "%s", dir_arg);
	else if (find_target(dir, sizeof dir) < 0) {
		fail(&st, "no writable disk");
		return 2;
	}
	mkdir_p(dir);
	/* before anything is written: stopped during the rate measurement, the file still gets closed properly */
	signal(SIGTERM, on_term);
	signal(SIGINT, on_term);
	localtime_r(&t, &tm);
	strftime(stamp, sizeof stamp, "%Y-%m-%d %H.%M.%S", &tm);
	snprintf(path, sizeof path, "%s/Recording %s%s%s.flac", dir, stamp, name ? " " : "", name ? name : "");
	sanitize(path + strlen(dir) + 1);   /* keep the directory part as is */

	if ((e = snd_pcm_open(&pcm, CAP_DEV, SND_PCM_STREAM_CAPTURE, 0)) < 0) {
		fail(&st, snd_strerror(e));
		return 3;
	}
	snd_pcm_hw_params_alloca(&hw);
	snd_pcm_hw_params_any(pcm, hw);
	if ((e = snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0 ||
	    (e = snd_pcm_hw_params_set_format(pcm, hw, bits == 16 ? SND_PCM_FORMAT_S16_LE
								   : SND_PCM_FORMAT_S24_LE)) < 0 ||
	    (e = snd_pcm_hw_params_set_channels(pcm, hw, 2)) < 0 ||
	    (e = snd_pcm_hw_params_set_rate_near(pcm, hw, &r, 0)) < 0) {
		fail(&st, snd_strerror(e));
		return 3;
	}
	snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, 0);
	buf = period * 8;
	snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &buf);
	if ((e = snd_pcm_hw_params(pcm, hw)) < 0) {
		fail(&st, snd_strerror(e));
		return 3;
	}

	/* The capture device accepts any rate but delivers frames at whatever the SSI port is clocked at (the rate
	 * of what plays: 44.1 kHz for most network audio, never the 192 kHz asked for). So when no rate was
	 * given, listen for a moment, work out the real rate from the time the frames take, and name the file
	 * accordingly; what was heard meanwhile is written first. */
	if (rate == 192000) {
		long tstart = now_ms(), tfirst = 0, tlast = 0;
		int have_first = 0;
		long long first_n = 0, got = 0;
		size_t cap = (size_t)192000 * 2 * 2;   /* 2 s at the highest rate */
		double real;
		static const unsigned std_rates[] = { 44100, 48000, 88200, 96000, 176400, 192000 };
		unsigned best = 44100;
		double bestd = 1e9;

		pre = malloc(cap * sizeof *pre);
		raw = malloc(period * 2 * 4);
		if (!pre || !raw)
			return 5;
		while (!g_stop && now_ms() - tstart < 1500 && (size_t)(got + (long long)period) * 2 <= cap) {
			snd_pcm_sframes_t n = snd_pcm_readi(pcm, raw, period);
			long tnow;

			if (n < 0) {
				if (n == -EINTR)
					continue;
				n = snd_pcm_recover(pcm, (int)n, 1);
				if (n < 0) {
					fail(&st, "capture failed");
					return 6;
				}
				continue;
			}
			tnow = now_ms();
			if (!have_first) {   /* a flag: 0 is a valid time near the 32-bit wrap */
				have_first = 1;
				tfirst = tnow;
				first_n = n;
			}
			tlast = tnow;
			for (i = 0; i < n * 2; i++)
				pre[got * 2 + i] = bits == 16 ? ((int16_t *)raw)[i]
							      : (int32_t)((uint32_t)((int32_t *)raw)[i] << 8) >> 8;
			got += n;
		}
		real = tlast - tfirst > 0 ? (double)(got - first_n) * 1000.0 / (double)(tlast - tfirst) : 0;   /* a difference, not two absolute times */
		for (i = 0; i < (int)(sizeof std_rates / sizeof *std_rates); i++) {
			double d = real > 0 ? fabs(real / std_rates[i] - 1.0) : 1e9;

			if (d < bestd) {
				bestd = d;
				best = std_rates[i];
			}
		}
		r = best;
		pre_frames = got;
		fprintf(stderr, "x45rec: capture delivers %.0f frames/s: recording as %u Hz\n", real, r);
		free(raw);
	}
	if (name) {
		tags[ntags][0] = "ALBUM";
		tags[ntags++][1] = name;
	}
	tags[ntags][0] = "TITLE";
	tags[ntags++][1] = stamp;
	enc = enc_open(path, 2, bits, r, tags, ntags);
	if (!enc) {
		fail(&st, "cannot write the file");
		return 4;
	}
	raw = malloc(period * 2 * 4);
	pcm32 = malloc(period * 2 * sizeof *pcm32);
	if (!raw || !pcm32)
		return 5;
	if (pre_frames > 0) {   /* what was heard while the rate was being measured */
		long long done_f = 0;

		while (done_f < pre_frames) {
			unsigned chunk = pre_frames - done_f > 4096 ? 4096 : (unsigned)(pre_frames - done_f);

			if (!FLAC__stream_encoder_process_interleaved(enc, pre + done_f * 2, chunk)) {
				fail(&st, "encoder failed (disk full?)");
				return 7;
			}
			done_f += chunk;
		}
		frames = pre_frames;
	}

	st.state = "rec";
	snprintf(st.file, sizeof st.file, "%s", path);
	t0 = now_ms();
	while (!g_stop) {
		snd_pcm_sframes_t n = snd_pcm_readi(pcm, raw, period);

		if (n < 0) {
			if (n == -EINTR)
				continue;
			n = snd_pcm_recover(pcm, (int)n, 1);
			if (n < 0) {
				fail(&st, "capture failed");
				break;
			}
			continue;
		}
		for (i = 0; i < n * 2; i++) {
			if (bits == 16)
				pcm32[i] = ((int16_t *)raw)[i];
			else
				pcm32[i] = (int32_t)((uint32_t)((int32_t *)raw)[i] << 8) >> 8;
		}
		if (!FLAC__stream_encoder_process_interleaved(enc, pcm32, (unsigned)n)) {
			fail(&st, "encoder failed (disk full?)");
			break;
		}
		frames += n;
		if (now_ms() - last > 500) {
			last = now_ms();
			st.secs = frames / r;
			st.bytes = file_size(path);
			write_status(&st);
		}
	}
	if (!FLAC__stream_encoder_finish(enc) && strcmp(st.state, "error"))
		fail(&st, "encoder failed (disk full?)");
	FLAC__stream_encoder_delete(enc);
	snd_pcm_drop(pcm);
	snd_pcm_close(pcm);
	if (strcmp(st.state, "error")) {
		st.state = "done";
		st.secs = frames / r;
		st.bytes = file_size(path);
		write_status(&st);
	}
	(void)t0;
	return 0;
}

/* ---------------------------------------------------------------------- CD --- */

#define CDROMREADTOCHDR   0x5305
#define CDROMREADTOCENTRY 0x5306
#define CDROMREADAUDIO    0x530e
#define CDROM_LBA         0x01
#define CDROM_LEADOUT     0xAA
#define CD_FRAMESIZE_RAW  2352
#define CHUNK             13
#define MAXTRK            99

struct cd_tochdr { uint8_t first, last; };
struct cd_tocentry {
	uint8_t track, adr_ctrl, format;
	int32_t lba;
	uint8_t datamode;
};
struct cd_readaudio {
	int32_t addr;
	uint8_t addr_format;
	int nframes;
	uint8_t *buf;
};

struct toc {
	int first, last;
	int32_t start[MAXTRK + 2];
	int audio[MAXTRK + 2];
};

/* Where the sound of a track ends: an audio track followed by a data track (an Enhanced CD, whose data is a second
 * session) ends 11400 sectors before it. The table of contents counts them (lead-out, lead-in, pre-gap) to the audio
 * track, but nothing can be read there. Same rule as x45cd's track_end(). */
#define SESSION_GAP 11400

static int32_t track_end(const struct toc *t, int trk)
{
	int32_t end = t->start[trk + 1];

	if (t->audio[trk] && trk < t->last && !t->audio[trk + 1] && end - t->start[trk] > SESSION_GAP + 75)
		end -= SESSION_GAP;
	return end;
}

static int read_toc(int fd, struct toc *t)
{
	struct cd_tochdr h;
	struct cd_tocentry e;
	int i;

	memset(t, 0, sizeof *t);
	if (ioctl(fd, CDROMREADTOCHDR, &h) < 0)
		return -errno;
	t->first = h.first;
	t->last = h.last;
	if (t->last > MAXTRK)
		t->last = MAXTRK;
	for (i = t->first; i <= t->last + 1; i++) {
		memset(&e, 0, sizeof e);
		e.track = i > t->last ? CDROM_LEADOUT : i;
		e.format = CDROM_LBA;
		if (ioctl(fd, CDROMREADTOCENTRY, &e) < 0)
			return -errno;
		t->start[i] = e.lba;
		t->audio[i] = !((e.adr_ctrl >> 4) & 4);
	}
	return 0;
}

/* what `x45cd text` says about the disc: its CD-Text, with the user's own tags (web page) laid over it */
struct cdtext {
	char album[120], artist[120], genre[120], upc[40];
	char title[MAXTRK + 1][120], perf[MAXTRK + 1][120], composer[MAXTRK + 1][120];
};

static void load_cdtext(struct cdtext *c)
{
	FILE *p = popen(X45CD " text " CDDEV " 2>/dev/null", "r");
	char line[400];

	memset(c, 0, sizeof *c);
	if (!p)
		return;
	while (fgets(line, sizeof line, p)) {
		char kind = line[0], *txt;
		int n;

		if (!strchr("TPCGU", kind) || sscanf(line + 1, "%d", &n) != 1 || n < 0 || n > MAXTRK)
			continue;
		txt = strchr(line + 2, ' ');
		if (!txt)
			continue;
		txt++;
		txt[strcspn(txt, "\r\n")] = '\0';
		if (kind == 'T' && n == 0)
			snprintf(c->album, sizeof c->album, "%s", txt);
		else if (kind == 'P' && n == 0)
			snprintf(c->artist, sizeof c->artist, "%s", txt);
		else if (kind == 'G' && n == 0)
			snprintf(c->genre, sizeof c->genre, "%s", txt);
		else if (kind == 'U' && n == 0)
			snprintf(c->upc, sizeof c->upc, "%s", txt);
		else if (kind == 'T')
			snprintf(c->title[n], sizeof c->title[0], "%s", txt);
		else if (kind == 'C')
			snprintf(c->composer[n], sizeof c->composer[0], "%s", txt);
		else if (kind == 'P')
			snprintf(c->perf[n], sizeof c->perf[0], "%s", txt);
	}
	pclose(p);
}

/* ------------------------------------------------------- rip folder, log, reading --- */

#define RIPLOG "rip log.txt"

/* the CRC-32 of the PCM of a track: the same for the same audio whatever file format it went into, so two rips of one
 * disc (or one track) can be compared from the log */
static uint32_t crc_tab[256];

static void crc_init(void)
{
	uint32_t i, k, c;

	for (i = 0; i < 256; i++) {
		for (c = i, k = 0; k < 8; k++)
			c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
		crc_tab[i] = c;
	}
}

static uint32_t crc_update(uint32_t crc, const uint8_t *p, size_t n)
{
	while (n--)
		crc = crc_tab[(crc ^ *p++) & 0xff] ^ (crc >> 8);
	return crc;
}

/* The rip log lives on a removable disk that may hold anything: a link in its place is not followed (this runs as root) */
static FILE *log_open(const char *path, int append)
{
	int fd = open(path, (append ? O_WRONLY | O_APPEND | O_CREAT : O_RDONLY) | O_NOFOLLOW | O_CLOEXEC, 0644);
	FILE *f;

	if (fd < 0)
		return NULL;
	f = fdopen(fd, append ? "a" : "r");
	if (!f)
		close(fd);
	return f;
}

/* The disc a folder holds, from the rip log in it; 0 when it has none. A folder of another disc is never reused. */
static uint32_t dir_disc(const char *dir)
{
	char path[500], line[300];
	FILE *f;
	uint32_t id = 0;
	unsigned v;

	snprintf(path, sizeof path, "%s/" RIPLOG, dir);
	f = log_open(path, 0);
	if (!f)
		return 0;
	while (fgets(line, sizeof line, f))
		if (sscanf(line, "disc %x", &v) == 1)
			id = v;
	fclose(f);
	return id;
}

/* The folder of a disc: base/name when that is free or already this disc's, else base/name (2), (3) ... Nothing that
 * is there is touched. */
static void pick_dir(char *out, size_t cap, const char *base, const char *name, uint32_t sig)
{
	struct stat st;
	int n;

	for (n = 1; n < 100; n++) {
		if (n == 1)
			snprintf(out, cap, "%s/%s", base, name);
		else
			snprintf(out, cap, "%s/%s (%d)", base, name, n);
		if (lstat(out, &st) != 0)
			break;
		if (S_ISDIR(st.st_mode) && dir_disc(out) == sig)   /* lstat: a link is never taken for the folder */
			break;
	}
	mkdir_p(out);
}

static int file_exists(const char *path)
{
	struct stat st;

	return stat(path, &st) == 0;
}

/* dir/name.ext, or dir/name (2).ext when that file is there */
static void unique_file(char *out, size_t cap, const char *dir, const char *name, const char *ext)
{
	int n;

	snprintf(out, cap, "%s/%s.%s", dir, name, ext);
	for (n = 2; n < 100 && file_exists(out); n++)
		snprintf(out, cap, "%s/%s (%d).%s", dir, name, n, ext);
}

/* One line per ripped track, tab separated, appended to the folder's log:
 *   track <n> <ok|damaged> <ext> <crc32> <unreadable sectors> <peak %> <frames> <file> <note> */
static void log_line(const char *dir, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void log_line(const char *dir, const char *fmt, ...)
{
	char path[500];
	va_list ap;
	FILE *f;

	snprintf(path, sizeof path, "%s/" RIPLOG, dir);
	f = log_open(path, 1);
	if (!f)
		return;
	va_start(ap, fmt);
	vfprintf(f, fmt, ap);
	va_end(ap);
	fclose(f);
}

/* the earlier line of this track in the log: 1 when found (crc, status "ok" or "damaged", file name) */
static int log_find(const char *dir, int trk, const char *ext, uint32_t *crc, int *ok, char *file, size_t cap)
{
	char path[500], line[700];
	FILE *f;
	int found = 0;

	snprintf(path, sizeof path, "%s/" RIPLOG, dir);
	f = log_open(path, 0);
	if (!f)
		return 0;
	while (fgets(line, sizeof line, f)) {
		char *p[10], *s = line;
		int k;

		line[strcspn(line, "\r\n")] = '\0';
		for (k = 0; k < 10; k++) {
			p[k] = s;
			if (k < 9) {
				s = strchr(s, '\t');
				if (!s)
					break;
				*s++ = '\0';
			}
		}
		if (k < 8 || strcmp(p[0], "track") || atoi(p[1]) != trk)
			continue;
		if (ext && strcmp(p[3], ext))
			continue;
		*crc = (uint32_t)strtoul(p[4], NULL, 16);
		*ok = !strcmp(p[2], "ok");
		snprintf(file, cap, "%s", p[8]);
		found = 1;
	}
	fclose(f);
	return found;
}

/* Test hook: X45REC_FAULT=<first>-<last> makes the sectors in that range unreadable (a scratched disc to try the
 * error handling on). Not set, the drive is asked as it is. */
static int fault_lo = -1, fault_hi = -1;

static int cd_read(int fd, struct cd_readaudio *ra)
{
	if (fault_lo >= 0 && ra->addr <= fault_hi && ra->addr + ra->nframes - 1 >= fault_lo) {
		errno = EIO;
		return -1;
	}
	return ioctl(fd, CDROMREADAUDIO, ra);
}

/* How the drive got on with the sectors of one track. */
struct rdstat {
	int bad;                 /* sectors that could not be read (silence went into the file) */
	int retries;             /* reads that failed and were tried again */
	int streak;              /* unreadable sectors in a row: a damaged area is passed quickly */
	int32_t at[8];           /* the first unreadable sectors */
};

/* Reads n sectors at lba. A chunk that fails is read again sector by sector, so that one scratch costs one sector and
 * not 13; a sector that fails three times is silence. Returns the number of unreadable sectors. */
static int read_frames(int fd, int32_t lba, int n, uint8_t *raw, struct rdstat *rs)
{
	struct cd_readaudio ra;
	int tries, bad = 0, f;

	if (rs->streak < 26) {
		for (tries = 0; tries < 3; tries++) {
			ra.addr = lba;
			ra.addr_format = CDROM_LBA;
			ra.nframes = n;
			ra.buf = raw;
			if (cd_read(fd, &ra) == 0) {
				rs->streak = 0;
				return 0;
			}
			rs->retries++;
		}
	}
	for (f = 0; f < n; f++) {
		int max_tries = rs->streak >= 26 ? 1 : 3;

		for (tries = 0; tries < max_tries; tries++) {
			ra.addr = lba + f;
			ra.addr_format = CDROM_LBA;
			ra.nframes = 1;
			ra.buf = raw + (size_t)f * CD_FRAMESIZE_RAW;
			if (cd_read(fd, &ra) == 0)
				break;
			rs->retries++;
		}
		if (tries == max_tries) {
			memset(raw + (size_t)f * CD_FRAMESIZE_RAW, 0, CD_FRAMESIZE_RAW);
			if (rs->bad + bad < 8)
				rs->at[rs->bad + bad] = lba + f;
			bad++;
			rs->streak++;
		} else {
			rs->streak = 0;
		}
	}
	rs->bad += bad;
	return bad;
}

/* ------------------------------------------------------------------- video DVD --- */

#define DVD_CHUNK      16        /* sectors per read */
#define DVD_FRAMES_MAX 1100      /* PCM frames one sector can hold */

/* same number as x45cd's toc_sig: lets this process share the verdict that x45cd cached for the disc */
static uint32_t toc_sig(const struct toc *t)
{
	uint32_t h = 2166136261u;
	int i;

	h = (h ^ (uint32_t)(t->last - t->first + 1)) * 16777619u;
	for (i = t->first; i <= t->last + 1; i++)
		h = (h ^ (uint32_t)t->start[i]) * 16777619u;
	return h ? h : 1;
}

/* the volume name of the disc, for the folder; "Video DVD" when it has none */
static void dvd_label(char *out, size_t cap)
{
	FILE *p = popen("blkid -s LABEL -o value " CDDEV " 2>/dev/null", "r");
	char *q;

	out[0] = '\0';
	if (p) {
		if (fgets(out, (int)cap, p))
			out[strcspn(out, "\r\n")] = '\0';
		pclose(p);
	}
	for (q = out; *q; q++)
		if (*q == '_')
			*q = ' ';
	if (!out[0])
		snprintf(out, cap, "Video DVD");
}

/* One 32-bit left-justified sample as `bits` bits, rounded. */
static int32_t to_bits(int32_t v, int bits)
{
	int shift = 32 - bits;
	int64_t r;

	if (shift <= 0)
		return v;
	r = ((int64_t)v + ((int64_t)1 << (shift - 1))) >> shift;
	if (r > ((int64_t)1 << (bits - 1)) - 1)
		r = ((int64_t)1 << (bits - 1)) - 1;
	return (int32_t)r;
}

/* Rips the sound of a video DVD (PCM, or Dolby Digital as x45cd plays it: the stereo stream, or a downmix) to
 * one FLAC per chapter. PCM keeps its bit depth (16, 20 or 24), Dolby Digital becomes 16 bit. */
static int cmd_dvd(int only, const char *base, uint32_t sig, struct stat_info *st)
{
	static uint8_t buf[DVD_CHUNK * DVD_SECTOR];
	static int32_t out[DVD_FRAMES_MAX * 2], smp[DVD_FRAMES_MAX * 2];
	struct dvd d;
	struct dvd_pcm pc;
	char label[120], album_dir[400];
	int nch, ch, bits, rate, done = 0, rc = 0;

	if (dvd_open(&d, CDDEV, sig) <= 0) {
		fail(st, "no audio track");
		return 4;
	}
	nch = d.t.nch;
	rate = d.t.rate;
	bits = d.t.fmt == 4 && d.t.bits >= 16 && d.t.bits <= 24 ? d.t.bits : 16;
	dvd_label(label, sizeof label);
	sanitize(label);
	pick_dir(album_dir, sizeof album_dir, base, label, sig);
	snprintf(st->dir, sizeof st->dir, "%s", album_dir);
	log_line(album_dir, "disc %08x\nvideo DVD, sound only, one FLAC per chapter\n", (unsigned)sig);
	signal(SIGTERM, on_term);
	signal(SIGINT, on_term);
	st->state = "ripping";
	st->tracks = only && only <= nch ? 1 : nch;
	memset(&pc, 0, sizeof pc);
	fprintf(stderr, "x45rec: DVD \"%s\", %d chapters, %s, %d bit %d Hz\n", label, nch,
		d.t.fmt == 4 ? "PCM" : "Dolby Digital", bits, rate);

	for (ch = 1; ch <= nch && !g_stop && !rc; ch++) {
		const struct dvd_chapter *c = &d.t.ch[ch - 1];
		char name[200], path[600], part[610], num[16], tot[16], title[40];
		const char *tags[6][2];
		FLAC__StreamEncoder *enc;
		long long frames = 0;
		long last = now_ms() - 1000;
		int32_t sec = c->first;
		int bad = 0;

		if (only && ch != only)
			continue;
		st->track = ++done;
		snprintf(title, sizeof title, "Chapter %d", ch);
		snprintf(name, sizeof name, "%02d %s", ch, title);
		snprintf(path, sizeof path, "%s/%s.flac", album_dir, name);
		snprintf(st->file, sizeof st->file, "%s", path);
		if (file_exists(path)) {
			fprintf(stderr, "x45rec: %s is there already, kept\n", name);
			continue;       /* never overwrite what an earlier rip of this disc left */
		}
		snprintf(part, sizeof part, "%s.part", path);
		unlink(part);   /* a link or an old half file on the disk is replaced, never written through */
		snprintf(num, sizeof num, "%d", ch);
		snprintf(tot, sizeof tot, "%d", nch);
		tags[0][0] = "TITLE";       tags[0][1] = title;
		tags[1][0] = "ARTIST";      tags[1][1] = "";
		tags[2][0] = "ALBUM";       tags[2][1] = label;
		tags[3][0] = "ALBUMARTIST"; tags[3][1] = "";
		tags[4][0] = "TRACKNUMBER"; tags[4][1] = num;
		tags[5][0] = "TRACKTOTAL";  tags[5][1] = tot;
		enc = enc_open(part, 2, bits, (unsigned)rate, tags, 6);
		if (!enc) {
			fail(st, "cannot write the file");
			rc = 5;
			break;
		}
		dvd_pcm_init(&pc, &d.t);
		while (sec <= c->last && !g_stop) {
			int n = c->last - sec + 1 < DVD_CHUNK ? (int)(c->last - sec + 1) : DVD_CHUNK;
			int got = dvd_read(&d, sec, n, buf), i, k, ok = 1;

			if (got <= 0) {
				fprintf(stderr, "x45rec: read error at sector %d\n", (int)sec);
				if (++bad >= 20) {
					fail(st, "the disc cannot be read");
					rc = 7;
					break;
				}
				sec += n;       /* a sector the drive cannot read is skipped */
				continue;
			}
			bad = 0;
			for (i = 0; i < got && ok && !g_stop; i++) {
				int nf = dvd_pcm_sector(&pc, buf + (size_t)i * DVD_SECTOR, out, DVD_FRAMES_MAX);

				while (nf > 0 && ok) {
					for (k = 0; k < nf * 2; k++)
						smp[k] = to_bits(out[k], bits);
					if (!FLAC__stream_encoder_process_interleaved(enc, smp, (unsigned)nf))
						ok = 0;
					frames += nf;
					nf = dvd_pcm_flush(&pc, out, DVD_FRAMES_MAX);
				}
			}
			if (!ok) {
				fail(st, "encoder failed (disk full?)");
				rc = 6;
				break;
			}
			sec += got;
			if (now_ms() - last > 500) {
				last = now_ms();
				st->secs = (long)(frames / rate);
				st->bytes = file_size(part);
				write_status(st);
			}
		}
		if (!FLAC__stream_encoder_finish(enc) && !rc && !g_stop) {
			fail(st, "encoder failed (disk full?)");
			rc = 6;
		}
		FLAC__stream_encoder_delete(enc);
		dvd_pcm_done(&pc);
		if (g_stop || rc)
			unlink(part);   /* a half-ripped chapter is not kept */
		else if (rename(part, path) != 0)
			unlink(part);
	}
	dvd_close(&d);
	if (!rc && strcmp(st->state, "error")) {
		st->state = "done";
		write_status(st);
	}
	return rc;
}

/* ------------------------------------------------------------ rip file format --- */

enum { RIP_FLAC, RIP_MP3, RIP_MP3V0 };

static int parse_format(const char *s)
{
	if (!strcmp(s, "mp3"))
		return RIP_MP3;
	if (!strcmp(s, "mp3v0"))
		return RIP_MP3V0;
	return RIP_FLAC;
}

/* CD-Text is ISO 8859-1 (x45cd hands the bytes over as they are): FLAC tags and a UTF-16 ID3 tag
 * need the text as UTF-8, and a string that already is valid UTF-8 is left alone */
static int utf8_ok(const char *s)
{
	const unsigned char *p = (const unsigned char *)s;

	while (*p) {
		int n = *p < 0x80 ? 0 : (*p & 0xe0) == 0xc0 ? 1 : (*p & 0xf0) == 0xe0 ? 2 : (*p & 0xf8) == 0xf0 ? 3 : -1;

		if (n < 0)
			return 0;
		p++;
		while (n--)
			if ((*p++ & 0xc0) != 0x80)
				return 0;
	}
	return 1;
}

static void text_to_utf8(char *s, size_t cap)
{
	char tmp[300];
	size_t o = 0;
	const unsigned char *p;

	if (utf8_ok(s))
		return;
	for (p = (const unsigned char *)s; *p && o + 3 < sizeof tmp; p++) {
		if (*p < 0x80) {
			tmp[o++] = (char)*p;
		} else {
			tmp[o++] = (char)(0xc0 | *p >> 6);
			tmp[o++] = (char)(0x80 | (*p & 0x3f));
		}
	}
	tmp[o] = '\0';
	snprintf(s, cap, "%s", tmp);
	while (!utf8_ok(s) && *s)   /* cut by the buffer in the middle of a character */
		s[strlen(s) - 1] = '\0';
}

static void cdtext_utf8(struct cdtext *c)
{
	int i;

	text_to_utf8(c->album, sizeof c->album);
	text_to_utf8(c->artist, sizeof c->artist);
	text_to_utf8(c->genre, sizeof c->genre);
	for (i = 0; i <= MAXTRK; i++) {
		text_to_utf8(c->title[i], sizeof c->title[0]);
		text_to_utf8(c->perf[i], sizeof c->perf[0]);
		text_to_utf8(c->composer[i], sizeof c->composer[0]);
	}
}

struct trktags {
	const char *title, *artist, *album, *albumartist, *composer, *genre;
	int track, total;
	long frames;   /* CD frames (1/75 s) in the track */
};

/* one ID3v2 text frame; plain ASCII goes as ISO 8859-1, anything else as UTF-16 with a byte order mark */
static void id3_text(lame_t g, const char *id, const char *utf8)
{
	unsigned short w[260];
	const unsigned char *p;
	int n = 1, ascii = 1;

	if (!utf8[0])
		return;
	for (p = (const unsigned char *)utf8; *p; p++)
		ascii &= *p < 0x80;
	if (ascii) {
		id3tag_set_textinfo_latin1(g, id, utf8);
		return;
	}
	w[0] = 0xfeff;
	for (p = (const unsigned char *)utf8; *p && n < 256;) {
		uint32_t cp = *p < 0x80 ? *p : (*p & 0xe0) == 0xc0 ? *p & 0x1f : (*p & 0xf0) == 0xe0 ? *p & 0x0f : *p & 0x07;
		int more = *p < 0x80 ? 0 : (*p & 0xe0) == 0xc0 ? 1 : (*p & 0xf0) == 0xe0 ? 2 : 3;

		p++;
		while (more-- && *p)
			cp = cp << 6 | (*p++ & 0x3f);
		if (cp >= 0x10000) {
			cp -= 0x10000;
			w[n++] = (unsigned short)(0xd800 | cp >> 10);
			w[n++] = (unsigned short)(0xdc00 | (cp & 0x3ff));
		} else {
			w[n++] = (unsigned short)cp;
		}
	}
	w[n] = 0;
	id3tag_set_textinfo_utf16(g, id, w);
}

/* where the PCM of one track goes: a FLAC file or an MP3 file */
struct sink {
	int fmt;
	FLAC__StreamEncoder *flac;
	lame_t lame;
	FILE *f;
	long audio_at;   /* MP3: where the frames start, behind the ID3v2 tag */
};

/* worst case of lame_encode_buffer_interleaved is 1.25 * samples + 7200 bytes; the flush needs 7200 */
static unsigned char mp3buf[CHUNK * 588 * 5 / 4 + 7200 + 64];

static int mp3_open(struct sink *s, int fmt, const char *path, const struct trktags *tg)
{
	lame_t g = lame_init();
	char both[40];
	unsigned char *tag;
	size_t n;

	if (!g)
		return -1;
	lame_set_in_samplerate(g, 44100);
	lame_set_num_channels(g, 2);
	lame_set_num_samples(g, (unsigned long)tg->frames * 588);
	if (fmt == RIP_MP3) {
		lame_set_VBR(g, vbr_off);
		lame_set_brate(g, 320);
		lame_set_quality(g, 5);    /* the A9 encodes about 5x real time at 5, 8x at 7: the drive reads 4.5-9x */
	} else {
		lame_set_VBR(g, vbr_mtrh);
		lame_set_VBR_q(g, 0);
		lame_set_quality(g, 3);
	}
	lame_set_write_id3tag_automatic(g, 0);   /* the tag goes in front by hand, the Xing/Info frame is patched in at the end */
	id3tag_init(g);
	id3tag_add_v2(g);
	id3tag_v2_only(g);
	id3_text(g, "TIT2", tg->title);
	id3_text(g, "TPE1", tg->artist);
	id3_text(g, "TALB", tg->album);
	id3_text(g, "TPE2", tg->albumartist);
	id3_text(g, "TCOM", tg->composer);
	id3_text(g, "TCON", tg->genre);
	snprintf(both, sizeof both, "%d/%d", tg->track, tg->total);
	id3tag_set_textinfo_latin1(g, "TRCK", both);
	if (lame_init_params(g) < 0) {
		lame_close(g);
		return -1;
	}
	s->f = fopen(path, "wb");
	if (!s->f) {
		lame_close(g);
		return -1;
	}
	n = lame_get_id3v2_tag(g, NULL, 0);   /* too small a buffer answers with the size it needs */
	tag = malloc(n ? n : 1);
	if (!tag || (n && (lame_get_id3v2_tag(g, tag, n) != n || fwrite(tag, 1, n, s->f) != n))) {
		free(tag);
		fclose(s->f);
		lame_close(g);
		return -1;
	}
	free(tag);
	s->audio_at = (long)n;
	s->lame = g;
	return 0;
}

static int sink_open(struct sink *s, int fmt, const char *path, const struct trktags *tg)
{
	char num[16], tot[16];
	const char *tags[8][2];

	memset(s, 0, sizeof *s);
	s->fmt = fmt;
	if (fmt != RIP_FLAC)
		return mp3_open(s, fmt, path, tg);
	snprintf(num, sizeof num, "%d", tg->track);
	snprintf(tot, sizeof tot, "%d", tg->total);
	tags[0][0] = "TITLE";       tags[0][1] = tg->title;
	tags[1][0] = "ARTIST";      tags[1][1] = tg->artist;
	tags[2][0] = "ALBUM";       tags[2][1] = tg->album;
	tags[3][0] = "ALBUMARTIST"; tags[3][1] = tg->albumartist;
	tags[4][0] = "TRACKNUMBER"; tags[4][1] = num;
	tags[5][0] = "TRACKTOTAL";  tags[5][1] = tot;
	tags[6][0] = "COMPOSER";    tags[6][1] = tg->composer;
	tags[7][0] = "GENRE";       tags[7][1] = tg->genre;
	s->flac = enc_open(path, 2, 16, 44100, tags, 8);
	return s->flac ? 0 : -1;
}

/* 16-bit stereo, interleaved; 0 on success */
static int sink_write(struct sink *s, const int16_t *pcm, int frames)
{
	static int32_t wide[CHUNK * 588 * 2];
	int i, n;

	if (s->fmt == RIP_FLAC) {
		for (i = 0; i < frames * 2; i++)
			wide[i] = pcm[i];
		return FLAC__stream_encoder_process_interleaved(s->flac, wide, (unsigned)frames) ? 0 : -1;
	}
	n = lame_encode_buffer_interleaved(s->lame, (short *)pcm, frames, mp3buf, sizeof mp3buf);
	if (n < 0)
		return -1;
	return fwrite(mp3buf, 1, (size_t)n, s->f) == (size_t)n ? 0 : -1;
}

/* the last frames, then the Xing/Info frame (length, gapless delays) over the placeholder at the start of the audio;
 * 0 on success. A track that is thrown away (keep == 0) skips all that. */
static int sink_close(struct sink *s, int keep)
{
	int rc = 0;

	if (s->fmt == RIP_FLAC) {
		rc = FLAC__stream_encoder_finish(s->flac) ? 0 : -1;   /* the last block did not fit: a short file is no track */
		FLAC__stream_encoder_delete(s->flac);
		return rc;
	}
	if (keep) {
		static unsigned char lametag[4096];
		int n = lame_encode_flush(s->lame, mp3buf, sizeof mp3buf);
		size_t t;

		if (n < 0 || fwrite(mp3buf, 1, (size_t)n, s->f) != (size_t)n)
			rc = -1;
		t = lame_get_lametag_frame(s->lame, lametag, sizeof lametag);
		if (!rc && t > 0 && t <= sizeof lametag &&
		    (fseek(s->f, s->audio_at, SEEK_SET) != 0 || fwrite(lametag, 1, t, s->f) != t))
			rc = -1;
	}
	if (fclose(s->f) != 0)
		rc = -1;
	lame_close(s->lame);
	return rc;
}

/* The drive hands over a chunk every 0.1-0.2 s but LAME needs about 0.2 s of the A9 for the same audio, so the reading
 * and the encoding run side by side: this thread encodes and writes, the main thread reads. */
#define QSLOTS 48

static struct {
	struct { int16_t pcm[CHUNK * 588 * 2]; int frames; } slot[QSLOTS];
	int head, count, closed, abort, failed;
	pthread_mutex_t mu;
	pthread_cond_t more, room;
	struct sink *sink;
	pthread_t th;
} rq = { .mu = PTHREAD_MUTEX_INITIALIZER, .more = PTHREAD_COND_INITIALIZER, .room = PTHREAD_COND_INITIALIZER };

static void *rq_worker(void *arg)
{
	(void)arg;
	pthread_mutex_lock(&rq.mu);
	for (;;) {
		int i, bad;

		while (!rq.count && !rq.closed && !rq.abort)
			pthread_cond_wait(&rq.more, &rq.mu);
		if (rq.abort || !rq.count)
			break;
		i = rq.head;   /* stays queued while it is encoded: the reader never writes to this slot */
		pthread_mutex_unlock(&rq.mu);
		bad = sink_write(rq.sink, rq.slot[i].pcm, rq.slot[i].frames);
		pthread_mutex_lock(&rq.mu);
		rq.head = (rq.head + 1) % QSLOTS;
		rq.count--;
		if (bad) {
			rq.failed = 1;
			pthread_cond_broadcast(&rq.room);
			break;
		}
		pthread_cond_signal(&rq.room);
	}
	pthread_mutex_unlock(&rq.mu);
	return NULL;
}

static int rq_start(struct sink *s)
{
	pthread_attr_t at;
	sigset_t all, old;
	int r;

	rq.head = rq.count = rq.closed = rq.abort = rq.failed = 0;
	rq.sink = s;
	pthread_attr_init(&at);
	pthread_attr_setstacksize(&at, 1 << 20);   /* LAME keeps sizeable arrays on the stack; musl's default is 128 KiB */
	sigfillset(&all);
	pthread_sigmask(SIG_BLOCK, &all, &old);    /* SIGTERM is for the main thread */
	r = pthread_create(&rq.th, &at, rq_worker, NULL);
	pthread_sigmask(SIG_SETMASK, &old, NULL);
	pthread_attr_destroy(&at);
	return r;
}

/* queue one chunk; 0 when the encoder has failed or the rip is being stopped */
static int rq_push(const int16_t *pcm, int frames)
{
	int ok = 1, i;

	pthread_mutex_lock(&rq.mu);
	while (rq.count == QSLOTS && !rq.failed && !g_stop) {
		struct timespec until;

		clock_gettime(CLOCK_REALTIME, &until);   /* a timed wait: SIGTERM does not wake a condition variable */
		until.tv_nsec += 100000000L;
		if (until.tv_nsec >= 1000000000L) {
			until.tv_sec++;
			until.tv_nsec -= 1000000000L;
		}
		pthread_cond_timedwait(&rq.room, &rq.mu, &until);
	}
	if (rq.failed || g_stop) {
		ok = 0;
	} else {
		i = (rq.head + rq.count) % QSLOTS;
		memcpy(rq.slot[i].pcm, pcm, (size_t)frames * 4);
		rq.slot[i].frames = frames;
		rq.count++;
		pthread_cond_signal(&rq.more);
	}
	pthread_mutex_unlock(&rq.mu);
	return ok;
}

/* wait for the queue to be written out (or throw it away) and for the thread to end; 0 when nothing failed */
static int rq_finish(int discard)
{
	int failed;

	pthread_mutex_lock(&rq.mu);
	if (discard)
		rq.abort = 1;
	rq.closed = 1;
	pthread_cond_broadcast(&rq.more);
	pthread_mutex_unlock(&rq.mu);
	pthread_join(rq.th, NULL);
	failed = rq.failed;
	return failed ? -1 : 0;
}

/* More unreadable sectors than this in one track and the disc is given up: it would take hours, and the file would be
 * mostly silence. */
#define BAD_LIMIT 1500

static int cmd_cd(int only, const char *dir_arg, int fmt)
{
	struct stat_info st;
	struct toc t;
	struct cdtext ct;
	char base[400], album_dir[400], date[40];
	static uint8_t raw[CHUNK * CD_FRAMESIZE_RAW];
	static int16_t pcm[CHUNK * 588 * 2];
	const char *ext = fmt == RIP_FLAC ? "flac" : "mp3";
	uint32_t sig;
	time_t now = time(NULL);
	struct tm tm;
	int fd, r, trk, ntracks = 0, done = 0, ripped = 0, kept = 0;

	memset(&st, 0, sizeof st);
	crc_init();
	{
		const char *ft = getenv("X45REC_FAULT");

		if (ft)
			sscanf(ft, "%d-%d", &fault_lo, &fault_hi);
	}
	if (dir_arg)
		snprintf(base, sizeof base, "%s", dir_arg);
	else if (find_target(base, sizeof base) < 0) {
		fail(&st, "no writable disk");
		return 2;
	}
	fd = open(CDDEV, O_RDONLY | O_NONBLOCK);
	if (fd < 0 || (r = read_toc(fd, &t)) < 0) {
		fail(&st, "no disc");
		return 3;
	}
	sig = toc_sig(&t);
	for (trk = t.first; trk <= t.last; trk++)
		if (t.audio[trk] && (!only || trk == only))
			ntracks++;
	if (!ntracks) {   /* no audio track: a video DVD has its sound in files, not in tracks */
		close(fd);
		return cmd_dvd(only, base, sig, &st);
	}
	load_cdtext(&ct);
	cdtext_utf8(&ct);
	{
		char name[260];

		if (ct.artist[0] || ct.album[0]) {
			char a[120], b[120];

			snprintf(a, sizeof a, "%s", ct.artist[0] ? ct.artist : "Unknown Artist");
			snprintf(b, sizeof b, "%s", ct.album[0] ? ct.album : "Audio CD");
			sanitize(a);
			sanitize(b);
			snprintf(name, sizeof name, "%s - %s", a, b);
		} else {
			/* a disc nothing is known about: its number keeps it apart from the next such disc */
			snprintf(name, sizeof name, "Unknown Artist - Audio CD %08x", (unsigned)sig);
		}
		pick_dir(album_dir, sizeof album_dir, base, name, sig);
	}
	snprintf(st.dir, sizeof st.dir, "%s", album_dir);
	if (now > 1700000000 && localtime_r(&now, &tm))
		strftime(date, sizeof date, "%Y-%m-%d %H:%M", &tm);
	else
		snprintf(date, sizeof date, "clock not set");
	log_line(album_dir, "\n# x45rec rip, %s\ndisc %08x\nformat %s\nupc %s\n"
		 "# track <n> <ok|damaged> <format> <crc32 of the PCM> <unreadable sectors> <peak %%> <frames> <file> <note>\n",
		 date, (unsigned)sig, ext, ct.upc);
	signal(SIGTERM, on_term);
	signal(SIGINT, on_term);
	st.state = "ripping";
	st.tracks = ntracks;
	write_status(&st);

	for (trk = t.first; trk <= t.last && !g_stop; trk++) {
		int32_t lba, end;
		char name[200], path[600], part[610], pfile[300], note[160], full[700];
		struct trktags tg;
		struct sink sink;
		struct rdstat rs;
		long long frames = 0;
		long last = now_ms() - 1000;
		uint32_t crc = 0xffffffffu, pcrc = 0;
		int ok = 1, peak = 0, pok = 0, hadlog, toobad = 0;

		if (!t.audio[trk] || (only && trk != only))
			continue;
		st.track = ++done;
		snprintf(name, sizeof name, "%02d %s", trk, ct.title[trk][0] ? ct.title[trk] : "Track");
		sanitize(name);
		/* the same track already ripped whole in this format (an interrupted rip, a second go): it stays as it is */
		if (log_find(album_dir, trk, ext, &pcrc, &pok, pfile, sizeof pfile) && pok) {
			snprintf(full, sizeof full, "%s/%s", album_dir, pfile);
			if (file_exists(full)) {
				kept++;
				fprintf(stderr, "x45rec: track %d is there already, kept\n", trk);
				continue;
			}
		}
		hadlog = log_find(album_dir, trk, NULL, &pcrc, &pok, pfile, sizeof pfile);   /* for the comparison below */
		unique_file(path, sizeof path, album_dir, name, ext);
		snprintf(part, sizeof part, "%s.part", path);
		unlink(part);   /* a link or an old half file on the disk is replaced, never written through */
		snprintf(st.file, sizeof st.file, "%s", path);
		lba = t.start[trk];
		end = track_end(&t, trk);
		tg.title = ct.title[trk][0] ? ct.title[trk] : name;
		tg.artist = ct.perf[trk][0] ? ct.perf[trk] : ct.artist;
		tg.album = ct.album;
		tg.albumartist = ct.artist;
		tg.composer = ct.composer[trk];
		tg.genre = ct.genre;
		tg.track = trk;
		tg.total = t.last - t.first + 1;
		tg.frames = end - lba;
		if (sink_open(&sink, fmt, part, &tg) < 0) {
			fail(&st, "cannot write the file");
			close(fd);
			return 5;
		}
		if (rq_start(&sink) != 0) {
			sink_close(&sink, 0);
			unlink(part);
			fail(&st, "cannot start the encoder");
			close(fd);
			return 5;
		}
		memset(&rs, 0, sizeof rs);
		while (lba < end && !g_stop) {
			int n = end - lba < CHUNK ? (int)(end - lba) : CHUNK, i;
			int bad = read_frames(fd, lba, n, raw, &rs);

			if (bad) {
				st.bad += bad;
				fprintf(stderr, "x45rec: track %d: %d unreadable sector(s) at %d\n", trk, bad, (int)lba);
				snprintf(st.msg, sizeof st.msg, "%d unreadable sectors so far", st.bad);
				write_status(&st);
				if (rs.bad > BAD_LIMIT) {
					toobad = 1;
					break;
				}
			}
			crc = crc_update(crc, raw, (size_t)n * CD_FRAMESIZE_RAW);
			for (i = 0; i < n * 588 * 2; i++) {
				int v = (int16_t)(raw[i * 2] | raw[i * 2 + 1] << 8);

				pcm[i] = (int16_t)v;
				if (v < 0)
					v = -v;
				if (v > peak)
					peak = v;
			}
			if (!rq_push(pcm, n * 588))
				break;   /* stopped, or the encoder failed: rq_finish tells which */
			lba += n;
			frames += n * 588;
			if (now_ms() - last > 500) {
				last = now_ms();
				st.secs = frames / 44100;
				st.bytes = file_size(part);
				write_status(&st);
			}
		}
		if (rq_finish(g_stop || toobad) != 0)
			ok = 0;
		if (sink_close(&sink, ok && !g_stop && !toobad) != 0)
			ok = 0;
		if (toobad) {
			unlink(part);
			log_line(album_dir, "# track %d given up: more than %d unreadable sectors\n", trk, BAD_LIMIT);
			fail(&st, "the disc cannot be read (scratched or dirty?)");
			close(fd);
			return 7;
		}
		if (!ok) {
			unlink(part);
			fail(&st, "encoder failed (disk full?)");
			close(fd);
			return 6;
		}
		if (g_stop) {
			unlink(part);   /* a half-ripped track is not kept */
			log_line(album_dir, "# stopped during track %d\n", trk);
			break;
		}
		if (rename(part, path) != 0) {
			unlink(part);
			fail(&st, "cannot write the file");
			close(fd);
			return 5;
		}
		crc ^= 0xffffffffu;
		note[0] = '\0';
		if (rs.bad) {
			int k, off = snprintf(note, sizeof note, "unreadable at sector");

			for (k = 0; k < rs.bad && k < 8 && off < (int)sizeof note - 12; k++)
				off += snprintf(note + off, sizeof note - (size_t)off, " %d", (int)rs.at[k]);
			if (rs.bad > 8)
				snprintf(note + off, sizeof note - (size_t)off, " ...");
		} else if (hadlog) {
			snprintf(note, sizeof note, crc == pcrc ? "same audio as the earlier rip" : "differs from the earlier rip");
		}
		{
			const char *base_name = strrchr(path, '/');

			log_line(album_dir, "track\t%02d\t%s\t%s\t%08x\t%d\t%d\t%lld\t%s\t%s\n", trk, rs.bad ? "damaged" : "ok", ext,
				 (unsigned)crc, rs.bad, (peak * 100 + 16383) / 32768, frames, base_name ? base_name + 1 : path, note);
		}
		ripped++;
	}
	close(fd);
	if (strcmp(st.state, "error")) {
		if (st.bad)
			snprintf(st.msg, sizeof st.msg, "ripped, but %d sectors could not be read: see the rip log", st.bad);
		else if (kept && !ripped)
			snprintf(st.msg, sizeof st.msg, "already ripped, nothing was overwritten");
		else
			st.msg[0] = '\0';
		st.state = "done";
		write_status(&st);
	}
	return 0;
}

int main(int argc, char **argv)
{
	const char *dir = NULL, *name = NULL;
	int rate = 192000, bits = 24, i, only = 0, fmt = RIP_FLAC;

	for (i = 2; i < argc; i++) {
		if (!strcmp(argv[i], "-r") && i + 1 < argc)
			rate = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-f") && i + 1 < argc)
			fmt = parse_format(argv[++i]);
		else if (!strcmp(argv[i], "-b") && i + 1 < argc)
			bits = atoi(argv[++i]) == 16 ? 16 : 24;
		else if (!strcmp(argv[i], "-n") && i + 1 < argc)
			name = argv[++i];
		else if (!strcmp(argv[i], "-d") && i + 1 < argc)
			dir = argv[++i];
		else if (argv[i][0] != '-')
			only = atoi(argv[i]);
	}
	if (argc >= 2 && !strcmp(argv[1], "in"))
		return cmd_in(rate, bits, name, dir);
	if (argc >= 2 && !strcmp(argv[1], "cd"))
		return cmd_cd(only, dir, fmt);
	if (argc >= 2 && !strcmp(argv[1], "dir")) {
		char d[300];

		if (find_target(d, sizeof d) < 0)
			return 1;
		puts(d);
		return 0;
	}
	fprintf(stderr, "usage: x45rec in [-r rate] [-b 16|24] [-n name] [-d dir] | cd [track] [-d dir] [-f flac|mp3|mp3v0] | dir\n");
	return 1;
}
