/* x45ui  -  a front-panel UI for the X45, with no vendor code in the path.
 *
 * Software rendering onto /dev/fb0, driven by the real front panel and IR,
 * playing through MPD and controlling the hardware through libx45. No Qt, no
 * EGL, no Vivante blobs - which is what keeps it independent of the
 * galcore-locked GPU stack x20 is tied to.
 *
 * Screens (MENU cycles through them):
 *   NOW PLAYING  tags, audio format, progress; OK play/pause, scroll = prev/next
 *   RADIO        internet radio presets from etc/stations.txt; OK tunes in
 *   LIBRARY      browse the music directory; OK on a file queues the whole
 *                folder and plays from there; RETURN goes up a level
 *   X45 STATUS   live hardware state read back from the driver
 *   INPUT        input selector
 *   OUTPUTS      RCA / XLR / headphone enables
 *
 * Volume goes through MPD when it is up (its hardware mixer is the board's own
 * Master control, so MPD's percentage is the raw 0-100 index), so MPD's idea of
 * the volume never goes stale; with MPD down it writes /dev/x45 directly.
 *
 * It borrows the panel politely: SIGSTOP on x20 (which only repaints on change,
 * so it hands the framebuffer over cleanly), save the frame, draw; on exit
 * restore the frame and SIGCONT. Ctrl-C is safe.
 *
 *   x45ui            take the panel, run until Ctrl-C, hand it back
 *   x45ui --keep     do not stop/resume x20 (for when it is already stopped)
 *   x45ui --once     draw one frame, hold 5 s, restore (a smoke test)
 *   x45ui --trace    log every input event to stderr (for diagnosing the panel)
 *   x45ui --port N   MPD port (default 6600)
 *   x45ui --screen player|library|radio|status|input|outputs [--dir PATH] [--sel N]
 *                    start on a given screen (for screenshots and smoke tests)
 */
#define _GNU_SOURCE
#include "ui.h"
#include "ext/ext.h"
#include "skins/skin.h"
#include "vis/vis.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <poll.h>
#include <pthread.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <errno.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/statvfs.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <sys/wait.h>
#include <stdarg.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define SAVED_FRAME "/tmp/x45ui-saved.raw"
/* Left by deploy/x45-airplay-begin while an AirPlay session owns the DAC */
#define AIRPLAY_MARKER "/tmp/x45-airplay-active"
#define BROWSE_MAX 256
#define MPD_POLL_MS 500
#define MPD_RETRY_MS 2000
/* a volume echo from MPD within this window of a local change is ignored, so a
 * status poll racing the knob cannot snap the bar back */
#define VOL_ECHO_MS 1200

/* Linux input codes the remote can send (names resolved from the kernel header;
 * which physical button sends which has to be learned with `x45in learn`) */
#define IR_ESC     1
#define IR_POWER   116
#define IR_ENTER   28
#define IR_HOME    102
#define IR_ISERVICE 150   /* the i-service key: the network input */
#define IR_UP      103
#define IR_LEFT    105
#define IR_RIGHT   106
#define IR_DOWN    108
#define IR_REWIND  168
#define IR_FFWD    208
#define IR_NEXT    407
#define IR_SHUFFLE 410
#define IR_PREV    412
#define IR_REC     65
#define IR_INFO    59
#define IR_MUTE    60
#define IR_VOLDOWN 61
#define IR_VOLUP   62
#define IR_PLAYPAUSE 63
#define IR_STOP    64
#define IR_NOWPLAY 66
#define IR_MENU    68
#define IR_BOOKMARK 87
#define IR_FAVOURITE 88
#define IR_SETUP   141
#define IR_EJECT   161
#define IR_RIPCD   383
#define IR_RADIO   385
#define IR_INPUT   390
#define IR_BRIGHT  399   /* the green key, also labelled brightness */
#define IR_BALANCE 466   /* the "Vol balance" key: ext/balance.c */
#define IR_PLAYLIST 467
#define IR_SEARCH  67    /* the search key: ext/search.c */
#define IR_BKSP    472   /* backspace (held: goes on deleting) */
#define IR_NUM0    512   /* the number keys: 513..521 are 1..9 */
#define IR_NUM1    513
#define IR_NUM9    521


static volatile sig_atomic_t stop_now, restart_req;
static void on_sig(int s) { (void)s; stop_now = 1; }
/* SIGUSR1: finish politely and come back (x45stack starts a new one without counting a crash) */
static void on_usr1(int s) { (void)s; restart_req = 1; stop_now = 1; }

/* Counts from the start of this process, at 60 s: many timers start as 0 and mean "a long time ago", which only holds
 * while the clock is positive, so it must not begin wherever the uptime of the unit happens to be (a long is 32 bits:
 * the uptime in ms is negative from day 24.8 on). */
static long now_ms(void)
{
	static long base_s;
	static int have_base;
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	if (!have_base) {
		base_s = (long)ts.tv_sec - 60;
		have_base = 1;
	}
	return ((long)ts.tv_sec - base_s) * 1000L + ts.tv_nsec / 1000000L;
}

/* A long is 32 bits here, so now_ms() wraps (it turns negative after 24.8 days of running and starts over after 49.7).
 * A difference stays right across that; a deadline compared by size does not. Deadlines go through these. */
static inline long ms_since(long now, long t)
{
	return (long)((unsigned long)now - (unsigned long)t);
}

static inline int ms_due(long now, long t)
{
	return ms_since(now, t) >= 0;
}

static int cd_active(const struct ui *u);
static int bt_active(const struct ui *u);
static void bt_cmd(struct ui *u, char c);
static void bt_stop(struct ui *u);
static void bt_start(struct ui *u);
static void tuner_stop(struct ui *u);
static void tuner_cmd(struct ui *u, const char *fmt, ...);
static void draw_tuner_list(struct ui *u);
static enum screen ret_target(const struct ui *u);
static int menu_screen_ok(const struct ui *u, enum screen s);
static int screen_valid(const struct ui *u, enum screen s);

/* the CD is a software input: the hardware stays on the network path */
#define UI_IN_CD 9
#define UI_IN_DAB 10
#define UI_IN_BT 11
#define NUM_INPUTS 12
static const char *in_name(int in)
{
	if (in == UI_IN_CD)
		return "cd";
	if (in == UI_IN_DAB)
		return "dab";
	if (in == UI_IN_BT)
		return "bluetooth";
	if (in == X45_IN_FMRADIO)
		return "fm";
	return x45_input_name(in);
}
/* the name a person reads (lists, headings): in_name() stays the key for the config file and the icons */
static const char *in_label(int in)
{
	static const char *const l[NUM_INPUTS] = {
		"Network", "FM", "PC USB", "AES/EBU", "Coaxial", "Optical", "Line in", "Aux in", "Phono", "CD", "DAB+", "Bluetooth"
	};

	return in >= 0 && in < NUM_INPUTS ? l[in] : "?";
}
/* input list order: DAB sits next to FM; list rows are positions in this table */
static const int in_order[NUM_INPUTS] = { 0, 1, UI_IN_DAB, 2, 3, 4, 5, 6, 7, 8, UI_IN_CD, UI_IN_BT };
static int in_row(int input)
{
	int i;

	for (i = 0; i < NUM_INPUTS; i++)
		if (in_order[i] == input)
			return i;
	return 0;
}
static int tuner_input(const struct ui *u) { return u->input == X45_IN_FMRADIO || u->input == UI_IN_DAB; }
/* Reception as 0..4 bars. FM has a field strength; DAB reports a quality of 0..100 (`tu_sq`), and until the module
 * has sent the first report only whether it decodes (`tu_q`). A DAB lock that is lost caps the bars at one. */
static int tuner_bars(const struct ui *u)
{
	int sq = u->tu_sq;

	if (u->input != UI_IN_DAB)
		return !u->tu_found ? 0 : u->tu_rssi >= 45 ? 4 : u->tu_rssi >= 30 ? 3 : u->tu_rssi >= 15 ? 2 : 1;
	if (sq <= 0)
		return u->tu_q > 0 ? 3 : 1;
	if (u->tu_q <= 0 && sq > 24)
		sq = 24;
	return sq >= 80 ? 4 : sq >= 55 ? 3 : sq >= 30 ? 2 : sq >= 8 ? 1 : 0;
}
static const char *dab_state(const struct ui *u)
{
	return u->tu_q <= 0 ? "SYNC LOST" : u->tu_sq <= 0 ? "LOCKED" : u->tu_sq >= 80 ? "STRONG" : u->tu_sq >= 55 ? "GOOD" :
	       u->tu_sq >= 30 ? "FAIR" : "WEAK";
}
static int tuner_scanning(const struct ui *u)
{
	return u->tu_fmscan || !strcmp(u->tu_state, "scan") || !strcmp(u->tu_state, "list");
}

/* the colours of the screen: the skin's palette with the accent, background and text colour the owner chose
 * (skins/colours.c) */
static void theme_apply(struct ui *u)
{
	skin_palette(u);
	u->dirty = 1;
}

/* the look chosen by name ("tiles"); an unknown name leaves it as it is */
static void skin_choose(struct ui *u, const char *name)
{
	int i = skin_find(name);

	if (i < 0)
		return;
	u->skin_id = i;
	theme_apply(u);
}

/* The RETURN menu of the player screen: what is on it depends on the source. The visualizers
 * have a sub menu of their own, so the sources with other things to offer do not get clogged. */
enum { VR_SAVE, VR_SCAN, VR_BTPAIR, VR_BTFORGET, VR_SUB, VR_REC, VR_LOOK, VR_SEARCH, VR_BALANCE };
#define VR_MAX 8   /* rows the menu can have (the arrays of its callers); the longest list is 7 */

static int visuals_rows(const struct ui *u, int *ids)
{
	int n = 0;

	ids[n++] = VR_SUB;
	ids[n++] = VR_LOOK;      /* the options of the visuals (colours, the stereo field): Options > Visualizers, from here */
	ids[n++] = VR_BALANCE;   /* the balance control (ext/balance.c), also on the remote's Vol balance key */
	ids[n++] = VR_REC;
	if (u->input == X45_IN_NETWORK && u->have_song && !strncmp(u->song.file, "http", 4) && u->st.duration <= 0) {
		ids[n++] = VR_SAVE;      /* an internet radio stream: keep it in the favourites (the RADIO list) */
	} else if (u->input == X45_IN_FMRADIO) {
		ids[n++] = VR_SAVE;      /* FM is tuned by hand: a frequency worth keeping */
		ids[n++] = VR_SCAN;
	} else if (u->input == UI_IN_DAB) {
		ids[n++] = VR_SCAN;
	} else if (u->input == UI_IN_BT) {
		ids[n++] = VR_BTPAIR;
		ids[n++] = VR_BTFORGET;
	}
	if (stations_searchable(u))
		ids[n++] = VR_SEARCH;    /* find a station by typing a piece of its name */
	return n;
}

/* the balance in words, for the lists: "centre", "left 3" (the left side louder by 3 steps), "right 12" */
static void balance_text(const struct ui *u, char *out, size_t cap)
{
	if (!u->balance)
		snprintf(out, cap, "centre");
	else
		snprintf(out, cap, "%s %d", u->balance < 0 ? "left" : "right", abs(u->balance));
}

#define VISSUB_MAX 16  /* rows of the visualizers menu: one per registered visual (vis/vis.c) */

void say(struct ui *u, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(u->status, sizeof u->status, fmt, ap);
	va_end(ap);
	if (u->status[0] >= 'a' && u->status[0] <= 'z')   /* messages start with a capital, whoever wrote them */
		u->status[0] = (char)(u->status[0] - 32);
	u->dirty = 1;
}

/* Draw text that came from a tag or a path: fold UTF-8 to what the font has. */
void text_fold(struct x45_surface *s, int x, int y, const char *utf8,
	       int scale, int max_w, uint32_t col)
{
	char buf[320];

	x45_ascii_fold(buf, sizeof buf, utf8);
	x45_text_clipped(s, x, y, buf, scale, max_w, col);
}

/* A line that does not fit runs to the left instead of being cut off: it rests a moment, glides
 * to its end, rests, and starts over. `slot` tells the texts of one screen apart so each starts
 * from its own beginning when it changes. */
#define SC_SLOTS   6
#define SC_REST_MS 1800
#define SC_PX_S    55
#define SC_GAP     60

void text_scroll(struct ui *u, int slot, int x, int y, const char *utf8, int scale, int max_w, uint32_t col)
{
	static unsigned hash[SC_SLOTS];
	static long t0[SC_SLOTS];
	struct x45_surface *s = &u->g.back;
	char buf[320];
	unsigned h = 5381;
	long now = now_ms(), t, run_ms, cyc;
	int tw, travel, off;
	const char *c;

	x45_ascii_fold(buf, sizeof buf, utf8);
	tw = x45_text_w(buf, scale);
	if (tw <= max_w || slot < 0 || slot >= SC_SLOTS) {
		x45_text_clipped(s, x, y, buf, scale, max_w, col);
		return;
	}
	for (c = buf; *c; c++)
		h = h * 33 + (unsigned char)*c;
	if (hash[slot] != h || !t0[slot]) {
		hash[slot] = h;
		t0[slot] = now;
	}
	travel = tw + SC_GAP - max_w;   /* how far to move so the tail has been seen */
	if (travel < 1)
		travel = 1;
	run_ms = (long)travel * 1000 / SC_PX_S;
	cyc = SC_REST_MS + run_ms + SC_REST_MS;
	t = (now - t0[slot]) % cyc;
	if (t < SC_REST_MS)
		off = 0;
	else if (t < SC_REST_MS + run_ms)
		off = (int)((t - SC_REST_MS) * SC_PX_S / 1000);
	else
		off = travel;
	x45_clip_set(s, x, y - 2, max_w, x45_text_h(scale) + 4);
	x45_text(s, x - off, y, buf, scale, col);
	x45_clip_reset(s);
	u->scrolling = 1;
}

static const char *base_name(const char *path)
{
	const char *slash = strrchr(path, '/');
	return slash ? slash + 1 : path;
}

/* "01 Intro.flac" -> "01 Intro": drop a short alphanumeric extension so the
 * row shows the name, not the container. Leaves dotfiles and "v1.5 mix" alone. */
static void strip_ext(char *dst, size_t cap, const char *name)
{
	const char *dot = strrchr(name, '.');
	size_t n = strlen(name), i;

	if (dot && dot != name && strlen(dot) >= 2 && strlen(dot) <= 5) {
		int alnum = 1;

		for (i = 1; dot[i]; i++)
			if (!((dot[i] >= '0' && dot[i] <= '9') ||
			      (dot[i] >= 'a' && dot[i] <= 'z') ||
			      (dot[i] >= 'A' && dot[i] <= 'Z')))
				alnum = 0;
		if (alnum)
			n = (size_t)(dot - name);
	}
	if (n >= cap)
		n = cap - 1;
	memcpy(dst, name, n);
	dst[n] = '\0';
}

static void fmt_time(char *out, size_t n, double sec)
{
	int t = sec < 0 ? 0 : (int)sec;

	if (t >= 3600)
		snprintf(out, n, "%d:%02d:%02d", t / 3600, (t / 60) % 60, t % 60);
	else
		snprintf(out, n, "%d:%02d", t / 60, t % 60);
}

/* ---------------------------------------------------------------- drawing --- */

/* PC USB: the driver selects the input mode but leaves IMX6_USBPC_SEL high, so the
 * XMOS stays on the unit's own USB host and the PC never sees it. The select line
 * has to be driven explicitly: 0 = XMOS on the PC port, 1 = back on the unit. */
static int usbpc_read(void)
{
	char buf[512], *p;
	ssize_t n;
	int fd = open("/proc/x45/gpio", O_RDONLY | O_CLOEXEC);

	if (fd < 0)
		return -1;
	n = read(fd, buf, sizeof buf - 1);
	close(fd);
	if (n <= 0)
		return -1;
	buf[n] = '\0';
	p = strstr(buf, "IMX6_USBPC_SEL");
	if (!p || !(p = strchr(p, ':')))
		return -1;
	while (*++p == ' ')
		;
	return *p == '1';
}

/* The line is written and read back; a write the driver refused (it once logged "key:value
 * too long") must not leave the XMOS on the wrong side while the UI believes otherwise. */
static void usbpc_select(struct ui *u, int pc)
{
	char msg[24];
	int want = pc ? 0 : 1, tries;

	if (u->usbpc_pc == pc && usbpc_read() == want)
		return;
	for (tries = 0; tries < 4; tries++) {
		int fd = open("/proc/x45/gpio", O_WRONLY | O_CLOEXEC);
		int n = snprintf(msg, sizeof msg, "IMX6_USBPC_SEL:%d\n", want);

		if (fd >= 0) {
			ssize_t wr = write(fd, msg, (size_t)n);

			(void)wr;
			close(fd);
		}
		if (usbpc_read() == want)
			break;
		usleep(150000);
	}
	u->usbpc_pc = pc;
}

#define BL_DIR "/sys/class/backlight/backlight.24"

static int bl_max(void)
{
	FILE *f = fopen(BL_DIR "/max_brightness", "r");
	int v = 17;

	if (f) {
		if (fscanf(f, "%d", &v) != 1 || v < 1)
			v = 17;
		fclose(f);
	}
	return v;
}

static int bl_get(void)
{
	FILE *f = fopen(BL_DIR "/brightness", "r");
	int v = 0;

	if (f) {
		if (fscanf(f, "%d", &v) != 1)
			v = 0;
		fclose(f);
	}
	return v;
}

static void bl_set(struct ui *u, int v)
{
	FILE *f;
	int mx = bl_max();

	if (v < 2)
		v = 2;
	if (v > mx)
		v = mx;
	f = fopen(BL_DIR "/brightness", "w");
	if (f) {
		fprintf(f, "%d\n", v);
		fclose(f);
		u->bright = v;
	}
}

/* Screen auto-off: the backlight goes dark (1: 0 would be the brightest) after an idle time and comes back on any
 * key, a new track or a web command. */
static void bl_raw(int v)
{
	FILE *f = fopen(BL_DIR "/brightness", "w");

	if (f) {
		fprintf(f, "%d\n", v);
		fclose(f);
	}
}

static void wake(struct ui *u)
{
	u->act_t = now_ms();
	if (u->scr_dark) {
		u->scr_dark = 0;
		bl_raw(u->bright >= 2 ? u->bright : 2);
		u->dirty = 1;
	}
}

static void scr_poll(struct ui *u)
{
	if (!u->scr_off_s || u->scr_dark || !u->act_t)
		return;
	if (now_ms() - u->act_t > (long)u->scr_off_s * 1000) {
		if (u->bright < 2)
			u->bright = bl_get() >= 2 ? bl_get() : 2;
		u->scr_dark = 1;
		bl_raw(1);   /* 1 is dark on this panel; 0 would be the brightest */
	}
}

/* The DAC chain needs a moment to lock whenever a stream starts or the clock family moves, and plays garbage until
 * it has: about half a second of distortion after an input switch or a resume. The output is muted in hardware over
 * that moment, from the play command until MPD reports the stream's format and SETTLE_MS more, or for SETTLE_MS after
 * the clock was moved. The owner's mute flag and the volume are not touched. */
#define SETTLE_MS 500
#define SETTLE_MAX_MS 8000   /* a stream that never comes up does not keep the unit silent */

static void settle_hw(struct ui *u)
{
	if (u->x45fd >= 0)
		x45_mute(u->x45fd, u->muted || u->settle_until != 0);
}

/* a stream is about to start: mute until it is up */
static void dac_settle(struct ui *u)
{
	u->settle_t0 = now_ms();
	u->settle_open = 0;
	u->settle_until = u->settle_t0 + SETTLE_MAX_MS;
	settle_hw(u);
}

/* the clock family was just moved, or a stream was found starting: mute for ms more */
static void dac_settle_more(struct ui *u, long ms)
{
	long until = now_ms() + ms;

	if (!u->settle_until) {
		u->settle_t0 = now_ms();
		u->settle_open = 1;
		u->settle_until = until;
	} else if (u->settle_open && !ms_due(u->settle_until, until)) {
		u->settle_until = until;
	}
	settle_hw(u);
}

/* from every fresh MPD state: the stream is up (its format is known), so the mute runs out SETTLE_MS from now; if
 * nothing plays any more there is no point in muting */
static void dac_settle_tick(struct ui *u, const struct mpdc_status *st)
{
	long now = now_ms();
	int playing = !strcmp(st->state, "play");

	if (!u->settle_until || u->settle_open || now - u->settle_t0 < 300)   /* younger: taken before the command */
		return;
	if (playing && st->audio[0]) {
		u->settle_open = 1;
		u->settle_until = now + SETTLE_MS;
	} else if (!playing && now - u->settle_t0 > 1500) {
		u->settle_until = now;
	}
}

static void settle_poll(struct ui *u)
{
	if (!u->settle_until || !ms_due(now_ms(), u->settle_until))
		return;
	u->settle_until = 0;
	settle_hw(u);
}

/* PC USB: the computer chooses the sample rate, and the XMOS only locks when FS_4844 (see x45clock.c) is on the
 * matching clock family. Nobody does that on this input: the driver moved the line for the first rate it saw and
 * not for the later ones, and MPD's clock patch moves it for a stream that cannot play here at all (the XMOS is on
 * the computer's port), which is what distorted the computer's sound. So the panel reads the rate the driver
 * logs ("SR=[code:rate:...]" whenever the computer starts or changes a stream) and holds the line on that family
 * for as long as PC USB is the input. */
static int pc_kmsg = -1;    /* /dev/kmsg, read from the start of the ring on first use */
static int pc_rate;         /* the computer's rate since the XMOS went to its port; 0 until the driver has logged one */

static void pc_kmsg_text(const char *m)
{
	const char *p;

	if (strstr(m, "setting gpio IMX6_USBPC_SEL")) {   /* the XMOS changed sides: what was seen before no longer counts */
		pc_rate = 0;
		return;
	}
	p = strstr(m, "SR=[");
	if (p && (p = strchr(p, ':'))) {
		int r = atoi(p + 1);

		if (r >= 8000 && r <= 768000)
			pc_rate = r;
	}
}

static void pc_kmsg_drain(void)
{
	char buf[8192];
	int guard;

	if (pc_kmsg < 0) {
		pc_kmsg = open("/dev/kmsg", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		pc_rate = 0;
		if (pc_kmsg < 0)
			return;
	}
	for (guard = 0; guard < 8192; guard++) {
		ssize_t n = read(pc_kmsg, buf, sizeof buf - 1);
		char *m;

		if (n < 0) {
			if (errno == EPIPE || errno == EINTR)
				continue;   /* lines were overwritten before they were read: go on with the oldest left */
			break;          /* EAGAIN: all read */
		}
		if (n == 0)
			break;
		buf[n] = '\0';
		m = strchr(buf, ';');   /* "prio,seq,usec,flags;text" */
		if (m)
			pc_kmsg_text(m + 1);
	}
}

static void pcusb_clock_poll(struct ui *u)
{
	static long next;
	long now = now_ms();
	int fam, cur;

	if (u->input != X45_IN_PCUSB || u->usbpc_pc != 1) {
		if (pc_kmsg >= 0) {
			close(pc_kmsg);
			pc_kmsg = -1;
		}
		pc_rate = 0;
		return;
	}
	if (next && !ms_due(now, next))
		return;
	next = now + 100;
	pc_kmsg_drain();
	fam = x45_clock_family((unsigned)pc_rate);
	cur = x45_clock_get();
	if (fam < 0 || cur < 0 || cur == fam)
		return;
	dac_settle_more(u, 350);   /* the XMOS locks again: silent meanwhile */
	x45_clock_select((unsigned)pc_rate);
}

/* after the XMOS is handed back to the unit it enumerates again, and MPD finds no DAC before that */
static void dac_wait(long ms)
{
	long t0 = now_ms();

	while (access("/proc/asound/card3/stream0", F_OK) != 0 && now_ms() - t0 < ms)
		usleep(50000);
}

/* a speaker with a slash through it, 26 x 24, top left at x,y */
static void draw_mute_icon(struct ui *u, int x, int y)
{
	struct x45_surface *s = &u->g.back;
	uint32_t c = u->t.warn;
	int i;

	x += 2;
	x45_fill_rect(s, x, y + 8, 6, 8, c);                 /* magnet */
	for (i = 0; i < 10; i++)                             /* cone, opening to the right */
		x45_fill_rect(s, x + 6 + i, y + 8 - i / 2 - (i > 0), 1, 8 + (i / 2) * 2 + (i > 0) * 2, c);
	for (i = 0; i < 20; i++) {                           /* the slash, centred on the speaker, with a gap either side */
		x45_fill_rect(s, x - 3 + i, y + 2 + i, 5, 2, u->t.panel);
		x45_fill_rect(s, x - 2 + i, y + 2 + i, 3, 2, c);
	}
}

#define IN_AUTO_MS(u) ((long)(u)->in_auto_s * 1000)

/* Leaving the cursor on another input switches to it after 3 s: a strip under the header
 * shrinks to show how long is left. */
static void draw_input_countdown(struct ui *u)
{
	struct x45_surface *s = &u->g.back;
	long left = IN_AUTO_MS(u) - (now_ms() - u->in_t);
	char buf[48];
	int w;

	if (!u->in_auto_s || !u->in_t || in_order[u->sel] == u->input)
		return;
	if (left < 0)
		left = 0;
	snprintf(buf, sizeof buf, "SWITCH IN %ld", (left + 999) / 1000);
	x45_text(s, 280, 22, buf, 2, u->t.accent);
	w = (int)((long)(s->w - 24) * left / IN_AUTO_MS(u));
	x45_fill_rect(s, 0, 50, s->w, 6, u->t.panel);
	x45_fill_rect(s, 12, 50, w, 6, u->t.accent);
}

#define ICON_DIR "/home/root/x45new/icons"

/* the icon called name (ICON_DIR/name.png), or NULL when there is no such file; remembered either way */
const struct x45_icon *icon_get(struct ui *u, const char *name)
{
	char path[160];
	int i;

	for (i = 0; i < u->nicons; i++)
		if (!strcmp(u->icon_names[i], name))
			return u->icons[i].a ? &u->icons[i] : NULL;
	if (u->nicons >= 40)
		return NULL;
	i = u->nicons++;
	snprintf(u->icon_names[i], sizeof u->icon_names[i], "%s", name);
	snprintf(path, sizeof path, ICON_DIR "/%s.png", name);
	if (x45_icon_load(path, &u->icons[i]) != 0) {
		snprintf(path, sizeof path, ICON_DIR "/%s.bmp", name);
		if (x45_icon_load(path, &u->icons[i]) != 0)
			return NULL;
	}
	return &u->icons[i];
}

/* the USB trident, 34 x 28, top left at x,y: the computer is connected */
static void draw_usb_icon(struct ui *u, int x, int y)
{
	struct x45_surface *s = &u->g.back;
	uint32_t c = u->t.accent;

	x45_fill_rect(s, x + 4, y + 13, 24, 2, c);              /* the stem */
	x45_fill_rect(s, x, y + 10, 8, 8, c);                   /* the round end */
	x45_fill_rect(s, x + 26, y + 9, 2, 10, c);
	for (int i = 0; i < 6; i++)                              /* arrow head */
		x45_fill_rect(s, x + 27 + i, y + 14 - (6 - i) / 2 - 1, 1, (6 - i) + 1, c);
	x45_fill_rect(s, x + 10, y + 7, 2, 7, c);               /* branch up, square end */
	x45_fill_rect(s, x + 10, y + 7, 8, 2, c);
	x45_fill_rect(s, x + 16, y + 3, 8, 8, c);
	x45_fill_rect(s, x + 10, y + 15, 2, 8, c);              /* branch down, round end */
	x45_fill_rect(s, x + 10, y + 21, 7, 2, c);
	x45_fill_rect(s, x + 15, y + 18, 8, 8, c);
}

/* where the playing station stands in its list, for the top bar; 0 when there is none */
static int station_pos(const struct ui *u, int *pos, int *tot)
{
	int i;

	if (u->input == UI_IN_DAB && u->tu_nsvc > 0 && u->tu_sel >= 0 && u->tu_sel < u->tu_nsvc) {
		*pos = u->tu_sel + 1;
		*tot = u->tu_nsvc;
		return 1;
	}
	if (u->input == X45_IN_FMRADIO && u->tl_n > 0 && u->tu_freq >= 8750) {
		for (i = 0; i < u->tl_n; i++)
			if (u->tl_val[i] == u->tu_freq) {
				*pos = i + 1;
				*tot = u->tl_n;
				return 1;
			}
		return 0;
	}
	if (u->input == X45_IN_NETWORK && u->have_song && u->song.name[0] && u->nstations > 0) {
		for (i = 0; i < u->nstations; i++)
			if (!strcmp(u->stations[i].name, u->song.name)) {
				*pos = i + 1;
				*tot = u->nstations;
				return 1;
			}
	}
	return 0;
}

static void draw_header(struct ui *u)
{
	struct x45_surface *s = &u->g.back;
	static const char *const titles[SCR_COUNT] = {
		"NOW PLAYING", "LIBRARY", "RADIO", "CD", "STATIONS", "STATUS", "INPUT", "OPTIONS", "MENU", "VISUALIZERS", "ARE YOU SURE?", "OPTIONS", ""
	};
	char buf[64];
	int right;

	{
		static const char *const optcat_title[] = { "OPTIONS - SOUND", "OPTIONS - DISPLAY", "OPTIONS - VISUALIZERS", "OPTIONS - CONTROLS" };

		skin_bar(u, u->screen == SCR_OPTCAT && u->opt_cat >= 0 && u->opt_cat < 4 ? optcat_title[u->opt_cat] :
			 u->screen == SCR_EXT && u->ext ? u->ext->title : titles[u->screen]);
	}

	{
		const struct x45_icon *ic = u->in_icons ? icon_get(u, in_name(u->input)) : NULL;

		if (ic) {   /* the input as its icon */
			x45_icon_draw(s, ic, s->w - 24 - ic->w, 28 - ic->h / 2, u->t.accent);
			right = s->w - 24 - ic->w - 20;
		} else {
			snprintf(buf, sizeof buf, "%s", in_label(u->input));
			x45_text(s, s->w - 24 - x45_text_w(buf, 2), 24, buf, 2, u->t.accent);
			right = s->w - 24 - x45_text_w(buf, 2) - 20;   /* free space ends here */
		}
	}
	if (u->input == X45_IN_PCUSB && u->pcusb) {
		const struct x45_icon *ic = icon_get(u, "usb");

		if (ic) {
			x45_icon_draw(s, ic, right - ic->w, 28 - ic->h / 2, u->t.accent);
			right -= ic->w + 20;
		} else {
			draw_usb_icon(u, right - 34, 14);
			right -= 34 + 20;
		}
	}
	if (tuner_input(u) && u->tu_up && !tuner_scanning(u)) {
		int lvl = tuner_bars(u);
		uint32_t col = lvl >= 2 ? u->t.accent : u->t.warn;

		for (int b = 0; b < 4; b++) {
			int bh = 6 + b * 5, bx = right - 28 + b * 7;

			x45_fill_rect(s, bx, 38 - bh, 5, bh, b < lvl ? col : u->t.line);
		}
		right -= 28 + 20;
	}
	{
		int pos, tot;

		if (station_pos(u, &pos, &tot)) {
			char sp[24];

			snprintf(sp, sizeof sp, "%d/%d", pos, tot);
			right -= x45_text_w(sp, 2);
			x45_text(s, right, 24, sp, 2, u->t.dim);
			right -= 20;
		}
	}
	if (u->muted) {
		const struct x45_icon *ic = icon_get(u, "mute");

		if (ic) {
			x45_icon_draw(s, ic, right - ic->w, 28 - ic->h / 2, u->t.warn);
			right -= ic->w + 20;
		} else {
			draw_mute_icon(u, right - 26, 16);
			right -= 26 + 20;
		}
	}
	right = ui_ext_badges(u, right);   /* symbols of the screens in ext/ (WiFi) */
	if (u->p_started && !u->mpd_alive && icon_get(u, "warn")) {
		const struct x45_icon *ic = icon_get(u, "warn");

		x45_icon_draw(s, ic, right - ic->w, 28 - ic->h / 2, u->t.warn);
		right -= ic->w + 20;
	} else if (u->p_started && !u->mpd_alive) {            /* music player not answering: a warning sign */
		int ix = right - 28, iy = 14;

		x45_fill_rect(s, ix + 12 - 12, iy + 22, 28, 4, u->t.warn);
		for (int i = 0; i < 24; i++)         /* triangle, apex up */
			x45_fill_rect(s, ix + 14 - i / 2 - 1, iy + i, i + 3, 1, u->t.warn);
		x45_text(s, ix + 14 - x45_text_w("!", 2) / 2, iy + 8, "!", 2, u->t.panel);
		right -= 28 + 20;
	}
	if (u->rec_pid > 0) {
		char r[48];
		int tw, bx;

		if (u->rec_cd)
			snprintf(r, sizeof r, "RIP %d/%d", u->rec_track, u->rec_tracks);
		else
			snprintf(r, sizeof r, "REC %02ld:%02ld", u->rec_secs / 60, u->rec_secs % 60);
		/* a solid badge in the warning colour, not a line of small text: it must be seen from across the room */
		tw = x45_text_w(r, 3);
		bx = right - tw - 28;
		x45_fill_rect(s, bx, 9, tw + 28, 38, u->t.warn);
		x45_fill_rect(s, bx + 9, 24, 8, 8, u->t.bg);   /* a dot, the recorder's lamp */
		x45_text(s, bx + 22, 17, r, 3, u->t.bg);
	}
}

/* The volume graph is not a permanent bar: it fades in over the content when the volume
 * moves, stays a moment and fades out again. Only where the volume does something
 * (variable level, or the headphones); with fixed line outputs it would mean nothing. */
#define VOL_SHOW_MS    1600
#define VOL_FADE_IN_MS  120
#define VOL_FADE_OUT_MS 450

static int vol_relevant(const struct ui *u)
{
	return !(u->vol_fixed && !u->hp_present);
}

/* with fixed outputs the volume is not the owner's to change: the level stays where it is */
#define VOL_LOCKED(u) (!vol_relevant(u))

/* advances the fade; marks the screen dirty while it moves */
static void apply_volume(struct ui *u);
static void apply_balance(struct ui *u, int out);

static void vol_anim(struct ui *u)
{
	long now = now_ms(), dt = u->vol_t ? now - u->vol_t : 0;

	if (u->vol_again_t && ms_due(now, u->vol_again_t)) {
		u->vol_again_t = 0;
		apply_volume(u);
	}
	float a = u->vol_a, target;

	u->vol_t = now;
	target = vol_relevant(u) && u->last_vol_change && now - u->last_vol_change < VOL_SHOW_MS ? 255.0f : 0.0f;
	if (a < target) {
		a += 255.0f * (float)dt / VOL_FADE_IN_MS;
		if (a > target)
			a = target;
	} else if (a > target) {
		a -= 255.0f * (float)dt / VOL_FADE_OUT_MS;
		if (a < target)
			a = target;
	}
	if ((int)a != (int)u->vol_a || (a > 0 && dt > 0))
		u->dirty = 1;
	u->vol_a = a;
}

/* Track and station changes from the knob are a rubber band: each click stretches it, the
 * change happens only when it is fully stretched, and it snaps back when the knob rests. */
static void skip_track(struct ui *u, int dir);

#define RB_STEPS(u) ((u)->rb_steps)
#define RB_HOLD_MS 1500

static void scroll_poll(struct ui *u)
{
	static long last;
	long now = now_ms();

	if (u->scrolling && now - last >= 50) {
		last = now;
		u->dirty = 1;
	}
}

static void rb_poll(struct ui *u)
{
	if (!u->rb)
		return;
	if (now_ms() - u->rb_t > RB_HOLD_MS) {
		u->rb = 0;
		u->dirty = 1;
	} else if (now_ms() - u->rb_t > RB_HOLD_MS - 500) {
		u->dirty = 1;   /* the fade-out */
	}
}

/* The rubber band, in the top bar: a bar grows from the middle to the left (previous) or right
 * (next), green when slack, red as the skip comes near; it fades out when the knob rests. */
static void draw_rubber(struct ui *u)
{
	struct x45_surface *s = &u->g.back;
	const int bw = 300, bh = 14;
	int d = u->rb, steps = RB_STEPS(u), cx = s->w / 2, y = 21, len, fade = 256, r, g;
	long age = now_ms() - u->rb_t;
	float f;

	if (!d || s->w < 700)
		return;
	if (age > RB_HOLD_MS - 500)
		fade = (int)((RB_HOLD_MS - age) * 256 / 500);
	if (fade <= 0)
		return;
	f = (float)abs(d) / (float)steps;
	if (f > 1)
		f = 1;
	len = (int)(f * (float)(bw / 2));
	/* colour: green (108,198,134) -> yellow -> red (226,96,84) */
	if (f < 0.5f) {
		r = 108 + (int)((236 - 108) * f * 2);
		g = 198 + (int)((200 - 198) * f * 2);
	} else {
		r = 236 - (int)((236 - 226) * (f - 0.5f) * 2);
		g = 200 - (int)((200 - 96) * (f - 0.5f) * 2);
	}
	/* fading = blending towards the panel (22,26,36) */
	r = 22 + (r - 22) * fade / 256;
	g = 26 + (g - 26) * fade / 256;
	{
		int b = 36 + ((f < 0.5f ? 110 : 84) - 36) * fade / 256;
		uint32_t col = x45_rgb(&u->g, (uint8_t)r, (uint8_t)g, (uint8_t)b);
		uint32_t trk = x45_rgb(&u->g, (uint8_t)(22 + (46 - 22) * fade / 256), (uint8_t)(26 + (54 - 26) * fade / 256),
				       (uint8_t)(36 + (72 - 36) * fade / 256));

		x45_fill_rect(s, cx - bw / 2, y, bw, bh, trk);
		if (d > 0)
			x45_fill_rect(s, cx, y, len, bh, col);
		else
			x45_fill_rect(s, cx - len, y, len, bh, col);
		x45_fill_rect(s, cx - 1, y - 3, 2, bh + 6, x45_rgb(&u->g, (uint8_t)(22 + (232 - 22) * fade / 256),
								  (uint8_t)(26 + (236 - 26) * fade / 256),
								  (uint8_t)(36 + (244 - 36) * fade / 256)));
	}
}

static void rb_knob(struct ui *u, int ticks)
{
	long now = now_ms();

	/* only inputs with a track or station to step through: the external inputs (PC USB...)
	 * have nothing to skip and MPD would answer "not playing" */
	if (u->input == X45_IN_NETWORK ? !(u->have_song || u->st.length > 0) :
	    !(u->input == UI_IN_CD || u->input == UI_IN_BT || tuner_input(u)))
		return;
	if (u->rb && now - u->rb_t > RB_HOLD_MS)
		u->rb = 0;
	u->rb += ticks;
	u->rb_t = now;
	u->dirty = 1;
	if (abs(u->rb) >= RB_STEPS(u)) {
		int dir = u->rb > 0 ? 1 : -1;

		u->rb = 0;
		skip_track(u, dir);
	}
}

#define VOLBOX_W 640
#define VOLBOX_H 76

static void draw_volume_overlay(struct ui *u)
{
	static uint32_t keep[VOLBOX_W * VOLBOX_H];
	struct x45_surface *s = &u->g.back;
	int a = (int)u->vol_a, x, y, i, j;
	char buf[80];

	if (a <= 0 || s->w < VOLBOX_W || s->h < VOLBOX_H + 24)
		return;
	x = (s->w - VOLBOX_W) / 2;
	y = s->h - VOLBOX_H - 24;
	for (j = 0; j < VOLBOX_H; j++)
		memcpy(keep + j * VOLBOX_W, s->pixels + (size_t)(y + j) * s->stride + x, VOLBOX_W * sizeof(uint32_t));

	x45_fill_rect(s, x, y, VOLBOX_W, VOLBOX_H, u->t.panel);
	x45_draw_rect(s, x, y, VOLBOX_W, VOLBOX_H, u->t.line);
	snprintf(buf, sizeof buf, "VOL %d", u->volume);
	x45_text(s, x + 20, y + 14, buf, 3, u->muted ? u->t.dim : u->t.text);
	snprintf(buf, sizeof buf, "%.1f dB", x45_es9018_vol_db(u->volume));
	if (u->balance)   /* the side that is louder, and by how many steps */
		snprintf(buf + strlen(buf), sizeof buf - strlen(buf), " %c%d", u->balance < 0 ? 'L' : 'R', abs(u->balance));
	x45_text(s, x + 20, y + 46, buf, 2, u->t.dim);
	x45_bar(s, x + 190, y + 20, VOLBOX_W - 210, 36,
		u->muted ? 0.0 : u->volume / 100.0,
		u->muted ? u->t.dim : u->t.barfg, u->t.barbg, u->t.line);

	if (a >= 255)
		return;
	for (j = 0; j < VOLBOX_H; j++) {
		uint32_t *d = s->pixels + (size_t)(y + j) * s->stride + x;
		const uint32_t *k = keep + j * VOLBOX_W;

		for (i = 0; i < VOLBOX_W; i++) {
			uint32_t out = 0;
			int sh;

			for (sh = 0; sh < 24; sh += 8) {
				int c0 = (k[i] >> sh) & 0xff, c1 = (d[i] >> sh) & 0xff;

				out |= (uint32_t)((c0 * (255 - a) + c1 * a) / 255) << sh;
			}
			d[i] = out;
		}
	}
}

/* ---------------------------------------------------------- level meter --- */

#define METER_FILE "/tmp/x45meter.json"
#define METER_WANT "/tmp/x45meter.want"   /* x45web touches it while a browser shows a meter */
#define METER_FIFO "/tmp/x45mpd.fifo"

#define METER_DECAY   30.0f  /* dB per second, bars and peak marker alike */
#define SIGNAL_DB    -55.0f  /* above this counts as "there is a signal" */

int meter_wanted(const struct ui *u)
{
	if (u->rec_pid > 0)     /* the recorder owns the capture device */
		return 0;
	return u->input >= X45_IN_AES_EBU && u->input <= X45_IN_PHONO;
}

static void meter_stop(struct ui *u)
{
	if (u->m_pid > 0) {
		int i, done = 0;

		/* SIGKILL while x45loop streams at 192k interrupts the xHCI
		 * endpoint teardown and wedges the XMOS until reboot, so ask it
		 * to finish and only force it if it hangs */
		kill(u->m_pid, SIGTERM);
		for (i = 0; i < 100 && !done; i++) {
			if (waitpid(u->m_pid, NULL, WNOHANG) == u->m_pid)
				done = 1;
			else
				usleep(20000);
		}
		if (!done) {
			kill(u->m_pid, SIGKILL);
			waitpid(u->m_pid, NULL, 0);
		}
	}
	if (u->m_fd >= 0)
		close(u->m_fd);
	u->m_pid = 0;
	u->m_fd = -1;
	u->m_ncarry = 0;
}

static void meter_start(struct ui *u)
{
	int p[2];
	pid_t pid;

	if (pipe(p) < 0)
		return;
	pid = fork();
	if (pid < 0) {
		close(p[0]);
		close(p[1]);
		return;
	}
	if (pid == 0) {
		int nul = open("/dev/null", O_WRONLY);

		dup2(p[1], 1);
		if (nul >= 0)
			dup2(nul, 2);
		close(p[0]);
		close(p[1]);
		/* analogue sources are heard in hardware; x45loop only reads the
		 * SSI capture on hw:1,0 for the meter, like the stock firmware */
		if (u->input >= X45_IN_AES_EBU && u->input <= X45_IN_PHONO) {
			execl("/home/root/x45new/bin/x45loop", "x45loop", "-s", METER_FILE, (char *)NULL);
			_exit(127);
		}
		execlp("arecord", "arecord", "-q", "-D", "hw:1,0", "-t", "raw",
		       "-f", "S16_LE", "-c", "2", "-r", "44100",
		       "--buffer-time=60000", "--period-time=20000", (char *)NULL);
		_exit(127);
	}
	close(p[1]);
	fcntl(p[0], F_SETFL, O_NONBLOCK);
	fcntl(p[0], F_SETFD, FD_CLOEXEC);
	u->m_pid = pid;
	u->m_fd = p[0];
	u->m_last = now_ms();
	u->m_lvl[0] = u->m_lvl[1] = u->m_pk[0] = u->m_pk[1] = -90.0f;
}

/* Starts/stops the capture with the input selection and folds whatever it has
 * delivered into the bar and peak-hold state. Marks the screen dirty while the
 * meter is on show. */
static void meter_poll(struct ui *u)
{
	unsigned char buf[4096 + 4];
	long now = now_ms();
	float pk[2] = { 0, 0 }, dt;
	int ch, got_any = 0;

	if (!meter_wanted(u)) {
		if (u->m_pid)
			meter_stop(u);
		return;
	}
	if (!u->m_pid) {
		if (ms_due(now, u->m_retry))
			meter_start(u);
		return;
	}
	if (waitpid(u->m_pid, NULL, WNOHANG) == u->m_pid) { /* arecord died */
		u->m_pid = 0;
		meter_stop(u);
		u->m_retry = now + 2000;
		u->dirty = 1;
		return;
	}

	for (;;) {
		ssize_t n, i, have;

		memcpy(buf, u->m_carry, (size_t)u->m_ncarry);
		n = read(u->m_fd, buf + u->m_ncarry, 4096);
		if (n <= 0)
			break;
		have = n + u->m_ncarry;
		for (i = 0; i + 4 <= have; i += 4) {
			int l = (int16_t)(buf[i] | (buf[i + 1] << 8));
			int r = (int16_t)(buf[i + 2] | (buf[i + 3] << 8));

			if (l < 0) l = -l;
			if (r < 0) r = -r;
			if (l > pk[0]) pk[0] = (float)l;
			if (r > pk[1]) pk[1] = (float)r;
		}
		u->m_ncarry = (int)(have - i);
		memcpy(u->m_carry, buf + i, (size_t)u->m_ncarry);
		got_any = 1;
	}

	dt = (float)(now - u->m_last) / 1000.0f;
	u->m_last = now;
	for (ch = 0; ch < 2; ch++) {
		float db = got_any && pk[ch] > 0 ? 20.0f * log10f(pk[ch] / 32768.0f)
						 : -90.0f;

		if (!got_any || db < u->m_lvl[ch]) {
			u->m_lvl[ch] -= METER_DECAY * dt;
			if (u->m_lvl[ch] < -90.0f)
				u->m_lvl[ch] = -90.0f;
			if (got_any && db > u->m_lvl[ch])
				u->m_lvl[ch] = db;
		} else {
			u->m_lvl[ch] = db;
		}
		if (u->m_lvl[ch] >= u->m_pk[ch]) {
			u->m_pk[ch] = u->m_lvl[ch];
			u->m_pk_t[ch] = now;
		} else if (now - u->m_pk_t[ch] > 1200) {
			u->m_pk[ch] -= METER_DECAY * dt;
			if (u->m_pk[ch] < u->m_lvl[ch])
				u->m_pk[ch] = u->m_lvl[ch];
		}
		if (db > SIGNAL_DB)
			u->m_sig_t = now;
		if (pk[ch] >= 32700.0f)
			u->m_clip_t = now;
	}
	if (u->screen == SCR_PLAYER)
		u->dirty = 1;
}

/* Folds the analyzer file (the same one the web meters read) into sp_b/sp_l. Only while
 * the player screen is up on a source that has one. */
static void spec_read(struct ui *u)
{
	struct stat st;
	char buf[8200], *p;
	FILE *f;
	size_t n;
	long now = now_ms();
	int i;

	if (u->screen != SCR_PLAYER || !(meter_wanted(u) || u->sp_pid > 0 || getenv("X45UI_PREVIEW"))) {
		u->sp_have = 0;
		u->sp_wave = 0;
		return;
	}
	if (now - u->sp_read_t < 30)
		return;
	u->sp_read_t = now;
	if (stat(METER_FILE, &st) < 0 || time(NULL) - st.st_mtime > 2 || !(f = fopen(METER_FILE, "r"))) {
		if (u->sp_have)
			u->dirty = 1;
		u->sp_have = 0;
		return;
	}
	n = fread(buf, 1, sizeof buf - 1, f);
	fclose(f);
	buf[n] = '\0';
	p = strstr(buf, "\"seq\":");
	if (!p)
		return;
	{
		unsigned seq = (unsigned)strtoul(p + 6, NULL, 10);

		if (u->sp_have && seq == u->sp_seq)
			return;
		u->sp_seq = seq;
	}
	for (i = 0; i < 2; i++) {
		p = strstr(buf, i ? "\"r\":[" : "\"l\":[");
		if (!p)
			continue;
		u->sp_r[i] = (float)strtod(p + 5, NULL);
		if ((p = strchr(p + 5, ',')))
			u->sp_l[i] = (float)strtod(p + 1, NULL);
		if (u->sp_l[i] >= u->sp_pkh[i] || ms_since(now, u->sp_pkt[i]) > 1200) {
			u->sp_pkh[i] = u->sp_l[i];
			u->sp_pkt[i] = now;
		}
	}
	if ((p = strstr(buf, "\"c\":")))
		u->sp_c = (float)strtod(p + 4, NULL);
	if ((p = strstr(buf, "\"rate\":")))
		u->sp_frate = (unsigned)strtoul(p + 7, NULL, 10);   /* lays out the bands (vis_band_hz) */
	p = strstr(buf, "\"b\":[");
	if (!p)
		return;
	p += 5;
	{
		float nb[NSB];
		int changed;

		for (i = 0; i < NSB; i++) {
			char *e;

			nb[i] = (float)strtod(p, &e);
			if (e == p)
				return;
			p = e + 1;
		}
		changed = memcmp(nb, u->sp_b, sizeof nb) != 0;
		memcpy(u->sp_b, nb, sizeof nb);
		/* the picture of the wave: two arrays of hundredths of a percent, after the bands */
		u->sp_wave = 0;
		for (i = 0; i < 2; i++) {
			short *dst = i ? u->sp_wr : u->sp_wl;
			int k;

			p = strstr(p, i ? "\"wr2\":[" : "\"wl2\":[");
			if (!p)
				break;
			p += 7;
			for (k = 0; k < 200; k++) {
				char *e;
				long v = strtol(p, &e, 10);

				if (e == p)
					break;
				dst[k] = (short)(v < -10000 ? -10000 : v > 10000 ? 10000 : v);
				p = e + 1;
			}
			if (k < 200)
				break;
			u->sp_wave = i;   /* set after the second array has been read whole */
		}
		/* the bands of the side signal; an analyzer from before 2026-10-10 does not write them (sp_side stays 0) */
		u->sp_side = 0;
		if ((p = strstr(buf, "\"sb\":["))) {
			float sb[NSB];

			p += 6;
			for (i = 0; i < NSB; i++) {
				char *e;

				sb[i] = (float)strtod(p, &e);
				if (e == p)
					break;
				p = e + 1;
			}
			if (i == NSB) {
				memcpy(u->sp_sb, sb, sizeof sb);
				u->sp_side = 1;
			}
		}
		u->sp_have = 1;
		if (changed)
			vis_feed(u);   /* the bands move twelve times a second, the file is written twice as often */
	}
	if (u->vf_ok && u->screen == SCR_PLAYER)
		u->dirty_vis = 1;   /* only the strip moves: the rest of the frame stays */
	else
		u->dirty = 1;
}

/* the classic page: along the bottom, edge to edge, or beside the DAB picture */
static void draw_spec_strip_foot(struct ui *u, int w, int y, int foot)
{
	int x = u->slide.px ? 24 : 12;

	w = u->slide.px ? w - (u->slide.w + 16) : w + 24;   /* nothing beside it: 12 px from the panel's sides */
	draw_visuals_rect(u, x, y, w, u->g.back.h - 12 - foot - y);
}

static void draw_spec_strip(struct ui *u, int w, int y)
{
	draw_spec_strip_foot(u, w, y, 0);
}

static void draw_meter(struct ui *u)
{
	struct x45_surface *s = &u->g.back;
	const int w = s->w - 48;
	int y = 84;
	long now = now_ms();
	int signal = u->m_pid > 0 && now - u->m_sig_t < 1500 && u->m_sig_t;
	int clip = u->m_clip_t && now - u->m_clip_t < 1500;

	{
		char name[48];
		size_t n;

		snprintf(name, sizeof name, "%s", in_label(u->input));
		for (n = 0; name[n]; n++)
			if (name[n] >= 'a' && name[n] <= 'z')
				name[n] = (char)(name[n] - 32);
		x45_text(s, 24, y, name, 4, u->t.text);
	}
	if (u->m_pid <= 0)
		x45_text(s, 24, y + 56, "Level meter unavailable", 2, u->t.warn);
	else if (clip)
		x45_text(s, 24, y + 56, "CLIP", 3, u->t.warn);
	else
		x45_text(s, 24, y + 56, signal ? "SIGNAL" : "NO SIGNAL", 3,
			 signal ? u->t.accent : u->t.dim);
	x45_text(s, w - x45_text_w("dBFS", 2) + 24, y + 56, "dBFS", 2, u->t.dim);
	draw_visuals(u, 12, y + 96, w + 24, (s->h - 12) - (y + 96));
}

/* ---------------------------------------------------------------- tuner --- */

static void draw_tuner(struct ui *u)
{
	struct x45_surface *s = &u->g.back;
	const int w = s->w - 48, y = 84;
	char buf[96];
	int dab = u->input == UI_IN_DAB;

	if (!strcmp(u->tu_state, "absent")) {
		x45_text(s, 24, y + 20, dab ? "DAB+" : "FM", 4, u->t.dim);
		x45_text(s, 24, y + 90, "No radio module answers", 2, u->t.warn);
		x45_text(s, 24, y + 120, "Set tuner = off in x45ui.conf to hide this", 2, u->t.dim);
		return;
	}
	if (!u->tu_up) {
		x45_text(s, 24, y + 20, dab ? "DAB+" : "FM", 4, u->t.dim);
		x45_text(s, 24, y + 90, "Starting the radio module...", 2, u->t.dim);
		return;
	}
	if (!dab) {
		if (u->tu_freq >= 8750)
			snprintf(buf, sizeof buf, "%d.%02d MHz", u->tu_freq / 100, u->tu_freq % 100);
		else
			snprintf(buf, sizeof buf, "--.-- MHz");
		x45_text(s, 24, y, buf, 6, u->t.text);
		text_scroll(u, 0, 24, y + 80, u->tu_ps, 4, w, u->t.accent);
		if (u->tu_rt[0])
			text_scroll(u, 1, 24, y + 250, u->tu_rt, 3, w, u->t.text);
	} else {
		if (u->tu_nsvc == 0 && strcmp(u->tu_state, "scan") && strcmp(u->tu_state, "list")) {
			x45_text(s, 24, y, "NO STATIONS", 4, u->t.warn);
			x45_text(s, 24, y + 56, "Open the menu with RETURN and scan for stations.", 2, u->t.dim);
		} else {
			text_scroll(u, 0, 24, y, u->tu_svc[0] ? u->tu_svc : "-", 4, w, u->t.text);
			text_scroll(u, 1, 24, y + 52, u->tu_ens, 2, w, u->t.dim);
			text_scroll(u, 2, 24, y + 92, u->tu_text, 3, u->slide.px ? w - 290 : w, u->t.accent);
		}
		if (u->slide.px)
			x45_image_draw(s, &u->slide, s->w - 24 - u->slide.w, y + 130);
		if (u->tu_time[0])
			x45_text(s, 24, y + 230, u->tu_time, 2, u->t.dim);
	}
	if (!strcmp(u->tu_state, "scan") || !strcmp(u->tu_state, "list")) {
		x45_text(s, 24, y + 150, "SCANNING", 3, u->t.warn);
		x45_fill_rect(s, 24, y + 190, w, 14, u->t.barbg);
		x45_fill_rect(s, 24, y + 190, (int)((long)w * (u->tu_scan > 40 ? 40 : u->tu_scan) / 40), 14,
			      u->t.barfg);
		snprintf(buf, sizeof buf, "%d stations so far", u->tu_nsvc);
		x45_text(s, 24, y + 214, buf, 2, u->t.dim);
	} else if (!strcmp(u->tu_state, "seek") || u->tu_fmscan) {
		x45_text(s, 24, y + 150, u->tu_fmscan ? "SCANNING" : "SEEKING", 3, u->t.warn);
	} else if (!dab) {
		int r = u->tu_rssi > 60 ? 60 : u->tu_rssi;

		x45_text(s, 24, y + 150, "SIGNAL", 2, u->t.dim);
		x45_fill_rect(s, 150, y + 146, w - 126, 22, u->t.barbg);
		x45_fill_rect(s, 150, y + 146, (int)((long)(w - 126) * r / 60), 22,
			      u->tu_found ? u->t.barfg : u->t.warn);
		snprintf(buf, sizeof buf, "level %d   snr %d%s", u->tu_rssi, u->tu_snr,
			 u->tu_found ? "" : "   no station");
		x45_text(s, 24, y + 186, buf, 2, u->t.dim);
	} else if (u->tu_nsvc > 0) {
		x45_text(s, 24, y + 150, "SIGNAL", 2, u->t.dim);
		x45_fill_rect(s, 150, y + 146, w - 126, 22, u->t.barbg);
		x45_fill_rect(s, 150, y + 146, (int)((long)(w - 126) * u->tu_sq / 100), 22,
			      u->tu_q > 0 ? u->t.barfg : u->t.warn);
		snprintf(buf, sizeof buf, "%d%%   %s", u->tu_sq, dab_state(u));
		x45_text(s, 24, y + 186, buf, 2, u->tu_q > 0 ? u->t.dim : u->t.warn);
	}
	if (!u->sp_have)
		draw_hints(u, dab ? "prev/next|station;OK|list;RETURN|menu" : "scroll|tune;prev/next|seek;OK|presets;RETURN|menu");
}

/* The rows of the station list: the stations and, below them (one step up from the first), "Search stations". */
static int tl_rows(const struct ui *u)
{
	return u->tl_n > 0 && !tuner_scanning(u) ? u->tl_n + 1 : u->tl_n;
}

static void tuner_item(struct ui *u, void *ctx, int i, struct ui_item *it)
{
	if (i >= u->tl_n)
		snprintf(it->label, sizeof it->label, "Search stations");
	else
		snprintf(it->label, sizeof it->label, "%s", u->tl_name[i]);
}

static void draw_tuner_list(struct ui *u)
{
	struct x45_surface *s = &u->g.back;
	int scanning = tuner_scanning(u);
	char hd[80];
	struct ui_list l;

	if (scanning && u->input != UI_IN_DAB)
		snprintf(hd, sizeof hd, "SCANNING  %d.%02d MHz   %d found", u->tu_freq / 100,
			 u->tu_freq % 100, u->tu_npreset);
	else if (scanning)
		snprintf(hd, sizeof hd, "SCANNING  %d%%", (u->tu_scan > 40 ? 40 : u->tu_scan) * 100 / 40);
	else
		snprintf(hd, sizeof hd, "%s stations", u->input == UI_IN_DAB ? "DAB+" : "FM");
	if (scanning && u->input == UI_IN_DAB) {
		/* the module hands over the names only when the scan has ended: show how far it is, not the old list */
		int p = u->tu_scan > 40 ? 40 : u->tu_scan;

		x45_text(s, 24, 68, hd, 2, u->t.warn);
		x45_text(s, 24, 120, "SCANNING", 4, u->t.warn);
		x45_fill_rect(s, 24, 190, s->w - 48, 18, u->t.barbg);
		x45_fill_rect(s, 24, 190, (int)((long)(s->w - 48) * p / 40), 18, u->t.barfg);
		x45_text(s, 24, 232, "The stations are listed when the scan is over", 2, u->t.dim);
		draw_hints(u, "STOP|cancel the scan");
		return;
	}
	memset(&l, 0, sizeof l);
	l.kind = LK_TEXT;
	l.title = hd;
	l.title_kind = scanning ? 1 : 0;
	l.n = tl_rows(u);
	l.sel = u->tl_sel;
	l.current = -1;
	l.top = &u->tl_top;
	l.msg = scanning ? "Looking for stations..." : "None yet - RETURN, then scan stations";
	l.msg_kind = scanning ? 0 : 1;
	l.item = tuner_item;
	l.hint = scanning ? "STOP|cancel the scan" : "scroll|move;OK|tune;RETURN|back";
	skin_list(u, &l);
}

/* ---------------------------------------------------------------- bluetooth --- */

static void draw_bt(struct ui *u)
{
	struct x45_surface *s = &u->g.back;
	const int w = s->w - 48, y = 84;
	char buf[96];

	x45_text(s, 24, y, "BLUETOOTH", 4, u->t.text);
	if (!bt_active(u)) {
		text_fold(s, 24, y + 56, u->bt_err[0] ? u->bt_err : "off", 2, w, u->t.warn);
		x45_text(s, 24, y + 90, "Choose the input again to retry", 2, u->t.dim);
		return;
	}
	if (u->bt_confirm[0]) {
		/* a phone asks to pair: the owner compares the code and confirms here */
		int cw = x45_text_w(u->bt_confirm, 9);

		x45_text(s, 24, y + 50, "PAIRING REQUEST", 3, u->t.warn);
		if (u->bt_confirm_name[0]) {
			snprintf(buf, sizeof buf, "from %s", u->bt_confirm_name);
			text_fold(s, 24, y + 88, buf, 2, w, u->t.dim);
		}
		x45_text(s, (s->w - cw) / 2, y + 140, u->bt_confirm, 9, u->t.accent);
		x45_text(s, 24, y + 250, "Is the same code on the phone?", 2, u->t.text);
		draw_hints(u, "OK|pair;RETURN|refuse");
		return;
	}
	if (!strcmp(u->bt_state, "starting") || !u->bt_state[0]) {
		x45_text(s, 24, y + 56, "Starting...", 3, u->t.dim);
		if (u->bt_step[0])
			x45_text(s, 24, y + 100, u->bt_step, 2, u->t.dim);
		x45_text(s, 24, y + 130, "After a power-up this takes about 30 seconds", 2, u->t.dim);
		return;
	}
	if (u->bt_name[0]) {
		int streaming = !strcmp(u->bt_state, "playing");

		text_scroll(u, 0, 24, y + 52, u->bt_name, 3, w, u->t.accent);
		if (u->bt_title[0])
			text_scroll(u, 1, 24, y + 100, u->bt_title, 3, w, u->t.text);
		if (u->bt_artist[0])
			text_scroll(u, 2, 24, y + 140, u->bt_artist, 2, w, u->t.accent);
		if (u->bt_album[0])
			text_scroll(u, 3, 24, y + 168, u->bt_album, 2, w, u->t.dim);
		if (streaming) {
			snprintf(buf, sizeof buf, "STREAMING   %d.%d kHz", u->bt_rate / 1000, (u->bt_rate % 1000) / 100);
			x45_text(s, 24, y + 214, buf, 2, u->t.accent);
		} else {
			x45_text(s, 24, y + 214, "CONNECTED   waiting for audio", 2, u->t.dim);
		}
		return;
	}
	if (u->bt_bonds == 0) {
		x45_text(s, 24, y + 52, "NOT PAIRED", 3, u->t.warn);
		x45_text(s, 24, y + 100, "1  Open Bluetooth on the phone", 2, u->t.text);
		x45_text(s, 24, y + 128, "2  Choose  CocktailAudio-X45", 2, u->t.text);
		x45_text(s, 24, y + 156, "3  A code appears here: compare it, press OK", 2, u->t.text);
	} else {
		x45_text(s, 24, y + 52, "WAITING FOR A DEVICE", 3, u->t.dim);
		x45_text(s, 24, y + 100, "Paired phones connect by themselves", 2, u->t.text);
		if (u->bt_open)
			x45_text(s, 24, y + 140, "OPEN FOR PAIRING   choose  CocktailAudio-X45", 2, u->t.accent);
		else
			x45_text(s, 24, y + 140, "RETURN menu: pair a new device", 2, u->t.dim);
	}
}

static void radio_load(struct ui *u);

/* The station that plays, from the preset list (the stream's own name tag can be a different brand, or missing
 * while it connects). */
static const char *station_name(struct ui *u)
{
	const char *name = u->song.name;
	int i;

	if (!u->nstations && !u->stations_err[0])
		radio_load(u);
	for (i = 0; i < u->nstations; i++)
		if (!strcmp(u->stations[i].url, u->song.file))
			name = u->stations[i].name;
	return name[0] ? name : "Internet radio";
}

/* ----- the facts of the now-playing page, for a skin to lay out (skins/skin.h, struct ui_np) ----- */

static void np_set(char *dst, size_t cap, const char *src)
{
	snprintf(dst, cap, "%s", src ? src : "");
}

static int np_tuner(struct ui *u, struct ui_np *n)
{
	int dab = u->input == UI_IN_DAB, seeking = !strcmp(u->tu_state, "seek") || u->tu_fmscan;

	if (!u->tu_up || !strcmp(u->tu_state, "absent") || !strcmp(u->tu_state, "scan") || !strcmp(u->tu_state, "list"))
		return 0;   /* the plain page says what is going on */
	if (dab && u->tu_nsvc == 0)
		return 0;
	n->kicker = dab ? "DAB+" : "FM";
	if (dab) {
		n->lab[0] = "STATION";
		n->lab[1] = "TEXT";
		n->lab[2] = "ENSEMBLE";
		np_set(n->title, sizeof n->title, u->tu_svc[0] ? u->tu_svc : "-");
		np_set(n->sub, sizeof n->sub, u->tu_text);
		np_set(n->extra, sizeof n->extra, u->tu_ens);
		np_set(n->time, sizeof n->time, u->tu_time);
		n->slide = u->slide.px ? &u->slide : NULL;
		n->playing = u->tu_q > 0;
		np_set(n->state, sizeof n->state, dab_state(u));
		snprintf(n->flags, sizeof n->flags, "signal %d%%", u->tu_sq);
	} else {
		n->lab[0] = "FREQUENCY";
		n->lab[1] = "STATION";
		n->lab[2] = "TEXT";
		n->big_title = 1;
		if (u->tu_freq >= 8750)
			snprintf(n->title, sizeof n->title, "%d.%02d MHz", u->tu_freq / 100, u->tu_freq % 100);
		else
			np_set(n->title, sizeof n->title, "--.-- MHz");
		np_set(n->sub, sizeof n->sub, u->tu_ps);
		np_set(n->extra, sizeof n->extra, u->tu_rt);
		n->playing = u->tu_found;
		np_set(n->state, sizeof n->state, u->tu_found ? "TUNED" : "NO STATION");
		snprintf(n->flags, sizeof n->flags, "level %d   snr %d", u->tu_rssi, u->tu_snr);
	}
	if (seeking) {
		n->playing = 0;
		np_set(n->state, sizeof n->state, u->tu_fmscan ? "SCANNING" : "SEEKING");
		n->flags[0] = '\0';
	}
	n->hint = dab ? "prev/next|station;OK|list;RETURN|menu" : "scroll|tune;prev/next|seek;OK|presets;RETURN|menu";
	return 1;
}

static int np_bt(struct ui *u, struct ui_np *n)
{
	int streaming = !strcmp(u->bt_state, "playing");

	if (!bt_active(u) || u->bt_confirm[0] || !u->bt_state[0] || !strcmp(u->bt_state, "starting") || !u->bt_name[0])
		return 0;   /* pairing and starting have their plain pages */
	n->kicker = "BLUETOOTH";
	n->lab[2] = "DEVICE";
	np_set(n->title, sizeof n->title, u->bt_title[0] ? u->bt_title : u->bt_name);
	np_set(n->sub, sizeof n->sub, u->bt_artist);
	np_set(n->extra, sizeof n->extra, u->bt_album[0] ? u->bt_album : u->bt_title[0] ? u->bt_name : "");
	if (streaming)
		snprintf(n->fmt, sizeof n->fmt, "%d.%d kHz", u->bt_rate / 1000, (u->bt_rate % 1000) / 100);
	np_set(n->state, sizeof n->state, streaming ? "STREAMING" : "CONNECTED   waiting for audio");
	n->playing = streaming;
	return 1;
}

/* What the CD screens say about HDCD: empty until x45cd has found codes in the
 * stream, then "HDCD" and what the codes do (peak extend, gain). */
static void cd_hdcd_text(const struct ui *u, char *out, size_t n)
{
	int k;

	out[0] = '\0';
	if (!u->cd_hdcd)
		return;
	k = snprintf(out, n, "HDCD");
	if (u->cd_hdcd == 1 && k < (int)n)
		k += snprintf(out + k, n - k, "   no effect");
	if (u->cd_hdcd_pe && k < (int)n)
		k += snprintf(out + k, n - k, "   peak extend");
	if (u->cd_hdcd_gain && k < (int)n)
		snprintf(out + k, n - k, "   -%d.%d dB", u->cd_hdcd_gain / 2, u->cd_hdcd_gain % 2 * 5);
}

static int np_cd(struct ui *u, struct ui_np *n)
{
	n->kicker = u->cd_dvd ? "DVD" : "CD";
	snprintf(n->title, sizeof n->title, "%s %d / %d", u->cd_dvd ? "Chapter" : "Track", u->cd_track, u->cd_ntracks);
	cd_hdcd_text(u, n->sub, sizeof n->sub);
	if (n->sub[0])
		n->lab[1] = "DECODER";
	if (u->cd_dvd && u->cd_rate > 0 && u->cd_bits == 0)         /* no bit depth: Dolby Digital */
		snprintf(n->fmt, sizeof n->fmt, "%d.%d kHz  Dolby Digital", u->cd_rate / 1000, u->cd_rate % 1000 / 100);
	else if (u->cd_dvd && u->cd_rate > 0)
		snprintf(n->fmt, sizeof n->fmt, "%d.%d kHz  %d bit  PCM", u->cd_rate / 1000, u->cd_rate % 1000 / 100, u->cd_bits);
	else
		np_set(n->fmt, sizeof n->fmt, "44.1 kHz  16 bit");
	snprintf(n->time, sizeof n->time, "%d:%02d / %d:%02d", u->cd_pos / 60, u->cd_pos % 60,
		 u->cd_tlen / 60, u->cd_tlen % 60);
	n->frac = u->cd_tlen > 0 ? (double)u->cd_pos / u->cd_tlen : 0;
	np_set(n->state, sizeof n->state, u->cd_playing ? "PLAYING" : "PAUSED");
	n->playing = u->cd_playing;
	return 1;
}

static int np_mpd(struct ui *u, struct ui_np *n)
{
	const struct mpdc_status *st = &u->st;
	static char kick[24];
	char desc[64], t1[16], t2[16];
	int playing = !strcmp(st->state, "play"), stream = !strncmp(u->song.file, "http", 4) && st->duration <= 0;
	size_t i;

	if (u->air || !mpdc_is_open(&u->mpd) || (!u->have_song && !strcmp(st->state, "stop")))
		return 0;
	if (stream) {
		n->kicker = "INTERNET RADIO";
		n->lab[0] = "STATION";
		n->lab[1] = "NOW PLAYING";
		np_set(n->title, sizeof n->title, station_name(u));
		if (u->song.title[0])
			np_set(n->sub, sizeof n->sub, u->song.title);
		else if (playing && st->elapsed < 6)
			np_set(n->sub, sizeof n->sub, "Connecting...");
	} else {
		snprintf(kick, sizeof kick, "%s", in_label(u->input));
		for (i = 0; kick[i]; i++)
			if (kick[i] >= 'a' && kick[i] <= 'z')
				kick[i] = (char)(kick[i] - 32);
		n->kicker = kick;
		np_set(n->title, sizeof n->title, u->song.title[0] ? u->song.title : base_name(u->song.file));
		np_set(n->sub, sizeof n->sub, u->song.artist);
		np_set(n->extra, sizeof n->extra, u->song.album);
	}
	if (st->audio[0]) {   /* nothing is decoded while stopped: no format to show */
		mpdc_describe_audio(st->audio, desc, sizeof desc);
		if (st->bitrate > 0)
			snprintf(n->fmt, sizeof n->fmt, "%s   %d kbps", desc, st->bitrate);
		else
			np_set(n->fmt, sizeof n->fmt, desc);
	}
	fmt_time(t1, sizeof t1, st->elapsed);
	fmt_time(t2, sizeof t2, st->duration);
	if (st->duration > 0)
		snprintf(n->time, sizeof n->time, "%s / %s", t1, t2);
	else if (playing)          /* a stream has no length: show time on air */
		snprintf(n->time, sizeof n->time, "LIVE   %s", t1);
	n->frac = stream ? -1 : st->duration > 0 ? st->elapsed / st->duration : 0;
	snprintf(n->state, sizeof n->state, "%s", playing ? "PLAYING" : !strcmp(st->state, "pause") ? "PAUSED" : "STOPPED");
	if (!stream)
		snprintf(n->state + strlen(n->state), sizeof n->state - strlen(n->state), "   %d / %d", st->song + 1, st->length);
	n->playing = playing;
	snprintf(n->flags, sizeof n->flags, "%s%s%s%s", st->random ? "shuffle  " : "", st->repeat ? "repeat  " : "",
		 st->single ? "single  " : "", st->consume ? "consume" : "");
	if (st->error[0]) {
		n->msg = st->error;
		n->msg_warn = 1;
	}
	n->hint = "OK|play/pause;scroll|prev/next;MENU|screens;RETURN|menu;hold MENU|options";
	return 1;
}

/* the external inputs have no now-playing page of a skin, only their level meter: the facts of it for the large
 * stereo field's column (what draw_meter writes) */
static int np_meter(struct ui *u, struct ui_np *n)
{
	long now = now_ms();
	int signal = u->m_pid > 0 && now - u->m_sig_t < 1500 && u->m_sig_t;
	int clip = u->m_clip_t && now - u->m_clip_t < 1500;

	memset(n, 0, sizeof *n);
	n->frac = -1;
	n->kicker = "INPUT";
	np_set(n->title, sizeof n->title, in_label(u->input));
	for (char *c = n->title; *c; c++)
		if (*c >= 'a' && *c <= 'z')
			*c = (char)(*c - 32);
	np_set(n->state, sizeof n->state, u->m_pid <= 0 ? "LEVEL METER UNAVAILABLE" : clip ? "CLIP" : signal ? "SIGNAL" : "NO SIGNAL");
	n->playing = u->m_pid > 0 && signal && !clip;
	n->msg = u->status[0] ? u->status : NULL;
	return 1;
}

/* what the page shows when a skin lays it out; 0 for the states that only have the plain page (nothing playing,
 * AirPlay, a module that is missing, scanning, the level meter) */
static int fill_np(struct ui *u, struct ui_np *n)
{
	memset(n, 0, sizeof *n);
	n->frac = -1;
	n->msg = u->status[0] ? u->status : NULL;
	if (meter_wanted(u))
		return 0;
	if (u->input == X45_IN_PCUSB) {
		n->kicker = "INPUT";
		np_set(n->title, sizeof n->title, "PC USB");
		np_set(n->state, sizeof n->state, u->pcusb ? "CONNECTED" : "NO COMPUTER ON THE USB-DAC PORT");
		n->playing = u->pcusb;
		return 1;
	}
	if (u->input == UI_IN_BT)
		return np_bt(u, n);
	if (tuner_input(u))
		return np_tuner(u, n);
	if (cd_active(u))
		return np_cd(u, n);
	if (u->input == UI_IN_CD)
		return 0;
	return np_mpd(u, n);
}

static void draw_player(struct ui *u)
{
	struct x45_surface *s = &u->g.back;
	const struct mpdc_status *st = &u->st;
	const int w = s->w - 48;
	char buf[160], t1[16], t2[16], desc[64];
	int y = 84, playing, stream;
	double frac;

	if (vis_big(u)) {   /* the stereo field almost fullscreen: one page for every skin, the text a small column */
		struct ui_np np;

		if (meter_wanted(u) ? np_meter(u, &np) : fill_np(u, &np)) {
			scope_page(u, &np);
			return;
		}
	}
	if (skin_has_np(u)) {
		struct ui_np np;

		if (fill_np(u, &np)) {
			skin_np(u, &np);
			if (np.hint && !u->sp_have)
				draw_hints(u, np.hint);
			return;
		}
	}
	if (u->input == X45_IN_PCUSB) {
		x45_text(s, 24, y + 20, "PC USB", 4, u->t.text);
		if (!u->pcusb)
			x45_text(s, 24, y + 80, "No computer on the USB-DAC port", 3, u->t.warn);
		if (u->status[0])
			text_fold(s, 24, y + 294, u->status, 2, w, u->t.dim);
		draw_spec_strip(u, w, u->pcusb ? y + 88 : y + 130);   /* nothing else on this screen: the visuals take it */
		return;
	}
	if (meter_wanted(u)) {
		draw_meter(u);
		return;
	}
	if (u->input == UI_IN_BT) {
		draw_bt(u);
		if (u->status[0])
			text_fold(s, 24, y + 294, u->status, 2, w, u->t.dim);
		draw_spec_strip(u, w, 400);
		return;
	}
	if (tuner_input(u)) {
		draw_tuner(u);
		if (u->status[0])
			text_fold(s, 24, y + 294, u->status, 2, w, u->t.dim);
		draw_spec_strip(u, w, 400);
		return;
	}

	if (u->input == UI_IN_CD && !cd_active(u)) {
		if (u->cd_loading) {
			x45_text(s, 24, y + 20, "CD", 4, u->t.text);
			x45_text(s, 24, y + 90, "LOADING...", 4, u->t.accent);
			x45_text(s, 24, y + 150, "Reading the disc", 2, u->t.dim);
			goto status;
		}
		x45_text(s, 24, y + 20, u->cd_dvd ? "DVD   STOPPED" : "CD   STOPPED", 4, u->t.dim);
		if (u->cd_ntracks > 0 && u->cd_note[0])
			snprintf(buf, sizeof buf, "Video DVD: %s sound   not supported", u->cd_note);
		else if (u->cd_ntracks > 0)
			snprintf(buf, sizeof buf, "%d %s   OK starts %s 1   MENU: %s list", u->cd_ntracks,
				 u->cd_dvd ? "chapters" : "tracks", u->cd_dvd ? "chapter" : "track", u->cd_dvd ? "chapter" : "track");
		else
			snprintf(buf, sizeof buf, "%s", u->cd_err[0] ? u->cd_err : "No disc read yet   PLAY to read");
		x45_text(s, 24, y + 90, buf, 2, u->t.dim);
		goto status;
	}
	if (cd_active(u)) {
		snprintf(buf, sizeof buf, "%s   %s %d / %d", u->cd_dvd ? "DVD" : "CD", u->cd_dvd ? "chapter" : "track",
			 u->cd_track, u->cd_ntracks);
		x45_text(s, 24, y + 20, buf, 4, u->t.text);
		snprintf(buf, sizeof buf, "%d:%02d / %d:%02d", u->cd_pos / 60, u->cd_pos % 60,
			 u->cd_tlen / 60, u->cd_tlen % 60);
		x45_text(s, 24, y + 90, buf, 3, u->t.accent);
		x45_fill_rect(s, 24, y + 150, w, 14, u->t.barbg);
		if (u->cd_tlen > 0)
			x45_fill_rect(s, 24, y + 150,
				      (int)((long)w * u->cd_pos / u->cd_tlen), 14, u->t.barfg);
		cd_hdcd_text(u, buf, sizeof buf);
		if (buf[0])
			x45_text(s, 24, y + 190, buf, 3, u->t.accent);
		x45_text(s, 24, y + 260, u->cd_playing ? "PLAYING" : "PAUSED", 3,
			 u->cd_playing ? u->t.accent : u->t.dim);
		goto status;
	}

	if (u->air) {
		/* shairport-sync is built without metadata support, so there is no
		 * title to show; what is known is the format AirPlay always uses */
		x45_text(s, 24, y + 20, "AirPlay", 4, u->t.text);
		x45_text(s, 24, y + 80, "Streaming from a network device", 3, u->t.accent);
		x45_text(s, 24, y + 128, "44.1 kHz  16 bit  stereo", 2, u->t.text);
		x45_text(s, 24, y + 260, "STREAMING", 3, u->t.accent);
		goto hint;
	}

	if (!mpdc_is_open(&u->mpd)) {
		x45_text(s, 24, y + 20, "MPD OFFLINE", 4, u->t.warn);
		snprintf(buf, sizeof buf, "retrying 127.0.0.1:%d", u->mpd_port);
		x45_text(s, 24, y + 76, buf, 2, u->t.dim);
		goto hint;
	}

	if (!u->have_song && !strcmp(st->state, "stop")) {
		x45_text(s, 24, y + 20, "STOPPED", 4, u->t.dim);
		snprintf(buf, sizeof buf, "%d track%s in the queue", st->length,
			 st->length == 1 ? "" : "s");
		x45_text(s, 24, y + 76, buf, 2, u->t.dim);
		x45_text(s, 24, y + 110, "Open LIBRARY to choose something", 2, u->t.dim);
		goto status;
	}

	stream = !strncmp(u->song.file, "http", 4) && st->duration <= 0;
	if (stream) {
		/* internet radio: station name, then what is playing on it, nothing else.
		 * The name comes from the preset list (the stream's own name tag can be
		 * a different brand, or missing while it connects) */
		text_scroll(u, 0, 24, y, station_name(u), 4, w, u->t.text);
		if (u->song.title[0])
			text_scroll(u, 1, 24, y + 50, u->song.title, 3, w, u->t.accent);
		else if (!strcmp(st->state, "play") && st->elapsed < 6)
			x45_text(s, 24, y + 50, "Connecting...", 3, u->t.dim);
	} else {
		/* title falls back to the file name when there is no tag */
		text_scroll(u, 0, 24, y, u->song.title[0] ? u->song.title : base_name(u->song.file), 4, w, u->t.text);
		if (u->song.artist[0])
			text_scroll(u, 1, 24, y + 50, u->song.artist, 3, w, u->t.accent);
		if (u->song.album[0])
			text_scroll(u, 2, 24, y + 90, u->song.album, 2, w, u->t.dim);
	}

	if (st->audio[0]) {   /* nothing is decoded while stopped: no format to show */
		mpdc_describe_audio(st->audio, desc, sizeof desc);
		if (st->bitrate > 0)
			snprintf(buf, sizeof buf, "%s   %d kbps", desc, st->bitrate);
		else
			snprintf(buf, sizeof buf, "%s", desc);
		x45_text_clipped(s, 24, y + 128, buf, 2, w, u->t.text);
	}

	/* progress (a live stream has none) */
	frac = st->duration > 0 ? st->elapsed / st->duration : 0;
	if (!stream)
		x45_bar(s, 24, y + 176, w, 22, frac, u->t.barfg, u->t.barbg, u->t.line);
	fmt_time(t1, sizeof t1, st->elapsed);
	fmt_time(t2, sizeof t2, st->duration);
	playing = !strcmp(st->state, "play");
	if (st->duration > 0)
		snprintf(buf, sizeof buf, "%s / %s", t1, t2);
	else if (playing)          /* a stream has no length: show time on air */
		snprintf(buf, sizeof buf, "LIVE   %s", t1);
	else
		buf[0] = '\0';
	x45_text(s, 24, y + 212, buf, 3, u->t.text);

	snprintf(buf, sizeof buf, "%s", playing ? "PLAYING" :
		 !strcmp(st->state, "pause") ? "PAUSED" : "STOPPED");
	if (!stream)
		snprintf(buf + strlen(buf), sizeof buf - strlen(buf), "   %d / %d",
			 st->song + 1, st->length);
	x45_text(s, 24, y + 260, buf, 3, playing ? u->t.accent : u->t.dim);
	snprintf(buf, sizeof buf, "%s%s%s%s", st->random ? "shuffle  " : "",
		 st->repeat ? "repeat  " : "", st->single ? "single  " : "",
		 st->consume ? "consume" : "");
	x45_text(s, w - x45_text_w(buf, 2) + 24, y + 264, buf, 2, u->t.dim);

status:
	if (st->error[0])
		text_fold(s, 24, y + 294, st->error, 2, w, u->t.warn);
	else if (u->status[0])
		text_fold(s, 24, y + 294, u->status, 2, w, u->t.dim);
hint:
	if (!u->sp_have)
		draw_hints(u, "OK|play/pause;scroll|prev/next;MENU|screens;RETURN|menu;hold MENU|options");
	/* the keys along the foot are drawn while the analyzer has nothing (a stopped disc, nothing queued) and the
	 * large stereo field is drawn even then: it must stay above them, not over them */
	draw_spec_strip_foot(u, w, 400, u->sp_have ? 0 : 50);
}

/* where the library browser is, as a person reads it: "/radio/favourites", "/network/Music", "/HDD4/Recordings" */
static void browse_label(const struct ui *u, char *path, size_t cap)
{
	if (!strcmp(u->dir, "@net"))
		snprintf(path, cap, "/network");
	else if (!strcmp(u->dir, "@fav"))
		snprintf(path, cap, "/radio/favourites");
	else if (!strcmp(u->dir, "@rt"))
		snprintf(path, cap, "/radio/router stations");
	else if (!strncmp(u->dir, "@r|", 3))
		snprintf(path, cap, "/radio%s%s", !strcmp(u->dir, "@r|root") ? "" : "/", !strcmp(u->dir, "@r|root") ? "" : u->net_title);
	else if (u->dir[0] == '@')
		snprintf(path, cap, "/network/%s", u->net_title);
	else
		snprintf(path, cap, "/%s", u->dir);
}

static void browse_item(struct ui *u, void *ctx, int i, struct ui_item *it)
{
	const struct mpdc_entry *e = &u->rows[i];

	it->action = !strcmp(e->path, "@refresh") || !strcmp(e->path, "@syncrt") || !strcmp(e->path, "@search");   /* commands, not folders */
	it->dir = e->is_dir;
	if (e->is_dir || e->path[0] == '@')
		snprintf(it->label, sizeof it->label, "%s", e->name);
	else
		strip_ext(it->label, sizeof it->label, e->name);
}

static void draw_browse(struct ui *u)
{
	char path[MPDC_PATH_MAX + 96];
	struct ui_list l;

	browse_label(u, path, sizeof path);
	memset(&l, 0, sizeof l);
	l.kind = LK_TEXT;
	l.title = path;
	l.note = u->truncated ? "First 256 shown" : NULL;
	l.n = u->net_busy[0] ? 0 : u->nrows;   /* while loading, the old rows are not drawn under the message */
	l.sel = u->bsel;
	l.current = -1;
	l.top = &u->top;
	l.marks = 1;
	l.scroll_sel = 1;
	if (u->net_busy[0]) {
		l.msg = u->net_busy;
		l.msg_kind = 2;
	} else if (u->dir_err[0]) {
		l.msg = u->dir_err;
		l.msg_kind = 1;
	} else {
		l.msg = "(empty)";
	}
	l.item = browse_item;
	/* deleting works on the disks and on the favourites, not on the network or the radio directory */
	l.hint = u->dir[0] != '@' || !strcmp(u->dir, "@fav") ? "scroll|move;OK|open / play;RETURN|up;hold STOP|delete" :
		 "scroll|move;OK|open / play;RETURN|up";
	skin_list(u, &l);
}

/* Re-read the presets every time the screen is opened, so an edit made over ssh
 * shows up without restarting anything. */
static void radio_load(struct ui *u)
{
	int n;

	u->stations_err[0] = '\0';
	n = x45_stations_load(u->stations_path, u->stations, 64, &u->stations_skipped);
	if (n < 0) {
		u->nstations = 0;
		snprintf(u->stations_err, sizeof u->stations_err, "%s: %s",
			 u->stations_path, strerror(-n));
		return;
	}
	u->nstations = n;
	if (u->rsel >= n)
		u->rsel = 0;
	if (u->rtop > u->rsel)
		u->rtop = u->rsel;
}

static void radio_item(struct ui *u, void *ctx, int i, struct ui_item *it)
{
	snprintf(it->label, sizeof it->label, "%s", u->stations[i].name);
}

static void draw_radio(struct ui *u)
{
	struct ui_list l;
	char note[48];
	int i;

	memset(&l, 0, sizeof l);
	l.kind = LK_TEXT;
	l.title = "Internet radio";
	if (u->stations_skipped > 0) {
		snprintf(note, sizeof note, "%d row%s ignored", u->stations_skipped, u->stations_skipped == 1 ? "" : "s");
		l.note = note;
	}
	l.n = u->nstations;
	l.sel = u->rsel;
	l.current = -1;
	if (u->input == X45_IN_NETWORK && u->have_song)   /* the station on the air */
		for (i = 0; i < u->nstations; i++)
			if (!strcmp(u->stations[i].url, u->song.file))
				l.current = i;
	l.top = &u->rtop;
	if (u->stations_err[0]) {
		l.msg = u->stations_err;
		l.msg_kind = 1;
	} else {
		l.msg = "No stations in etc/stations.txt";
	}
	l.item = radio_item;
	l.hint = "scroll|move;OK|tune in;RETURN|back";
	skin_list(u, &l);
}

static void cd_item(struct ui *u, void *ctx, int t, struct ui_item *it)
{
	if (u->cd_len[t] < 0)
		snprintf(it->lead, sizeof it->lead, "%2d  data", t + 1);
	else
		snprintf(it->lead, sizeof it->lead, "%2d  %d:%02d", t + 1, u->cd_len[t] / 60, u->cd_len[t] % 60);
	snprintf(it->label, sizeof it->label, "%s", u->cd_title[t]);
	if (cd_active(u) && u->cd_track == t + 1 && u->cd_sel != t)   /* under the cursor it needs no sign of its own */
		snprintf(it->mark, sizeof it->mark, "%s", u->cd_playing ? ">" : "||");
}

static void draw_cd(struct ui *u)
{
	struct ui_list l;
	char buf[96];

	memset(&l, 0, sizeof l);
	l.kind = LK_TEXT;
	if (u->cd_err[0]) {
		l.title = u->cd_err;
		l.title_kind = 1;
	} else if (cd_active(u)) {
		snprintf(buf, sizeof buf, "%s %s %d   %d:%02d / %d:%02d",
			 u->cd_playing ? "playing" : "paused", u->cd_dvd ? "chapter" : "track", u->cd_track,
			 u->cd_pos / 60, u->cd_pos % 60, u->cd_tlen / 60, u->cd_tlen % 60);
		l.title = buf;
		l.title_kind = u->cd_playing ? 2 : 0;
	} else if (u->cd_note[0]) {
		snprintf(buf, sizeof buf, "Video DVD: %s - not supported", u->cd_note);
		l.title = buf;
		l.title_kind = 1;
	} else {
		l.title = u->cd_album[0] ? u->cd_album : u->cd_dvd ? "Video DVD (sound)" : "Audio cd";
	}
	l.n = u->cd_ntracks;
	l.sel = u->cd_sel;
	l.current = -1;
	l.top = &u->cd_top;
	l.lead_w = 168;
	l.item = cd_item;
	l.hint = "scroll|move;OK|play/pause;STOP|stop / eject;RETURN|back";
	skin_list(u, &l);
}

/* debug: what the unit is busy with. Sampled once a second, and only while the status page is on the screen (the walk
 * through /proc costs a few milliseconds): the busy share of all cores, the memory, and the three busiest processes. */
#define SYS_PROCS 256

static void sys_poll(struct ui *u)
{
	static unsigned long long prev_busy, prev_total;
	static struct { int pid; unsigned long ticks; } prev[SYS_PROCS];
	static int nprev;
	static long prev_t;
	struct { int pid; unsigned long ticks; char name[16]; } cur[SYS_PROCS];
	unsigned long long v[8] = { 0 }, total = 0, busy;
	long now = now_ms(), hz = sysconf(_SC_CLK_TCK);
	int ncur = 0, i, j, k;
	char line[256];
	struct dirent *e;
	FILE *f;
	DIR *d;

	if (u->screen != SCR_MAIN) {
		u->sys_t = 0;
		prev_t = 0;
		prev_total = 0;   /* the first sample on the way back is not an average over the time away */
		return;
	}
	if (u->sys_t && now - u->sys_t < 1000)
		return;
	if ((f = fopen("/proc/stat", "r"))) {
		if (fgets(line, sizeof line, f) &&
		    sscanf(line, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]) >= 4) {
			for (i = 0; i < 8; i++)
				total += v[i];
			busy = total - v[3] - v[4];
			u->sys_cpu = prev_total && total > prev_total ? (int)((busy - prev_busy) * 1000 / (total - prev_total)) : -1;
			prev_busy = busy;
			prev_total = total;
		}
		fclose(f);
	}
	u->sys_cores = (int)sysconf(_SC_NPROCESSORS_ONLN);
	if ((f = fopen("/proc/meminfo", "r"))) {
		long cache = 0, kb;

		while (fgets(line, sizeof line, f)) {
			if (sscanf(line, "MemTotal: %ld", &kb) == 1)
				u->sys_mem_total_kb = kb;
			else if (sscanf(line, "MemFree: %ld", &kb) == 1)
				u->sys_mem_free_kb = kb;
			else if (sscanf(line, "Buffers: %ld", &kb) == 1 || sscanf(line, "Cached: %ld", &kb) == 1)
				cache += kb;
		}
		u->sys_mem_cache_kb = cache;
		fclose(f);
	}
	if ((d = opendir("/proc"))) {
		while ((e = readdir(d)) && ncur < SYS_PROCS) {
			char path[32], buf[512], *lp, *rp;
			unsigned long ut, st;
			int pid = atoi(e->d_name), n;

			if (pid <= 0)
				continue;
			snprintf(path, sizeof path, "/proc/%d/stat", pid);
			if (!(f = fopen(path, "r")))
				continue;
			n = (int)fread(buf, 1, sizeof buf - 1, f);
			fclose(f);
			buf[n > 0 ? n : 0] = '\0';
			lp = strchr(buf, '(');
			rp = strrchr(buf, ')');
			if (!lp || !rp || rp < lp || sscanf(rp + 1, " %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &ut, &st) != 2)
				continue;
			cur[ncur].pid = pid;
			cur[ncur].ticks = ut + st;
			snprintf(cur[ncur].name, sizeof cur[ncur].name, "%.*s", (int)(rp - lp - 1) < 15 ? (int)(rp - lp - 1) : 15, lp + 1);
			ncur++;
		}
		closedir(d);
	}
	memset(u->sys_top, 0, sizeof u->sys_top);
	if (prev_t && ms_since(now, prev_t) > 0 && hz > 0) {
		for (i = 0; i < ncur; i++) {
			int pct = -1;

			for (j = 0; j < nprev; j++)
				if (prev[j].pid == cur[i].pid && cur[i].ticks >= prev[j].ticks) {
					pct = (int)((cur[i].ticks - prev[j].ticks) * 100000L / (ms_since(now, prev_t) * hz));
					break;
				}
			for (k = 0; pct > 0 && k < 3; k++)
				if (pct > u->sys_top[k].pct) {
					memmove(&u->sys_top[k + 1], &u->sys_top[k], (size_t)(2 - k) * sizeof u->sys_top[0]);
					u->sys_top[k].pct = pct;
					snprintf(u->sys_top[k].name, sizeof u->sys_top[k].name, "%s", cur[i].name);
					break;
				}
		}
	}
	for (i = 0; i < ncur; i++) {
		prev[i].pid = cur[i].pid;
		prev[i].ticks = cur[i].ticks;
	}
	nprev = ncur;
	prev_t = now;
	u->sys_t = now;
	u->dirty = 1;
}

/* one line of the status screen: the name dim on the left, its value in a column of its own */
static void kv_row(struct ui *u, int y, int scale, const char *k, const char *v, uint32_t vc)
{
	struct x45_surface *s = &u->g.back;

	x45_text(s, 24, y, k, scale, u->t.dim);
	x45_text_clipped(s, 280, y, v, scale, s->w - 280 - 24, vc);
}

static void draw_main(struct ui *u)
{
	struct x45_surface *s = &u->g.back;
	int y = 80;
	char buf[96];

	snprintf(buf, sizeof buf, "%s", in_label(u->input));
	kv_row(u, y, 3, "Input", buf, u->t.text); y += 38;
	kv_row(u, y, 3, "Headphones", u->hp_present ? "plugged in" : "not plugged in", u->hp_present ? u->t.accent : u->t.text); y += 38;
	kv_row(u, y, 3, "PC USB", u->pcusb ? "computer connected" : "no computer", u->pcusb ? u->t.accent : u->t.text); y += 38;
	/* the CS8422 is shared: on the analogue inputs it locks to the ADC's own
	 * 192 kHz feed, which says nothing about an S/PDIF cable */
	if (u->input != X45_IN_AES_EBU && u->input != X45_IN_COAXIAL &&
	    u->input != X45_IN_OPTICAL)
		snprintf(buf, sizeof buf, "-");
	else if (u->cs_rate > 0)
		snprintf(buf, sizeof buf, "%d Hz", (int)u->cs_rate);
	else
		snprintf(buf, sizeof buf, "no lock");
	kv_row(u, y, 3, "S/PDIF in", buf, u->t.text); y += 50;
	x45_hline(s, 24, y - 12, s->w - 48, u->t.line);

	snprintf(buf, sizeof buf, "RCA %s    XLR %s    headphones %s", u->out_lineout ? "on" : "off",
		 u->out_xlr ? "on" : "off", u->out_hp ? "on" : "off");
	kv_row(u, y, 2, "Outputs", buf, u->t.text); y += 30;
	{
		FILE *f;
		double up = 0, l1 = 0;
		int tmp = 0;
		char ip[32] = "-";
		struct statvfs sv;

		if ((f = fopen("/proc/uptime", "r"))) {
			if (fscanf(f, "%lf", &up) != 1)
				up = 0;
			fclose(f);
		}
		if ((f = fopen("/proc/loadavg", "r"))) {
			if (fscanf(f, "%lf", &l1) != 1)
				l1 = 0;
			fclose(f);
		}
		if ((f = fopen("/sys/class/thermal/thermal_zone0/temp", "r"))) {
			if (fscanf(f, "%d", &tmp) != 1)
				tmp = 0;
			fclose(f);
		}
		{
			int sk = socket(AF_INET, SOCK_DGRAM, 0);
			struct ifreq ifr;

			if (sk >= 0) {
				memset(&ifr, 0, sizeof ifr);
				snprintf(ifr.ifr_name, sizeof ifr.ifr_name, "eth0");
				if (ioctl(sk, SIOCGIFADDR, &ifr) == 0)
					inet_ntop(AF_INET, &((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr, ip, sizeof ip);
				close(sk);
			}
		}
		snprintf(buf, sizeof buf, "up %dh%02dm    load %.2f    %d C", (int)(up / 3600),
			 (int)(up / 60) % 60, l1, tmp > 1000 ? tmp / 1000 : tmp);
		kv_row(u, y, 2, "System", buf, u->t.text); y += 30;
		kv_row(u, y, 2, "Network", ip, u->t.text); y += 30;
		if (statvfs("/mnt/HDD4", &sv) == 0)
			snprintf(buf, sizeof buf, "%.1f GB free", (double)sv.f_bavail * sv.f_frsize / 1e9);
		else
			snprintf(buf, sizeof buf, "-");
		kv_row(u, y, 2, "Disk HDD4", buf, u->t.text); y += 30;
		snprintf(buf, sizeof buf, "MPD %s    AirPlay %s    recording %s", u->st.state, u->air ? "on" : "idle",
			 u->rec_pid > 0 ? "on" : "off");
		kv_row(u, y, 2, "Services", buf, u->t.text); y += 36;
	}
	x45_hline(s, 24, y - 12, s->w - 48, u->t.line);
	/* debug: the load of the unit (sys_poll) */
	if (u->sys_cpu < 0 || !u->sys_t)
		snprintf(buf, sizeof buf, "measuring ...");
	else
		snprintf(buf, sizeof buf, "%d.%d %%  of %d cores   (a core is 100 %%)", u->sys_cpu / 10, u->sys_cpu % 10, u->sys_cores);
	kv_row(u, y, 2, "CPU", buf, u->t.text); y += 30;
	snprintf(buf, sizeof buf, "app %ld MB    cache %ld MB    free %ld MB    of %ld MB",
		 (u->sys_mem_total_kb - u->sys_mem_free_kb - u->sys_mem_cache_kb) / 1024, u->sys_mem_cache_kb / 1024,
		 u->sys_mem_free_kb / 1024, u->sys_mem_total_kb / 1024);
	kv_row(u, y, 2, "Memory", buf, u->t.text); y += 30;
	buf[0] = '\0';
	for (int k = 0; k < 3; k++)
		if (u->sys_top[k].pct > 0)
			snprintf(buf + strlen(buf), sizeof buf - strlen(buf), "%s%s %d %%", k ? "    " : "", u->sys_top[k].name, u->sys_top[k].pct);
	kv_row(u, y, 2, "Busiest", buf[0] ? buf : "-", u->t.text); y += 30;

	if (u->status[0])
		x45_text_clipped(s, 24, y, u->status, 2, s->w - 48, u->t.warn);

	draw_hints(u, "MENU|next;knob|volume;push|mute;RETURN|back");
}

/* The keys that work on a screen, along its foot: "KEY|what;KEY|what". How they look is up to the skin. */
void draw_hints(struct ui *u, const char *spec)
{
	skin_hints(u, spec);
}

/* Every list on the panel is described, not drawn: the skin (skins/) lays it out. This one is the plain kind, in arrays:
 * each row may have a value (the settings) and an icon (the inputs). `current` is the row that is in force, or -1;
 * the highlighted row is u->sel. */
struct rows_ctx {
	const char *const *rows, *const *vals, *const *icons;
};

static void rows_item(struct ui *u, void *ctx, int i, struct ui_item *it)
{
	const struct rows_ctx *c = ctx;

	snprintf(it->label, sizeof it->label, "%s", c->rows[i]);
	if (c->vals && c->vals[i])
		snprintf(it->value, sizeof it->value, "%s", c->vals[i]);
	if (c->icons)
		it->icon = c->icons[i];
}

void draw_rows(struct ui *u, const char *const *rows, const char *const *vals, const char *const *icons,
	       int n, int current, const char *hint)
{
	struct rows_ctx c = { rows, vals, icons };
	struct ui_list l;

	memset(&l, 0, sizeof l);
	l.kind = icons ? LK_ICONS : LK_SETTINGS;
	l.n = n;
	l.sel = u->sel;
	l.current = current;
	l.item = rows_item;
	l.ctx = &c;
	l.hint = hint;
	skin_list(u, &l);
}


/* ---- the options list ---- */
enum { OP_VOL, OP_TRIM, OP_RCA, OP_XLR, OP_BL, OP_SCOPE, OP_BANDS, OP_SPECCOL, OP_THEME, OP_COUNT,
       OP_RUBBER, OP_SCROFF, OP_FONT, OP_INICON, OP_RESUME, OP_SKIN, OP_GROUND, OP_INK, OP_GLOOK, OP_GCOL, OP_GBARS, OP_GBIG,
       OP_RIPFMT, OP_SPECSTYLE, OP_BAL, OP_N };

/* the options are grouped; OK on a group opens its list */
#define OPT_MAX 8
static const int optcat_rows[4][OPT_MAX] = {
	{ OP_VOL, OP_BAL, OP_TRIM, OP_RCA, OP_XLR, -1, -1, -1 },
	{ OP_BL, OP_SKIN, OP_THEME, OP_GROUND, OP_INK, OP_FONT, OP_SCROFF, OP_INICON },
	{ OP_SCOPE, OP_SPECSTYLE, OP_BANDS, OP_SPECCOL, OP_GLOOK, OP_GCOL, OP_GBARS, OP_GBIG },
	{ OP_COUNT, OP_RUBBER, OP_RESUME, OP_RIPFMT, -1, -1, -1, -1 },
};

/* the look option a row belongs to (VIS_LOOK_*), or 0 for one that is always there */
static unsigned opt_look(int id)
{
	switch (id) {
	case OP_SPECSTYLE: return VIS_LOOK_STYLE;
	case OP_BANDS:  return VIS_LOOK_BANDS;
	case OP_SPECCOL: return VIS_LOOK_COLOURS;
	case OP_GLOOK: case OP_GCOL: case OP_GBARS: case OP_GBIG: return VIS_LOOK_FIELD;
	default:        return 0;
	}
}

/* The rows of a group as they are shown. The options of a visual that is not switched on (the stereo field's, the
 * number of bands, the bar colours) are left out: they would change nothing on the screen. Returns how many. */
static int optcat_ids(const struct ui *u, int cat, int *ids)
{
	unsigned looks = vis_looks(u);
	int n = 0, i;

	for (i = 0; i < OPT_MAX && optcat_rows[cat][i] >= 0; i++) {
		unsigned need = opt_look(optcat_rows[cat][i]);

		if (!need || (looks & need))
			ids[n++] = optcat_rows[cat][i];
	}
	return n;
}

static int optcat_len(const struct ui *u, int cat)
{
	int ids[OPT_MAX];

	return optcat_ids(u, cat, ids);
}

/* the file format of a CD rip: x45rec's -f argument, the config value and the name shown */
static const char *const rip_fmt_arg[] = { "flac", "mp3", "mp3v0" };
static const char *const rip_fmt_name[] = { "FLAC", "MP3 320 kbps", "MP3 V0 (VBR)" };
#define NRIP_FMT ((int)(sizeof rip_fmt_arg / sizeof *rip_fmt_arg))

static const int band_opts[] = { 16, 24, 32, 48, 64, 96, 128 };
static const int scroff_opts[] = { 0, 15, 30, 60, 120, 300, 600, 1800, 3600 };
#define NBAND_OPTS ((int)(sizeof band_opts / sizeof *band_opts))
#define NSCROFF_OPTS ((int)(sizeof scroff_opts / sizeof *scroff_opts))

/* one row of the options: its name and, apart, its value (drawn right aligned) */
static void opt_row(const struct ui *u, int id, char *l, size_t nl, char *v, size_t nv)
{
	v[0] = '\0';
	switch (id) {
	case OP_VOL:
		snprintf(l, nl, "Volume");
		snprintf(v, nv, "%s", u->vol_fixed ? "fixed" : "variable");
		break;
	case OP_BAL:
		snprintf(l, nl, "Balance");
		balance_text(u, v, nv);
		break;
	case OP_TRIM:
		if (u->input >= X45_IN_LINEIN && u->input <= X45_IN_PHONO) {
			snprintf(l, nl, "Trim, %s", in_label(u->input));
			snprintf(v, nv, "%+d dB", u->trim[u->input] - 12);
		} else {
			snprintf(l, nl, "Input trim");
			snprintf(v, nv, "analog inputs only");
		}
		break;
	case OP_RCA:
		snprintf(l, nl, "RCA out");
		snprintf(v, nv, "%s", u->out_lineout ? "on" : "off");
		break;
	case OP_XLR:
		snprintf(l, nl, "XLR out");
		snprintf(v, nv, "%s", u->out_xlr ? "on" : "off");
		break;
	case OP_BL:
		snprintf(l, nl, "Brightness");
		snprintf(v, nv, "%s%d / %d%s", u->bl_edit ? "< " : "", bl_get(), bl_max(), u->bl_edit ? " >" : "");
		break;
	case OP_SCOPE:
		snprintf(l, nl, "Visualizers");
		snprintf(v, nv, "%s", u->vis_all ? "same on all inputs" : "per input");
		break;
	case OP_SPECSTYLE:
		snprintf(l, nl, "Spectrum style");
		snprintf(v, nv, "%s", vis_spec_style_name(u->spec_style));
		break;
	case OP_BANDS:
		snprintf(l, nl, "Spectrum bands");
		snprintf(v, nv, "%d bands", u->bands);
		break;
	case OP_SPECCOL:
		snprintf(l, nl, "Bar colours");
		snprintf(v, nv, "%s", vis_bar_colour_name(u->spec_col));
		break;
	case OP_GLOOK:
		snprintf(l, nl, "Stereo field");
		snprintf(v, nv, "%s", vis_gonio_look_name(u->gon_look));
		break;
	case OP_GCOL:
		snprintf(l, nl, "Stereo field colour");
		snprintf(v, nv, "%s", vis_gonio_colour_name(u->gon_col));
		break;
	case OP_GBARS:
		snprintf(l, nl, "Stereo field bars");
		snprintf(v, nv, "%s", u->gon_nobars ? "left out" : "correlation, balance");
		break;
	case OP_GBIG:
		snprintf(l, nl, "Stereo field size");
		snprintf(v, nv, "%s", !u->gon_big ? "beside the text" : u->gon_nobars ? "almost fullscreen" : "fullscreen: bars off first");
		break;
	case OP_SKIN:
		snprintf(l, nl, "Look");
		snprintf(v, nv, "%s", skin_at(u->skin_id)->label);
		break;
	case OP_THEME:
		snprintf(l, nl, "Accent colour");
		snprintf(v, nv, "%s", colour_name(COL_ACCENT, u->theme_id));
		break;
	case OP_GROUND:
		snprintf(l, nl, "Background");
		snprintf(v, nv, "%s", u->ground_id ? colour_name(COL_GROUND, u->ground_id) : "skin's own");
		break;
	case OP_INK:
		snprintf(l, nl, "Text colour");
		snprintf(v, nv, "%s", u->ink_id ? colour_name(COL_INK, u->ink_id) : "skin's own");
		break;
	case OP_COUNT:
		snprintf(l, nl, "Input switch");
		if (u->in_auto_s)
			snprintf(v, nv, "after %d s", u->in_auto_s);
		else
			snprintf(v, nv, "only with OK");
		break;
	case OP_RUBBER:
		snprintf(l, nl, "Knob skip");
		if (u->rb_steps <= 1)
			snprintf(v, nv, "every click");
		else
			snprintf(v, nv, "%d clicks", u->rb_steps);
		break;
	case OP_SCROFF:
		snprintf(l, nl, "Screen off");
		if (!u->scr_off_s)
			snprintf(v, nv, "never");
		else if (u->scr_off_s < 60)
			snprintf(v, nv, "after %d s", u->scr_off_s);
		else if (u->scr_off_s < 3600)
			snprintf(v, nv, "after %d min", u->scr_off_s / 60);
		else
			snprintf(v, nv, "after 1 h");
		break;
	case OP_INICON:
		snprintf(l, nl, "Input in the top bar");
		snprintf(v, nv, "%s", u->in_icons ? "icon" : "name");
		break;
	case OP_FONT:
		snprintf(l, nl, "Text");
		snprintf(v, nv, "%s", x45_font_smooth >= 2 && x45_ttf_ready() ? "font" : x45_font_smooth ? "smooth" : "pixels");
		break;
	case OP_RIPFMT:
		snprintf(l, nl, "CD rip format");
		snprintf(v, nv, "%s", rip_fmt_name[u->rip_fmt]);
		break;
	default:
		snprintf(l, nl, "Resume playback");
		snprintf(v, nv, "%s", u->resume ? "on" : "off");
		break;
	}
}


static void save_int_key(const char *key, int v)
{
	char b[16];

	snprintf(b, sizeof b, "%d", v);
	save_config_key(key, b);
}

static void save_vis_key(const char *key, const struct vis_sel *s)
{
	char b[16];

	snprintf(b, sizeof b, "%lu", vis_pack(s));
	save_config_key(key, b);
}

static void set_trim(struct ui *u, int input, int idx);
static void set_volume_mode(struct ui *u, int fixed);
static void apply_volume(struct ui *u);
static void set_out(struct ui *u, int which, int on);

/* OK on an option: a switch flips, a list steps to its next value (and wraps) */
static void opt_activate(struct ui *u, int id)
{
	int i;

	switch (id) {
	case OP_VOL:
		set_volume_mode(u, !u->vol_fixed);
		break;
	case OP_BAL:   /* its own screen: left and right step it, 0 centres it */
		ui_ext_open_name(u, "balance");
		return;
	case OP_TRIM:
		if (u->input >= X45_IN_LINEIN && u->input <= X45_IN_PHONO) {
			set_trim(u, u->input, (u->trim[u->input] + 1) % 22);
		} else {
			say(u, "trim is per analog input: choose line in, aux in or phono");
			return;
		}
		break;
	case OP_RCA:
		set_out(u, 0, !u->out_lineout);
		break;
	case OP_XLR:
		set_out(u, 1, !u->out_xlr);
		break;
	case OP_BL:
		if (!u->bl_edit) {
			/* push: the knob now sets the brightness; push again to keep it */
			u->bl_edit = 1;
			u->status[0] = '\0';
			u->dirty = 1;
			return;
		}
		u->bl_edit = 0;
		save_int_key("brightness", u->bright);
		break;
	case OP_SCOPE:
		u->vis_all = !u->vis_all;
		if (u->vis_all)
			u->vis_shared = u->vis[u->input];   /* start from what this input shows */
		save_config_key("vis_scope", u->vis_all ? "all" : "input");
		save_vis_key("vis_all_set", &u->vis_shared);
		break;
	case OP_SPECSTYLE:
		u->spec_style = (u->spec_style + 1) % vis_spec_styles();
		save_int_key("spec_style", u->spec_style);
		break;
	case OP_BANDS:
		for (i = 0; i < NBAND_OPTS && band_opts[i] != u->bands; i++)
			;
		u->bands = band_opts[(i + 1) % NBAND_OPTS];
		save_int_key("bands", u->bands);
		break;
	case OP_SPECCOL:
		u->spec_col = (u->spec_col + 1) % vis_bar_colours();
		save_int_key("spec_color", u->spec_col);
		break;
	case OP_GLOOK:
		u->gon_look = (u->gon_look + 1) % vis_gonio_looks();
		save_int_key("gon_look", u->gon_look);
		break;
	case OP_GCOL:
		u->gon_col = (u->gon_col + 1) % vis_gonio_colours();
		save_int_key("gon_color", u->gon_col);
		break;
	case OP_GBARS:
		u->gon_nobars = !u->gon_nobars;
		save_int_key("gon_nobars", u->gon_nobars);
		if (!u->gon_nobars && u->gon_big) {   /* the bars and the big scope exclude each other: the later choice wins */
			u->gon_big = 0;
			save_int_key("gon_big", 0);
		}
		break;
	case OP_GBIG:
		u->gon_big = !u->gon_big;
		save_int_key("gon_big", u->gon_big);
		if (u->gon_big && !u->gon_nobars) {
			u->gon_nobars = 1;
			save_int_key("gon_nobars", 1);
		}
		break;
	case OP_SKIN:
		u->skin_id = (u->skin_id + 1) % skin_count();
		theme_apply(u);
		save_config_key("skin", skin_at(u->skin_id)->name);
		break;
	case OP_THEME:
		u->theme_id = (u->theme_id + 1) % colour_count(COL_ACCENT);
		theme_apply(u);
		save_int_key("theme", u->theme_id);
		break;
	case OP_GROUND:
		u->ground_id = (u->ground_id + 1) % colour_count(COL_GROUND);
		theme_apply(u);
		save_int_key("ground", u->ground_id);
		break;
	case OP_INK:
		u->ink_id = (u->ink_id + 1) % colour_count(COL_INK);
		theme_apply(u);
		save_int_key("ink", u->ink_id);
		break;
	case OP_COUNT:
		u->in_auto_s = u->in_auto_s == 0 ? 3 : u->in_auto_s >= 10 ? 0 : u->in_auto_s + 1;
		save_int_key("in_auto", u->in_auto_s);
		break;
	case OP_RUBBER:
		u->rb_steps = u->rb_steps >= 10 ? 1 : u->rb_steps + 1;
		save_int_key("rb_steps", u->rb_steps);
		break;
	case OP_SCROFF:
		for (i = 0; i < NSCROFF_OPTS && scroff_opts[i] != u->scr_off_s; i++)
			;
		u->scr_off_s = scroff_opts[(i + 1) % NSCROFF_OPTS];
		save_int_key("screen_off", u->scr_off_s);
		break;
	case OP_INICON:
		u->in_icons = !u->in_icons;
		save_int_key("input_icons", u->in_icons);
		break;
	case OP_FONT:
		x45_font_smooth = (x45_font_smooth + 1) % (x45_ttf_ready() ? 3 : 2);
		save_int_key("font_smooth", x45_font_smooth);
		break;
	case OP_RIPFMT:
		u->rip_fmt = (u->rip_fmt + 1) % NRIP_FMT;
		save_config_key("rip_format", rip_fmt_arg[u->rip_fmt]);
		break;
	default:
		u->resume = !u->resume;
		save_config_key("resume", u->resume ? "on" : "off");
		break;
	}
	snprintf(u->status, sizeof u->status, "Saved");
}

/* X45UI_PROF=1: every 5 s, how much of the time went into drawing and into sending the frame */
static long prof_draw_us, prof_pres_us, prof_frames;

static long mono_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000000L + ts.tv_nsec / 1000;
}

/* The card that says a recording or a rip has begun: large, in the warning colour, over whatever the screen shows,
 * for REC_NOTE_MS. (The line under the content, and the badge in the bar, are small.) */
#define REC_NOTE_MS 5000

static int rec_note_on(const struct ui *u)
{
	return u->rec_note_t && ms_since(now_ms(), u->rec_note_t) < REC_NOTE_MS;
}

static void rec_note_poll(struct ui *u)
{
	if (u->rec_note_t && !rec_note_on(u)) {
		u->rec_note_t = 0;
		u->dirty = 1;
	}
}

static void draw_rec_note(struct ui *u)
{
	struct x45_surface *s = &u->g.back;
	const char *t1 = u->rec_cd ? "RIPPING THE DISC" : "RECORDING";
	const char *t2 = "to the hard disk    hold STOP to end it";
	int w1 = x45_text_w(t1, 5), w2 = x45_text_w(t2, 2), cw = (w1 > w2 ? w1 : w2) + 80, ch = 140;
	int x, y;

	if (!rec_note_on(u) || u->rec_pid <= 0)
		return;
	if (cw > s->w - 24)
		cw = s->w - 24;
	x = (s->w - cw) / 2;
	y = (s->h - ch) / 2;
	x45_fill_rect(s, x, y, cw, ch, u->t.warn);
	x45_text_clipped(s, x + (cw - w1) / 2, y + 28, t1, 5, cw - 16, u->t.bg);
	x45_text_clipped(s, x + (cw - w2) / 2, y + 28 + 35 + 22, t2, 2, cw - 16, u->t.bg);
}

/* A frame where only the visuals strip changed: put back what was behind it, draw the strip, and send
 * just those rows. The rest of the screen costs memory bandwidth for nothing. */
static void render_strip(struct ui *u)
{
	struct x45_surface *s = &u->g.back;
	long t_start = mono_us(), t_drawn;

	u->dirty_vis = 0;
	if (!u->vf_ok || u->screen != SCR_PLAYER || u->vol_a > 0 || rec_note_on(u) || u->vf_y + u->vf_h > s->h) {
		u->dirty = 1;
		return;
	}
	memcpy(s->pixels + (size_t)u->vf_y * s->stride, u->vf_dials ? u->vf_bg2 : u->vf_bg,
	       (size_t)u->vf_h * (size_t)s->stride * 4);
	u->dial_pass = u->vf_dials ? 2 : 0;
	draw_visuals(u, u->vf_x, u->vf_y, u->vf_w, u->vf_h);
	u->dial_pass = 0;
	t_drawn = mono_us();
	x45_gfx_present_rows(&u->g, u->vf_y, u->vf_y + u->vf_h);
	if (access("/tmp/x45ui.prof", F_OK) == 0) {
		static long last, n, dr, pr;
		long t_end = mono_us();

		dr += t_drawn - t_start;
		pr += t_end - t_drawn;
		n++;
		if (t_end - last > 5000000) {
			if (last)
				fprintf(stderr, "x45ui prof: strip %ld frames/5s, draw %ld us, present %ld us per frame\n", n, dr / n, pr / n);
			last = t_end;
			dr = pr = n = 0;
		}
	}
}

static void render(struct ui *u)
{
	long t_start = mono_us(), t_drawn;

	u->scrolling = 0;
	u->vf_ok = 0;
	u->dirty_vis = 0;

	x45_clear(&u->g.back, u->t.bg);
	draw_header(u);

	switch (u->screen) {
	case SCR_PLAYER:
		draw_player(u);
		break;
	case SCR_BROWSE:
		draw_browse(u);
		break;
	case SCR_RADIO:
		draw_radio(u);
		break;
	case SCR_CD:
		draw_cd(u);
		break;
	case SCR_TUNER:
		draw_tuner_list(u);
		break;
	case SCR_MAIN:
		draw_main(u);
		break;
	case SCR_INPUTS: {
		const char *rows[NUM_INPUTS], *icons[NUM_INPUTS];
		int i;

		for (i = 0; i < NUM_INPUTS; i++) {
			rows[i] = in_label(in_order[i]);
			icons[i] = in_name(in_order[i]);
		}
		draw_rows(u, rows, NULL, icons, NUM_INPUTS, in_row(u->input),
			  "scroll|move;INPUT|next;OK|choose;RETURN|back");
		draw_input_countdown(u);
		break;
	}
	case SCR_VISUALS: {
		char lab[VR_MAX][48], bal_val[VR_MAX][24];
		const char *rows[VR_MAX], *vals[VR_MAX];
		int ids[VR_MAX], n = visuals_rows(u, ids), i;

		for (i = 0; i < n; i++) {
			vals[i] = "";
			switch (ids[i]) {
			case VR_SUB:  snprintf(lab[i], sizeof lab[i], "Visualizers"); vals[i] = ">"; break;
			case VR_LOOK: snprintf(lab[i], sizeof lab[i], "Visualizer look"); vals[i] = ">"; break;
			case VR_BALANCE:
				snprintf(lab[i], sizeof lab[i], "Balance");
				balance_text(u, bal_val[i], sizeof bal_val[i]);
				vals[i] = bal_val[i];
				break;
			case VR_REC:  snprintf(lab[i], sizeof lab[i], "%s", u->rec_pid > 0 ? "Stop recording" :
					       u->input == UI_IN_CD ? "Rip the disc" : "Record"); break;
			case VR_SAVE: snprintf(lab[i], sizeof lab[i], "%s", u->input == X45_IN_NETWORK ? "Add to favourites" : "Save station"); break;
			case VR_BTPAIR: snprintf(lab[i], sizeof lab[i], "Pair a new device"); break;
			case VR_BTFORGET: snprintf(lab[i], sizeof lab[i], "Forget all devices"); break;
			case VR_SEARCH: snprintf(lab[i], sizeof lab[i], "Search stations"); break;
			default:      snprintf(lab[i], sizeof lab[i], "%s", tuner_scanning(u) ? "Cancel the scan" : "Scan for stations"); break;
			}
			rows[i] = lab[i];
		}
		draw_rows(u, rows, vals, NULL, n, -1, "OK|select;RETURN|back");
		break;
	}
	case SCR_CONFIRM: {
		struct x45_surface *sf = &u->g.back;
		char l1[100], l2[100];
		static const char *const yn[2] = { "Yes", "No" };
		int i;

		/* the question, folded to two lines of the big font */
		snprintf(l1, sizeof l1, "%.38s", u->cf_what);
		l2[0] = '\0';
		if (strlen(u->cf_what) > 38) {
			int cut = 38;

			while (cut > 20 && u->cf_what[cut] != ' ')
				cut--;
			snprintf(l1, sizeof l1, "%.*s", cut, u->cf_what);
			snprintf(l2, sizeof l2, "%.38s", u->cf_what + cut + (u->cf_what[cut] == ' '));
		}
		x45_text(sf, 32, 120, l1, 3, u->t.text);
		if (l2[0])
			x45_text(sf, 32, 160, l2, 3, u->t.text);
		for (i = 0; i < 2; i++) {
			int ry = 236 + i * 56;

			if (i == u->sel) {
				x45_fill_rect(sf, 12, ry - 8, sf->w - 24, 46, u->t.accent);
				x45_text(sf, 32, ry, yn[i], 3, u->t.bg);
			} else {
				x45_text(sf, 32, ry, yn[i], 3, u->t.text);
			}
		}
		draw_hints(u, "scroll|move;OK|choose;RETURN|cancel");
		break;
	}
	case SCR_VISSUB: {
		char buf[96];

		vis_menu_draw(u);   /* a card for each visual with its picture (vis/preview.c) */
		draw_hints(u, "arrows|choose;OK|switch;RETURN|back");
		snprintf(buf, sizeof buf, "Two at most, a third replaces the oldest. For %s",
			 u->vis_all ? "all inputs" : in_label(u->input));
		x45_text_clipped(&u->g.back, 24, u->g.back.h - 84, buf, 2, u->g.back.w - 48, u->t.dim);
		break;
	}
	case SCR_OPTIONS: {
		/* the four groups of this file, then whatever the screens in ext/ list under Options */
		const char *nm[4 + 8] = { "Sound", "Display", "Visualizers", "Controls" };
		const char *vals[4 + 8] = { ">", ">", ">", ">" };
		int i, n = 4 + ui_ext_menu_count();

		if (n > 4 + 8)
			n = 4 + 8;
		for (i = 4; i < n; i++) {
			nm[i] = ui_ext_menu_at(i - 4)->menu;
			vals[i] = ">";
		}
		draw_rows(u, nm, vals, NULL, n, -1, "OK|open;RETURN|back");
		break;
	}
	case SCR_EXT:
		ui_ext_draw(u);
		break;
	case SCR_OPTCAT: {
		char lab[OPT_MAX][56], val[OPT_MAX][40];
		const char *rows[OPT_MAX], *vals[OPT_MAX];
		int i, n, cat = u->opt_cat, cur = -1, ids[OPT_MAX];

		if (cat < 0 || cat > 3)
			cat = 0;
		n = optcat_ids(u, cat, ids);
		if (u->sel >= n && n > 0)
			u->sel = n - 1;   /* a visual was switched off (the web, the input changed) while the list was open */
		for (i = 0; i < n; i++) {
			opt_row(u, ids[i], lab[i], sizeof lab[i], val[i], sizeof val[i]);
			rows[i] = lab[i];
			vals[i] = val[i];
			if (u->bl_edit && ids[i] == OP_BL)
				cur = i;
		}
		draw_rows(u, rows, vals, NULL, n, cur, u->bl_edit ?
			  "scroll|adjust;OK|keep" : "OK|change;RETURN|back");
		break;
	}
	default:
		break;
	}
	if (u->screen == SCR_PLAYER)
		draw_rubber(u);
	draw_rec_note(u);
	draw_volume_overlay(u);
	t_drawn = mono_us();
	x45_gfx_present(&u->g);
	if (access("/tmp/x45ui.prof", F_OK) == 0) {   /* touch it to get the figures */
		static long last;
		long t_end = mono_us();

		prof_draw_us += t_drawn - t_start;
		prof_pres_us += t_end - t_drawn;
		prof_frames++;
		if (t_end - last > 5000000) {
			if (last)
				fprintf(stderr, "x45ui prof: %ld frames/5s, draw %ld us, present %ld us per frame\n", prof_frames,
					prof_draw_us / prof_frames, prof_pres_us / prof_frames);
			last = t_end;
			prof_draw_us = prof_pres_us = prof_frames = 0;
		}
	}
	u->dirty = 0;
}

/* -------------------------------------------------------------------- MPD --- */

static void mpd_connect(struct ui *u)
{
	long now = now_ms();
	int r;

	if (mpdc_is_open(&u->mpd) || now - u->last_try < MPD_RETRY_MS)
		return;
	u->last_try = now;
	r = mpdc_open(&u->mpd, MPDC_DEFAULT_HOST, u->mpd_port);
	if (r < 0) {
		u->mpd.fd = -1;
		return;
	}
	u->seen_pos = -2;
	u->seen_state[0] = '\0';
	u->dirty = 1;
}

static void *mpd_thread(void *arg)
{
	struct ui *u = arg;
	struct mpdc c;
	long last_song_t = 0;
	int last_pos = -3;
	char last_state[8] = "";
	struct mpdc_song sg;

	memset(&c, 0, sizeof c);
	c.fd = -1;
	memset(&sg, 0, sizeof sg);
	while (!u->p_stop) {
		struct mpdc_status st;
		long now = now_ms();

		if (!mpdc_is_open(&c)) {
			if (mpdc_open(&c, MPDC_DEFAULT_HOST, u->mpd_port) < 0) {
				c.fd = -1;
				pthread_mutex_lock(&u->p_mx);
				u->p_alive = 0;
				u->p_seq++;
				pthread_mutex_unlock(&u->p_mx);
				usleep(1000000);
				continue;
			}
		}
		if (mpdc_status(&c, &st) < 0) {
			/* no answer in time: MPD is busy or gone. Start over on a new connection (a late reply would
			 * otherwise be taken for the answer to the next question); the panel keeps its last state. */
			int busy = c.last_error[0] == '\0';

			mpdc_close(&c);
			c.fd = -1;
			pthread_mutex_lock(&u->p_mx);
			u->p_alive = busy;   /* a timeout is busy, a refused connection is handled above */
			u->p_seq++;
			pthread_mutex_unlock(&u->p_mx);
			usleep(200000);
			continue;
		}
		if (u->p_refresh) {
			u->p_refresh = 0;
			last_song_t = 0;   /* read the song again whatever the position says */
		}
		if (st.song != last_pos || strcmp(st.state, last_state) || now - last_song_t >= 2000) {
			struct mpdc_song n;

			if (!strcmp(st.state, "stop") && st.song < 0) {
				memset(&sg, 0, sizeof sg);
			} else if (mpdc_currentsong(&c, &n) == 0) {
				sg = n;
			}
			last_pos = st.song;
			snprintf(last_state, sizeof last_state, "%s", st.state);
			last_song_t = now;
		}
		pthread_mutex_lock(&u->p_mx);
		u->p_st = st;
		u->p_song = sg;
		u->p_valid = 1;
		u->p_alive = 1;
		u->p_seq++;
		pthread_mutex_unlock(&u->p_mx);
		usleep((u->settle_until ? 100 : MPD_POLL_MS) * 1000);   /* quicker while the output waits for the stream */
	}
	mpdc_close(&c);
	return NULL;
}

static void input_select(struct ui *u, int n);
static void go_network(struct ui *u);
static void lib_refresh(struct ui *u);
static int mpd_do(struct ui *u, const char *cmd);

static void mpd_poll(struct ui *u)
{
	struct mpdc_status st;
	struct mpdc_song sg;
	long now = now_ms();
	int changed_track, was_playing = !strcmp(u->seen_state, "play"), alive, valid;
	unsigned seq;

	mpd_connect(u);   /* the connection the panel's own commands use */
	if (!u->p_started) {
		pthread_mutex_init(&u->p_mx, NULL);
		u->p_started = 1;
		if (pthread_create(&u->p_thr, NULL, mpd_thread, u) != 0)
			u->p_started = 0;
		return;
	}
	pthread_mutex_lock(&u->p_mx);
	alive = u->p_alive;
	valid = u->p_valid;
	seq = u->p_seq;
	st = u->p_st;
	sg = u->p_song;
	pthread_mutex_unlock(&u->p_mx);
	if (alive != u->mpd_alive) {
		u->mpd_alive = alive;
		u->dirty = 1;
	}
	if (!alive && !valid) {
		if (u->st.state[0]) {          /* never seen: nothing to show */
			memset(&u->st, 0, sizeof u->st);
			u->have_song = 0;
			u->dirty = 1;
		}
		return;
	}
	if (seq == u->p_seen || !valid || !ms_due(now, u->song_hold))
		return;
	if (sg.file[0] && u->play_url[0] && !strcmp(sg.file, u->play_url) && !sg.name[0])
		snprintf(sg.name, sizeof sg.name, "%s", u->play_name);   /* the name we know until the stream tells its own */
	u->p_seen = seq;
	if (!alive)      /* stalled: the last state stays on the screen */
		return;
	u->last_poll = now;
	if (st.updating) {
		u->lib_updating = 1;
	} else if (u->lib_updating) {
		u->lib_updating = 0;
		lib_refresh(u);   /* the update is over: the list shows what it found */
	}

	/* The XMOS follows the FS_4844 clock family, and something in the driver
	 * flips it back after MPD set it (44.1k stream on the 48k family plays
	 * distorted until the next pause/play). Re-assert it while playing; a
	 * no-op when it is already right. */
	dac_settle_tick(u, &st);
	if (u->seen_state[0] && !strcmp(u->seen_state, "stop") && !strcmp(st.state, "play") && !u->settle_until)
		dac_settle_more(u, 350);   /* somebody else started it (a controller, the web page): the start is half over, mute the rest */
	if (u->input == X45_IN_NETWORK && !strcmp(st.state, "play") &&
	    ((st.audio[0] >= '0' && st.audio[0] <= '9') || !strncmp(st.audio, "dsd", 3))) {
		/* every DSD rate (native 88.2k/176.4k frames, or the 352.8k PCM DSD256 is played as) is a 44.1k multiple */
		unsigned rate = st.audio[0] == 'd' ? 44100 : (unsigned)atoi(st.audio);
		int want = x45_clock_family(rate);

		if (want >= 0 && x45_clock_get() != want)
			dac_settle_more(u, 400);   /* moving the clock makes the chain lock again: silent meanwhile */
		x45_clock_select(rate);
	}
	changed_track = st.song != u->seen_pos || strcmp(st.state, u->seen_state) ||
		       (u->have_song && sg.file[0] && strcmp(sg.file, u->song.file));   /* the queue was replaced under us: other file, same position, still playing */
	if (changed_track && !was_playing && u->input == X45_IN_PCUSB && now - u->in_sw_t > 2000 &&
	    (!strcmp(st.state, "play") || (!strcmp(st.state, "pause") && !strncmp(st.error, "Failed to open", 14)))) {
		/* a controller started MPD while the XMOS is on the computer's port: MPD finds no DAC (it pauses with an
		 * error) and has already moved the clock line. The network input takes over and plays. */
		go_network(u);
		mpdc_run(&u->mpd, "clearerror", NULL, NULL, NULL, 0);
		mpd_do(u, "play");
		u->p_refresh = 1;
		return;
	}
	if (changed_track && !was_playing && !strcmp(st.state, "play") && u->input != X45_IN_NETWORK &&
	    u->input != UI_IN_CD && !u->air && now - u->in_sw_t > 6000 && now - u->start_t > 10000) {
		/* somebody started MPD (a UPnP/DLNA controller, the web page, a phone app) while another input
		 * is selected: the music is on the network path, so the panel follows it */
		input_select(u, X45_IN_NETWORK);
	}
	if (changed_track) {
		wake(u);
		if (!strcmp(st.state, "stop") && st.song < 0) {
			u->have_song = 0;
			memset(&u->song, 0, sizeof u->song);
		} else {
			u->song = sg;
			u->have_song = u->song.file[0] != '\0';
		}
		u->seen_pos = st.song;
		u->last_song = now;
		snprintf(u->seen_state, sizeof u->seen_state, "%s", st.state);
	} else if (u->have_song && !strncmp(u->song.file, "http", 4) &&
		   now - u->last_song >= 2000) {
		/* a stream's title changes without the queue changing, and the stream
		 * name only arrives once it is connected */
		u->last_song = now;
		if (sg.file[0] && memcmp(&sg, &u->song, sizeof sg)) {
			u->song = sg;
			u->dirty = 1;
		}
	}

	/* adopt MPD's volume unless the user just moved it ourselves */
	if (st.volume >= 0 && st.volume != u->volume && !VOL_LOCKED(u) &&
	    now - u->last_vol_change > VOL_ECHO_MS) {
		u->volume = st.volume;
		apply_balance(u, st.volume);   /* the master write of whoever moved it set both sides alike: the balance is put back */
	}

	/* only repaint for what changed on screen (elapsed ticks matter only on
	 * the player screen) */
	if (changed_track || st.volume != u->st.volume ||
	    (u->screen == SCR_PLAYER &&
	     ((int)st.elapsed != (int)u->st.elapsed ||
	      st.random != u->st.random || st.repeat != u->st.repeat ||
	      strcmp(st.error, u->st.error))))
		u->dirty = 1;
	u->st = st;
}

/* The DAC has one owner at a time. If an AirPlay session is live (the hook in
 * deploy/x45-airplay-begin leaves this marker) and the user now starts MPD from
 * the panel, the newer action wins: cut the session. The supervisor restarts the
 * receiver a moment later, so AirPlay is available again right away. */
static void preempt_airplay(struct ui *u)
{
	if (access(AIRPLAY_MARKER, F_OK) != 0)
		return;
	/* the session may be AirPlay or Spotify Connect (same marker) */
	if (system("r=1; pkill -x shairport-sync && r=0; pkill -x librespot && r=0; exit $r") != 0)
		return;
	unlink(AIRPLAY_MARKER);
	usleep(300000);   /* let the process exit and release the PCM */
	say(u, "AirPlay stopped");
}

static int mpd_do(struct ui *u, const char *cmd);

/* ------------------------------------------------------------------ CD --- */
/* The built-in drive is played by x45cd (a child of the UI, like the meter
 * helper). It owns the DAC while it runs, so MPD is stopped first; next/prev
 * are signals and the state comes back through /tmp/x45cd.status. */

#define X45CD "/home/root/x45new/bin/x45cd"
#define X45REC "/home/root/x45new/bin/x45rec"
#define REC_STATUS "/tmp/x45rec.status"
#define CD_STATUS "/tmp/x45cd.status"

static int cd_active(const struct ui *u) { return u->cd_pid > 0; }

static void rec_stop(struct ui *u)
{
	int i, done = 0;

	if (u->rec_pid <= 0)
		return;
	/* like the meter: finish politely, SIGKILL at 192k can wedge the XMOS */
	kill(u->rec_pid, SIGTERM);
	for (i = 0; i < 250 && !done; i++) {
		if (waitpid(u->rec_pid, NULL, WNOHANG) == u->rec_pid)
			done = 1;
		else
			usleep(20000);
	}
	if (!done) {
		kill(u->rec_pid, SIGKILL);
		waitpid(u->rec_pid, NULL, 0);
	}
	u->rec_pid = 0;
	if (mpdc_is_open(&u->mpd))
		mpdc_run(&u->mpd, "update", NULL, NULL, NULL, 0);   /* the library lists the new file */
	say(u, "recording saved");
	u->dirty = 1;
}

static void cd_stop(struct ui *u);
static void meter_stop(struct ui *u);

static void spec_stop(struct ui *u);

/* The file's name part: the input, or for an internet radio stream the station. */
static void rec_name(struct ui *u, char *out, size_t cap)
{
	const char *n = in_name(u->input);
	size_t i;

	if (u->input == X45_IN_NETWORK && u->have_song && !strncmp(u->song.file, "http", 4) && u->song.name[0])
		n = u->song.name;
	snprintf(out, cap, "%s", n);
	for (i = 0; out[i]; i++)   /* a name that is safe in a file name */
		if (strchr("/\\:*?\"<>|", out[i]) || (unsigned char)out[i] < 32)
			out[i] = ' ';
}

/* Records whatever the selected input plays: the disc is ripped on the CD input, the capture of the DAC's
 * feed on the others (the radios at 44.1 kHz / 16 bit). The file carries the input's name. */
static void rec_start(struct ui *u)
{
	pid_t pid;
	int cd = u->input == UI_IN_CD;
	char name[96];

	if (u->rec_pid > 0)
		return;
	rec_name(u, name, sizeof name);
	if (cd) {
		cd_stop(u);   /* the drive is read digitally: stop playback first */
	} else {
		meter_stop(u);   /* release the capture device */
		spec_stop(u);    /* the analyzer holds it too */
	}
	pid = fork();
	if (pid < 0) {
		say(u, "cannot start the recorder");
		return;
	}
	if (pid == 0) {
		int nul = open("/dev/null", O_RDWR);

		if (nul >= 0) {
			int lg = open("/tmp/x45rec.log", O_WRONLY | O_CREAT | O_APPEND, 0644);

			dup2(nul, 0);
			dup2(nul, 1);
			dup2(lg >= 0 ? lg : nul, 2);
		}
		if (cd)
			execl(X45REC, "x45rec", "cd", "-f", rip_fmt_arg[u->rip_fmt], (char *)NULL);
		else if (tuner_input(u))
			execl(X45REC, "x45rec", "in", "-r", "44100", "-b", "16", "-n", name, (char *)NULL);
		else
			execl(X45REC, "x45rec", "in", "-n", name, (char *)NULL);
		_exit(127);
	}
	u->rec_pid = pid;
	u->rec_cd = cd;
	u->rec_poll = 0;
	u->rec_secs = 0;
	u->rec_track = 0;
	u->rec_tracks = 0;
	snprintf(u->rec_state, sizeof u->rec_state, "rec");
	say(u, cd ? "ripping the disc to disk" : "recording %s to disk", name);
	u->rec_note_t = now_ms();   /* and a large card for a few seconds: the line above is easy to miss */
	u->dirty = 1;
}

/* REC: start, or stop when it runs */
static void rec_toggle(struct ui *u)
{
	if (u->rec_pid > 0)
		rec_stop(u);
	else
		rec_start(u);
}

static void rec_poll(struct ui *u)
{
	long now = now_ms();
	FILE *f;
	char line[300];
	int st = 0;

	if (u->rec_pid <= 0)
		return;
	if (waitpid(u->rec_pid, &st, WNOHANG) == u->rec_pid) {
		u->rec_pid = 0;
		f = fopen(REC_STATUS, "r");
		/* a finished rip leaves state=done, a failure state=error + msg */
		if (f) {
			while (fgets(line, sizeof line, f))
				if (!strncmp(line, "msg=", 4) && line[4] != '\n') {
					line[strcspn(line, "\r\n")] = '\0';
					say(u, "recording stopped: %s", line + 4);
					fclose(f);
					u->dirty = 1;
					return;
				}
			fclose(f);
		}
		if (mpdc_is_open(&u->mpd))
			mpdc_run(&u->mpd, "update", NULL, NULL, NULL, 0);
		say(u, u->rec_cd ? "disc ripped" : "recording saved");
		u->dirty = 1;
		return;
	}
	if (now - u->rec_poll < 500)
		return;
	u->rec_poll = now;
	f = fopen(REC_STATUS, "r");
	if (!f)
		return;
	while (fgets(line, sizeof line, f)) {
		if (!strncmp(line, "secs=", 5)) u->rec_secs = atol(line + 5);
		else if (!strncmp(line, "track=", 6)) u->rec_track = atoi(line + 6);
		else if (!strncmp(line, "tracks=", 7)) u->rec_tracks = atoi(line + 7);
	}
	fclose(f);
	u->dirty = 1;
}

/* release the DAC from MPD without going through the input routing in mpd_do */
static void mpd_raw_stop(struct ui *u)
{
	if (mpdc_is_open(&u->mpd)) {
		mpdc_run(&u->mpd, "stop", NULL, NULL, NULL, 0);
		mpdc_run(&u->mpd, "clearerror", NULL, NULL, NULL, 0);   /* an old "no such device" must not look like a new start */
	}
}

static void cd_stop(struct ui *u)
{
	int i, done = 0;

	if (u->cd_pid <= 0)
		return;
	kill(u->cd_pid, SIGTERM);
	for (i = 0; i < 150 && !done; i++) {
		if (waitpid(u->cd_pid, NULL, WNOHANG) == u->cd_pid)
			done = 1;
		else
			usleep(20000);
	}
	if (!done) {
		kill(u->cd_pid, SIGKILL);
		waitpid(u->cd_pid, NULL, 0);
	}
	u->cd_pid = 0;
	u->cd_playing = 0;
	u->cd_track = 0;
	u->cd_hdcd = u->cd_hdcd_pe = u->cd_hdcd_gain = u->cd_hdcd_on = 0;
	u->dirty = 1;
}

/* CD-Text from `x45cd text`: "T <n> <title>" / "P <n> <performer>", n = 0 the album */
static void cd_load_text(struct ui *u)
{
	FILE *p;
	char line[300], album[40] = "", artist[40] = "";

	memset(u->cd_title, 0, sizeof u->cd_title);
	u->cd_album[0] = '\0';
	p = popen(X45CD " text 2>/dev/null", "r");
	if (!p)
		return;
	while (fgets(line, sizeof line, p)) {
		char kind = line[0], *txt, *nl;
		int n;

		if ((kind != 'T' && kind != 'P') || sscanf(line + 1, "%d", &n) != 1 || n < 0 || n > 99)
			continue;
		txt = strchr(line + 2, ' ');
		if (!txt)
			continue;
		txt++;
		if ((nl = strpbrk(txt, "\r\n")))
			*nl = '\0';
		if (kind == 'T' && n == 0)
			snprintf(album, sizeof album, "%s", txt);
		else if (kind == 'P' && n == 0)
			snprintf(artist, sizeof artist, "%s", txt);
		else if (kind == 'T')
			snprintf(u->cd_title[n - 1], sizeof u->cd_title[0], "%s", txt);
	}
	pclose(p);
	if (album[0] && artist[0])
		snprintf(u->cd_album, sizeof u->cd_album, "%s - %s", album, artist);
	else
		snprintf(u->cd_album, sizeof u->cd_album, "%s", album[0] ? album : artist);
}

static void cd_load(struct ui *u)
{
	FILE *p;
	char line[96];
	int n = 0, i, t, len;
	char kind[16];

	u->cd_err[0] = '\0';
	if (cd_active(u) && u->cd_ntracks > 0)
		return;
	u->cd_ntracks = 0;
	u->cd_sig_n = u->cd_sig_total = u->cd_dvd = 0;
	u->cd_note[0] = '\0';
	u->cd_hdcd = u->cd_hdcd_pe = u->cd_hdcd_gain = u->cd_hdcd_on = 0;   /* what the last disc showed is not this one's */
	p = popen(X45CD " toc 2>&1", "r");
	if (!p) {
		snprintf(u->cd_err, sizeof u->cd_err, "Cannot start x45cd");
		return;
	}
	if (!fgets(line, sizeof line, p) || sscanf(line, "%d %d", &n, &len) != 2) {
		snprintf(u->cd_err, sizeof u->cd_err, "No disc");
		pclose(p);
		return;
	}
	u->cd_sig_n = n;
	u->cd_sig_total = len;
	/* a video DVD with PCM sound: "<n> <frames> dvd <chapters>", and the chapters follow as tracks */
	u->cd_dvd = 0;
	int rate = 0, bits = 0;

	if (sscanf(line, "%*d %*d %15s %d %d %d", kind, &t, &rate, &bits) >= 2 && !strcmp(kind, "dvd") && t > 0) {
		u->cd_dvd = 1;
		u->cd_rate = rate;
		u->cd_bits = bits;
		n = t;
	} else if (sscanf(line, "%*d %*d %15s %23s", kind, u->cd_note) == 2 && !strcmp(kind, "dvdx")) {
		char *q;

		for (q = u->cd_note; *q; q++)
			if (*q == '_')
				*q = ' ';
	} else {
		u->cd_note[0] = '\0';
	}
	for (i = 0; i < n && i < 99 && fgets(line, sizeof line, p); i++)
		if (sscanf(line, "%d %15s %d", &t, kind, &len) == 3) {
			u->cd_len[i] = !strcmp(kind, "audio") ? len / 75 : -1;
			u->cd_ntracks = i + 1;
		}
	pclose(p);
	if (u->cd_ntracks == 0)
		snprintf(u->cd_err, sizeof u->cd_err, "No disc");
	else
		cd_load_text(u);
	if (u->cd_sel >= u->cd_ntracks)
		u->cd_sel = 0;
}

/* cd_load with a "loading" screen: reading a disc takes seconds and blocks the UI */
static void cd_load_show(struct ui *u)
{
	if (cd_active(u)) {
		cd_load(u);
		return;
	}
	u->cd_loading = 1;
	u->dirty = 1;
	render(u);
	cd_load(u);
	u->cd_loading = 0;
	u->dirty = 1;
}

static void cd_play(struct ui *u, int track)
{
	char tr[16];
	pid_t pid;

	/* The disc plays only on the CD input. Starting it from any other input used to switch
	 * the path silently, which pulled the XMOS away from PC USB / the digital inputs. */
	if (u->input != UI_IN_CD) {
		say(u, "select the CD input first");
		return;
	}
	if (u->cd_note[0]) {
		say(u, "DVD sound is %s - cannot play", u->cd_note);
		return;
	}
	cd_stop(u);
	mpd_raw_stop(u);
	unlink(CD_STATUS);   /* the last player's last word ("stop", track 0) must not pass for the new one's */
	snprintf(tr, sizeof tr, "%d", track);
	pid = fork();
	if (pid < 0) {
		say(u, "Cannot start the CD player");
		return;
	}
	if (pid == 0) {
		setenv("X45CD_FOLLOW_PARENT", "1", 1);
		execl(X45CD, "x45cd", "play", tr, (char *)NULL);
		_exit(127);
	}
	u->cd_pid = pid;
	u->cd_track = track;
	u->cd_pos = 0;
	u->cd_tlen = track >= 1 && track <= u->cd_ntracks && u->cd_len[track - 1] > 0 ? u->cd_len[track - 1] : 0;
	u->cd_playing = 1;
	u->cd_lastpoll = 0;
	u->dirty = 1;
	say(u, "%s: %s %d", u->cd_dvd ? "DVD" : "CD", u->cd_dvd ? "chapter" : "track", track);
}

/* Reaps the player and mirrors its status file into the UI state. */
static void cd_poll(struct ui *u)
{
	long now = now_ms();
	FILE *f;
	char st[8];
	int tr, pos, len, n, hd[4];

	if (u->cd_pid <= 0 || ms_since(now, u->cd_lastpoll) < 150)
		return;
	u->cd_lastpoll = now;
	if (waitpid(u->cd_pid, NULL, WNOHANG) == u->cd_pid) {
		u->cd_pid = 0;
		u->cd_playing = 0;
		u->cd_track = 0;
		u->cd_hdcd = u->cd_hdcd_pe = u->cd_hdcd_gain = u->cd_hdcd_on = 0;
		u->dirty = 1;
		return;
	}
	f = fopen(CD_STATUS, "r");
	if (!f)
		return;
	hd[0] = hd[1] = hd[2] = hd[3] = 0;     /* an older x45cd leaves these out */
	if (fscanf(f, "%7s %d %d %d %d %d %d %d %d", st, &tr, &pos, &len, &n,
		   &hd[0], &hd[1], &hd[2], &hd[3]) >= 5) {
		int playing = !strcmp(st, "play");

		/* a skip was asked for a moment ago and the file still tells the old track or position: the number on the
		 * screen is already the new one, wait for the player's answer (which starts at second 0) */
		if (u->cd_skip_t) {
			if (ms_since(now, u->cd_skip_t) < 700 && !(tr == u->cd_track && pos <= 2)) {
				fclose(f);
				return;
			}
			u->cd_skip_t = 0;
		}
		if (tr != u->cd_track || pos != u->cd_pos || playing != u->cd_playing ||
		    len != u->cd_tlen || hd[0] != u->cd_hdcd || hd[1] != u->cd_hdcd_pe ||
		    hd[2] != u->cd_hdcd_gain || hd[3] != u->cd_hdcd_on) {
			u->cd_track = tr;
			u->cd_pos = pos;
			u->cd_tlen = len;
			u->cd_playing = playing;
			u->cd_hdcd = hd[0];
			u->cd_hdcd_pe = hd[1];
			u->cd_hdcd_gain = hd[2];
			u->cd_hdcd_on = hd[3];
			u->dirty = 1;
		}
	}
	fclose(f);
}

/* ------------------------------------------------------------- bluetooth --- */
/* The Bluetooth input. The USB radio stick is switched on only while this input is chosen:
 * x45bt-run loads the stick's firmware (about 30 s after a power-up, a few seconds when it is
 * still loaded) and becomes x45bt, the A2DP speaker. The UI keeps the write end of its stdin
 * and sends one letter per command (y/n pairing, k/L/K/i/I transport, D pair a new device,
 * F forget all); it answers through BT_STATUS. When the UI goes away the pipe closes and
 * x45bt leaves by itself. */

#define X45BT_RUN "/home/root/x45new/bin/x45bt-run"
#define BT_STATUS "/tmp/x45bt.status"
#define BT_LOG "/tmp/x45bt.log"

static int bt_active(const struct ui *u) { return u->bt_pid > 0; }

static void bt_cmd(struct ui *u, char c)
{
	ssize_t r;

	if (u->bt_pid <= 0 || u->bt_wfd < 0)
		return;
	r = write(u->bt_wfd, &c, 1);
	(void)r;
}

static void bt_forget_state(struct ui *u)
{
	u->bt_state[0] = '\0';
	u->bt_name[0] = u->bt_confirm[0] = u->bt_confirm_name[0] = '\0';
	u->bt_title[0] = u->bt_artist[0] = u->bt_album[0] = '\0';
	u->bt_step[0] = '\0';
	u->bt_bonds = u->bt_open = u->bt_rate = u->bt_avrcp_play = 0;
	u->bt_seq = 0;
}

static void bt_start(struct ui *u)
{
	int fds[2];
	pid_t pid;

	if (u->bt_pid > 0)
		return;
	unlink(BT_STATUS);
	bt_forget_state(u);
	u->bt_err[0] = '\0';
	snprintf(u->bt_state, sizeof u->bt_state, "starting");
	if (pipe(fds) < 0) {
		snprintf(u->bt_err, sizeof u->bt_err, "cannot start bluetooth");
		return;
	}
	pid = fork();
	if (pid < 0) {
		close(fds[0]);
		close(fds[1]);
		snprintf(u->bt_err, sizeof u->bt_err, "cannot start bluetooth");
		return;
	}
	if (pid == 0) {
		int log = open(BT_LOG, O_WRONLY | O_CREAT | O_TRUNC, 0644);

		dup2(fds[0], 0);
		if (log >= 0) {
			dup2(log, 1);
			dup2(log, 2);
		}
		close(fds[0]);
		close(fds[1]);
		signal(SIGINT, SIG_DFL);
		signal(SIGTERM, SIG_DFL);
		signal(SIGPIPE, SIG_DFL);
		execl(X45BT_RUN, "x45bt-run", (char *)NULL);
		_exit(127);
	}
	close(fds[0]);
	fcntl(fds[1], F_SETFD, FD_CLOEXEC);   /* other children must not keep the pipe open */
	u->bt_wfd = fds[1];
	u->bt_pid = pid;
	u->bt_started = now_ms();
	u->bt_lastpoll = 0;
	u->dirty = 1;
}

/* SIGINT is the polite way out: x45bt stops the audio, drops the link, switches the scans off and
 * resets the stick. A kill while it streams can wedge the DAC's USB endpoint, so wait for it. */
static void bt_stop(struct ui *u)
{
	int i, done = 0;

	if (u->bt_pid <= 0)
		return;
	kill(u->bt_pid, SIGINT);
	for (i = 0; i < 500 && !done; i++) {
		if (waitpid(u->bt_pid, NULL, WNOHANG) == u->bt_pid)
			done = 1;
		else
			usleep(20000);
	}
	if (!done) {
		kill(u->bt_pid, SIGKILL);
		waitpid(u->bt_pid, NULL, 0);
	}
	if (u->bt_wfd >= 0)
		close(u->bt_wfd);
	u->bt_wfd = -1;
	u->bt_pid = 0;
	unlink(BT_STATUS);
	bt_forget_state(u);
	u->dirty = 1;
}

static void bt_read_status(struct ui *u)
{
	FILE *f = fopen(BT_STATUS, "r");
	char line[256];
	unsigned seq = 0;
	int have_seq = 0;

	if (!f)
		return;
	while (fgets(line, sizeof line, f)) {
		char *v = strchr(line, '=');

		if (!v)
			continue;
		*v++ = '\0';
		v[strcspn(v, "\r\n")] = '\0';
		if (!strcmp(line, "seq")) {
			seq = (unsigned)atol(v);
			have_seq = 1;
			if (seq == u->bt_seq) {   /* nothing new */
				fclose(f);
				return;
			}
		} else if (!strcmp(line, "state")) snprintf(u->bt_state, sizeof u->bt_state, "%s", v);
		else if (!strcmp(line, "step")) snprintf(u->bt_step, sizeof u->bt_step, "%s", v);
		else if (!strcmp(line, "error")) snprintf(u->bt_err, sizeof u->bt_err, "%s", v);
		else if (!strcmp(line, "name")) snprintf(u->bt_name, sizeof u->bt_name, "%s", v);
		else if (!strcmp(line, "confirm")) snprintf(u->bt_confirm, sizeof u->bt_confirm, "%s", v);
		else if (!strcmp(line, "confirm_name")) snprintf(u->bt_confirm_name, sizeof u->bt_confirm_name, "%s", v);
		else if (!strcmp(line, "bonds")) u->bt_bonds = atoi(v);
		else if (!strcmp(line, "pairing_open")) u->bt_open = atoi(v);
		else if (!strcmp(line, "rate")) u->bt_rate = atoi(v);
		else if (!strcmp(line, "avrcp_play")) u->bt_avrcp_play = atoi(v);
		else if (!strcmp(line, "title")) snprintf(u->bt_title, sizeof u->bt_title, "%s", v);
		else if (!strcmp(line, "artist")) snprintf(u->bt_artist, sizeof u->bt_artist, "%s", v);
		else if (!strcmp(line, "album")) snprintf(u->bt_album, sizeof u->bt_album, "%s", v);
	}
	fclose(f);
	if (have_seq)
		u->bt_seq = seq;
	u->dirty = 1;
}

/* Reaps x45bt and mirrors its status file into the UI state. */
static void bt_poll(struct ui *u)
{
	long now = now_ms();

	if (u->bt_pid <= 0 || now - u->bt_lastpoll < 250)
		return;
	u->bt_lastpoll = now;
	if (waitpid(u->bt_pid, NULL, WNOHANG) == u->bt_pid) {
		u->bt_pid = 0;
		if (u->bt_wfd >= 0)
			close(u->bt_wfd);
		u->bt_wfd = -1;
		bt_read_status(u);   /* x45bt-run leaves the reason behind when it cannot start */
		if (!u->bt_err[0])
			snprintf(u->bt_err, sizeof u->bt_err, "bluetooth stopped, see " BT_LOG);
		u->bt_state[0] = '\0';
		u->dirty = 1;
		return;
	}
	bt_read_status(u);
}

static void spec_stop(struct ui *u)
{
	int i, done = 0;

	if (u->sp_pid > 0) {
		kill(u->sp_pid, SIGTERM);
		for (i = 0; i < 100 && !done; i++) {
			if (waitpid(u->sp_pid, NULL, WNOHANG) == u->sp_pid)
				done = 1;
			else
				usleep(20000);
		}
		if (!done) {
			kill(u->sp_pid, SIGKILL);
			waitpid(u->sp_pid, NULL, 0);
		}
	}
	if (u->sp_mode == 2 && mpdc_is_open(&u->mpd))
		mpdc_run(&u->mpd, "disableoutput 1", NULL, NULL, NULL, 0);
	u->sp_pid = 0;
	u->sp_mode = 0;
	u->sp_rate = 0;
}

/* The analyzer behind the web meters: the capture for the external inputs is already
 * running as the panel meter; the radio is read from the same capture at 44.1 kHz, and
 * network playback from MPD's meter FIFO. Anything else (PC USB, CD, AirPlay, Spotify)
 * never passes through a place the unit can tap. Runs only while a browser asks. */
static void spec_poll(struct ui *u)
{
	struct stat st;
	/* a browser asks, or the panel's own player screen is up */
	int want = (stat(METER_WANT, &st) == 0 && time(NULL) - st.st_mtime < 6) ||
		   (u->screen == SCR_PLAYER && u->rec_pid <= 0 && vis_get(u));
	int mode = 0;
	unsigned rate = 0;
	pid_t pid;

	if (getenv("X45UI_PREVIEW"))   /* host preview: a script writes the analyzer file, nothing is spawned */
		return;
	if (!want) {
		if (u->sp_pid > 0)
			spec_stop(u);
		u->sp_why = "";
		return;
	}
	if (meter_wanted(u)) {
		if (u->sp_pid > 0)
			spec_stop(u);
		u->sp_why = u->m_pid > 0 ? "" : "capture not running";
		return;
	}
	if (tuner_input(u)) {
		mode = 1;
		rate = 44100;
	} else if (u->input == X45_IN_NETWORK && u->sp_fails >= 2) {
		/* the SSI capture is dead: MPD can still hand its own samples to the analyzer (while it plays) */
		if (u->st.audio[0] >= '0' && u->st.audio[0] <= '9' && atoi(u->st.audio) >= 8000) {
			mode = 2;
			rate = (unsigned)atoi(u->st.audio);
		}
	} else {
		/* everything else - network, AirPlay, Spotify, CD, PC USB - ends up at the DAC through the
		 * same SSI port the stock firmware reads its meters from, so one 192 kHz capture serves
		 * all of them */
		mode = 3;
		rate = 192000;
	}
	if (u->sp_pid > 0) {
		/* reap it once: a second waitpid on a child already collected finds nothing, and spec_stop would
		 * then wait two seconds for a process that is gone (and signal a pid that may be somebody else's) */
		int died = waitpid(u->sp_pid, NULL, WNOHANG) == u->sp_pid;

		if (died) {
			/* the SSI capture gets no clocks when its state is off (it waits, then fails): count the captures
			 * that never wrote a frame, and let the network input use the fifo of MPD instead */
			if (u->sp_mode == 3) {
				struct stat ms;

				if (stat(METER_FILE, &ms) == 0 && ms.st_mtime >= u->sp_spawn)
					u->sp_fails = 0;
				else if (u->sp_fails < 100)
					u->sp_fails++;
			}
			u->sp_pid = 0;
			u->sp_retry = now_ms() + 2000;   /* the capture died: do not respawn at once */
			spec_stop(u);                    /* only the bookkeeping is left to clear */
		} else if (mode != u->sp_mode || rate != u->sp_rate) {
			spec_stop(u);
		} else if (u->sp_mode == 3 && !u->sp_ok && time(NULL) - u->sp_spawn > 6) {
			/* a capture that has written nothing after six seconds is waiting for clocks that will not come */
			struct stat ms;

			if (stat(METER_FILE, &ms) == 0 && ms.st_mtime >= u->sp_spawn) {
				u->sp_ok = 1;
				u->sp_fails = 0;
			} else {
				if (u->sp_fails < 100)
					u->sp_fails++;
				u->sp_retry = now_ms() + 2000;
				spec_stop(u);
			}
		}
	}
	if (!mode || u->sp_pid > 0 || !ms_due(now_ms(), u->sp_retry))
		return;
	if (mode == 2) {
		if (mpdc_is_open(&u->mpd))
			mpdc_run(&u->mpd, "enableoutput 1", NULL, NULL, NULL, 0);
		else
			return;
	}
	pid = fork();
	if (pid < 0)
		return;
	if (pid == 0) {
		char r[16];
		int nul = open("/dev/null", O_RDWR);

		if (nul >= 0) {
			dup2(nul, 0);
			dup2(nul, 1);
			dup2(nul, 2);
		}
		snprintf(r, sizeof r, "%u", rate);
		if (mode == 2)
			execl("/home/root/x45new/bin/x45loop", "x45loop", "-r", r, "-f", METER_FIFO, "-s", METER_FILE, (char *)NULL);
		else if (mode == 3)
			execl("/home/root/x45new/bin/x45loop", "x45loop", "-s", METER_FILE, (char *)NULL);
		else
			execl("/home/root/x45new/bin/x45loop", "x45loop", "-r", r, "-b", "16", "-s", METER_FILE, (char *)NULL);
		_exit(127);
	}
	u->sp_pid = pid;
	u->sp_mode = mode;
	u->sp_rate = rate;
	u->sp_spawn = time(NULL) - 1;
	u->sp_ok = 0;
	u->sp_why = "";
}

static void cd_probe_end(struct ui *u)
{
	if (u->cd_probe_pid > 0) {
		kill(u->cd_probe_pid, SIGKILL);
		waitpid(u->cd_probe_pid, NULL, 0);
	}
	if (u->cd_probe_fd > 0)
		close(u->cd_probe_fd);
	u->cd_probe_pid = 0;
	u->cd_probe_fd = 0;
}

/* The CD input looks at the drive by itself: a disc that is put in is listed, one that is taken out or
 * swapped is forgotten (its tracks, its HDCD flag), whatever way it came out (the drive's own button too).
 * `x45cd id` asks the drive in a child; the answer is collected without ever waiting for it, because a
 * drive that spins up or reads a bad disc takes seconds to answer. */
static void cd_watch(struct ui *u)
{
	long now = now_ms();

	if (u->input != UI_IN_CD || cd_active(u) || u->rec_pid > 0) {
		cd_probe_end(u);
		return;
	}
	if (u->cd_probe_pid > 0) {
		char tmp[32];
		ssize_t r;
		int status, n = 0, total = 0, ok;

		while ((r = read(u->cd_probe_fd, tmp, sizeof tmp)) > 0)
			if (u->cd_probe_len + r < (int)sizeof u->cd_probe_buf) {
				memcpy(u->cd_probe_buf + u->cd_probe_len, tmp, (size_t)r);
				u->cd_probe_len += (int)r;
			}
		if (waitpid(u->cd_probe_pid, &status, WNOHANG) != u->cd_probe_pid) {
			if (now - u->cd_probe_t0 > 20000)
				cd_probe_end(u);
			return;
		}
		u->cd_probe_pid = 0;
		close(u->cd_probe_fd);
		u->cd_probe_fd = 0;
		u->cd_probe_buf[u->cd_probe_len] = '\0';
		ok = WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
		     sscanf(u->cd_probe_buf, "%d %d", &n, &total) == 2;
		if (!ok) {
			if (u->cd_ntracks > 0 || u->cd_sig_n) {
				u->cd_ntracks = 0;
				u->cd_sig_n = u->cd_sig_total = u->cd_dvd = 0;
				u->cd_note[0] = '\0';
				u->cd_hdcd = u->cd_hdcd_pe = u->cd_hdcd_gain = u->cd_hdcd_on = 0;
				snprintf(u->cd_err, sizeof u->cd_err, "No disc");
				u->dirty = 1;
			}
		} else if (u->cd_ntracks == 0 || n != u->cd_sig_n || total != u->cd_sig_total) {
			cd_load_show(u);
		}
		return;
	}
	if (!ms_due(now, u->cd_watch_t))
		return;
	u->cd_watch_t = now + 2000;
	{
		int fds[2];
		pid_t pid;

		if (pipe(fds) < 0)
			return;
		pid = fork();
		if (pid < 0) {
			close(fds[0]);
			close(fds[1]);
			return;
		}
		if (pid == 0) {
			int nul = open("/dev/null", O_RDWR);

			if (nul >= 0) {
				dup2(nul, 0);
				dup2(nul, 2);
			}
			dup2(fds[1], 1);
			close(fds[0]);
			close(fds[1]);
			execl(X45CD, "x45cd", "id", (char *)NULL);
			_exit(127);
		}
		close(fds[1]);
		fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);
		u->cd_probe_pid = pid;
		u->cd_probe_fd = fds[0];
		u->cd_probe_len = 0;
		u->cd_probe_t0 = now;
	}
}

/* ---------------------------------------------------------------- tuner --- */
/* The DAB+/FM module is driven by x45tuner (a child of the UI, like x45cd). It
 * takes one text command per line on a FIFO and reports through a status file;
 * the station lists it builds live in etc/fm.list and etc/dab.list. */

#define X45TUNER    "/home/root/x45new/bin/x45tuner"
#define TU_STATUS   "/tmp/x45tuner.status"
#define TU_FIFO     "/tmp/x45tuner.cmd"
#define TU_FMLIST   "/home/root/x45new/etc/fm.list"
#define TU_DABLIST  "/home/root/x45new/etc/dab.list"

static void tuner_cmd(struct ui *u, const char *fmt, ...)
{
	char line[64];
	va_list ap;
	int fd;

	(void)u;
	va_start(ap, fmt);
	vsnprintf(line, sizeof line - 1, fmt, ap);
	va_end(ap);
	strcat(line, "\n");
	fd = open(TU_FIFO, O_WRONLY | O_NONBLOCK);
	if (fd >= 0) {
		/* best effort: a full or vanished FIFO just drops the command */
		if (write(fd, line, strlen(line)) < 0)
			(void)0;
		close(fd);
	}
}

static void tuner_stop(struct ui *u)
{
	int i, done = 0;

	if (u->tu_pid <= 0)
		return;
	u->tu_unclean = !u->tu_up;   /* cut off before it was up: the module may be half way through a set-up */
	kill(u->tu_pid, SIGTERM);
	for (i = 0; i < 150 && !done; i++) {
		if (waitpid(u->tu_pid, NULL, WNOHANG) == u->tu_pid)
			done = 1;
		else
			usleep(20000);
	}
	if (!done) {
		kill(u->tu_pid, SIGKILL);
		waitpid(u->tu_pid, NULL, 0);
		u->tu_unclean = 1;
	}
	u->tu_pid = 0;
	u->tu_up = 0;
	u->dirty = 1;
}

static void tuner_start(struct ui *u, int dab)
{
	pid_t pid;
	int reset;

	tuner_stop(u);
	reset = u->tu_unclean;
	u->tu_unclean = 0;
	unlink(TU_STATUS);
	memset(u->tu_state, 0, sizeof u->tu_state);
	u->tu_ps[0] = u->tu_svc[0] = u->tu_ens[0] = u->tu_text[0] = '\0';
	u->tu_rt[0] = u->tu_time[0] = '\0';
	x45_image_free(&u->slide);
	u->tu_slide = u->slide_seen = 0;
	u->tu_freq = u->tu_rssi = u->tu_snr = u->tu_found = u->tu_q = u->tu_sq = u->tu_scan = 0;
	u->tu_fmscan = u->tu_up = 0;
	pid = fork();
	if (pid < 0) {
		say(u, "cannot start the tuner");
		return;
	}
	if (pid == 0) {
		int nul = open("/dev/null", O_RDWR);

		if (nul >= 0) {
			dup2(nul, 0);
			dup2(nul, 1);
		}
		{
			int lg = open("/tmp/x45tuner.log", O_WRONLY | O_CREAT | O_APPEND, 0644);

			if (lg >= 0)
				dup2(lg, 2);
		}
		setenv("X45CD_FOLLOW_PARENT", "1", 1);
		if (reset)
			setenv("X45TUNER_RESET", "1", 1);
		execl(X45TUNER, "x45tuner", "run", dab ? "dab" : "fm", (char *)NULL);
		_exit(127);
	}
	u->tu_pid = pid;
	u->tu_mode = dab;
	u->tu_lastpoll = 0;
	u->dirty = 1;
}

/* fills the station list screen from the module's list files */
static void tuner_load_list(struct ui *u)
{
	FILE *f = fopen(u->input == UI_IN_DAB ? TU_DABLIST : TU_FMLIST, "r");
	char line[96];

	u->tl_n = 0;
	if (!f)
		return;
	while (u->tl_n < 128 && fgets(line, sizeof line, f)) {
		int n = u->tl_n;

		line[strcspn(line, "\r\n")] = '\0';
		if (u->input == UI_IN_DAB) {
			char *l = strchr(line, '|');

			l = l ? strchr(l + 1, '|') : NULL;
			l = l ? strchr(l + 1, '|') : NULL;
			if (!l)
				continue;
			snprintf(u->tl_name[n], sizeof u->tl_name[n], "%s", l + 1);
			u->tl_val[n] = n;
		} else {
			int fr;

			char *nm = strchr(line, '|');

			if (sscanf(line, "%d", &fr) != 1)
				continue;
			if (nm && nm[1])
				snprintf(u->tl_name[n], sizeof u->tl_name[n], "%d.%02d  %s", fr / 100, fr % 100, nm + 1);
			else
				snprintf(u->tl_name[n], sizeof u->tl_name[n], "%d.%02d MHz", fr / 100, fr % 100);
			u->tl_val[n] = fr;
		}
		u->tl_n++;
	}
	fclose(f);
	if (u->tl_sel >= u->tl_n)
		u->tl_sel = 0;
	if (u->tl_top > u->tl_sel)
		u->tl_top = u->tl_sel;
}

/* what the screens show of the tuner: compared before and after reading its status file. (Copying the whole
 * struct ui for that, as this once did, moved a few hundred KB ten times a second.) */
struct tu_snap {
	int freq, rssi, snr, found, scan, nsvc, sel, q, sq, up, fmscan, npreset, slide;
	char state[12], ps[16], svc[24], ens[24], text[140], rt[72], time[40];
};

static void tu_snapshot(const struct ui *u, struct tu_snap *s)
{
	memset(s, 0, sizeof *s);
	s->freq = u->tu_freq; s->rssi = u->tu_rssi; s->snr = u->tu_snr; s->found = u->tu_found; s->scan = u->tu_scan;
	s->nsvc = u->tu_nsvc; s->sel = u->tu_sel; s->q = u->tu_q; s->sq = u->tu_sq; s->up = u->tu_up; s->fmscan = u->tu_fmscan;
	s->npreset = u->tu_npreset; s->slide = u->tu_slide;
	snprintf(s->state, sizeof s->state, "%s", u->tu_state);
	snprintf(s->ps, sizeof s->ps, "%s", u->tu_ps);
	snprintf(s->svc, sizeof s->svc, "%s", u->tu_svc);
	snprintf(s->ens, sizeof s->ens, "%s", u->tu_ens);
	snprintf(s->text, sizeof s->text, "%s", u->tu_text);
	snprintf(s->rt, sizeof s->rt, "%s", u->tu_rt);
	snprintf(s->time, sizeof s->time, "%s", u->tu_time);
}

static void tuner_poll(struct ui *u)
{
	long now = now_ms();
	FILE *f;
	char line[200];
	int up = 0;

	if (u->tu_pid <= 0 || now - u->tu_lastpoll < 100)
		return;
	u->tu_lastpoll = now;
	if (waitpid(u->tu_pid, NULL, WNOHANG) == u->tu_pid) {
		u->tu_pid = 0;
		u->tu_up = 0;
		u->dirty = 1;
		if (tuner_input(u) && u->tu_retries < 2) {
			/* it gave up before the module answered (the module is slow to come back after a switch
			 * between DAB and FM): start it again, with a reset */
			u->tu_retries++;
			u->tu_unclean = 1;
			tuner_start(u, u->input == UI_IN_DAB);
		}
		return;
	}
	f = fopen(TU_STATUS, "r");
	if (!f)
		return;
	{
		struct tu_snap old, cur;

		tu_snapshot(u, &old);

		while (fgets(line, sizeof line, f)) {
			char *v = strchr(line, '=');

			if (!v)
				continue;
			*v++ = '\0';
			v[strcspn(v, "\r\n")] = '\0';
			{   /* radiotext is often padded with spaces to centre it on a display of its own */
				char *e = v + strlen(v);

				v += strspn(v, " ");
				while (e > v && e[-1] == ' ')
					*--e = '\0';
			}
			up = 1;
			if (!strcmp(line, "state")) snprintf(u->tu_state, sizeof u->tu_state, "%s", v);
			else if (!strcmp(line, "rssi")) u->tu_rssi = atoi(v);
			else if (!strcmp(line, "snr")) u->tu_snr = atoi(v);
			else if (!strcmp(line, "freq")) u->tu_freq = atoi(v);
			else if (!strcmp(line, "found")) u->tu_found = atoi(v);
			else if (!strcmp(line, "ps")) snprintf(u->tu_ps, sizeof u->tu_ps, "%s", v);
			else if (!strcmp(line, "fmscan")) u->tu_fmscan = atoi(v);
			else if (!strcmp(line, "npreset")) u->tu_npreset = atoi(v);
			else if (!strcmp(line, "scan")) u->tu_scan = atoi(v);
			else if (!strcmp(line, "nsvc")) u->tu_nsvc = atoi(v);
			else if (!strcmp(line, "sel")) u->tu_sel = atoi(v);
			else if (!strcmp(line, "q")) u->tu_q = atoi(v);
			else if (!strcmp(line, "sq")) u->tu_sq = atoi(v);
			else if (!strcmp(line, "svc")) snprintf(u->tu_svc, sizeof u->tu_svc, "%s", v);
			else if (!strcmp(line, "ens")) snprintf(u->tu_ens, sizeof u->tu_ens, "%s", v);
			else if (!strcmp(line, "text")) snprintf(u->tu_text, sizeof u->tu_text, "%s", v);
			else if (!strcmp(line, "rt")) snprintf(u->tu_rt, sizeof u->tu_rt, "%s", v);
			else if (!strcmp(line, "time")) snprintf(u->tu_time, sizeof u->tu_time, "%s", v);
			else if (!strcmp(line, "slide")) u->tu_slide = atoi(v);
			else if (!strcmp(line, "listrev")) u->tu_listrev = atoi(v);
		}
		u->tu_up = up;
		tu_snapshot(u, &cur);
		if (memcmp(&old, &cur, sizeof old) != 0)
			u->dirty = 1;
		if (u->tu_slide != u->slide_seen) {
			/* a new (or cleared) slideshow picture */
			x45_image_free(&u->slide);
			u->slide_seen = u->tu_slide;
			x45_image_load(&u->g, "/tmp/x45-slide.img", 260, 260, &u->slide);
			u->dirty = 1;
		}
	}
	fclose(f);
	{
		static int seen_rev = -1;

		if (u->tu_fmscan || !strcmp(u->tu_state, "scan") || !strcmp(u->tu_state, "list") ||
		    u->tu_listrev != seen_rev ||
		    u->tl_n != (u->input == UI_IN_DAB ? u->tu_nsvc : u->tu_npreset)) {
			seen_rev = u->tu_listrev;
			tuner_load_list(u);
			u->dirty = 1;
		}
	}
}

/* leave the radio for the network path */
static void leave_tuner(struct ui *u)
{
	tuner_stop(u);
	if (u->x45fd >= 0) {
		x45_input(u->x45fd, X45_IN_NETWORK);
		x45_outputs_apply(u->x45fd);
	}
	u->input = X45_IN_NETWORK;
	u->dirty = 1;
}

/* Move the FM frequency by d (10 kHz units) from where the dial is, or from where the last turn
 * sent it while the module's answer is still catching up. Always lands on the 50 kHz grid. */
static void tuner_step(struct ui *u, int d)
{
	static int target;
	static long target_t;
	long now = now_ms();
	int f = (target && now - target_t < 700) ? target : (u->tu_freq ? u->tu_freq : 8750);

	f = (f + 2) / 5 * 5 + d;
	if (f < 8750)
		f = 10800 - (8750 - f) % 2050;
	if (f > 10800)
		f = 8750 + (f - 10800) % 2050;
	target = f;
	target_t = now;
	u->tu_freq = f;      /* show it at once; the module confirms a moment later */
	u->tu_ps[0] = '\0';
	u->dirty = 1;
	tuner_cmd(u, "tune %d", f);
}

/* The knob (or IR left/right) on FM: a slow turn is a 0.05 MHz fine step, a faster one steps
 * further per click, a spin sweeps the band. */
static void fm_knob(struct ui *u, int ticks)
{
	static long last;
	long now = now_ms(), dt = now - last;
	int step = dt < 35 ? 100 : dt < 70 ? 50 : dt < 140 ? 20 : dt < 300 ? 10 : 5;

	last = now;
	tuner_step(u, (ticks > 0 ? step : -step) * (abs(ticks) > 1 ? abs(ticks) : 1));
}

/* next/previous: FM seeks, DAB moves along the list */
static void tuner_skip(struct ui *u, int dir)
{
	if (u->input == UI_IN_DAB) {
		if (u->tu_nsvc > 0)
			tuner_cmd(u, "select %d", (u->tu_sel + dir + u->tu_nsvc) % u->tu_nsvc);
	} else {
		tuner_cmd(u, "seek %s", dir > 0 ? "up" : "down");
	}
}

/* MPD plays on the network input only. On PC USB the XMOS is on the computer's port: MPD finds no DAC, but its
 * clock patch has already moved FS_4844 to its stream's family by then, which distorted the computer's sound. The
 * line, digital and radio inputs listen to their own sources. So starting playback, from the panel, the web page
 * or a controller, means the network input. */
static void go_network(struct ui *u)
{
	int from_pc = u->input == X45_IN_PCUSB;

	input_select(u, X45_IN_NETWORK);
	if (from_pc)
		dac_wait(4000);
}

/* Run a command and report a refusal on the status line. */
static int mpd_do(struct ui *u, const char *cmd)
{
	int r;

	if (tuner_input(u)) {
		/* the radio owns the transport: next/previous move along the stations,
		 * and anything that starts MPD playback leaves the radio */
		if (!strcmp(cmd, "next") || !strcmp(cmd, "previous")) {
			tuner_skip(u, cmd[0] == 'n' ? 1 : -1);
			return 0;
		}
		if (!strcmp(cmd, "stop")) {
			/* STOP only ever cancels a running scan; scanning is in the RETURN menu */
			if (tuner_scanning(u)) {
				tuner_cmd(u, "cancel");
				say(u, "scan stopped");
			}
			return 0;
		}
		if (!strncmp(cmd, "seekcur", 7) || !strncmp(cmd, "random", 6) ||
		    !strcmp(cmd, "pause 1"))
			return 0;
		if (!strncmp(cmd, "play", 4) || !strncmp(cmd, "clear", 5) ||
		    !strcmp(cmd, "pause 0"))
			leave_tuner(u);
	}

	if (u->input == UI_IN_BT) {
		/* Bluetooth: the transport belongs to the phone (AVRCP); anything that starts MPD
		 * playback (a library or radio choice) leaves the input for the network */
		if (!strcmp(cmd, "next") || !strcmp(cmd, "previous")) {
			bt_cmd(u, cmd[0] == 'n' ? 'i' : 'I');
			return 0;
		}
		if (!strcmp(cmd, "stop")) {
			if (u->bt_confirm[0])
				bt_cmd(u, 'n');
			else
				bt_cmd(u, 'K');
			return 0;
		}
		if (!strcmp(cmd, "pause 1")) {
			bt_cmd(u, 'L');
			return 0;
		}
		if (!strncmp(cmd, "seekcur", 7) || !strncmp(cmd, "random", 6))
			return 0;
		if (!strncmp(cmd, "play", 4) || !strncmp(cmd, "clear", 5) ||
		    !strcmp(cmd, "pause 0")) {
			bt_stop(u);
			u->input = X45_IN_NETWORK;   /* the hardware is on it already */
			u->dirty = 1;
		}
	}

	if (u->input == UI_IN_CD) {
		/* the CD input owns the transport: next/previous/stop are for the
		 * disc and never reach MPD. Anything that starts MPD playback (a
		 * library or radio choice) leaves the CD input for the network. */
		if (!strcmp(cmd, "next") || !strcmp(cmd, "previous")) {
			if (cd_active(u)) {
				int nt = u->cd_track;

				kill(u->cd_pid, cmd[0] == 'n' ? SIGUSR2 : SIGHUP);
				/* show the track the player is about to be on now; its status file follows within a moment
				 * (x45cd: after 3 s into a track, previous restarts the track; data tracks are passed over) */
				if (cmd[0] == 'n') {
					while (nt < u->cd_ntracks && (++nt, u->cd_len[nt - 1] < 0))
						;
				} else if (u->cd_pos <= 3) {
					while (nt > 1 && (--nt, u->cd_len[nt - 1] < 0))
						;
				}
				if (nt >= 1 && nt <= u->cd_ntracks && u->cd_len[nt - 1] >= 0 && (nt != u->cd_track || cmd[0] == 'p')) {
					u->cd_track = nt;
					u->cd_pos = 0;
					u->cd_tlen = u->cd_len[nt - 1];
					u->cd_skip_t = now_ms();
					u->dirty = 1;
				}
			}
			return 0;
		}
		if (!strcmp(cmd, "stop")) {
			cd_stop(u);
			return 0;
		}
		if (!strncmp(cmd, "seekcur", 7) || !strncmp(cmd, "random", 6) ||
		    !strcmp(cmd, "pause 1"))
			return 0;
		if (!strncmp(cmd, "play", 4) || !strncmp(cmd, "clear", 5) ||
		    !strcmp(cmd, "pause 0")) {
			cd_stop(u);
			u->input = X45_IN_NETWORK;   /* the hardware is on it already */
			u->dirty = 1;
		}
	}

	if (u->input != X45_IN_NETWORK && (!strncmp(cmd, "play", 4) || !strcmp(cmd, "pause 0")))
		go_network(u);

	if (!strncmp(cmd, "play", 4) && strcmp(u->st.state, "play"))
		dac_settle(u);   /* the output opens: silent until the stream is up */
	if (!strncmp(cmd, "play", 4) || !strcmp(cmd, "pause 0")) {
		preempt_airplay(u);
		/* MPD keeps reporting an old error (a USB hiccup, a busy DAC) until it
		 * is cleared, and the player screen would show it over a working
		 * stream. A fresh start of playback is the moment to drop it. */
		mpdc_run(&u->mpd, "clearerror", NULL, NULL, NULL, 0);
	}

	if (!strncmp(cmd, "seekcur", 7) && u->st.duration <= 0)
		return 0;   /* a live stream has no position: MPD would hold the panel for over a second finding out */
	if (!mpdc_is_open(&u->mpd)) {
		say(u, "MPD is offline");
		return -ENOTCONN;
	}
	{
		long t0 = now_ms();

		r = mpdc_run(&u->mpd, cmd, NULL, NULL, NULL, 0);
		t0 = now_ms() - t0;
		if (t0 > 300)   /* the panel waits for MPD here: say which command held it up */
			fprintf(stderr, "x45ui: slow mpd '%.40s' %ld ms\n", cmd, t0);
	}
	if (r == -EIO)
		say(u, "MPD: %s", u->mpd.last_error);
	else if (r < 0)
		say(u, "MPD: %s", strerror(-r));
	u->last_poll = 0;   /* refresh the display now, not in 500 ms */
	return r;
}

static void mpd_toggle(struct ui *u)
{
	if (tuner_input(u)) {
		enter_screen(u, SCR_TUNER);
		return;
	}
	if (u->input == UI_IN_BT) {
		if (u->bt_confirm[0])
			bt_cmd(u, 'y');
		else
			bt_cmd(u, u->bt_avrcp_play ? 'L' : 'k');
		return;
	}
	if (u->input == UI_IN_CD) {
		/* playing: pause/resume; stopped: start again from track 1 */
		if (cd_active(u))
			kill(u->cd_pid, SIGUSR1);
		else {
			if (u->cd_ntracks <= 0)
				cd_load(u);
			if (u->cd_ntracks > 0)
				cd_play(u, 1);
			else
				say(u, "CD: %s", u->cd_err[0] ? u->cd_err : "No disc");
		}
		return;
	}
	if (!strcmp(u->st.state, "play"))
		mpd_do(u, "pause 1");
	else if (!strcmp(u->st.state, "pause"))
		mpd_do(u, "pause 0");
	else if (u->st.length > 0)
		mpd_do(u, "play");
	else
		say(u, "Nothing queued: open LIBRARY");
}

/* The two sides, after the master level has been written (which sets both alike): the balance moves one down and
 * the other up, as the stock firmware does it (libx45 x45_balance_levels). Nothing is sent while the DAC is known
 * to hold equal levels and the balance is centre: the master write alone gives that, and the knob would otherwise
 * send two more commands per click for nothing.
 *
 * The DAC takes per-side levels only while its analogue stage is variable. With volume_mode fixed (the unit as a
 * source for an external volume control, `out` is then 100) that stage is switched over for as long as the balance
 * is off centre: the louder side stays at full level, the quieter one is turned down, and the digital stage stays
 * fixed so that the S/PDIF-type outputs are not touched. At centre it is put back. */
static void apply_balance(struct ui *u, int out)
{
	int l, r;

	if (u->x45fd < 0 || (!u->balance && !u->bal_live && !u->bal_var))
		return;
	x45_balance_levels(out, u->balance, &l, &r);
	if (u->vol_fixed && u->balance && !u->bal_var) {
		x45_set(u->x45fd, X45_SET_ANALOG_VOL_TYPE, 0, 0);
		u->bal_var = 1;
		u->vol_again_t = now_ms() + 400;   /* the driver ignores a level that arrives while it is still switching */
	}
	x45_volume_lr(u->x45fd, l, r, out);
	u->bal_live = l != r;
	if (u->bal_var && !u->balance) {
		x45_set(u->x45fd, X45_SET_ANALOG_VOL_TYPE, u->vol_fixed, 0);   /* the levels were just made equal */
		u->bal_var = 0;
	}
}

/* Sends the volume to MPD / the hardware. With fixed outputs that is always full level: u->volume keeps the
 * owner's variable level untouched and is what comes back when the mode is switched to variable. */
static void apply_volume(struct ui *u)
{
	char cmd[24];
	int out, sent = 0;

	if (u->volume < 0)
		u->volume = 0;
	if (u->volume > 100)
		u->volume = 100;
	out = VOL_LOCKED(u) ? 100 : u->volume;
	if (!VOL_LOCKED(u)) {
		u->last_vol_change = now_ms();
		u->vol_dirty = 1;
	}
	if (mpdc_is_open(&u->mpd)) {
		snprintf(cmd, sizeof cmd, "setvol %d", out);
		sent = mpdc_run(&u->mpd, cmd, NULL, NULL, NULL, 0) == 0;
	}
	if (!sent && u->x45fd >= 0)
		x45_volume(u->x45fd, out);
	apply_balance(u, out);
	u->dirty = 1;
}

int ui_volume_locked(const struct ui *u)
{
	return VOL_LOCKED(u);
}

/* The balance, -24..+24 (positive: the right side louder). Heard at once; `save` keeps it for the next start. */
void ui_set_balance(struct ui *u, int balance, int save)
{
	if (balance < -X45_BALANCE_MAX)
		balance = -X45_BALANCE_MAX;
	if (balance > X45_BALANCE_MAX)
		balance = X45_BALANCE_MAX;
	if (balance != u->balance) {
		u->balance = balance;
		apply_balance(u, VOL_LOCKED(u) ? 100 : u->volume);
		u->dirty = 1;
	}
	if (save) {
		char b[16];

		snprintf(b, sizeof b, "%d", u->balance);
		save_config_key("balance", b);
	}
}

/* mute on or off: the flag the screens show and the hardware, always together (setting only the flag once left
 * the web page's volume slider "unmuted" over a unit that stayed silent) */
static void set_mute(struct ui *u, int on)
{
	u->muted = on != 0;
	settle_hw(u);   /* x45_mute with the owner's flag, and the settle in progress */
	u->dirty = 1;
}

/* a change of the volume by knob, remote or web is an answer to "too quiet": it ends a mute */
static void volume_touch(struct ui *u)
{
	if (u->muted)
		set_mute(u, 0);
}

/* fixed <-> variable: the hardware stages are switched, then the level is sent; the driver ignores a level that
 * arrives while it is still switching, so it is sent again a moment later */
static void set_volume_mode(struct ui *u, int fixed)
{
	u->vol_fixed = fixed;
	u->bal_var = fixed && u->balance;   /* an off-centre balance keeps the analogue stage variable (apply_balance) */
	x45_set(u->x45fd, X45_SET_ANALOG_VOL_TYPE, u->bal_var ? 0 : u->vol_fixed, 0);
	x45_set(u->x45fd, X45_SET_DIGITAL_VOL_TYPE, u->vol_fixed, 0);
	u->bal_live = 1;
	apply_volume(u);
	u->vol_again_t = now_ms() + 400;
	save_config_key("volume_mode", u->vol_fixed ? "fixed" : "variable");
}

/* ---------------------------------------------------------------- library --- */

#define X45RB "/home/root/x45new/bin/x45rb"
#define X45UPNP "/home/root/x45new/bin/x45upnp"

/* sh single-quoting; -1 when it does not fit */
static int shq(char *out, size_t cap, const char *s)
{
	size_t n = 0;

	if (cap < 3)
		return -1;
	out[n++] = '\'';
	for (; *s; s++) {
		if (*s == '\'') {
			if (n + 6 >= cap)
				return -1;
			memcpy(out + n, "'\\''", 4);
			n += 4;
		} else {
			if (n + 3 >= cap)
				return -1;
			out[n++] = *s;
		}
	}
	out[n++] = '\'';
	out[n] = '\0';
	return 0;
}

/* The network folder: "@net" lists the UPnP/DLNA media servers x45upnp finds, "@u|<control url>|<id>"
 * lists one of their folders. Blocks for a moment (the search waits for answers), so the screen says so. */
static int net_list(struct ui *u, const char *want, struct mpdc_entry **out)
{
	char cmd[1000], ctrl[260] = "", id[260], qc[400], qi[400], line[1200];
	int discover = !strcmp(want, "@net"), n = 0, st, total = 0;
	struct mpdc_entry *rows;
	FILE *f;

	*out = NULL;
	u->net_more = 0;
	if (discover) {
		snprintf(cmd, sizeof cmd, "%s discover 2500 2>/dev/null", X45UPNP);
	} else {
		const char *bar = strchr(want + 3, '|');

		if (strncmp(want, "@u|", 3) || !bar || (size_t)(bar - want - 3) >= sizeof ctrl) {
			snprintf(u->dir_err, sizeof u->dir_err, "bad network address");
			return -EINVAL;
		}
		memcpy(ctrl, want + 3, (size_t)(bar - want - 3));
		ctrl[bar - want - 3] = '\0';
		snprintf(id, sizeof id, "%s", bar + 1);
		if (shq(qc, sizeof qc, ctrl) < 0 || shq(qi, sizeof qi, id) < 0) {
			snprintf(u->dir_err, sizeof u->dir_err, "bad network address");
			return -EINVAL;
		}
		snprintf(cmd, sizeof cmd, "%s browse %s %s 2>/tmp/x45upnp.err", X45UPNP, qc, qi);
	}
	rows = calloc(BROWSE_MAX, sizeof *rows);
	if (!rows)
		return -ENOMEM;
	if (discover && !u->net_force && u->net_cache_n > 0) {
		memcpy(rows, u->net_cache, (size_t)u->net_cache_n * sizeof *rows);
		*out = rows;
		return u->net_cache_n;
	}
	u->net_force = 0;
	snprintf(u->net_busy, sizeof u->net_busy, "%s", discover ? "searching for devices" : "loading");
	u->dirty = 1;
	render(u);
	f = popen(cmd, "r");
	if (!f) {
		u->net_busy[0] = '\0';
		free(rows);
		snprintf(u->dir_err, sizeof u->dir_err, "cannot start x45upnp");
		return -EIO;
	}
	{   /* the helper answers when it is done: show that something is going on meanwhile */
		struct pollfd pfd = { fileno(f), POLLIN, 0 };
		long t0 = now_ms();
		char base[32];

		snprintf(base, sizeof base, "%s", discover ? "searching for devices" : "loading");
		while (poll(&pfd, 1, 250) == 0) {
			long secs = (now_ms() - t0) / 1000;

			snprintf(u->net_busy, sizeof u->net_busy, "%s%.*s  %lds", base, (int)((now_ms() - t0) / 250 % 4), "...", secs);
			u->dirty = 1;
			render(u);
			if (now_ms() - t0 > 60000)
				break;
		}
	}
	while (n < BROWSE_MAX && fgets(line, sizeof line, f)) {
		char *nl = strpbrk(line, "\r\n"), *t1, *t2, *title;
		struct mpdc_entry *e = &rows[n];
		int w;

		if (nl)
			*nl = '\0';
		if (!discover && line[0] == 'T' && line[1] == '\t') {   /* how many entries the folder really holds */
			total = atoi(line + 2);
			continue;
		}
		if (discover) {
			t1 = strchr(line, '\t');
			if (!t1)
				continue;
			*t1 = '\0';
			w = snprintf(e->path, sizeof e->path, "@u|%s|0", t1 + 1);
			title = line;
			e->is_dir = 1;
		} else {
			if (line[0] != 'D' && line[0] != 'F')
				continue;
			t1 = strchr(line, '\t');
			t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
			if (!t2)
				continue;
			*t2 = '\0';
			title = t2 + 1;
			e->is_dir = line[0] == 'D';
			if (e->is_dir)
				w = snprintf(e->path, sizeof e->path, "@u|%s|%s", ctrl, t1 + 1);
			else
				w = snprintf(e->path, sizeof e->path, "@f|%s", t1 + 1);
		}
		if (w < 0 || (size_t)w >= sizeof e->path)
			continue;
		snprintf(e->name, sizeof e->name, "%s", title);
		n++;
	}
	while (!discover && fgets(line, sizeof line, f))   /* the count comes last, after the rows */
		if (line[0] == 'T' && line[1] == '\t')
			total = atoi(line + 2);
	st = pclose(f);
	u->net_busy[0] = '\0';
	u->net_more = total > n;
	if (n == 0 && (discover || st != 0)) {
		if (discover) {
			u->net_cache_n = 0;
			snprintf(u->dir_err, sizeof u->dir_err, "no media servers found on the network");
		} else {
			FILE *ef = fopen("/tmp/x45upnp.err", "r");

			snprintf(u->dir_err, sizeof u->dir_err, "the server did not answer");
			if (ef) {
				if (fgets(line, sizeof line, ef) && strlen(line) > 1) {
					line[strcspn(line, "\r\n")] = '\0';
					snprintf(u->dir_err, sizeof u->dir_err, "%.150s", line);
				}
				fclose(ef);
			}
		}
		free(rows);
		return -EIO;
	}
	if (discover && n > 0) {
		u->net_cache_n = n < 32 ? n : 32;
		memcpy(u->net_cache, rows, (size_t)u->net_cache_n * sizeof *rows);
	}
	*out = rows;
	return n;
}

/* "@fav": the favourites (the station file's own lines); "@rt": the stations copied from the router (the block
 * x45radio-sync manages). Both as playable rows. */
static int fav_scan(struct ui *u, struct mpdc_entry **out, int router, int cap)
{
	FILE *f = fopen(u->stations_path, "r");
	char line[1200];
	struct mpdc_entry *rows = calloc((size_t)cap, sizeof *rows);
	int k = 0, managed = 0;

	*out = NULL;
	if (!rows) {
		if (f)
			fclose(f);
		return -ENOMEM;
	}
	while (f && fgets(line, sizeof line, f) && k < cap) {
		char *bar, *name, *url;
		int w;

		if (strstr(line, "fritzbox begin")) {
			managed = 1;
			continue;
		}
		if (strstr(line, "fritzbox end")) {
			managed = 0;
			continue;
		}
		if (line[0] == '#' || managed != router)
			continue;
		bar = strchr(line, '|');
		if (!bar)
			continue;
		*bar = '\0';
		name = line + strspn(line, " \t");
		while (*name && name[strlen(name) - 1] == ' ')
			name[strlen(name) - 1] = '\0';
		url = bar + 1 + strspn(bar + 1, " \t");
		url[strcspn(url, "\r\n")] = '\0';
		while (*url && url[strlen(url) - 1] == ' ')
			url[strlen(url) - 1] = '\0';
		if (!name[0] || strncmp(url, "http", 4))
			continue;
		w = snprintf(rows[k].path, sizeof rows[k].path, "@s|%s", url);
		if (w < 0 || (size_t)w >= sizeof rows[k].path)
			continue;
		snprintf(rows[k].name, sizeof rows[k].name, "%s", name);
		k++;
	}
	if (f)
		fclose(f);
	if (router && k < cap) {   /* the last row asks the router again */
		rows[k].is_dir = 1;
		snprintf(rows[k].path, sizeof rows[k].path, "@syncrt");
		snprintf(rows[k].name, sizeof rows[k].name, "refresh");
		k++;
	}
	*out = rows;
	return k;
}

static int fav_list(struct ui *u, struct mpdc_entry **out, int router)
{
	return fav_scan(u, out, router, BROWSE_MAX);
}

/* The community radio directory: "@r|<command>[|<argument>]" runs x45rb, whose lines become rows. */
static int rb_list(struct ui *u, const char *want, struct mpdc_entry **out)
{
	char cmd[700], arg1[160] = "", arg2[160] = "", qa[400], line[1200];
	const char *p = want + 3, *bar;
	struct mpdc_entry *rows;
	FILE *f;
	int n = 0;

	*out = NULL;
	bar = strchr(p, '|');
	if (bar) {
		snprintf(arg1, sizeof arg1, "%.*s", (int)(bar - p), p);
		snprintf(arg2, sizeof arg2, "%s", bar + 1);
	} else {
		snprintf(arg1, sizeof arg1, "%s", p);
	}
	if (shq(qa, sizeof qa, arg2) < 0)
		return -EINVAL;
	{
		char qc[200];

		if (shq(qc, sizeof qc, arg1) < 0)
			return -EINVAL;
		snprintf(cmd, sizeof cmd, X45RB " %s %s 2>/dev/null", qc, arg2[0] ? qa : "");
	}
	rows = calloc(BROWSE_MAX, sizeof *rows);
	if (!rows)
		return -ENOMEM;
	snprintf(u->net_busy, sizeof u->net_busy, "loading");
	u->dirty = 1;
	render(u);
	f = popen(cmd, "r");
	if (!f) {
		u->net_busy[0] = '\0';
		free(rows);
		snprintf(u->dir_err, sizeof u->dir_err, "cannot start x45rb");
		return -EIO;
	}
	while (n < BROWSE_MAX && fgets(line, sizeof line, f)) {
		char *t1, *t2;
		struct mpdc_entry *e = &rows[n];
		int w;

		line[strcspn(line, "\r\n")] = '\0';
		if ((line[0] != 'D' && line[0] != 'S') || line[1] != '\t')
			continue;
		t1 = line + 2;
		t2 = strchr(t1, '\t');
		if (!t2)
			continue;
		*t2++ = '\0';
		if (line[0] == 'D') {
			e->is_dir = 1;
			w = snprintf(e->path, sizeof e->path, "%s", t1);
		} else {
			e->is_dir = 0;
			w = snprintf(e->path, sizeof e->path, "@s|%s", t1);
		}
		if (w < 0 || (size_t)w >= sizeof e->path)
			continue;
		snprintf(e->name, sizeof e->name, "%s", t2);
		n++;
	}
	pclose(f);
	u->net_busy[0] = '\0';
	if (n == 0) {
		snprintf(u->dir_err, sizeof u->dir_err, "the radio directory did not answer");
		free(rows);
		return -EIO;
	}
	*out = rows;
	return n;
}

#define LIB_ROOT "/home/root/x45new/media"   /* one link per attached disk (x45media) */

/* 1 when one element of the path is exactly "..": a track called "Wait... for me.mp3" or "Mr..Brightside" is not a
 * way out of the library */
static int path_has_dotdot(const char *p)
{
	while (*p) {
		const char *e = p;

		while (*e && *e != '/')
			e++;
		if (e - p == 2 && p[0] == '.' && p[1] == '.')
			return 1;
		p = *e ? e + 1 : e;
	}
	return 0;
}

/* MPD lists only folders that hold music, so a disk without any (a new one, one that only holds recordings to come)
 * would not show in the library at all. The disks are all in LIB_ROOT: */
static int lib_is_dir(const char *rel)
{
	char full[MPDC_PATH_MAX + 64];
	struct stat st;

	if (!rel[0] || path_has_dotdot(rel))
		return 0;
	snprintf(full, sizeof full, "%s/%s", LIB_ROOT, rel);
	return stat(full, &st) == 0 && S_ISDIR(st.st_mode);
}

/* add to the root listing the disks MPD left out, each in its place among the folders */
static struct mpdc_entry *lib_add_disks(struct mpdc_entry *rows, int *n)
{
	DIR *d = opendir(LIB_ROOT);
	struct dirent *e;

	if (!d)
		return rows;
	while ((e = readdir(d))) {
		struct mpdc_entry *nr;
		int i, at;

		if (e->d_name[0] == '.' || strlen(e->d_name) >= sizeof rows[0].name || !lib_is_dir(e->d_name))
			continue;
		for (i = 0; i < *n; i++)
			if (rows[i].is_dir && !strcmp(rows[i].path, e->d_name))
				break;
		if (i < *n)
			continue;
		nr = realloc(rows, (size_t)(*n + 1) * sizeof *rows);
		if (!nr)
			break;
		rows = nr;
		for (at = 0; at < *n && rows[at].is_dir && strcasecmp(rows[at].name, e->d_name) < 0; at++)
			;
		memmove(rows + at + 1, rows + at, (size_t)(*n - at) * sizeof *rows);
		memset(&rows[at], 0, sizeof rows[at]);
		rows[at].is_dir = 1;
		snprintf(rows[at].path, sizeof rows[at].path, "%s", e->d_name);
		snprintf(rows[at].name, sizeof rows[at].name, "%s", e->d_name);
		(*n)++;
	}
	closedir(d);
	return rows;
}

/* Load `path` into the browser, optionally leaving the cursor on `select_path`.
 * Both arguments are copied first: callers legitimately pass u->dir itself or a
 * pointer into u->rows, and both are about to be overwritten. On failure the
 * listing is emptied and the reason shown; u->dir keeps its old value so
 * RETURN still climbs out of wherever we were. */
static void browse_load(struct ui *u, const char *path, const char *select_path)
{
	char want[MPDC_PATH_MAX], pick[MPDC_PATH_MAX];
	struct mpdc_entry *rows = NULL;
	int n = 0, trunc = 0, i;

	snprintf(want, sizeof want, "%s", path);
	snprintf(pick, sizeof pick, "%s", select_path ? select_path : "");

	u->dir_err[0] = '\0';
	if (want[0] == '@') {
		n = !strcmp(want, "@fav") ? fav_list(u, &rows, 0) : !strcmp(want, "@rt") ? fav_list(u, &rows, 1) :
		    !strncmp(want, "@r|", 3) ? rb_list(u, want, &rows) : net_list(u, want, &rows);   /* each explains itself in dir_err when it fails */
		if (!strncmp(want, "@u|", 3))
			trunc = u->net_more;   /* "First 256 shown" */
		if (n > 0 && !strcmp(want, "@net")) {
			struct mpdc_entry *nr = realloc(rows, (size_t)(n + 1) * sizeof *rows);

			if (nr) {
				rows = nr;
				memset(&rows[n], 0, sizeof rows[n]);
				rows[n].is_dir = 1;
				snprintf(rows[n].path, sizeof rows[n].path, "@refresh");
				snprintf(rows[n].name, sizeof rows[n].name, "refresh");
				n++;
			}
		}
		if (n >= 0 && !u->dir_err[0] && (!strcmp(want, "@fav") || !strcmp(want, "@rt") || !strncmp(want, "@r|", 3))) {
			/* the last row of a station list finds a station by its name (scroll up once from the first) */
			struct mpdc_entry *nr = realloc(rows, (size_t)(n + 1) * sizeof *rows);

			if (nr) {
				rows = nr;
				memset(&rows[n], 0, sizeof rows[n]);
				rows[n].is_dir = 1;
				snprintf(rows[n].path, sizeof rows[n].path, "@search");
				snprintf(rows[n].name, sizeof rows[n].name, "Search stations");
				n++;
			}
		}
	} else if (!mpdc_is_open(&u->mpd)) {
		snprintf(u->dir_err, sizeof u->dir_err, "mpd is offline");
	} else {
		n = mpdc_lsinfo(&u->mpd, want, &rows, BROWSE_MAX, &trunc);
		while (n == -EIO && want[0] && strstr(u->mpd.last_error, "No such directory")) {
			char *slash;

			free(rows);
			rows = NULL;
			if (lib_is_dir(want)) {
				n = 0;   /* a folder on a disk, but MPD knows nothing in it: it is empty as far as the library goes */
				break;
			}
			/* the folder is gone (deleted here or from the web page, and the empty parents with it): the nearest
			 * one that is left is shown instead, so RETURN and the list still work */
			slash = strrchr(want, '/');
			if (slash)
				*slash = '\0';
			else
				want[0] = '\0';
			pick[0] = '\0';
			n = mpdc_lsinfo(&u->mpd, want, &rows, BROWSE_MAX, &trunc);
		}
		if (n < 0) {
			snprintf(u->dir_err, sizeof u->dir_err, "%s",
				 n == -EIO ? u->mpd.last_error : strerror(-n));
		} else if (!want[0]) {          /* the music root also leads to the network's servers */
			struct mpdc_entry *nr;

			rows = lib_add_disks(rows, &n);
			nr = realloc(rows, (size_t)(n + 2) * sizeof *rows);

			if (nr) {
				rows = nr;
				memmove(rows + 2, rows, (size_t)n * sizeof *rows);
				memset(&rows[0], 0, 2 * sizeof rows[0]);
				rows[0].is_dir = 1;
				snprintf(rows[0].path, sizeof rows[0].path, "@r|root");
				snprintf(rows[0].name, sizeof rows[0].name, "Radio");
				rows[1].is_dir = 1;
				snprintf(rows[1].path, sizeof rows[1].path, "@net");
				snprintf(rows[1].name, sizeof rows[1].name, "Network");
				n += 2;
			}
		}
	}

	if (n < 0 && !u->dir_err[0])   /* a list that failed without a reason of its own (no memory, an address it refused) */
		snprintf(u->dir_err, sizeof u->dir_err, "%s", strerror(-n));
	if (n < 0 || u->dir_err[0]) {
		free(rows);   /* a failed list keeps no rows (and the old listing's go below) */
		rows = NULL;
		n = 0;
	}
	free(u->rows);
	u->rows = NULL;
	u->nrows = u->truncated = 0;
	u->bsel = u->top = 0;
	if (u->dir_err[0]) {
		u->dirty = 1;
		return;
	}

	u->rows = rows;
	u->nrows = n;
	u->truncated = trunc;
	snprintf(u->dir, sizeof u->dir, "%s", want);
	if (pick[0])
		for (i = 0; i < n; i++)
			if (!strcmp(rows[i].path, pick)) {
				u->bsel = i;
				u->top = i > 4 ? i - 4 : 0;
				break;
			}
	u->dirty = 1;
}

/* The library changed under the list (an update finished, or files came in through the file manager): load the
 * folder again, with the cursor on the row it was on. Folders of the network and the radio list are not MPD's. */
static void lib_refresh(struct ui *u)
{
	char keep[MPDC_PATH_MAX] = "", dir[MPDC_PATH_MAX];

	if (u->dir[0] == '@' || !mpdc_is_open(&u->mpd))
		return;
	if (u->bsel >= 0 && u->bsel < u->nrows)
		snprintf(keep, sizeof keep, "%s", u->rows[u->bsel].path);
	snprintf(dir, sizeof dir, "%s", u->dir);
	browse_load(u, dir, keep);
}

/* a rescan was asked for: look again when MPD says it is done, and once more shortly after in case the update
 * was over before it was seen */
static void lib_rescan(struct ui *u)
{
	u->lib_reload_t = now_ms() + 2500;
}

static void lib_poll(struct ui *u)
{
	if (u->lib_reload_t && ms_due(now_ms(), u->lib_reload_t)) {
		u->lib_reload_t = 0;
		lib_refresh(u);
	}
}

static void browse_up(struct ui *u)
{
	char parent[MPDC_PATH_MAX], came_from[MPDC_PATH_MAX];
	char *slash;

	if (!u->dir[0]) {
		u->screen = SCR_PLAYER;
		u->dirty = 1;
		return;
	}
	snprintf(came_from, sizeof came_from, "%s", u->dir);
	if (u->dir[0] == '@') {              /* network folders: back to where each was entered from */
		if (!strcmp(u->dir, "@net")) {
			u->net_depth = 0;
			browse_load(u, "", came_from);
		} else if (u->net_depth > 0) {
			u->net_depth--;
			snprintf(u->net_title, sizeof u->net_title, "%s", u->net_names[u->net_depth]);
			snprintf(parent, sizeof parent, "%s", u->net_stack[u->net_depth]);
			browse_load(u, parent, came_from);
		} else {
			browse_load(u, "@net", came_from);
		}
		return;
	}
	snprintf(parent, sizeof parent, "%s", u->dir);
	slash = strrchr(parent, '/');
	if (slash)
		*slash = '\0';
	else
		parent[0] = '\0';
	browse_load(u, parent, came_from);
}

/* Queue the whole folder and play from the chosen file: choosing track 5 of an
 * album should give you tracks 5..N, not a queue of one. */
static void net_id_cb(const char *key, const char *val, void *ud)
{
	if (!strcmp(key, "Id"))
		*(int *)ud = atoi(val);
}

static void radio_play(struct ui *u, const char *url, const char *name);

static void browse_play(struct ui *u)
{
	char q[2 * MPDC_PATH_MAX + 8], cmd[2 * MPDC_PATH_MAX + 24];

	if (u->bsel >= 0 && u->bsel < u->nrows && !strncmp(u->rows[u->bsel].path, "@s|", 3)) {
		/* a station of the radio directory: just that stream, named without the "[codec]" tail */
		char name[96], *tail;

		snprintf(name, sizeof name, "%s", u->rows[u->bsel].name);
		tail = strstr(name, "  [");
		if (tail)
			*tail = '\0';
		/* remember this folder's stations: previous / next and the knob stay inside it */
		if (!u->ctx)
			u->ctx = malloc(256 * sizeof *u->ctx);
		u->ctx_n = 0;
		if (u->ctx) {
			int i;

			for (i = 0; i < u->nrows && u->ctx_n < 256; i++) {
				char *t;

				if (strncmp(u->rows[i].path, "@s|", 3))
					continue;
				snprintf(u->ctx[u->ctx_n].url, sizeof u->ctx[0].url, "%s", u->rows[i].path + 3);
				snprintf(u->ctx[u->ctx_n].name, sizeof u->ctx[0].name, "%s", u->rows[i].name);
				t = strstr(u->ctx[u->ctx_n].name, "  [");
				if (t)
					*t = '\0';
				u->ctx_n++;
			}
		}
		radio_play(u, u->rows[u->bsel].path + 3, name);
		return;
	}
	int i, pos = 0, added = 0, target = -1, net = u->rows[u->bsel].path[0] == '@';

	if (!mpdc_is_open(&u->mpd)) {
		say(u, "MPD is offline");
		return;
	}
	if (mpd_do(u, "clear") < 0)
		return;
	for (i = 0; i < u->nrows; i++) {
		if (u->rows[i].is_dir)
			continue;
		if (i == u->bsel)
			target = pos;
		if (net) {                       /* a stream on the network: queue it with a readable title */
			int id = -1;

			if (strncmp(u->rows[i].path, "@f|", 3) || mpdc_quote(q, sizeof q, u->rows[i].path + 3) < 0) {
				if (i == u->bsel)
					target = -1;
				continue;
			}
			snprintf(cmd, sizeof cmd, "addid %s", q);
			if (mpdc_run(&u->mpd, cmd, net_id_cb, &id, NULL, 0) != 0) {
				if (i == u->bsel)
					target = -1;
				continue;
			}
			if (id >= 0 && mpdc_quote(q, sizeof q, u->rows[i].name) > 0) {
				snprintf(cmd, sizeof cmd, "addtagid %d title %s", id, q);
				mpdc_run(&u->mpd, cmd, NULL, NULL, NULL, 0);
			}
			pos++;
			added++;
			continue;
		}
		if (mpdc_quote(q, sizeof q, u->rows[i].path) < 0) {
			if (i == u->bsel)
				target = -1;
			continue;
		}
		snprintf(cmd, sizeof cmd, "add %s", q);
		if (mpdc_run(&u->mpd, cmd, NULL, NULL, NULL, 0) != 0) {
			if (i == u->bsel)
				target = -1;   /* the chosen file did not go in: the next one is not what was asked for */
			continue;
		}
		pos++;
		added++;
	}
	if (target < 0 || !added) {
		say(u, "could not queue that file");
		return;
	}
	snprintf(cmd, sizeof cmd, "play %d", target);
	if (mpd_do(u, cmd) == 0) {
		u->screen = SCR_PLAYER;
		say(u, "queued %d track%s", added, added == 1 ? "" : "s");
	}
}

static void browse_enter(struct ui *u)
{
	if (u->bsel < 0 || u->bsel >= u->nrows)
		return;
	if (u->rows[u->bsel].is_dir) {
		char path[MPDC_PATH_MAX], name[96], prev[MPDC_PATH_MAX];

		snprintf(path, sizeof path, "%s", u->rows[u->bsel].path);
		snprintf(name, sizeof name, "%s", u->rows[u->bsel].name);
		snprintf(prev, sizeof prev, "%s", u->dir);
		if (!strcmp(path, "@syncrt")) {   /* copy the router's radio list again */
			snprintf(u->net_busy, sizeof u->net_busy, "asking the router");
			u->dirty = 1;
			render(u);
			if (system("/home/root/x45new/bin/x45radio-sync >/dev/null 2>&1") != 0)
				say(u, "the router did not answer");
			u->net_busy[0] = '\0';
			browse_load(u, "@rt", NULL);
			return;
		}
		if (!strcmp(path, "@search")) {
			ui_ext_open_name(u, "search");
			return;
		}
		if (!strcmp(path, "@refresh")) {   /* look for the servers again, and stay on that list */
			u->net_force = 1;
			browse_load(u, "@net", NULL);
			return;
		}
		browse_load(u, path, NULL);
		if (path[0] == '@' && !strcmp(u->dir, path)) {
			if (!strcmp(path, "@net")) {
				u->net_depth = 0;
			} else if (u->net_depth < 16) {
				snprintf(u->net_stack[u->net_depth], sizeof u->net_stack[0], "%s", prev);
				snprintf(u->net_names[u->net_depth], sizeof u->net_names[0], "%s", u->net_title);
				u->net_depth++;
			}
			snprintf(u->net_title, sizeof u->net_title, "%.63s", name);
		}
	} else {
		browse_play(u);
	}
}

/* Replace the queue with one stream and play it. */
static void radio_play(struct ui *u, const char *url, const char *name)
{
	char q[600], cmd[640];

	if (mpdc_quote(q, sizeof q, url) < 0) {
		say(u, "cannot use that station URL");
		return;
	}
	if (mpd_do(u, "clear") < 0)
		return;
	snprintf(cmd, sizeof cmd, "add %s", q);
	if (mpd_do(u, cmd) < 0)
		return;
	if (mpd_do(u, "play") == 0) {
		save_config_key("last_radio", name);
		save_config_key("last_radio_url", url);
		snprintf(u->last_url, sizeof u->last_url, "%s", url);
		snprintf(u->last_name, sizeof u->last_name, "%s", name);
		u->screen = SCR_PLAYER;
		u->status[0] = '\0';
		/* MPD reports the same queue slot and state for the new stream, so the
		 * poll would never notice the change: record the new song ourselves and
		 * make the next poll re-read it */
		memset(&u->song, 0, sizeof u->song);
		snprintf(u->song.file, sizeof u->song.file, "%s", url);
		snprintf(u->song.name, sizeof u->song.name, "%s", name);
		u->have_song = 1;
		snprintf(u->play_url, sizeof u->play_url, "%s", url);
		snprintf(u->play_name, sizeof u->play_name, "%s", name);
		u->seen_pos = -2;
		u->last_song = 0;
		u->p_refresh = 1;
		u->song_hold = now_ms() + 1500;   /* what the poller has now still describes the old station */
		u->dirty = 1;
	}
}

static void radio_tune(struct ui *u)
{
	const struct x45_station *st;

	if (u->rsel < 0 || u->rsel >= u->nstations)
		return;
	st = &u->stations[u->rsel];
	radio_play(u, st->url, st->name);
}

/* ------------------------------------------------------- the station search --- */

int stations_searchable(const struct ui *u)
{
	if (u->input == X45_IN_NETWORK)
		return 1;   /* the favourites, the router's list and the community directory */
	if (u->input == UI_IN_DAB)
		return u->tu_nsvc > 0 && !tuner_scanning(u);
	if (u->input == X45_IN_FMRADIO)
		return u->tu_npreset > 0 && !tuner_scanning(u);
	return 0;
}

static int stn_from_rows(struct mpdc_entry *rows, int n, int kind, struct ui_stn *out, int k, int max)
{
	int i;

	for (i = 0; i < n && k < max; i++) {
		if (rows[i].is_dir || strncmp(rows[i].path, "@s|", 3))
			continue;
		memset(&out[k], 0, sizeof out[k]);
		snprintf(out[k].name, sizeof out[k].name, "%s", rows[i].name);
		snprintf(out[k].url, sizeof out[k].url, "%s", rows[i].path + 3);
		out[k].kind = kind;
		if (kind == STN_LIST) {   /* the directory's rows read "Name  [MP3 128 DE]": the bracket is the info, not the name */
			char *br = strstr(out[k].name, "  [");

			if (br) {
				char *close_br;

				snprintf(out[k].info, sizeof out[k].info, "%s", br + 3);
				close_br = strchr(out[k].info, ']');
				if (close_br)
					*close_br = '\0';
				*br = '\0';
			}
		}
		k++;
	}
	return k;
}

int stations_collect(struct ui *u, struct ui_stn *out, int max)
{
	int k = 0, i;

	if (u->input == X45_IN_NETWORK) {
		struct mpdc_entry *rows;
		int n;

		/* opened from a radio list of the directory (top voted, a country, a genre ...): its stations come first */
		if (u->ext_ret == SCR_BROWSE && !strncmp(u->dir, "@r|", 3) && u->rows && u->nrows > 0)
			k = stn_from_rows(u->rows, u->nrows, STN_LIST, out, k, max);
		n = fav_scan(u, &rows, 0, max);
		if (n > 0)
			k = stn_from_rows(rows, n, STN_FAV, out, k, max);
		free(rows);
		n = fav_scan(u, &rows, 1, max);
		if (n > 0)
			k = stn_from_rows(rows, n, STN_ROUTER, out, k, max);
		free(rows);
		return k;
	}
	if (u->input == UI_IN_DAB || u->input == X45_IN_FMRADIO) {
		tuner_load_list(u);
		for (i = 0; i < u->tl_n && k < max; i++) {
			memset(&out[k], 0, sizeof out[k]);
			snprintf(out[k].name, sizeof out[k].name, "%s", u->tl_name[i]);
			out[k].kind = u->input == UI_IN_DAB ? STN_DAB : STN_FM;
			out[k].val = u->tl_val[i];
			k++;
		}
	}
	return k;
}

/* plays / tunes a station of the search and shows the player (the search screen closes with it) */
void station_play(struct ui *u, const struct ui_stn *s)
{
	struct ui_stn st = *s;   /* the screen's own copy goes away with it */

	if (u->screen == SCR_EXT)
		ui_ext_close(u);
	if (st.kind == STN_DAB && u->input == UI_IN_DAB) {
		tuner_cmd(u, "select %d", st.val);
		enter_screen(u, SCR_PLAYER);
	} else if (st.kind == STN_FM && u->input == X45_IN_FMRADIO) {
		tuner_cmd(u, "tune %d", st.val);
		enter_screen(u, SCR_PLAYER);
	} else if (st.url[0] && u->input == X45_IN_NETWORK) {
		radio_play(u, st.url, st.name);
	}
}

/* at start-up, on the network input: put the last internet radio station back on */
static void resume_poll(struct ui *u)
{
	long now = now_ms();
	int i;

	if (!u->resume_name[0] || !ms_due(now, u->resume_t))
		return;
	if (!mpdc_is_open(&u->mpd)) {
		if (ms_since(now, u->resume_t) > 20000)
			u->resume_name[0] = '\0';
		return;
	}
	if (!u->resume || u->input != X45_IN_NETWORK || !strcmp(u->st.state, "play")) {
		u->resume_name[0] = '\0';
		return;
	}
	if (u->resume_url[0]) {
		radio_play(u, u->resume_url, u->resume_name);
		u->resume_name[0] = '\0';
		return;
	}
	radio_load(u);
	for (i = 0; i < u->nstations; i++) {
		if (!strcmp(u->stations[i].name, u->resume_name)) {
			u->rsel = i;
			radio_tune(u);
			break;
		}
	}
	u->resume_name[0] = '\0';
}

/* The network input is left with music on: remember what it was, so that choosing the input again puts it back. */
static void net_remember(struct ui *u)
{
	int playing = !strcmp(u->st.state, "play"), paused = !strcmp(u->st.state, "pause");

	memset(&u->net, 0, sizeof u->net);
	u->net.valid = 1;   /* a stop on purpose is remembered too: it must not bring a station back */
	if (!(playing || paused) || !u->have_song || !u->song.file[0])
		return;
	u->net.was = playing ? 1 : 2;
	u->net.pos = u->st.song;
	u->net.dur = (int)u->st.duration;
	u->net.elapsed = (int)u->st.elapsed;
	snprintf(u->net.file, sizeof u->net.file, "%s", u->song.file);
	snprintf(u->net.name, sizeof u->net.name, "%s", u->song.name[0] ? u->song.name : u->play_name);
}

static void net_file_cb(const char *key, const char *val, void *ud)
{
	if (!strcmp(key, "file"))
		snprintf(ud, MPDC_PATH_MAX, "%s", val);
}

/* The network input was chosen again: a moment later (the audio path settles first) its music goes back on,
 * unless something is playing already (a controller started it) or the owner had stopped it. */
static void net_poll(struct ui *u)
{
	long now = now_ms();
	char cmd[48], got[MPDC_PATH_MAX];
	int live;

	if (u->net_end) {
		/* the song is up again: jump to where it was and, if it had been paused, pause it again */
		if (ms_since(now, u->net_end) > 0 || u->input != X45_IN_NETWORK) {
			u->net_end = 0;
		} else if (!strcmp(u->st.state, "play") && u->have_song && !strcmp(u->song.file, u->net.file)) {
			if (u->net_seek > 0 && u->st.duration > u->net_seek + 2) {
				snprintf(cmd, sizeof cmd, "seekcur %d", u->net_seek);
				mpd_do(u, cmd);
			}
			if (u->net_pause)
				mpd_do(u, "pause 1");
			u->net_seek = u->net_pause = 0;
			u->net_end = 0;
		}
	}
	if (!u->net_t || !ms_due(now, u->net_t))
		return;
	u->net_t = 0;
	if (u->input != X45_IN_NETWORK || !mpdc_is_open(&u->mpd) || !strcmp(u->st.state, "play"))
		return;
	if (!u->net.valid) {
		/* the input was never left since start-up: the last station is the best guess */
		if (u->last_url[0])
			radio_play(u, u->last_url, u->last_name[0] ? u->last_name : "Stream");
		return;
	}
	if (!u->net.was)
		return;
	live = !strncmp(u->net.file, "http", 4) && u->net.dur <= 0;
	if (live) {
		if (u->net.was == 1)   /* a paused stream has nothing to come back to */
			radio_play(u, u->net.file, u->net.name[0] ? u->net.name : "Stream");
		return;
	}
	/* a stop keeps the queue: play the same entry, if it is still the one it was */
	got[0] = '\0';
	snprintf(cmd, sizeof cmd, "playlistinfo %d", u->net.pos);
	if (mpdc_run(&u->mpd, cmd, net_file_cb, got, NULL, 0) != 0 || strcmp(got, u->net.file))
		return;
	snprintf(cmd, sizeof cmd, "play %d", u->net.pos);
	if (mpd_do(u, cmd) == 0) {
		u->net_seek = u->net.elapsed;
		u->net_pause = u->net.was == 2;
		u->net_end = now + 8000;
	}
}

/* Previous/next. An internet radio stream is a one-entry queue, so "next" would simply run off
 * the end and stop it: step through the station list instead. */
static void skip_track(struct ui *u, int dir)
{
	static long last;
	long now = now_ms();

	if (u->input == X45_IN_NETWORK && u->have_song && !strncmp(u->song.file, "http", 4) && u->ctx && u->ctx_n > 1) {
		int i, cur = -1;

		if (now - last < 250)
			return;
		for (i = 0; i < u->ctx_n; i++)
			if (!strcmp(u->ctx[i].url, u->song.file))
				cur = i;
		if (cur >= 0) {   /* the next station of the folder it was chosen from */
			last = now;
			i = (cur + (dir > 0 ? 1 : u->ctx_n - 1)) % u->ctx_n;
			radio_play(u, u->ctx[i].url, u->ctx[i].name);
			return;
		}
		u->ctx_n = 0;   /* something else plays now: that folder no longer applies */
	}
	if (u->input == X45_IN_NETWORK && u->have_song && !strncmp(u->song.file, "http", 4) &&
	    u->st.length <= 1) {
		int i, cur = -1;

		if (now - last < 250)
			return;
		last = now;
		radio_load(u);
		if (u->nstations <= 0)
			return;
		for (i = 0; i < u->nstations; i++)
			if (!strcmp(u->stations[i].url, u->song.file))
				cur = i;
		if (cur < 0)
			cur = u->rsel;
		u->rsel = (cur + (dir > 0 ? 1 : u->nstations - 1)) % u->nstations;
		radio_tune(u);
		return;
	}
	mpd_do(u, dir > 0 ? "next" : "previous");
}

/* ------------------------------------------------------------------ logic --- */

/* etc/x45ui.conf: `key = value` lines, # comments. They are here, not compiled
 * in, because the right values can only be confirmed on the real unit:
 *   knob_invert  1 (default) or 0   flip the turn direction
 *   knob_edges   1..16 (default 4)  quadrature edges per detent
 *   volume_mode  variable (default) | fixed   line-out level stage; fixed = the
 *                                   unit's volume is bypassed (use with an
 *                                   external preamp)
 *   input_trim   0..21 (default 12) analogue input gain, -12..+9 dB, 12 = 0 dB
 *   tuner        on (default) | off   hide FM/DAB when no module is fitted
 *   mpd_password text               sent to MPD after connecting */
#define CONF_PATH "/home/root/x45new/etc/x45ui.conf"

/* Rewrites `key = value` in the config (adds the line if missing), keeping every
 * other line, so the options screen survives a reboot. Best effort: on any error
 * the change simply lasts until the next start. */
void save_config_key(const char *key, const char *val)
{
	static char buf[16384];
	char line[1100], tmp[] = CONF_PATH ".tmp";
	size_t n = 0, kl = strlen(key);
	int done = 0, w;
	FILE *f = fopen(CONF_PATH, "r");

	/* a value that does not fit a line would lose its newline and run into the next setting */
	if (strlen(key) + strlen(val) + 4 > sizeof line)
		return;
	if (f) {
		while (fgets(line, sizeof line, f)) {
			const char *k = line;
			int hit;

			while (*k == ' ' || *k == '\t')
				k++;
			hit = !strncmp(k, key, kl) &&
			      (k[kl] == ' ' || k[kl] == '\t' || k[kl] == '=');
			if (hit) {
				if (done)
					continue;
				snprintf(line, sizeof line, "%s = %s\n", key, val);
				done = 1;
			}
			if (n + strlen(line) >= sizeof buf) {   /* never write half of a file */
				fclose(f);
				return;
			}
			memcpy(buf + n, line, strlen(line));
			n += strlen(line);
		}
		fclose(f);
	}
	if (!done) {
		w = snprintf(line, sizeof line, "%s = %s\n", key, val);

		if (n + (size_t)w >= sizeof buf)
			return;
		memcpy(buf + n, line, (size_t)w);
		n += (size_t)w;
	}
	f = fopen(tmp, "w");
	if (!f)
		return;
	if (fwrite(buf, 1, n, f) == n && fclose(f) == 0)
		rename(tmp, CONF_PATH);
	else
		unlink(tmp);
}

static void load_config(struct ui *u, const char *path)
{
	char line[1100];
	FILE *f = fopen(path, "r");
	int have_style = 0, old_style = -1;   /* spec_style was in the file; the style that an older visual setting implies */

	if (!f)
		return;
	while (fgets(line, sizeof line, f)) {
		char *k = line, *v, *e;

		while (*k == ' ' || *k == '\t')
			k++;
		if (*k == '#' || *k == '\n' || !*k)
			continue;
		v = strchr(k, '=');
		if (!v)
			continue;
		*v++ = '\0';
		for (e = k + strlen(k); e > k && (e[-1] == ' ' || e[-1] == '\t'); e--)
			e[-1] = '\0';
		while (*v == ' ' || *v == '\t')
			v++;
		{   /* the value ends at the end of the line: "pcusb\n" is not the input called "pcusb" */
			size_t vl = strcspn(v, "\r\n");

			v[vl] = '\0';
			while (vl > 0 && (v[vl - 1] == ' ' || v[vl - 1] == '\t'))
				v[--vl] = '\0';
		}
		if (!strcmp(k, "knob_invert")) {
			u->in.invert = atoi(v) != 0;
		} else if (!strcmp(k, "knob_edges")) {
			int n = atoi(v);

			if (n >= 1 && n <= 16)
				u->in.ticks_per_detent = n;
		} else if (!strcmp(k, "volume_mode")) {
			u->vol_fixed = !strncmp(v, "fixed", 5);
		} else if (!strcmp(k, "input_trim")) {   /* old single setting: applies to all analog inputs */
			int n = atoi(v), j;

			if (n >= 0 && n <= 21)
				for (j = X45_IN_LINEIN; j <= X45_IN_PHONO; j++)
					u->trim[j] = n;
		} else if (!strcmp(k, "vis_scope")) {
			u->vis_all = !strncmp(v, "all", 3);
		} else if (!strcmp(k, "vis_all_set")) {
			int st = vis_unpack(&u->vis_shared, strtoul(v, NULL, 10));

			if (old_style < 0)
				old_style = st;
		} else if (!strcmp(k, "bands")) {
			int n = atoi(v);

			if (n >= 8 && n <= 128)
				u->bands = n;
		} else if (!strcmp(k, "input_icons")) {
			u->in_icons = atoi(v) != 0;
		} else if (!strcmp(k, "font_smooth")) {
			x45_font_smooth = atoi(v) < 0 ? 0 : atoi(v) > 2 ? 2 : atoi(v);
		} else if (!strcmp(k, "spec_color")) {
			u->spec_col = atoi(v) < 0 || atoi(v) >= vis_bar_colours() ? 0 : atoi(v);
		} else if (!strcmp(k, "spec_style")) {
			u->spec_style = atoi(v) < 0 || atoi(v) >= vis_spec_styles() ? 0 : atoi(v);
			have_style = 1;
		} else if (!strcmp(k, "gon_look")) {
			u->gon_look = atoi(v) < 0 || atoi(v) >= vis_gonio_looks() ? 0 : atoi(v);
		} else if (!strcmp(k, "gon_color")) {
			u->gon_col = atoi(v) < 0 || atoi(v) >= vis_gonio_colours() ? 0 : atoi(v);
		} else if (!strcmp(k, "gon_nobars")) {
			u->gon_nobars = atoi(v) != 0;
		} else if (!strcmp(k, "gon_big")) {
			u->gon_big = atoi(v) != 0;
		} else if (!strcmp(k, "skin")) {
			int n = skin_find(v);

			if (n >= 0)
				u->skin_id = n;
		} else if (!strcmp(k, "theme")) {
			int n = atoi(v);

			if (n >= 0 && n < colour_count(COL_ACCENT))
				u->theme_id = n;
		} else if (!strcmp(k, "ground")) {
			int n = atoi(v);

			if (n >= 0 && n < colour_count(COL_GROUND))
				u->ground_id = n;
		} else if (!strcmp(k, "ink")) {
			int n = atoi(v);

			if (n >= 0 && n < colour_count(COL_INK))
				u->ink_id = n;
		} else if (!strcmp(k, "in_auto")) {
			int n = atoi(v);

			if (n == 0 || (n >= 3 && n <= 10))
				u->in_auto_s = n;
		} else if (!strcmp(k, "rb_steps")) {
			int n = atoi(v);

			if (n >= 1 && n <= 10)
				u->rb_steps = n;
		} else if (!strcmp(k, "screen_off")) {
			int n = atoi(v);

			if (n >= 0 && n <= 3600)
				u->scr_off_s = n;
		} else if (!strcmp(k, "rip_format")) {
			int n;

			for (n = 0; n < NRIP_FMT; n++)
				if (!strncmp(v, rip_fmt_arg[n], strlen(rip_fmt_arg[n])) && (v[strlen(rip_fmt_arg[n])] == '\0' || v[strlen(rip_fmt_arg[n])] == '\n'))
					u->rip_fmt = n;
		} else if (!strcmp(k, "resume")) {
			u->resume = strncmp(v, "off", 3) != 0 && *v != '0';
		} else if (!strcmp(k, "last_radio_url")) {
			char *nl = strpbrk(v, "\r\n");

			if (nl)
				*nl = '\0';
			snprintf(u->resume_url, sizeof u->resume_url, "%s", v);
			snprintf(u->last_url, sizeof u->last_url, "%s", v);
		} else if (!strcmp(k, "last_radio")) {
			char *nl = strpbrk(v, "\r\n");

			if (nl)
				*nl = '\0';
			snprintf(u->resume_name, sizeof u->resume_name, "%s", v);
			snprintf(u->last_name, sizeof u->last_name, "%s", v);
		} else if (!strncmp(k, "vis_", 4)) {
			int j;

			for (j = 0; j < NUM_INPUTS; j++)
				if (!strcmp(k + 4, in_name(j))) {
					int st = vis_unpack(&u->vis[j], strtoul(v, NULL, 10));

					if (old_style < 0)
						old_style = st;
				}
		} else if (!strncmp(k, "trim_", 5)) {
			int n = atoi(v), j;

			for (j = 0; j < NUM_INPUTS; j++)
				if (!strcmp(k + 5, in_name(j)) && n >= 0 && n <= 21)
					u->trim[j] = n;
		} else if (!strcmp(k, "volume")) {
			int n = atoi(v);

			if (n >= 1 && n <= 100)
				u->cfg_vol = n;
		} else if (!strcmp(k, "balance")) {
			int n = atoi(v);

			if (n >= -X45_BALANCE_MAX && n <= X45_BALANCE_MAX)
				u->balance = n;
		} else if (!strcmp(k, "input")) {
			int j;

			for (j = 0; j < NUM_INPUTS; j++)
				if (!strcmp(v, in_name(j)))
					u->cfg_input = j;
		} else if (!strcmp(k, "lineout")) {
			u->out_lineout = atoi(v) != 0;
		} else if (!strcmp(k, "xlr")) {
			u->out_xlr = atoi(v) != 0;
		} else if (!strcmp(k, "brightness")) {
			int n = atoi(v);

			if (n >= 1 && n <= 255)
				u->bright = n;
		} else if (!strcmp(k, "tuner")) {
			u->tuner_off = !strncmp(v, "off", 3) || *v == '0' || !strncmp(v, "no", 2);
		} else if (!strcmp(k, "mpd_password")) {
			char *nl = strpbrk(v, "\r\n");

			if (nl)
				*nl = '\0';
			mpdc_set_password(v);
		}
	}
	fclose(f);
	/* settings from before the spectrum visuals were one (LED, mirror, curve were visuals): the one they had on is the style */
	if (!have_style && old_style > 0) {
		u->spec_style = old_style;
		save_int_key("spec_style", old_style);
	}
}

#define HP_VOLUME 30   /* what the volume drops to when headphones are plugged in */

static void apply_volume(struct ui *u);

/* Headphones: the jack detect cuts the main outputs in hardware, but the
 * headphone amp stays off until it is enabled. Plugging in turns it on and
 * drops the volume to a safe level (the speaker volume is put back on unplug);
 * unplugging turns it off again. */
static void headphone_edge(struct ui *u, int prev)
{
	int present = u->hp_present != 0;

	if (u->hp_seen && present == (prev != 0))
		return;
	if (!u->hp_seen && !present) {
		u->hp_seen = 1;
		return;
	}
	u->hp_seen = 1;
	u->out_hp = present;
	x45_out_headphone(u->x45fd, present);
	x45_outputs_apply(u->x45fd);
	if (present) {
		if (u->volume > HP_VOLUME) {
			u->spk_volume = u->volume;
			u->volume = HP_VOLUME;
			apply_volume(u);
		}
		say(u, "headphones on, volume %d", HP_VOLUME);
	} else {
		if (u->spk_volume > 0) {
			u->volume = u->spk_volume;
			apply_volume(u);
		}
		u->spk_volume = 0;
		say(u, "headphones off");
	}
}

static void refresh_hw(struct ui *u)
{
	if (u->x45fd < 0)
		return;
	{
		int32_t prev = u->hp_present;

		x45_get(u->x45fd, X45_GET_HEADPHONE, &u->hp_present);
		headphone_edge(u, (int)prev);
	}
	x45_get(u->x45fd, X45_GET_PCUSB, &u->pcusb);
	/* the receiver chip's rate is only of interest (and only meaningful) on the digital
	 * inputs; leave the XMOS side alone otherwise, notably while PC USB is selected */
	if (u->input == X45_IN_AES_EBU || u->input == X45_IN_COAXIAL || u->input == X45_IN_OPTICAL)
		x45_get(u->x45fd, X45_GET_CS8422_RATE, &u->cs_rate);
	u->dirty = 1;
}

static int list_len(struct ui *u)
{
	switch (u->screen) {
	case SCR_BROWSE:  return u->nrows;
	case SCR_RADIO:   return u->nstations;
	case SCR_CD:      return u->cd_ntracks;
	case SCR_INPUTS:  return NUM_INPUTS;
	case SCR_TUNER:   return tl_rows(u);
	case SCR_OPTIONS: return ui_ext_menu_count() < 8 ? 4 + ui_ext_menu_count() : 4 + 8;   /* the rows the screen draws */
	case SCR_OPTCAT:  return optcat_len(u, u->opt_cat >= 0 && u->opt_cat < 4 ? u->opt_cat : 0);
	case SCR_VISUALS: { int ids[VR_MAX]; return visuals_rows(u, ids); }
	case SCR_VISSUB:  return vis_count() < VISSUB_MAX ? vis_count() : VISSUB_MAX;
	case SCR_CONFIRM: return 2;
	default:          return 0;
	}
}

static void move_sel(struct ui *u, int delta)
{
	int n = list_len(u);

	if (u->screen == SCR_EXT) {
		ui_ext_move(u, delta);
		return;
	}
	int *cur = u->screen == SCR_BROWSE ? &u->bsel :
		   u->screen == SCR_RADIO ? &u->rsel :
		   u->screen == SCR_CD ? &u->cd_sel :
		   u->screen == SCR_TUNER ? &u->tl_sel : &u->sel;
	int *top = u->screen == SCR_BROWSE ? &u->top :
		   u->screen == SCR_RADIO ? &u->rtop :
		   u->screen == SCR_CD ? &u->cd_top :
		   u->screen == SCR_TUNER ? &u->tl_top : NULL;

	if (!n)
		return;
	if (u->screen == SCR_INPUTS)
		u->in_t = now_ms();
	*cur += delta;
	if (*cur < 0)
		*cur = n - 1;
	if (*cur >= n)
		*cur = 0;
	if (top) {                             /* keep the row in the window (the skin shows skin_rows() of them) */
		int vis = skin_rows(u);

		if (*cur < *top)
			*top = *cur;
		if (*cur >= *top + vis)
			*top = *cur - vis + 1;
	}
	u->dirty = 1;
}

static void set_trim(struct ui *u, int input, int idx)
{
	char k[32], v[16];

	if (input < X45_IN_LINEIN || input > X45_IN_PHONO || idx < 0 || idx > 21)
		return;
	u->trim[input] = idx;
	if (u->input == input)
		x45_set(u->x45fd, X45_SET_NJU72341_VOLUME, idx, 0);
	snprintf(k, sizeof k, "trim_%s", in_name(input));
	snprintf(v, sizeof v, "%d", idx);
	save_config_key(k, v);
	u->dirty = 1;
}

/* which 0 = rca line out, 1 = xlr; the headphone amp follows the jack */
static void set_out(struct ui *u, int which, int on)
{
	if (which == 0) {
		u->out_lineout = on;
		x45_out_lineout(u->x45fd, on);
		save_config_key("lineout", on ? "1" : "0");
	} else {
		u->out_xlr = on;
		x45_out_xlr(u->x45fd, on);
		save_config_key("xlr", on ? "1" : "0");
	}
	/* enables only store driver state; SET 28 is what applies them */
	x45_outputs_apply(u->x45fd);
	u->dirty = 1;
}

/* Select input `n`: the hardware path, then whatever software belongs to it. */
static void input_select(struct ui *u, int n)
{
	int r, rec_again = 0, prev = u->input;

	u->in_sw_t = now_ms();
	u->sel = in_row(n);
	if (n == u->input && tuner_input(u) && u->tu_pid > 0 && u->tu_up) {   /* already on it and it runs: nothing to restart */
		enter_screen(u, SCR_PLAYER);
		return;
	}
	if (u->tuner_off && (n == X45_IN_FMRADIO || n == UI_IN_DAB)) {
		snprintf(u->status, sizeof u->status, "No tuner fitted (tuner = off)");
		return;
	}
	/* the CD input is software: the hardware sits on the network path */
	r = x45_input(u->x45fd, (n == UI_IN_CD || n == UI_IN_BT) ? X45_IN_NETWORK :
				n == UI_IN_DAB ? X45_IN_FMRADIO : (enum x45_input)n);
	if (r < 0 && !getenv("X45UI_PREVIEW")) {   /* the host preview has no /dev/x45 */
		snprintf(u->status, sizeof u->status, "input %s failed: %s", in_name(n), strerror(-r));
		return;
	}
	if (n != u->input) {
		if (u->input == X45_IN_NETWORK)
			net_remember(u);
		meter_stop(u); /* restart on the new source */
		u->m_retry = 0;
		if (u->rec_pid > 0) {   /* the file belongs to the old input: finish it, a new one starts below */
			rec_again = !u->rec_cd;
			rec_stop(u);
		}
	}
	u->input = n;
	/* the new path only takes effect after SET 28 (without it the analogue inputs
	 * capture digital silence) */
	x45_outputs_apply(u->x45fd);
	/* the NJU72341 analogue input stage stays muted until it is sent a gain (SET 27);
	 * the trim is per input, 12 = 0 dB */
	if (u->input >= X45_IN_LINEIN && u->input <= X45_IN_PHONO)
		x45_set(u->x45fd, X45_SET_NJU72341_VOLUME, u->trim[u->input], 0);
	if (u->input != UI_IN_CD)
		cd_stop(u);
	if (!tuner_input(u))
		tuner_stop(u);
	if (u->input != UI_IN_BT)
		bt_stop(u);   /* the radio is only on while the Bluetooth input is chosen */
	if (u->input != X45_IN_NETWORK)
		mpd_raw_stop(u); /* release the XMOS */
	usbpc_select(u, u->input == X45_IN_PCUSB);
	if (u->input == X45_IN_NETWORK)
		x45_clock_select(44100);
	if (tuner_input(u)) {
		u->tu_retries = 0;
		tuner_start(u, u->input == UI_IN_DAB);
		u->tl_n = 0;
		enter_screen(u, SCR_PLAYER);
	}
	if (u->input == UI_IN_CD) {
		/* the disc is read once the CD input is chosen, and again whenever one is inserted */
		u->cd_ntracks = 0;
		enter_screen(u, SCR_PLAYER);
		cd_load_show(u);
	}
	if (u->input == X45_IN_PCUSB)
		enter_screen(u, SCR_PLAYER);
	if (u->input == UI_IN_BT) {
		bt_start(u);
		enter_screen(u, SCR_PLAYER);
	}
	u->status[0] = '\0';   /* the input is named in the header */
	if (rec_again && u->input != UI_IN_CD)
		rec_start(u);   /* the recording goes on, in a file of the new input's name */
	save_config_key("input", in_name(u->input));
	/* choosing an input always lands on that input's player page */
	enter_screen(u, SCR_PLAYER);
	if (u->input == X45_IN_NETWORK && prev != X45_IN_NETWORK && u->resume)
		u->net_t = now_ms() + 900;   /* put its music back (net_poll) */
	u->dirty = 1;
}

/* The panel visuals (level meter, spectrum) are chosen per input from a small menu that
 * RETURN opens on the player screen. Only the sources the unit can tap have any. */
static int visuals_available(const struct ui *u)
{
	(void)u;
	return 1;   /* every source passes the capture the analyzer reads */
}

static void visuals_open(struct ui *u)
{
	if (!visuals_available(u)) {
		say(u, "no visuals on this source");
		return;
	}
	enter_screen(u, SCR_VISUALS);
}

/* Adds the stream that is playing to etc/stations.txt (after the synced block, which a sync
 * leaves alone). */
static void station_save(struct ui *u)
{
	const char *name = u->song.name[0] ? u->song.name : u->song.title[0] ? u->song.title : "Stream";
	char clean[136];
	FILE *f;
	int i;

	radio_load(u);
	for (i = 0; i < u->nstations; i++)
		if (!strcmp(u->stations[i].url, u->song.file)) {
			say(u, "already in the station list: %s", u->stations[i].name);
			return;
		}
	/* the name it was chosen under (from the list, the web page or the directory) beats the one the stream announces */
	if (u->play_url[0] && u->play_name[0] && !strcmp(u->play_url, u->song.file))
		name = u->play_name;
	snprintf(clean, sizeof clean, "%s", name);
	for (i = 0; clean[i]; i++)
		if (clean[i] == '|' || clean[i] == '\n' || clean[i] == '\r')
			clean[i] = ' ';
	f = fopen(u->stations_path, "a+");
	if (!f) {
		say(u, "cannot write the station list");
		return;
	}
	if (fseek(f, -1, SEEK_END) == 0) {   /* a file edited by hand may end without a newline: the entry would join its last line */
		int last = fgetc(f);

		fseek(f, 0, SEEK_END);   /* between a read and a write */
		if (last != '\n')
			fputc('\n', f);
	}
	fprintf(f, "%s | %s\n", clean, u->song.file);
	fclose(f);
	radio_load(u);
	say(u, "saved: %s", clean);
}

/* ---- a yes/no question: before a station scan replaces the list, and before something is deleted ---- */
#define MENU_HOLD_MS 1000   /* a hold of MENU or STOP */
enum { CF_SCAN, CF_DELETE, CF_FAV, CF_REC };

static void confirm_open(struct ui *u, int act, const char *what, const char *path)
{
	u->cf_act = act;
	u->cf_ret = u->screen == SCR_VISUALS ? SCR_PLAYER : u->screen;
	snprintf(u->cf_what, sizeof u->cf_what, "%s", what);
	snprintf(u->cf_path, sizeof u->cf_path, "%s", path ? path : "");
	enter_screen(u, SCR_CONFIRM);
	u->sel = 1;   /* the cursor starts on "no" */
}

/* REC on the remote asks first: a stray press must not take the capture device and start filling the disk. A
 * recording that runs is stopped by the same key, without a question. */
static void rec_ask(struct ui *u)
{
	char name[96], what[160];

	if (u->rec_pid > 0) {
		rec_stop(u);
		return;
	}
	if (u->input == UI_IN_CD) {
		snprintf(what, sizeof what, "Rip the disc to disk as %s?", rip_fmt_name[u->rip_fmt]);
	} else {
		rec_name(u, name, sizeof name);
		snprintf(what, sizeof what, "Record %s to disk?", name);
	}
	confirm_open(u, CF_REC, what, NULL);
}

static int rmtree(const char *path, int depth)
{
	struct stat st;
	DIR *d;
	struct dirent *e;
	char sub[1024];

	if (lstat(path, &st) != 0)
		return -1;
	if (!S_ISDIR(st.st_mode))
		return unlink(path);
	if (depth > 24 || !(d = opendir(path)))
		return -1;
	while ((e = readdir(d))) {
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		if (snprintf(sub, sizeof sub, "%s/%s", path, e->d_name) >= (int)sizeof sub)
			continue;   /* a cut-off name would name some other path */
		rmtree(sub, depth + 1);
	}
	closedir(d);
	return rmdir(path);
}

static char mount_dev[128];   /* the device of the mount mount_rw found */

/* is `real` on a read-write mounted storage? root gets the mount point */
static int mount_rw(const char *real, char *root, size_t sz)
{
	FILE *m = fopen("/proc/mounts", "r");
	char line[512], dev[128], mp[128], fs[32], opt[128];
	size_t best = 0;
	int rw = 0;

	if (!m)
		return 0;
	while (fgets(line, sizeof line, m)) {
		size_t l;

		if (sscanf(line, "%127s %127s %31s %127s", dev, mp, fs, opt) != 4 || strncmp(dev, "/dev/", 5))
			continue;
		l = strlen(mp);
		if (!strncmp(real, mp, l) && (real[l] == '/' || real[l] == '\0') && l > best && strcmp(mp, "/")) {
			best = l;
			rw = !strncmp(opt, "rw", 2);
			snprintf(root, sz, "%s", mp);
			snprintf(mount_dev, sizeof mount_dev, "%s", dev);
		}
	}
	fclose(m);
	return best && rw;
}

/* Something in the library was deleted from the disk (here, or by the web page): what plays from it stops (MPD would go
 * on reading the open file), and the rows of the list that name it are dropped now, not when MPD's update has run. */
static void lib_gone(struct ui *u, const char *rel)
{
	size_t n = strlen(rel);
	int i;

	if (!n)
		return;
	if (u->have_song && !strncmp(u->song.file, rel, n) && (u->song.file[n] == '/' || u->song.file[n] == '\0') &&
	    strcmp(u->st.state, "stop"))
		mpd_do(u, "stop");
	if (u->rows && u->dir[0] != '@') {
		for (i = 0; i < u->nrows;) {
			const char *p = u->rows[i].path;

			if (strncmp(p, rel, n) || (p[n] != '/' && p[n] != '\0')) {
				i++;
				continue;
			}
			memmove(&u->rows[i], &u->rows[i + 1], (size_t)(u->nrows - i - 1) * sizeof *u->rows);
			u->nrows--;
			if (i < u->bsel)
				u->bsel--;
		}
		if (u->bsel >= u->nrows)
			u->bsel = u->nrows > 0 ? u->nrows - 1 : 0;
		if (u->top > u->bsel)
			u->top = u->bsel;
	}
	u->dirty = 1;
}

/* the same rules as the web interface's delete: only below a read-write storage, never a
 * library root or a whole disk. Returns 0 on success, else a message in why. */
static int lib_delete_do(const char *rel, const char **why);

/* After a deletion, the folders it left empty go too, up to (not including) the library root `base`: MPD lists no
 * folder without music, so an emptied album folder could not be seen, let alone deleted, from the library. */
static void prune_empty_dirs(const char *deleted, const char *base)
{
	char dir[PATH_MAX], *slash;
	size_t bl = strlen(base);

	snprintf(dir, sizeof dir, "%s", deleted);
	while ((slash = strrchr(dir, '/')) && slash != dir) {
		*slash = '\0';
		if (strncmp(dir, base, bl) || dir[bl] != '/')
			return;
		if (rmdir(dir) != 0)   /* not empty (or not ours to remove) */
			return;
	}
}

/* x45media puts disks back to read-only; this marker keeps it from doing so in the middle of a deletion.
 * The web file manager changes the disks under /tmp/x45files.lock and owns the same marker while it does: a deletion
 * from the panel takes that lock too, or it would remove the marker (and the read-write mount) under an upload. */
static int lib_delete(const char *rel, const char **why)
{
	int lock = open("/tmp/x45files.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0666), hold, r;

	if (lock >= 0 && flock(lock, LOCK_EX | LOCK_NB) != 0) {
		close(lock);
		*why = "busy with another change";
		return -1;
	}
	hold = open("/tmp/x45media.hold", O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
	r = lib_delete_do(rel, why);
	if (hold >= 0) {
		close(hold);
		unlink("/tmp/x45media.hold");
	}
	if (lock >= 0)
		close(lock);
	return r;
}

static int lib_delete_do(const char *rel, const char **why)
{
	char full[900], real[PATH_MAX], root[160], base[PATH_MAX], top[700];
	const char *e;
	int rw, remounted = 0;

	if (!rel[0] || path_has_dotdot(rel) || rel[0] == '/') {
		*why = "bad path";
		return -1;
	}
	snprintf(full, sizeof full, "%s/%s", LIB_ROOT, rel);
	if (!realpath(full, real)) {
		*why = "no such file";
		return -1;
	}
	e = strchr(rel, '/');
	if (!e || !e[1]) {
		*why = "that is a library root";
		return -1;
	}
	snprintf(top, sizeof top, "%s/%.*s", LIB_ROOT, (int)(e - rel), rel);
	if (!realpath(top, base) || !strcmp(base, real)) {
		*why = "that is a library root";
		return -1;
	}
	root[0] = '\0';
	rw = mount_rw(real, root, sizeof root);
	if (!root[0]) {
		*why = "that is not a disk";
		return -1;
	}
	if (!strcmp(root, real)) {
		*why = "that is a whole disk";
		return -1;
	}
	if (!rw) {
		/* an attached disk is mounted read-only by the music player: switch it for the deletion */
		char cmd[300];

		if (strncmp(mount_dev, "/dev/sd", 7) != 0 || strpbrk(root, "'\\")) {
			*why = "that storage is read only";
			return -1;
		}
		snprintf(cmd, sizeof cmd, "mount -o remount,rw '%s' >/dev/null 2>&1", root);
		if (system(cmd) != 0) {
			*why = "that storage is read only";
			return -1;
		}
		remounted = 1;
	}
	{
		int rc = rmtree(real, 0);

		if (rc == 0)
			prune_empty_dirs(real, base);
		if (remounted) {
			char cmd[300];

			sync();
			snprintf(cmd, sizeof cmd, "mount -o remount,ro '%s' >/dev/null 2>&1", root);
			(void)system(cmd);
		}
		if (rc != 0) {
			*why = "could not delete everything";
			return -1;
		}
	}
	return 0;
}

/* Takes the entry with this URL out of the station file (the part the FRITZ!Box sync does not manage). */
static int fav_remove(struct ui *u, const char *url, const char **why)
{
	FILE *f = fopen(u->stations_path, "r");
	char tmp[320], line[1200];
	FILE *o;
	int managed = 0, removed = 0, in_managed_only = 0;

	if (!f) {
		*why = "cannot read the station list";
		return -1;
	}
	snprintf(tmp, sizeof tmp, "%s.new", u->stations_path);
	o = fopen(tmp, "w");
	if (!o) {
		fclose(f);
		*why = "cannot write the station list";
		return -1;
	}
	while (fgets(line, sizeof line, f)) {
		const char *bar;

		if (strstr(line, "fritzbox begin"))
			managed = 1;
		if (!strncmp(line, "#", 1) || !(bar = strchr(line, '|'))) {
			fputs(line, o);
			if (strstr(line, "fritzbox end"))
				managed = 0;
			continue;
		}
		{
			char val[1000];
			size_t n;

			snprintf(val, sizeof val, "%s", bar + 1);
			n = strspn(val, " \t");
			memmove(val, val + n, strlen(val + n) + 1);
			val[strcspn(val, "\r\n")] = '\0';
			while (val[0] && val[strlen(val) - 1] == ' ')
				val[strlen(val) - 1] = '\0';
			if (!strcmp(val, url)) {
				if (managed) {
					in_managed_only = 1;
					fputs(line, o);
				} else {
					removed = 1;   /* dropped */
				}
				continue;
			}
		}
		fputs(line, o);
	}
	fclose(f);
	if (fclose(o) != 0 || (!removed && !in_managed_only)) {
		unlink(tmp);
		*why = "not found in the list";
		return -1;
	}
	if (!removed) {
		unlink(tmp);
		*why = "that one comes from the FRITZ!Box list";
		return -1;
	}
	if (rename(tmp, u->stations_path) != 0) {
		unlink(tmp);
		*why = "cannot write the station list";
		return -1;
	}
	return 0;
}

/* holding STOP: delete the track that plays, or the entry the library cursor is on; on the radio list it takes
 * the station out of the favourites */
static void stop_hold_fire(struct ui *u)
{
	const char *rel = NULL;
	char what[160];

	if (u->screen == SCR_BROWSE && !strcmp(u->dir, "@fav") && u->rows && u->bsel >= 0 && u->bsel < u->nrows &&
	    !strncmp(u->rows[u->bsel].path, "@s|", 3)) {
		snprintf(what, sizeof what, "Remove %s from the favourites?", u->rows[u->bsel].name);
		confirm_open(u, CF_FAV, what, u->rows[u->bsel].path + 3);
		return;
	}
	if (u->screen == SCR_RADIO && u->rsel >= 0 && u->rsel < u->nstations) {
		snprintf(what, sizeof what, "Remove %s from the favourites?", u->stations[u->rsel].name);
		confirm_open(u, CF_FAV, what, u->stations[u->rsel].url);
		return;
	}

	if (u->screen == SCR_BROWSE && u->rows && u->bsel >= 0 && u->bsel < u->nrows &&
	    u->rows[u->bsel].path[0] != '@' && strncmp(u->dir, "@net", 4)) {
		rel = u->rows[u->bsel].path;
		snprintf(what, sizeof what, "Delete %s%s?", u->rows[u->bsel].is_dir ? "the folder " : "", u->rows[u->bsel].name);
	} else if (u->screen == SCR_PLAYER && u->input == X45_IN_NETWORK && u->have_song && u->song.file[0] &&
		   !strstr(u->song.file, "://") && u->song.file[0] != '@') {
		const char *b = strrchr(u->song.file, '/');

		rel = u->song.file;
		snprintf(what, sizeof what, "Delete %s?", b ? b + 1 : u->song.file);
	}
	if (!rel) {
		if (u->screen == SCR_BROWSE || u->screen == SCR_PLAYER)   /* a screen that shows files; a menu says nothing */
			say(u, "nothing to delete here");
		return;
	}
	confirm_open(u, CF_DELETE, what, rel);
}

static void stop_hold_poll(struct ui *u)
{
	long now = now_ms();

	if (!u->stop_down || u->stop_fired || now - u->stop_down < MENU_HOLD_MS)
		return;
	u->stop_fired = 1;
	if (u->rec_pid > 0) {   /* a recording runs: this stops it, nothing is deleted */
		rec_stop(u);
		return;
	}
	stop_hold_fire(u);
}

static void confirm_ok(struct ui *u)
{
	int ret = u->cf_ret;

	if (u->sel != 0) {
		enter_screen(u, ret);
		return;
	}
	if (u->cf_act == CF_REC) {
		enter_screen(u, ret);
		rec_start(u);
		return;
	}
	if (u->cf_act == CF_FAV) {
		const char *why = "";

		if (fav_remove(u, u->cf_path, &why) == 0) {
			int keep = u->rsel;

			radio_load(u);
			u->rsel = keep < u->nstations ? keep : (u->nstations > 0 ? u->nstations - 1 : 0);
			if (ret == SCR_BROWSE && u->rows && u->bsel >= 0 && u->bsel < u->nrows) {
				memmove(&u->rows[u->bsel], &u->rows[u->bsel + 1], (size_t)(u->nrows - u->bsel - 1) * sizeof *u->rows);
				u->nrows--;
				if (u->bsel >= u->nrows)
					u->bsel = u->nrows > 0 ? u->nrows - 1 : 0;
				if (u->top > u->bsel)
					u->top = u->bsel;
			}
			say(u, "removed from the favourites");
		} else {
			say(u, "%s", why);
		}
		enter_screen(u, ret);
		return;
	}
	if (u->cf_act == CF_SCAN) {
		tuner_cmd(u, u->input == UI_IN_DAB ? "scan" : "fmscan");
		enter_screen(u, SCR_TUNER);   /* shows the progress */
		return;
	}
	{
		const char *why = "";

		if (lib_delete(u->cf_path, &why) == 0) {
			say(u, "deleted");
			mpd_do(u, "update");
			lib_gone(u, u->cf_path);   /* not a reload: MPD still lists it until its update has run */
		} else {
			say(u, "%s", why);
		}
	}
	{
		int keep = u->bsel, keep_top = u->top;

		enter_screen(u, ret);   /* the browser keeps its cursor: do not reload it */
		u->bsel = keep;
		u->top = keep_top;
	}
	u->dirty = 1;
}

/* OK on a row of the visualizers menu: a switch flips (for this input or for all of them) */
static void vissub_toggle(struct ui *u, int row)
{
	struct vis_sel *sel;
	unsigned bit, dropped;
	int i;

	if (u->input < 0 || u->input >= NUM_INPUTS || row < 0 || row >= vis_count())
		return;
	bit = vis_at(row)->bit;
	sel = vis_sel_of(u);
	dropped = vis_switch(sel, bit);
	if (u->vis_all) {
		save_vis_key("vis_all_set", sel);
	} else {
		char k[32];

		snprintf(k, sizeof k, "vis_%s", in_name(u->input));
		save_vis_key(k, sel);
	}
	u->status[0] = '\0';
	for (i = 0; dropped && i < vis_count(); i++)
		if (vis_at(i)->bit == dropped)
			say(u, "At most %d at a time: %s is off", VIS_MAX_ON, vis_at(i)->label);
	u->dirty = 1;
}

/* the station that plays goes into the favourites: an FM frequency into its list, an internet radio stream into the
 * station list */
static void favourite_save(struct ui *u)
{
	int bit;

	if (u->input == X45_IN_FMRADIO) {
		if (u->tu_freq >= 8750) {
			tuner_load_list(u);
			for (bit = 0; bit < u->tl_n; bit++)
				if (abs(u->tl_val[bit] - u->tu_freq) < 5)
					break;
			if (bit < u->tl_n) {
				say(u, "%d.%02d MHz is already in the list", u->tu_freq / 100, u->tu_freq % 100);
			} else {
				tuner_cmd(u, "fmadd");
				say(u, "saved %d.%02d MHz", u->tu_freq / 100, u->tu_freq % 100);
			}
		}
	} else if (u->input == X45_IN_NETWORK && u->have_song && !strncmp(u->song.file, "http", 4) && u->st.duration <= 0) {
		station_save(u);
	} else {
		say(u, "only a radio station can be saved");
	}
}

/* OK on a row of the RETURN menu: the sub menu opens, an action runs and returns to the player */
static void visuals_toggle(struct ui *u, int row)
{
	int ids[VR_MAX], n = visuals_rows(u, ids);

	if (u->input < 0 || u->input >= NUM_INPUTS || row < 0 || row >= n)
		return;
	switch (ids[row]) {
	case VR_SUB:
		enter_screen(u, SCR_VISSUB);
		return;
	case VR_SEARCH:
		ui_ext_open_name(u, "search");
		return;
	case VR_LOOK:   /* the visualizer group of the options; RETURN comes back to this menu, not to the options */
		u->opt_cat = 2;
		enter_screen(u, SCR_OPTCAT);
		u->look_from_menu = 1;
		return;
	case VR_BALANCE:   /* RETURN or OK there comes back to this menu */
		ui_ext_open_name(u, "balance");
		return;
	case VR_REC:
		rec_toggle(u);
		enter_screen(u, SCR_PLAYER);
		return;
	case VR_SAVE:
		favourite_save(u);
		enter_screen(u, SCR_PLAYER);
		return;
	case VR_BTPAIR:
		bt_cmd(u, 'D');
		say(u, "open for pairing for two minutes");
		enter_screen(u, SCR_PLAYER);
		return;
	case VR_BTFORGET:
		bt_cmd(u, 'F');
		say(u, "all paired devices forgotten");
		enter_screen(u, SCR_PLAYER);
		return;
	default:   /* VR_SCAN */
		if (tuner_scanning(u)) {
			tuner_cmd(u, "cancel");
			say(u, "scan stopped");
			enter_screen(u, SCR_PLAYER);
		} else {
			confirm_open(u, CF_SCAN, u->input == UI_IN_DAB ? "Scan for DAB stations? The list is replaced." :
				     "Scan for FM stations? The list is replaced.", NULL);
		}
		return;
	}
}

/* the brightness is never 0 or 1 (both look like a dead unit): 2..max */
static void bl_adjust(struct ui *u, int ticks)
{
	int cur = bl_get();

	bl_set(u, cur + ticks);
	u->dirty = 1;
}

static void on_ok(struct ui *u)
{
	if (u->input == UI_IN_BT && u->bt_confirm[0]) {   /* the owner confirms the pairing request */
		bt_cmd(u, 'y');
		return;
	}
	if (u->screen == SCR_PLAYER) {
		mpd_toggle(u);
	} else if (u->screen == SCR_BROWSE) {
		browse_enter(u);
	} else if (u->screen == SCR_RADIO) {
		radio_tune(u);
	} else if (u->screen == SCR_CD) {
		if (u->cd_ntracks <= 0)
			cd_load_show(u);
		else if (u->cd_len[u->cd_sel] < 0)
			say(u, "that is a data track");
		else if (cd_active(u) && u->cd_track == u->cd_sel + 1)
			kill(u->cd_pid, SIGUSR1);   /* pause / resume */
		else {
			cd_play(u, u->cd_sel + 1);
			enter_screen(u, SCR_PLAYER);
		}
	} else if (u->screen == SCR_TUNER) {
		if (u->tl_n > 0 && u->tl_sel >= u->tl_n) {
			ui_ext_open_name(u, "search");
		} else if (u->tl_n > 0) {
			if (u->input == UI_IN_DAB)
				tuner_cmd(u, "select %d", u->tl_val[u->tl_sel]);
			else
				tuner_cmd(u, "tune %d", u->tl_val[u->tl_sel]);
			enter_screen(u, SCR_PLAYER);
		}
	} else if (u->screen == SCR_INPUTS) {
		input_select(u, in_order[u->sel]);
	} else if (u->screen == SCR_VISUALS) {
		visuals_toggle(u, u->sel);
	} else if (u->screen == SCR_CONFIRM) {
		confirm_ok(u);
		return;
	} else if (u->screen == SCR_VISSUB) {
		vissub_toggle(u, u->sel);
		return;
	} else if (u->screen == SCR_EXT) {
		ui_ext_ok(u);
		return;
	} else if (u->screen == SCR_OPTIONS) {
		if (u->sel >= 4) {   /* a screen of ext/ listed under Options */
			const struct ui_ext *e = ui_ext_menu_at(u->sel - 4);

			if (e)
				ui_ext_open(u, e);
			return;
		}
		u->opt_cat = u->sel;
		u->look_from_menu = 0;
		enter_screen(u, SCR_OPTCAT);
		return;
	} else if (u->screen == SCR_OPTCAT) {
		int cat = u->opt_cat >= 0 && u->opt_cat < 4 ? u->opt_cat : 0, ids[OPT_MAX], n = optcat_ids(u, cat, ids), i;

		if (u->sel >= 0 && u->sel < n) {
			int id = ids[u->sel];

			opt_activate(u, id);
			/* a choice can bring a row in (the stereo field's colour "by distance" adds the bar colours):
			 * the cursor stays on the row it was on */
			n = optcat_ids(u, cat, ids);
			for (i = 0; i < n; i++)
				if (ids[i] == id)
					u->sel = i;
		}
	}
	u->dirty = 1;
}

/* where the input list returns to; the radio list only exists on the network input */
static enum screen ret_target(const struct ui *u)
{
	if (!screen_valid(u, u->ret_screen))
		return SCR_PLAYER;
	return u->ret_screen;
}

/* Which screens the MENU key cycles through for the current input: the library and
 * internet radio belong to the network input (MPD), the track list to the CD input,
 * the station list to FM/DAB. The INPUT screen has its own key. */
static int menu_screen_ok(const struct ui *u, enum screen s)
{
	switch (s) {
	case SCR_RADIO:  return 0;   /* the favourites are a folder of the library now */
	case SCR_BROWSE: return u->input == X45_IN_NETWORK;
	case SCR_CD:     return u->input == UI_IN_CD;
	case SCR_TUNER:  return tuner_input(u);
	case SCR_INPUTS:
	case SCR_MAIN:      /* a 1 s hold of MENU */
	case SCR_OPTIONS:   /* a 1 s hold of MENU */
	case SCR_OPTCAT:
	case SCR_VISUALS:
	case SCR_VISSUB:
	case SCR_EXT:
	case SCR_CONFIRM: return 0;   /* RETURN on the player */
	default:         return 1;
	}
}

/* a screen that may stay up when the input changes: options and status belong to no input */
static int screen_valid(const struct ui *u, enum screen s)
{
	return s == SCR_OPTIONS || s == SCR_OPTCAT || s == SCR_MAIN || s == SCR_PLAYER || menu_screen_ok(u, s);
}

/* player_menu: RETURN of the front panel opens the player's menu when there is nothing to go back from; the
 * remote's RETURN does not (its MENU key does) */
static void go_back(struct ui *u, int player_menu)
{
	if (u->input == UI_IN_BT && u->bt_confirm[0]) {   /* refuse the pairing request */
		bt_cmd(u, 'n');
		return;
	}
	if (u->screen == SCR_CONFIRM) {
		enter_screen(u, u->cf_ret);
		return;
	}
	if (u->screen == SCR_EXT) {
		ui_ext_back(u);
		return;
	}
	if (u->screen == SCR_OPTCAT) {
		if (u->look_from_menu) {   /* opened from the RETURN menu of the player: back to it */
			u->look_from_menu = 0;
			enter_screen(u, SCR_VISUALS);
			return;
		}
		enter_screen(u, SCR_OPTIONS);
		u->sel = u->opt_cat;
		return;
	}
	if (u->screen == SCR_VISSUB) {
		enter_screen(u, SCR_VISUALS);
		return;
	}
	if (u->screen == SCR_PLAYER) {
		if (player_menu)
			visuals_open(u);
		return;
	}
	if (u->screen == SCR_BROWSE) {
		browse_up(u);
		return;
	}
	if (u->screen == SCR_INPUTS) {
		enter_screen(u, ret_target(u));
		return;
	}
	u->screen = SCR_PLAYER;
	u->dirty = 1;
}

void enter_screen(struct ui *u, enum screen s)
{
	u->screen = s;
	u->bl_edit = 0;
	u->rb = 0;
	if (s == SCR_BROWSE) {
		/* the browser keeps its own cursor (bsel), so coming back lands on
		 * the same row; load only if nothing usable is there yet */
		if (!u->rows || u->dir_err[0])
			browse_load(u, u->dir, NULL);
	} else if (s == SCR_RADIO) {
		radio_load(u);
	} else if (s == SCR_CD) {
		cd_load_show(u);
		if (cd_active(u) && u->cd_track >= 1 && u->cd_track <= u->cd_ntracks) {   /* the cursor starts on what plays */
			u->cd_sel = u->cd_track - 1;
			u->cd_top = u->cd_sel > 4 ? u->cd_sel - 4 : 0;
		}
	} else if (s == SCR_TUNER) {
		tuner_load_list(u);
	} else {
		u->sel = s == SCR_INPUTS ? in_row(u->input) : 0;
		u->in_t = now_ms();
	}
	u->dirty = 1;
}

/* Shut-down needs two presses within 3 s, so a stray touch of the key (or of
 * the remote) cannot switch the unit off mid-album. The second press ends the
 * UI with status 3; the supervisor (deploy/x45stack) does the actual poweroff.
 * Every press goes to POWER_LOG, which survives the reboot: once the unit went off without the question, and
 * nothing could say whether this program had asked for it. */
#define POWER_CONFIRM_MS 3000
#define STATUS_MS 8000   /* how long a message stays under the content */
#define POWER_LOG "/home/root/x45new/power.log"

static void power_log(const char *src, const char *what, long gap)
{
	FILE *f = fopen(POWER_LOG, "a"), *up = fopen("/proc/uptime", "r");
	double t = 0;

	if (up) {
		if (fscanf(up, "%lf", &t) != 1)
			t = 0;
		fclose(up);
	}
	if (f) {
		fprintf(f, "up %.1f s  src=%s  %s  gap=%ld ms\n", t, src, what, gap);
		fclose(f);
	}
}

static void power_press(struct ui *u, const char *src)
{
	long now = now_ms(), gap = u->power_armed ? now - u->power_armed : -1;

	if (u->power_armed && gap <= POWER_CONFIRM_MS) {
		power_log(src, "second press: shutting down", gap);
		sync();
		u->quit_code = 3;
		stop_now = 1;
		return;
	}
	u->power_armed = now;
	power_log(src, "first press: asking", gap);
	say(u, "press POWER again within 3 s to shut down");
}

static void stop_press(struct ui *u);
static void cd_eject(struct ui *u);

static void volume_step(struct ui *u, int d)
{
	if (VOL_LOCKED(u))
		return;
	volume_touch(u);
	u->volume += d;
	apply_volume(u);
}

/* INPUT: the list of inputs; pressed again it steps to the next entry */
static void key_input(struct ui *u)
{
	if (u->screen == SCR_INPUTS) {
		move_sel(u, 1);
		return;
	}
	u->ret_screen = u->screen;
	enter_screen(u, SCR_INPUTS);
}

/* RADIO: the radio input; pressed on it, the other band (DAB, then FM, then DAB again) */
static void key_radio(struct ui *u)
{
	if (u->tuner_off) {
		say(u, "no radio module");
		return;
	}
	input_select(u, u->input == UI_IN_DAB ? X45_IN_FMRADIO : UI_IN_DAB);
}

/* PLAYLIST: the list of what the current input has (the library, the disc's tracks, the stations) */
static void key_list(struct ui *u)
{
	if (u->input == X45_IN_NETWORK)
		enter_screen(u, SCR_BROWSE);
	else if (u->input == UI_IN_CD)
		enter_screen(u, SCR_CD);
	else if (tuner_input(u))
		enter_screen(u, SCR_TUNER);
	else
		say(u, "this input has no list");
}

/* HOME: the top of what the current input has - the library's root (the disks, Radio, Network) on the network input,
 * the tracks of the disc, the station list; the inputs without a list (S/PDIF, line in, PC USB, Bluetooth ...) have
 * no top to go to and show the playing screen. NOW PLAYING is the way back to it. */
static void key_home(struct ui *u)
{
	if (u->input == X45_IN_NETWORK) {
		enter_screen(u, SCR_BROWSE);
		u->net_depth = 0;
		browse_load(u, "", NULL);
	} else if (u->input == UI_IN_CD || tuner_input(u)) {
		key_list(u);
	} else {
		enter_screen(u, SCR_PLAYER);
	}
}

/* the green key (labelled brightness): four steps down, then back to the brightest */
static void bl_cycle(struct ui *u)
{
	int mx = bl_max(), cur = bl_get(), step = (mx - 2 + 3) / 4, nx;

	if (step < 1)
		step = 1;
	nx = cur - step;
	if (nx < 2)
		nx = mx;
	bl_set(u, nx);
	say(u, "Brightness %d of %d", u->bright, mx);
}

/* the number keys seek on the playing screen: 1 to 9 are tenths of the track, 0 the start */
static void seek_tenth(struct ui *u, int n)
{
	char cmd[32];

	if (u->screen != SCR_PLAYER || u->st.duration <= 0)
		return;
	snprintf(cmd, sizeof cmd, "seekcur %d", (int)(u->st.duration * n / 10));
	mpd_do(u, cmd);
}

/* The cursor keys on a list. A thumb stays on a button longer than the 108 ms between two NEC frames, so an ordinary
 * tap arrived as a press and a repeat and moved the cursor two rows or more. Now a repeat counts only after the
 * button was held LIST_HOLD_MS, and then once per LIST_REPEAT_MS; the input list takes no repeat at all (one press,
 * one row); a second press of the same button inside LIST_BOUNCE_MS (see handle()) is dropped. */
#define LIST_BOUNCE_MS 140
#define LIST_HOLD_MS   550
#define LIST_REPEAT_MS 200

static int list_key_ok(struct ui *u, const struct x45_event *ev)
{
	long now = now_ms();

	if (ev->pressed != 2)
		return !u->ir_bounce;
	if (u->screen == SCR_INPUTS || ev->code != u->ir_code)
		return 0;
	if (now - u->ir_down < LIST_HOLD_MS || now - u->ir_step < LIST_REPEAT_MS)
		return 0;
	u->ir_step = now;
	return 1;
}

/* The visualizers menu is a grid of cards: left and right go one card, up and down one row (the same column; a short
 * last row is skipped over), and both wrap round */
static void vissub_move(struct ui *u, int dcol, int drow)
{
	int n = list_len(u), cols = vis_menu_cols(), rows, r, c;

	if (n < 1)
		return;
	rows = (n + cols - 1) / cols;
	if (dcol) {
		u->sel = (u->sel + dcol + n) % n;
	} else {
		r = u->sel / cols;
		c = u->sel % cols;
		do
			r = (r + drow + rows) % rows;
		while (r * cols + c >= n);
		u->sel = r * cols + c;
	}
	u->in_t = now_ms();
	u->dirty = 1;
}

/* The remote. What a button does follows its label and the manual (docs/X45-REMOTE.md); the buttons nobody has a
 * function for are left out. RETURN goes back one level and nothing more: the menu the front panel's RETURN opens
 * on the player is on MENU here. */
static void handle_ir(struct ui *u, const struct x45_event *ev)
{
	int on_list = u->screen == SCR_BROWSE || u->screen == SCR_RADIO ||
		      u->screen == SCR_CD || u->screen == SCR_TUNER || u->screen == SCR_INPUTS ||
		      u->screen == SCR_OPTIONS || u->screen == SCR_OPTCAT || u->screen == SCR_VISUALS || u->screen == SCR_VISSUB ||
		      u->screen == SCR_CONFIRM || u->screen == SCR_EXT;

	if (u->screen == SCR_EXT && ui_ext_ir(u, ev->code))   /* a screen of ext/ with keys of its own (the search types) */
		return;
	switch (ev->code) {
	case IR_SEARCH:   /* find a station by its name (the radio inputs) */
		if (u->screen != SCR_EXT && !u->ir_bounce) {
			if (stations_searchable(u))
				ui_ext_open_name(u, "search");
			else
				say(u, "no stations to search on this input");
		}
		break;
	case IR_ENTER:   /* the remote has a play/pause button of its own: OK on the playing screen goes back to the list */
		if (u->screen == SCR_PLAYER && (u->input == X45_IN_NETWORK || u->input == UI_IN_CD || tuner_input(u)))
			key_list(u);
		else
			on_ok(u);
		break;
	case IR_ESC:
		go_back(u, 0);
		break;
	case IR_HOME:
		key_home(u);
		break;
	case IR_NOWPLAY:
		u->screen = SCR_PLAYER;
		u->dirty = 1;
		break;
	case IR_POWER:
		power_press(u, "remote");
		break;
	case IR_MENU:   /* the menu of the player: visualizers, record, save the station, scan; again closes it */
		if (u->screen == SCR_VISUALS)
			enter_screen(u, SCR_PLAYER);
		else
			visuals_open(u);
		break;
	case IR_SETUP:   /* the settings; again leaves them */
		enter_screen(u, u->screen == SCR_OPTIONS || u->screen == SCR_OPTCAT || u->screen == SCR_EXT ? SCR_PLAYER : SCR_OPTIONS);
		break;
	case IR_INFO:    /* the status page */
		enter_screen(u, u->screen == SCR_MAIN ? SCR_PLAYER : SCR_MAIN);
		break;
	case IR_INPUT:
		if (u->screen != SCR_INPUTS || !u->ir_bounce)
			key_input(u);
		break;
	case IR_ISERVICE:   /* the online services: the network input (the playing screen when already on it) */
		if (u->input == X45_IN_NETWORK)
			enter_screen(u, SCR_PLAYER);
		else
			input_select(u, X45_IN_NETWORK);
		break;
	case IR_RADIO:
		key_radio(u);
		break;
	case IR_PLAYLIST:
		key_list(u);
		break;
	case IR_MUTE:
		set_mute(u, !u->muted);
		break;
	case IR_VOLUP:
		volume_step(u, 2);
		break;
	case IR_VOLDOWN:
		volume_step(u, -2);
		break;
	case IR_BALANCE:   /* the balance control; the same key on it closes it again (ext/balance.c) */
		if (u->screen != SCR_EXT && !u->ir_bounce)
			ui_ext_open_name(u, "balance");
		break;
	case IR_UP:
	case IR_DOWN:
		if (u->screen == SCR_OPTCAT && u->bl_edit)
			bl_adjust(u, ev->code == IR_UP ? 1 : -1);
		else if (u->screen == SCR_EXT) {
			if (list_key_ok(u, ev))
				ui_ext_nav(u, 0, ev->code == IR_UP ? -1 : 1);
		} else if (u->screen == SCR_VISSUB) {
			if (list_key_ok(u, ev))
				vissub_move(u, 0, ev->code == IR_UP ? -1 : 1);
		} else if (on_list) {
			if (list_key_ok(u, ev))
				move_sel(u, ev->code == IR_UP ? -1 : 1);
		} else {
			volume_step(u, ev->code == IR_UP ? 2 : -2);
		}
		break;
	case IR_LEFT:
		if (u->screen == SCR_INPUTS) {   /* the input list: left and right go through it like up and down */
			if (list_key_ok(u, ev))
				move_sel(u, -1);
		} else if (u->screen == SCR_VISSUB) {
			if (list_key_ok(u, ev))
				vissub_move(u, -1, 0);
		} else if (u->screen == SCR_EXT)
			ui_ext_nav(u, -1, 0);
		else if (u->screen == SCR_BROWSE)
			browse_up(u);
		else if (tuner_input(u) && u->input == X45_IN_FMRADIO)
			fm_knob(u, -1);
		else
			skip_track(u, -1);
		break;
	case IR_RIGHT:
		if (u->screen == SCR_INPUTS) {
			if (list_key_ok(u, ev))
				move_sel(u, 1);
		} else if (u->screen == SCR_VISSUB) {
			if (list_key_ok(u, ev))
				vissub_move(u, 1, 0);
		} else if (u->screen == SCR_EXT)
			ui_ext_nav(u, 1, 0);
		else if (u->screen == SCR_BROWSE)
			browse_enter(u);
		else if (tuner_input(u) && u->input == X45_IN_FMRADIO)
			fm_knob(u, 1);
		else
			skip_track(u, 1);
		break;
	case IR_PLAYPAUSE:
		mpd_toggle(u);
		break;
	case IR_STOP:
		stop_press(u);
		break;
	case IR_NEXT:
		skip_track(u, 1);
		break;
	case IR_PREV:
		skip_track(u, -1);
		break;
	case IR_FFWD:
		mpd_do(u, "seekcur +15");
		break;
	case IR_REWIND:
		mpd_do(u, "seekcur -15");
		break;
	case IR_SHUFFLE:
		mpd_do(u, u->st.random ? "random 0" : "random 1");
		break;
	case IR_REC:   /* asks first: a recording takes the capture device and fills the disk; REC again answers yes */
		if (u->screen == SCR_CONFIRM && u->cf_act == CF_REC) {
			u->sel = 0;
			confirm_ok(u);
		} else {
			rec_ask(u);
		}
		break;
	case IR_RIPCD:
		if (u->input == UI_IN_CD)
			rec_ask(u);
		else
			say(u, "choose the CD input to rip a disc");
		break;
	case IR_EJECT:
		cd_eject(u);
		break;
	case IR_BOOKMARK:    /* saves what plays as a new favourite (a station of the network input, a frequency of FM) */
		favourite_save(u);
		break;
	case IR_FAVOURITE:   /* the network input: the favourites folder; the other inputs keep saving, as before */
		if (u->input == X45_IN_NETWORK) {
			enter_screen(u, SCR_BROWSE);
			browse_load(u, "@fav", NULL);
		} else {
			favourite_save(u);
		}
		break;
	case IR_BRIGHT:
		bl_cycle(u);
		break;
	default:
		if (ev->code >= IR_NUM1 && ev->code <= IR_NUM9)
			seek_tenth(u, ev->code - IR_NUM0);
		else if (ev->code == IR_NUM0)
			seek_tenth(u, 0);
		break;
	}
}

/* A held button: the volume keys and the arrows go on, nothing else (a held NEXT must not skip ten tracks) */
static void handle_ir_hold(struct ui *u, const struct x45_event *ev)
{
	switch (ev->code) {
	case IR_VOLUP:
	case IR_VOLDOWN:
	case IR_UP:
	case IR_DOWN:
		handle_ir(u, ev);
		break;
	case IR_LEFT:
	case IR_RIGHT:
		if (u->input == X45_IN_FMRADIO && u->screen == SCR_PLAYER)   /* sweeps the band */
			handle_ir(u, ev);
		else if (u->screen == SCR_EXT && ui_ext_wants_hold(u) && list_key_ok(u, ev))   /* a screen that goes on (the balance) */
			ui_ext_hold(u, ev->code);
		break;
	case IR_BKSP:   /* held in the search: deletes on, like the arrows go on */
		if (u->screen == SCR_EXT && list_key_ok(u, ev))
			handle_ir(u, ev);
		break;
	default:
		break;
	}
}

/* MENU steps through the screens that belong to the current input. Options and status are
 * not in the cycle: a 1 s hold of MENU opens the options, another 1 s there the status. */
static void menu_short(struct ui *u)
{
	enum screen nx = u->screen;
	int guard;

	/* every candidate is checked, so one skipped screen cannot land on the next one that
	 * does not belong to this input either */
	for (guard = 0; guard < SCR_COUNT; guard++) {
		nx = (enum screen)((nx + 1) % SCR_COUNT);
		if (menu_screen_ok(u, nx))
			break;
	}
	if (guard == SCR_COUNT)
		nx = SCR_PLAYER;
	enter_screen(u, nx);
}


/* a main-loop stage that takes long delays everything behind it (key holds included): say so */
#define STAGE(name, call) do { long t0_ = now_ms(), d_; call; d_ = now_ms() - t0_; \
	if (d_ > 150) fprintf(stderr, "x45ui: slow %s %ld ms\n", name, d_); } while (0)

/* the input list: a highlighted input other than the current one is chosen after IN_AUTO_MS idle */
static void input_auto_poll(struct ui *u)
{
	static long last_draw;
	long now = now_ms();

	if (u->screen != SCR_INPUTS || !u->in_auto_s || !u->in_t || in_order[u->sel] == u->input)
		return;
	if (now - u->in_t >= IN_AUTO_MS(u)) {
		u->in_t = 0;
		input_select(u, in_order[u->sel]);
		return;
	}
	if (now - last_draw >= 60) {         /* animate the countdown strip */
		last_draw = now;
		u->dirty = 1;
	}
}

static void menu_hold_poll(struct ui *u)
{
	long now = now_ms();

	if (!u->menu_down || now - u->menu_down < MENU_HOLD_MS)
		return;
	u->menu_fired = 1;
	u->menu_down = now;   /* holding on re-arms: the next second goes one level further */
	enter_screen(u, u->screen == SCR_OPTIONS || u->screen == SCR_OPTCAT ? SCR_MAIN : SCR_OPTIONS);
}

/* the disc comes out (slot drive, no tray); one that plays is stopped first */
static void cd_eject(struct ui *u)
{
	if (cd_active(u))
		cd_stop(u);
	if (system(X45CD " eject") == 0)
		say(u, "disc ejected");
	else
		say(u, "eject failed");
	u->cd_ntracks = 0;
	u->dirty = 1;
}

/* a short press of STOP */
static void stop_press(struct ui *u)
{
	if (u->screen == SCR_CD && !cd_active(u)) {
		/* nothing is playing: STOP on the CD screen ejects the disc */
		cd_eject(u);
		return;
	}
	mpd_do(u, "stop");
}

static void handle(struct ui *u, const struct x45_event *ev)
{
	wake(u);
	u->resume_name[0] = '\0';   /* the owner is doing something: no surprise station */
	if (ev->kind == X45_EV_ENCODER) {
		if (ev->code == X45_KNOB_VOLUME) {
			if (!VOL_LOCKED(u)) {
				volume_touch(u);
				u->volume += ev->ticks;
				apply_volume(u);
			}
		} else if (u->screen == SCR_OPTCAT && u->bl_edit) {
			bl_adjust(u, ev->ticks);
		} else if (u->screen == SCR_PLAYER) {
			if (u->input == X45_IN_FMRADIO)
				fm_knob(u, ev->ticks);
			else
				rb_knob(u, ev->ticks);
		} else {
			move_sel(u, ev->ticks);
		}
		return;
	}
	if (ev->kind == X45_EV_IR) {
		if (ev->pressed == 1) {
			long now = now_ms();

			/* the same button again within LIST_BOUNCE_MS is a bounce for the list keys; the hold clock keeps the first press */
			u->ir_bounce = ev->code == u->ir_code && now - u->ir_down < LIST_BOUNCE_MS;
			if (!u->ir_bounce) {
				u->ir_code = ev->code;
				u->ir_down = now;
			}
			handle_ir(u, ev);
		}
		else if (ev->pressed == 2)
			handle_ir_hold(u, ev);
		return;
	}
	if (ev->kind == X45_EV_KEY && ev->code == X45_KEY_MENU && !(ev->name && !strcmp(ev->name, "web"))) {
		if (ev->pressed) {
			if (!u->menu_down) {
				u->menu_down = now_ms();
				u->menu_fired = 0;
			}
		} else {
			if (u->menu_down && !u->menu_fired)
				menu_short(u);
			u->menu_down = 0;
		}
		return;
	}
	if (ev->kind == X45_EV_KEY && ev->code == X45_KEY_STOP && !(ev->name && !strcmp(ev->name, "web"))) {
		if (ev->pressed) {
			if (!u->stop_down) {
				u->stop_down = now_ms();
				u->stop_fired = 0;
			}
		} else {
			if (u->stop_down && !u->stop_fired)
				stop_press(u);
			u->stop_down = 0;
		}
		return;
	}
	if (ev->kind != X45_EV_KEY || !ev->pressed)
		return;

	switch (ev->code) {
	case X45_KEY_MENU:   /* a press that is not a hold: handled on release, see above */
		menu_short(u);
		break;
	case X45_KEY_RETURN:
		go_back(u, 1);
		break;
	case X45_KEY_INPUT:
		key_input(u);
		break;
	case X45_KEY_OK:
		on_ok(u);
		break;
	case X45_KEY_VOL_PUSH:
		set_mute(u, !u->muted);
		break;
	case X45_KEY_STOP:
		stop_press(u);
		break;
	case X45_KEY_POWER:
		power_press(u, ev->name && !strcmp(ev->name, "web") ? "web" : "panel");
		break;
	default:
		break;
	}
}

/* ---------------------------------------------------------------- web bridge --- */
/* x45web (port 80) drives this UI rather than the hardware, so the browser and any
 * app can do exactly what the panel does and nothing fights over the devices:
 *   commands  <- /tmp/x45ui.cmd   one per line, picked up by renaming the file
 *   state     -> /tmp/x45ui.json  rewritten about once a second and on every change
 * Commands (see docs/X45-API.md, web_command() below has the exact lists):
 *   key <name>   ir <name>   knob <vol|scroll> <ticks>   screen <n>   ext <name>   sel <n>
 *   vol <0-100>   balance <-24..24>   input <n>   bl <n>   trim <input#> <0-21>   out <lineout|xlr> <0|1>
 *   opt volume <fixed|variable>   mpd <command>   lib <add|play|update|up> [path]
 *   tuner <command>   radio <add|play> <name> | <url>   rec toggle   bt <command>   cd eject
 *   the look: skin theme ground ink vis bars gonlook goncol gonbars gonbig */
#define WEB_CMD "/tmp/x45ui.cmd"
#define WEB_STATE "/tmp/x45ui.json"

static void jstr(FILE *f, const char *s)
{
	fputc('"', f);
	for (; *s; s++) {
		unsigned char c = (unsigned char)*s;

		if (c == '"' || c == '\\')
			fprintf(f, "\\%c", c);
		else if (c < 32)
			fprintf(f, "\\u%04x", c);
		else
			fputc(c, f);
	}
	fputc('"', f);
}

static void web_event(struct ui *u, int kind, int code, int ticks)
{
	struct x45_event ev;

	memset(&ev, 0, sizeof ev);
	ev.kind = kind;
	ev.code = code;
	ev.pressed = 1;
	ev.ticks = ticks;
	ev.name = "web";
	handle(u, &ev);
}

/* MPD argument quoting: "path with \"quotes\"" */
static void mpd_quote(char *out, size_t cap, const char *s)
{
	size_t o = 0;

	if (cap < 3) {
		if (cap)
			out[0] = '\0';
		return;
	}
	out[o++] = '"';
	for (; *s && o + 3 < cap; s++) {
		if (*s == '"' || *s == '\\')
			out[o++] = '\\';
		out[o++] = *s;
	}
	out[o++] = '"';
	out[o] = '\0';
}

static void web_command(struct ui *u, char *line)
{
	char a[24] = "", b[1100] = "";
	int n = 0;

	line[strcspn(line, "\r\n")] = '\0';
	if (sscanf(line, "%23s %1099[^\n]", a, b) < 1)
		return;
	if (strcmp(a, "lib") || (strncmp(b, "update", 6) && strncmp(b, "gone", 4)))   /* what the web file manager reports after a change is nobody's touch */
		wake(u);
	n = atoi(b);
	if (!strcmp(a, "key")) {
		static const struct { const char *n; int c; } k[] = {
			{ "menu", X45_KEY_MENU }, { "return", X45_KEY_RETURN },
			{ "input", X45_KEY_INPUT }, { "ok", X45_KEY_OK },
			{ "stop", X45_KEY_STOP }, { "power", X45_KEY_POWER },
			{ "mute", X45_KEY_VOL_PUSH },
		};
		unsigned i;

		/* what holding the key for a second does (the web has no way to hold a key) */
		if (!strcmp(b, "stophold")) {
			if (u->rec_pid > 0)
				rec_stop(u);
			else
				stop_hold_fire(u);
		} else if (!strcmp(b, "menuhold")) {
			enter_screen(u, u->screen == SCR_OPTIONS || u->screen == SCR_OPTCAT ? SCR_MAIN : SCR_OPTIONS);
		}
		for (i = 0; i < sizeof k / sizeof *k; i++)
			if (!strcmp(b, k[i].n))
				web_event(u, X45_EV_KEY, k[i].c, 0);
	} else if (!strcmp(a, "ir")) {
		static const struct { const char *n; int c; } k[] = {
			{ "up", IR_UP }, { "down", IR_DOWN }, { "left", IR_LEFT },
			{ "right", IR_RIGHT }, { "enter", IR_ENTER }, { "esc", IR_ESC },
			{ "home", IR_HOME }, { "next", IR_NEXT }, { "prev", IR_PREV },
			{ "ffwd", IR_FFWD }, { "rew", IR_REWIND }, { "shuffle", IR_SHUFFLE },
			{ "rec", IR_REC }, { "menu", IR_MENU }, { "setup", IR_SETUP }, { "info", IR_INFO },
			{ "mute", IR_MUTE }, { "input", IR_INPUT }, { "radio", IR_RADIO }, { "playlist", IR_PLAYLIST },
			{ "play", IR_PLAYPAUSE }, { "stop", IR_STOP }, { "volup", IR_VOLUP }, { "voldown", IR_VOLDOWN },
			{ "nowplaying", IR_NOWPLAY }, { "favourite", IR_FAVOURITE }, { "bookmark", IR_BOOKMARK }, { "eject", IR_EJECT },
			{ "iservice", IR_ISERVICE }, { "search", IR_SEARCH }, { "backspace", IR_BKSP }, { "capnum", 373 },
			{ "balance", IR_BALANCE },
			{ "0", IR_NUM0 }, { "1", IR_NUM0 + 1 }, { "2", IR_NUM0 + 2 }, { "3", IR_NUM0 + 3 }, { "4", IR_NUM0 + 4 },
			{ "5", IR_NUM0 + 5 }, { "6", IR_NUM0 + 6 }, { "7", IR_NUM0 + 7 }, { "8", IR_NUM0 + 8 }, { "9", IR_NUM0 + 9 },
		};
		unsigned i;

		for (i = 0; i < sizeof k / sizeof *k; i++)
			if (!strcmp(b, k[i].n))
				web_event(u, X45_EV_IR, k[i].c, 0);
	} else if (!strcmp(a, "knob")) {
		char w[12] = "";
		int t = 0;

		if (sscanf(b, "%11s %d", w, &t) == 2 && t)
			web_event(u, X45_EV_ENCODER, !strcmp(w, "vol") ? X45_KNOB_VOLUME : X45_KNOB_SCROLL, t);
	} else if (!strcmp(a, "screen")) {
		if (n >= 0 && n < SCR_COUNT && n != SCR_EXT)
			enter_screen(u, (enum screen)n);
	} else if (!strcmp(a, "ext")) {   /* open a screen of ext/ by name */
		ui_ext_open_name(u, b);
	} else if (!strcmp(a, "skin")) {  /* the look, by name; kept */
		int i = skin_find(b);

		if (i >= 0) {
			skin_choose(u, b);
			save_config_key("skin", skin_at(i)->name);
		}
	} else if (!strcmp(a, "theme")) {  /* the accent colour, by number; kept */
		if (n >= 0 && n < colour_count(COL_ACCENT)) {
			u->theme_id = n;
			theme_apply(u);
			save_int_key("theme", n);
		}
	} else if (!strcmp(a, "ground")) {  /* the background, by number (0: the skin's own); kept */
		if (n >= 0 && n < colour_count(COL_GROUND)) {
			u->ground_id = n;
			theme_apply(u);
			save_int_key("ground", n);
		}
	} else if (!strcmp(a, "vis")) {  /* a visual on or off by name, "none" clears them; for this input or all, as Options says */
		static const char *const old_names[] = { "led", "mirror", "area" };   /* styles of the spectrum 1, 2, 3 now */
		int i;

		for (i = 0; i < 3; i++)
			if (!strcmp(b, old_names[i])) {   /* an older script: the spectrum, in that style */
				u->spec_style = i + 1;
				save_int_key("spec_style", u->spec_style);
				snprintf(b, sizeof b, "spectrum");
			}
		for (i = 0; i < vis_count(); i++)
			if (!strcmp(b, "none") ? (vis_get(u) & vis_at(i)->bit) != 0 : !strcmp(b, vis_at(i)->name))
				vissub_toggle(u, i);
	} else if (!strcmp(a, "specstyle")) {  /* the spectrum's style, by number; kept */
		if (n >= 0 && n < vis_spec_styles()) {
			u->spec_style = n;
			save_int_key("spec_style", n);
			u->dirty = 1;
		}
	} else if (!strcmp(a, "bars")) {  /* the bar colours of the visuals, by number; kept */
		if (n >= 0 && n < vis_bar_colours()) {
			u->spec_col = n;
			save_int_key("spec_color", n);
			u->dirty = 1;
		}
	} else if (!strcmp(a, "gonlook")) {  /* the stereo field's look, by number; kept */
		if (n >= 0 && n < vis_gonio_looks()) {
			u->gon_look = n;
			save_int_key("gon_look", n);
			u->dirty = 1;
		}
	} else if (!strcmp(a, "goncol")) {  /* the colour of its trace, by number; kept */
		if (n >= 0 && n < vis_gonio_colours()) {
			u->gon_col = n;
			save_int_key("gon_color", n);
			u->dirty = 1;
		}
	} else if (!strcmp(a, "gonbars")) {  /* its correlation and balance bars: 1 shown, 0 left out; kept */
		u->gon_nobars = n == 0;
		save_int_key("gon_nobars", u->gon_nobars);
		if (!u->gon_nobars && u->gon_big) {
			u->gon_big = 0;
			save_int_key("gon_big", 0);
		}
		u->dirty = 1;
	} else if (!strcmp(a, "gonbig")) {  /* the stereo field almost fullscreen, text smaller beside it (bars go): 1 on, 0 off; kept */
		u->gon_big = n != 0;
		save_int_key("gon_big", u->gon_big);
		if (u->gon_big && !u->gon_nobars) {
			u->gon_nobars = 1;
			save_int_key("gon_nobars", 1);
		}
		u->dirty = 1;
	} else if (!strcmp(a, "ink")) {  /* the text colour, by number (0: the skin's own); kept */
		if (n >= 0 && n < colour_count(COL_INK)) {
			u->ink_id = n;
			theme_apply(u);
			save_int_key("ink", n);
		}
	} else if (!strcmp(a, "sel")) {
		int len = list_len(u);

		if (n >= 0 && n < len) {
			int *cur = u->screen == SCR_BROWSE ? &u->bsel :
				   u->screen == SCR_RADIO ? &u->rsel :
				   u->screen == SCR_CD ? &u->cd_sel :
				   u->screen == SCR_TUNER ? &u->tl_sel : &u->sel;

			*cur = n;
			u->dirty = 1;
		}
	} else if (!strcmp(a, "vol")) {
		if (VOL_LOCKED(u))
			return;   /* fixed outputs: the level is not changed */
		u->volume = n < 0 ? 0 : n > 100 ? 100 : n;
		volume_touch(u);
		apply_volume(u);
	} else if (!strcmp(a, "balance")) {   /* -24..24, kept; "balance 0" centres */
		if (b[0] == '-' || b[0] == '+' || (b[0] >= '0' && b[0] <= '9'))
			ui_set_balance(u, n, 1);
	} else if (!strcmp(a, "input")) {
		if (n >= 0 && n < NUM_INPUTS && (b[0] >= '0' && b[0] <= '9')) {
			if (u->ret_screen == SCR_INPUTS)
				u->ret_screen = SCR_PLAYER;
			u->screen = SCR_INPUTS;
			input_select(u, n);
		}
	} else if (!strcmp(a, "bl")) {
		char v[16];

		bl_set(u, n);
		snprintf(v, sizeof v, "%d", u->bright);
		save_config_key("brightness", v);
	} else if (!strcmp(a, "trim")) {
		int in = -1, idx = 0;

		if (sscanf(b, "%d %d", &in, &idx) != 2)
			in = -1;
		if (in >= 0)
			set_trim(u, in, idx);
	} else if (!strcmp(a, "out")) {
		char w[12] = "";
		int on = 0;

		if (sscanf(b, "%11s %d", w, &on) == 2) {
			if (!strcmp(w, "lineout"))
				set_out(u, 0, on != 0);
			else if (!strcmp(w, "xlr"))
				set_out(u, 1, on != 0);
		}
	} else if (!strcmp(a, "opt")) {
		if (!strcmp(b, "volume fixed") || !strcmp(b, "volume variable")) {
			set_volume_mode(u, !strcmp(b, "volume fixed"));
		}
	} else if (!strcmp(a, "ripfmt")) {   /* the file format of CD rips, by name; kept */
		int i;

		for (i = 0; i < NRIP_FMT; i++)
			if (!strcmp(b, rip_fmt_arg[i])) {
				u->rip_fmt = i;
				save_config_key("rip_format", rip_fmt_arg[i]);
				u->dirty = 1;
			}
	} else if (!strcmp(a, "mpd")) {
		static const char *const ok[] = {
			"play", "pause", "stop", "next", "previous", "random 0", "random 1",
			"repeat 0", "repeat 1", "single 0", "single 1", "consume 0", "consume 1",
			"update", "clear",
		};
		unsigned i;
		int pos;

		for (i = 0; i < sizeof ok / sizeof *ok; i++)
			if (!strcmp(b, ok[i]))
				mpd_do(u, b);
		if (sscanf(b, "seekcur %d", &pos) == 1 || sscanf(b, "seekcur +%d", &pos) == 1 ||
		    sscanf(b, "seekcur -%d", &pos) == 1)
			mpd_do(u, b);
		else if (sscanf(b, "play %d", &pos) == 1 && pos >= 0)
			mpd_do(u, b);
		else if (sscanf(b, "deleteid %d", &pos) == 1 || sscanf(b, "delete %d", &pos) == 1)
			mpd_do(u, b);
	} else if (!strcmp(a, "lib")) {
		char verb[12] = "", path[236] = "", q[300], cmd[340];

		if (sscanf(b, "%11s %235[^\n]", verb, path) >= 1) {
			if (path_has_dotdot(path) || strlen(path) >= sizeof path - 1)
				return;   /* (a path that filled the buffer was cut short: it would name another folder) */
			mpd_quote(q, sizeof q, path);
			if (!strcmp(verb, "update")) {
				snprintf(cmd, sizeof cmd, "update %s", path[0] ? q : "");
				mpd_do(u, cmd);
				lib_rescan(u);
			} else if (!strcmp(verb, "gone") && path[0]) {
				lib_gone(u, path);
			} else if (!strcmp(verb, "up")) {
				/* one folder up in the list the web page shows, whatever the panel is showing (RETURN opens
				 * the visuals menu on the player screen) */
				if (u->dir[0])
					browse_up(u);
			} else if (!strcmp(verb, "add") && path[0]) {
				snprintf(cmd, sizeof cmd, "add %s", q);
				mpd_do(u, cmd);
				say(u, "added to the queue");
			} else if (!strcmp(verb, "play") && path[0]) {
				if (u->input != X45_IN_NETWORK)
					input_select(u, X45_IN_NETWORK);
				mpd_do(u, "clear");
				snprintf(cmd, sizeof cmd, "add %s", q);
				mpd_do(u, cmd);
				mpd_do(u, "play");
			}
		}
	} else if (!strcmp(a, "tuner")) {
		static const char *const verbs[] = { "tune ", "step ", "seek ", "fmscan", "fmadd", "fmdel ",
						     "scan", "select ", "cancel" };
		unsigned i;

		for (i = 0; i < sizeof verbs / sizeof *verbs; i++)
			if (!strncmp(b, verbs[i], strlen(verbs[i])) && tuner_input(u) && u->tu_pid > 0) {
				tuner_cmd(u, "%s", b);
				break;
			}
	} else if (!strcmp(a, "radio")) {
		/* radio add|play <name> | <url>: stations found in a web directory */
		char verb[8] = "", name[96] = "", url[600] = "";
		char *bar = strchr(b, '|');
		size_t i;

		if (sscanf(b, "%7s", verb) == 1 && bar) {
			snprintf(name, sizeof name, "%.*s", (int)(bar - b - strlen(verb)) > 95 ? 95 : (int)(bar - b - strlen(verb)), b + strlen(verb));
			snprintf(url, sizeof url, "%s", bar + 1);
			if (strlen(bar + 1) >= sizeof url)
				url[0] = '\0';   /* cut short it would be another station's address: refused below */
			for (i = 0; name[i]; i++)
				if ((unsigned char)name[i] < 32 || name[i] == '|' || name[i] == '"')
					name[i] = ' ';
			{
				char *s = name, *e;

				while (*s == ' ')
					s++;
				memmove(name, s, strlen(s) + 1);
				e = name + strlen(name);
				while (e > name && e[-1] == ' ')
					*--e = '\0';
			}
			{
				char *s = url;

				while (*s == ' ')
					s++;
				memmove(url, s, strlen(s) + 1);
			}
			for (i = 0; url[i]; i++)
				if ((unsigned char)url[i] <= 32 || url[i] == '"' || url[i] == '\\')
					break;
			if (url[i] || (strncmp(url, "http://", 7) && strncmp(url, "https://", 8)) || !name[0]) {
				say(u, "Radio: bad name or address");
			} else if (!strcmp(verb, "add")) {
				FILE *sf = fopen(u->stations_path, "a");

				if (sf) {
					fprintf(sf, "%s | %s\n", name, url);
					fclose(sf);
					radio_load(u);
					say(u, "station added");
				}
			} else if (!strcmp(verb, "play")) {
				if (u->input != X45_IN_NETWORK)
					input_select(u, X45_IN_NETWORK);
				u->ctx_n = 0;   /* not chosen from a folder of the panel */
				radio_play(u, url, name);
			}
		}
	} else if (!strcmp(a, "rec")) {
		rec_toggle(u);
	} else if (!strcmp(a, "bt")) {
		if (u->input == UI_IN_BT) {
			if (!strcmp(b, "pair"))
				bt_cmd(u, 'D');
			else if (!strcmp(b, "forget"))
				bt_cmd(u, 'F');
			else if (!strcmp(b, "yes"))
				bt_cmd(u, 'y');
			else if (!strcmp(b, "no"))
				bt_cmd(u, 'n');
		}
	} else if (!strcmp(a, "cd")) {
		if (!strcmp(b, "eject") && u->input == UI_IN_CD) {
			cd_stop(u);
			if (system(X45CD " eject") == 0)
				say(u, "disc ejected");
			u->cd_ntracks = 0;
		}
	}
	u->dirty = 1;
}

static void web_poll(struct ui *u)
{
	static long last_state;
	long now = now_ms();
	char line[1200];
	FILE *f;

	if (rename(WEB_CMD, WEB_CMD ".run") == 0) {
		f = fopen(WEB_CMD ".run", "r");
		if (f) {
			while (fgets(line, sizeof line, f))
				web_command(u, line);
			fclose(f);
		}
		unlink(WEB_CMD ".run");
		last_state = 0;
	}
	if (!u->dirty && now - last_state < 1000)
		return;
	if (now - last_state < 250)
		return;
	last_state = now;

	f = fopen(WEB_STATE ".tmp", "w");
	if (!f)
		return;
	{
		static const char *const scr[SCR_COUNT] = { "player", "library", "radio", "cd", "stations",
							     "status", "input", "options", "visuals", "visualizers", "confirm", "optcat", "ext" };
		static const char *const inputs[] = { "network", "fm", "pc usb", "aes ebu", "coaxial",
						       "optical", "line in", "aux in", "phono", "cd", "dab", "bluetooth" };
		int i;

		fprintf(f, "{\"screen\":%d,\"screen_name\":\"%s\",\"input\":%d,\"input_name\":\"%s\","
			"\"volume\":%d,\"balance\":%d,\"balance_max\":%d,\"muted\":%d,\"volume_hidden\":%d,\"status\":", u->screen, scr[u->screen], u->input,
			inputs[u->input >= 0 && u->input < NUM_INPUTS ? u->input : 0], u->volume, u->balance, X45_BALANCE_MAX, u->muted,
			u->vol_fixed && !u->hp_present);
		jstr(f, u->status);
		fprintf(f, ",\"brightness\":%d,\"brightness_max\":%d,\"air\":%d", bl_get(), bl_max(), u->air);
		fprintf(f, ",\"meter\":{\"source\":\"%s\",\"rate\":%u,\"why\":", meter_wanted(u) ? "capture" :
			u->sp_mode == 1 ? "radio" : u->sp_mode == 2 ? "mpd" : "none",
			meter_wanted(u) ? 192000u : u->sp_rate);
		jstr(f, u->sp_why ? u->sp_why : "");
		fputc('}', f);
		fprintf(f, ",\"hw\":{\"headphone\":%d,\"pcusb_cable\":%d,\"pcusb_selected\":%d,\"spdif_rate\":%d}",
			(int)u->hp_present, (int)u->pcusb, u->usbpc_pc, (int)u->cs_rate);
		fprintf(f, ",\"options\":{\"volume_fixed\":%d,\"lineout\":%d,\"xlr\":%d,\"trim\":{\"line in\":%d,\"aux in\":%d,\"phono\":%d},\"trim_range\":[-12,9],\"rip_format\":\"%s\",\"rip_formats\":[",
			u->vol_fixed, u->out_lineout, u->out_xlr, u->trim[X45_IN_LINEIN] - 12,
			u->trim[X45_IN_AUXIN] - 12, u->trim[X45_IN_PHONO] - 12, rip_fmt_arg[u->rip_fmt]);
		for (int k = 0; k < NRIP_FMT; k++)
			fprintf(f, "%s{\"name\":\"%s\",\"label\":\"%s\"}", k ? "," : "", rip_fmt_arg[k], rip_fmt_name[k]);
		fputs("]}", f);
		fprintf(f, ",\"look\":{\"skin\":\"%s\",\"skins\":[", skin_at(u->skin_id)->name);
		for (int k = 0; k < skin_count(); k++)
			fprintf(f, "%s{\"name\":\"%s\",\"label\":\"%s\",\"about\":\"%s\"}", k ? "," : "", skin_at(k)->name, skin_at(k)->label,
				skin_at(k)->about ? skin_at(k)->about : "");
		fprintf(f, "],\"theme\":%d,\"ground\":%d,\"ink\":%d", u->theme_id, u->ground_id, u->ink_id);
		for (int kind = 0; kind < 3; kind++) {
			static const char *const key[3] = { "themes", "grounds", "inks" };

			fprintf(f, ",\"%s\":[", key[kind]);
			for (int k = 0; k < colour_count(kind); k++) {
				uint8_t c[3];

				colour_swatch(kind, k, c);
				fprintf(f, "%s{\"name\":\"%s\",\"color\":", k ? "," : "", colour_name(kind, k));
				if (kind && !k)   /* the skin's own: no colour of its own to show */
					fputs("\"\"}", f);
				else
					fprintf(f, "\"#%02x%02x%02x\"}", c[0], c[1], c[2]);
			}
			fputc(']', f);
		}
		fputs(",\"vis\":[", f);   /* the visuals (at most `max_vis` on at once) and the bar colours */
		for (int k = 0; k < vis_count(); k++)
			fprintf(f, "%s{\"name\":\"%s\",\"label\":\"%s\",\"on\":%d}", k ? "," : "", vis_at(k)->name, vis_at(k)->label,
				(vis_get(u) & vis_at(k)->bit) != 0);
		/* `looks`: the VIS_LOOK_* options of the visuals that are on; the Look tab shows only those */
		fprintf(f, "],\"max_vis\":%d,\"looks\":%u,\"style\":%d,\"styles\":[", VIS_MAX_ON, vis_looks(u), u->spec_style);
		for (int k = 0; k < vis_spec_styles(); k++)
			fprintf(f, "%s\"%s\"", k ? "," : "", vis_spec_style_name(k));
		fprintf(f, "],\"bars\":%d,\"bar_names\":[", u->spec_col);
		for (int k = 0; k < vis_bar_colours(); k++)
			fprintf(f, "%s\"%s\"", k ? "," : "", vis_bar_colour_name(k));
		fprintf(f, "],\"gon\":{\"look\":%d,\"colour\":%d,\"bars\":%d,\"big\":%d,\"looks\":[", u->gon_look, u->gon_col, !u->gon_nobars, u->gon_big);
		for (int k = 0; k < vis_gonio_looks(); k++)
			fprintf(f, "%s\"%s\"", k ? "," : "", vis_gonio_look_name(k));
		fputs("],\"colours\":[", f);
		for (int k = 0; k < vis_gonio_colours(); k++)
			fprintf(f, "%s\"%s\"", k ? "," : "", vis_gonio_colour_name(k));
		fputs("]}", f);
		fputc('}', f);
		fprintf(f, ",\"mpd\":{\"state\":\"%s\",\"elapsed\":%.1f,\"duration\":%.1f,\"random\":%d,\"repeat\":%d,\"single\":%d,\"consume\":%d,\"song\":%d,\"length\":%d,\"bitrate\":%d,\"audio\":",
			u->st.state, u->st.elapsed, u->st.duration, u->st.random, u->st.repeat, u->st.single, u->st.consume,
			u->st.song, u->st.length, u->st.bitrate);
		jstr(f, u->st.audio);
		fputs(",\"title\":", f); jstr(f, u->have_song ? u->song.title : "");
		fputs(",\"artist\":", f); jstr(f, u->have_song ? u->song.artist : "");
		fputs(",\"album\":", f); jstr(f, u->have_song ? u->song.album : "");
		fputs(",\"name\":", f); jstr(f, u->have_song ? u->song.name : "");
		fputs(",\"file\":", f); jstr(f, u->have_song ? u->song.file : "");
		fputs(",\"error\":", f); jstr(f, u->st.error);
		fputs("}", f);
		fprintf(f, ",\"rec\":{\"active\":%d,\"cd\":%d,\"secs\":%ld,\"track\":%d,\"tracks\":%d}",
			u->rec_pid > 0, u->rec_cd, u->rec_secs, u->rec_track, u->rec_tracks);
		fprintf(f, ",\"cd\":{\"active\":%d,\"playing\":%d,\"track\":%d,\"pos\":%d,\"len\":%d,\"sel\":%d,"
			"\"hdcd\":%d,\"hdcd_pe\":%d,\"hdcd_gain\":%d,\"album\":",
			u->cd_pid > 0, u->cd_playing, u->cd_track, u->cd_pos, u->cd_tlen, u->cd_sel,
			u->cd_hdcd, u->cd_hdcd_pe, u->cd_hdcd_gain);
		jstr(f, u->cd_album);
		fputs(",\"err\":", f); jstr(f, u->cd_err);
		fputs(",\"tracks\":[", f);
		for (i = 0; i < u->cd_ntracks && i < 99; i++) {
			fprintf(f, "%s{\"n\":%d,\"len\":%d,\"title\":", i ? "," : "", i + 1, u->cd_len[i]);
			jstr(f, u->cd_title[i]);
			fputc('}', f);
		}
		fputs("]}", f);
		fprintf(f, ",\"tuner\":{\"running\":%d,\"dab\":%d,\"state\":", u->tu_pid > 0, u->tu_mode);
		jstr(f, u->tu_state);
		fprintf(f, ",\"freq\":%d,\"rssi\":%d,\"snr\":%d,\"quality\":%d,\"strength\":%d,\"found\":%d,\"sel\":%d,\"nsvc\":%d,\"slide\":%d,\"fmscan\":%d,\"ps\":",
			u->tu_freq, u->tu_rssi, u->tu_snr, u->tu_q, u->tu_sq, u->tu_found, u->tu_sel, u->tu_nsvc, u->tu_slide, u->tu_fmscan);
		jstr(f, u->tu_ps);
		fputs(",\"service\":", f); jstr(f, u->tu_svc);
		fputs(",\"ensemble\":", f); jstr(f, u->tu_ens);
		fputs(",\"text\":", f); jstr(f, u->tu_text);
		fputs(",\"rt\":", f); jstr(f, u->tu_rt);
		fputs(",\"time\":", f); jstr(f, u->tu_time);
		fputs(",\"list\":[", f);
		for (i = 0; i < u->tl_n; i++) {
			fprintf(f, "%s{\"v\":%d,\"name\":", i ? "," : "", u->tl_val[i]);
			jstr(f, u->tl_name[i]);
			fputc('}', f);
		}
		fprintf(f, "],\"list_sel\":%d}", u->tl_sel);
		fputs(",\"inputs\":[", f);
		for (i = 0; i < NUM_INPUTS; i++) {
			fprintf(f, "%s", i ? "," : "");
			jstr(f, inputs[i]);
		}
		fprintf(f, "],\"bt\":{\"active\":%d,\"state\":", u->bt_pid > 0);
		jstr(f, u->bt_state);
		fputs(",\"name\":", f); jstr(f, u->bt_name);
		fputs(",\"confirm\":", f); jstr(f, u->bt_confirm);
		fputs(",\"confirm_name\":", f); jstr(f, u->bt_confirm_name);
		fprintf(f, ",\"bonds\":%d,\"pairing_open\":%d,\"rate\":%d,\"playing\":%d,\"title\":", u->bt_bonds, u->bt_open, u->bt_rate, u->bt_avrcp_play);
		jstr(f, u->bt_title);
		fputs(",\"artist\":", f); jstr(f, u->bt_artist);
		fputs(",\"album\":", f); jstr(f, u->bt_album);
		fputs(",\"error\":", f); jstr(f, u->bt_err);
		fputs("},\"stations\":[", f);
		for (i = 0; i < u->nstations; i++) {
			fprintf(f, "%s", i ? "," : "");
			jstr(f, u->stations[i].name);
		}
		fprintf(f, "],\"station_sel\":%d,\"dir\":", u->rsel);
		jstr(f, u->dir);
		{
			char lab[MPDC_PATH_MAX + 96];

			browse_label(u, lab, sizeof lab);
			fputs(",\"dir_label\":", f);
			jstr(f, lab);
		}
		fprintf(f, ",\"browse_sel\":%d,\"browse\":[", u->bsel);
		for (i = 0; u->rows && i < u->nrows && i < 400; i++) {
			fprintf(f, "%s{\"dir\":%d,\"name\":", i ? "," : "", u->rows[i].is_dir);
			jstr(f, u->rows[i].name);
			fputs(",\"path\":", f);
			jstr(f, u->rows[i].path);
			fputc('}', f);
		}
		fprintf(f, "],\"sel\":%d}\n", u->sel);
	}
	if (fclose(f) == 0)
		rename(WEB_STATE ".tmp", WEB_STATE);
}

int main(int argc, char **argv)
{
	struct ui u;
	int r, i, keep = 0, once = 0, trace = 0, stopped_x20 = 0;
	long last_hw = 0;
	const char *start_screen = NULL, *start_dir = "", *stations_arg = NULL;
	int start_sel = 0, start_input = -1;
	char pidbuf[32];
	FILE *p;
	int x20pid = 0;

	memset(&u, 0, sizeof u);
	u.mpd_port = MPDC_DEFAULT_PORT;
	{
		int j;

		for (j = 0; j < NUM_INPUTS; j++)
			u.trim[j] = 12;   /* 0 dB */
		for (j = 0; j < NUM_INPUTS; j++) {
			u.vis[j].on = 3;   /* level bars and spectrum */
			u.vis[j].first = 1;
		}
		u.bt_wfd = -1;
	}
	u.cfg_input = -1;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--keep"))
			keep = 1;
		else if (!strcmp(argv[i], "--once"))
			once = 1;
		else if (!strcmp(argv[i], "--trace"))
			trace = 1;
		else if (!strcmp(argv[i], "--port") && i + 1 < argc)
			u.mpd_port = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--stations") && i + 1 < argc)
			stations_arg = argv[++i];
		else if (!strcmp(argv[i], "--screen") && i + 1 < argc)
			start_screen = argv[++i];
		else if (!strcmp(argv[i], "--dir") && i + 1 < argc)
			start_dir = argv[++i];
		else if (!strcmp(argv[i], "--sel") && i + 1 < argc)
			start_sel = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--input") && i + 1 < argc)
			start_input = atoi(argv[++i]); /* show this source, without switching */
		else {
			puts("usage: x45ui [--keep] [--once] [--trace] [--port N] [--stations FILE] "
			     "[--screen player|library|radio|status|input|outputs] "
			     "[--dir PATH] [--sel N] [--input N]");
			return 2;
		}
	}

	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);
	signal(SIGUSR1, on_usr1);
	signal(SIGPIPE, SIG_IGN);

	u.x45fd = -1;
	u.m_fd = -1;
	u.mpd.fd = -1;
	u.volume = 30;
	u.out_lineout = 1;
	u.out_xlr = 1;
	u.bands = 128;
	x45_font_smooth = x45_ttf_load("/home/root/x45new/fonts/ui.ttf") == 0 ? 2 : 1;   /* the font file is optional */
	u.in_icons = 1;
	u.in_auto_s = 3;
	u.rb_steps = 4;
	u.resume = 1;
	u.vis_shared.on = 3;   /* level bars and spectrum */
	u.vis_shared.first = 1;
	u.seen_pos = -2;
	u.screen = SCR_PLAYER;
	snprintf(u.stations_path, sizeof u.stations_path, "%s",
		 stations_arg ? stations_arg : "/home/root/x45new/etc/stations.txt");

	if ((r = x45_gfx_open(&u.g)) < 0) {
		fprintf(stderr, "x45ui: /dev/fb0: %s\n", strerror(-r));
		return 1;
	}
	u.t.bg     = x45_rgb(&u.g, 10, 12, 18);
	u.t.panel  = x45_rgb(&u.g, 22, 26, 36);
	u.t.line   = x45_rgb(&u.g, 60, 70, 90);
	u.t.text   = x45_rgb(&u.g, 232, 236, 244);
	u.t.dim    = x45_rgb(&u.g, 130, 142, 165);
	u.t.accent = x45_rgb(&u.g, 108, 198, 134);
	u.t.warn   = x45_rgb(&u.g, 226, 128, 96);
	u.t.barfg  = x45_rgb(&u.g, 108, 198, 134);
	u.t.barbg  = x45_rgb(&u.g, 32, 38, 50);

	if (x45_gfx_save(&u.g, SAVED_FRAME) < 0)
		fprintf(stderr, "x45ui: warning: could not save the current frame\n");

	/* borrow the panel from x20 */
	if (!keep) {
		p = popen("pidof x20", "r");
		if (p) {
			if (fgets(pidbuf, sizeof pidbuf, p))
				x20pid = atoi(pidbuf);
			pclose(p);
		}
		if (x20pid > 0 && kill(x20pid, SIGSTOP) == 0) {
			stopped_x20 = 1;
			fprintf(stderr, "x45ui: x20 (pid %d) paused\n", x20pid);
		}
	}

	u.x45fd = x45_open();
	if (u.x45fd < 0)
		fprintf(stderr, "x45ui: /dev/x45: %s (read-only UI)\n",
			strerror(-u.x45fd));
	if ((r = x45_input_open(&u.in)) < 0)
		fprintf(stderr, "x45ui: no input devices: %s\n", strerror(-r));
	load_config(&u, CONF_PATH);
	u.bal_live = 1;   /* the process before may have left the two sides apart: the first volume write puts them right */
	theme_apply(&u);
	u.act_t = u.start_t = now_ms();
	if (u.resume_name[0])
		u.resume_t = now_ms() + 4000;
	if (trace)
		fprintf(stderr, "x45ui: knob decode=%s invert=%d edges/detent=%d\n",
			u.in.decode == X45_DECODE_QUADRATURE ? "quadrature" : "direction",
			u.in.invert, u.in.ticks_per_detent);

	refresh_hw(&u);
	if (u.bright)
		bl_set(&u, u.bright);
	if (start_input >= 0)
		u.input = start_input;
	/* the driver comes up with both volume stages Fixed (0 = Variable), which
	 * makes every volume write a no-op; only mute worked */
	if (u.x45fd >= 0) {
		x45_set(u.x45fd, X45_SET_ANALOG_VOL_TYPE, u.vol_fixed, 0);
		x45_set(u.x45fd, X45_SET_DIGITAL_VOL_TYPE, u.vol_fixed, 0);
	}
	if (u.x45fd >= 0)
		x45_mute(u.x45fd, 0);   /* the flag says unmuted: a mute left by the process before must not outlive it */
	/* start from a known state: the label says network, so make the hardware
	 * match (a UI restart leaves the previous input selected in the chips) */
	if (u.x45fd >= 0 && start_input < 0 && u.input == X45_IN_NETWORK) {
		x45_input(u.x45fd, X45_IN_NETWORK);
		x45_outputs_apply(u.x45fd);
	}
	mpd_connect(&u);
	u.last_poll = 0;
	mpd_poll(&u);
	/* settings that outlive a restart: the volume and the outputs */
	if (u.cfg_vol > 0)
		u.volume = u.cfg_vol;
	if (u.x45fd >= 0) {
		x45_out_lineout(u.x45fd, u.out_lineout);
		x45_out_xlr(u.x45fd, u.out_xlr);
		x45_outputs_apply(u.x45fd);
	}
	if (u.cfg_vol > 0)
		apply_volume(&u);
	u.vol_dirty = 0;
	u.last_vol_change = 0;   /* the start-up volume write is not a change to show */
	if (u.x45fd >= 0 && u.volume >= 0 && u.volume <= 100) {
		x45_volume(u.x45fd, u.volume);
		u.bal_live = 1;   /* the master write set both sides alike */
		apply_balance(&u, VOL_LOCKED(&u) ? 100 : u.volume);
	}
	if (u.hp_present && u.volume > HP_VOLUME) { /* booted with headphones in */
		u.spk_volume = u.volume;
		u.volume = HP_VOLUME;
		apply_volume(&u);
	}

	/* the XMOS belongs on the unit's own USB port unless PC USB is selected below */
	u.usbpc_pc = 1;
	usbpc_select(&u, 0);
	if (u.x45fd >= 0 && start_input < 0 && u.resume && u.cfg_input > 0 && u.cfg_input < NUM_INPUTS)
		input_select(&u, u.cfg_input);   /* come back on the input that was in use */
	u.vol_dirty = 0;
	u.last_vol_change = 0;

	/* A stream decides the master-clock family when MPD opens the DAC; if that differs from what
	 * the boot left behind (the vendor start-up leaves FS_4844 at 1) the XMOS re-locks while the
	 * first notes play: about 1 s of distortion on the first stream after every reboot. 44.1 kHz is the
	 * common family, so park it there while idle. */
	if (u.input == X45_IN_NETWORK)
		x45_clock_select(44100);
	if (start_screen) {
		static const char *const names[SCR_COUNT] = {
			"player", "library", "radio", "cd", "stations", "status", "input", "options", "visuals", "visualizers", "confirm", "optcat", "ext"
		};
		for (i = 0; i < SCR_COUNT; i++)
			if (!strcmp(start_screen, names[i]))
				break;
		if (i == SCR_COUNT) {
			fprintf(stderr, "x45ui: unknown screen '%s'\n", start_screen);
		} else {
			snprintf(u.dir, sizeof u.dir, "%s", start_dir);
			enter_screen(&u, (enum screen)i);
			if (u.screen == SCR_BROWSE && start_sel > 0 && start_sel < u.nrows) {
				u.bsel = start_sel;
				u.top = start_sel > 8 ? start_sel - 8 : 0;
			}
		}
	}
	render(&u);

	if (once) {
		fprintf(stderr, "x45ui: --once, holding 5 s\n");
		for (i = 0; i < 500; i++) {
			STAGE("meter_poll", meter_poll(&u));
			if (u.dirty)
				STAGE("render", render(&u));
			usleep(10000);
		}
	} else {
		while (!stop_now) {
			struct x45_event ev;
			r = x45_input_next(&u.in, &ev, u.m_pid > 0 || u.sp_pid > 0 || u.vol_a > 0 ? 30 : 100);
			if (trace && r != 0) {
				fprintf(stderr, "input: r=%d kind=%d code=%d pressed=%d ticks=%d %s\n",
					r, (int)ev.kind, ev.code, ev.pressed, ev.ticks,
					ev.name ? ev.name : "");
				fflush(stderr);
			}
			if (r > 0 && ev.kind != X45_EV_NONE) {
				int more;

				STAGE("handle", handle(&u, &ev));
				/* A knob turned fast leaves a record per detent in the pipe, and one pass of this loop (the polls and a
				 * redraw of the list) takes a tenth of a second: taking one record per pass limited the scroll to about
				 * ten rows a second. Take what is waiting now, the screen is drawn once for all of it. */
				for (more = 0; more < 200 && x45_input_next(&u.in, &ev, 0) > 0; more++)
					if (ev.kind != X45_EV_NONE)
						handle(&u, &ev);
			}
			STAGE("mpd_poll", mpd_poll(&u));
			STAGE("meter_poll", meter_poll(&u));
			STAGE("cd_poll", cd_poll(&u));
			STAGE("bt_poll", bt_poll(&u));
			STAGE("ext_poll", ui_ext_poll(&u));
			STAGE("scr_poll", scr_poll(&u));
			STAGE("sys_poll", sys_poll(&u));
			STAGE("stop_hold", stop_hold_poll(&u));
			STAGE("resume", resume_poll(&u));
			STAGE("netres", net_poll(&u));
			STAGE("cd_watch", cd_watch(&u));
			STAGE("spec_poll", spec_poll(&u));
			STAGE("spec_read", spec_read(&u));
			vol_anim(&u);
			rb_poll(&u);
			scroll_poll(&u);
			input_auto_poll(&u);
			menu_hold_poll(&u);
			STAGE("rec_poll", rec_poll(&u));
			rec_note_poll(&u);
			settle_poll(&u);
			pcusb_clock_poll(&u);
			lib_poll(&u);
			if (u.vol_dirty && now_ms() - u.last_vol_change > 2000) {
				char v[16];
				int sv = u.hp_present && u.spk_volume > 0 ? u.spk_volume : u.volume;

				u.vol_dirty = 0;
				if (!u.hp_present || u.spk_volume > 0) {
					snprintf(v, sizeof v, "%d", sv);
					save_config_key("volume", v);
				}
			}
			STAGE("web_poll", web_poll(&u));
			STAGE("tuner_poll", tuner_poll(&u));
			{
				int air = access(AIRPLAY_MARKER, F_OK) == 0;

				if (air != u.air) {
					u.air = air;
					u.dirty = 1;
				}
			}
			{   /* a message on the screen is for a moment, not for good ("mpd: Connection reset" stayed for hours) */
				static char shown[sizeof u.status];
				static long shown_t;

				if (strcmp(shown, u.status)) {
					snprintf(shown, sizeof shown, "%s", u.status);
					shown_t = now_ms();
				} else if (u.status[0] && now_ms() - shown_t > STATUS_MS) {
					u.status[0] = '\0';
					shown[0] = '\0';
					u.dirty = 1;
				}
			}
			if (u.power_armed && now_ms() - u.power_armed > POWER_CONFIRM_MS) {
				u.power_armed = 0;
				power_log("-", "no second press: cancelled", POWER_CONFIRM_MS);
				say(&u, "power-off cancelled");
			}
			if (now_ms() - last_hw > 1000) {
				last_hw = now_ms();
				STAGE("refresh_hw", refresh_hw(&u));
				/* the XMOS must sit on the PC port while PC USB is the input */
				if (u.input == X45_IN_PCUSB && usbpc_read() == 1)
					usbpc_select(&u, 1);
			}
			if (u.dirty)
				render(&u);
			else if (u.dirty_vis)
				render_strip(&u);
			if (u.dirty)   /* the strip could not be drawn alone */
				render(&u);
		}
	}

	/* hand the panel back exactly as we found it */
	if (x45_gfx_restore(&u.g, SAVED_FRAME) < 0)
		fprintf(stderr, "x45ui: warning: could not restore the frame\n");
	unlink(SAVED_FRAME);
	if (u.scr_dark)
		wake(&u);   /* the panel goes back with its light on */
	if (stopped_x20)
		kill(x20pid, SIGCONT);
	if (u.settle_until) {   /* never leave the output muted behind */
		u.settle_until = 0;
		settle_hw(&u);
	}
	x45_input_close(&u.in);
	if (u.x45fd >= 0)
		x45_close(u.x45fd);
	rec_stop(&u);
	spec_stop(&u);
	meter_stop(&u);
	tuner_stop(&u);
	bt_stop(&u);
	mpdc_close(&u.mpd);
	free(u.rows);
	x45_gfx_close(&u.g);
	fprintf(stderr, "x45ui: panel restored%s\n",
		stopped_x20 ? ", x20 resumed" : "");
	if (restart_req && !u.quit_code)
		return 4;   /* x45stack starts the UI again, without counting a crash */
	return u.quit_code;
}
