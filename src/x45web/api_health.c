#define _GNU_SOURCE
/* api_health - /api/health and /api/logs: how the unit is doing, and what it has written down.
 *
 *   GET /api/health        one JSON object (everything read from /proc and /sys on the spot, nothing is cached):
 *                          up (seconds), load [1,5,15], mem {total,free,avail,swap_total,swap_free} (kB),
 *                          temps [{zone,c}], disks [{mount,total_mb,free_mb}], net [{if,state,speed,rx,tx,rx_err,tx_err,rx_drop}],
 *                          procs {name:{n,rss}} (rss in kB), wd {x45wd's status, null when it is not running},
 *                          stack {bootcount,enabled,childlock,rootfs...}, crashes [[time,who,what]], boots [[time,what,detail]]
 *   GET /api/logs          {"logs":[{"name":"ui","file":"x45ui.log","size":N,"old":[1,2]}, ...]}
 *   GET /api/logs/NAME     the last 300 lines of the log, as text; NAME.1 and NAME.2 are the logs of the runs before
 *   GET /api/logs/NAME/N   the last N lines (1..2000)
 *
 * The names are a fixed list (no path comes from the request). With a web_token set the logs need it too (main.c).
 */
#include "web.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#define ROOT "/home/root/x45new"
#define TAIL_BYTES 262144L

static const struct { const char *name, *file; } LOGS[] = {
	{ "stack", "x45stack.log" }, { "ui", "x45ui.log" }, { "web", "x45web.log" }, { "wd", "x45wd.log" },
	{ "crashes", "crashes.log" }, { "boots", "boots.log" }, { "mpd", "mpd/log" }, { "upnp", "upmpdcli.out" },
	{ "airplay", "shairport.log" }, { "spotify", "librespot.log" },
};
#define NLOGS ((int)(sizeof LOGS / sizeof *LOGS))

static int read_small(const char *path, char *buf, size_t cap)
{
	FILE *f = fopen(path, "r");
	size_t n;

	if (!f)
		return -1;
	n = fread(buf, 1, cap - 1, f);
	fclose(f);
	buf[n] = '\0';
	return (int)n;
}

static long read_long(const char *path, long def)
{
	char b[64];

	return read_small(path, b, sizeof b) > 0 ? atol(b) : def;
}

/* ---------------------------------------------------------------- history files --- */

/* the last `max` lines of a TSV file as [["a","b","c"], ...] */
static void tsv_tail(FILE *o, const char *path, int max)
{
	static char rows[40][400];
	char line[400];
	int n = 0, i, first = 1;
	FILE *f = fopen(path, "r");

	if (max > 40)
		max = 40;
	if (f) {
		while (fgets(line, sizeof line, f)) {
			line[strcspn(line, "\r\n")] = '\0';
			if (!line[0])
				continue;
			if (n == max) {
				memmove(rows[0], rows[1], (size_t)(max - 1) * sizeof rows[0]);
				n--;
			}
			snprintf(rows[n++], sizeof rows[0], "%s", line);
		}
		fclose(f);
	}
	fputc('[', o);
	for (i = n - 1; i >= 0; i--) {   /* newest first */
		char *p[3] = { rows[i], "", "" }, *s = rows[i];
		int k;

		for (k = 0; k < 2; k++) {
			s = strchr(s, '\t');
			if (!s)
				break;
			*s++ = '\0';
			p[k + 1] = s;
		}
		fputs(first ? "[" : ",[", o);
		first = 0;
		jesc(o, p[0]);
		fputc(',', o);
		jesc(o, p[1]);
		fputc(',', o);
		jesc(o, p[2]);
		fputc(']', o);
	}
	fputc(']', o);
}

/* ------------------------------------------------------------------ the sections --- */

