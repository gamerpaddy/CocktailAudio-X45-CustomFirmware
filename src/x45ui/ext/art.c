#define _GNU_SOURCE
/* ext/art - the picture of what is playing: the cover of a track, the logo of a radio station, the cover of the song
 * a station announces.
 *
 * It has no screen. poll() looks at what MPD plays, asks the helper x45art for a picture (one helper at a time, in the
 * background, at low priority), remembers the answers, and keeps the picture that fits in u->art. The skins draw it
 * beside the text (struct ui_np.slide), the classic page too, and the web page gets the same one as /api/art (the file
 * ART_CUR) with a counter in the state.
 *
 *   a file of the library    the picture MPD has for it, else (mode 2) the album cover of the catalogue
 *   an internet radio stream the cover of the song that is announced (mode 2), else the station's logo (mode 2)
 *   anything else            nothing (the DAB slideshow and the other inputs have their own pictures)
 *
 * u->art_mode: 0 none, 1 only what the library holds (nothing leaves the unit), 2 also looked up on the internet
 * (the artist and title, or the station, are sent to the services named in x45art).
 */
#include "ext.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define X45ART  "/home/root/x45new/bin/x45art"
#define ART_DIR "/tmp/x45art"
#define ART_CUR ART_DIR "/cur"
#define ART_BOX 260             /* the picture is fitted into this square on the panel */
#define ART_SMALL 160           /* and, for the page of the large stereo field, into this one */
#define NCACHE  16
#define NWANT   2
#define JOB_MS  45000           /* a helper that runs longer is killed */
#define RETRY_MS 60000          /* a failure that may pass (no network) is tried again after this */
#define LOOK_MS 200             /* the state is looked at this often */

extern const struct ui_ext ext_art;

enum { E_PENDING = 1, E_FOUND, E_MISS, E_RETRY };

struct ent {
	char key[1200];
	char file[64];              /* name in ART_DIR */
	int st;
	long t;
};

struct want {
	char kind[8];               /* the helper's mode: track, station, song */
	char key[1200];
	char arg[3][520];
	int nargs;
};

static struct ent cache[NCACHE];
static int next_slot;
static struct { pid_t pid; struct ent *e; long t0; } job;
static char shown[64];          /* the file of the picture in u->art */
static long last_look;
static unsigned seq;

static struct ent *find(const char *key)
{
	int i;

	for (i = 0; i < NCACHE; i++)
		if (cache[i].st && !strcmp(cache[i].key, key))
			return &cache[i];
	return NULL;
}

/* a free slot, or the oldest one that is not being filled */
static struct ent *slot(void)
{
	int i;

	for (i = 0; i < NCACHE; i++) {
		struct ent *e = &cache[next_slot];

		next_slot = (next_slot + 1) % NCACHE;
		if (e->st != E_PENDING)
			return e;
	}
	return NULL;
}

/* ---------------------------------------------------------------- what to look for --- */

static void set_want(struct want *w, const char *kind, int nargs, const char *a, const char *b, const char *c)
{
	const char *args[3] = { a, b, c };
	size_t n;
	int i;

	memset(w, 0, sizeof *w);
	snprintf(w->kind, sizeof w->kind, "%s", kind);
	w->nargs = nargs;
	n = (size_t)snprintf(w->key, sizeof w->key, "%s", kind);
	for (i = 0; i < nargs; i++) {
		snprintf(w->arg[i], sizeof w->arg[i], "%s", args[i] ? args[i] : "");
		if (n < sizeof w->key - 1)
			n += (size_t)snprintf(w->key + n, sizeof w->key - n, "|%s", w->arg[i]);
	}
}

/* "Artist - Title" of a stream's title tag */
static int split_title(const char *t, char *artist, size_t ca, char *title, size_t ct)
{
	const char *p = strstr(t, " - ");
	size_t n;

	if (!p || p == t || !p[3])
		return 0;
	n = (size_t)(p - t);
	if (n >= ca)
		return 0;
	memcpy(artist, t, n);
	artist[n] = '\0';
	snprintf(title, ct, "%s", p + 3);
	return 1;
}

/* what would fit what plays now, the best one first; returns how many */
static int wants(struct ui *u, struct want *w)
{
	const struct mpdc_status *st = &u->st;
	int stream;

	if (!u->art_mode || u->input != X45_IN_NETWORK || u->air || !mpdc_is_open(&u->mpd) || !u->have_song ||
	    (strcmp(st->state, "play") && strcmp(st->state, "pause")) || !u->song.file[0])
		return 0;
	stream = !strncmp(u->song.file, "http", 4) && st->duration <= 0;
	if (!stream) {
		int web = u->art_mode >= 2;

		set_want(&w[0], "track", 3, u->song.file, web ? u->song.artist : "", web ? u->song.album : "");
		return 1;
	}
	if (u->art_mode < 2)
		return 0;
	{
		char artist[130], title[130];
		int n = 0;

		if (split_title(u->song.title, artist, sizeof artist, title, sizeof title))
			set_want(&w[n++], "song", 2, artist, title, NULL);
		set_want(&w[n++], "station", 2, u->song.file, ui_station_name(u), NULL);
		return n;
	}
}

/* ------------------------------------------------------------------------- helper --- */

static void job_done(struct ui *u, int status)
{
	struct ent *e = job.e;
	char path[128];

	(void)u;
	job.pid = 0;
	job.e = NULL;
	if (!e)
		return;
	snprintf(path, sizeof path, ART_DIR "/%s", e->file);
	if (WIFEXITED(status) && WEXITSTATUS(status) == 0 && access(path, R_OK) == 0) {
		e->st = E_FOUND;
	} else if (WIFEXITED(status) && WEXITSTATUS(status) == 1) {
		e->st = E_MISS;
	} else {
		e->st = E_RETRY;
		e->t = ext_now_ms();
	}
}

