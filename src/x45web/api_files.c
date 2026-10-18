#define _GNU_SOURCE
/* api_files - /api/files: the file manager of files.html. It works on the storage of the unit: the attached disks
 * (USB / SATA, whatever is mounted from /dev/sd*) and the internal music folder, and on nothing else. Every path
 * is resolved (symlinks included) and has to lie inside one of those roots; the roots themselves cannot be
 * renamed, moved or deleted.
 *
 *   GET  /api/files/roots                          the roots: path, name, file system, writable, size, free
 *   GET  /api/files/list?p=DIR                     {"path","root","rw","total","free","items":[{n,t,s,m,l?}]}
 *                                                  t: d folder, f file; l: a link; s bytes; m seconds since 1970
 *   GET  /api/files/get?p=FILE[&dl=1]              the file (Range supported). Shown inline only when a browser can
 *                                                  show it safely (pictures, sound, PDF, text); dl=1 always downloads
 *   PUT  /api/files/put?p=FILE[&over=1]            the request body becomes the file (written beside it, then renamed:
 *                                                  an upload that breaks off leaves the old file alone)
 *   POST /api/files/op?cmd=mkdir&p=PARENT&name=N   a new folder
 *   POST /api/files/op?cmd=rename&p=PATH&name=N    a new name in the same folder
 *   POST /api/files/op?cmd=move&p=A&p=B&to=DIR     (p may repeat) move; across disks it copies, then deletes
 *   POST /api/files/op?cmd=copy&p=A&p=B&to=DIR     copy; into its own folder it makes "name (copy)"
 *   POST /api/files/op?cmd=delete&p=A&p=B          files and folders
 *   GET  /api/files/size?p=A&p=B                   {"files","dirs","bytes","cut"}: what a selection holds
 *   GET  /api/files/find?p=DIR&q=TEXT              names below DIR containing TEXT: {"items":[{path,n,t,s,m}],"cut"}
 *   POST /api/files/zip?p=A&p=B[&name=N.zip]       the selection as one (stored) zip file, written while it is read
 *
 * op and zip also take their parameters as a form in the body (a long selection does not fit into a URL).
 * With a web_token set every request needs it ("Authorization: Bearer", or tok= in the query for links).
 *
 * Paths and names are percent-encoded in the query (encodeURIComponent). The answer to op is
 * {"done":n,"failed":n,"error":"the first reason"}. An attached disk is mounted read-only by the media helper:
 * for a change it is switched to read-write and back (and the helper is told to leave it alone meanwhile), one
 * change at a time - a second one is answered with 503 "busy". An upload that is stopped half way (x45web restarted)
 * removes its temp file; one killed hard leaves it, and the next upload into that folder sweeps it. What a change
 * touched is told to MPD by libsync.c. See docs/X45-FILES.md.
 */
#include "web.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define INTERNAL_MUSIC "/home/root/x45new/music"
#define HOLD_FILE      "/tmp/x45media.hold"
#define LOCK_FILE      "/tmp/x45files.lock"
#define MAX_ROOTS      12
#define MAX_ITEMS      20000
#define IO_BUF         (128 * 1024)
#define MAX_OPS        500            /* paths in one operation */
#define MAX_FORM       (512 * 1024)   /* the parameters of one operation, when they come in the body */

/* ------------------------------------------------------------------ helpers --- */

static void send_all(int fd, const void *buf, size_t n)
{
	const char *p = buf;

	while (n) {
		ssize_t w = write(fd, p, n);

		if (w < 0) {
			if (errno == EINTR)
				continue;
			return;
		}
		p += w;
		n -= (size_t)w;
	}
}

