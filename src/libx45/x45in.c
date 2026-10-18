/* libx45/input  -  see x45in.h. */
#define _GNU_SOURCE
#include "x45in.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <pthread.h>
#include <sys/mman.h>
#include <time.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* ----------------------------------------------------- device discovery --- */

/* Reads /sys/class/input/<ev>/device/name. */
static int read_dev_name(const char *ev, char *out, size_t n)
{
	char path[512];
	FILE *f;
	size_t got;

	snprintf(path, sizeof path, "/sys/class/input/%s/device/name", ev);
	f = fopen(path, "r");
	if (!f)
		return -1;
	got = fread(out, 1, n - 1, f);
	fclose(f);
	out[got] = 0;
	while (got && (out[got - 1] == '\n' || out[got - 1] == '\r'))
		out[--got] = 0;
	return 0;
}

/* Opens /dev/input/eventN whose device name contains `needle`. */
static int open_by_name(const char *needle)
{
	DIR *d = opendir("/sys/class/input");
	struct dirent *e;
	int fd = -1;

	if (!d)
		return -errno;
	while ((e = readdir(d))) {
		char name[128], path[512];
		if (strncmp(e->d_name, "event", 5))
			continue;
		if (read_dev_name(e->d_name, name, sizeof name) < 0)
			continue;
		if (!strstr(name, needle))
			continue;
		snprintf(path, sizeof path, "/dev/input/%s", e->d_name);
		fd = open(path, O_RDONLY | O_CLOEXEC);
		if (fd >= 0)
			break;
	}
	closedir(d);
	return fd >= 0 ? fd : -ENOENT;
}

/* ------------------------------------------------------- fast knob sampler --- */

#define GPIO4_BASE 0x020A8000UL /* i.MX6 GPIO4; PSR (pad status) at +0x08 */
#define GPIO4_PSR  0x08
/* knob pins on GPIO4: volume A=23 B=22, scroll A=25 B=24 (active low) */
static const int knob_pin_a[2] = { 23, 25 };
static const int knob_pin_b[2] = { 22, 24 };

static int quad_step(struct x45_quad *q, uint8_t next, int per_detent);

struct sampler {
	struct x45_indev *in;
	volatile uint32_t *psr;
	int wfd;
	pthread_t th;
	void *map;
	volatile int stop;
	int running;
};
static struct sampler g_sampler;