static void sec_basic(FILE *o)
{
	char b[512];
	double up = 0, l1 = 0, l5 = 0, l15 = 0;
	long tot = 0, fre = 0, buf = 0, cac = 0, st = 0, sf = 0;
	char *p;

	if (read_small("/proc/uptime", b, sizeof b) > 0)
		up = atof(b);
	if (read_small("/proc/loadavg", b, sizeof b) > 0)
		sscanf(b, "%lf %lf %lf", &l1, &l5, &l15);
	fprintf(o, "\"up\":%.0f,\"load\":[%.2f,%.2f,%.2f]", up, l1, l5, l15);
	{
		FILE *f = fopen("/proc/meminfo", "r");
		char line[160];

		if (f) {
			while (fgets(line, sizeof line, f)) {
				if (!strncmp(line, "MemTotal:", 9))
					tot = atol(line + 9);
				else if (!strncmp(line, "MemFree:", 8))
					fre = atol(line + 8);
				else if (!strncmp(line, "Buffers:", 8))
					buf = atol(line + 8);
				else if (!strncmp(line, "Cached:", 7))
					cac = atol(line + 7);
				else if (!strncmp(line, "SwapTotal:", 10))
					st = atol(line + 10);
				else if (!strncmp(line, "SwapFree:", 9))
					sf = atol(line + 9);
			}
			fclose(f);
		}
	}
	(void)p;
	fprintf(o, ",\"mem\":{\"total\":%ld,\"free\":%ld,\"avail\":%ld,\"swap_total\":%ld,\"swap_free\":%ld}", tot, fre, fre + buf + cac, st, sf);
}

static void sec_temps(FILE *o)
{
	int z, first = 1;

	fputs(",\"temps\":[", o);
	for (z = 0; z < 6; z++) {
		char path[80], type[40] = "";
		long v;

		snprintf(path, sizeof path, "/sys/class/thermal/thermal_zone%d/temp", z);
		v = read_long(path, -1000000L);
		if (v == -1000000L)
			continue;
		snprintf(path, sizeof path, "/sys/class/thermal/thermal_zone%d/type", z);
		if (read_small(path, type, sizeof type) > 0)
			type[strcspn(type, "\r\n")] = '\0';
		fprintf(o, "%s{\"zone\":", first ? "" : ",");
		jesc(o, type[0] ? type : "cpu");
		fprintf(o, ",\"c\":%.1f}", v / 1000.0);
		first = 0;
	}
	fputc(']', o);
}

static void sec_disks(FILE *o)
{
	FILE *f = fopen("/proc/mounts", "r");
	char line[400], seen[16][64];
	int ns = 0, first = 1;
	struct statvfs sv;

	fputs(",\"disks\":[", o);
	if (f) {
		while (fgets(line, sizeof line, f)) {
			char dev[128], mnt[200], fst[32];
			int k, dup = 0;

			if (sscanf(line, "%127s %199s %31s", dev, mnt, fst) != 3)
				continue;
			if (strcmp(mnt, "/") && strncmp(mnt, "/mnt/", 5) && strcmp(mnt, "/home"))
				continue;
			if (!strcmp(fst, "rootfs") || !strncmp(dev, "none", 4))
				continue;
			for (k = 0; k < ns; k++)
				if (!strcmp(seen[k], dev))
					dup = 1;
			if (dup || ns == 16 || statvfs(mnt, &sv) != 0 || !sv.f_blocks)
				continue;
			snprintf(seen[ns++], sizeof seen[0], "%s", dev);
			fprintf(o, "%s{\"mount\":", first ? "" : ",");
			jesc(o, mnt);
			fprintf(o, ",\"fs\":");
			jesc(o, fst);
			fprintf(o, ",\"total_mb\":%llu,\"free_mb\":%llu}", (unsigned long long)sv.f_blocks * sv.f_frsize >> 20,
				(unsigned long long)sv.f_bavail * sv.f_frsize >> 20);
			first = 0;
		}
		fclose(f);
	}
	if (statvfs(ROOT, &sv) == 0 && sv.f_blocks)
		fprintf(o, "%s{\"mount\":\"" ROOT "\",\"fs\":\"program folder\",\"total_mb\":%llu,\"free_mb\":%llu}", first ? "" : ",",
			(unsigned long long)sv.f_blocks * sv.f_frsize >> 20, (unsigned long long)sv.f_bavail * sv.f_frsize >> 20);
	fputc(']', o);
}

static void sec_net(FILE *o)
{
	DIR *d = opendir("/sys/class/net");
	struct dirent *e;
	int first = 1;

	fputs(",\"net\":[", o);
	if (d) {
		while ((e = readdir(d))) {
			char path[160], st[24] = "";
			long speed;

			if (e->d_name[0] == '.' || !strcmp(e->d_name, "lo") || strlen(e->d_name) > 20)
				continue;
			snprintf(path, sizeof path, "/sys/class/net/%s/operstate", e->d_name);
			if (read_small(path, st, sizeof st) > 0)
				st[strcspn(st, "\r\n")] = '\0';
			snprintf(path, sizeof path, "/sys/class/net/%s/speed", e->d_name);
			speed = read_long(path, 0);
			fprintf(o, "%s{\"if\":", first ? "" : ",");
			jesc(o, e->d_name);
			fprintf(o, ",\"state\":");
			jesc(o, st);
			fprintf(o, ",\"speed\":%ld", speed > 0 ? speed : 0L);
#define STAT(key, file) \
	snprintf(path, sizeof path, "/sys/class/net/%s/statistics/" file, e->d_name); \
	fprintf(o, ",\"" key "\":%ld", read_long(path, 0))
			STAT("rx", "rx_bytes");
			STAT("tx", "tx_bytes");
			STAT("rx_err", "rx_errors");
			STAT("tx_err", "tx_errors");
			STAT("rx_drop", "rx_dropped");
#undef STAT
			fputc('}', o);
			first = 0;
		}
		closedir(d);
	}
	fputc(']', o);
}

