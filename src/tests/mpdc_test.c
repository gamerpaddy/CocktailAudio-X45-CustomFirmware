/* mpdc protocol test against a scripted fake MPD server.
 *
 * The server is a forked child listening on an ephemeral 127.0.0.1 port, so the
 * test needs no real MPD and touches no hardware; it builds natively and for
 * ARM. It exercises what could go wrong on the wire rather than the happy path:
 * ACK replies, a line far longer than the buffer, a server that vanishes
 * mid-reply, a wrong greeting, and arguments that try to smuggle a second
 * command.
 */
#define _GNU_SOURCE
#include "../libx45/mpdc.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static int fails, checks;

#define CHECK(cond, ...) do { \
	checks++; \
	if (!(cond)) { fails++; printf("FAIL line %d: ", __LINE__); \
		printf(__VA_ARGS__); printf("\n"); } \
} while (0)

enum mode { MODE_NORMAL, MODE_BAD_GREETING, MODE_DROP_MID_REPLY, MODE_ONE_COMMAND };

static void put(int fd, const char *s)
{
	size_t n = strlen(s);
	while (n) {
		ssize_t w = send(fd, s, n, MSG_NOSIGNAL);
		if (w <= 0)
			return;
		s += w;
		n -= (size_t)w;
	}
}

static void serve_one(int cfd, enum mode m)
{
	char buf[2048], *nl;
	size_t have = 0;

	if (m == MODE_BAD_GREETING) {
		put(cfd, "HELLO this is not mpd\n");
		return;
	}
	put(cfd, "OK MPD 0.23.15\n");

	for (;;) {
		ssize_t r = recv(cfd, buf + have, sizeof buf - have - 1, 0);
		if (r <= 0)
			return;
		have += (size_t)r;
		while ((nl = memchr(buf, '\n', have))) {
			*nl = '\0';
			if (m == MODE_DROP_MID_REPLY) {
				put(cfd, "state: play\nvolume: 4");   /* no newline, no OK */
				return;
			}
			if (!strcmp(buf, "status")) {
				put(cfd, "volume: 37\nrepeat: 0\nrandom: 1\nsingle: oneshot\n"
					 "consume: 0\nplaylistlength: 12\nstate: play\nsong: 3\n"
					 "elapsed: 61.250\nduration: 245.500\nbitrate: 1411\n"
					 "audio: 96000:24:2\nOK\n");
			} else if (!strcmp(buf, "currentsong")) {
				put(cfd, "file: Music/A/b.flac\nTitle: Caf\xc3\xa9 del Mar\n"
					 "ARTIST: The Artists\nAlbum: Long Play\nOK\n");
			} else if (!strcmp(buf, "lsinfo \"\"")) {
				put(cfd, "directory: Jazz\nLast-Modified: 2026-01-01T00:00:00Z\n"
					 "directory: Rock/Live\nfile: one.flac\nTitle: One\n"
					 "playlist: mix.m3u\nfile: two.flac\nfile: three.flac\nOK\n");
			} else if (!strcmp(buf, "lsinfo \"sorted\"")) {
				/* deliberately scrambled: database order is not display order */
				put(cfd, "file: s/track 10.flac\ndirectory: s/b dir\n"
					 "file: s/Track 2.flac\ndirectory: s/10 dir\n"
					 "file: s/track 02.flac\ndirectory: s/A dir\n"
					 "file: s/track 1.flac\ndirectory: s/9 dir\n"
					 "file: s/Zed.flac\nfile: s/alpha.flac\n"
					 /* padding as a tiebreak only: "t 2" before "t 02" although
					  * plain strcmp says the opposite ... */
					 "file: s/t 02.flac\nfile: s/t 2.flac\n"
					 /* ... and the rest of the name outranks padding */
					 "file: s/x 2 b.flac\nfile: s/x 02 a.flac\nOK\n");
			} else if (!strcmp(buf, "lsinfo \"big\"")) {
				/* 300 files in DESCENDING order: cutting before sorting would
				 * keep f299..f295 instead of f000..f004 */
				char line[64];
				int k;
				for (k = 299; k >= 0; k--) {
					snprintf(line, sizeof line, "file: big/f%03d.flac\n", k);
					put(cfd, line);
				}
				put(cfd, "OK\n");
			} else if (!strcmp(buf, "lsinfo \"a \\\"quoted\\\" dir\"")) {
				put(cfd, "file: a \"quoted\" dir/x.flac\nOK\n");
			} else if (!strncmp(buf, "password ", 9)) {
				if (!strcmp(buf, "password \"sekret\""))
					put(cfd, "OK\n");
				else
					put(cfd, "ACK [3@0] {password} incorrect password\n");
			} else if (!strcmp(buf, "nosuch")) {
				put(cfd, "ACK [5@0] {nosuch} unknown command \"nosuch\"\n");
			} else if (!strcmp(buf, "biglines")) {
				char *big = malloc(6001);
				memset(big, 'x', 6000);
				big[6000] = '\0';
				put(cfd, "Title: ");
				put(cfd, big);
				put(cfd, "\nstate: pause\nOK\n");
				free(big);
			} else if (!strcmp(buf, "ping")) {
				put(cfd, "OK\n");
			} else {
				put(cfd, "ACK [5@0] {} unexpected\n");
			}
			have -= (size_t)(nl - buf) + 1;
			memmove(buf, nl + 1, have);
			if (m == MODE_ONE_COMMAND)
				return;   /* hangs up, like MPD does with a client that has been idle too long */
		}
	}
}

