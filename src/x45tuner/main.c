/* x45tuner - DAB+/FM radio module driver (KSM module on /dev/ttymxc1, 38400 8N1).
 *
 *   x45tuner run fm|dab    daemon: owns the serial port until killed or its parent changes
 *   x45tuner reset         pulse the module's reset line (GPIO5.15) and exit
 *
 * Control: write one command per line to /tmp/x45tuner.cmd
 *   fm:   tune <10kHz>   step <+n|-n>   seek up|down   fmscan
 *   dab:  scan   select <index>
 *   both: quit
 * State:  /tmp/x45tuner.status (key=value, rewritten on change, at most ~4x/s)
 * Lists:  etc/fm.list ("freq|name"), etc/dab.list ("hexsid6|hexc|hext|label")
 *
 * Wire format: 5A A5, 16-bit big-endian payload length, payload (opcode first),
 * one checksum byte = XOR of the payload. See docs/X45-TUNER.md.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define TTY     "/dev/ttymxc1"
#define CMDFIFO "/tmp/x45tuner.cmd"
#define STATUS  "/tmp/x45tuner.status"
#define ETC     "/home/root/x45new/etc"

#define MAXSVC 128

static volatile sig_atomic_t g_stop;
static void on_term(int s) { (void)s; g_stop = 1; }

static long now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* A long is 32 bits here: now_ms() turns negative after 24.8 days of uptime. Times are compared by their difference. */
static long ms_diff(long a, long b)
{
	return (long)((unsigned long)a - (unsigned long)b);
}

static int ms_after(long a, long b)   /* a is later than b */
{
	return ms_diff(a, b) > 0;
}

static int ms_due(long t)   /* the time t has come */
{
	return ms_diff(now_ms(), t) >= 0;
}

/* ------------------------------------------------------------- reset line --- */

static int do_reset(void)
{
	/* GPIO5 data register; bit 15 is the module's reset line (active low) */
	int fd = open("/dev/mem", O_RDWR | O_SYNC);
	volatile uint32_t *g;

	if (fd < 0) {
		perror("/dev/mem");
		return -1;
	}
	g = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x020AC000);
	if (g == MAP_FAILED) {
		perror("mmap");
		close(fd);
		return -1;
	}
	g[0] &= ~(1u << 15);
	usleep(150000);
	g[0] |= 1u << 15;
	munmap((void *)g, 0x1000);
	close(fd);
	usleep(1200000);
	return 0;
}

/* ----------------------------------------------------------------- state --- */

struct svc {
	uint32_t sid;
	uint8_t comp, type;
	char label[17];
};

enum { M_FM, M_DAB };
enum { S_IDLE, S_TUNING, S_SEEK, S_SCAN, S_LIST };

static struct {
	int fd, cmd, mode;
	/* shared */
	int state, rssi, snr, dirty, frames;
	long last_status, state_t;
	/* fm */
	int freq, found, seek_dir, fmscan, fmscan_start, fmscan_n, listrev;
	char ps[9], rt[65];     /* RDS programme service name / radiotext */
	struct { int freq; char name[9]; } fmlist[64];
	/* dab */
	int scan_n, scan_band, nsvc, sel, q, sq;   /* q: CD sync flag; sq: C9 reception 0..100 */
	struct svc svc[MAXSVC];
	char label[17], ens[17], dls[130], when[40];
	int slide_n;             /* counts completed slideshow images */
	uint32_t cur_sid;
	long list_t, seek_at, cand_at;
	int cand, scan_last, hit;
	int list_dirty, asked;
} T;

/* DAB MOT slideshow. The module hands over MOT segments as `cb` frames:
 *   cb 73 ctr 80 00 12 <tid2> e0 <hl> <MOT header, hl bytes> <crc2>
 *   cb 74 ctr <80 = last segment> <idx> 12 <tid2> <e0|e2 | len> <len> <data> <crc2>
 * (len = 13 bits from the two bytes after the transport id; data starts at byte 10).
 * The carousel repeats an object every few seconds, so segments are collected by
 * index until 0..last are all present, then the image is written to SLIDE_FILE. */
#define SLIDE_FILE "/tmp/x45-slide.img"
#define MOT_MAXSEG 512
#define MOT_MAXBYTES (256 * 1024)
#define MOT_OBJS 3

struct mot_obj {
	int used, last, got, bytes;
	uint16_t tid;
	long t;
	struct { uint8_t *d; int len; } seg[MOT_MAXSEG];
};
static struct mot_obj mot[MOT_OBJS];
static int mot_done_tid = -1;

