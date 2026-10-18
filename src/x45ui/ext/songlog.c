#define _GNU_SOURCE
/* ext/songlog - the log of what the radio played.
 *
 * Internet radio sends the title of the song with the stream, the DAB+ module the station's dynamic label and an FM
 * station its RadioText. Whenever that text changes and stays for a few seconds, a line goes to the log, with the time
 * and the station: "what was that song at lunch" has an answer. OK on a line marks it (a star); the remote's RED key
 * marks what plays now, YELLOW opens the log. The web page lists, marks, exports and clears it too.
 *
 * The log is a file (songlog.tsv, one line per song, at most SL_MAX of them; the oldest go first). This file is the one
 * writer; x45web only reads it (api_songlog.c). Nothing leaves the unit: there is no lookup of any kind.
 *
 *   line:  id TAB epoch TAB mark TAB source TAB station TAB text      (source: net, dab, fm)
 *
 * etc/x45ui.conf:  songlog = 0|1  (default 1)
 */
#include "ext.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

extern const struct ui_ext ext_songlog;

#define LOG_FILE "/home/root/x45new/songlog.tsv"
#define SL_MAX 500
#define SL_DROP 100       /* when the file is full this many of the oldest go at once (one rewrite, not one per song) */
#define SETTLE_MS 3000L   /* a text counts when it stays this long: a station that has just been tuned shows the old text for a moment */
#define TIME_FLOOR 1735689600L
#define KEY_RED 398
#define KEY_YELLOW 400

struct entry {
	long id, epoch;
	int mark;
	char src[4], station[48], text[100];
};

static struct entry E[SL_MAX];   /* oldest first */
static int N;
static long next_id = 1;
static int loaded, enabled = 1;

/* a candidate that has to stay */
static char pend_key[200];
static long pend_t;
static int pend_have;

static int sel, top;

/* ------------------------------------------------------------------- storage --- */

static void clean(char *s)
{
	for (; *s; s++)
		if (*s == '\t' || *s == '\n' || *s == '\r')
			*s = ' ';
}

static void put_line(FILE *f, const struct entry *e)
{
	fprintf(f, "%ld\t%ld\t%d\t%s\t%s\t%s\n", e->id, e->epoch, e->mark, e->src, e->station, e->text);
}

static void rewrite(void)
{
	char tmp[96];
	FILE *f;
	int i;

	snprintf(tmp, sizeof tmp, "%s.tmp", LOG_FILE);
	f = fopen(tmp, "w");
	if (!f)
		return;
	for (i = 0; i < N; i++)
		put_line(f, &E[i]);
	if (fflush(f) != 0 || ferror(f)) {   /* a full disk must not replace the log with a cut one */
		fclose(f);
		unlink(tmp);
		return;
	}
	fclose(f);
	rename(tmp, LOG_FILE);
}

static void load(void)
{
	char line[400];
	FILE *f = fopen(LOG_FILE, "r");

	loaded = 1;
	enabled = ext_conf_int("songlog", 1) != 0;
	N = 0;
	if (!f)
		return;
	while (fgets(line, sizeof line, f)) {
		struct entry *e;
		char *p[6], *s = line;
		int k;

		line[strcspn(line, "\r\n")] = '\0';
		for (k = 0; k < 6; k++) {
			p[k] = s;
			if (k < 5) {
				s = strchr(s, '\t');
				if (!s)
					break;
				*s++ = '\0';
			}
		}
		if (k < 5)
			continue;
		if (N == SL_MAX) {   /* a longer file than we keep: the oldest are let go */
			memmove(E, E + 1, sizeof E - sizeof *E);
			N--;
		}
		e = &E[N++];
		e->id = atol(p[0]);
		e->epoch = atol(p[1]);
		e->mark = atoi(p[2]) != 0;
		snprintf(e->src, sizeof e->src, "%s", p[3]);
		snprintf(e->station, sizeof e->station, "%s", p[4]);
		snprintf(e->text, sizeof e->text, "%s", p[5]);
		if (e->id >= next_id)
			next_id = e->id + 1;
	}
	fclose(f);
}