static void sec_procs(FILE *o)
{
	static const char *const names[] = { "x45ui", "mpd", "x45web", "x45media", "x45wd", "x45stack", "upmpdcli", "shairport-sync",
					     "librespot", "x20", "x45rec", "x45cd", "x45loop" };
	enum { NN = sizeof names / sizeof *names };
	int cnt[NN] = { 0 };
	long rss[NN] = { 0 };
	DIR *d = opendir("/proc");
	struct dirent *e;
	int i, first = 1;

	if (d) {
		while ((e = readdir(d))) {
			char path[64], comm[32] = "", sm[96];
			long pages = 0;

			if (e->d_name[0] < '1' || e->d_name[0] > '9')
				continue;
			snprintf(path, sizeof path, "/proc/%s/comm", e->d_name);
			if (read_small(path, comm, sizeof comm) <= 0)
				continue;
			comm[strcspn(comm, "\r\n")] = '\0';
			for (i = 0; i < (int)NN; i++)
				if (!strcmp(comm, names[i])) {
					snprintf(path, sizeof path, "/proc/%s/statm", e->d_name);
					if (read_small(path, sm, sizeof sm) > 0)
						sscanf(sm, "%*s %ld", &pages);
					cnt[i]++;
					rss[i] += pages * 4;
				}
		}
		closedir(d);
	}
	fputs(",\"procs\":{", o);
	for (i = 0; i < (int)NN; i++) {
		fprintf(o, "%s\"%s\":{\"n\":%d,\"rss\":%ld}", first ? "" : ",", names[i], cnt[i], rss[i]);
		first = 0;
	}
	fputc('}', o);
}

static void sec_wd(FILE *o)
{
	FILE *f = fopen("/tmp/x45wd.status", "r");
	char line[96];
	int first = 1;

	fputs(",\"wd\":", o);
	if (!f) {
		fputs("null", o);
		return;
	}
	fputc('{', o);
	while (fgets(line, sizeof line, f)) {
		char *eq = strchr(line, '=');

		if (!eq)
			continue;
		*eq = '\0';
		fprintf(o, "%s\"%s\":%ld", first ? "" : ",", line, atol(eq + 1));
		first = 0;
	}
	fputc('}', o);
	fclose(f);
}

static void get_health(int fd)
{
	char *buf = NULL;
	size_t len = 0;
	FILE *o = open_memstream(&buf, &len);

	if (!o) {
		respond_text(fd, 500, "out of memory");
		return;
	}
	fputc('{', o);
	sec_basic(o);
	sec_temps(o);
	sec_disks(o);
	sec_net(o);
	sec_procs(o);
	sec_wd(o);
	fprintf(o, ",\"stack\":{\"bootcount\":%ld,\"enabled\":%d,\"childlock\":%d,\"watchdog_off\":%d}", read_long(ROOT "/bootcount", 0),
		access(ROOT "/ENABLED", F_OK) == 0, access(ROOT "/etc/childlock", F_OK) == 0, access(ROOT "/etc/no-watchdog", F_OK) == 0);
	fputs(",\"crashes\":", o);
	tsv_tail(o, ROOT "/crashes.log", 15);
	fputs(",\"boots\":", o);
	tsv_tail(o, ROOT "/boots.log", 10);
	fputc('}', o);
	fclose(o);
	respond_json(fd, 200, buf);
	free(buf);
}

/* ---------------------------------------------------------------------- logs --- */

