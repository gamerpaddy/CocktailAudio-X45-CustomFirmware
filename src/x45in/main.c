/* x45in  -  read the X45 front panel and IR remote.
 *
 *   x45in devices        list the input devices and what we matched
 *   x45in monitor        decoded events, human readable  (default)
 *   x45in raw            every evdev event, undecoded  -  for working out wiring
 *   x45in events         one line per event, for piping into a daemon
 *   x45in learn          press remote buttons, get a code table to paste
 *   x45in knob           score both knob models against one real turn
 *   x45in demo           live volume control: turn the knob, it sets the volume
 *
 * `demo` is the end-to-end proof that Layer 1 is complete: panel input ->
 * libx45 -> the DACs, with no vendor code in the path.
 */
#define _GNU_SOURCE
#include "../libx45/x45.h"
#include "../libx45/x45in.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stop_now;
static void on_sig(int s) { (void)s; stop_now = 1; }

static void list_devices(void)
{
	DIR *d = opendir("/sys/class/input");
	struct dirent *e;

	if (!d) {
		perror("x45in: /sys/class/input");
		return;
	}
	puts("evdev    name                      matched as");
	while ((e = readdir(d))) {
		char path[512], name[128] = "";
		FILE *f;
		size_t got;
		const char *as = "-";

		if (strncmp(e->d_name, "event", 5))
			continue;
		snprintf(path, sizeof path, "/sys/class/input/%s/device/name", e->d_name);
		f = fopen(path, "r");
		if (f) {
			got = fread(name, 1, sizeof name - 1, f);
			fclose(f);
			name[got] = 0;
			while (got && name[got - 1] == '\n')
				name[--got] = 0;
		}
		if (strstr(name, "gpio-keys"))
			as = "front panel";
		if (strstr(name, "gpio_ir_recv"))
			as = "IR receiver";
		printf("%-8s %-25s %s\n", e->d_name, name, as);
	}
	closedir(d);
}

static void raw_dump(void)
{
	struct x45_indev in;
	int r;

	if ((r = x45_input_open(&in)) < 0) {
		fprintf(stderr, "x45in: no input devices: %s\n", strerror(-r));
		return;
	}
	printf("panel fd=%d  ir fd=%d   (ctrl-C to stop)\n", in.fd_panel, in.fd_ir);
	puts("source  type  code  value  meaning");
	if (in.fd_panel >= 0)
		fcntl(in.fd_panel, F_SETFL, O_NONBLOCK);
	if (in.fd_ir >= 0)
		fcntl(in.fd_ir, F_SETFL, O_NONBLOCK);
	while (!stop_now) {
		int fds[2] = { in.fd_panel, in.fd_ir };
		const char *src[2] = { "panel", "ir" };
		struct timespec ts = { 0, 20 * 1000 * 1000 };
		int i;

		for (i = 0; i < 2; i++) {
			struct x45_input_event iev;
			if (fds[i] < 0)
				continue;
			while (read(fds[i], &iev, sizeof iev) == (ssize_t)sizeof iev) {
				if (iev.type == 0 && iev.code == 0)
					continue; /* EV_SYN */
				printf("%-7s %-5u %-5u %-6d %s\n", src[i], iev.type,
				       iev.code, iev.value,
				       i == 0 ? x45_key_name(iev.code)
					      : x45_ir_name(iev.code));
				fflush(stdout);
			}
		}
		nanosleep(&ts, NULL);
	}
	x45_input_close(&in);
}