static void add(const char *src, const char *station, const char *text)
{
	struct entry *e;
	time_t now = time(NULL);
	FILE *f;

	if (N == SL_MAX) {
		memmove(E, E + SL_DROP, (size_t)(SL_MAX - SL_DROP) * sizeof *E);
		N = SL_MAX - SL_DROP;
		rewrite();
	}
	e = &E[N++];
	memset(e, 0, sizeof *e);
	e->id = next_id++;
	e->epoch = now >= TIME_FLOOR ? (long)now : 0;   /* a clock that was never set stamps nothing rather than a wrong time */
	snprintf(e->src, sizeof e->src, "%s", src);
	snprintf(e->station, sizeof e->station, "%s", station);
	snprintf(e->text, sizeof e->text, "%s", text);
	clean(e->station);
	clean(e->text);
	f = fopen(LOG_FILE, "a");
	if (f) {
		put_line(f, e);
		fclose(f);
	}
}

/* ----------------------------------------------------------- what plays now --- */

/* the text of the song or label now on air, "" when there is none; src is net, dab or fm */
static void now_playing(struct ui *u, const char **src, char *station, size_t sc, char *text, size_t tc)
{
	station[0] = text[0] = '\0';
	*src = "";
	if (u->input == X45_IN_NETWORK) {
		if (!u->have_song || strncmp(u->song.file, "http", 4) || strcmp(u->st.state, "play"))
			return;
		*src = "net";
		if (u->song.title[0])
			snprintf(text, tc, "%s%s%s", u->song.artist[0] ? u->song.artist : "", u->song.artist[0] ? " - " : "", u->song.title);
		snprintf(station, sc, "%s", ui_station_name(u));
		if (!station[0])
			snprintf(station, sc, "%s", u->song.name);
	} else if (u->input == X45_IN_FMRADIO && u->tu_pid > 0 && !strcmp(u->tu_state, "play")) {
		if (u->tu_mode == 1) {
			*src = "dab";
			snprintf(text, tc, "%s", u->tu_text);
			snprintf(station, sc, "%s", u->tu_svc);
		} else {
			*src = "fm";
			snprintf(text, tc, "%s", u->tu_rt);
			snprintf(station, sc, "%s", u->tu_ps);
		}
	}
	/* a label that is only the station's name, or nothing, says nothing about the song */
	if (!strcasecmp(text, station) || !strcmp(text, "-") || !strcasecmp(text, "unknown"))
		text[0] = '\0';
}

static void trim(char *s)
{
	size_t n = strlen(s);

	while (n && (s[n - 1] == ' ' || s[n - 1] == '\t'))
		s[--n] = '\0';
}

static void poll(struct ui *u)
{
	const char *src;
	char station[48], text[100], key[200];
	long now = ext_now_ms();

	if (!loaded)
		load();
	if (!enabled)
		return;
	now_playing(u, &src, station, sizeof station, text, sizeof text);
	trim(text);
	if (!text[0]) {
		pend_have = 0;
		return;
	}
	snprintf(key, sizeof key, "%s|%s|%s", src, station, text);
	if (!pend_have || strcmp(key, pend_key)) {   /* something new: wait and see whether it stays */
		snprintf(pend_key, sizeof pend_key, "%s", key);
		pend_t = now;
		pend_have = 1;
		return;
	}
	if (pend_have == 1 && ext_since(now, pend_t) >= SETTLE_MS) {
		pend_have = 2;   /* logged (or found to be the last line already) */
		if (N && !strcmp(E[N - 1].text, text) && !strcmp(E[N - 1].station, station))
			return;
		add(src, station, text);
		if (u->screen == SCR_EXT && u->ext == &ext_songlog) {
			sel = 0;
			u->dirty = 1;
		}
	}
}

/* the line of what plays now (the newest entry when it is on air), marked or not */
void ui_songlog_mark_now(struct ui *u)
{
	if (!loaded)
		load();
	if (N && pend_have == 2) {
		E[N - 1].mark = 1;
		rewrite();
		say(u, "Marked: %s", E[N - 1].text);
	} else if (pend_have == 1) {
		say(u, "Wait a moment - the song is not in the log yet");
	} else {
		say(u, "Nothing to mark: no song title on air");
	}
}

/* ------------------------------------------------------------------- screen --- */

static void enter(struct ui *u)
{
	(void)u;
	if (!loaded)
		load();
	sel = top = 0;
}

static int shown(void)
{
	return N;
}

static const struct entry *at(int row)   /* row 0 is the newest */
{
	return &E[N - 1 - row];
}