static void mot_free(struct mot_obj *m)
{
	int i;

	for (i = 0; i < MOT_MAXSEG; i++)
		free(m->seg[i].d);
	memset(m, 0, sizeof *m);
}

static void mot_reset(void)
{
	int i;

	for (i = 0; i < MOT_OBJS; i++)
		mot_free(&mot[i]);
	mot_done_tid = -1;
	unlink(SLIDE_FILE);
	T.slide_n++;      /* tells the UI to drop the old picture */
	T.dirty = 1;
}

static void mot_finish(struct mot_obj *m)
{
	FILE *f = fopen(SLIDE_FILE ".tmp", "wb");
	int i;

	if (f) {
		for (i = 0; i <= m->last; i++)
			fwrite(m->seg[i].d, 1, m->seg[i].len, f);
		if (fclose(f) == 0 && rename(SLIDE_FILE ".tmp", SLIDE_FILE) == 0) {
			T.slide_n++;
			T.dirty = 1;
		}
	}
	mot_done_tid = m->tid;
	mot_free(m);
}

static void mot_frame(const uint8_t *p, int n)
{
	struct mot_obj *m = NULL;
	uint16_t tid;
	int idx, len, i;

	if (n < 12 || p[1] != 0x74)       /* headers (0x73) carry only name and size */
		return;
	idx = p[4];
	tid = (p[6] << 8) | p[7];
	len = ((p[8] & 0x1f) << 8) | p[9];
	if (len <= 0 || 10 + len + 2 > n || idx >= MOT_MAXSEG || tid == mot_done_tid)
		return;
	for (i = 0; i < MOT_OBJS; i++)
		if (mot[i].used && mot[i].tid == tid)
			m = &mot[i];
	if (!m) {
		/* a free slot, else the one that has gone longest without a segment (compared by difference: now_ms() wraps) */
		for (i = 0; i < MOT_OBJS; i++)
			if (!mot[i].used) {
				m = &mot[i];
				break;
			}
		if (!m) {
			m = &mot[0];
			for (i = 1; i < MOT_OBJS; i++)
				if (ms_after(m->t, mot[i].t))
					m = &mot[i];
		}
		mot_free(m);
		m->used = 1;
		m->tid = tid;
		m->last = -1;
	}
	m->t = now_ms();
	if (!m->seg[idx].d) {
		m->seg[idx].d = malloc(len);
		if (!m->seg[idx].d)
			return;
		memcpy(m->seg[idx].d, p + 10, len);
		m->seg[idx].len = len;
		m->got++;
		m->bytes += len;
		if (m->bytes > MOT_MAXBYTES) {
			mot_free(m);
			return;
		}
	}
	if (p[3] & 0x80)
		m->last = idx;
	if (m->last >= 0 && m->got == m->last + 1)
		mot_finish(m);
}

static uint8_t rxb[8192];
static int rxn;

static void tx(const uint8_t *p, int n)
{
	uint8_t f[300];
	uint8_t c = 0;
	int i;

	f[0] = 0x5a; f[1] = 0xa5; f[2] = n >> 8; f[3] = n & 0xff;
	memcpy(f + 4, p, n);
	for (i = 0; i < n; i++)
		c ^= p[i];
	f[4 + n] = c;
	if (write(T.fd, f, 5 + n) < 0)
		perror("x45tuner: write");
}

static void tx1(int op, ...)
{
	uint8_t b[16];
	int n = 0, v;
	__builtin_va_list ap;

	b[n++] = op;
	__builtin_va_start(ap, op);
	while ((v = __builtin_va_arg(ap, int)) >= 0 && n < 16)
		b[n++] = v;
	__builtin_va_end(ap);
	tx(b, n);
}

/* EBU Latin charset (DAB text) above 0x7f -> UTF-8, rows 0x80..0xDF */
static const char *const ebu[96] = {
	"\303\241","\303\240","\303\251","\303\250","\303\255","\303\254","\303\263","\303\262","\303\272","\303\271","\303\221","\303\207","\305\236","\303\237","\302\241","\304\262",
	"\303\242","\303\244","\303\252","\303\253","\303\256","\303\257","\303\264","\303\266","\303\273","\303\274","\303\261","\303\247","\305\237","\304\237","\304\261","\304\263",
	"\302\252","\316\261","\302\251","\342\200\260","\304\236","\304\233","\305\210","\305\221","\317\200","\342\202\254","\302\243","$","\342\206\220","\342\206\221","\342\206\222","\342\206\223",
	"\302\272","\302\271","\302\262","\302\263","\302\261","\304\260","\305\204","\305\261","\302\265","\302\277","\303\267","\302\260","\302\274","\302\275","\302\276","\302\247",
	"\303\201","\303\200","\303\211","\303\210","\303\215","\303\214","\303\223","\303\222","\303\232","\303\231","\305\230","\304\214","\305\240","\305\275","\303\220","\304\277",
	"\303\202","\303\204","\303\212","\303\213","\303\216","\303\217","\303\224","\303\226","\303\233","\303\234","\305\231","\304\215","\305\241","\305\276","\304\221","\305\200",
};

