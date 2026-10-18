#define _GNU_SOURCE
/* ext/heartbeat - tells the watchdog service (x45wd) that the main loop of the UI is still turning.
 *
 * Every second /tmp/x45ui.beat is replaced by one line, "pid seconds" (seconds on the monotonic clock). x45wd kills a
 * UI whose line has stood still for 90 s, which is what a main loop stuck in a blocking call looks like from outside;
 * x45stack then starts a new one. This runs inside the same loop that reads the keys and draws the panel, so a beat
 * that is late means the panel is not answering either.
 */
#include "ext.h"

#include <stdio.h>
#include <time.h>
#include <unistd.h>

#define BEAT_FILE "/tmp/x45ui.beat"

extern const struct ui_ext ext_heartbeat;

static void poll(struct ui *u)
{
	static long last_s;
	struct timespec ts;
	FILE *f;

	(void)u;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	if ((long)ts.tv_sec == last_s)
		return;
	last_s = (long)ts.tv_sec;
	f = fopen(BEAT_FILE ".tmp", "w");
	if (!f)
		return;
	fprintf(f, "%d %ld\n", (int)getpid(), (long)ts.tv_sec);
	fclose(f);
	rename(BEAT_FILE ".tmp", BEAT_FILE);
}

const struct ui_ext ext_heartbeat = {
	.name = "heartbeat",
	.poll = poll,
};