static void rowfn(struct ui *u, int i, int x, int y, int rw, int selected, void *ctx)
{
	struct x45_surface *s = &u->g.back;
	const struct entry *e = at(i);
	uint32_t fg = ext_fg(u, selected), dim = selected ? u->t.bg : u->t.dim;
	char when[24] = "--.-- --:--";

	(void)ctx;
	if (e->epoch >= TIME_FLOOR) {
		time_t t = (time_t)e->epoch;
		struct tm tm;

		localtime_r(&t, &tm);
		strftime(when, sizeof when, "%d.%m %H:%M", &tm);
	}
	if (e->mark)
		x45_text(s, x + 10, y + 9, "*", 3, selected ? u->t.bg : u->t.accent);
	x45_text(s, x + 40, y + 12, when, 2, dim);
	text_fold(s, x + 190, y + 9, e->text, 3, rw - 210, fg);
}

static void draw(struct ui *u)
{
	struct x45_surface *s = &u->g.back;
	int n = shown();

	if (!loaded)
		load();
	if (!enabled)
		x45_text(s, 24, 70, "Song log is off", 3, u->t.warn);
	else
		x45_text(s, 24, 70, "What the radio played", 3, u->t.text);
	if (!n) {
		ext_para(u, 24, 150, s->w - 48,
			 "Nothing yet. Tune an internet radio station, a DAB+ or an FM station that sends song titles: "
			 "every new title lands here with the time and the station.", 3, u->t.dim);
		draw_hints(u, "RETURN|back");
		return;
	}
	if (sel >= n)
		sel = n - 1;
	ext_list(u, 12, 104, s->w - 24, 360, n, sel, &top, rowfn, NULL);
	{
		const struct entry *e = at(sel);
		char line[100];

		snprintf(line, sizeof line, "%s%s%s", e->src[0] ? e->src : "", e->station[0] ? "   " : "", e->station);
		text_fold(s, 24, 480, line, 3, s->w - 48, u->t.accent);
	}
	draw_hints(u, "OK|mark;RED|mark now;left/right|logging on/off;RETURN|back");
}

static void ok(struct ui *u)
{
	(void)u;
	if (!N)
		return;
	E[N - 1 - sel].mark = !E[N - 1 - sel].mark;
	rewrite();
}

static void move(struct ui *u, int d)
{
	int n = shown();

	(void)u;
	if (!n)
		return;
	sel += d;
	if (sel < 0)
		sel = 0;
	if (sel >= n)
		sel = n - 1;
}

static void set_enabled(int on)
{
	char b[4];

	enabled = on != 0;
	snprintf(b, sizeof b, "%d", enabled);
	save_config_key("songlog", b);
	pend_have = 0;
}

static void nav(struct ui *u, int dx, int dy)
{
	if (dy)
		move(u, dy);
	else if (dx)
		set_enabled(!enabled);
}

static int ir(struct ui *u, int code)
{
	if (code == KEY_RED && !u->ir_bounce) {
		ui_songlog_mark_now(u);
		return 1;
	}
	if (code == KEY_YELLOW) {   /* the key that opened it closes it */
		if (!u->ir_bounce)
			ui_ext_close(u);
		return 1;
	}
	return 0;
}

static void json(struct ui *u, FILE *f)
{
	int i, marked = 0;

	(void)u;
	if (!loaded)
		load();
	for (i = 0; i < N; i++)
		marked += E[i].mark;
	fprintf(f, "{\"on\":%d,\"count\":%d,\"marked\":%d,\"last\":%ld}", enabled, N, marked, N ? E[N - 1].id : 0L);
}

/* songlog mark ID | unmark ID | now | clear | log 0|1 | open */
static int cmd(struct ui *u, const char *args)
{
	char w[16] = "";
	long id = 0;
	int i;

	if (!loaded)
		load();
	if (sscanf(args, "%15s %ld", w, &id) < 1)
		return 0;
	if (!strcmp(w, "mark") || !strcmp(w, "unmark")) {
		for (i = 0; i < N; i++)
			if (E[i].id == id) {
				E[i].mark = w[0] == 'm';
				rewrite();
				return 1;
			}
		return 0;
	}
	if (!strcmp(w, "now")) {
		ui_songlog_mark_now(u);
	} else if (!strcmp(w, "clear")) {
		N = 0;
		sel = top = 0;
		unlink(LOG_FILE);
	} else if (!strcmp(w, "log")) {
		set_enabled((int)id);
	} else if (!strcmp(w, "open")) {
		ui_ext_open_name(u, "songlog");
	} else {
		return 0;
	}
	return 1;
}

const struct ui_ext ext_songlog = {
	.name = "songlog",
	.title = "SONG LOG",
	.menu = "Song log",
	.enter = enter,
	.draw = draw,
	.ok = ok,
	.move = move,
	.nav = nav,
	.ir = ir,
	.poll = poll,
	.json = json,
	.cmd = cmd,
};
