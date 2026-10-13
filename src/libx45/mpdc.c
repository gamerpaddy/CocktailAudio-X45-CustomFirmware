/* mpdc  -  see mpdc.h. */
#define _GNU_SOURCE
#include "mpdc.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define IO_TIMEOUT_S 2

/* Bounded string copy; clipping is the intent (tags and ACK text can be long). */
static void copy(char *dst, size_t cap, const char *src)
{
	size_t n = strlen(src);

	if (n >= cap)
		n = cap - 1;
	memcpy(dst, src, n);
	dst[n] = '\0';
}

/* ------------------------------------------------------------------- wire --- */

static int send_all(int fd, const char *p, size_t n)
{
	while (n) {
		ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
		if (w < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		p += w;
		n -= (size_t)w;
	}
	return 0;
}

/* Reads one line into `line` (newline stripped). A line longer than `cap` is
 * truncated to fit but fully consumed, so one huge tag cannot desynchronise
 * the stream. Returns the length, or -errno (-ECONNRESET on EOF). */
static int read_line(struct mpdc *c, char *line, size_t cap)
{
	size_t out = 0;

	for (;;) {
		while (c->rpos < c->rlen) {
			char ch = c->rbuf[c->rpos++];
			if (ch == '\n') {
				line[out] = '\0';
				return (int)out;
			}
			if (out + 1 < cap)
				line[out++] = ch;
		}
		c->rpos = c->rlen = 0;
		{
			ssize_t r = recv(c->fd, c->rbuf, sizeof c->rbuf, 0);
			if (r < 0) {
				if (errno == EINTR)
					continue;
				if (errno == EAGAIN || errno == EWOULDBLOCK)
					return -ETIMEDOUT;
				return -errno;
			}
			if (r == 0)
				return -ECONNRESET;
			c->rlen = (size_t)r;
		}
	}
}

/* "[50@0] {lsinfo} No such directory" -> "No such directory". The code and
 * command name are noise to a person reading a panel or a terminal. */
static const char *ack_message(const char *ack)
{
	const char *m = strstr(ack, "} ");

	return m ? m + 2 : ack;
}

static char g_password[128];

void mpdc_set_password(const char *pw)
{
	snprintf(g_password, sizeof g_password, "%s", pw ? pw : "");
}

const char *mpdc_password(void)
{
	return g_password;
}

int mpdc_open(struct mpdc *c, const char *host, int port)
{
	struct sockaddr_in sa;
	struct timeval tv = { IO_TIMEOUT_S, 0 };
	char line[128];
	int fd, one = 1, r;

	memset(c, 0, sizeof *c);
	c->fd = -1;

	memset(&sa, 0, sizeof sa);
	sa.sin_family = AF_INET;
	sa.sin_port = htons((unsigned short)port);
	/* numeric IPv4 only: the UI talks to the local daemon, and resolving a
	 * name would drag the resolver into a static binary for nothing */
	if (inet_pton(AF_INET, host, &sa.sin_addr) != 1)
		return -EINVAL;

	fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -errno;
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

	if (connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
		r = -errno;
		close(fd);
		return r;
	}
	c->fd = fd;

	r = read_line(c, line, sizeof line);
	if (r < 0) {
		mpdc_close(c);
		return r;
	}
	if (strncmp(line, "OK MPD ", 7) != 0) {
		mpdc_close(c);
		return -EPROTO;
	}
	if (g_password[0]) {
		char q[300];

		if (mpdc_quote(q, sizeof q, g_password) < 0 ||
		    mpdc_cmd(c, "password %s", q) < 0) {
			mpdc_close(c);
			return -EACCES;
		}
	}
	copy(c->host, sizeof c->host, host);
	c->port = port;
	return 0;
}

/* MPD closes a client that has been idle for its connection_timeout; the FIN is already
 * waiting on the socket, so a peek finds out without sending anything. */
static int peer_gone(int fd)
{
	char b;
	ssize_t r = recv(fd, &b, 1, MSG_PEEK | MSG_DONTWAIT);

	if (r == 0)
		return 1;
	if (r < 0)
		return errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR;
	return 0;
}

/* A new connection to the server this handle was opened on; the handle is closed when it fails. */
static int reopen(struct mpdc *c)
{
	char host[sizeof c->host];
	int port = c->port;

	memcpy(host, c->host, sizeof host);
	mpdc_close(c);
	return mpdc_open(c, host, port);
}

void mpdc_close(struct mpdc *c)
{
	if (c->fd >= 0)
		close(c->fd);
	c->fd = -1;
	c->rpos = c->rlen = 0;
}

int mpdc_run(struct mpdc *c, const char *cmd, mpdc_kv_fn cb, void *ud,
	     char *err, size_t errlen)
{
	char line[1024], *sep;
	size_t n = strlen(cmd);
	int r;

	if (err && errlen)
		err[0] = '\0';
	c->last_error[0] = '\0';
	if (c->fd < 0)
		return -ENOTCONN;
	if (n && cmd[n - 1] == '\n')
		return -EINVAL;
	if (strchr(cmd, '\n'))
		return -EINVAL; /* would smuggle a second command */

	{
		char out[1100];
		if (n + 2 > sizeof out)
			return -ENAMETOOLONG;
		/* idle for longer than the server's patience: it has hung up, start over on a new connection */
		if (c->port > 0 && peer_gone(c->fd) && (r = reopen(c)) < 0)
			return r;
		memcpy(out, cmd, n);
		out[n] = '\n';
		r = send_all(c->fd, out, n + 1);
		/* the hang-up came between the check and the send: nothing was delivered, so once more is safe */
		if (r < 0 && c->port > 0 && (r == -EPIPE || r == -ECONNRESET) && reopen(c) == 0)
			r = send_all(c->fd, out, n + 1);
		if (r < 0) {
			mpdc_close(c);
			return r;
		}
	}

	for (;;) {
		r = read_line(c, line, sizeof line);
		if (r < 0) {
			mpdc_close(c);
			return r;
		}
		if (!strcmp(line, "OK"))
			return 0;
		if (!strncmp(line, "ACK ", 4)) {
			copy(c->last_error, sizeof c->last_error, ack_message(line + 4));
			if (err && errlen)
				copy(err, errlen, ack_message(line + 4));
			return -EIO;
		}
		sep = strstr(line, ": ");
		if (sep && cb) {
			*sep = '\0';
			cb(line, sep + 2, ud);
		}
	}
}

int mpdc_cmd(struct mpdc *c, const char *fmt, ...)
{
	char buf[1024];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	if (n < 0 || (size_t)n >= sizeof buf)
		return -ENAMETOOLONG;
	return mpdc_run(c, buf, NULL, NULL, NULL, 0);
}

int mpdc_quote(char *out, size_t outlen, const char *arg)
{
	size_t o = 0;

	if (outlen < 3)
		return -ENAMETOOLONG;
	out[o++] = '"';
	for (; *arg; arg++) {
		if (*arg == '\n' || *arg == '\r')
			return -EINVAL;
		if (*arg == '"' || *arg == '\\') {
			if (o + 3 >= outlen)
				return -ENAMETOOLONG;
			out[o++] = '\\';
		} else if (o + 2 >= outlen) {
			return -ENAMETOOLONG;
		}
		out[o++] = *arg;
	}
	out[o++] = '"';
	out[o] = '\0';
	return (int)o;
}

/* ------------------------------------------------------------------ typed --- */

static void status_kv(const char *k, const char *v, void *ud)
{
	struct mpdc_status *s = ud;

	if (!strcmp(k, "state"))
		copy(s->state, sizeof s->state, v);
	else if (!strcmp(k, "volume"))
		s->volume = atoi(v);
	else if (!strcmp(k, "song"))
		s->song = atoi(v);
	else if (!strcmp(k, "playlistlength"))
		s->length = atoi(v);
	else if (!strcmp(k, "elapsed"))
		s->elapsed = atof(v);
	else if (!strcmp(k, "duration"))
		s->duration = atof(v);
	else if (!strcmp(k, "audio"))
		copy(s->audio, sizeof s->audio, v);
	else if (!strcmp(k, "bitrate"))
		s->bitrate = atoi(v);
	else if (!strcmp(k, "random"))
		s->random = atoi(v);
	else if (!strcmp(k, "repeat"))
		s->repeat = atoi(v);
	else if (!strcmp(k, "single"))
		s->single = strcmp(v, "0") != 0; /* "oneshot" counts as on */
	else if (!strcmp(k, "consume"))
		s->consume = strcmp(v, "0") != 0;
	else if (!strcmp(k, "updating_db"))
		s->updating = 1;
	else if (!strcmp(k, "error"))
		copy(s->error, sizeof s->error, v);
}

int mpdc_status(struct mpdc *c, struct mpdc_status *st)
{
	memset(st, 0, sizeof *st);
	st->volume = -1;
	st->song = -1;
	copy(st->state, sizeof st->state, "stop");
	return mpdc_run(c, "status", status_kv, st, NULL, 0);
}

static void song_kv(const char *k, const char *v, void *ud)
{
	struct mpdc_song *s = ud;

	/* MPD capitalises tag names; compare case-insensitively to be safe */
	if (!strcasecmp(k, "file"))
		copy(s->file, sizeof s->file, v);
	else if (!strcasecmp(k, "Title"))
		copy(s->title, sizeof s->title, v);
	else if (!strcasecmp(k, "Artist"))
		copy(s->artist, sizeof s->artist, v);
	else if (!strcasecmp(k, "Album"))
		copy(s->album, sizeof s->album, v);
	else if (!strcasecmp(k, "Name"))
		copy(s->name, sizeof s->name, v);
}

int mpdc_currentsong(struct mpdc *c, struct mpdc_song *sg)
{
	memset(sg, 0, sizeof *sg);
	return mpdc_run(c, "currentsong", song_kv, sg, NULL, 0);
}

/* Most rows one directory listing may collect before sorting. 4096 entries is
 * ~1.4 MB transient; a folder bigger than that is truncated before the sort. */
#define LS_HARD_CAP 4096

struct ls_ctx {
	struct mpdc_entry *rows;
	int n, max, truncated;
};

static void ls_kv(const char *k, const char *v, void *ud)
{
	struct ls_ctx *l = ud;
	int is_dir;
	const char *slash;

	if (!strcmp(k, "directory"))
		is_dir = 1;
	else if (!strcmp(k, "file"))
		is_dir = 0;
	else
		return; /* tags, Last-Modified, "playlist" records: not rows */

	if (l->n >= l->max) {
		l->truncated = 1;
		return;
	}
	copy(l->rows[l->n].path, sizeof l->rows[l->n].path, v);
	slash = strrchr(v, '/');
	copy(l->rows[l->n].name, sizeof l->rows[l->n].name, slash ? slash + 1 : v);
	l->rows[l->n].is_dir = is_dir;
	l->n++;
}

/* Natural, case-insensitive order: "track 2" < "track 10", "a" == "A" for
 * ordering purposes. Digit runs compare by value; if two runs have the same
 * value but different zero padding ("2" / "02"), the rest of the string decides
 * first and padding only breaks a tie, fewer zeros first. A final strcmp keeps
 * the order total and repeatable. */
static int natcmp(const char *a, const char *b)
{
	const char *sa = a, *sb = b;
	int tie = 0;

	while (*a && *b) {
		if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
			size_t za = 0, zb = 0, la = 0, lb = 0;

			while (*a == '0') { a++; za++; }
			while (*b == '0') { b++; zb++; }
			while (isdigit((unsigned char)a[la])) la++;
			while (isdigit((unsigned char)b[lb])) lb++;
			if (la != lb)
				return la < lb ? -1 : 1;
			if (la && memcmp(a, b, la))
				return memcmp(a, b, la) < 0 ? -1 : 1;
			a += la;
			b += lb;
			if (za != zb && !tie)
				tie = za < zb ? -1 : 1;
			continue;
		}
		{
			int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);

			if (ca != cb)
				return ca < cb ? -1 : 1;
		}
		a++;
		b++;
	}
	if (*a)
		return 1;
	if (*b)
		return -1;
	if (tie)
		return tie;
	return strcmp(sa, sb);
}