/* returns the port; the child serves one connection then exits */
static int start_server(enum mode m, pid_t *child)
{
	struct sockaddr_in sa;
	socklen_t len = sizeof sa;
	int lfd = socket(AF_INET, SOCK_STREAM, 0), one = 1;

	setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	memset(&sa, 0, sizeof sa);
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(lfd, (struct sockaddr *)&sa, sizeof sa) < 0 || listen(lfd, 1) < 0) {
		perror("server setup");
		exit(2);
	}
	getsockname(lfd, (struct sockaddr *)&sa, &len);

	*child = fork();
	if (*child == 0) {
		int cfd = accept(lfd, NULL, NULL);
		if (cfd >= 0) {
			serve_one(cfd, m);
			close(cfd);
		}
		_exit(0);
	}
	close(lfd);
	return ntohs(sa.sin_port);
}

/* serves `n` connections one after the other, each for one command only */
static int start_server_idle(int n, pid_t *child)
{
	struct sockaddr_in sa;
	socklen_t len = sizeof sa;
	int lfd = socket(AF_INET, SOCK_STREAM, 0), one = 1;

	setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	memset(&sa, 0, sizeof sa);
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(lfd, (struct sockaddr *)&sa, sizeof sa) < 0 || listen(lfd, 1) < 0) {
		perror("server setup");
		exit(2);
	}
	getsockname(lfd, (struct sockaddr *)&sa, &len);
	*child = fork();
	if (*child == 0) {
		while (n-- > 0) {
			int cfd = accept(lfd, NULL, NULL);

			if (cfd < 0)
				break;
			serve_one(cfd, MODE_ONE_COMMAND);
			close(cfd);
		}
		_exit(0);
	}
	close(lfd);
	return ntohs(sa.sin_port);
}

static void stop_server(pid_t pid)
{
	int st;
	kill(pid, SIGTERM);
	waitpid(pid, &st, 0);
}

