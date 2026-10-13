#define _GNU_SOURCE
/* libsync - tells the music library about what the file manager changed.
 *
 * MPD only knows a file once it has scanned it, so a track that came in through the web page (or was moved, renamed
 * or deleted there) would stay missing from, or linger in, the library lists until somebody pressed "rescan". Each
 * change notes the folder it touched (libsync_note); after the last change of a burst has been quiet for a few
 * seconds one detached worker turns the noted folders into "lib update <folder>" lines for x45ui, which has MPD look
 * at just those folders and then reloads the list on the panel and the web page. An upload of a whole album is
 * thirteen requests; it costs one scan.
 *
 * The folders are real paths on the disks (/mnt/HDD4/My Music/Recordings); MPD names them through the links
 * x45media keeps in the library root (HDD4 -> /mnt/HDD4/My Music), so a folder is mapped through those. A folder
 * that is gone (a delete) is replaced by the nearest one above it that is still there. A folder outside every link
 * (the backup folder on the disk, say) is not music for MPD and is left alone. When a burst touched more folders
 * than a handful, everything is updated instead.
 */
#include "web.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define CMD_FILE   "/tmp/x45ui.cmd"
#define DIRS_FILE  "/tmp/x45libsync.dirs"
#define STAMP_FILE "/tmp/x45libsync.stamp"
#define MEDIA_DIR  "/home/root/x45new/media"
#define SETTLE_S   6     /* quiet time after the last change */
#define MAX_DIRS   8

void libsync_note(const char *dir)
{
	char line[PATH_MAX + 2];
	int f, n;

	n = snprintf(line, sizeof line, "%s\n", dir);
	if (n <= 0 || n >= (int)sizeof line)
		return;
	f = open(DIRS_FILE, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0666);
	if (f < 0)
		return;
	n = (int)write(f, line, (size_t)n);   /* a note that cannot be written only costs one missed update */
	close(f);
}

/* the update line for one noted folder; 0 when it is not in the library */
static int uri_of(const char *dir, char *out, size_t cap)
{
	char path[PATH_MAX], target[PATH_MAX], name[256], *slash;
	DIR *d;
	struct dirent *e;
	size_t bl = 0, l;

	snprintf(path, sizeof path, "%s", dir);
	while (access(path, F_OK) != 0) {   /* deleted: the nearest folder above that is left */
		slash = strrchr(path, '/');
		if (!slash || slash == path)
			return 0;
		*slash = '\0';
	}
	d = opendir(MEDIA_DIR);
	if (!d)
		return 0;
	name[0] = '\0';
	while ((e = readdir(d))) {
		char link[PATH_MAX];

		if (e->d_name[0] == '.')
			continue;
		snprintf(link, sizeof link, "%s/%s", MEDIA_DIR, e->d_name);
		if (!realpath(link, target))
			continue;
		l = strlen(target);
		if (strncmp(path, target, l) || (path[l] != '/' && path[l] != '\0') || l <= bl)
			continue;
		bl = l;
		snprintf(name, sizeof name, "%s", e->d_name);
	}
	closedir(d);
	if (!name[0])
		return 0;
	return snprintf(out, cap, "%s%s", name, path + bl) < (int)cap;
}

static void send_cmd(const char *line)
{
	int f = open(CMD_FILE, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0666);
	ssize_t got;

	if (f < 0)
		return;
	got = write(f, line, strlen(line));
	(void)got;
	close(f);
}

static void flush_dirs(void)
{
	char work[] = DIRS_FILE ".work", buf[8 * PATH_MAX], *line, *save = NULL;
	char seen[MAX_DIRS][PATH_MAX];
	int f, n = 0, i, all = 0;
	ssize_t got;

	if (rename(DIRS_FILE, work) != 0)
		return;
	f = open(work, O_RDONLY | O_CLOEXEC);
	if (f < 0)
		return;
	got = read(f, buf, sizeof buf - 1);
	close(f);
	unlink(work);
	if (got <= 0)
		return;
	buf[got] = '\0';
	for (line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		char uri[PATH_MAX];

		if (!uri_of(line, uri, sizeof uri) || path_has_dotdot(uri))
			continue;
		if (strlen(uri) > 230) {   /* more than the panel's command line takes: update it all */
			all = 1;
			break;
		}
		for (i = 0; i < n; i++)
			if (!strcmp(seen[i], uri))
				break;
		if (i < n)
			continue;
		if (n >= MAX_DIRS) {
			all = 1;
			break;
		}
		snprintf(seen[n++], sizeof seen[0], "%s", uri);
	}
	if (all) {
		send_cmd("lib update\n");
		return;
	}
	for (i = 0; i < n; i++) {
		char cmd[300];

		snprintf(cmd, sizeof cmd, "lib update %.230s\n", seen[i]);
		send_cmd(cmd);
	}
}

void libsync_gone(const char *rel)
{
	char line[700];
	int n = snprintf(line, sizeof line, "lib gone %s\n", rel);

	if (n > 0 && n < (int)sizeof line && strlen(rel) < 230)   /* (the panel refuses a path its command line cut short) */
		send_cmd(line);
}

void prune_empty_dirs(const char *deleted, const char *base)
{
	char dir[PATH_MAX], *slash;
	size_t bl = strlen(base);

	snprintf(dir, sizeof dir, "%s", deleted);
	while ((slash = strrchr(dir, '/')) && slash != dir) {
		*slash = '\0';
		if (strncmp(dir, base, bl) || dir[bl] != '/')   /* the library root itself, or outside it: stop */
			return;
		if (rmdir(dir) != 0)                            /* not empty (or not ours to remove): stop */
			return;
	}
}

void libsync_kick(int conn)
{
	char stamp[64], now[64];
	struct timespec t;
	int f;
	ssize_t got;

	clock_gettime(CLOCK_REALTIME, &t);
	snprintf(stamp, sizeof stamp, "%lld.%09ld %d", (long long)t.tv_sec, t.tv_nsec, (int)getpid());
	f = open(STAMP_FILE, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
	if (f < 0)
		return;
	got = write(f, stamp, strlen(stamp));
	close(f);
	if (got <= 0)
		return;
	if (fork() != 0)
		return;
	/* the worker: the connection it inherited must not stay open on its account */
	if (conn >= 0)
		close(conn);
	setsid();
	prctl(PR_SET_NAME, "x45libsync");   /* `pkill -x x45web` (a restart) must not take the pending update with it */
	sleep(SETTLE_S);
	f = open(STAMP_FILE, O_RDONLY | O_CLOEXEC);
	if (f < 0)
		_exit(0);
	got = read(f, now, sizeof now - 1);
	close(f);
	if (got > 0) {
		now[got] = '\0';
		if (!strcmp(now, stamp))   /* nothing newer came in meanwhile */
			flush_dirs();
	}
	_exit(0);
}