static int hexv(int c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/* %XX decoded in place ('+' stays a plus: encodeURIComponent writes spaces as %20) */
static void url_decode(char *s)
{
	char *o = s;

	while (*s) {
		if (s[0] == '%' && hexv(s[1]) >= 0 && hexv(s[2]) >= 0) {
			*o++ = (char)(hexv(s[1]) * 16 + hexv(s[2]));
			s += 3;
		} else {
			*o++ = *s++;
		}
	}
	*o = '\0';
}

/* the n-th (from 0) value of `key` in the query, decoded; 1 when there is one */
static int qget(const char *q, const char *key, int nth, char *out, size_t cap)
{
	size_t kl = strlen(key);

	while (*q) {
		size_t l = strcspn(q, "&");

		if (l > kl && !strncmp(q, key, kl) && q[kl] == '=' && nth-- == 0) {
			size_t vl = l - kl - 1;

			if (vl >= cap)
				return 0;
			memcpy(out, q + kl + 1, vl);
			out[vl] = '\0';
			url_decode(out);
			return 1;
		}
		q += l;
		if (*q == '&')
			q++;
	}
	return 0;
}

static void json_msg(int fd, int code, const char *err)
{
	char buf[400];
	FILE *f = fmemopen(buf, sizeof buf, "w");

	if (!f) {
		respond_text(fd, code, err);
		return;
	}
	fputs("{\"error\":", f);
	jesc(f, err);
	fputc('}', f);
	fclose(f);
	respond_json(fd, code, buf);
}

/* An answer sent while the sender is still sending (an upload that is refused): the rest is read and thrown away for a
 * moment, or the browser would get a reset instead of the answer. */
static void early_reply(int fd, int code, const char *err)
{
	char junk[4096];
	struct timeval tv = { 1, 0 };
	int n = 0;

	json_msg(fd, code, err);
	shutdown(fd, SHUT_WR);
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	while (n++ < 256 && read(fd, junk, sizeof junk) > 0)
		;
}

static const char *reason(int e)
{
	switch (e) {
	case EEXIST:  return "that name is already there";
	case ENOENT:  return "no such file or folder";
	case EACCES:
	case EPERM:   return "not allowed";
	case EROFS:   return "the disk is read only";
	case ENOSPC:  return "the disk is full";
	case ENOTEMPTY: return "the folder is not empty";
	case ENAMETOOLONG: return "the name is too long";
	case EINVAL:  return "that cannot be done";
	case EBUSY:   return "in use";
	default:      return strerror(e);
	}
}

/* a name that is one path element and safe to create */
static int valid_name(const char *n)
{
	const char *p;

	if (!n[0] || !strcmp(n, ".") || !strcmp(n, "..") || strlen(n) > 255)
		return 0;
	for (p = n; *p; p++)
		if (*p == '/' || (unsigned char)*p < 32)
			return 0;
	return 1;
}

/* ------------------------------------------------------------------- roots --- */

struct root {
	char path[200], name[64], dev[64], fs[24];
	int rw;
};

static int load_roots(struct root *r, int max)
{
	FILE *f = fopen("/proc/mounts", "r");
	char line[512], real[PATH_MAX];
	int n = 0, i;

	if (f) {
		while (n < max && fgets(line, sizeof line, f)) {
			char dev[64], mp[200], fs[24], opt[128], *b;

			if (sscanf(line, "%63s %199s %23s %127s", dev, mp, fs, opt) != 4 || strncmp(dev, "/dev/sd", 7))
				continue;
			for (i = 0; i < n; i++)   /* a disk mounted twice */
				if (!strcmp(r[i].path, mp))
					break;
			if (i < n || !realpath(mp, real) || strlen(real) >= sizeof r[0].path)
				continue;
			snprintf(r[n].path, sizeof r[n].path, "%s", real);
			snprintf(r[n].dev, sizeof r[n].dev, "%s", dev);
			snprintf(r[n].fs, sizeof r[n].fs, "%s", fs);
			b = strrchr(real, '/');
			snprintf(r[n].name, sizeof r[n].name, "%s", b && b[1] ? b + 1 : real);
			r[n].rw = !strncmp(opt, "rw", 2);
			n++;
		}
		fclose(f);
	}
	if (n < max && realpath(INTERNAL_MUSIC, real) && strlen(real) < sizeof r[0].path) {
		snprintf(r[n].path, sizeof r[n].path, "%s", real);
		snprintf(r[n].dev, sizeof r[n].dev, "internal");
		snprintf(r[n].fs, sizeof r[n].fs, "ext4");
		snprintf(r[n].name, sizeof r[n].name, "Internal music");
		r[n].rw = 1;
		n++;
	}
	return n;
}

static const struct root *root_of(const struct root *r, int n, const char *real)
{
	int i;

	for (i = 0; i < n; i++) {
		size_t l = strlen(r[i].path);

		if (!strncmp(real, r[i].path, l) && (real[l] == '\0' || real[l] == '/'))
			return &r[i];
	}
	return NULL;
}

/* A path from the browser to a real path inside a root. With must_exist == 0 the last element may be new.
 * Returns 0, or a negative errno (-EACCES: outside the storage). */
static int resolve(const struct root *roots, int nroots, const char *user, int must_exist, char *out,
		   const struct root **rootp)
{
	const struct root *rt;

	if (user[0] != '/' || strlen(user) >= PATH_MAX - 2)
		return -EINVAL;
	if (!realpath(user, out)) {
		char dir[PATH_MAX], *b, real[PATH_MAX];

		if (must_exist || errno != ENOENT)
			return -errno;
		snprintf(dir, sizeof dir, "%s", user);
		b = strrchr(dir, '/');
		if (!b || !valid_name(b + 1))
			return -EINVAL;
		*b = '\0';
		if (!realpath(dir[0] ? dir : "/", real))
			return -errno;
		if (snprintf(out, PATH_MAX, "%s/%s", strcmp(real, "/") ? real : "", b + 1) >= PATH_MAX)
			return -ENAMETOOLONG;
	}
	rt = root_of(roots, nroots, out);
	if (!rt)
		return -EACCES;
	if (rootp)
		*rootp = rt;
	return 0;
}

/* Like resolve() for something that exists, but a link in the last place stands for itself and not for what it points to:
 * deleting, renaming or moving a link must not touch its target (a link into another disk would delete that disk's folder).
 * A link that points nowhere, or out of the storage, can be removed like any other entry. */
static int resolve_entry(const struct root *roots, int nroots, const char *user, char *out, const struct root **rootp)
{
	char dir[PATH_MAX], real[PATH_MAX], ent[PATH_MAX], *b;
	const struct root *rt;
	struct stat st;

	if (user[0] == '/' && strlen(user) < PATH_MAX - 2) {
		snprintf(dir, sizeof dir, "%s", user);
		b = strrchr(dir, '/');
		if (b && valid_name(b + 1)) {
			*b++ = '\0';
			if (realpath(dir[0] ? dir : "/", real) &&
			    snprintf(ent, sizeof ent, "%s/%s", strcmp(real, "/") ? real : "", b) < (int)sizeof ent &&
			    lstat(ent, &st) == 0 && S_ISLNK(st.st_mode) && (rt = root_of(roots, nroots, ent))) {
				snprintf(out, PATH_MAX, "%s", ent);
				if (rootp)
					*rootp = rt;
				return 0;
			}
		}
	}
	return resolve(roots, nroots, user, 1, out, rootp);
}

static int is_root_path(const struct root *roots, int nroots, const char *real)
{
	int i;

	for (i = 0; i < nroots; i++)
		if (!strcmp(roots[i].path, real))
			return 1;
	return 0;
}

/* ----------------------------------------------------------------- writing --- */

/* One change at a time, with the disks it touches switched to read-write for as long as it takes. */
struct wr {
	int lock, hold, n;
	char mp[3][200];
};

static int wr_begin(struct wr *w)
{
	memset(w, 0, sizeof *w);
	w->hold = -1;
	w->lock = open(LOCK_FILE, O_CREAT | O_RDWR | O_CLOEXEC, 0666);
	if (w->lock < 0)
		return -EIO;   /* without the lock two changes could remount a disk under each other */
	if (flock(w->lock, LOCK_EX | LOCK_NB) != 0) {
		close(w->lock);
		w->lock = -1;
		return -EBUSY;
	}
	return 0;
}

/* The media helper leaves a disk read-write while this marker exists. It holds the pid of the process that made it, so a
 * marker left behind by a killed one (a restart of the web server in the middle of a change) is not believed. */
int hold_open(void)
{
	char b[16];
	int fd = open(HOLD_FILE, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);

	if (fd >= 0) {
		int n = snprintf(b, sizeof b, "%d\n", (int)getpid());

		if (write(fd, b, (size_t)n) < 0) {
			/* an empty marker still holds: the helper only distrusts one that names a dead process */
		}
	}
	return fd;
}

static int wr_add(struct wr *w, const struct root *r)
{
	char cmd[400];
	int i;

	if (r->rw)
		return 0;
	for (i = 0; i < w->n; i++)
		if (!strcmp(w->mp[i], r->path))
			return 0;
	if (w->n >= 3 || strncmp(r->dev, "/dev/sd", 7) || strpbrk(r->path, "'\\"))
		return -EROFS;
	if (w->hold < 0)
		w->hold = hold_open();   /* the media helper leaves it be */
	snprintf(cmd, sizeof cmd, "mount -o remount,rw '%s' >/dev/null 2>&1", r->path);
	if (system(cmd) != 0)
		return -EROFS;
	snprintf(w->mp[w->n++], sizeof w->mp[0], "%s", r->path);
	return 0;
}

static void wr_end(struct wr *w)
{
	char cmd[400];
	int i;

	if (w->n)
		sync();
	for (i = 0; i < w->n; i++) {
		snprintf(cmd, sizeof cmd, "mount -o remount,ro '%s' >/dev/null 2>&1", w->mp[i]);
		(void)system(cmd);
	}
	if (w->hold >= 0) {
		close(w->hold);
		unlink(HOLD_FILE);
	}
	if (w->lock >= 0) {
		flock(w->lock, LOCK_UN);
		close(w->lock);
	}
}

/* ------------------------------------------------------------- tree walking --- */

static int rm_tree(const char *path, int depth)
{
	struct stat st;
	DIR *d;
	struct dirent *e;
	int rc = 0;

	if (lstat(path, &st) != 0)
		return -errno;
	if (!S_ISDIR(st.st_mode))
		return unlink(path) == 0 ? 0 : -errno;
	if (depth > 40)
		return -ELOOP;
	d = opendir(path);
	if (!d)
		return -errno;
	while ((e = readdir(d))) {
		char sub[PATH_MAX];
		int r;

		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		if (snprintf(sub, sizeof sub, "%s/%s", path, e->d_name) >= (int)sizeof sub) {
			rc = -ENAMETOOLONG;
			continue;
		}
		r = rm_tree(sub, depth + 1);
		if (r)
			rc = r;
	}
	closedir(d);
	if (rc)
		return rc;
	return rmdir(path) == 0 ? 0 : -errno;
}

static int copy_file(const char *src, const char *dst, mode_t mode, const struct timespec *mt)
{
	int in = open(src, O_RDONLY | O_CLOEXEC), out, rc = 0;
	char *buf;

	if (in < 0)
		return -errno;
	out = open(dst, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
	if (out < 0) {
		rc = -errno;
		close(in);
		return rc;
	}
	buf = malloc(IO_BUF);
	if (!buf) {
		close(in);
		close(out);
		unlink(dst);
		return -ENOMEM;
	}
	for (;;) {
		ssize_t r = read(in, buf, IO_BUF), off = 0;

		if (r < 0) {
			if (errno == EINTR)
				continue;
			rc = -errno;
			break;
		}
		if (r == 0)
			break;
		while (off < r) {
			ssize_t w = write(out, buf + off, (size_t)(r - off));

			if (w < 0) {
				if (errno == EINTR)
					continue;
				rc = -errno;
				goto done;
			}
			off += w;
		}
	}
done:
	free(buf);
	close(in);
	if (rc == 0 && close(out) != 0)
		rc = -errno;
	else if (rc != 0)
		close(out);
	if (rc) {
		unlink(dst);
		return rc;
	}
	{   /* best effort: the file systems of the disks do not all keep modes and times */
		struct timespec ts[2];

		(void)mode;
		ts[0] = *mt;
		ts[1] = *mt;
		utimensat(AT_FDCWD, dst, ts, 0);
	}
	return 0;
}

static int copy_tree(const char *src, const char *dst, int depth)
{
	struct stat st;
	DIR *d;
	struct dirent *e;
	int rc = 0;

	if (lstat(src, &st) != 0)
		return -errno;
	if (S_ISLNK(st.st_mode)) {   /* a link is copied as a link: following it could walk out of the disk, or loop */
		char target[PATH_MAX];
		ssize_t n = readlink(src, target, sizeof target - 1);

		if (n < 0)
			return -errno;
		target[n] = '\0';
		return symlink(target, dst) == 0 ? 0 : -errno;
	}
	if (!S_ISDIR(st.st_mode)) {
		if (!S_ISREG(st.st_mode))
			return 0;   /* sockets, devices: not copied */
		return copy_file(src, dst, st.st_mode, &st.st_mtim);
	}
	if (depth > 40)
		return -ELOOP;
	if (mkdir(dst, 0777) != 0)
		return -errno;
	d = opendir(src);
	if (!d)
		return -errno;
	while ((e = readdir(d))) {
		char s2[PATH_MAX], d2[PATH_MAX];
		int r;

		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		if (snprintf(s2, sizeof s2, "%s/%s", src, e->d_name) >= (int)sizeof s2 ||
		    snprintf(d2, sizeof d2, "%s/%s", dst, e->d_name) >= (int)sizeof d2) {
			rc = -ENAMETOOLONG;
			continue;
		}
		r = copy_tree(s2, d2, depth + 1);
		if (r)
			rc = r;
	}
	closedir(d);
	return rc;
}

static int exists(const char *p)
{
	struct stat st;

	return lstat(p, &st) == 0;
}

static int same_file(const char *a, const char *b)
{
	struct stat sa, sb;

	return lstat(a, &sa) == 0 && lstat(b, &sb) == 0 && sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}

static int inside(const char *dir, const char *tree)   /* `dir` is `tree` or lies below it */
{
	size_t l = strlen(tree);

	return !strncmp(dir, tree, l) && (dir[l] == '\0' || dir[l] == '/');
}

/* ----------------------------------------------------------------- endpoints --- */

static void ep_roots(int fd, const struct root *r, int n)
{
	char *buf = NULL;
	size_t sz = 0;
	FILE *f = open_memstream(&buf, &sz);
	int i;

	if (!f) {
		json_msg(fd, 503, "out of memory");
		return;
	}
	fputc('[', f);
	for (i = 0; i < n; i++) {
		struct statvfs v;
		unsigned long long total = 0, avail = 0;

		if (statvfs(r[i].path, &v) == 0) {
			total = (unsigned long long)v.f_blocks * v.f_frsize;
			avail = (unsigned long long)v.f_bavail * v.f_frsize;
		}
		fprintf(f, "%s{\"name\":", i ? "," : "");
		jesc(f, r[i].name);
		fputs(",\"path\":", f);
		jesc(f, r[i].path);
		fputs(",\"fs\":", f);
		jesc(f, r[i].fs);
		fprintf(f, ",\"rw\":%d,\"total\":%llu,\"free\":%llu}", r[i].rw, total, avail);
	}
	fputc(']', f);
	fclose(f);
	respond_json(fd, 200, buf);
	free(buf);
}

static void ep_list(int fd, const struct root *roots, int nroots, const char *query)
{
	char p[PATH_MAX], real[PATH_MAX];
	const struct root *rt;
	struct stat st;
	DIR *d;
	struct dirent *e;
	char *buf = NULL;
	size_t sz = 0;
	FILE *f;
	struct statvfs v;
	int rc, n = 0, cut = 0;

	if (!qget(query, "p", 0, p, sizeof p)) {
		json_msg(fd, 400, "no path");
		return;
	}
	rc = resolve(roots, nroots, p, 1, real, &rt);
	if (rc) {
		json_msg(fd, rc == -EACCES ? 403 : rc == -ENOENT ? 404 : 400, rc == -EACCES ? "outside the storage" : reason(-rc));
		return;
	}
	if (stat(real, &st) != 0 || !S_ISDIR(st.st_mode)) {
		json_msg(fd, 400, "not a folder");
		return;
	}
	d = opendir(real);
	if (!d) {
		json_msg(fd, 403, reason(errno));
		return;
	}
	f = open_memstream(&buf, &sz);
	if (!f) {
		closedir(d);
		json_msg(fd, 503, "out of memory");
		return;
	}
	fputs("{\"path\":", f);
	jesc(f, real);
	fputs(",\"root\":", f);
	jesc(f, rt->path);
	fprintf(f, ",\"rw\":%d", rt->rw);
	if (statvfs(real, &v) == 0)
		fprintf(f, ",\"total\":%llu,\"free\":%llu", (unsigned long long)v.f_blocks * v.f_frsize,
			(unsigned long long)v.f_bavail * v.f_frsize);
	fputs(",\"items\":[", f);
	while ((e = readdir(d))) {
		char full[PATH_MAX];
		struct stat ls, ts;
		int link, isdir;

		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		if (n >= MAX_ITEMS) {
			cut = 1;
			break;
		}
		if (snprintf(full, sizeof full, "%s/%s", real, e->d_name) >= (int)sizeof full || lstat(full, &ls) != 0)
			continue;
		link = S_ISLNK(ls.st_mode);
		ts = ls;
		if (link && stat(full, &ts) != 0)
			ts = ls;   /* a broken link: shown as the link itself */
		isdir = S_ISDIR(ts.st_mode);
		fprintf(f, "%s{\"n\":", n ? "," : "");
		jesc(f, e->d_name);
		fprintf(f, ",\"t\":\"%c\",\"s\":%lld,\"m\":%lld%s}", isdir ? 'd' : 'f', isdir ? 0LL : (long long)ts.st_size,
			(long long)ts.st_mtime, link ? ",\"l\":1" : "");
		n++;
	}
	closedir(d);
	fprintf(f, "],\"cut\":%d}", cut);
	fclose(f);
	respond_json(fd, 200, buf);
	free(buf);
}

/* ---- download ---- */

static const char *ext_of(const char *name)
{
	const char *e = strrchr(name, '.');

	return e ? e + 1 : "";
}

/* returns the type for the extension and whether a browser may show it inline without running anything of the file's */
static const char *file_type(const char *name, int *inline_ok)
{
	static const struct { const char *ext, *type; int ok; } t[] = {
		{ "png", "image/png", 1 }, { "jpg", "image/jpeg", 1 }, { "jpeg", "image/jpeg", 1 }, { "gif", "image/gif", 1 },
		{ "webp", "image/webp", 1 }, { "bmp", "image/bmp", 1 }, { "ico", "image/x-icon", 1 },
		{ "mp3", "audio/mpeg", 1 }, { "flac", "audio/flac", 1 }, { "wav", "audio/wav", 1 }, { "ogg", "audio/ogg", 1 },
		{ "oga", "audio/ogg", 1 }, { "opus", "audio/ogg", 1 }, { "m4a", "audio/mp4", 1 }, { "aac", "audio/aac", 1 },
		{ "mp4", "video/mp4", 1 }, { "webm", "video/webm", 1 }, { "m4v", "video/mp4", 1 },
		{ "pdf", "application/pdf", 1 },
		{ "txt", "text/plain; charset=utf-8", 1 }, { "log", "text/plain; charset=utf-8", 1 },
		{ "conf", "text/plain; charset=utf-8", 1 }, { "cfg", "text/plain; charset=utf-8", 1 },
		{ "ini", "text/plain; charset=utf-8", 1 }, { "m3u", "text/plain; charset=utf-8", 1 },
		{ "m3u8", "text/plain; charset=utf-8", 1 }, { "pls", "text/plain; charset=utf-8", 1 },
		{ "cue", "text/plain; charset=utf-8", 1 }, { "md", "text/plain; charset=utf-8", 1 },
		{ "csv", "text/plain; charset=utf-8", 1 }, { "json", "text/plain; charset=utf-8", 1 },
		{ "xml", "text/plain; charset=utf-8", 1 }, { "sh", "text/plain; charset=utf-8", 1 },
		{ "html", "text/plain; charset=utf-8", 1 }, { "htm", "text/plain; charset=utf-8", 1 },
		{ "svg", "text/plain; charset=utf-8", 1 }, { "js", "text/plain; charset=utf-8", 1 },
		{ "css", "text/plain; charset=utf-8", 1 }, { "nfo", "text/plain; charset=utf-8", 1 },
	};
	const char *e = ext_of(name);
	size_t i;

	for (i = 0; i < sizeof t / sizeof *t; i++)
		if (!strcasecmp(e, t[i].ext)) {
			*inline_ok = t[i].ok;
			return t[i].type;
		}
	*inline_ok = 0;
	return "application/octet-stream";
}

static void pct(FILE *f, const char *s)   /* RFC 5987 value */
{
	for (; *s; s++) {
		unsigned char c = (unsigned char)*s;

		if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '-' || c == '.' || c == '_')
			fputc(c, f);
		else
			fprintf(f, "%%%02X", c);
	}
}

static void ep_get(int fd, const char *method, const struct web_req *rq, const struct root *roots, int nroots,
		   const char *query)
{
	char p[PATH_MAX], real[PATH_MAX], dl[8], hdr[2048], *buf;
	struct stat st;
	int rc, in, ok, head = !strcmp(method, "HEAD");
	long long from = 0, to, remaining;
	int code = 200;
	const char *type, *name, *rh, *rv;
	char range_line[96] = "";
	FILE *f;

	if (!qget(query, "p", 0, p, sizeof p)) {
		json_msg(fd, 400, "no path");
		return;
	}
	rc = resolve(roots, nroots, p, 1, real, NULL);
	if (rc) {
		json_msg(fd, rc == -EACCES ? 403 : rc == -ENOENT ? 404 : 400, rc == -EACCES ? "outside the storage" : reason(-rc));
		return;
	}
	if (stat(real, &st) != 0 || !S_ISREG(st.st_mode)) {
		json_msg(fd, 400, "not a file");
		return;
	}
	in = open(real, O_RDONLY | O_CLOEXEC);
	if (in < 0) {
		json_msg(fd, 403, reason(errno));
		return;
	}
	to = (long long)st.st_size - 1;
	/* Range: bytes=a-b | a- | -n */
	rh = rq->hdrs ? strcasestr(rq->hdrs, "\r\nrange:") : NULL;
	rv = rh ? strstr(rh + 2, "bytes=") : NULL;
	if (rv && rv < rh + 2 + strcspn(rh + 2, "\r\n")) {
		long long a = -1, b = -1;
		char *e;

		rv += 6;
		if (*rv == '-') {
			long long n = strtoll(rv + 1, &e, 10);

			a = (long long)st.st_size - n;
			if (a < 0)
				a = 0;
			b = (long long)st.st_size - 1;
		} else {
			a = strtoll(rv, &e, 10);
			b = *e == '-' && e[1] >= '0' && e[1] <= '9' ? strtoll(e + 1, NULL, 10) : (long long)st.st_size - 1;
		}
		if (a < 0 || a >= (long long)st.st_size || b < a) {
			char h[200];
			int n = snprintf(h, sizeof h, "HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */%lld\r\n"
					 "Content-Length: 0\r\nConnection: close\r\n\r\n", (long long)st.st_size);

			send_all(fd, h, (size_t)n);
			close(in);
			return;
		}
		if (b >= (long long)st.st_size)
			b = (long long)st.st_size - 1;
		from = a;
		to = b;
		code = 206;
		snprintf(range_line, sizeof range_line, "Content-Range: bytes %lld-%lld/%lld\r\n", from, to, (long long)st.st_size);
	}
	remaining = to - from + 1;
	if (st.st_size == 0)
		remaining = 0;
	name = strrchr(real, '/') + 1;
	type = file_type(name, &ok);
	if (!ok || (qget(query, "dl", 0, dl, sizeof dl) && dl[0] == '1'))
		ok = 0;
	f = fmemopen(hdr, sizeof hdr, "w");
	if (!f) {
		close(in);
		return;
	}
	/* nothing of a file may run as part of this site; the PDF viewer of a browser is sandboxed already, and needs not be */
	fprintf(f, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %lld\r\nAccept-Ranges: bytes\r\n%s"
		"Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n%s"
		"%sContent-Disposition: %s; filename*=UTF-8''",
		code, code == 206 ? "Partial Content" : "OK", ok ? type : "application/octet-stream", remaining, range_line,
		ok && !strcmp(type, "application/pdf") ? "" : "Content-Security-Policy: sandbox\r\n",
		web_cors ? "Access-Control-Allow-Origin: *\r\n" : "", ok ? "inline" : "attachment");
	pct(f, name);
	fputs("\r\nConnection: close\r\n\r\n", f);
	fclose(f);
	send_all(fd, hdr, strlen(hdr));
	if (!head && remaining > 0 && lseek(in, (off_t)from, SEEK_SET) >= 0 && (buf = malloc(IO_BUF))) {
		while (remaining > 0) {
			ssize_t r = read(in, buf, remaining < IO_BUF ? (size_t)remaining : IO_BUF), off = 0;

			if (r <= 0)
				break;
			while (off < r) {
				ssize_t w = write(fd, buf + off, (size_t)(r - off));

				if (w < 0) {
					if (errno == EINTR)
						continue;
					remaining = 0;   /* gone, or no progress for the 30 s send timeout (EAGAIN) */
					break;
				}
				off += w;
			}
			remaining -= r;
		}
		free(buf);
	}
	close(in);
}

/* ---- upload (and saving an edited file) ---- */

/* The file being written, for the signal handler: when x45web is stopped or restarted in the middle of an upload the
 * half-written file must not be left on the disk (and the helper must be allowed to put the disk back to read-only). */
static char up_tmp[PATH_MAX + 300];

static void up_abort(int sig)
{
	(void)sig;
	if (up_tmp[0])
		unlink(up_tmp);
	unlink(HOLD_FILE);
	_exit(1);
}

static void up_end(void)
{
	up_tmp[0] = '\0';
	signal(SIGTERM, SIG_DFL);
	signal(SIGINT, SIG_DFL);
	signal(SIGHUP, SIG_DFL);
}

/* An upload killed hard (power, SIGKILL) leaves its ".NAME.x45up" behind. While this process holds the write lock no
 * other upload is running, so every such file in the folder is a dead one. */
static void sweep_stale(const char *dir)
{
	DIR *d = opendir(dir);
	struct dirent *e;

	if (!d)
		return;
	while ((e = readdir(d))) {
		size_t l = strlen(e->d_name);
		char p[PATH_MAX];

		if (e->d_name[0] == '.' && l > 7 && !strcmp(e->d_name + l - 6, ".x45up") &&
		    snprintf(p, sizeof p, "%s/%s", dir, e->d_name) < (int)sizeof p)
			unlink(p);
	}
	closedir(d);
}

/* "Expect: 100-continue" (curl sends it for a body over a megabyte): the sender waits for the go-ahead, and without it
 * for a second before it sends anyway */
static int wants_continue(const struct web_req *rq)
{
	const char *h = strcasestr(rq->hdrs, "\r\nexpect:"), *eol;

	if (!h)
		return 0;
	eol = strstr(h + 2, "\r\n");
	h = strcasestr(h, "100-continue");
	return h && (!eol || h < eol);
}

static void ep_put(int fd, const struct web_req *rq, const struct root *roots, int nroots, const char *query)
{
	char p[PATH_MAX], real[PATH_MAX], dir[PATH_MAX], tmp[PATH_MAX + 300], ov[8], *buf, *b;
	const struct root *rt;
	struct statvfs v;
	struct wr w;
	struct stat st;
	long long left;
	int rc, out, over, bad = 0;
	int have_old, gone = 0;

	if (!qget(query, "p", 0, p, sizeof p)) {
		early_reply(fd, 400, "no path");
		return;
	}
	over = qget(query, "over", 0, ov, sizeof ov) && ov[0] == '1';
	if (rq->clen < 0) {
		early_reply(fd, 411, "a length is needed");
		return;
	}
	rc = resolve(roots, nroots, p, 0, real, &rt);
	if (rc) {
		early_reply(fd, rc == -EACCES ? 403 : 400, rc == -EACCES ? "outside the storage" : reason(-rc));
		return;
	}
	if (is_root_path(roots, nroots, real)) {
		early_reply(fd, 403, "that is a whole disk");
		return;
	}
	if (strlen(real) > PATH_MAX - 300) {
		early_reply(fd, 400, "the path is too long");
		return;
	}
	snprintf(dir, sizeof dir, "%s", real);
	b = strrchr(dir, '/');
	*b++ = '\0';
	if (!dir[0])
		snprintf(dir, sizeof dir, "/");
	have_old = lstat(real, &st) == 0;
	if (have_old && (S_ISDIR(st.st_mode) || !over)) {
		early_reply(fd, 409, S_ISDIR(st.st_mode) ? "a folder has that name" : "that name is already there");
		return;
	}
	if (statvfs(dir, &v) == 0 && (unsigned long long)rq->clen > (unsigned long long)v.f_bavail * v.f_frsize) {
		early_reply(fd, 507, "not enough free space");
		return;
	}
	/* the request is acceptable: send the body (the disk is switched to read-write meanwhile, which takes a while) */
	if (wants_continue(rq) && (long long)rq->body_have < rq->clen)
		send_all(fd, "HTTP/1.1 100 Continue\r\n\r\n", 25);
	rc = wr_begin(&w);
	if (rc == 0) {
		/* from here this process owns the write lock and (soon) the helper's hold marker: stopping it must undo both */
		signal(SIGTERM, up_abort);
		signal(SIGINT, up_abort);
		signal(SIGHUP, up_abort);
		rc = wr_add(&w, rt);
	}
	if (rc) {
		if (rc != -EBUSY)
			up_end();
		wr_end(&w);
		early_reply(fd, rc == -EBUSY ? 503 : 403, rc == -EBUSY ? "busy with another change" : reason(-rc));
		return;
	}
	sweep_stale(dir);
	snprintf(tmp, sizeof tmp, "%s/.%.200s.x45up", dir, b);
	out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
	if (out < 0) {
		rc = errno;
		up_end();
		wr_end(&w);
		early_reply(fd, 403, reason(rc));
		return;
	}
	snprintf(up_tmp, sizeof up_tmp, "%s", tmp);
	buf = malloc(IO_BUF);
	left = rq->clen;
	if (!buf) {
		bad = ENOMEM;
	} else {
		size_t have = (long long)rq->body_have < rq->clen ? rq->body_have : (size_t)rq->clen;
		const char *src = rq->body;

		for (;;) {
			size_t off = 0;

			while (off < have) {
				ssize_t wn = write(out, src + off, have - off);

				if (wn < 0) {
					if (errno == EINTR)
						continue;
					bad = errno;
					break;
				}
				off += (size_t)wn;
			}
			left -= (long long)have;
			if (bad || left <= 0)
				break;
			{
				ssize_t r;

				do {   /* not `continue`: that would write the chunk before once more */
					r = read(fd, buf, left < IO_BUF ? (size_t)left : IO_BUF);
				} while (r < 0 && errno == EINTR);
				if (r <= 0) {
					bad = EIO, gone = 1;   /* the sender went away or stalled */
					break;
				}
				have = (size_t)r;
				src = buf;
			}
		}
		free(buf);
	}
	if (!bad && fsync(out) != 0)
		bad = errno;
	if (close(out) != 0 && !bad)
		bad = errno;
	if (!bad && rename(tmp, real) != 0) {
		/* some file systems will not rename over an existing file */
		if (have_old && unlink(real) == 0 && rename(tmp, real) == 0)
			;
		else
			bad = errno ? errno : EIO;
	}
	if (bad)
		unlink(tmp);
	up_tmp[0] = '\0';
	wr_end(&w);
	up_end();
	if (bad) {
		if (gone)
			return;   /* nobody is listening (EIO from a failing disk is still answered) */
		early_reply(fd, bad == ENOSPC ? 507 : 500, reason(bad));
		return;
	}
	respond_json(fd, 200, "{\"ok\":1}");
	libsync_note(dir);
	libsync_kick(fd);
}

/* ---- the operations ---- */

struct opres {
	int done, failed;
	char err[160];
};

static void note(struct opres *o, int rc, const char *what)
{
	if (rc == 0) {
		o->done++;
		return;
	}
	o->failed++;
	if (!o->err[0]) {
		const char *b = strrchr(what, '/');

		snprintf(o->err, sizeof o->err, "%.100s: %s", b ? b + 1 : what, reason(-rc));
	}
}

static const char *base_of(const char *p)
{
	const char *b = strrchr(p, '/');

	return b ? b + 1 : p;
}

static int unique_copy_name(const char *dir, const char *name, char *out, size_t cap)
{
	const char *dot = strrchr(name, '.');
	int n, stem;
	char cand[PATH_MAX];

	if (dot == name)
		dot = NULL;   /* ".bashrc" has no extension */
	stem = dot ? (int)(dot - name) : (int)strlen(name);
	for (n = 1; n < 100; n++) {
		if (n == 1)
			snprintf(cand, sizeof cand, "%s/%.*s (copy)%s", dir, stem, name, dot ? dot : "");
		else
			snprintf(cand, sizeof cand, "%s/%.*s (copy %d)%s", dir, stem, name, n, dot ? dot : "");
		if (!exists(cand)) {
			snprintf(out, cap, "%s", cand);
			return 0;
		}
	}
	return -EEXIST;
}

static void ep_op(int fd, const struct root *roots, int nroots, const char *query)
{
	char cmd[16], to[PATH_MAX], name[300], tor[PATH_MAX] = "";
	const struct root *torx = NULL;
	struct opres res;
	struct wr w;
	int i, rc, np = 0;
	char (*src)[PATH_MAX] = malloc((size_t)MAX_OPS * PATH_MAX);   /* untouched pages cost nothing */
	const struct root **srt = malloc(MAX_OPS * sizeof *srt);
	char *buf = NULL;
	size_t sz = 0;
	FILE *f;

	memset(&res, 0, sizeof res);
	if (!src || !srt) {
		free(src);
		free(srt);
		json_msg(fd, 503, "out of memory");
		return;
	}
	if (!qget(query, "cmd", 0, cmd, sizeof cmd)) {
		json_msg(fd, 400, "no command");
		goto out;
	}
	for (i = 0; i < MAX_OPS; i++) {
		char p[PATH_MAX];

		if (!qget(query, "p", i, p, sizeof p))
			break;
		if (!strcmp(cmd, "mkdir") || !strcmp(cmd, "copy"))
			rc = resolve(roots, nroots, p, strcmp(cmd, "mkdir") != 0, src[np], &srt[np]);
		else
			rc = resolve_entry(roots, nroots, p, src[np], &srt[np]);
		if (rc == -EACCES) {
			json_msg(fd, 403, "outside the storage");
			goto out;
		}
		if (rc) {
			note(&res, rc, p);
			continue;
		}
		np++;
	}
	if (i == MAX_OPS) {   /* more were asked for than one operation takes: say so instead of doing the first part only */
		char more[PATH_MAX];

		if (qget(query, "p", MAX_OPS, more, sizeof more)) {
			json_msg(fd, 413, "too many items selected (500 at most)");
			goto out;
		}
	}
	if (!np && !res.failed) {
		json_msg(fd, 400, "no path");
		goto out;
	}
	to[0] = name[0] = '\0';
	if (qget(query, "to", 0, to, sizeof to)) {
		rc = resolve(roots, nroots, to, 1, tor, &torx);
		if (rc) {
			json_msg(fd, rc == -EACCES ? 403 : 400, rc == -EACCES ? "outside the storage" : reason(-rc));
			goto out;
		}
	}
	qget(query, "name", 0, name, sizeof name);

	if (strcmp(cmd, "mkdir") && strcmp(cmd, "rename") && strcmp(cmd, "move") && strcmp(cmd, "copy") && strcmp(cmd, "delete")) {
		json_msg(fd, 400, "unknown command");
		goto out;
	}
	if ((!strcmp(cmd, "move") || !strcmp(cmd, "copy")) && !to[0]) {
		json_msg(fd, 400, "no destination");
		goto out;
	}
	if ((!strcmp(cmd, "mkdir") || !strcmp(cmd, "rename")) && !valid_name(name)) {
		json_msg(fd, 400, "that name will not do");
		goto out;
	}
	rc = wr_begin(&w);
	if (rc == 0) {
		signal(SIGTERM, up_abort);   /* stopped half way: let go of the hold marker (see ep_put) */
		signal(SIGINT, up_abort);
		signal(SIGHUP, up_abort);
	}
	for (i = 0; i < np && rc == 0; i++)
		rc = wr_add(&w, srt[i]);
	if (rc == 0 && torx)
		rc = wr_add(&w, torx);
	if (rc) {
		if (rc != -EBUSY)
			up_end();
		wr_end(&w);
		json_msg(fd, rc == -EBUSY ? 503 : 403, rc == -EBUSY ? "busy with another change" : reason(-rc));
		goto out;
	}

	for (i = 0; i < np; i++) {
		const char *s = src[i];
		char dest[PATH_MAX];
		struct stat st;

		if (!strcmp(cmd, "mkdir")) {
			if (snprintf(dest, sizeof dest, "%s/%s", s, name) >= (int)sizeof dest)
				note(&res, -ENAMETOOLONG, name);
			else
				note(&res, mkdir(dest, 0777) == 0 ? 0 : -errno, name);
			continue;
		}
		if (is_root_path(roots, nroots, s)) {
			note(&res, -EPERM, s);
			continue;
		}
		if (!strcmp(cmd, "delete")) {
			note(&res, rm_tree(s, 0), s);
			continue;
		}
		if (!strcmp(cmd, "rename")) {
			char dir[PATH_MAX], *b;

			snprintf(dir, sizeof dir, "%s", s);
			b = strrchr(dir, '/');
			*b = '\0';
			if (snprintf(dest, sizeof dest, "%s/%s", dir, name) >= (int)sizeof dest)
				note(&res, -ENAMETOOLONG, name);
			else if (!strcmp(dest, s))
				note(&res, 0, s);
			else if (exists(dest) && !same_file(dest, s))   /* "a" -> "A" on a disk that ignores case */
				note(&res, -EEXIST, name);
			else
				note(&res, rename(s, dest) == 0 ? 0 : -errno, s);
			continue;
		}
		/* move / copy into `to` */
		if (lstat(s, &st) != 0) {
			note(&res, -errno, s);
			continue;
		}
		if (S_ISDIR(st.st_mode) && inside(tor, s)) {
			note(&res, -EINVAL, s);   /* a folder into itself */
			continue;
		}
		{
			char dir[PATH_MAX], *b;
			int same;

			snprintf(dir, sizeof dir, "%s", s);
			b = strrchr(dir, '/');
			*b = '\0';
			same = !strcmp(dir[0] ? dir : "/", tor);
			if (same && !strcmp(cmd, "move")) {
				note(&res, 0, s);   /* already there */
				continue;
			}
			if (same) {
				rc = unique_copy_name(tor, base_of(s), dest, sizeof dest);
				if (rc) {
					note(&res, rc, s);
					continue;
				}
			} else if (snprintf(dest, sizeof dest, "%s/%s", strcmp(tor, "/") ? tor : "", base_of(s)) >= (int)sizeof dest) {
				note(&res, -ENAMETOOLONG, s);
				continue;
			} else if (exists(dest)) {
				note(&res, -EEXIST, base_of(s));
				continue;
			}
		}
		if (!strcmp(cmd, "copy")) {
			rc = copy_tree(s, dest, 0);
			if (rc)
				rm_tree(dest, 0);   /* no half copies */
			note(&res, rc, s);
		} else if (rename(s, dest) == 0) {
			note(&res, 0, s);
		} else if (errno == EXDEV) {
			rc = copy_tree(s, dest, 0);
			if (rc) {
				rm_tree(dest, 0);
				note(&res, rc, s);
			} else {
				note(&res, rm_tree(s, 0), s);
			}
		} else {
			note(&res, -errno, s);
		}
	}
	wr_end(&w);
	up_end();

	if (res.done) {   /* the library lists must follow: MPD looks at the folders that changed */
		for (i = 0; i < np; i++) {
			char dir[PATH_MAX], *sl;

			snprintf(dir, sizeof dir, "%s", src[i]);
			if (strcmp(cmd, "mkdir") && (sl = strrchr(dir, '/')))
				*sl = '\0';   /* the folder above what was changed (a new folder is made in the one given) */
			libsync_note(dir[0] ? dir : "/");
		}
		if (tor[0])
			libsync_note(tor);
	}
	f = open_memstream(&buf, &sz);
	if (!f) {
		respond_json(fd, 200, "{\"done\":0,\"failed\":1,\"error\":\"out of memory\"}");
		goto out;
	}
	fprintf(f, "{\"done\":%d,\"failed\":%d,\"error\":", res.done, res.failed);
	jesc(f, res.err);
	fputc('}', f);
	fclose(f);
	respond_json(fd, 200, buf);
	free(buf);
	if (res.done)
		libsync_kick(fd);
out:
	free(src);
	free(srt);
}

/* ---- size of a folder, search ---- */

struct tally {
	unsigned long long files, dirs, bytes;
	long left;
	int cut;
	time_t stop;   /* a slow disk must not hold a web process for minutes */
};

static void tally_walk(const char *path, struct tally *t, int depth)
{
	struct stat st;
	DIR *d;
	struct dirent *e;

	if (lstat(path, &st) != 0)
		return;
	if (!S_ISDIR(st.st_mode)) {
		t->files++;
		t->bytes += S_ISREG(st.st_mode) ? (unsigned long long)st.st_size : 0;
		return;
	}
	t->dirs++;
	if (depth > 40 || !(d = opendir(path)))
		return;
	while ((e = readdir(d))) {
		char sub[PATH_MAX];

		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		if (--t->left < 0 || ((t->left & 255) == 0 && time(NULL) > t->stop)) {
			t->left = -1;
			t->cut = 1;
			break;
		}
		if (snprintf(sub, sizeof sub, "%s/%s", path, e->d_name) < (int)sizeof sub)
			tally_walk(sub, t, depth + 1);
	}
	closedir(d);
}

static void ep_size(int fd, const struct root *roots, int nroots, const char *query)
{
	struct tally t = { 0, 0, 0, 300000, 0, 0 };
	char p[PATH_MAX], real[PATH_MAX], out[200];
	int i, rc;

	t.stop = time(NULL) + 20;
	for (i = 0; qget(query, "p", i, p, sizeof p); i++) {
		rc = resolve(roots, nroots, p, 1, real, NULL);
		if (rc) {
			json_msg(fd, rc == -EACCES ? 403 : rc == -ENOENT ? 404 : 400, rc == -EACCES ? "outside the storage" : reason(-rc));
			return;
		}
		tally_walk(real, &t, 0);
	}
	snprintf(out, sizeof out, "{\"files\":%llu,\"dirs\":%llu,\"bytes\":%llu,\"cut\":%d}", t.files, t.dirs, t.bytes, t.cut);
	respond_json(fd, 200, out);
}

struct found {
	FILE *f;
	const char *needle;
	int n, cut;
	long visited;
	time_t stop;
};

static void find_walk(const char *path, struct found *r, int depth)
{
	DIR *d;
	struct dirent *e;

	if (depth > 40 || !(d = opendir(path)))
		return;
	while ((e = readdir(d)) && !r->cut) {
		char sub[PATH_MAX];
		struct stat st;

		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		if (++r->visited > 300000 || ((r->visited & 255) == 0 && time(NULL) > r->stop)) {
			r->cut = 1;
			break;
		}
		if (snprintf(sub, sizeof sub, "%s/%s", path, e->d_name) >= (int)sizeof sub || lstat(sub, &st) != 0)
			continue;
		if (strcasestr(e->d_name, r->needle)) {
			if (r->n >= 1000) {
				r->cut = 1;
				break;
			}
			fprintf(r->f, "%s{\"path\":", r->n ? "," : "");
			jesc(r->f, sub);
			fputs(",\"n\":", r->f);
			jesc(r->f, e->d_name);
			fprintf(r->f, ",\"t\":\"%c\",\"s\":%lld,\"m\":%lld}", S_ISDIR(st.st_mode) ? 'd' : 'f',
				S_ISDIR(st.st_mode) ? 0LL : (long long)st.st_size, (long long)st.st_mtime);
			r->n++;
		}
		if (S_ISDIR(st.st_mode))
			find_walk(sub, r, depth + 1);
	}
	closedir(d);
}

static void ep_find(int fd, const struct root *roots, int nroots, const char *query)
{
	char p[PATH_MAX], real[PATH_MAX], q[256], *buf = NULL;
	size_t sz = 0;
	struct found r;
	int rc;

	if (!qget(query, "p", 0, p, sizeof p) || !qget(query, "q", 0, q, sizeof q) || !q[0]) {
		json_msg(fd, 400, "nothing to look for");
		return;
	}
	rc = resolve(roots, nroots, p, 1, real, NULL);
	if (rc) {
		json_msg(fd, rc == -EACCES ? 403 : rc == -ENOENT ? 404 : 400, rc == -EACCES ? "outside the storage" : reason(-rc));
		return;
	}
	memset(&r, 0, sizeof r);
	r.f = open_memstream(&buf, &sz);
	if (!r.f) {
		json_msg(fd, 503, "out of memory");
		return;
	}
	r.needle = q;
	r.stop = time(NULL) + 20;
	fputs("{\"items\":[", r.f);
	find_walk(real, &r, 0);
	fprintf(r.f, "],\"cut\":%d}", r.cut);
	fclose(r.f);
	respond_json(fd, 200, buf);
	free(buf);
}

/* ---- a folder (or a selection) as one zip file ----
 * Stored, not compressed (music does not shrink), written as it is read: sizes and checksums follow each file in a
 * data descriptor, so nothing is buffered and nothing has to be read twice. 64-bit sizes only when the total needs them. */

struct zent {
	char *name;
	uint32_t crc, dos_time, dos_date, mode;
	uint64_t size, off;
	int dir;
};

struct zip {
	int fd, z64, err;
	uint64_t off;
	struct zent *e;
	size_t n, cap;
	uint32_t crc_tab[256];
};

static void zw(struct zip *z, const void *buf, size_t n)
{
	const char *p = buf;

	if (z->err)
		return;
	z->off += n;
	while (n) {
		ssize_t w = write(z->fd, p, n);

		if (w < 0) {
			if (errno == EINTR)
				continue;
			z->err = 1;
			return;
		}
		p += w;
		n -= (size_t)w;
	}
}

static void put16(unsigned char *b, unsigned v)
{
	b[0] = (unsigned char)v;
	b[1] = (unsigned char)(v >> 8);
}

static void put32(unsigned char *b, uint32_t v)
{
	put16(b, v & 0xffff);
	put16(b + 2, v >> 16);
}

static void put64(unsigned char *b, uint64_t v)
{
	put32(b, (uint32_t)v);
	put32(b + 4, (uint32_t)(v >> 32));
}

static void crc_init(struct zip *z)
{
	uint32_t i, k, c;

	for (i = 0; i < 256; i++) {
		c = i;
		for (k = 0; k < 8; k++)
			c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
		z->crc_tab[i] = c;
	}
}

static uint32_t crc_upd(const struct zip *z, uint32_t crc, const unsigned char *b, size_t n)
{
	crc = ~crc;
	while (n--)
		crc = z->crc_tab[(crc ^ *b++) & 0xff] ^ (crc >> 8);
	return ~crc;
}

static void dos_stamp(time_t t, uint32_t *dt, uint32_t *dd)
{
	struct tm tm;

	gmtime_r(&t, &tm);
	if (tm.tm_year < 80) {
		*dt = 0;
		*dd = (1 << 5) | 1;
		return;
	}
	*dt = (uint32_t)(tm.tm_hour << 11 | tm.tm_min << 5 | tm.tm_sec / 2);
	*dd = (uint32_t)((tm.tm_year - 80) << 9 | (tm.tm_mon + 1) << 5 | tm.tm_mday);
}

/* how much is there: entries and bytes (the choice of 32 or 64 bit sizes) */
static void zip_measure(const char *path, unsigned long long *bytes, unsigned long *count, int depth)
{
	struct stat st;
	DIR *d;
	struct dirent *e;

	if (lstat(path, &st) != 0 || (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode)))
		return;
	(*count)++;
	if (S_ISREG(st.st_mode)) {
		*bytes += (unsigned long long)st.st_size;
		if ((unsigned long long)st.st_size >= 0xf0000000ull)
			*bytes += 0xf0000000ull;
		return;
	}
	if (depth > 40 || !(d = opendir(path)))
		return;
	while ((e = readdir(d)) && *count < 100000) {
		char sub[PATH_MAX];

		if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..") &&
		    snprintf(sub, sizeof sub, "%s/%s", path, e->d_name) < (int)sizeof sub)
			zip_measure(sub, bytes, count, depth + 1);
	}
	closedir(d);
}