static int64_t sampler_now_us(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

/* What the last burst of knob movement looked like, for whoever wonders why a fast turn loses steps: how often the pins
 * were read, and how many edges came in that jumped two states at once (the knob had moved on by more than one
 * edge between two reads, so the step is lost). */
#define KNOB_STAT_FILE "/tmp/x45knob.stat"

static void sampler_stat(int64_t t0, int64_t t1, long samples, long edges, long ticks, long diag)
{
	FILE *f;
	long ms = (long)((t1 - t0) / 1000);

	if (ms < 20 || edges < 4)
		return;
	f = fopen(KNOB_STAT_FILE, "w");
	if (!f)
		return;
	fprintf(f, "burst %ld ms  reads %ld (%ld per s)  edges %ld  steps %ld  two-state jumps %ld\n",
		ms, samples, (long)((long long)samples * 1000 / ms), edges, ticks, diag);
	fclose(f);
}

/* The pins are read every 250 us while the knob rests (sleeps are as long as the kernel's timers make them) and without
 * any pause while it moves and for 150 ms after: a turn fast enough to need it is over before a long sleep ends. */
#define KNOB_SPIN_US 150000

static void *sampler_main(void *arg)
{
	struct sampler *s = arg;
	struct x45_quad q[2];
	struct timespec ts = { 0, 250000 };
	uint32_t v = *s->psr;
	int64_t t0 = 0, t_edge = 0, now;
	long samples = 0, s_edge = 0, edges = 0, ticks = 0, diag = 0;
	unsigned spin = 0;
	int k, active = 0;

	for (k = 0; k < 2; k++) {
		q[k].accum = 0;
		q[k].seen = 1;
		q[k].state = (uint8_t)((!((v >> knob_pin_a[k]) & 1)) << 1 |
				       (!((v >> knob_pin_b[k]) & 1)));
	}
	while (!s->stop) {
		int changed = 0;

		v = *s->psr;
		samples++;
		for (k = 0; k < 2; k++) {
			uint8_t next = (uint8_t)((!((v >> knob_pin_a[k]) & 1)) << 1 |
						 (!((v >> knob_pin_b[k]) & 1)));
			int t;

			if (next == q[k].state)
				continue;
			changed = 1;
			edges++;
			if ((next ^ q[k].state) == 3)
				diag++;
			t = quad_step(&q[k], next, s->in->ticks_per_detent);
			if (t) {
				int8_t rec[2] = { (int8_t)k, (int8_t)t };
				ssize_t w = write(s->wfd, rec, 2);
				(void)w; /* pipe full: the UI is not keeping up */
				ticks++;
			}
		}
		if (changed) {
			now = sampler_now_us();
			if (!active) {
				active = 1;
				t0 = now;
			}
			t_edge = now;
			s_edge = samples;
		}
		if (active) {
			if ((++spin & 63) == 0 && sampler_now_us() - t_edge > KNOB_SPIN_US) {
				active = 0;
				sampler_stat(t0, t_edge, s_edge, edges, ticks, diag);
				edges = ticks = diag = 0;
			}
		} else {
			samples = 0;   /* a burst counts its reads from its first one (the one that saw the first edge is read 1) */
			nanosleep(&ts, NULL);
		}
	}
	return NULL;
}

static void sampler_start(struct x45_indev *in)
{
	int mfd, p[2];
	void *m;

	if (g_sampler.running)   /* one sampler per process; a second handle reads its knobs from evdev */
		return;
	mfd = open("/dev/mem", O_RDONLY | O_SYNC | O_CLOEXEC);
	if (mfd < 0)
		return;
	m = mmap(NULL, 0x1000, PROT_READ, MAP_SHARED, mfd, GPIO4_BASE);
	close(mfd);
	if (m == MAP_FAILED)
		return;
	if (pipe2(p, O_CLOEXEC | O_NONBLOCK) < 0) {
		munmap(m, 0x1000);
		return;
	}
	g_sampler.in = in;
	g_sampler.psr = (volatile uint32_t *)((char *)m + GPIO4_PSR);
	g_sampler.wfd = p[1];
	g_sampler.map = m;
	g_sampler.stop = 0;
	if (pthread_create(&g_sampler.th, NULL, sampler_main, &g_sampler)) {
		close(p[0]);
		close(p[1]);
		munmap(m, 0x1000);
		return;
	}
	g_sampler.running = 1;
	in->fd_fast = p[0];
}

/* Ends the sampler thread that x45_input_open() started, and closes its pipe. Without this a closed handle
 * left a thread writing into a pipe nobody read, and a second open started a second sampler beside it
 * (every detent counted twice). */
static void sampler_stop(struct x45_indev *in)
{
	if (in->fd_fast < 0 || !g_sampler.running)
		return;
	g_sampler.stop = 1;
	pthread_join(g_sampler.th, NULL);
	g_sampler.running = 0;
	close(g_sampler.wfd);
	close(in->fd_fast);
	munmap(g_sampler.map, 0x1000);
	in->fd_fast = -1;
}

int x45_input_open(struct x45_indev *in)
{
	memset(in, 0, sizeof *in);
	in->fd_fast = -1;
	in->decode = X45_DECODE_QUADRATURE; /* the lines are phases: see x45in.h */
	in->ticks_per_detent = 4;
	in->invert = 1;                     /* hardware polarity, see x45in.h */
	in->fd_panel = open_by_name("gpio-keys");
	in->fd_ir = open_by_name("gpio_ir_recv");
	if (in->fd_panel < 0 && in->fd_ir < 0)
		return in->fd_panel; /* -errno */
	if (in->fd_panel >= 0)
		sampler_start(in);
	return 0;
}

void x45_input_close(struct x45_indev *in)
{
	sampler_stop(in);
	if (in->fd_panel >= 0)
		close(in->fd_panel);
	if (in->fd_ir >= 0)
		close(in->fd_ir);
	in->fd_panel = in->fd_ir = -1;
}

/* --------------------------------------------------- quadrature decoding --- */

/* Standard Gray-code transition table, indexed [previous][current] with
 * state = (A<<1)|B. +1 clockwise, -1 counter-clockwise, 0 for no change, and
 * 0 for the illegal diagonal transitions (both channels appearing to change
 * at once, which means an edge was missed). */
static const int8_t quad_tbl[4][4] = {
	/*        to 00  01  10  11 */
	/* 00 */ {  0,  +1, -1,  0 },
	/* 01 */ { -1,   0,  0, +1 },
	/* 10 */ { +1,   0,  0, -1 },
	/* 11 */ {  0,  -1, +1,  0 }
};

/* Feeds one channel change into a knob. Returns signed detents, 0 if the
 * movement has not yet amounted to one. */
static int quad_feed(struct x45_quad *q, int chan_b, int value, int per_detent)
{
	uint8_t next;

	next = q->state;
	if (chan_b)
		next = (uint8_t)((next & 0x2) | (value ? 1 : 0));
	else
		next = (uint8_t)((next & 0x1) | (value ? 2 : 0));
	return quad_step(q, next, per_detent);
}

static int quad_step(struct x45_quad *q, uint8_t next, int per_detent)
{
	int8_t delta;

	if (!q->seen) {
		/* First edge only establishes where the shaft currently is. The
		 * channels can rest in any combination, so there is nothing to
		 * report yet. */
		q->seen = 1;
		q->state = next;
		return 0;
	}
	delta = quad_tbl[q->state][next];
	q->state = next;
	if (!delta)
		return 0;

	q->accum = (int8_t)(q->accum + delta);
	if (per_detent <= 1) {
		q->accum = 0;
		return delta;
	}
	if (q->accum >= per_detent) {
		q->accum = 0;
		return 1;
	}
	if (q->accum <= -per_detent) {
		q->accum = 0;
		return -1;
	}
	return 0;
}

int x45_quad_feed(struct x45_quad *q, int chan_b, int value, int per_detent)
{
	return quad_feed(q, chan_b, value, per_detent);
}

/* ------------------------------------------------------------- event loop --- */

static int handle_code(struct x45_indev *in, struct x45_event *ev,
		       int code, int value, int is_ir)
{
	if (is_ir) {
		if (value == 2) /* autorepeat */
			return 0;
		ev->kind = X45_EV_IR;
		ev->code = code;
		ev->pressed = value ? 1 : 0;
		ev->name = x45_ir_name(code);
		return 1;
	}

	switch (code) {
	case X45_ENC_VOL_UP:
	case X45_ENC_VOL_DOWN:
	case X45_ENC_SCROLL_UP:
	case X45_ENC_SCROLL_DN: {
		int knob = (code == X45_ENC_VOL_UP || code == X45_ENC_VOL_DOWN)
				   ? X45_KNOB_VOLUME : X45_KNOB_SCROLL;
		int is_down = (code == X45_ENC_VOL_DOWN || code == X45_ENC_SCROLL_DN);
		int t;

		if (in->fd_fast >= 0) /* the sampler thread owns the knobs */
			return 0;

		if (in->decode == X45_DECODE_DIRECTION) {
			/* One line per direction, as the device tree labels them.
			 * Count the press edge only; the release carries no
			 * information, and a shaft left resting on a line simply
			 * never produces one. */
			if (!value)
				return 0;
			t = is_down ? -1 : +1;
		} else {
			t = quad_feed(&in->knob[knob], is_down, value,
				      in->ticks_per_detent);
			if (!t)
				return 0;
			if (in->invert)
				t = -t;
		}
		ev->kind = X45_EV_ENCODER;
		ev->code = knob;
		ev->ticks = t;
		ev->name = knob == X45_KNOB_VOLUME ? "volume-knob" : "scroll-wheel";
		return 1;
	}
	default:
		if (value == 2) /* autorepeat */
			return 0;
		ev->kind = X45_EV_KEY;
		ev->code = code;
		ev->pressed = value ? 1 : 0;
		ev->name = x45_key_name(code);
		return 1;
	}
}

/* The receiver sends a packet per NEC frame: MSC_SCAN (the raw scancode), then EV_KEY if the keytable knows it,
 * then EV_SYN. A button that is held keeps sending frames, but this kernel turns the later ones into a refresh of
 * its release timer only: the packet holds the scancode and no key event, and no value-2 key event ever comes.
 * So a packet with the scancode of the button that is down, and no key, is the button being held. */
static int ir_repeat(struct x45_indev *in, const struct x45_input_event *iev, struct x45_event *ev)
{
	/* milliseconds as an unsigned 32-bit count that wraps (seconds x 1000 does not fit a 32-bit long); only the
	 * difference between two of them is used */
	uint32_t ms = (uint32_t)iev->sec * 1000u + (uint32_t)iev->usec / 1000u;

	if (iev->type == EV_MSC && iev->code == MSC_SCAN) {
		in->ir_scan = (uint32_t)iev->value;
		in->ir_scan_new = 1;
		return 0;
	}
	if (iev->type == EV_KEY) {
		in->ir_key_new = 1;
		if (iev->value == 0) {
			in->ir_last_code = 0;
		} else if (iev->value == 1) {
			in->ir_last_code = iev->code;
			in->ir_last_scan = in->ir_scan_new ? in->ir_scan : 0;
			in->ir_last_ms = (long)ms;
		}
		return 0;
	}
	if (iev->type != EV_SYN)
		return 0;
	{
		int held = in->ir_scan_new && !in->ir_key_new && in->ir_last_code && in->ir_last_scan == in->ir_scan &&
			   (uint32_t)(ms - (uint32_t)in->ir_last_ms) < 400;

		in->ir_scan_new = in->ir_key_new = 0;
		if (!held)
			return 0;
		in->ir_last_ms = (long)ms;
		ev->kind = X45_EV_IR;
		ev->code = in->ir_last_code;
		ev->pressed = 2;
		ev->name = x45_ir_name(ev->code);
		ev->when.sec = (long)iev->sec;
		ev->when.usec = (long)iev->usec;
		return 1;
	}
}

int x45_input_next(struct x45_indev *in, struct x45_event *ev, int timeout_ms)
{
	struct pollfd p[3];
	int n = 0, i, r;
	int idx_ir = -1, idx_fast = -1;

	memset(ev, 0, sizeof *ev);
	ev->name = "";

	if (in->fd_panel >= 0) {
		p[n].fd = in->fd_panel;
		p[n].events = POLLIN;
		n++;
	}
	if (in->fd_ir >= 0) {
		p[n].fd = in->fd_ir;
		p[n].events = POLLIN;
		idx_ir = n++;
	}
	if (in->fd_fast >= 0) {
		p[n].fd = in->fd_fast;
		p[n].events = POLLIN;
		idx_fast = n++;
	}
	if (!n)
		return -ENODEV;

	r = poll(p, (unsigned)n, timeout_ms);
	if (r < 0)
		return errno == EINTR ? 0 : -errno;
	if (r == 0)
		return 0;

	for (i = 0; i < n; i++) {
		struct x45_input_event iev;
		ssize_t got;

		if (!(p[i].revents & POLLIN))
			continue;
		if (i == idx_fast) {
			int8_t rec[2];

			if (read(p[i].fd, rec, 2) != 2)
				continue;
			ev->kind = X45_EV_ENCODER;
			ev->code = rec[0];
			ev->ticks = in->invert ? -rec[1] : rec[1];
			ev->name = rec[0] == X45_KNOB_VOLUME ? "volume-knob" : "scroll-wheel";
			return 1;
		}
			got = read(p[i].fd, &iev, sizeof iev);
		if (got != (ssize_t)sizeof iev)
			continue;
		if (i == idx_ir && ir_repeat(in, &iev, ev))
			return 1;
		if (iev.type != EV_KEY)
			continue; /* EV_SYN and anything else: ignore */

		ev->when.sec = (long)iev.sec;
		ev->when.usec = (long)iev.usec;
		if (handle_code(in, ev, iev.code, iev.value, i == idx_ir))
			return 1;
		return 0; /* consumed, but no complete event yet */
	}
	return 0;
}

/* ------------------------------------------------------------------ names --- */

const char *x45_key_name(int code)
{
	switch (code) {
	case X45_KEY_RETURN:   return "return";
	case X45_KEY_VOL_PUSH: return "volume-push(mute)";
	case X45_KEY_MENU:     return "menu";
	case X45_KEY_POWER:    return "power";
	case X45_KEY_STOP:     return "stop";
	case X45_KEY_INPUT:    return "input";
	case X45_KEY_OK:       return "ok(pause)";
	case X45_ENC_VOL_UP:    return "volume-up";
	case X45_ENC_VOL_DOWN:  return "volume-down";
	case X45_ENC_SCROLL_UP: return "scroll-up";
	case X45_ENC_SCROLL_DN: return "scroll-down";
	default:               return "unknown";
	}
}

/* The IR keytable is the stock one, held in the kernel. The capability
 * bitmask on /dev/input/event0 reports **66** codes, not the 56 noted earlier.
 *
 * These are the Linux keycode names, resolved from the kernel's own
 * input-event-codes.h - NOT what is printed on the remote. The vendor
 * repurposed F1-F13, PROG1/2 and FN_F1-F7 for their own functions, so the
 * mapping from a physical button to a code has to be learned:
 * run 'x45in learn' and press each button once. */
const char *x45_ir_name(int code)
{
	switch (code) {
	case 1: return "ir:esc";
	case 28: return "ir:enter";
	case 59: return "ir:f1";
	case 60: return "ir:f2";
	case 61: return "ir:f3";
	case 62: return "ir:f4";
	case 63: return "ir:f5";
	case 64: return "ir:f6";
	case 65: return "ir:f7";
	case 66: return "ir:f8";
	case 67: return "ir:f9";
	case 68: return "ir:f10";
	case 87: return "ir:f11";
	case 88: return "ir:f12";
	case 102: return "ir:home";
	case 103: return "ir:up";
	case 105: return "ir:left";
	case 106: return "ir:right";
	case 108: return "ir:down";
	case 116: return "ir:power";
	case 141: return "ir:setup";
	case 142: return "ir:sleep";
	case 148: return "ir:prog1";
	case 149: return "ir:prog2";
	case 150: return "ir:www";
	case 161: return "ir:ejectcd";
	case 168: return "ir:rewind";
	case 176: return "ir:edit";
	case 183: return "ir:f13";
	case 208: return "ir:fastforward";
	case 218: return "ir:connect";
	case 227: return "ir:switchvideomode";
	case 236: return "ir:battery";
	case 243: return "ir:brightness_cycle";
	case 359: return "ir:time";
	case 370: return "ir:subtitle";
	case 373: return "ir:mode";
	case 375: return "ir:screen";
	case 383: return "ir:cd";
	case 385: return "ir:radio";
	case 390: return "ir:aux";
	case 398: return "ir:red";
	case 399: return "ir:green";
	case 400: return "ir:yellow";
	case 401: return "ir:blue";
	case 407: return "ir:next";
	case 410: return "ir:shuffle";
	case 412: return "ir:previous";
	case 442: return "ir:images";
	case 466: return "ir:fn_f1";
	case 467: return "ir:fn_f2";
	case 468: return "ir:fn_f3";
	case 469: return "ir:fn_f4";
	case 470: return "ir:fn_f5";
	case 471: return "ir:fn_f6";
	case 472: return "ir:fn_f7";
	case 512: return "ir:numeric_0";
	case 513: return "ir:numeric_1";
	case 514: return "ir:numeric_2";
	case 515: return "ir:numeric_3";
	case 516: return "ir:numeric_4";
	case 517: return "ir:numeric_5";
	case 518: return "ir:numeric_6";
	case 519: return "ir:numeric_7";
	case 520: return "ir:numeric_8";
	case 521: return "ir:numeric_9";
	default:  return "ir:?";
	}
}