static void get_log_list(int fd)
{
	char *buf = NULL;
	size_t len = 0;
	FILE *o = open_memstream(&buf, &len);
	int i;

	if (!o) {
		respond_text(fd, 500, "out of memory");
		return;
	}
	fputs("{\"logs\":[", o);
	for (i = 0; i < NLOGS; i++) {
		char path[200];
		struct stat st;
		int k, have;

		snprintf(path, sizeof path, ROOT "/%s", LOGS[i].file);
		fprintf(o, "%s{\"name\":\"%s\",\"file\":\"%s\",\"size\":%ld,\"old\":[", i ? "," : "", LOGS[i].name, LOGS[i].file,
			stat(path, &st) == 0 ? (long)st.st_size : -1L);
		for (k = 1, have = 0; k <= 2; k++) {
			char old[210];

			snprintf(old, sizeof old, "%s.%d", path, k);
			if (access(old, F_OK) == 0)
				fprintf(o, "%s%d", have++ ? "," : "", k);
		}
		fputs("]}", o);
	}
	fputs("],\"kernel\":true}", o);
	fclose(o);
	respond_json(fd, 200, buf);
	free(buf);
}

static void send_tail(int fd, FILE *f, int lines)
{
	static char buf[TAIL_BYTES + 1];
	size_t n;
	long size;
	char *p;
	int have = 0;

	fseek(f, 0, SEEK_END);
	size = ftell(f);
	if (size > TAIL_BYTES)
		fseek(f, size - TAIL_BYTES, SEEK_SET);
	else
		fseek(f, 0, SEEK_SET);
	n = fread(buf, 1, TAIL_BYTES, f);
	buf[n] = '\0';
	if (n && buf[n - 1] == '\n')
		n--;
	/* walk back over `lines` line ends */
	p = buf + n;
	while (p > buf) {
		if (p[-1] == '\n' && ++have == lines)
			break;
		p--;
	}
	if (size > TAIL_BYTES && p == buf) {   /* the first line is cut: leave it out */
		char *nl = strchr(buf, '\n');

		if (nl)
			p = nl + 1;
	}
	respond(fd, 200, "text/plain; charset=utf-8", p, (size_t)(buf + n - p) + (n ? 1 : 0));
}

static void get_log(int fd, const char *spec)
{
	char name[24];
	int old = 0, lines = 300, i;
	const char *slash = strchr(spec, '/');
	size_t nl = slash ? (size_t)(slash - spec) : strlen(spec);
	char *dot;
	char path[220];
	FILE *f;

	if (nl == 0 || nl >= sizeof name) {
		respond_text(fd, 404, "no such log");
		return;
	}
	memcpy(name, spec, nl);
	name[nl] = '\0';
	if (slash) {
		lines = atoi(slash + 1);
		if (lines < 1 || lines > 2000)
			lines = 300;
	}
	if (!strcmp(name, "kernel")) {
		FILE *p = popen("dmesg 2>/dev/null | tail -n 400", "r");
		static char kb[65536];
		size_t n;

		if (!p) {
			respond_text(fd, 500, "cannot run dmesg");
			return;
		}
		n = fread(kb, 1, sizeof kb - 1, p);
		pclose(p);
		respond(fd, 200, "text/plain; charset=utf-8", kb, n);
		return;
	}
	dot = strchr(name, '.');
	if (dot) {
		old = atoi(dot + 1);
		*dot = '\0';
		if (old < 1 || old > 2) {
			respond_text(fd, 404, "no such log");
			return;
		}
	}
	for (i = 0; i < NLOGS; i++)
		if (!strcmp(LOGS[i].name, name))
			break;
	if (i == NLOGS) {
		respond_text(fd, 404, "no such log");
		return;
	}
	if (old)
		snprintf(path, sizeof path, ROOT "/%s.%d", LOGS[i].file, old);
	else
		snprintf(path, sizeof path, ROOT "/%s", LOGS[i].file);
	f = fopen(path, "r");
	if (!f) {
		respond_text(fd, 404, "the log is empty or does not exist");
		return;
	}
	send_tail(fd, f, lines);
	fclose(f);
}

int api_health(int fd, const char *method, const char *path, const char *body, size_t len)
{
	(void)body;
	(void)len;
	if (strcmp(path, "/api/health") && strncmp(path, "/api/logs", 9))
		return 0;
	if (strcmp(method, "GET") && strcmp(method, "HEAD")) {
		respond_text(fd, 405, "read only");
		return 1;
	}
	if (!strcmp(path, "/api/health"))
		get_health(fd);
	else if (!strcmp(path, "/api/logs"))
		get_log_list(fd);
	else if (!strncmp(path, "/api/logs/", 10))
		get_log(fd, path + 10);
	else
		respond_text(fd, 404, "not found");
	return 1;
}
