#define _GNU_SOURCE
/* api_cd - /api/cd: the disc in the drive and its tags, kept on the unit (no lookup on the internet).
 *
 *   GET  /api/cd/meta      {"id":"1a2b3c4d","tracks":[{"n":1,"audio":true,"frames":N}...],
 *                           "cdtext":{"T":{"0":"album","1":"title"},"P":{..},"C":{..}},"user":{same shape}}
 *                          "cdtext" is what the disc carries, "user" what was entered here; a rip uses "user" first.
 *                          {"id":""} when there is no disc.
 *   POST /api/cd/meta      text/plain: an optional first line "id <disc id>" (refused when another disc is in the
 *                          drive), then lines "K <track> <text>" (K = T title, P performer, S songwriter, C composer,
 *                          A arranger, M message, G genre, U UPC; track 0 = the disc). An empty list removes the
 *                          disc's own tags.
 *   GET  /api/cd/riplog    the "rip log.txt" of the folder of the last rip, as text
 *
 * All the work is done by x45cd (cmd_meta / cmd_setmeta), the same program the panel and the recorder ask.
 */
#include "web.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define X45CD "/home/root/x45new/bin/x45cd"
#define REC_STATUS "/tmp/x45rec.status"
#define BODY_MAX 30000

static void get_meta(int fd)
{
	static char out[65536];
	FILE *p = popen(X45CD " meta 2>/dev/null", "r");
	size_t n = 0;

	if (!p) {
		respond_text(fd, 500, "cannot run x45cd");
		return;
	}
	while (n < sizeof out - 1) {
		size_t r = fread(out + n, 1, sizeof out - 1 - n, p);

		if (r == 0)
			break;
		n += r;
	}
	pclose(p);
	out[n] = '\0';
	respond_json(fd, 200, n ? out : "{\"id\":\"\",\"tracks\":[]}");
}

static void post_meta(int fd, const char *body, size_t len)
{
	FILE *p;
	int st;

	if (len > BODY_MAX) {
		respond_text(fd, 413, "too much text");
		return;
	}
	p = popen(X45CD " setmeta 2>&1", "w");
	if (!p) {
		respond_text(fd, 500, "cannot run x45cd");
		return;
	}
	if (len && fwrite(body, 1, len, p) != len) {
		pclose(p);
		respond_text(fd, 500, "cannot write");
		return;
	}
	st = pclose(p);
	if (st == 0)
		respond_json(fd, 200, "{\"ok\":true}");
	else if (st != -1 && (st >> 8) == 5)
		respond_json(fd, 409, "{\"ok\":false,\"error\":\"the disc has changed\"}");
	else if (st != -1 && (st >> 8) == 3)
		respond_json(fd, 409, "{\"ok\":false,\"error\":\"there is no disc\"}");
	else
		respond_json(fd, 500, "{\"ok\":false,\"error\":\"could not save\"}");
}

static void get_riplog(int fd)
{
	static char out[65536];
	char line[400], dir[320] = "", path[400];
	FILE *f = fopen(REC_STATUS, "r");
	size_t n;

	if (f) {
		while (fgets(line, sizeof line, f))
			if (!strncmp(line, "dir=", 4)) {
				line[strcspn(line, "\r\n")] = '\0';
				snprintf(dir, sizeof dir, "%s", line + 4);
			}
		fclose(f);
	}
	/* the folder comes from the recorder's own status file, and must be on an attached disk */
	if (strncmp(dir, "/mnt/", 5) || path_has_dotdot(dir)) {
		respond_text(fd, 404, "no rip yet");
		return;
	}
	snprintf(path, sizeof path, "%s/rip log.txt", dir);
	f = fopen(path, "r");
	if (!f) {
		respond_text(fd, 404, "no rip log");
		return;
	}
	n = fread(out, 1, sizeof out, f);
	fclose(f);
	respond(fd, 200, "text/plain; charset=utf-8", out, n);
}

int api_cd(int fd, const char *method, const char *path, const char *body, size_t len)
{
	int post = !strcmp(method, "POST");

	if (strncmp(path, "/api/cd/", 8))
		return 0;
	if (!strcmp(path, "/api/cd/meta")) {
		if (post)
			post_meta(fd, body, len);
		else
			get_meta(fd);
	} else if (!strcmp(path, "/api/cd/riplog") && !post) {
		get_riplog(fd);
	} else {
		respond_text(fd, 404, "not found");
	}
	return 1;
}