static void monitor(int line_mode)
{
	struct x45_indev in;
	struct x45_event ev;
	long vol_ticks = 0, scroll_ticks = 0;
	int r;

	if ((r = x45_input_open(&in)) < 0) {
		fprintf(stderr, "x45in: no input devices: %s\n", strerror(-r));
		return;
	}
	if (!line_mode) {
		printf("panel fd=%d  ir fd=%d  decode=%s   (ctrl-C to stop)\n",
		       in.fd_panel, in.fd_ir,
		       in.decode == X45_DECODE_DIRECTION ? "direction" : "quadrature");
		puts("turn a knob, press a button, or point the remote at it");
	}
	while (!stop_now) {
		r = x45_input_next(&in, &ev, 500);
		if (r < 0) {
			fprintf(stderr, "x45in: %s\n", strerror(-r));
			break;
		}
		if (r == 0 || ev.kind == X45_EV_NONE)
			continue;

		switch (ev.kind) {
		case X45_EV_KEY:
			if (line_mode)
				printf("key %d %s %d\n", ev.code, ev.name, ev.pressed);
			else
				printf("[%ld.%03ld] KEY      %-20s code=%-4d %s\n",
				       ev.when.sec, ev.when.usec / 1000, ev.name,
				       ev.code, ev.pressed ? "press" : "release");
			break;
		case X45_EV_ENCODER: {
			long *acc = ev.code == X45_KNOB_VOLUME ? &vol_ticks
							       : &scroll_ticks;
			*acc += ev.ticks;
			if (line_mode)
				printf("enc %d %s %+d\n", ev.code, ev.name, ev.ticks);
			else
				printf("[%ld.%03ld] ENCODER  %-20s %+d   total=%+ld\n",
				       ev.when.sec, ev.when.usec / 1000, ev.name,
				       ev.ticks, *acc);
			break;
		}
		case X45_EV_IR:
			if (line_mode)
				printf("ir %d %s %d\n", ev.code, ev.name, ev.pressed);
			else
				printf("[%ld.%03ld] IR       %-20s code=%-4d %s\n",
				       ev.when.sec, ev.when.usec / 1000, ev.name,
				       ev.code, ev.pressed == 2 ? "held" : ev.pressed ? "press" : "release");
			break;
		default:
			break;
		}
		fflush(stdout);
	}
	x45_input_close(&in);
}

static void learn(void)
{
	struct x45_indev in;
	struct x45_event ev;
	int seen[1024];
	int r, n = 0;

	memset(seen, 0, sizeof seen);
	if ((r = x45_input_open(&in)) < 0) {
		fprintf(stderr, "x45in: no input devices: %s\n", strerror(-r));
		return;
	}
	puts("Press each remote button once. Ctrl-C when done.");
	puts("New codes print as they arrive; a paste-ready table follows.");
	puts("");
	while (!stop_now) {
		r = x45_input_next(&in, &ev, 500);
		if (r <= 0 || ev.kind != X45_EV_IR || ev.pressed != 1)
			continue;
		if (ev.code >= 0 && ev.code < 1024 && !seen[ev.code]) {
			seen[ev.code] = 1;
			n++;
			printf("  new code %-5d  currently labelled %s\n",
			       ev.code, ev.name);
			fflush(stdout);
		}
	}
	printf("\n%d distinct codes seen (the keytable advertises 66).\n", n);
	puts("paste-ready:");
	for (r = 0; r < 1024; r++)
		if (seen[r])
			printf("\tcase %d: return \"ir:?\";\n", r);
	x45_input_close(&in);
}

/* Settles the one thing the device tree cannot: is each knob a pair of
 * direction lines, or a quadrature phase pair? Turn the knob slowly one way and
 * watch. One line pulsing per detent means direction; both lines interleaving
 * in a Gray sequence means quadrature. Both running totals are shown, so a
 * single turn settles it. */