static void job_poll(struct ui *u)
{
	int status;

	if (job.pid <= 0)
		return;
	if (waitpid(job.pid, &status, WNOHANG) == job.pid) {
		job_done(u, status);
		return;
	}
	if (ext_since(ext_now_ms(), job.t0) > JOB_MS) {
		kill(-job.pid, SIGKILL);   /* the helper's own group: its curl goes with it */
		kill(job.pid, SIGKILL);
		if (waitpid(job.pid, &status, 0) == job.pid)
			job_done(u, status);
		else
			job_done(u, 0xff00);
	}
}

static void job_start(struct ui *u, const struct want *w)
{
	struct ent *e = slot();
	char out[128], port[16];
	const char *argv[8];
	pid_t pid;
	int i, n = 0;

	if (!e || job.pid > 0)
		return;
	memset(e, 0, sizeof *e);
	snprintf(e->key, sizeof e->key, "%s", w->key);
	snprintf(e->file, sizeof e->file, "%lx-%u.bmp", (unsigned long)time(NULL), seq++);
	snprintf(out, sizeof out, ART_DIR "/%s", e->file);
	snprintf(port, sizeof port, "%d", u->mpd_port);
	argv[n++] = "x45art";
	argv[n++] = w->kind;
	argv[n++] = out;
	for (i = 0; i < w->nargs; i++)
		argv[n++] = w->arg[i];
	argv[n] = NULL;
	pid = fork();
	if (pid < 0)
		return;
	if (pid == 0) {
		int fd = open("/dev/null", O_RDWR);

		setpgid(0, 0);
		if (fd >= 0) {
			dup2(fd, 0);
			dup2(fd, 1);
			dup2(fd, 2);
		}
		if (nice(15) < 0) {
			/* a normal priority is only a little worse */
		}
		setenv("MPD_PORT", port, 1);
		execv(X45ART, (char *const *)argv);
		_exit(127);
	}
	e->st = E_PENDING;
	job.pid = pid;
	job.e = e;
	job.t0 = ext_now_ms();
}

/* ------------------------------------------------------------------------- showing --- */

/* the hard link the web page's /api/art reads: it stays whole when the helper tidies the folder */
static void publish_cur(const char *file)
{
	char src[128];

	unlink(ART_DIR "/.cur.tmp");
	if (!file) {
		unlink(ART_CUR);
		return;
	}
	snprintf(src, sizeof src, ART_DIR "/%s", file);
	if (link(src, ART_DIR "/.cur.tmp") == 0)
		rename(ART_DIR "/.cur.tmp", ART_CUR);
}

static void show(struct ui *u, struct ent *e)
{
	char path[128];

	if (!e) {
		if (shown[0] || u->art.px) {
			x45_image_free(&u->art);
			x45_image_free(&u->art_small);
			shown[0] = '\0';
			publish_cur(NULL);
			u->art_serial++;
			u->dirty = 1;
		}
		return;
	}
	if (!strcmp(shown, e->file) && u->art.px)
		return;
	snprintf(path, sizeof path, ART_DIR "/%s", e->file);
	x45_image_free(&u->art);
	x45_image_free(&u->art_small);
	if (x45_image_load(&u->g, path, ART_BOX, ART_BOX, &u->art) < 0) {
		e->st = E_MISS;   /* it is not a picture after all */
		shown[0] = '\0';
		publish_cur(NULL);
	} else {
		x45_image_load(&u->g, path, ART_SMALL, ART_SMALL, &u->art_small);
		snprintf(shown, sizeof shown, "%s", e->file);
		publish_cur(e->file);
	}
	u->art_serial++;
	u->dirty = 1;
}

static int usable(struct ent *e)
{
	char path[128];

	if (e->st != E_FOUND)
		return 0;
	snprintf(path, sizeof path, ART_DIR "/%s", e->file);
	if (access(path, R_OK) == 0)
		return 1;
	if (!strcmp(shown, e->file))   /* tidied away, but still loaded and linked: it is good for now */
		return 1;
	memset(e, 0, sizeof *e);       /* gone: ask again if it is wanted */
	return 0;
}

static void art_poll(struct ui *u)
{
	struct want w[NWANT];
	struct ent *pick = NULL;
	long now = ext_now_ms();
	int n, i, started = 0;

	job_poll(u);
	if (ext_since(now, last_look) < LOOK_MS)
		return;
	last_look = now;
	mkdir(ART_DIR, 0755);
	n = wants(u, w);
	for (i = 0; i < n; i++) {
		struct ent *e = find(w[i].key);

		if (e && e->st == E_RETRY && ext_since(now, e->t) >= RETRY_MS) {
			memset(e, 0, sizeof *e);
			e = NULL;
		}
		if (e && usable(e)) {
			pick = e;
			break;
		}
		if (!e && !started && job.pid <= 0) {
			job_start(u, &w[i]);
			started = 1;
		}
	}
	show(u, pick);
}

const struct ui_ext ext_art = {
	.name = "art",
	.poll = art_poll,
};

const struct x45_image *ui_art_image(const struct ui *u)
{
	return u->art.px ? &u->art : NULL;
}

const struct x45_image *ui_art_small(const struct ui *u)
{
	return u->art_small.px ? &u->art_small : NULL;
}
