#define _GNU_SOURCE
/* x45wd - the watchdog service of the x20 replacement.
 *
 * Started by /etc/init.d/x45new next to the supervisor (x45stack), stopped by it. It does three things:
 *
 *   1. feeds the i.MX6 hardware watchdog (/dev/watchdog, 60 s). When the whole system stops - the kernel hangs, the
 *      process table is wedged, the CPU is stuck - nothing feeds it any more and the chip resets the unit. Closing
 *      the device the proper way (SIGTERM) disarms it again, so a shutdown or a restart of the stack never trips it.
 *   2. watches the heartbeat of the panel UI (/tmp/x45ui.beat, written by ext/health.c every second: "pid seconds").
 *      A UI whose main loop has stood still for UI_STALE seconds is killed; x45stack starts a new one and counts it
 *      like any crash. A UI that cannot even be killed (stuck in the kernel) for UI_DEAD seconds more is a hang of
 *      the machine as far as the user can tell: the watchdog is left unfed and the unit resets.
 *   3. watches the supervisor itself. When x45stack is gone without having said goodbye (its pid file is still there
 *      but the process is not: killed, out of memory), everything it started is cleaned up and the stack is started
 *      again, at most three times an hour; after that the vendor UI is given back, as the supervisor would do.
 *
 * Everything it does is written to x45wd.log (kept across boots, rotated by size) and the state to /tmp/x45wd.status
 * (key=value lines, read by x45web for the health page). Before a deliberate reset it leaves the file wd.trip so
 * that the next start of the stack can say why the unit restarted.
 *
 *   x45wd [--timeout SECONDS] [--no-wd] [--stall SECONDS]
 *     --no-wd     do not open the hardware watchdog (watch only)
 *     --stall N   test: after N seconds stop feeding the watchdog (the unit resets within --timeout seconds)
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef WDIOC_KEEPALIVE
#define WDIOC_KEEPALIVE 0x80045705
#define WDIOC_SETTIMEOUT 0xc0045706
#endif

#define ROOT "/home/root/x45new"
#define LOGFILE ROOT "/x45wd.log"
#define LOG_MAX 262144L
#define CRASHES ROOT "/crashes.log"
#define TRIP ROOT "/wd.trip"
#define ENABLED ROOT "/ENABLED"
#define STACK_PID "/var/run/x45stack.pid"
#define WD_PID "/var/run/x45wd.pid"
#define STOPPED "/tmp/x45-stopped"
#define BEAT "/tmp/x45ui.beat"
#define STATUS "/tmp/x45wd.status"
#define PERIOD 2
#define UI_STALE 90L    /* seconds without a heartbeat before the UI is killed */
#define UI_DEAD 150L    /* seconds after the kill before the unit is left to reset */
#define STACK_TRIES 3
#define TRIES_WINDOW 3600L
#define RECOVER_WAIT 45L

static volatile sig_atomic_t g_stop;
static int wdfd = -1;

static void on_term(int sig)
{
	(void)sig;
	g_stop = 1;
}

static long mono(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long)ts.tv_sec;
}

static void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void logf_(const char *fmt, ...)
{
	struct stat st;
	char stamp[32];
	time_t now = time(NULL);
	struct tm tm;
	va_list ap;
	FILE *f;

	if (stat(LOGFILE, &st) == 0 && st.st_size > LOG_MAX) {
		unlink(LOGFILE ".1");
		rename(LOGFILE, LOGFILE ".1");
	}
	f = fopen(LOGFILE, "a");
	if (!f)
		return;
	if (localtime_r(&now, &tm))
		strftime(stamp, sizeof stamp, "%F %T", &tm);
	else
		snprintf(stamp, sizeof stamp, "-");
	fprintf(f, "%s x45wd: ", stamp);
	va_start(ap, fmt);
	vfprintf(f, fmt, ap);
	va_end(ap);
	fputc('\n', f);
	fclose(f);
}

/* the history of crashes and kills the health page shows: time TAB who TAB what */
static void crash_line(const char *what)
{
	char stamp[32];
	time_t now = time(NULL);
	struct tm tm;
	FILE *f = fopen(CRASHES, "a");

	if (!f)
		return;
	if (localtime_r(&now, &tm))
		strftime(stamp, sizeof stamp, "%F %T", &tm);
	else
		snprintf(stamp, sizeof stamp, "-");
	fprintf(f, "%s\tx45wd\t%s\n", stamp, what);
	fclose(f);
}