static void knob_arbiter(void)
{
	struct x45_indev in;
	struct x45_quad q[2];
	int r, dir_total[2] = { 0, 0 }, quad_total[2] = { 0, 0 };
	int edges[2] = { 0, 0 };

	if ((r = x45_input_open(&in)) < 0) {
		fprintf(stderr, "x45in: no input devices: %s\n", strerror(-r));
		return;
	}
	memset(q, 0, sizeof q);
	puts("Turn ONE knob slowly in ONE direction, about ten detents, then stop.");
	puts("");
	puts("  one line pulsing per detent          -> DIRECTION (what the DT says)");
	puts("  both lines interleaving 00-01-11-10  -> QUADRATURE");
	puts("");
	printf("  %-14s %-5s %-5s | %-20s | %s\n",
	       "line", "value", "knob", "direction total", "quadrature");

	while (!stop_now) {
		struct x45_input_event iev;
		struct pollfd pf;
		int knob, is_down, qt;

		if (in.fd_panel < 0)
			break;
		pf.fd = in.fd_panel;
		pf.events = POLLIN;
		r = poll(&pf, 1, 500);
		if (r <= 0)
			continue;
		if (read(in.fd_panel, &iev, sizeof iev) != (ssize_t)sizeof iev)
			continue;
		if (iev.type != EV_KEY)
			continue;
		if (iev.code != X45_ENC_VOL_UP && iev.code != X45_ENC_VOL_DOWN &&
		    iev.code != X45_ENC_SCROLL_UP && iev.code != X45_ENC_SCROLL_DN) {
			printf("  (button %d %s, ignoring)\n", iev.code,
			       x45_key_name(iev.code));
			fflush(stdout);
			continue;
		}
		knob = (iev.code == X45_ENC_VOL_UP || iev.code == X45_ENC_VOL_DOWN)
			       ? X45_KNOB_VOLUME : X45_KNOB_SCROLL;
		is_down = (iev.code == X45_ENC_VOL_DOWN ||
			   iev.code == X45_ENC_SCROLL_DN);
		edges[knob]++;
		if (iev.value)
			dir_total[knob] += is_down ? -1 : +1;
		qt = x45_quad_feed(&q[knob], is_down, iev.value, 4);
		quad_total[knob] += qt;
		printf("  %-14s %-5d %-5s | %+4d (%2d edges)      | %+4d\n",
		       x45_key_name(iev.code), iev.value,
		       knob == X45_KNOB_VOLUME ? "vol" : "scrl",
		       dir_total[knob], edges[knob], quad_total[knob]);
		fflush(stdout);
	}
	puts("");
	printf("volume knob : %d edges, direction=%+d, quadrature=%+d\n",
	       edges[0], dir_total[0], quad_total[0]);
	printf("scroll wheel: %d edges, direction=%+d, quadrature=%+d\n",
	       edges[1], dir_total[1], quad_total[1]);
	puts("");
	puts("Whichever total matches the detents you actually turned is the right");
	puts("model. If only ONE line ever appeared, it is DIRECTION.");
	x45_input_close(&in);
}

static void demo(void)
{
	struct x45_indev in;
	struct x45_event ev;
	int fd, vol = 30, r, muted = 0;

	fd = x45_open();
	if (fd < 0) {
		fprintf(stderr, "x45in: open /dev/x45: %s\n", strerror(-fd));
		return;
	}
	if ((r = x45_input_open(&in)) < 0) {
		fprintf(stderr, "x45in: no input devices: %s\n", strerror(-r));
		x45_close(fd);
		return;
	}
	x45_volume_mode(fd, 0);   /* the driver starts with the volume fixed: the knob would change nothing */
	x45_volume(fd, vol);
	printf("volume knob -> the DACs, directly. starting at %d.\n", vol);
	puts("volume-push toggles mute. ctrl-C to stop.");
	puts("");
	while (!stop_now) {
		r = x45_input_next(&in, &ev, 500);
		if (r <= 0)
			continue;
		if (ev.kind == X45_EV_ENCODER && ev.code == X45_KNOB_VOLUME) {
			vol += ev.ticks;
			if (vol < 0)
				vol = 0;
			if (vol > 100)
				vol = 100;
			x45_volume(fd, vol);
			printf("\rvolume %3d   SRC4392=0x%02X %6.1f dB   ES9018=0x%02X %6.1f dB   ",
			       vol, x45_src4392_vol_reg(vol), x45_src4392_vol_db(vol),
			       x45_es9018_vol_reg(vol), x45_es9018_vol_db(vol));
			fflush(stdout);
		} else if (ev.kind == X45_EV_KEY &&
			   ev.code == X45_KEY_VOL_PUSH && ev.pressed) {
			muted = !muted;
			x45_mute(fd, muted);
			printf("\n%s\n", muted ? "muted" : "unmuted");
		}
	}
	putchar('\n');
	x45_input_close(&in);
	x45_close(fd);
}

int main(int argc, char **argv)
{
	const char *cmd = argc > 1 ? argv[1] : "monitor";

	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);

	if (!strcmp(cmd, "devices"))
		list_devices();
	else if (!strcmp(cmd, "knob"))
		knob_arbiter();
	else if (!strcmp(cmd, "raw"))
		raw_dump();
	else if (!strcmp(cmd, "monitor"))
		monitor(0);
	else if (!strcmp(cmd, "events"))
		monitor(1);
	else if (!strcmp(cmd, "learn"))
		learn();
	else if (!strcmp(cmd, "demo"))
		demo();
	else {
		puts("usage: x45in devices|monitor|raw|events|learn|knob|demo");
		return 2;
	}
	return 0;
}