static void text_out(char *dst, size_t cap, const uint8_t *s, int n)
{
	size_t o = 0;
	int i;

	for (i = 0; i < n && s[i]; i++) {
		const char *r;
		char one[2] = { (char)s[i], 0 };

		if (s[i] >= 0x80 && s[i] < 0xe0)
			r = ebu[s[i] - 0x80];
		else if (s[i] >= 0x20 && s[i] < 0x7f)
			r = one;
		else if (s[i] >= 0xe0)
			r = "?";
		else
			r = " ";
		if (o + strlen(r) >= cap)
			break;
		memcpy(dst + o, r, strlen(r));
		o += strlen(r);
	}
	while (o > 0 && dst[o - 1] == ' ')
		o--;
	dst[o] = '\0';
}

/* ----------------------------------------------------------- persistence --- */

static void save_fm(void)
{
	FILE *f = fopen(ETC "/fm.list.tmp", "w");
	int i, j;

	T.listrev++;
	/* keep the list in frequency order */
	for (i = 1; i < T.fmscan_n; i++)
		for (j = i; j > 0 && T.fmlist[j].freq < T.fmlist[j - 1].freq; j--) {
			int tf = T.fmlist[j].freq;
			char tn[sizeof T.fmlist[0].name];

			memcpy(tn, T.fmlist[j].name, sizeof tn);
			T.fmlist[j].freq = T.fmlist[j - 1].freq;
			memcpy(T.fmlist[j].name, T.fmlist[j - 1].name, sizeof tn);
			T.fmlist[j - 1].freq = tf;
			memcpy(T.fmlist[j - 1].name, tn, sizeof tn);
		}

	if (!f)
		return;
	for (i = 0; i < T.fmscan_n; i++)
		fprintf(f, "%d|%s\n", T.fmlist[i].freq, T.fmlist[i].name);
	fclose(f);
	rename(ETC "/fm.list.tmp", ETC "/fm.list");
}

static int svc_cmp(const void *a, const void *b)
{
	return strcasecmp(((const struct svc *)a)->label, ((const struct svc *)b)->label);
}

static void save_dab(void)
{
	FILE *f;
	int i;

	if (T.nsvc <= 0)
		return;     /* an interrupted scan must not wipe the saved list */
	qsort(T.svc, T.nsvc, sizeof T.svc[0], svc_cmp);
	f = fopen(ETC "/dab.list.tmp", "w");
	if (!f)
		return;
	for (i = 0; i < T.nsvc; i++)
		fprintf(f, "%06x|%02x|%02x|%s\n", T.svc[i].sid, T.svc[i].comp,
			T.svc[i].type, T.svc[i].label);
	fclose(f);
	rename(ETC "/dab.list.tmp", ETC "/dab.list");
}

static void save_last(void)
{
	FILE *f = fopen(ETC "/tuner.last.tmp", "w");

	if (!f)
		return;
	if (T.mode == M_FM)
		fprintf(f, "%d\n", T.freq);
	else if (T.nsvc > T.sel)
		fprintf(f, "%06x|%02x\n", T.svc[T.sel].sid, T.svc[T.sel].comp);
	fclose(f);
	rename(ETC "/tuner.last.tmp", T.mode == M_FM ? ETC "/fm.last" : ETC "/dab.last");
}

static void load_dab(void)
{
	FILE *f = fopen(ETC "/dab.list", "r");
	char line[128];

	T.nsvc = 0;
	if (!f)
		return;
	while (fgets(line, sizeof line, f) && T.nsvc < MAXSVC) {
		unsigned sid, c, t;
		char *l = strchr(line, '|');
		struct svc *s = &T.svc[T.nsvc];

		if (!l || sscanf(line, "%x|%x|%x|", &sid, &c, &t) != 3)
			continue;
		l = strchr(l + 1, '|');
		l = l ? strchr(l + 1, '|') : NULL;
		if (!l)
			continue;
		s->sid = sid; s->comp = c; s->type = t;
		snprintf(s->label, sizeof s->label, "%.16s", l + 1);
		s->label[strcspn(s->label, "\n")] = '\0';
		T.nsvc++;
	}
	fclose(f);
}

