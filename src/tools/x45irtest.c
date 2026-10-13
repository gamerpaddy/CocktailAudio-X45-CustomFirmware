/* x45irtest - log what the infrared remote sends, and keep it from doing anything meanwhile.
 *
 * The receiver (gpio_ir_recv, NEC) reports every button twice: the raw scancode (MSC_SCAN, 0x657xx) and, when the
 * the stock keytable knows the scancode, a Linux key code. The panel software reads the same device, so this
 * takes it exclusively (EVIOCGRAB): while this runs, no program gets a remote button, and nothing happens on the unit.
 * The grab ends with the process, however it ends. The front panel and the knobs are another device and keep working.
 *
 *   x45irtest [-o LOGFILE] [-m MINUTES] [-d NAME]    default /tmp/x45irtest.log, stops by itself after 120 minutes;
 *                                                    -d takes another input device (the front panel is gpio-keys.19)
 *
 *   kill -USR1 PID    start the next stage (writes a marker line to the log)
 *   kill PID          stop
 *
 * Log, one line per press (a held button adds "repeat" lines):
 *   [  12.345] stage 1 #07  scan=0x65710  code 116  ir:power  down
 * A scancode with no key code behind it is a button the keytable does not know:  scan=0x65733  not in the keytable
 * (the same scancode again within 0.4 s of the last one is the remote repeating a held button, and is not logged).
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
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "../libx45/x45in.h"

/* <linux/input.h> constants, written out so the tool builds against any header set */
#define EV_SYN_ 0
#define EV_KEY_ 1
#define EV_MSC_ 4
#define MSC_SCAN_ 4
#define EVIOCGNAME_(len) _IOC(_IOC_READ, 'E', 0x06, len)
#define EVIOCGRAB_ _IOW('E', 0x90, int)

static volatile sig_atomic_t stop, next_stage;

static void on_term(int sig)
{
	(void)sig;
	stop = 1;
}

static void on_usr1(int sig)
{
	(void)sig;
	next_stage = 1;
}

static double now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static int open_ir(const char *want)
{
	char path[40], name[80];
	int i;

	for (i = 0; i < 8; i++) {
		int fd;

		snprintf(path, sizeof path, "/dev/input/event%d", i);
		fd = open(path, O_RDONLY | O_CLOEXEC);
		if (fd < 0)
			continue;
		memset(name, 0, sizeof name);
		if (ioctl(fd, EVIOCGNAME_(sizeof name - 1), name) >= 0 && !strcmp(name, want)) {
			fprintf(stderr, "x45irtest: %s is %s\n", path, name);
			return fd;
		}
		close(fd);
	}
	return -1;
}

int main(int argc, char **argv)
{
	const char *log = "/tmp/x45irtest.log", *dev = "gpio_ir_recv";
	int minutes = 120, fd, o, stage = 1, n = 0, have_scan = 0, have_key = 0;
	uint32_t scan = 0, last_scan = 0;
	double t0, deadline, last_scan_t = -10;
	FILE *f;

	while ((o = getopt(argc, argv, "o:m:d:")) != -1) {
		if (o == 'o')
			log = optarg;
		else if (o == 'd')
			dev = optarg;
		else if (o == 'm')
			minutes = atoi(optarg);
		else {
			fprintf(stderr, "usage: x45irtest [-o LOGFILE] [-m MINUTES] [-d DEVICE NAME]\n");
			return 2;
		}
	}
	fd = open_ir(dev);
	if (fd < 0) {
		fprintf(stderr, "x45irtest: the input device %s was not found\n", dev);
		return 1;
	}
	if (ioctl(fd, EVIOCGRAB_, 1) < 0) {
		fprintf(stderr, "x45irtest: cannot take the receiver exclusively: %s\n", strerror(errno));
		return 1;
	}
	f = fopen(log, "w");
	if (!f) {
		fprintf(stderr, "x45irtest: cannot write %s: %s\n", log, strerror(errno));
		return 1;
	}
	setvbuf(f, NULL, _IOLBF, 0);
	signal(SIGTERM, on_term);
	signal(SIGINT, on_term);
	signal(SIGHUP, SIG_IGN);
	signal(SIGUSR1, on_usr1);
	t0 = now();
	deadline = t0 + 60.0 * minutes;
	fprintf(f, "# x45irtest: the remote is grabbed, nothing on the unit sees it. stage 1\n");
	fprintf(stderr, "x45irtest: the remote is now grabbed (pid %d), log %s\n", (int)getpid(), log);

	while (!stop && now() < deadline) {
		struct pollfd p = { .fd = fd, .events = POLLIN };
		struct x45_input_event ev;
		int r = poll(&p, 1, 500);
		ssize_t got;

		if (next_stage) {
			next_stage = 0;
			stage++;
			fprintf(f, "# ===== stage %d =====\n", stage);
		}
		if (r <= 0)
			continue;
		got = read(fd, &ev, sizeof ev);   /* the kernel's record is 16 bytes, not musl's 24: see x45in.h */
		if (got != (ssize_t)sizeof ev)
			continue;
		if (ev.type == EV_MSC_ && ev.code == MSC_SCAN_) {
			scan = (uint32_t)ev.value;
			have_scan = 1;
		} else if (ev.type == EV_KEY_) {
			char sc[16];

			if (have_scan)
				snprintf(sc, sizeof sc, "0x%05x", scan);
			else
				snprintf(sc, sizeof sc, "-      ");
			fprintf(f, "[%8.3f] stage %d #%02d  scan=%s  code %d  %s  %s\n", now() - t0, stage,
				ev.value == 1 ? ++n : n, sc, ev.code,
				x45_ir_name(ev.code), ev.value == 1 ? "down" : ev.value == 2 ? "repeat" : "up");
			have_key = 1;
		} else if (ev.type == EV_SYN_) {
			if (have_scan && !have_key && !(scan == last_scan && now() - last_scan_t < 0.4))
				fprintf(f, "[%8.3f] stage %d #%02d  scan=0x%05x  not in the keytable\n", now() - t0, stage,
					++n, scan);
			if (have_scan) {
				last_scan = scan;
				last_scan_t = now();
			}
			have_scan = have_key = 0;
		}
	}
	fprintf(f, "# stopped after %.0f s\n", now() - t0);
	fclose(f);
	ioctl(fd, EVIOCGRAB_, 0);
	return 0;
}