static void zip_add(struct zip *z, const char *path, const char *arc, int depth)
{
	struct stat st;
	struct zent *en;
	unsigned char h[30 + 20];
	size_t nl;
	int in = -1;

	if (z->err || z->n >= 100000 || lstat(path, &st) != 0 || (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode)))
		return;
	nl = strlen(arc);
	if (nl > 1000)
		return;
	if (z->n == z->cap) {
		size_t nc = z->cap ? z->cap * 2 : 256;
		struct zent *ne = realloc(z->e, nc * sizeof *ne);

		if (!ne) {
			z->err = 1;
			return;
		}
		z->e = ne;
		z->cap = nc;
	}
	if (S_ISREG(st.st_mode) && (in = open(path, O_RDONLY | O_CLOEXEC)) < 0)
		return;   /* unreadable: left out */
	en = &z->e[z->n++];
	memset(en, 0, sizeof *en);
	en->name = malloc(nl + 2);
	if (!en->name) {
		z->err = 1;
		if (in >= 0)
			close(in);
		return;
	}
	en->dir = S_ISDIR(st.st_mode);
	snprintf(en->name, nl + 2, "%s%s", arc, en->dir ? "/" : "");
	nl = strlen(en->name);
	en->mode = (uint32_t)st.st_mode & 07777;
	en->mode |= en->dir ? 0040000 : 0100000;
	dos_stamp(st.st_mtime, &en->dos_time, &en->dos_date);
	en->off = z->off;

	memset(h, 0, sizeof h);
	put32(h, 0x04034b50);
	put16(h + 4, z->z64 ? 45 : 20);
	put16(h + 6, en->dir ? 0x0800 : 0x0808);
	put16(h + 10, en->dos_time);
	put16(h + 12, en->dos_date);
	put16(h + 26, (unsigned)nl);
	if (z->z64 && !en->dir) {
		put32(h + 18, 0xffffffffu);
		put32(h + 22, 0xffffffffu);
		put16(h + 28, 20);
		put16(h + 30 + 0, 0x0001);   /* the zip64 field: sizes still unknown, zero */
		put16(h + 30 + 2, 16);
	}
	zw(z, h, 30);
	zw(z, en->name, nl);
	if (z->z64 && !en->dir)
		zw(z, h + 30, 20);   /* the extra field follows the name */

	if (en->dir) {
		DIR *d = opendir(path);
		struct dirent *e;

		if (d && depth <= 40) {
			while ((e = readdir(d)) && !z->err) {
				char sub[PATH_MAX], a2[1100];

				if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
					continue;
				if (snprintf(sub, sizeof sub, "%s/%s", path, e->d_name) < (int)sizeof sub &&
				    snprintf(a2, sizeof a2, "%s/%s", arc, e->d_name) < (int)sizeof a2)
					zip_add(z, sub, a2, depth + 1);
			}
		}
		if (d)
			closedir(d);
		return;
	}
	{
		unsigned char *buf = malloc(IO_BUF), dd[24];
		uint32_t crc = 0;
		uint64_t total = 0;

		if (!buf) {
			z->err = 1;
			close(in);
			return;
		}
		for (;;) {
			ssize_t r = read(in, buf, IO_BUF);

			if (r < 0 && errno == EINTR)
				continue;
			if (r <= 0)
				break;
			crc = crc_upd(z, crc, buf, (size_t)r);
			total += (uint64_t)r;
			zw(z, buf, (size_t)r);
			if (z->err)
				break;
		}
		free(buf);
		close(in);
		en->crc = crc;
		en->size = total;
		put32(dd, 0x08074b50);
		put32(dd + 4, crc);
		if (z->z64) {
			put64(dd + 8, total);
			put64(dd + 16, total);
			zw(z, dd, 24);
		} else {
			put32(dd + 8, (uint32_t)total);
			put32(dd + 12, (uint32_t)total);
			zw(z, dd, 16);
		}
	}
}