static void load_fm(void)
{
	FILE *f = fopen(ETC "/fm.list", "r");
	char line[64];

	T.fmscan_n = 0;
	if (!f)
		return;
	while (fgets(line, sizeof line, f) && T.fmscan_n < 64) {
		int fr;
		char *b = strchr(line, '|');

		if (!b || sscanf(line, "%d", &fr) != 1)
			continue;
		T.fmlist[T.fmscan_n].freq = fr;
		snprintf(T.fmlist[T.fmscan_n].name, 9, "%.8s", b + 1);
		T.fmlist[T.fmscan_n].name[strcspn(T.fmlist[T.fmscan_n].name, "\n")] = '\0';
		T.fmscan_n++;
	}
	fclose(f);
}

/* ---------------------------------------------------------------- status --- */

static const char *const state_name[] = { "idle", "tuning", "seek", "scan", "list" };

static void write_status(void)
{
	FILE *f = fopen(STATUS ".tmp", "w");

	if (!f)
		return;
	fprintf(f, "mode=%s\nstate=%s\nrssi=%d\nsnr=%d\n", T.mode == M_FM ? "fm" : "dab",
		state_name[T.state], T.rssi, T.snr);
	if (T.mode == M_FM) {
		fprintf(f, "freq=%d\nfound=%d\nps=%s\nrt=%s\nnpreset=%d\nfmscan=%d\nlistrev=%d\n", T.freq,
			T.found, T.ps, T.rt, T.fmscan_n, T.fmscan, T.listrev);
	} else {
		fprintf(f, "scan=%d\nnsvc=%d\nsel=%d\nq=%d\nsq=%d\nsvc=%s\nens=%s\ntext=%s\ntime=%s\nslide=%d\n",
			T.scan_n, T.nsvc, T.sel, T.q, T.sq, T.label, T.ens, T.dls, T.when, T.slide_n);
	}
	fclose(f);
	rename(STATUS ".tmp", STATUS);
	T.dirty = 0;
	T.last_status = now_ms();
}

/* ------------------------------------------------------------- operations --- */

static void set_state(int s)
{
	T.state = s;
	T.state_t = now_ms();
	T.dirty = 1;
}

static void save_last(void);

static void fm_tune(int f)
{
	if (f < 8750)
		f = 10800;
	if (f > 10800)
		f = 8750;
	T.freq = f;
	T.found = 0;
	T.ps[0] = T.rt[0] = '\0';
	tx1(0x03, f >> 8, f & 0xff, -1);
	if (!T.fmscan)
		save_last();
	set_state(S_TUNING);
}

/* the user's station list (etc/fm.list): add the current frequency, drop one */
static int fm_find(int f)
{
	int i;

	for (i = 0; i < T.fmscan_n; i++)
		if (abs(T.fmlist[i].freq - f) < 5)
			return i;
	return -1;
}

static void fm_add(int f, const char *name)
{
	if (fm_find(f) >= 0 || T.fmscan_n >= 64)
		return;
	T.fmlist[T.fmscan_n].freq = f;
	snprintf(T.fmlist[T.fmscan_n].name, sizeof T.fmlist[0].name, "%.8s", name);
	T.fmscan_n++;
	save_fm();
	T.dirty = 1;
}

static void fm_del(int f)
{
	int i = fm_find(f);

	if (i < 0)
		return;
	memmove(&T.fmlist[i], &T.fmlist[i + 1], (size_t)(T.fmscan_n - i - 1) * sizeof T.fmlist[0]);
	T.fmscan_n--;
	save_fm();
	T.dirty = 1;
}

static void fm_seek(int dir)
{
	T.seek_dir = dir;
	T.ps[0] = T.rt[0] = '\0';
	T.found = 0;
	tx1(0x08, dir < 0 ? 1 : 0, 0, -1);
	set_state(S_SEEK);
}

static void dab_select(int i)
{
	struct svc *s;

	if (i < 0 || i >= T.nsvc)
		return;
	s = &T.svc[i];
	T.sel = i;
	T.q = T.sq = 0;
	T.dls[0] = '\0';
	mot_reset();
	snprintf(T.label, sizeof T.label, "%s", s->label);
	T.cur_sid = s->sid;
	save_last();
	tx1(0x05, 0, 0, (s->sid >> 8) & 0xff, s->sid & 0xff, 0, s->comp, s->type, -1);
	set_state(S_TUNING);
}

/* The list as it was before a scan started. A scan that finds nothing (or is
 * cancelled before it finds anything) puts it back; one that has found stations
 * replaces it. */
static struct svc dab_bak[MAXSVC];
static int dab_bak_n, dab_bak_on;

