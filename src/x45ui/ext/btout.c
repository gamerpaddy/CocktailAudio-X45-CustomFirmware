#define _GNU_SOURCE
/* ext/btout - Bluetooth output: play what MPD plays on a Bluetooth headset or speaker.
 *
 * The screen is a browser of devices: switch the output on, search, choose a device; a device that is not paired
 * yet has to be in pairing mode when it is chosen. The work is done by x45btsrc (started through x45btout-run, with
 * this file holding its stdin as the command channel), an A2DP source that reads the PCM of MPD's "bluetooth"
 * fifo output. While a device is connected MPD's own output (the DAC) is switched off and the Bluetooth one on;
 * when the link ends it is the other way round, so a headset that is switched off gives the music back to the
 * speakers. The radio stick is shared with the Bluetooth input (x45bt): only one of them can have it, the input
 * wins (ui_btout_release), and the output comes back when the input is left.
 *
 * Kept in etc/x45ui.conf:  btout = on|off  (the output is wanted)  and  btout_device = ADDR  (called again by
 * itself whenever it is in reach).
 *
 * Only what MPD plays goes to the headset. The tuner, CD, line inputs and the like have their own paths.
 */
#include "ext.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define BTOUT_RUN   "/home/root/x45new/bin/x45btout-run"
#define BTOUT_ST    "/tmp/x45btout.status"
#define BTOUT_LOG   "/tmp/x45btout.log"
#define CONF        "/home/root/x45new/etc/x45ui.conf"
#define MAXDEV      32
#define POLL_MS     250
#define ROUTE_MS    10000
#define BOOT_WAIT_MS 20000
#define RETRY_FAIL_MS 60000
#define RETRY_BT_MS   3000

extern const struct ui_ext ext_btout;

struct bdev {
	char addr[18], name[64];
	int known, conn, rssi, heard;
};

static struct {
	pid_t pid;
	int wfd;
	long t_poll, t_start;
	unsigned seq;
	char state[16], target[18], name[64], err[96], autod[18];
	int connected, streaming, scanning;
	unsigned underruns, slips;
	int battery;                /* the headset's charge in percent, -1 unknown */
	int hsvol;                  /* the level the headset reported on its own keys, 0..100, -1 none */
	unsigned hsseq;             /* counts those reports */
	struct bdev d[MAXDEV];
	int nd;
} B = { .wfd = -1, .battery = -1, .hsvol = -1 };

static int want_on;                 /* btout = on */
static char want_dev[18];           /* btout_device */
static int conf_read, init_done;
static long init_t, retry_t, route_t, route_try;
static int retry_ms;
static int mute_sent = -1;          /* the panel's mute as the daemon last heard it */
static int vol_sent = -1;           /* the headset level as the daemon last heard it (-1: not at all since it started) */
static unsigned hs_seen;            /* the headset's own volume reports already taken over */
static int routed;                  /* the Bluetooth output is the one enabled in MPD */
static int route_known;             /* routing was applied at least once since the daemon last changed state */
static char own_err[96];            /* what the screen says apart from the daemon (refusals, MPD without the output) */

enum view { V_LIST, V_ACTION };
static enum view view;
static int sel, top, asel;
static char cur_addr[18], cur_name[64];
static long forget_t;
static char note[96];
static int note_bad;
static long note_t;

/* ------------------------------------------------------------------ helpers --- */

static void set_note(const char *msg, int bad)
{
	snprintf(note, sizeof note, "%s", msg);
	note_bad = bad;
	note_t = ext_now_ms();
}

static void conf_load(void)
{
	FILE *f = fopen(CONF, "r");
	char line[300];

	conf_read = 1;
	if (!f)
		return;
	while (fgets(line, sizeof line, f)) {
		char *k = line, *v;

		while (*k == ' ' || *k == '\t')
			k++;
		v = strchr(k, '=');
		if (!v || *k == '#')
			continue;
		*v++ = '\0';
		while (*v == ' ' || *v == '\t')
			v++;
		v[strcspn(v, "\r\n ")] = '\0';
		k[strcspn(k, " \t")] = '\0';
		if (!strcmp(k, "btout"))
			want_on = !strcmp(v, "on") || !strcmp(v, "1");
		else if (!strcmp(k, "btout_device") && strlen(v) == 17)
			snprintf(want_dev, sizeof want_dev, "%s", v);
	}
	fclose(f);
}