static void zip_finish(struct zip *z)
{
	uint64_t cd_off = z->off, cd_size;
	size_t i;
	unsigned char h[64];

	for (i = 0; i < z->n && !z->err; i++) {
		struct zent *en = &z->e[i];
		size_t nl = strlen(en->name);

		memset(h, 0, sizeof h);
		put32(h, 0x02014b50);
		put16(h + 4, (3 << 8) | (z->z64 ? 45 : 20));   /* made by: unix */
		put16(h + 6, z->z64 ? 45 : 20);
		put16(h + 8, en->dir ? 0x0800 : 0x0808);
		put16(h + 12, en->dos_time);
		put16(h + 14, en->dos_date);
		put32(h + 16, en->crc);
		put32(h + 20, z->z64 ? 0xffffffffu : (uint32_t)en->size);
		put32(h + 24, z->z64 ? 0xffffffffu : (uint32_t)en->size);
		put16(h + 28, (unsigned)nl);
		put16(h + 30, z->z64 ? 28 : 0);
		put32(h + 38, en->mode << 16);
		put32(h + 42, z->z64 ? 0xffffffffu : (uint32_t)en->off);
		zw(z, h, 46);
		zw(z, en->name, nl);
		if (z->z64) {
			put16(h, 0x0001);
			put16(h + 2, 24);
			put64(h + 4, en->size);
			put64(h + 12, en->size);
			put64(h + 20, en->off);
			zw(z, h, 28);
		}
	}
	cd_size = z->off - cd_off;
	if (z->z64) {
		uint64_t eocd64 = z->off;

		memset(h, 0, sizeof h);
		put32(h, 0x06064b50);
		put64(h + 4, 44);
		put16(h + 12, 45);
		put16(h + 14, 45);
		put64(h + 24, z->n);
		put64(h + 32, z->n);
		put64(h + 40, cd_size);
		put64(h + 48, cd_off);
		zw(z, h, 56);
		memset(h, 0, sizeof h);
		put32(h, 0x07064b50);
		put64(h + 8, eocd64);
		put32(h + 16, 1);
		zw(z, h, 20);
	}
	memset(h, 0, sizeof h);
	put32(h, 0x06054b50);
	put16(h + 8, z->z64 ? 0xffff : (unsigned)z->n);
	put16(h + 10, z->z64 ? 0xffff : (unsigned)z->n);
	put32(h + 12, z->z64 ? 0xffffffffu : (uint32_t)cd_size);
	put32(h + 16, z->z64 ? 0xffffffffu : (uint32_t)cd_off);
	zw(z, h, 22);
}

