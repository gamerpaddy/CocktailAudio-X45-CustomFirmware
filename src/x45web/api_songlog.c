#define _GNU_SOURCE
/* api_songlog - /api/songlog: the log of what the radio played (the panel keeps it, ext/songlog.c).
 *
 *   GET /api/songlog     {"entries":[{"id":N,"t":EPOCH,"mark":0|1,"src":"net|dab|fm","station":"..","text":".."}, ...]}
 *                        newest first; "t" is 0 when the unit's clock was not set at the time
 *
 * Marking, clearing and switching the log off go through POST /api/cmd (`songlog mark ID`, `songlog unmark ID`,
 * `songlog clear`, `songlog log 0|1`); this module only reads the file the panel writes.
 */
#include "web.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG_FILE "/home/root/x45new/songlog.tsv"
#define MAX_ENTRIES 600

struct row {
	long id, epoch;
	int mark;
	char src[8], station[64], text[128];
};

static void get_log(int fd)
{
	static struct row rows[MAX_ENTRIES];
	char line[512];
	int n = 0, i;
	FILE *f = fopen(LOG_FILE, "r"), *o;
	char *buf = NULL;
	size_t blen = 0;

	if (f) {
		while (fgets(line, sizeof line, f)) {
			char *p[6], *s = line;
			int k;
			struct row *r;

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
			if (n == MAX_ENTRIES) {
				memmove(rows, rows + 1, sizeof rows - sizeof *rows);
				n--;
			}
			r = &rows[n++];
			r->id = atol(p[0]);
			r->epoch = atol(p[1]);
			r->mark = atoi(p[2]) != 0;
			snprintf(r->src, sizeof r->src, "%s", p[3]);
			snprintf(r->station, sizeof r->station, "%s", p[4]);
			snprintf(r->text, sizeof r->text, "%s", p[5]);
		}
		fclose(f);
	}
	o = open_memstream(&buf, &blen);
	if (!o) {
		respond_text(fd, 500, "out of memory");
		return;
	}
	fputs("{\"entries\":[", o);
	for (i = n - 1; i >= 0; i--) {
		fprintf(o, "%s{\"id\":%ld,\"t\":%ld,\"mark\":%d,\"src\":", i == n - 1 ? "" : ",", rows[i].id, rows[i].epoch, rows[i].mark);
		jesc(o, rows[i].src);
		fputs(",\"station\":", o);
		jesc(o, rows[i].station);
		fputs(",\"text\":", o);
		jesc(o, rows[i].text);
		fputc('}', o);
	}
	fputs("]}", o);
	fclose(o);
	respond_json(fd, 200, buf);
	free(buf);
}

int api_songlog(int fd, const char *method, const char *path, const char *body, size_t len)
{
	(void)body;
	(void)len;
	if (strcmp(path, "/api/songlog"))
		return 0;
	if (strcmp(method, "GET") && strcmp(method, "HEAD")) {
		respond_text(fd, 405, "use POST /api/cmd with `songlog ...`");
		return 1;
	}
	get_log(fd);
	return 1;
}
