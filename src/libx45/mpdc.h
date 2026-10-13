/* mpdc  -  a small blocking client for the MPD text protocol.
 *
 * Just what the front-panel UI and the CLI need: connect, run a command and
 * read back its key/value reply, plus typed helpers for status, current song
 * and directory listings. No dependency beyond libc.
 *
 * Protocol facts this relies on (checked against MPD 0.23.15 on the device):
 *   - the server greets with "OK MPD <version>"
 *   - a command is one line; the reply is "key: value" lines ended by "OK",
 *     or a single "ACK [code@index] {command} message" on failure
 *   - arguments containing spaces or quotes must be wrapped in double quotes,
 *     with backslash and double quote escaped (mpdc_quote)
 *
 * Every socket operation has a 2 s timeout, so a wedged server stalls a UI
 * frame, not forever.
 */
#ifndef MPDC_H
#define MPDC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct mpdc {
	int fd;
	char rbuf[4096];
	size_t rlen;   /* bytes currently buffered */
	size_t rpos;   /* start of the unread data */
	/* Message of the most recent "ACK [code@index] {command} message" reply (the
	 * message only), empty after a successful command.
	 * Kept on the handle so the typed helpers (which have no err parameter) do
	 * not swallow the server's explanation. */
	char last_error[160];
	/* Where mpdc_open connected to. MPD drops a client that has been idle for its
	 * connection_timeout (60 s by default); with these the handle opens a fresh
	 * connection by itself instead of failing the first command after a quiet spell. */
	char host[32];
	int  port;
};

#define MPDC_DEFAULT_HOST "127.0.0.1"
#define MPDC_DEFAULT_PORT 6600

/* Password sent right after the greeting by every mpdc_open() (NULL or "" = none).
 * The string is copied. A wrong password makes mpdc_open fail with -EACCES. */
void mpdc_set_password(const char *pw);
/* The password set above ("" = none), for a helper process that has to log in itself. */
const char *mpdc_password(void);

/* 0 on success, -errno on failure (including a bad greeting: -EPROTO). */
int  mpdc_open(struct mpdc *c, const char *host, int port);
void mpdc_close(struct mpdc *c);
static inline int mpdc_is_open(const struct mpdc *c) { return c->fd >= 0; }

typedef void (*mpdc_kv_fn)(const char *key, const char *val, void *ud);

/* Send `cmd` (no trailing newline), call `cb` for each reply line, and return
 *   0       on "OK"
 *   -EIO    on "ACK ..." (text copied into `err` when non-NULL)
 *   -errno  on a socket failure; the connection is closed in that case.
 * A connection the server closed while it sat idle is replaced by a new one first
 * (nothing was sent on the dead one, so the command runs exactly once). */
int mpdc_run(struct mpdc *c, const char *cmd, mpdc_kv_fn cb, void *ud,
	     char *err, size_t errlen);

/* printf-style convenience for commands with no reply body. */
int mpdc_cmd(struct mpdc *c, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

/* Quote one argument for the wire: "..." with \ and " escaped. Returns the
 * length written, or -ENAMETOOLONG if `out` is too small. */
int mpdc_quote(char *out, size_t outlen, const char *arg);

/* ------------------------------------------------------------------ typed --- */

struct mpdc_status {
	char   state[8];       /* "play", "pause" or "stop" */
	int    volume;         /* 0..100, -1 if the server has no mixer */
	int    song;           /* position in the queue, -1 if none */
	int    length;         /* queue length */
	double elapsed;        /* seconds */
	double duration;       /* seconds, 0 if unknown */
	char   audio[32];      /* "96000:24:2", or "dsd64:2" for native DSD */
	int    bitrate;        /* kbit/s, 0 if unknown */
	int    random, repeat, single, consume;
	int    updating;       /* a database update is running (updating_db) */
	char   error[128];
};

struct mpdc_song {
	char file[512];   /* a stream address can be long */
	char title[128];
	char artist[128];
	char album[128];
	char name[128];    /* stream name ("Name:" tag), set by radio streams */
};

int mpdc_status(struct mpdc *c, struct mpdc_status *st);
int mpdc_currentsong(struct mpdc *c, struct mpdc_song *sg);

/* One row of a directory listing. */
#define MPDC_PATH_MAX 512   /* a path or a stream address as a row carries it */
struct mpdc_entry {
	int  is_dir;
	char path[MPDC_PATH_MAX];   /* full path as the server knows it */
	char name[96];    /* last path component, for display */
};

/* List `path` ("" = the music root). Allocates *out (free() it). Entries are
 * sorted here, because MPD returns database order: directories first, then
 * files, each group in case-insensitive natural order ("track 2" before
 * "track 10"). Playlists and other record types are skipped. The whole
 * listing is sorted before it is cut to `max` rows, so what you get is the
 * first `max` in display order; *truncated is set if there were more. Returns
 * the row count or -errno. */
int mpdc_lsinfo(struct mpdc *c, const char *path, struct mpdc_entry **out,
		int max, int *truncated);

/* "96000:24:2" -> "96 kHz  24 bit  stereo"; "dsd64:2" -> "DSD64  stereo".
 * Writes into `out` and returns it. */
const char *mpdc_describe_audio(const char *audio, char *out, size_t outlen);

#ifdef __cplusplus
}
#endif
#endif /* MPDC_H */