static int entry_cmp(const void *pa, const void *pb)
{
	const struct mpdc_entry *a = pa, *b = pb;

	if (a->is_dir != b->is_dir)
		return a->is_dir ? -1 : 1;
	return natcmp(a->name, b->name);
}

int mpdc_lsinfo(struct mpdc *c, const char *path, struct mpdc_entry **out,
		int max, int *truncated)
{
	struct ls_ctx l;
	struct mpdc_entry *shrunk;
	char q[2 * MPDC_PATH_MAX + 8], cmd[2 * MPDC_PATH_MAX + 24];
	int r;

	if (max < 1)
		return -EINVAL;
	/* Collect up to LS_HARD_CAP rows, sort the lot, THEN cut to `max`: cutting
	 * first would show an arbitrary slice of a big folder, in database order. */
	memset(&l, 0, sizeof l);
	l.max = LS_HARD_CAP;
	l.rows = calloc((size_t)l.max, sizeof *l.rows);
	if (!l.rows)
		return -ENOMEM;
	if ((r = mpdc_quote(q, sizeof q, path)) < 0) {
		free(l.rows);
		return r;
	}
	snprintf(cmd, sizeof cmd, "lsinfo %s", q);
	r = mpdc_run(c, cmd, ls_kv, &l, NULL, 0);
	if (r < 0) {
		free(l.rows);
		return r;
	}
	/* MPD returns database order, which is neither alphabetical nor stable
	 * across rescans; a browser needs a predictable one. */
	qsort(l.rows, (size_t)l.n, sizeof *l.rows, entry_cmp);
	if (l.n > max) {
		l.n = max;
		l.truncated = 1;
	}
	shrunk = realloc(l.rows, (size_t)(l.n ? l.n : 1) * sizeof *l.rows);
	if (truncated)
		*truncated = l.truncated;
	*out = shrunk ? shrunk : l.rows;
	return l.n;
}