static void dab_scan(void)
{
	memcpy(dab_bak, T.svc, sizeof dab_bak);
	dab_bak_n = T.nsvc;
	dab_bak_on = 1;
	T.nsvc = 0;
	T.scan_n = 0;
	T.label[0] = T.dls[0] = '\0';
	tx1(0x04, 1, 1, -1);
	set_state(S_SCAN);
}

static void module_init(void)
{
	tcflush(T.fd, TCIOFLUSH);
	if (T.mode == M_FM) {
		tx1(0x24, 1, -1);
		tx1(0x1c, 2, 0, 0x22, 0x2e, 0x2a, 0x30, -1);
	}
	tx1(0x0c, -1);
	tx1(0x01, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, -1);
	tx1(0x1d, 1, 0, 1, 1, 1, 1, -1);
	tx1(0x1b, 2, -1);
	tx1(0x23, 1, 0, 0, 0, -1);
	tx1(0x02, T.mode == M_FM ? 1 : 2, -1);
}

/* ------------------------------------------------------------- frame input --- */

static void svc_add(const uint8_t *p, int n)
{
	struct svc s;
	int i;

	if (n < 27)
		return;
	memset(&s, 0, sizeof s);
	s.type = p[4];
	s.sid = ((uint32_t)p[7] << 8) | p[8];
	s.comp = p[10];
	text_out(s.label, sizeof s.label, p + 11, 16);
	for (i = 0; i < T.nsvc; i++)
		if (T.svc[i].sid == s.sid && T.svc[i].comp == s.comp)
			break;
	if (i == T.nsvc) {
		if (T.nsvc >= MAXSVC || (!s.sid && !s.label[0]))
			return;
		T.nsvc++;
	} else if (!s.label[0]) {
		return;
	}
	T.svc[i] = s;
	T.list_t = now_ms();
	T.list_dirty = 1;
	T.dirty = 1;
}

static void on_frame(const uint8_t *p, int n)
{
	T.frames++;
	switch (p[0]) {
	case 0xf4:
		break;
	case 0xf1:
		if (p[1] == 0x01 && n >= 5 && T.mode == M_FM) {
			T.freq = (p[2] << 8) | p[3];
			if (getenv("X45TUNER_DEBUG"))
				fprintf(stderr, "F1 %d found=%d state=%d\n", T.freq, p[4], T.state);
			T.found = p[4] & 1;
			/* the seek timeout is for a silent module, not a long sweep */
			if (T.state == S_SEEK)
				T.state_t = now_ms();
			if (T.state == S_SEEK && T.found) {
				T.hit = 1;
				set_state(S_TUNING);
			}
			T.dirty = 1;
		} else if (p[1] == 0x02 && n >= 28 && T.mode == M_DAB) {
			text_out(T.label, sizeof T.label, p + 6, 16);
			T.dirty = 1;
		}
		break;
	case 0xc8:
		if (n >= 4) {
			T.rssi = p[1];
			T.snr = p[2];
			T.dirty = 1;
		}
		break;
	case 0xd2:
		if (n >= 10) {
			text_out(T.ps, sizeof T.ps, p + 2, 8);
			T.dirty = 1;
			if (T.mode == M_FM && T.ps[0]) {
				int i = fm_find(T.freq);

				/* a listed station learns its name from RDS */
				if (i >= 0 && strncmp(T.fmlist[i].name, T.ps, 8)) {
					snprintf(T.fmlist[i].name, sizeof T.fmlist[i].name, "%.8s", T.ps);
					save_fm();
				}
			}
		}
		break;
	case 0xd3:
		/* RDS RadioText: flag byte, then up to 64 characters */
		if (n >= 3) {
			text_out(T.rt, sizeof T.rt, p + 2, n - 2);
			T.dirty = 1;
		}
		break;
	case 0xc6:
		/* ensemble time: years since 1900, month, day, hour, minute, second */
		if (n >= 7) {
			snprintf(T.when, sizeof T.when, "%04d-%02d-%02d %02d:%02d:%02d",
				 1900 + p[1], p[2], p[3], p[4], p[5], p[6]);
			T.dirty = 1;
		}
		break;
	case 0xcb:
		if (T.mode == M_DAB)
			mot_frame(p, n);
		break;
	case 0xcc:
		if (n >= 17)
			text_out(T.ens, sizeof T.ens, p + 1, 16);
		T.dirty = 1;
		break;
	case 0xc9:
		/* reception report, every 2 s: `C9 <a> <b> <c> <quality 0..100> 00 <d> <e> 04`. The quality is about
		 * 0x60-0x64 on a strong ensemble and 0x04-0x2a on one that drops out; the other bytes differ per
		 * ensemble and are not understood */
		if (n >= 5 && T.mode == M_DAB) {
			T.sq = p[4] > 100 ? 100 : p[4];
			T.dirty = 1;
		}
		break;
	case 0xcd:
		if (n >= 2) {
			T.q = p[1];
			if (T.state == S_TUNING && T.mode == M_DAB && T.q > 0)
				set_state(S_IDLE);
			T.dirty = 1;
		}
		break;
	case 0xc5:
		if (n >= 3) {
			text_out(T.dls, sizeof T.dls, p + 2, n - 2);
			T.dirty = 1;
		}
		break;
	case 0xd1:
		if (n >= 3 && T.state == S_SCAN) {
			T.scan_n = p[1];
			T.scan_band = p[2];
			T.dirty = 1;
		}
		break;
	case 0xbe:
		svc_add(p, n);
		break;
	case 0xfe:
		if (n >= 1 && T.mode == M_DAB && T.state == S_SCAN) {
			tx1(0x21, -1);
			set_state(S_LIST);
			T.asked = 0;
			T.list_t = now_ms();
		}
		break;
	default:
		break;
	}
}

