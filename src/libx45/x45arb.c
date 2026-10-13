/* x45arb - see x45.h: one source at a time on the DAC.
 *
 * Kept free of everything else in libx45 so it can be linked into MPD (via
 * libx45clock.a) alongside the clock selector.
 */
#define _GNU_SOURCE
#include "x45.h"

#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define X45_AIRPLAY_MARKER  "/tmp/x45-airplay-active"
#define X45_AIRPLAY_PROCESS "shairport-sync"
#define PREEMPT_WAIT_MS     3000

/* Signal (sig != 0) and/or count the processes whose comm is exactly `name`.
 * Matching on comm, not on a command line, so a shell whose arguments merely
 * mention the name is never hit. Returns how many matched. */
static int match_processes(const char *name, int sig)
{
	DIR *d = opendir("/proc");
	struct dirent *e;
	int n = 0;

	if (!d)
		return 0;
	while ((e = readdir(d))) {
		char path[64], comm[64];
		FILE *f;
		char *end;
		long pid = strtol(e->d_name, &end, 10);

		if (*end || pid <= 1 || pid == (long)getpid())
			continue;
		/* a zombie has already exited and holds nothing; it only lingers in
		 * /proc until its parent reaps it, so it must not count as running */
		snprintf(path, sizeof path, "/proc/%ld/stat", pid);
		f = fopen(path, "r");
		if (f) {
			char st[256], *rp;
			int zombie = 0;

			if (fgets(st, sizeof st, f) && (rp = strrchr(st, ')')) &&
			    rp[1] == ' ' && rp[2] == 'Z')
				zombie = 1;
			fclose(f);
			if (zombie)
				continue;
		}
		snprintf(path, sizeof path, "/proc/%ld/comm", pid);
		f = fopen(path, "r");
		if (!f)
			continue;
		if (fgets(comm, sizeof comm, f)) {
			comm[strcspn(comm, "\n")] = '\0';
			if (!strcmp(comm, name)) {
				n++;
				if (sig)
					kill((pid_t)pid, sig);
			}
		}
		fclose(f);
	}
	closedir(d);
	return n;
}

int x45_preempt_ex(const char *marker, const char *process, int wait_ms)
{
	int waited = 0;

	if (access(marker, F_OK) != 0)
		return 0;
	match_processes(process, SIGTERM);
	unlink(marker);
	/* the PCM is released when the process is gone, not when it is signalled */
	while (waited < wait_ms && match_processes(process, 0) > 0) {
		usleep(20000);
		waited += 20;
	}
	return 1;
}

int x45_airplay_preempt(void)
{
	return x45_preempt_ex(X45_AIRPLAY_MARKER, X45_AIRPLAY_PROCESS, PREEMPT_WAIT_MS);
}