const char *mpdc_describe_audio(const char *audio, char *out, size_t outlen)
{
	unsigned rate = 0;
	int ch = 0, dsd = 0;
	char bits[8] = "";

	if (!strncasecmp(audio, "dsd", 3)) {
		const char *colon = strchr(audio, ':');
		dsd = atoi(audio + 3);
		ch = colon ? atoi(colon + 1) : 0;
		/* the XMOS takes native DSD up to DSD128; MPD plays the faster ones as 352.8 kHz PCM */
		snprintf(out, outlen, "DSD%d  %s%s", dsd,
			 ch == 1 ? "mono" : ch == 2 ? "stereo" : "multichannel", dsd > 128 ? "  to PCM" : "");
		return out;
	}
	if (sscanf(audio, "%u:%7[^:]:%d", &rate, bits, &ch) != 3) {
		snprintf(out, outlen, "%s", audio[0] ? audio : "-");
		return out;
	}
	{
		char rate_s[16], bit_s[16];

		if (rate % 1000)
			snprintf(rate_s, sizeof rate_s, "%.1f kHz", rate / 1000.0);
		else
			snprintf(rate_s, sizeof rate_s, "%u kHz", rate / 1000);
		if (!strcmp(bits, "f"))
			snprintf(bit_s, sizeof bit_s, "float");
		else
			snprintf(bit_s, sizeof bit_s, "%s bit", bits);
		snprintf(out, outlen, "%s  %s  %s", rate_s, bit_s,
			 ch == 1 ? "mono" : ch == 2 ? "stereo" : "multichannel");
	}
	return out;
}
