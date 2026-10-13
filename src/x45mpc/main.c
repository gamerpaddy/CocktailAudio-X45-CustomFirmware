/* x45mpc  -  a minimal MPD command-line client for the X45.
 *
 * Exists to test libx45's mpdc against the real daemon on the device, and to
 * be useful from an ssh session. Not a general `mpc`.
 *
 *   x45mpc [-h host] [-p port] [-P password] <command> [args]
 *   (the password may also come from the environment: MPD_PASSWORD)
 *
 *   status            state, volume, position, audio format
 *   current           the playing song's tags
 *   ls [path]         list a directory (d = directory, f = file)
 *   queue             show the play queue
 *   add <path>        append a file or directory to the queue
 *   clear             empty the queue
 *   play [pos]        start playing (queue position, 0-based)
 *   pause | toggle    pause / flip between play and pause
 *   stop | next | prev
 *   vol <0-100>       hardware volume (the board's raw 0-100)
 *   update            look for new and changed files
 *   rescan            re-read every file (needed after tag support changes)
 */
#define _GNU_SOURCE
#include "../libx45/mpdc.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_kv(const char *k, const char *v, void *ud)
{
	(void)ud;
	printf("%s: %s\n", k, v);
}

static int quoted(char *out, size_t n, const char *fmt, const char *arg)
{
	char q[2 * MPDC_PATH_MAX + 8];
	int r = mpdc_quote(q, sizeof q, arg);

	if (r < 0)
		return r;
	if ((size_t)snprintf(out, n, fmt, q) >= n)   /* a cut-short quoted path would be a broken command */
		return -ENAMETOOLONG;
	return 0;
}

/* a whole decimal number within lo..hi, else -1 */
static long number(const char *s, long lo, long hi)
{
	char *e;
	long v = strtol(s, &e, 10);

	return *s && !*e && v >= lo && v <= hi ? v : -1;
}

int main(int argc, char **argv)
{
	const char *host = MPDC_DEFAULT_HOST;
	int port = MPDC_DEFAULT_PORT, i = 1, r;
	struct mpdc c;

	mpdc_set_password(getenv("MPD_PASSWORD"));
	char err[160], cmd[400];

	while (i < argc && argv[i][0] == '-') {
		if (!strcmp(argv[i], "-h") && i + 1 < argc)
			host = argv[++i];
		else if (!strcmp(argv[i], "-p") && i + 1 < argc)
			port = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-P") && i + 1 < argc)
			mpdc_set_password(argv[++i]);
		else
			goto usage;
		i++;
	}
	if (i >= argc)
		goto usage;

	if ((r = mpdc_open(&c, host, port)) < 0) {
		fprintf(stderr, "x45mpc: connect %s:%d: %s\n", host, port, strerror(-r));
		return 1;
	}

	{
		const char *op = argv[i];
		const char *arg = i + 1 < argc ? argv[i + 1] : NULL;

		if (!strcmp(op, "status")) {
			struct mpdc_status st;
			char desc[64];

			if ((r = mpdc_status(&c, &st)) < 0)
				goto fail;
			printf("state    %s\n", st.state);
			printf("volume   %d\n", st.volume);
			printf("queue    song %d of %d\n", st.song, st.length);
			printf("elapsed  %.1f / %.1f s\n", st.elapsed, st.duration);
			printf("audio    %s  (%s)\n", st.audio[0] ? st.audio : "-",
			       mpdc_describe_audio(st.audio, desc, sizeof desc));
			if (st.error[0])
				printf("error    %s\n", st.error);
		} else if (!strcmp(op, "current")) {
			struct mpdc_song sg;

			if ((r = mpdc_currentsong(&c, &sg)) < 0)
				goto fail;
			printf("file    %s\ntitle   %s\nartist  %s\nalbum   %s\n",
			       sg.file, sg.title, sg.artist, sg.album);
		} else if (!strcmp(op, "ls")) {
			struct mpdc_entry *rows;
			int n, trunc = 0, k;

			n = mpdc_lsinfo(&c, arg ? arg : "", &rows, 512, &trunc);
			if (n < 0) {
				r = n;
				goto fail;
			}
			for (k = 0; k < n; k++)
				printf("%c  %s\n", rows[k].is_dir ? 'd' : 'f', rows[k].path);
			if (trunc)
				printf("(truncated at 512 entries)\n");
			free(rows);
		} else if (!strcmp(op, "queue")) {
			r = mpdc_run(&c, "playlistinfo", print_kv, NULL, err, sizeof err);
			if (r < 0)
				goto fail;
		} else if (!strcmp(op, "add") && arg) {
			if ((r = quoted(cmd, sizeof cmd, "add %s", arg)) < 0)
				goto fail;
			r = mpdc_run(&c, cmd, NULL, NULL, err, sizeof err);
			if (r < 0)
				goto fail;
		} else if (!strcmp(op, "clear") || !strcmp(op, "stop") ||
			   !strcmp(op, "next") || !strcmp(op, "update") || !strcmp(op, "rescan") ||
			   !strcmp(op, "pause")) {
			snprintf(cmd, sizeof cmd, "%s", op);
			if (!strcmp(op, "pause"))
				snprintf(cmd, sizeof cmd, "pause 1");
			if ((r = mpdc_run(&c, cmd, NULL, NULL, err, sizeof err)) < 0)
				goto fail;
		} else if (!strcmp(op, "prev")) {
			if ((r = mpdc_run(&c, "previous", NULL, NULL, err, sizeof err)) < 0)
				goto fail;
		} else if (!strcmp(op, "toggle")) {
			if ((r = mpdc_run(&c, "pause", NULL, NULL, err, sizeof err)) < 0)
				goto fail;
		} else if (!strcmp(op, "play")) {
			if (arg) {
				long pos = number(arg, 0, 1000000);

				if (pos < 0) {
					r = -EINVAL;
					goto fail;
				}
				snprintf(cmd, sizeof cmd, "play %ld", pos);
			} else {
				snprintf(cmd, sizeof cmd, "play");
			}
			if ((r = mpdc_run(&c, cmd, NULL, NULL, err, sizeof err)) < 0)
				goto fail;
		} else if (!strcmp(op, "vol") && arg) {
			long v = number(arg, 0, 100);

			if (v < 0) {   /* "vol loud" is not "vol 0" */
				r = -EINVAL;
				goto fail;
			}
			snprintf(cmd, sizeof cmd, "setvol %ld", v);
			if ((r = mpdc_run(&c, cmd, NULL, NULL, err, sizeof err)) < 0)
				goto fail;
		} else {
			mpdc_close(&c);
			goto usage;
		}
	}
	mpdc_close(&c);
	return 0;

fail:
	fprintf(stderr, "x45mpc: %s\n", r == -EIO ? c.last_error : strerror(-r));
	mpdc_close(&c);
	return 1;

usage:
	fputs("usage: x45mpc [-h host] [-p port] status|current|ls [path]|queue|"
	      "add <path>|clear|play [pos]|pause|toggle|stop|next|prev|vol <0-100>|update|rescan\n",
	      stderr);
	return 2;
}