int main(void)
{
	struct mpdc c;
	pid_t pid;
	int port, r;

	signal(SIGPIPE, SIG_IGN);

	/* ---------------------------------------------------- normal session --- */
	port = start_server(MODE_NORMAL, &pid);
	r = mpdc_open(&c, "127.0.0.1", port);
	CHECK(r == 0, "open: %d", r);
	if (r == 0) {
		struct mpdc_status st;
		struct mpdc_song sg;
		struct mpdc_entry *rows = NULL;
		int n, trunc = -1;
		char err[160];

		r = mpdc_status(&c, &st);
		CHECK(r == 0, "status rc %d", r);
		CHECK(!strcmp(st.state, "play"), "state '%s'", st.state);
		CHECK(st.volume == 37, "volume %d", st.volume);
		CHECK(st.song == 3 && st.length == 12, "song %d len %d", st.song, st.length);
		CHECK(st.elapsed > 61.2 && st.elapsed < 61.3, "elapsed %f", st.elapsed);
		CHECK(st.duration > 245.4 && st.duration < 245.6, "duration %f", st.duration);
		CHECK(!strcmp(st.audio, "96000:24:2"), "audio '%s'", st.audio);
		CHECK(st.bitrate == 1411, "bitrate %d", st.bitrate);
		CHECK(st.random == 1 && st.repeat == 0, "random/repeat");
		CHECK(st.single == 1, "single 'oneshot' should read as on");
		CHECK(st.consume == 0, "consume");

		r = mpdc_currentsong(&c, &sg);
		CHECK(r == 0, "currentsong rc %d", r);
		CHECK(!strcmp(sg.file, "Music/A/b.flac"), "file '%s'", sg.file);
		CHECK(!strcmp(sg.title, "Caf\xc3\xa9 del Mar"), "title '%s'", sg.title);
		CHECK(!strcmp(sg.artist, "The Artists"), "tag name case: artist '%s'", sg.artist);
		CHECK(!strcmp(sg.album, "Long Play"), "album '%s'", sg.album);

		n = mpdc_lsinfo(&c, "", &rows, 64, &trunc);
		CHECK(n == 5, "lsinfo rows %d (playlist record must be skipped)", n);
		if (n == 5) {
			CHECK(rows[0].is_dir && !strcmp(rows[0].name, "Jazz"), "row0");
			CHECK(rows[1].is_dir && !strcmp(rows[1].path, "Rock/Live") &&
			      !strcmp(rows[1].name, "Live"), "row1 path/name split");
			/* the server sent one, two, three; display order is alphabetical */
			CHECK(!rows[2].is_dir && !strcmp(rows[2].name, "one.flac"), "row2");
			CHECK(!rows[3].is_dir && !strcmp(rows[3].name, "three.flac"), "row3");
			CHECK(!rows[4].is_dir && !strcmp(rows[4].name, "two.flac"), "row4");
			CHECK(trunc == 0, "not truncated");
		}
		free(rows);

		/* natural order, directories first, case-insensitive, padding as tiebreak */
		rows = NULL;
		n = mpdc_lsinfo(&c, "sorted", &rows, 64, &trunc);
		CHECK(n == 14, "sorted rows %d", n);
		if (n == 14) {
			static const char *const want[14] = {
				"9 dir", "10 dir", "A dir", "b dir",
				"alpha.flac", "t 2.flac", "t 02.flac",
				"track 1.flac", "Track 2.flac", "track 02.flac", "track 10.flac",
				"x 02 a.flac", "x 2 b.flac", "Zed.flac",
			};
			int k;
			for (k = 0; k < 14; k++)
				CHECK(!strcmp(rows[k].name, want[k]), "sorted[%d] = '%s', want '%s'",
				      k, rows[k].name, want[k]);
			CHECK(rows[3].is_dir && !rows[4].is_dir, "dirs before files");
		}
		free(rows);

		/* sort first, then truncate: the FIRST five in display order, not in
		 * server order */
		rows = NULL;
		n = mpdc_lsinfo(&c, "big", &rows, 5, &trunc);
		CHECK(n == 5 && trunc == 1, "big: n=%d trunc=%d", n, trunc);
		if (n == 5) {
			CHECK(!strcmp(rows[0].name, "f000.flac") && !strcmp(rows[4].name, "f004.flac"),
			      "big kept '%s'..'%s', want f000..f004", rows[0].name, rows[4].name);
		}
		free(rows);
		rows = NULL;
		n = mpdc_lsinfo(&c, "big", &rows, 1000, &trunc);
		CHECK(n == 300 && trunc == 0, "big, roomy: n=%d trunc=%d", n, trunc);
		free(rows);
		CHECK(mpdc_lsinfo(&c, "", &rows, 0, &trunc) == -EINVAL, "max 0 refused");

		rows = NULL;
		n = mpdc_lsinfo(&c, "", &rows, 3, &trunc);
		CHECK(n == 3 && trunc == 1, "truncation: n=%d trunc=%d", n, trunc);
		free(rows);

		rows = NULL;
		n = mpdc_lsinfo(&c, "a \"quoted\" dir", &rows, 8, &trunc);
		CHECK(n == 1 && !strcmp(rows[0].path, "a \"quoted\" dir/x.flac"),
		      "quoted path round trip: n=%d", n);
		free(rows);

		/* ACK: reported, connection stays usable */
		r = mpdc_run(&c, "nosuch", NULL, NULL, err, sizeof err);
		CHECK(r == -EIO, "ACK rc %d", r);
		CHECK(strstr(err, "unknown command") != NULL, "ACK text '%s'", err);
		CHECK(err[0] != '[' && strstr(err, "{nosuch}") == NULL,
		      "ACK prefix must be stripped: '%s'", err);
		CHECK(strstr(c.last_error, "unknown command") != NULL,
		      "last_error '%s'", c.last_error);
		CHECK(mpdc_is_open(&c), "ACK must not close the connection");
		r = mpdc_cmd(&c, "ping");
		CHECK(r == 0, "ping after ACK rc %d", r);
		CHECK(c.last_error[0] == '\0', "last_error must clear on success: '%s'",
		      c.last_error);

		/* a 6000-byte line: must not desync or overflow */
		{
			struct mpdc_song big;
			r = mpdc_run(&c, "biglines", NULL, NULL, NULL, 0);
			CHECK(r == 0, "biglines rc %d", r);
			(void)big;
			r = mpdc_cmd(&c, "ping");
			CHECK(r == 0, "stream still in sync after a huge line: rc %d", r);
		}

		/* command injection: a newline must be refused before it is sent */
		r = mpdc_run(&c, "ping\nclear", NULL, NULL, NULL, 0);
		CHECK(r == -EINVAL, "embedded newline rc %d", r);
		r = mpdc_cmd(&c, "ping");
		CHECK(r == 0, "connection intact after refused command: rc %d", r);

		mpdc_close(&c);
		CHECK(!mpdc_is_open(&c), "close");
		r = mpdc_cmd(&c, "ping");
		CHECK(r == -ENOTCONN, "command on a closed handle rc %d", r);
	}
	stop_server(pid);

	/* ------------------------------------------------------- bad greeting --- */
	port = start_server(MODE_BAD_GREETING, &pid);
	r = mpdc_open(&c, "127.0.0.1", port);
	CHECK(r == -EPROTO, "bad greeting rc %d", r);
	CHECK(!mpdc_is_open(&c), "bad greeting must leave the handle closed");
	stop_server(pid);

	/* ------------------------------------------------------------ password --- */
	port = start_server(MODE_NORMAL, &pid);
	mpdc_set_password("sekret");
	r = mpdc_open(&c, "127.0.0.1", port);
	CHECK(r == 0, "open with the right password rc %d", r);
	if (r == 0) {
		r = mpdc_cmd(&c, "ping");
		CHECK(r == 0, "command after authenticating rc %d", r);
		mpdc_close(&c);
	}
	stop_server(pid);
	port = start_server(MODE_NORMAL, &pid);
	mpdc_set_password("wrong");
	r = mpdc_open(&c, "127.0.0.1", port);
	CHECK(r == -EACCES, "wrong password rc %d", r);
	CHECK(!mpdc_is_open(&c), "a rejected password must leave the handle closed");
	mpdc_set_password(NULL);
	stop_server(pid);

	/* ----------------------------------------------- server dies mid-reply --- */
	port = start_server(MODE_DROP_MID_REPLY, &pid);
	r = mpdc_open(&c, "127.0.0.1", port);
	CHECK(r == 0, "open (drop server) %d", r);
	if (r == 0) {
		struct mpdc_status st;
		r = mpdc_status(&c, &st);
		CHECK(r < 0 && r != -EIO, "truncated reply rc %d", r);
		CHECK(!mpdc_is_open(&c), "a broken stream must close the handle");
	}
	stop_server(pid);

	/* --------------------------------------- the server hangs up an idle client --- */
	port = start_server_idle(3, &pid);
	r = mpdc_open(&c, "127.0.0.1", port);
	CHECK(r == 0, "open (idle server) %d", r);
	if (r == 0) {
		struct mpdc_status st;

		r = mpdc_cmd(&c, "ping");
		CHECK(r == 0, "first command rc %d", r);
		usleep(100000);   /* the server has closed this connection by now */
		r = mpdc_status(&c, &st);
		CHECK(r == 0, "the command after the hang-up must run on a new connection: rc %d", r);
		CHECK(!strcmp(st.state, "play") && st.volume == 37, "and return the real answer: '%s' %d", st.state, st.volume);
		CHECK(mpdc_is_open(&c), "the handle stays open after the reconnect");
		usleep(100000);
		r = mpdc_cmd(&c, "ping");
		CHECK(r == 0, "second hang-up, same again: rc %d", r);
		usleep(100000);
		r = mpdc_cmd(&c, "ping");   /* the server has served its three and is gone */
		CHECK(r < 0, "no server any more must be an error: rc %d", r);
		CHECK(!mpdc_is_open(&c), "and leave the handle closed");
		r = mpdc_cmd(&c, "ping");
		CHECK(r == -ENOTCONN, "closed stays closed (the UI reconnects with mpdc_open): rc %d", r);
	}
	stop_server(pid);

	/* ------------------------------------------------------ nothing listening --- */
	r = mpdc_open(&c, "127.0.0.1", 1);   /* port 1: refused */
	CHECK(r == -ECONNREFUSED, "refused rc %d", r);
	r = mpdc_open(&c, "not-an-ip", 6601);
	CHECK(r == -EINVAL, "non-numeric host rc %d", r);

	/* ----------------------------------------------------------- quoting --- */
	{
		char q[64];
		CHECK(mpdc_quote(q, sizeof q, "plain") == 7 && !strcmp(q, "\"plain\""), "plain");
		CHECK(mpdc_quote(q, sizeof q, "a\"b\\c") > 0 && !strcmp(q, "\"a\\\"b\\\\c\""),
		      "escapes: %s", q);
		CHECK(mpdc_quote(q, sizeof q, "") == 2 && !strcmp(q, "\"\""), "empty");
		CHECK(mpdc_quote(q, sizeof q, "line\nbreak") == -EINVAL, "newline refused");
		CHECK(mpdc_quote(q, 6, "toolong") == -ENAMETOOLONG, "overflow refused");
		CHECK(mpdc_quote(q, 8, "\"\"\"") == -ENAMETOOLONG,
		      "escaping must be counted in the length check");
	}

	/* ------------------------------------------------- describe_audio --- */
	{
		char d[64];
		CHECK(!strcmp(mpdc_describe_audio("44100:16:2", d, sizeof d), "44.1 kHz  16 bit  stereo"), "44.1: %s", d);
		CHECK(!strcmp(mpdc_describe_audio("96000:24:2", d, sizeof d), "96 kHz  24 bit  stereo"), "96: %s", d);
		CHECK(!strcmp(mpdc_describe_audio("192000:f:2", d, sizeof d), "192 kHz  float  stereo"), "float: %s", d);
		CHECK(!strcmp(mpdc_describe_audio("44100:24:1", d, sizeof d), "44.1 kHz  24 bit  mono"), "mono: %s", d);
		CHECK(!strcmp(mpdc_describe_audio("dsd64:2", d, sizeof d), "DSD64  stereo"), "dsd64: %s", d);
		CHECK(!strcmp(mpdc_describe_audio("dsd128:2", d, sizeof d), "DSD128  stereo"), "dsd128: %s", d);
		CHECK(!strcmp(mpdc_describe_audio("dsd256:2", d, sizeof d), "DSD256  stereo  to PCM"), "dsd256: %s", d);
		CHECK(!strcmp(mpdc_describe_audio("", d, sizeof d), "-"), "empty: %s", d);
		CHECK(!strcmp(mpdc_describe_audio("garbage", d, sizeof d), "garbage"), "garbage: %s", d);
	}

	printf("mpdc_test: %d/%d pass\n", checks - fails, checks);
	return fails ? 1 : 0;
}