static int pid_comm_is(int pid, const char *name)
{
	char path[48], comm[32] = "";
	FILE *f;

	if (pid <= 1)
		return 0;
	snprintf(path, sizeof path, "/proc/%d/comm", pid);
	f = fopen(path, "r");
	if (!f)
		return 0;
	if (fgets(comm, sizeof comm, f))
		comm[strcspn(comm, "\r\n")] = '\0';
	fclose(f);
	return !strcmp(comm, name);
}

static int read_pidfile(const char *path)
{
	FILE *f = fopen(path, "r");
	int pid = 0;

	if (!f)
		return 0;
	if (fscanf(f, "%d", &pid) != 1)
		pid = 0;
	fclose(f);
	return pid;
}

/* 0: no pidfile, 1: running, 2: the file is there and the process is not */
static int stack_state(void)
{
	int pid = read_pidfile(STACK_PID);

	if (pid <= 0)
		return access(STACK_PID, F_OK) == 0 ? 2 : 0;
	return kill(pid, 0) == 0 ? 1 : 2;
}

static void wd_feed(void)
{
	if (wdfd >= 0)
		ioctl(wdfd, WDIOC_KEEPALIVE, 0);
}

/* the proper close: the magic character first, so the driver knows it is not a crash of the feeder */
static void wd_close(void)
{
	if (wdfd < 0)
		return;
	if (write(wdfd, "V", 1) < 0)
		logf_("the watchdog did not take its close character: %s", strerror(errno));
	close(wdfd);
	wdfd = -1;
}

struct state {
	long started, fed, trip_at;
	int ui_pid, ui_kills, stack_restarts, gave_up, stalled;
	long beat_age;
};

static void write_status(const struct state *s, int timeout, int armed)
{
	FILE *f = fopen(STATUS ".tmp", "w");

	if (!f)
		return;
	fprintf(f, "pid=%d\nup=%ld\narmed=%d\ntimeout=%d\nfed_ago=%ld\nui_pid=%d\nui_beat_age=%ld\nui_kills=%d\n"
		"stack_restarts=%d\ngave_up=%d\nstalled=%d\n",
		(int)getpid(), mono() - s->started, armed, timeout, s->fed ? mono() - s->fed : -1L, s->ui_pid,
		s->beat_age, s->ui_kills, s->stack_restarts, s->gave_up, s->stalled);
	fclose(f);
	rename(STATUS ".tmp", STATUS);
}

static void trip_marker(const char *why)
{
	FILE *f = fopen(TRIP, "w");

	if (f) {
		fprintf(f, "%s\n", why);
		fflush(f);
		fsync(fileno(f));
		fclose(f);
	}
	sync();
}