static int addr_ok(const char *a)
{
	int i;

	if (strlen(a) != 17)
		return 0;
	for (i = 0; i < 17; i++) {
		char c = a[i];

		if (i % 3 == 2 ? c != ':' : !((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f')))
			return 0;
	}
	return 1;
}

static int level_of(int rssi)
{
	return rssi >= -55 ? 4 : rssi >= -67 ? 3 : rssi >= -77 ? 2 : 1;
}

/* ------------------------------------------------------------------- daemon --- */

static void send_line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void send_line(const char *fmt, ...)
{
	char buf[160];
	va_list ap;
	int n;
	ssize_t r;

	if (B.pid <= 0 || B.wfd < 0)
		return;
	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof buf - 1, fmt, ap);
	va_end(ap);
	if (n <= 0 || n >= (int)sizeof buf - 1)
		return;
	buf[n++] = '\n';
	r = write(B.wfd, buf, (size_t)n);
	(void)r;
}

static void forget_state(void)
{
	B.seq = 0;
	B.state[0] = B.target[0] = B.name[0] = B.err[0] = B.autod[0] = '\0';
	B.connected = B.streaming = B.scanning = 0;
	B.underruns = B.slips = 0;
	B.battery = -1;
	B.hsvol = -1;
	B.hsseq = hs_seen = 0;
	B.nd = 0;
}

static int daemon_start(struct ui *u)
{
	int fds[2];
	pid_t pid;

	if (B.pid > 0)
		return 0;
	if (u->bt_pid > 0) {
		snprintf(own_err, sizeof own_err, "leave the Bluetooth input first");
		return -1;
	}
	own_err[0] = '\0';
	unlink(BTOUT_ST);
	forget_state();
	snprintf(B.state, sizeof B.state, "starting");
	if (pipe(fds) < 0) {
		snprintf(own_err, sizeof own_err, "cannot start bluetooth");
		return -1;
	}
	pid = fork();
	if (pid < 0) {
		close(fds[0]);
		close(fds[1]);
		snprintf(own_err, sizeof own_err, "cannot start bluetooth");
		return -1;
	}
	if (pid == 0) {
		int log = open(BTOUT_LOG, O_WRONLY | O_CREAT | O_TRUNC, 0644);

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
		execl(BTOUT_RUN, "x45btout-run", (char *)NULL);
		_exit(127);
	}
	close(fds[0]);
	fcntl(fds[1], F_SETFD, FD_CLOEXEC);   /* other children must not keep the pipe open */
	B.wfd = fds[1];
	B.pid = pid;
	B.t_start = ext_now_ms();
	B.t_poll = 0;
	if (want_dev[0])
		send_line("auto %s", want_dev);
	u->dirty = 1;
	return 0;
}

/* a link that is going away takes the sound with it: back to the speakers first */
static void route_apply(struct ui *u, int to_bt, int force);

/* SIGINT is the polite way out (the stream is ended, the link dropped, the controller reset); a kill while it
 * runs could wedge the stick, so it gets time. */
static void daemon_stop(struct ui *u)
{
	int i, done = 0;

	if (B.pid <= 0)
		return;
	route_apply(u, 0, 1);
	kill(B.pid, SIGINT);
	for (i = 0; i < 500 && !done; i++) {
		if (waitpid(B.pid, NULL, WNOHANG) == B.pid)
			done = 1;
		else
			usleep(20000);
	}
	if (!done) {
		kill(B.pid, SIGKILL);
		waitpid(B.pid, NULL, 0);
	}
	if (B.wfd >= 0)
		close(B.wfd);
	B.wfd = -1;
	B.pid = 0;
	unlink(BTOUT_ST);
	forget_state();
	u->dirty = 1;
}

static void status_read(struct ui *u)
{
	FILE *f = fopen(BTOUT_ST, "r");
	char line[256];
	struct bdev nd[MAXDEV];
	int n = 0, have_seq = 0;
	unsigned seq = 0;

	if (!f)
		return;
	memset(nd, 0, sizeof nd);
	while (fgets(line, sizeof line, f)) {
		char *v = strchr(line, '=');

		if (!v)
			continue;
		*v++ = '\0';
		v[strcspn(v, "\r\n")] = '\0';
		if (!strcmp(line, "seq")) {
			seq = (unsigned)strtoul(v, NULL, 10);
			have_seq = 1;
			if (seq == B.seq) {
				fclose(f);
				return;
			}
		} else if (!strcmp(line, "state")) {
			snprintf(B.state, sizeof B.state, "%s", v);
		} else if (!strcmp(line, "target")) {
			snprintf(B.target, sizeof B.target, "%s", v);
		} else if (!strcmp(line, "name")) {
			snprintf(B.name, sizeof B.name, "%s", v);
		} else if (!strcmp(line, "error")) {
			snprintf(B.err, sizeof B.err, "%s", v);
		} else if (!strcmp(line, "auto")) {
			snprintf(B.autod, sizeof B.autod, "%s", v);
		} else if (!strcmp(line, "connected")) {
			B.connected = atoi(v);
		} else if (!strcmp(line, "streaming")) {
			B.streaming = atoi(v);
		} else if (!strcmp(line, "scanning")) {
			B.scanning = atoi(v);
		} else if (!strcmp(line, "battery")) {
			B.battery = atoi(v);
		} else if (!strcmp(line, "hsvol")) {
			B.hsvol = atoi(v);
		} else if (!strcmp(line, "hsseq")) {
			B.hsseq = (unsigned)strtoul(v, NULL, 10);
		} else if (!strcmp(line, "underruns")) {
			B.underruns = (unsigned)strtoul(v, NULL, 10);
		} else if (!strcmp(line, "slips")) {
			B.slips = (unsigned)strtoul(v, NULL, 10);
		} else if (!strcmp(line, "dev") && n < MAXDEV) {
			/* ADDR|name|known|connected|rssi|heard */
			char *p[6], *s = v;
			int k;

			for (k = 0; k < 6; k++) {
				p[k] = s;
				s = strchr(s, '|');
				if (!s)
					break;
				*s++ = '\0';
			}
			if (k == 5 && addr_ok(p[0])) {
				snprintf(nd[n].addr, sizeof nd[n].addr, "%s", p[0]);
				snprintf(nd[n].name, sizeof nd[n].name, "%s", p[1]);
				nd[n].known = atoi(p[2]);
				nd[n].conn = atoi(p[3]);
				nd[n].rssi = atoi(p[4]);
				nd[n].heard = atoi(p[5]);
				n++;
			}
		}
	}
	fclose(f);
	if (!have_seq)
		return;
	B.seq = seq;
	memcpy(B.d, nd, sizeof B.d);
	B.nd = n;
	u->dirty = 1;
}

static const struct bdev *dev_at(int row);

static int dev_cmp(const void *pa, const void *pb)
{
	const struct bdev *a = pa, *b = pb;

	if (a->conn != b->conn)
		return b->conn - a->conn;
	if (a->known != b->known)
		return b->known - a->known;
	if (a->heard != b->heard)
		return b->heard - a->heard;
	return b->rssi - a->rssi;
}

/* ------------------------------------------------------------------ routing --- */

struct outs {
	int xmos, bt;
};

static void outs_cb(const char *key, const char *val, void *ud)
{
	struct outs *o = ud;
	static int id = -1;

	if (!strcmp(key, "outputid"))
		id = atoi(val);
	else if (!strcmp(key, "outputname")) {
		if (!strcmp(val, "XMOS"))
			o->xmos = id;
		else if (!strcmp(val, "bluetooth"))
			o->bt = id;
	}
}

/* Enable one of MPD's outputs and disable the other. Idempotent; `force` also repeats what was already applied. */
static void route_apply(struct ui *u, int to_bt, int force)
{
	struct outs o = { -1, -1 };

	if (!mpdc_is_open(&u->mpd))
		return;
	if (!force && route_known && routed == to_bt)
		return;
	if (mpdc_run(&u->mpd, "outputs", outs_cb, &o, NULL, 0) < 0 || o.xmos < 0)
		return;
	if (o.bt < 0) {
		if (to_bt) {
			snprintf(own_err, sizeof own_err, "MPD has no bluetooth output (restart the stack)");
			u->dirty = 1;
		} else {
			routed = 0;
			route_known = 1;
		}
		return;
	}
	if (to_bt) {   /* the new path first: the sound does not drop out in between */
		mpdc_cmd(&u->mpd, "enableoutput %d", o.bt);
		mpdc_cmd(&u->mpd, "disableoutput %d", o.xmos);
	} else {
		mpdc_cmd(&u->mpd, "enableoutput %d", o.xmos);
		mpdc_cmd(&u->mpd, "disableoutput %d", o.bt);
	}
	routed = to_bt;
	route_known = 1;
	route_t = ext_now_ms();
	own_err[0] = '\0';
	u->dirty = 1;
}

/* The state of MPD's outputs follows the link. Repeated while the sound goes to the device (MPD forgets it when it
 * restarts); nothing is repeated while it plays on the speakers, so another client may do as it likes. */
static void route_tick(struct ui *u, int want)
{
	long now = ext_now_ms();

	if (route_known && routed == want && (!want || ext_since(now, route_t) < ROUTE_MS))
		return;
	if (ext_since(now, route_try) < 2000)
		return;
	route_try = now;
	route_apply(u, want, 1);
}

/* ---------------------------------------------------------------------- API --- */

void ui_btout_release(struct ui *u)
{
	if (B.pid > 0) {
		daemon_stop(u);
		retry_t = ext_now_ms();
		retry_ms = RETRY_BT_MS;
	}
}

int ui_btout_busy(void)
{
	return B.pid > 0;
}

const char *ui_btout_connected_name(void)
{
	if (!B.connected)
		return NULL;
	return B.name[0] ? B.name : B.target[0] ? B.target : "connected";
}

int ui_btout_routed(void)
{
	return routed;
}

void ui_btout_set_volume(int v)
{
	if (v < 0)
		v = 0;
	if (v > 100)
		v = 100;
	if (B.pid > 0 && B.wfd >= 0) {
		send_line("volume %d", v);
		vol_sent = v;
	}
}

static void persist_on(int on)
{
	want_on = on;
	save_config_key("btout", on ? "on" : "off");
}

static void persist_dev(const char *addr)
{
	snprintf(want_dev, sizeof want_dev, "%s", addr);
	save_config_key("btout_device", addr);
}

static void do_switch(struct ui *u, int on)
{
	if (on) {
		if (u->bt_pid > 0) {
			set_note("Leave the Bluetooth input first", 1);
			return;
		}
		persist_on(1);
		retry_t = 0;
		if (daemon_start(u) < 0)
			set_note(own_err, 1);
	} else {
		persist_on(0);
		daemon_stop(u);
		own_err[0] = '\0';
	}
}

static void do_connect(struct ui *u, const char *addr)
{
	if (B.pid <= 0 || !addr_ok(addr))
		return;
	persist_dev(addr);
	send_line("connect %s", addr);
	send_line("auto %s", addr);
	own_err[0] = '\0';
	u->dirty = 1;
}

static void do_disconnect(struct ui *u)
{
	persist_dev("");
	send_line("disconnect");
	u->dirty = 1;
}

static void do_forget(struct ui *u, const char *addr)
{
	if (!addr_ok(addr))
		return;
	if (!strcmp(want_dev, addr))
		persist_dev("");
	send_line("forget %s", addr);
	u->dirty = 1;
}

static void do_scan(struct ui *u)
{
	if (B.pid <= 0)
		return;
	if (B.connected) {
		set_note("Disconnect first: searching would interrupt the sound", 1);
		return;
	}
	send_line("scan");
	u->dirty = 1;
}

/* the commands of the web page: on | off | scan | stopscan | connect ADDR | disconnect | forget ADDR */
void ui_btout_web(struct ui *u, const char *cmd)
{
	char w[16] = "", a[40] = "";

	if (sscanf(cmd, "%15s %39s", w, a) < 1)
		return;
	if (!strcmp(w, "on"))
		do_switch(u, 1);
	else if (!strcmp(w, "off"))
		do_switch(u, 0);
	else if (!strcmp(w, "scan"))
		do_scan(u);
	else if (!strcmp(w, "stopscan"))
		send_line("stopscan");
	else if (!strcmp(w, "connect"))
		do_connect(u, a);
	else if (!strcmp(w, "disconnect"))
		do_disconnect(u);
	else if (!strcmp(w, "forget"))
		do_forget(u, a);
}

static void jq(FILE *f, const char *s)
{
	fputc('"', f);
	for (; *s; s++) {
		unsigned char c = (unsigned char)*s;

		if (c == '"' || c == '\\')
			fprintf(f, "\\%c", c);
		else if (c < 32)
			fputc(' ', f);
		else
			fputc(c, f);
	}
	fputc('"', f);
}

/* the state for the web page: one JSON object */
void ui_btout_json(struct ui *u, FILE *f)
{
	int i;

	(void)u;
	fprintf(f, "{\"on\":%d,\"active\":%d,\"state\":", want_on, B.pid > 0);
	jq(f, B.state);
	fputs(",\"name\":", f);
	jq(f, B.name);
	fputs(",\"target\":", f);
	jq(f, B.target);
	fputs(",\"auto\":", f);
	jq(f, B.autod[0] ? B.autod : want_dev);
	fprintf(f, ",\"connected\":%d,\"streaming\":%d,\"scanning\":%d,\"routed\":%d,\"battery\":%d,\"error\":", B.connected, B.streaming,
		B.scanning, routed, B.connected ? B.battery : -1);
	jq(f, own_err[0] ? own_err : B.err);
	fputs(",\"devices\":[", f);
	for (i = 0; i < B.nd; i++) {
		fprintf(f, "%s{\"addr\":", i ? "," : "");
		jq(f, B.d[i].addr);
		fputs(",\"name\":", f);
		jq(f, B.d[i].name);
		fprintf(f, ",\"known\":%d,\"connected\":%d,\"rssi\":%d,\"heard\":%d}", B.d[i].known, B.d[i].conn, B.d[i].rssi,
			B.d[i].heard);
	}
	fputs("]}", f);
}

/* -------------------------------------------------------------------- polling --- */

static void poll(struct ui *u)
{
	long now = ext_now_ms();
	char keep[18];

	if (!conf_read)
		conf_load();
	if (!init_done) {
		if (!init_t)
			init_t = now;
		if (ext_since(now, init_t) < BOOT_WAIT_MS)
			return;
		init_done = 1;
		if (want_on && u->bt_pid <= 0 && daemon_start(u) < 0)
			retry_t = now, retry_ms = RETRY_FAIL_MS;
	}
	if (note[0] && ext_since(now, note_t) > 12000) {
		note[0] = '\0';
		u->dirty = 1;
	}
	if (forget_t && ext_since(now, forget_t) > 4000) {
		forget_t = 0;
		u->dirty = 1;
	}
	if (B.pid > 0 && B.wfd >= 0 && (u->muted != 0) != mute_sent) {   /* the panel's mute reaches the stream too */
		mute_sent = u->muted != 0;
		send_line("mute %d", mute_sent);
	}
	if (B.pid > 0 && B.wfd >= 0 && vol_sent < 0) {   /* a daemon that just started: the kept level goes first, before any link */
		if (u->bt_vol < 0)
			u->bt_vol = 25;
		ui_btout_set_volume(u->bt_swap ? u->volume : u->bt_vol);
	}
	if (B.pid <= 0) {
		mute_sent = -1;
		vol_sent = -1;
		/* wanted, not running: after the Bluetooth input was left, or a failure some time ago */
		if (want_on && u->bt_pid <= 0 && retry_t && ext_since(now, retry_t) >= retry_ms) {
			retry_t = 0;
			if (daemon_start(u) < 0) {
				retry_t = now;
				retry_ms = RETRY_FAIL_MS;
			}
		}
		route_tick(u, 0);
		return;
	}
	if (ext_since(now, B.t_poll) < POLL_MS)
		return;
	B.t_poll = now;
	if (waitpid(B.pid, NULL, WNOHANG) == B.pid) {   /* the launcher ended: it leaves the reason in the status file */
		B.pid = 0;
		if (B.wfd >= 0)
			close(B.wfd);
		B.wfd = -1;
		status_read(u);
		if (!B.err[0] && !own_err[0])
			snprintf(own_err, sizeof own_err, "bluetooth stopped, see " BTOUT_LOG);
		else if (B.err[0])
			snprintf(own_err, sizeof own_err, "%s", B.err);
		B.connected = B.streaming = B.scanning = 0;
		B.state[0] = '\0';
		retry_t = now;
		retry_ms = RETRY_FAIL_MS;
		route_apply(u, 0, 1);
		u->dirty = 1;
		return;
	}
	{
		const struct bdev *cur = sel >= 2 ? dev_at(sel) : NULL;

		keep[0] = 0;   /* the row under the cursor is a device, not a position: the list is sorted by signal strength */
		if (cur)
			snprintf(keep, sizeof keep, "%s", cur->addr);
	}
	status_read(u);
	if (B.hsseq != hs_seen) {   /* a key on the headset moved its volume: the unit follows */
		hs_seen = B.hsseq;
		if (B.hsvol >= 0 && B.hsvol <= 100 && ext_since(now, u->vol_fade_t) > 4000) {   /* not the echo of a fade */
			u->bt_vol = B.hsvol;
			u->vol_dirty = 1;
			if (u->bt_swap) {
				u->volume = B.hsvol;
				u->last_vol_change = now;
			}
			u->dirty = 1;
		}
	}
	if (B.nd > 1)
		qsort(B.d, (size_t)B.nd, sizeof B.d[0], dev_cmp);
	if (keep[0]) {   /* the cursor follows its device to the new row */
		int i;

		for (i = 0; i < B.nd; i++)
			if (!strcmp(B.d[i].addr, keep)) {
				sel = 2 + i;
				break;
			}
	}
	route_tick(u, B.connected);
}

/* --------------------------------------------------------------------- screen --- */

static int list_n(void)
{
	return B.pid > 0 ? 2 + B.nd : 1;
}

static const struct bdev *dev_at(int row)
{
	int i = row - 2;

	return i >= 0 && i < B.nd ? &B.d[i] : NULL;
}

static void enter(struct ui *u)
{
	(void)u;
	view = V_LIST;
	sel = top = 0;
	forget_t = 0;
	if (!conf_read)
		conf_load();
}

static void leave(struct ui *u)
{
	(void)u;
	view = V_LIST;
}

static int action_rows(int *ids)
{
	const struct bdev *d = dev_at(sel);
	int n = 0;

	if (!d)
		return 0;
	ids[n++] = d->conn ? 0 : 1;     /* 0 disconnect, 1 connect */
	if (d->known)
		ids[n++] = 2;           /* forget */
	return n;
}

static void open_action(struct ui *u, const struct bdev *d)
{
	snprintf(cur_addr, sizeof cur_addr, "%s", d->addr);
	snprintf(cur_name, sizeof cur_name, "%s", d->name[0] ? d->name : d->addr);
	asel = 0;
	forget_t = 0;
	view = V_ACTION;
	u->dirty = 1;
}

static void move(struct ui *u, int d)
{
	int n;

	(void)u;
	if (view == V_ACTION) {
		int ids[3];

		n = action_rows(ids);
		if (n)
			asel = ((asel + d) % n + n) % n;
		forget_t = 0;
		return;
	}
	n = list_n();
	sel = ((sel + d) % n + n) % n;
}

static void nav(struct ui *u, int dx, int dy)
{
	(void)dx;
	if (dy)
		move(u, dy);
}

static void ok(struct ui *u)
{
	if (view == V_ACTION) {
		int ids[3], n = action_rows(ids);

		if (asel >= n) {
			view = V_LIST;
			return;
		}
		switch (ids[asel]) {
		case 0:
			do_disconnect(u);
			view = V_LIST;
			break;
		case 1:
			do_connect(u, cur_addr);
			view = V_LIST;
			break;
		default:
			if (!forget_t) {
				forget_t = ext_now_ms();
			} else {
				do_forget(u, cur_addr);
				forget_t = 0;
				set_note("device forgotten", 0);
				view = V_LIST;
				sel = top = 0;
			}
			break;
		}
		return;
	}
	if (sel == 0) {
		do_switch(u, B.pid <= 0);
		return;
	}
	if (B.pid <= 0)
		return;
	if (sel == 1) {
		if (B.scanning)
			send_line("stopscan");
		else
			do_scan(u);
		return;
	}
	{
		const struct bdev *d = dev_at(sel);

		if (!d)
			return;
		if (d->conn || d->known) {
			open_action(u, d);
		} else {
			set_note("Connecting: the device has to be in pairing mode now", 0);
			do_connect(u, d->addr);
		}
	}
}

static void back(struct ui *u)
{
	if (view == V_ACTION) {
		view = V_LIST;
		forget_t = 0;
		return;
	}
	ui_ext_close(u);
}

static void status_block(struct ui *u)
{
	struct x45_surface *s = &u->g.back;
	char l1[128], l2[160] = "";
	uint32_t c1 = u->t.text, c2 = u->t.dim;
	const char *err = own_err[0] ? own_err : B.err;
	const char *nm = B.name[0] ? B.name : B.target;

	if (B.pid <= 0) {
		snprintf(l1, sizeof l1, "Bluetooth output is off");
		snprintf(l2, sizeof l2, "%s", err[0] ? err : "push to switch it on, then search for a device");
		if (err[0])
			c2 = u->t.warn;
		else
			c1 = u->t.dim;
	} else if (!strcmp(B.state, "starting") || !B.state[0]) {
		snprintf(l1, sizeof l1, "Starting Bluetooth...");
	} else if (!strcmp(B.state, "connecting")) {
		snprintf(l1, sizeof l1, "Connecting to %s...", nm);
		snprintf(l2, sizeof l2, "a new device has to be in pairing mode");
	} else if (B.connected) {
		snprintf(l1, sizeof l1, "Playing on %s", nm);
		snprintf(l2, sizeof l2, "%s", B.streaming ? "sending sound" : "connected, waiting for sound");
		if (B.battery >= 0)
			snprintf(l2 + strlen(l2), sizeof l2 - strlen(l2), "   battery %d%%", B.battery);
		c1 = u->t.accent;
	} else if (B.scanning) {
		snprintf(l1, sizeof l1, "Searching...");
		snprintf(l2, sizeof l2, "put the device in pairing mode now if it is new");
	} else {
		snprintf(l1, sizeof l1, "Not connected");
		snprintf(l2, sizeof l2, "%s", err[0] ? err : B.nd ? "choose a device" : "search for a device; a new one must be in pairing mode");
		if (err[0])
			c2 = u->t.warn;
	}
	if (note[0]) {
		snprintf(l2, sizeof l2, "%s", l1);
		snprintf(l1, sizeof l1, "%s", note);
		c1 = note_bad ? u->t.warn : u->t.accent;
		c2 = u->t.dim;
	}
	x45_text_clipped(s, 24, 72, l1, 3, s->w - 48, c1);
	x45_text_clipped(s, 24, 102, l2, 2, s->w - 48, c2);
}

static void row_list(struct ui *u, int i, int x, int y, int rw, int selected, void *ctx)
{
	struct x45_surface *s = &u->g.back;
	uint32_t fg = ext_fg(u, selected), dim = selected ? u->t.bg : u->t.dim;
	uint32_t acc = selected ? u->t.bg : u->t.accent;

	(void)ctx;
	if (i == 0) {
		const char *v = B.pid <= 0 ? "off" : "on";

		x45_text(s, x + 26, y + 9, "Bluetooth output", 3, fg);
		x45_text(s, x + rw - 24 - x45_text_w(v, 3), y + 9, v, 3, B.pid <= 0 ? dim : acc);
	} else if (i == 1) {
		const char *v = B.scanning ? "searching..." : "";

		x45_text(s, x + 26, y + 9, B.scanning ? "Stop searching" : "Search for devices", 3, fg);
		if (v[0])
			x45_text(s, x + rw - 24 - x45_text_w(v, 2), y + 12, v, 2, dim);
	} else {
		const struct bdev *d = dev_at(i);
		int rx = x + rw - 28;
		const char *tag = NULL;

		if (!d)
			return;
		if (d->conn)
			x45_fill_rect(s, x + 6, y + 14, 10, 10, acc);
		if (d->heard && d->rssi) {
			rx -= 30;
			ext_bars(u, rx, y + 6, level_of(d->rssi), acc, selected ? u->t.accent : u->t.line);
		}
		tag = d->conn ? "connected" : d->known ? "paired" : NULL;
		if (tag) {
			rx -= 18 + x45_text_w(tag, 2);
			x45_text(s, rx, y + 12, tag, 2, d->conn ? acc : dim);
		}
		x45_text_clipped(s, x + 26, y + 9, d->name[0] ? d->name : d->addr, 3, rx - x - 40,
				 d->heard || d->known ? fg : dim);
	}
}

static void row_action(struct ui *u, int i, int x, int y, int rw, int selected, void *ctx)
{
	struct x45_surface *s = &u->g.back;
	uint32_t fg = ext_fg(u, selected);
	int ids[3];
	const char *l;

	(void)ctx;
	(void)rw;
	if (i >= action_rows(ids))
		return;
	switch (ids[i]) {
	case 0:
		l = "Disconnect";
		break;
	case 1:
		l = "Connect";
		break;
	default:
		if (forget_t) {
			l = "Press OK again to forget";
			fg = selected ? u->t.bg : u->t.warn;
		} else {
			l = "Forget this device";
		}
		break;
	}
	x45_text(s, x + 26, y + 9, l, 3, fg);
}

static void draw(struct ui *u)
{
	struct x45_surface *s = &u->g.back;

	if (view == V_ACTION) {
		int ids[3], n = action_rows(ids);

		x45_text_clipped(s, 24, 72, cur_name, 3, s->w - 48, u->t.text);
		x45_text_clipped(s, 24, 102, strcmp(want_dev, cur_addr) ? "" : "called again by itself when it is switched on", 2,
				 s->w - 48, u->t.dim);
		ext_list(u, 12, 150, s->w - 24, n * EXT_ROW_H, n, asel, &(int){ 0 }, row_action, NULL);
		draw_hints(u, "scroll|move;OK|choose;RETURN|back");
		return;
	}
	status_block(u);
	x45_hline(s, 12, 136, s->w - 24, u->t.line);
	ext_list(u, 12, 148, s->w - 24, 390, list_n(), sel, &top, row_list, NULL);
	draw_hints(u, "scroll|move;OK|select;RETURN|back");
}

/* the top bar: the Bluetooth symbol while a device is connected (lit while the sound goes there) */
static int badge(struct ui *u, int right)
{
	struct x45_surface *s = &u->g.back;
	const struct x45_icon *ic;
	uint32_t col = B.streaming ? u->t.accent : u->t.dim;

	if (!B.connected)
		return 0;
	ic = icon_get(u, "bluetooth");
	if (B.battery >= 0) {   /* the headset's charge beside the symbol; red-ish when nearly empty */
		char t[8];
		int tw;

		snprintf(t, sizeof t, "%d%%", B.battery);
		tw = x45_text_w(t, 2);
		x45_text(s, right - tw, 24, t, 2, B.battery <= 15 ? u->t.warn : u->t.dim);
		right -= tw + 6;
		if (!ic)
			return tw + 6;
		x45_icon_draw(s, ic, right - ic->w, 28 - ic->h / 2, col);
		return tw + 6 + ic->w;
	}
	if (!ic) {
		x45_text(s, right - 40, 28, "BT", 2, col);
		return 48;
	}
	x45_icon_draw(s, ic, right - ic->w, 28 - ic->h / 2, col);
	return ic->w;
}

const struct ui_ext ext_btout = {
	.name = "btout",
	.title = "BLUETOOTH OUTPUT",
	.menu = "Bluetooth output",
	.enter = enter,
	.leave = leave,
	.draw = draw,
	.ok = ok,
	.back = back,
	.move = move,
	.nav = nav,
	.poll = poll,
	.badge = badge,
};