static void rx_pump(void)
{
	ssize_t r = read(T.fd, rxb + rxn, sizeof rxb - rxn);

	if (r > 0)
		rxn += r;
	for (;;) {
		int i = 0, len, k;
		uint8_t c = 0;

		while (i + 1 < rxn && !(rxb[i] == 0x5a && rxb[i + 1] == 0xa5))
			i++;
		if (i) {
			memmove(rxb, rxb + i, rxn - i);
			rxn -= i;
		}
		if (rxn < 4)
			break;
		len = (rxb[2] << 8) | rxb[3];
		if (len < 1 || len > 1024) {
			memmove(rxb, rxb + 2, rxn - 2);
			rxn -= 2;
			continue;
		}
		if (rxn < 5 + len)
			break;
		for (k = 0; k < len; k++)
			c ^= rxb[4 + k];
		if (c != rxb[4 + len]) {
			memmove(rxb, rxb + 2, rxn - 2);
			rxn -= 2;
			continue;
		}
		on_frame(rxb + 4, len);
		memmove(rxb, rxb + 5 + len, rxn - 5 - len);
		rxn -= 5 + len;
	}
	if (rxn == sizeof rxb)
		rxn = 0;
}

/* --------------------------------------------------------------- commands --- */

static void fmscan_next(void);
static void fmscan_done(void);
static struct { int freq; char name[9]; } fm_bak[64];
static int fm_bak_n, fm_bak_on;

static void command(char *line)
{
	int a;
	char w[16];

	line[strcspn(line, "\r\n")] = '\0';
	if (!strcmp(line, "quit")) {
		g_stop = 1;
	} else if (T.mode == M_FM && sscanf(line, "tune %d", &a) == 1) {
		T.fmscan = 0;
		fm_tune(a);
	} else if (T.mode == M_FM && sscanf(line, "step %d", &a) == 1) {
		T.fmscan = 0;
		fm_tune(T.freq + a);
	} else if (T.mode == M_FM && sscanf(line, "seek %15s", w) == 1) {
		T.fmscan = 0;
		fm_seek(strcmp(w, "down") ? 1 : -1);
	} else if (!strcmp(line, "cancel")) {
		/* STOP during a scan: keep what has been found, or the old list if nothing */
		if (T.mode == M_FM && T.fmscan) {
			fmscan_done();
		} else if (T.mode == M_DAB && (T.state == S_SCAN || T.state == S_LIST)) {
			tx1(0x21, -1);
			set_state(S_IDLE);
		}
	} else if (T.mode == M_FM && !strcmp(line, "fmscan")) {
		memcpy(fm_bak, T.fmlist, sizeof fm_bak);
		fm_bak_n = T.fmscan_n;
		fm_bak_on = 1;
		T.fmscan_n = 0;
		T.fmscan = 1;
		T.fmscan_start = 0;
		T.scan_last = 0;
		T.cand = 0;
		fm_tune(8750);
		T.seek_at = now_ms() + 1500;
	} else if (T.mode == M_FM && !strcmp(line, "fmadd")) {
		fm_add(T.freq, T.ps);
	} else if (T.mode == M_FM && sscanf(line, "fmdel %d", &a) == 1) {
		fm_del(a);
	} else if (T.mode == M_DAB && !strcmp(line, "scan")) {
		dab_scan();
	} else if (T.mode == M_DAB && sscanf(line, "select %d", &a) == 1) {
		dab_select(a);
	}
}

/* fmscan: seek up from the band start; every hit is recorded; done when the
 * seek wraps below the previous hit */