int main(int argc, char **argv)
{
	struct state s;
	struct sigaction sa;
	int timeout = 60, use_wd = 1, stall_after = -1, i;
	long killed_at = 0, check_stack_at = 0, tries[STACK_TRIES] = { 0 };
	int killed_pid = 0, bad_stack = 0, ntries = 0, armed = 0;
	FILE *pf;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--timeout") && i + 1 < argc)
			timeout = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--no-wd"))
			use_wd = 0;
		else if (!strcmp(argv[i], "--stall") && i + 1 < argc)
			stall_after = atoi(argv[++i]);
		else {
			fprintf(stderr, "usage: x45wd [--timeout S] [--no-wd] [--stall S]\n");
			return 2;
		}
	}
	if (timeout < 10 || timeout > 120)
		timeout = 60;

	memset(&sa, 0, sizeof sa);
	sa.sa_handler = on_term;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);

	memset(&s, 0, sizeof s);
	s.started = mono();
	pf = fopen(WD_PID, "w");
	if (pf) {
		fprintf(pf, "%d\n", (int)getpid());
		fclose(pf);
	}
	if (use_wd) {
		/* O_CLOEXEC: the programs started from here (the restart of the supervisor) must not inherit the device. A copy
		 * of the descriptor in a long-running child keeps the device open after this process has closed it, so the
		 * proper close never reaches the driver and the unit resets a minute after a clean stop. */
		wdfd = open("/dev/watchdog", O_WRONLY | O_CLOEXEC);
		for (i = 0; wdfd < 0 && errno == EBUSY && i < 5; i++) {   /* the last owner is still letting go */
			sleep(3);
			wdfd = open("/dev/watchdog", O_WRONLY | O_CLOEXEC);
		}
		if (wdfd < 0) {
			logf_("cannot open /dev/watchdog (%s): watching without it", strerror(errno));
		} else {
			if (ioctl(wdfd, WDIOC_SETTIMEOUT, &timeout) < 0)
				logf_("cannot set the watchdog timeout: %s", strerror(errno));
			wd_feed();
			s.fed = mono();
			armed = 1;
		}
	}
	logf_("started (pid %d, watchdog %s, %d s)", (int)getpid(), armed ? "armed" : "not armed", timeout);

	while (!g_stop) {
		long now = mono();
		int pid = 0;
		long beat = 0;
		FILE *f;

		/* ---- the heartbeat of the UI ---- */
		s.ui_pid = 0;
		s.beat_age = -1;
		f = fopen(BEAT, "r");
		if (f) {
			if (fscanf(f, "%d %ld", &pid, &beat) == 2 && pid_comm_is(pid, "x45ui")) {
				s.ui_pid = pid;
				s.beat_age = now - beat;
			}
			fclose(f);
		}
		if (s.ui_pid && s.beat_age > UI_STALE) {
			if (killed_pid != s.ui_pid) {
				logf_("the UI (pid %d) has not answered for %ld s: killed", s.ui_pid, s.beat_age);
				crash_line("the panel UI stopped answering and was killed");
				kill(s.ui_pid, SIGKILL);
				killed_pid = s.ui_pid;
				killed_at = now;
				s.ui_kills++;
			} else if (now - killed_at > UI_DEAD && !s.stalled) {
				logf_("the UI (pid %d) cannot be killed: letting the watchdog reset the unit", s.ui_pid);
				crash_line("the panel UI could not be killed: the watchdog resets the unit");
				trip_marker("ui");
				s.stalled = 1;
			}
		}

		/* ---- the supervisor ---- */
		if (!s.gave_up && access(ENABLED, F_OK) == 0 && access(STOPPED, F_OK) != 0 && !check_stack_at) {
			bad_stack = stack_state() == 2 ? bad_stack + 1 : 0;
			if (bad_stack >= 2) {   /* two looks in a row: not a pid file being written */
				int k, recent = 0;

				bad_stack = 0;
				for (k = 0; k < STACK_TRIES; k++)
					if (tries[k] && now - tries[k] < TRIES_WINDOW)
						recent++;
				if (recent >= STACK_TRIES) {
					logf_("the supervisor died again (%d times within the hour): giving the panel to the vendor UI", recent);
					crash_line("the supervisor died too often: vendor UI restored");
					s.gave_up = 1;
					system("pkill -x x45ui; [ -z \"$(pidof x20)\" ] && /etc/rc5.d/S99x20 start >/dev/null 2>&1; "
					       "[ -z \"$(pidof mdnsd)\" ] && /etc/init.d/mdnsd start >/dev/null 2>&1");
				} else {
					logf_("the supervisor (pid %d) is gone: cleaning up and starting it again", read_pidfile(STACK_PID));
					crash_line("the supervisor died: the stack was started again");
					tries[ntries++ % STACK_TRIES] = now;
					s.stack_restarts++;
					/* its helpers are orphans now (the loops that keep MPD and the web server up, the panel UI): all go, so
					 * that the new supervisor does not find a second copy of anything */
					system("pkill -x x45stack; pkill -x x45ui; pkill -x mpd; pkill -x x45web; pkill -x x45media; "
					       "pkill -x upmpdcli; pkill -x shairport-sync; pkill -x librespot; sleep 2; "
					       "rm -f " STACK_PID "; /etc/init.d/x45new start >/dev/null 2>&1");
					check_stack_at = now + RECOVER_WAIT;
				}
			}
		} else if (check_stack_at && now >= check_stack_at) {
			check_stack_at = 0;
			if (stack_state() != 1) {
				logf_("the supervisor did not come back: giving the panel to the vendor UI");
				crash_line("the supervisor did not come back: vendor UI restored");
				s.gave_up = 1;
				system("[ -z \"$(pidof x20)\" ] && /etc/rc5.d/S99x20 start >/dev/null 2>&1; "
				       "[ -z \"$(pidof mdnsd)\" ] && /etc/init.d/mdnsd start >/dev/null 2>&1");
			} else {
				logf_("the supervisor is back (pid %d)", read_pidfile(STACK_PID));
			}
		}

		/* ---- the watchdog ---- */
		if (stall_after >= 0 && !s.stalled && now - s.started >= stall_after) {
			logf_("test: no longer feeding the watchdog");
			trip_marker("test");
			s.stalled = 1;
		}
		if (!s.stalled) {
			wd_feed();
			s.fed = now;
		}
		write_status(&s, timeout, armed);
		sleep(PERIOD);
	}
	logf_("stopping (%s)", g_stop ? "signal" : "end");
	wd_close();
	unlink(STATUS);
	unlink(WD_PID);
	return 0;
}
