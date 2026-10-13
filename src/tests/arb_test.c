/* x45_preempt_ex: cutting an AirPlay session so another source can have the DAC.
 *
 * Uses real processes: the child renames itself with prctl(PR_SET_NAME) so it
 * looks to /proc exactly like a shairport-sync would. Linux-only, no hardware.
 */
#define _GNU_SOURCE
#include "../libx45/x45.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int fails, checks;

#define CHECK(cond, ...) do { \
	checks++; \
	if (!(cond)) { fails++; printf("FAIL line %d: ", __LINE__); \
		printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static long now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* A child that calls itself `name` and waits. ignore_term: survives SIGTERM. */
static pid_t spawn(const char *name, int ignore_term)
{
	int ready[2];
	pid_t p;
	char b;

	if (pipe(ready) < 0)
		exit(2);
	p = fork();
	if (p == 0) {
		close(ready[0]);
		prctl(PR_SET_NAME, name, 0, 0, 0);
		if (ignore_term)
			signal(SIGTERM, SIG_IGN);
		write(ready[1], "x", 1);       /* the name is set: parent may proceed */
		for (;;)
			pause();
	}
	close(ready[1]);
	read(ready[0], &b, 1);
	close(ready[0]);
	return p;
}

static int alive(pid_t p)
{
	int st;

	if (waitpid(p, &st, WNOHANG) == p)   /* reaped: it was dead */
		return 0;
	return kill(p, 0) == 0;
}

static void reap(pid_t p)
{
	int st;

	kill(p, SIGKILL);
	waitpid(p, &st, 0);
}

static void touch(const char *path)
{
	int fd = open(path, O_WRONLY | O_CREAT, 0644);

	if (fd >= 0)
		close(fd);
}

int main(void)
{
	char marker[] = "/tmp/x45_arb_test_XXXXXX";
	int fd = mkstemp(marker), r;
	pid_t p, other;
	long t0;

	if (fd < 0) {
		perror("mkstemp");
		return 2;
	}
	close(fd);
	unlink(marker);

	/* 1. no marker: nothing happens, even though the process exists */
	p = spawn("x45arbtest", 0);
	r = x45_preempt_ex(marker, "x45arbtest", 500);
	CHECK(r == 0, "no marker: returned %d", r);
	CHECK(alive(p), "no marker: process must be left alone");

	/* 2. marker + live process: it is terminated, the marker removed */
	touch(marker);
	t0 = now_ms();
	r = x45_preempt_ex(marker, "x45arbtest", 3000);
	CHECK(r == 1, "live session: returned %d", r);
	CHECK(!alive(p), "live session: process must be gone");
	CHECK(access(marker, F_OK) != 0, "live session: marker must be removed");
	CHECK(now_ms() - t0 < 1500, "live session: took %ld ms", now_ms() - t0);

	/* 3. marker but no such process: removed, returns at once */
	touch(marker);
	t0 = now_ms();
	r = x45_preempt_ex(marker, "x45arbtest", 3000);
	CHECK(r == 1, "stale marker: returned %d", r);
	CHECK(access(marker, F_OK) != 0, "stale marker must be removed");
	CHECK(now_ms() - t0 < 500, "stale marker: should not wait, took %ld ms", now_ms() - t0);

	/* 4. a process that will not die: the wait is bounded, nothing hangs */
	p = spawn("x45arbtest", 1);
	touch(marker);
	t0 = now_ms();
	r = x45_preempt_ex(marker, "x45arbtest", 300);
	CHECK(r == 1, "stubborn process: returned %d", r);
	CHECK(now_ms() - t0 >= 280 && now_ms() - t0 < 1500,
	      "stubborn process: waited %ld ms, wanted ~300", now_ms() - t0);
	CHECK(alive(p), "stubborn process ignored SIGTERM, so it is still there");
	reap(p);

	/* 5. only an exact name match is touched */
	p = spawn("x45arbtest", 0);
	other = spawn("x45arbtestX", 0);          /* longer name sharing the prefix */
	touch(marker);
	r = x45_preempt_ex(marker, "x45arbtest", 3000);
	CHECK(r == 1, "exact match: returned %d", r);
	CHECK(!alive(p), "exact match: the named process goes");
	CHECK(alive(other), "exact match: a different name must survive");
	reap(other);

	/* 6. every instance goes, not just the first */
	p = spawn("x45arbtest", 0);
	other = spawn("x45arbtest", 0);
	touch(marker);
	r = x45_preempt_ex(marker, "x45arbtest", 3000);
	CHECK(r == 1 && !alive(p) && !alive(other), "two instances: both must go");

	/* 7. the real entry point is a no-op when there is no AirPlay session */
	if (access("/tmp/x45-airplay-active", F_OK) == 0)
		printf("skip: a real AirPlay session is live, not touching its marker\n");
	else
		CHECK(x45_airplay_preempt() == 0, "x45_airplay_preempt() with no marker");

	printf("arb_test: %d/%d pass\n", checks - fails, checks);
	return fails ? 1 : 0;
}