static void fmscan_next(void)
{
	/* a seek started right on a weak station does not move; step off it first */
	fm_tune(T.freq + 10);
	T.seek_at = now_ms() + 1000;
}

static void fmscan_done(void)
{
	fprintf(stderr, "fmscan_done at %d n=%d\n", T.freq, T.fmscan_n);
	T.fmscan = 0;
	T.cand = 0;
	T.seek_at = 0;
	if (fm_bak_on) {
		if (!T.fmscan_n) {
			memcpy(T.fmlist, fm_bak, sizeof fm_bak);
			T.fmscan_n = fm_bak_n;
		}
		fm_bak_on = 0;
	}
	save_fm();
	if (T.fmscan_n > 0)
		fm_tune(T.fmlist[0].freq);
	T.dirty = 1;
}

/* a seek stopped on a station: judge it once the module has reported its level */
static void fmscan_hit(void)
{
	if (!T.fmscan)
		return;
	if (T.freq <= T.scan_last || T.fmscan_n >= 60) {
		fmscan_done();
		return;
	}
	fprintf(stderr, "hit %d\n", T.freq);
	T.scan_last = T.freq;
	T.cand = T.freq;
	T.cand_at = now_ms() + 1300;
}

int main(int argc, char **argv)
{
	struct termios t;
	int tries;
	pid_t parent = getppid();
	int follow = getenv("X45CD_FOLLOW_PARENT") != NULL;
	char cbuf[256];
	int cn = 0;

	if (argc >= 2 && !strcmp(argv[1], "reset"))
		return do_reset() ? 1 : 0;
	if (argc < 3 || strcmp(argv[1], "run") ||
	    (strcmp(argv[2], "fm") && strcmp(argv[2], "dab"))) {
		fprintf(stderr, "usage: x45tuner run fm|dab | reset\n");
		return 1;
	}
	T.mode = !strcmp(argv[2], "fm") ? M_FM : M_DAB;
	T.freq = 8750;
	signal(SIGTERM, on_term);
	signal(SIGINT, on_term);

	T.fd = open(TTY, O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (T.fd < 0) {
		perror(TTY);
		return 2;
	}
	tcgetattr(T.fd, &t);
	cfmakeraw(&t);
	cfsetispeed(&t, B38400);
	cfsetospeed(&t, B38400);
	t.c_cflag |= CLOCAL | CREAD;
	t.c_cc[VMIN] = 0;
	t.c_cc[VTIME] = 0;
	tcsetattr(T.fd, TCSANOW, &t);

	unlink(CMDFIFO);
	mkfifo(CMDFIFO, 0600);
	T.cmd = open(CMDFIFO, O_RDWR | O_NONBLOCK);

	/* bring the module up. It answers at once when it is running; after a reset (ours, or the previous
	 * instance was cut off half way through its set-up) it needs several seconds before it listens at
	 * all, so the set-up is sent again every 1.5 s rather than resetting it again in the middle of its start. */
	{
		int reset_done = 0;

		if (getenv("X45TUNER_RESET")) {
			do_reset();
			tcflush(T.fd, TCIOFLUSH);
			reset_done = 1;
		}
		for (tries = 0; tries < 3 && !g_stop; tries++) {
			long t0 = now_ms(), last_init = -10000, budget = (tries == 0 && !reset_done) ? 1500 : 8000;
			int up = 0;

			while (now_ms() - t0 < budget && !up && !g_stop) {
				struct pollfd p = { T.fd, POLLIN, 0 };

				if (now_ms() - last_init >= 1500) {
					module_init();
					last_init = now_ms();
				}
				if (poll(&p, 1, 100) > 0)
					rx_pump();
				up = T.frames > 0;
			}
			if (up) {
				fprintf(stderr, "x45tuner: module answered after %ld ms (attempt %d)\n", now_ms() - t0, tries + 1);
				break;
			}
			if (g_stop)
				break;
			fprintf(stderr, "x45tuner: no answer, resetting the module\n");
			do_reset();
			reset_done = 1;
			tcflush(T.fd, TCIOFLUSH);
			rxn = 0;
		}
	}

	if (g_stop) {   /* told to leave while starting: do it at once, the UI is waiting */
		unlink(CMDFIFO);
		close(T.fd);
		return 0;
	}
	if (!T.frames) {
		/* no module on the UART (other X45 builds ship without one): say so
		 * for the UI and leave instead of idling on a dead port */
		FILE *sf = fopen(STATUS ".tmp", "w");

		fprintf(stderr, "x45tuner: module absent\n");
		if (sf) {
			fprintf(sf, "mode=%s\nstate=absent\n", T.mode == M_FM ? "fm" : "dab");
			fclose(sf);
			rename(STATUS ".tmp", STATUS);
		}
		unlink(CMDFIFO);
		close(T.fd);
		return 4;
	}
	if (T.mode == M_DAB)
		load_dab();
	else
		load_fm();
	set_state(S_IDLE);
	{
		FILE *lf = fopen(T.mode == M_FM ? ETC "/fm.last" : ETC "/dab.last", "r");
		unsigned sid, c;
		int fr, i;

		if (lf) {
			if (T.mode == M_FM && fscanf(lf, "%d", &fr) == 1) {
				usleep(300000);
				fm_tune(fr);
			} else if (T.mode == M_DAB && fscanf(lf, "%x|%x", &sid, &c) == 2) {
				for (i = 0; i < T.nsvc; i++)
					if (T.svc[i].sid == sid && T.svc[i].comp == c) {
						usleep(300000);
						dab_select(i);
						break;
					}
			}
			fclose(lf);
		}
	}
	write_status();

	while (!g_stop) {
		struct pollfd p[2] = { { T.fd, POLLIN, 0 }, { T.cmd, POLLIN, 0 } };

		if (follow && getppid() != parent)
			break;
		poll(p, 2, 150);
		if (p[0].revents & POLLIN) {
			rx_pump();
			if (T.hit) {
				T.hit = 0;
				save_last();
				fmscan_hit();
			}
		}
		if (p[1].revents & POLLIN) {
			ssize_t r = read(T.cmd, cbuf + cn, sizeof cbuf - cn - 1);

			if (r > 0)
				cn += r;
			for (;;) {
				char *nl = memchr(cbuf, '\n', cn);
				int l;

				if (!nl)
					break;
				*nl = '\0';
				command(cbuf);
				l = nl + 1 - cbuf;
				memmove(cbuf, nl + 1, cn - l);
				cn -= l;
			}
			if (cn >= (int)sizeof cbuf - 1)
				cn = 0;
		}
		if (T.cand && ms_due(T.cand_at)) {
			fprintf(stderr, "eval %d snr=%d rssi=%d\n", T.cand, T.snr, T.rssi);
			if (T.snr >= 8)
				fm_add(T.cand, "");
			T.cand = 0;
			T.dirty = 1;
			fmscan_next();
		}
		if (T.seek_at && ms_due(T.seek_at)) {
			T.seek_at = 0;
			if (T.fmscan)
				fm_seek(1);
		}
		/* timeouts */
		if (T.state == S_SEEK && T.fmscan && now_ms() - T.state_t > 3000) {
			/* the module's seek can end without a station before the band
			 * top; carry on from where it stopped */
			fprintf(stderr, "seek stalled at %d\n", T.freq);
			if (T.freq >= 10780) {
				fmscan_done();
			} else {
				fm_tune(T.freq + 10);
				T.seek_at = now_ms() + 1000;
			}
		} else if (T.state == S_SEEK && now_ms() - T.state_t > 25000) {
			/* nothing found: park where we are */
			fm_tune(T.freq);
			if (T.fmscan)
				fmscan_done();
		}
		if (T.state == S_TUNING && T.mode == M_FM && now_ms() - T.state_t > 1500)
			set_state(S_IDLE);
		if (T.list_dirty && now_ms() - T.list_t > 2500) {
			save_dab();
			T.list_dirty = 0;
			if (T.state == S_LIST)
				set_state(S_IDLE);
		}
		if (T.state == S_LIST && !T.nsvc) {
			if (!T.asked && now_ms() - T.state_t > 6000) {
				T.asked = 1;
				tx1(0x0a, -1);
			}
			if (now_ms() - T.state_t > 14000)
				set_state(S_IDLE);
		}
		if (T.state == S_SCAN && now_ms() - T.state_t > 240000) {
			tx1(0x21, -1);
			set_state(S_IDLE);
		}
		if (dab_bak_on && T.state == S_IDLE) {
			dab_bak_on = 0;
			if (!T.nsvc) {
				memcpy(T.svc, dab_bak, sizeof dab_bak);
				T.nsvc = dab_bak_n;
				save_dab();
				if (T.sel >= T.nsvc)
					T.sel = 0;
				if (T.nsvc)
					dab_select(T.sel);
			}
		}
		if (T.dirty && now_ms() - T.last_status > 60)
			write_status();
		else if (now_ms() - T.last_status > 2000)
			write_status();
	}
	if (T.mode == M_DAB)
		tx1(0x21, -1);
	unlink(CMDFIFO);
	T.state = S_IDLE;
	write_status();
	close(T.fd);
	return 0;
}