static void ep_zip(int fd, const struct root *roots, int nroots, const char *query)
{
	struct zip *z = calloc(1, sizeof *z);
	char p[PATH_MAX], real[PATH_MAX], name[300], hdr[2048], fname[300];
	char (*src)[PATH_MAX] = malloc((size_t)MAX_OPS * PATH_MAX);
	unsigned long long bytes = 0;
	unsigned long count = 0;
	int i, np = 0, rc;
	size_t k;

	if (!z || !src) {
		json_msg(fd, 503, "out of memory");
		goto out;
	}
	for (i = 0; i < MAX_OPS && qget(query, "p", i, p, sizeof p); i++) {
		rc = resolve(roots, nroots, p, 1, real, NULL);
		if (rc) {
			json_msg(fd, rc == -EACCES ? 403 : rc == -ENOENT ? 404 : 400, rc == -EACCES ? "outside the storage" : reason(-rc));
			goto out;
		}
		snprintf(src[np++], PATH_MAX, "%s", real);
	}
	if (i == MAX_OPS) {
		if (qget(query, "p", MAX_OPS, p, sizeof p)) {
			json_msg(fd, 413, "too many items selected (500 at most)");
			goto out;
		}
	}
	if (!np) {
		json_msg(fd, 400, "no path");
		goto out;
	}
	for (i = 0; i < np; i++)
		zip_measure(src[i], &bytes, &count, 0);
	if (count >= 100000) {
		json_msg(fd, 413, "too many files for one zip");
		goto out;
	}
	z->fd = fd;
	z->z64 = bytes >= 0xf0000000ull || count >= 60000;
	crc_init(z);
	if (!qget(query, "name", 0, name, sizeof name) || !valid_name(name))
		snprintf(name, sizeof name, "%s.zip", np == 1 && !is_root_path(roots, nroots, src[0]) ? base_of(src[0]) : "x45-files");
	/* a header-safe copy of the name, the full one in the UTF-8 field */
	for (k = 0; name[k] && k < sizeof fname - 1; k++)
		fname[k] = (unsigned char)name[k] < 32 || name[k] == '"' || name[k] == '\\' || (unsigned char)name[k] > 126 ? '_' : name[k];
	fname[k] = '\0';
	{
		FILE *f = fmemopen(hdr, sizeof hdr, "w");

		if (!f)
			goto out;
		fprintf(f, "HTTP/1.1 200 OK\r\nContent-Type: application/zip\r\nCache-Control: no-store\r\n"
			"X-Content-Type-Options: nosniff\r\n%s"
			"Content-Disposition: attachment; filename=\"%s\"; filename*=UTF-8''",
			web_cors ? "Access-Control-Allow-Origin: *\r\n" : "", fname);
		pct(f, name);
		fputs("\r\nConnection: close\r\n\r\n", f);
		fclose(f);
	}
	send_all(fd, hdr, strlen(hdr));
	for (i = 0; i < np && !z->err; i++) {
		const char *b = is_root_path(roots, nroots, src[i]) ? NULL : base_of(src[i]);

		if (b) {
			zip_add(z, src[i], b, 0);
		} else {   /* a whole disk: its content, under the disk's name */
			DIR *d = opendir(src[i]);
			struct dirent *e;

			while (d && (e = readdir(d)) && !z->err) {
				char sub[PATH_MAX], a2[400];

				if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..") &&
				    snprintf(sub, sizeof sub, "%s/%s", src[i], e->d_name) < (int)sizeof sub &&
				    snprintf(a2, sizeof a2, "%s/%s", base_of(src[i]), e->d_name) < (int)sizeof a2)
					zip_add(z, sub, a2, 1);
			}
			if (d)
				closedir(d);
		}
	}
	if (!z->err)
		zip_finish(z);
