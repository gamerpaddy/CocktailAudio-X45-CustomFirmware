/* web - what the endpoint modules (api_*.c) take from the server in main.c. */
#ifndef X45WEB_H
#define X45WEB_H

#include <stddef.h>
#include <stdio.h>

extern int web_cors;   /* the answers carry the open CORS headers (see main.c); the modules that write their own headers test it */
void respond(int fd, int code, const char *type, const void *body, size_t len);
void respond_text(int fd, int code, const char *text);
void respond_json(int fd, int code, const char *json);
void jesc(FILE *f, const char *text);   /* a JSON string, quotes included */

/* An endpoint module answers the requests under its own prefix: returns 1 when it handled `path`
 * (and has written the response), 0 when the path is not one of its own. `body` is the request body.
 * To add a module: write api_NAME.c, declare it here, add the call to the table in main.c and the
 * file to the x45web rule in the Makefile. */
int api_wifi(int fd, const char *method, const char *path, const char *body, size_t len);
int api_songlog(int fd, const char *method, const char *path, const char *body, size_t len);
int api_cd(int fd, const char *method, const char *path, const char *body, size_t len);
int api_health(int fd, const char *method, const char *path, const char *body, size_t len);
int api_access(int fd, const char *method, const char *path, const char *body, size_t len);

/* A request whose body is not buffered whole (uploads): the headers, and what of the body came with them.
 * The module reads the rest from `fd` itself. */
struct web_req {
	const char *hdrs;      /* request line and headers, each line ended by CR LF; NUL-terminated */
	const char *body;      /* the part of the body that has been read already */
	size_t body_have;
	long long clen;        /* Content-Length, -1 when there is none */
};
int api_files(int fd, const char *method, const char *path, const char *query, const struct web_req *rq);

/* 1 when one element of the path is exactly "..". A name that merely contains two dots ("Mr..Brightside.mp3") is fine. */
static inline int path_has_dotdot(const char *p)
{
	while (*p) {
		const char *e = p;

		while (*e && *e != '/')
			e++;
		if (e - p == 2 && p[0] == '.' && p[1] == '.')
			return 1;
		p = *e ? e + 1 : e;
	}
	return 0;
}

/* libsync.c: what the file manager changed is made known to MPD a few seconds after the last change.
 * libsync_note: a folder was touched (a real path on a disk); libsync_kick: the changes of this request are done.
 * `conn` is the client's socket, closed in the background worker that is started. */
int hold_open(void);   /* api_files.c: makes /tmp/x45media.hold (with our pid in it); close the fd and unlink the file when done */
void libsync_note(const char *dir);
void libsync_kick(int conn);
/* libsync_gone: the library path (HDD4/Album/01.flac) was deleted; the panel drops it from its list at once and stops
 * what plays from it, without waiting for MPD's update. */
void libsync_gone(const char *rel);

/* After a deletion: remove the folders it left empty, from the one that held the item up to (not including) `base`, the
 * library root of that disk. MPD lists no folder without music, so such a folder could not be seen, let alone deleted,
 * from the library. The disk must still be writable. */
void prune_empty_dirs(const char *deleted, const char *base);

#endif