out:
	if (z) {
		for (k = 0; k < z->n; k++)
			free(z->e[k].name);
		free(z->e);
	}
	free(z);
	free(src);
}

/* The parameters of an operation: the query, and the body when the browser sends a form (a long selection does not
 * fit a URL). Returns a string to free, or NULL after answering. */
static char *form_params(int fd, const struct web_req *rq, const char *query)
{
	size_t ql = strlen(query), bl = rq->clen > 0 ? (size_t)rq->clen : 0, got;
	char *all;

	if (bl > MAX_FORM) {
		json_msg(fd, 413, "too many items");
		return NULL;
	}
	all = malloc(ql + bl + 2);
	if (!all) {
		json_msg(fd, 503, "out of memory");
		return NULL;
	}
	memcpy(all, query, ql);
	all[ql++] = '&';
	got = rq->body_have < bl ? rq->body_have : bl;
	memcpy(all + ql, rq->body, got);
	while (got < bl) {
		ssize_t r = read(fd, all + ql + got, bl - got);

		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			break;
		got += (size_t)r;
	}
	all[ql + got] = '\0';
	if (got < bl) {
		json_msg(fd, 400, "the request broke off");
		free(all);
		return NULL;
	}
	/* a form posted by the browser (the zip download) writes a space as '+'; a real plus arrives as %2B. The query
	 * part before the body keeps a '+' as it is. */
	{
		size_t k;

		for (k = ql; k < ql + got; k++)
			if (all[k] == '+')
				all[k] = ' ';
	}
	return all;
}

int api_files(int fd, const char *method, const char *path, const char *query, const struct web_req *rq)
{
	struct root roots[MAX_ROOTS];
	int n;

	if (strncmp(path, "/api/files/", 11))
		return 0;
	n = load_roots(roots, MAX_ROOTS);
	path += 11;
	if (!strcmp(path, "roots") && !strcmp(method, "GET"))
		ep_roots(fd, roots, n);
	else if (!strcmp(path, "list") && !strcmp(method, "GET"))
		ep_list(fd, roots, n, query);
	else if (!strcmp(path, "get") && (!strcmp(method, "GET") || !strcmp(method, "HEAD")))
		ep_get(fd, method, rq, roots, n, query);
	else if (!strcmp(path, "put") && (!strcmp(method, "PUT") || !strcmp(method, "POST")))
		ep_put(fd, rq, roots, n, query);
	else if (!strcmp(path, "size") && !strcmp(method, "GET"))
		ep_size(fd, roots, n, query);
	else if (!strcmp(path, "find") && !strcmp(method, "GET"))
		ep_find(fd, roots, n, query);
	else if ((!strcmp(path, "op") && !strcmp(method, "POST")) || (!strcmp(path, "zip") && !strcmp(method, "POST")) ||
		 (!strcmp(path, "zip") && !strcmp(method, "GET"))) {
		/* the parameters come in the query, in the body (as a form: a long selection does not fit a URL), or both */
		char *all = form_params(fd, rq, query);

		if (all) {
			if (path[0] == 'z')
				ep_zip(fd, roots, n, all);
			else
				ep_op(fd, roots, n, all);
			free(all);
		}
	} else
		json_msg(fd, 404, "unknown file manager request");
	return 1;
}
