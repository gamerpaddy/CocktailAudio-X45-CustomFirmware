/* x45web - the web interface (port 80).
 *
 * Serves the page in <webdir> and a few endpoints. It never touches the hardware: it
 * hands commands to x45ui (/tmp/x45ui.cmd) and publishes the state x45ui writes
 * (/tmp/x45ui.json), so the browser does exactly what the panel keys do.
 *
 *   GET  /                    index.html from the web directory (and any file below it)
 *   GET  /api/state           the UI state, JSON
 *   POST /api/cmd             body: command lines for x45ui (see x45ui web bridge)
 *   GET  /api/screen.bmp      what the panel shows right now
 *   GET  /api/slide           the current DAB slideshow picture
 *   GET  /api/meter           spectrum / level data (asks x45ui to run the analyzer)
 *   GET  /api/system          uptime, load, memory, temperatures, network, storage, services
 *   POST /api/library/delete  body: path relative to the library; folders and files
 *   GET|PUT|POST /api/files/..  the file manager of files.html (api_files.c; docs/X45-FILES.md); what it changes is
 *                             reported to MPD a few seconds later by libsync.c
 *   /api/wifi/..               the WiFi settings (api_wifi.c)
 *
 * Everything under /api is also served as /api/v1 (same thing, for apps). CORS is open;
 * if etc/x45ui.conf has "web_token = X", every POST must carry "Authorization: Bearer X"
 * (or "X-Token: X").
 *
 * usage: x45web [-p port] [-d webdir]
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <ifaddrs.h>
#include <limits.h>
#include <sys/statvfs.h>
#include <sys/sysinfo.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#define CMD_FILE   "/tmp/x45ui.cmd"
#define STATE_FILE "/tmp/x45ui.json"
#define SLIDE_FILE "/tmp/x45-slide.img"
#define METER_FILE "/tmp/x45meter.json"
#define METER_WANT "/tmp/x45meter.want"
#define CONF_FILE  "/home/root/x45new/etc/x45ui.conf"
#define STATIONS_FILE "/home/root/x45new/etc/stations.txt"
#define MEDIA_DIR  "/home/root/x45new/media"
#define HOLD_FILE  "/tmp/x45media.hold"   /* the media helper leaves a disk alone while this exists */
#define LOCK_FILE  "/tmp/x45files.lock"   /* one change to the disks at a time (shared with the file manager) */
#define FB_DEV     "/dev/fb0"
#include "jpegenc.h"
#include "web.h"

#define FB_W 1024
#define FB_H 600

static const char *webdir = "/home/root/x45new/web";

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

void respond(int fd, int code, const char *type, const void *body, size_t len)
{
	char h[512];   /* the longest header set (a 507 with a JSON type) is about 330 bytes */
	const char *msg = code == 200 ? "OK" : code == 204 ? "No Content" : code == 400 ? "Bad Request" : code == 401 ? "Unauthorized" : code == 403 ? "Forbidden" :
			  code == 404 ? "Not Found" : code == 409 ? "Conflict" : code == 411 ? "Length Required" : code == 413 ? "Payload Too Large" :
			  code == 500 ? "Internal Server Error" : code == 503 ? "Service Unavailable" : code == 507 ? "Insufficient Storage" : "Error";
	int n = snprintf(h, sizeof h,
			 "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
			 "Cache-Control: no-store\r\nAccess-Control-Allow-Origin: *\r\n"
				 "Access-Control-Allow-Headers: Authorization, X-Token, Content-Type\r\n"
				 "Access-Control-Allow-Methods: GET, POST, PUT, OPTIONS\r\nConnection: close\r\n\r\n",
			 code, msg, type, len);

	if (n >= (int)sizeof h)
		n = (int)sizeof h - 1;
	send_all(fd, h, (size_t)n);
	if (len)
		send_all(fd, body, len);
}

void respond_text(int fd, int code, const char *text)
{
	respond(fd, code, "text/plain; charset=utf-8", text, strlen(text));
}

void respond_json(int fd, int code, const char *json)
{
	respond(fd, code, "application/json; charset=utf-8", json, strlen(json));
}

static char *slurp(const char *path, size_t *len, size_t max)
{
	FILE *f = fopen(path, "rb");
	char *b;
	long n;

	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (n < 0 || (size_t)n > max || !(b = malloc((size_t)n + 1))) {
		fclose(f);
		return NULL;
	}
	if (fread(b, 1, (size_t)n, f) != (size_t)n) {
		free(b);
		fclose(f);
		return NULL;
	}
	fclose(f);
	b[n] = '\0';
	*len = (size_t)n;
	return b;
}

static const char *mime(const char *path)
{
	const char *e = strrchr(path, '.');

	if (!e)
		return "application/octet-stream";
	if (!strcmp(e, ".html")) return "text/html; charset=utf-8";
	if (!strcmp(e, ".js")) return "text/javascript; charset=utf-8";
	if (!strcmp(e, ".css")) return "text/css; charset=utf-8";
	if (!strcmp(e, ".svg")) return "image/svg+xml";
	if (!strcmp(e, ".png")) return "image/png";
	if (!strcmp(e, ".ico")) return "image/x-icon";
	if (!strcmp(e, ".json")) return "application/json";
	return "application/octet-stream";
}

/* the panel as a top-down 32-bit BMP (the framebuffer is BGRA, so it is a copy) */
static void send_screen(int fd)
{
	static unsigned char bmp[54 + FB_W * FB_H * 4];
	int f = open(FB_DEV, O_RDONLY);
	size_t got = 0;
	uint32_t v;

	if (f < 0) {
		respond_text(fd, 503, "no framebuffer");
		return;
	}
	while (got < (size_t)FB_W * FB_H * 4) {
		ssize_t r = read(f, bmp + 54 + got, (size_t)FB_W * FB_H * 4 - got);

		if (r <= 0)
			break;
		got += (size_t)r;
	}
	close(f);
	if (got != (size_t)FB_W * FB_H * 4) {
		respond_text(fd, 503, "short framebuffer read");
		return;
	}
	memset(bmp, 0, 54);
	bmp[0] = 'B';
	bmp[1] = 'M';
	v = 54 + FB_W * FB_H * 4;
	memcpy(bmp + 2, &v, 4);
	v = 54;
	memcpy(bmp + 10, &v, 4);
	v = 40;
	memcpy(bmp + 14, &v, 4);
	v = FB_W;
	memcpy(bmp + 18, &v, 4);
	v = (uint32_t)-FB_H;           /* top-down */
	memcpy(bmp + 22, &v, 4);
	bmp[26] = 1;
	bmp[28] = 32;
	/* alpha is whatever the framebuffer holds; BI_RGB 32-bit ignores it */
	respond(fd, 200, "image/bmp", bmp, sizeof bmp);
}

/* The panel as a motion JPEG stream (multipart/x-mixed-replace): what an <img> tag plays by itself. A frame is only
 * sent when the screen changed (and once a second regardless, so a dead client is noticed), at most about 3 a
 * second. Half size (a 2x2 average, soft text) unless full=1. Runs in the connection's own process. */
static void send_stream(int fd, int full)
{
	static uint32_t cur[FB_W * FB_H], prev[FB_W * FB_H], small[(FB_W / 2) * (FB_H / 2)];
	static uint8_t jpg[400 * 1024];
	const char *head = "HTTP/1.1 200 OK\r\nContent-Type: multipart/x-mixed-replace; boundary=x45frame\r\n"
			   "Cache-Control: no-store\r\nConnection: close\r\nAccess-Control-Allow-Origin: *\r\n\r\n";
	struct timespec ts;
	long last_send = 0, started;
	int first = 1;

	send_all(fd, head, strlen(head));
	clock_gettime(CLOCK_MONOTONIC, &ts);
	started = ts.tv_sec;
	for (;;) {
		int f = open(FB_DEV, O_RDONLY);
		size_t got = 0, n;
		long now;
		char part[160];
		int pl;
		const uint32_t *src = cur;
		int sw = FB_W, sh = FB_H;

		if (f < 0)
			return;
		while (got < sizeof cur) {
			ssize_t r = read(f, (char *)cur + got, sizeof cur - got);

			if (r <= 0)
				break;
			got += (size_t)r;
		}
		close(f);
		if (got != sizeof cur)
			return;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		now = ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
		if (!first && memcmp(cur, prev, sizeof cur) == 0 && now - last_send < 1000) {
			usleep(120000);
			continue;
		}
		if (!full) {
			int x, y;

			for (y = 0; y < FB_H / 2; y++)
				for (x = 0; x < FB_W / 2; x++) {
					const uint32_t *p = cur + (size_t)(2 * y) * FB_W + 2 * x;
					uint32_t a = p[0], b = p[1], c = p[FB_W], d = p[FB_W + 1];
					uint32_t blue = ((a & 0xff) + (b & 0xff) + (c & 0xff) + (d & 0xff)) / 4;
					uint32_t green = (((a >> 8) & 0xff) + ((b >> 8) & 0xff) + ((c >> 8) & 0xff) + ((d >> 8) & 0xff)) / 4;
					uint32_t red = (((a >> 16) & 0xff) + ((b >> 16) & 0xff) + ((c >> 16) & 0xff) + ((d >> 16) & 0xff)) / 4;

					small[(size_t)y * (FB_W / 2) + x] = red << 16 | green << 8 | blue;
				}
			src = small;
			sw = FB_W / 2;
			sh = FB_H / 2;
		}
		n = jpeg_encode_bgra(src, sw, sh, sw, full ? 80 : 86, jpg, sizeof jpg);
		if (n == 0)
			return;
		pl = snprintf(part, sizeof part, "--x45frame\r\nContent-Type: image/jpeg\r\nContent-Length: %zu\r\n\r\n", n);
		if (write(fd, part, (size_t)pl) != pl || write(fd, jpg, n) != (ssize_t)n || write(fd, "\r\n", 2) != 2)
			return;   /* the viewer went away (or stalled for 5 s) */
		memcpy(prev, cur, sizeof cur);
		first = 0;
		last_send = now;
		if (ts.tv_sec - started > 6 * 3600)
			return;   /* a stream is not kept for ever */
		usleep(160000);   /* reading the panel memory and encoding take about as long again: about 3 frames a second */
	}
}

/* commands: printable lines only (control characters dropped, UTF-8 kept: file and station names have umlauts),
 * appended for x45ui to pick up */
static void post_cmd(int fd, const char *body, size_t len)
{
	char clean[4096];
	size_t i, n = 0;
	int f;

	for (i = 0; i < len && n < sizeof clean - 2; i++) {
		unsigned char c = (unsigned char)body[i];

		if (c == '\n' || (c >= 32 && c != 127))
			clean[n++] = (char)c;
	}
	if (n && clean[n - 1] != '\n')
		clean[n++] = '\n';
	f = open(CMD_FILE, O_WRONLY | O_CREAT | O_APPEND, 0666);
	if (f < 0) {
		respond_text(fd, 503, "cannot queue the command");
		return;
	}
	if (write(f, clean, n) < 0) {
		close(f);
		respond_text(fd, 503, "cannot queue the command");
		return;
	}
	close(f);
	respond_text(fd, 200, "ok");
}

static int conf_get(const char *key, char *out, size_t sz)
{
	FILE *f = fopen(CONF_FILE, "r");
	char line[256];
	size_t kl = strlen(key);
	int found = 0;

	if (!f)
		return 0;
	while (fgets(line, sizeof line, f)) {
		char *v = line;

		if (strncmp(v, key, kl))
			continue;
		v += kl;
		while (*v == ' ' || *v == '\t')
			v++;
		if (*v != '=')
			continue;
		v++;
		while (*v == ' ' || *v == '\t')
			v++;
		v[strcspn(v, "\r\n")] = '\0';
		snprintf(out, sz, "%s", v);
		found = 1;
	}
	fclose(f);
	return found;
}

void jesc(FILE *f, const char *t)
{
	fputc('"', f);
	for (; *t; t++) {
		unsigned char c = (unsigned char)*t;

		if (c == '"' || c == '\\')
			fprintf(f, "\\%c", c);
		else if (c < 32)
			fputc(' ', f);
		else
			fputc(c, f);
	}
	fputc('"', f);
}

static char *read_small(const char *path)
{
	static char buf[8][512];
	static int n;
	char *b = buf[n++ & 7];
	FILE *f = fopen(path, "r");
	size_t r;

	b[0] = '\0';
	if (!f)
		return b;
	r = fread(b, 1, 511, f);
	b[r] = '\0';
	b[strcspn(b, "\r\n")] = '\0';
	fclose(f);
	return b;
}

static int proc_running(const char *name)
{
	FILE *f;
	char cmd[128];
	char *d;
	struct dirent *e;
	DIR *dir = opendir("/proc");
	int found = 0;

	if (!dir)
		return 0;
	while (!found && (e = readdir(dir))) {
		if (e->d_name[0] < '0' || e->d_name[0] > '9')
			continue;
		snprintf(cmd, sizeof cmd, "/proc/%s/comm", e->d_name);
		f = fopen(cmd, "r");
		if (!f)
			continue;
		if (fgets(cmd, sizeof cmd, f)) {
			d = cmd + strcspn(cmd, "\r\n");
			*d = '\0';
			if (!strcmp(cmd, name))
				found = 1;
		}
		fclose(f);
	}
	closedir(dir);
	return found;
}

static void send_system(int fd)
{
	static char out[8192];
	FILE *f = fmemopen(out, sizeof out, "w");
	struct sysinfo si;
	struct statvfs sv;
	struct ifaddrs *ifa, *i;
	char line[512], mac[32] = "";
	static const char *const svc[] = { "x45ui", "mpd", "x45tuner", "x45web", "x45loop", "x45rec",
					   "shairport-sync", "librespot", "upmpdcli", "x45media", "x45cd", NULL };
	int k, first;
	long t;

	if (!f) {
		respond_text(fd, 503, "no memory");
		return;
	}
	sysinfo(&si);
	fprintf(f, "{\"uptime\":%ld,\"load\":[%.2f,%.2f,%.2f],\"mem_total\":%lu,\"mem_free\":%lu,\"procs\":%u",
		(long)si.uptime, si.loads[0] / 65536.0, si.loads[1] / 65536.0, si.loads[2] / 65536.0,
		(unsigned long)(si.totalram / 1024 * si.mem_unit / 1024), (unsigned long)(si.freeram / 1024 * si.mem_unit / 1024),
		(unsigned)si.procs);
	{
		FILE *m = fopen("/proc/meminfo", "r");

		if (m) {
			unsigned long v;

			while (fgets(line, sizeof line, m))
				if (sscanf(line, "MemAvailable: %lu", &v) == 1)
					fprintf(f, ",\"mem_avail\":%lu", v / 1024);
			fclose(m);
		}
	}
	/* temperatures: whatever thermal zones exist */
	fputs(",\"temps\":[", f);
	first = 1;
	for (k = 0; k < 6; k++) {
		char path[96];

		snprintf(path, sizeof path, "/sys/class/thermal/thermal_zone%d/temp", k);
		if (access(path, R_OK))
			continue;
		t = atol(read_small(path));
		fprintf(f, "%s%.1f", first ? "" : ",", t / 1000.0);
		first = 0;
	}
	fputc(']', f);
	fprintf(f, ",\"cpu_mhz\":%ld", atol(read_small("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq")) / 1000);
	fputs(",\"kernel\":", f);
	jesc(f, read_small("/proc/sys/kernel/osrelease"));
	fputs(",\"hostname\":", f);
	jesc(f, read_small("/proc/sys/kernel/hostname"));
	/* network */
	fputs(",\"net\":[", f);
	first = 1;
	if (getifaddrs(&ifa) == 0) {
		for (i = ifa; i; i = i->ifa_next) {
			char ip[32], path[96];

			if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !strcmp(i->ifa_name, "lo"))
				continue;
			inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, ip, sizeof ip);
			snprintf(path, sizeof path, "/sys/class/net/%s/address", i->ifa_name);
			snprintf(mac, sizeof mac, "%s", read_small(path));
			fprintf(f, "%s{\"if\":", first ? "" : ",");
			jesc(f, i->ifa_name);
			fprintf(f, ",\"ip\":\"%s\",\"mac\":", ip);
			jesc(f, mac);
			snprintf(path, sizeof path, "/sys/class/net/%s/speed", i->ifa_name);
			fprintf(f, ",\"speed\":%ld}", atol(read_small(path)));
			first = 0;
		}
		freeifaddrs(ifa);
	}
	fputc(']', f);
	/* storage */
	fputs(",\"disks\":[", f);
	first = 1;
	{
		FILE *m = fopen("/proc/mounts", "r");
		char dev[128], mp[128], fs[32], opt[128];

		while (m && fgets(line, sizeof line, m)) {
			if (sscanf(line, "%127s %127s %31s %127s", dev, mp, fs, opt) != 4)
				continue;
			if (strncmp(dev, "/dev/", 5) || statvfs(mp, &sv) != 0)
				continue;
			fprintf(f, "%s{\"dev\":", first ? "" : ",");
			jesc(f, dev);
			fputs(",\"mount\":", f);
			jesc(f, mp);
			fputs(",\"fs\":", f);
			jesc(f, fs);
			fprintf(f, ",\"rw\":%d,\"total_mb\":%llu,\"free_mb\":%llu}", !strncmp(opt, "rw", 2),
				(unsigned long long)sv.f_blocks * sv.f_frsize / 1048576ull,
				(unsigned long long)sv.f_bavail * sv.f_frsize / 1048576ull);
			first = 0;
		}
		if (m)
			fclose(m);
	}
	fputc(']', f);
	/* services */
	fputs(",\"services\":{", f);
	for (k = 0; svc[k]; k++)
		fprintf(f, "%s\"%s\":%s", k ? "," : "", svc[k], proc_running(svc[k]) ? "true" : "false");
	fputc('}', f);
	/* USB devices */
	fputs(",\"usb\":[", f);
	first = 1;
	{
		DIR *d = opendir("/sys/bus/usb/devices");
		struct dirent *e;

		while (d && (e = readdir(d))) {
			char path[300], v[16], pr[16], nm[96];

			if (e->d_name[0] == '.' || strchr(e->d_name, ':') || !strncmp(e->d_name, "usb", 3))
				continue;
			snprintf(path, sizeof path, "/sys/bus/usb/devices/%s/idVendor", e->d_name);
			snprintf(v, sizeof v, "%s", read_small(path));
			snprintf(path, sizeof path, "/sys/bus/usb/devices/%s/idProduct", e->d_name);
			snprintf(pr, sizeof pr, "%s", read_small(path));
			snprintf(path, sizeof path, "/sys/bus/usb/devices/%s/product", e->d_name);
			snprintf(nm, sizeof nm, "%s", read_small(path));
			if (!v[0])
				continue;
			fprintf(f, "%s{\"id\":\"%s:%s\",\"port\":", first ? "" : ",", v, pr);
			jesc(f, e->d_name);
			fputs(",\"name\":", f);
			jesc(f, nm);
			fputc('}', f);
			first = 0;
		}
		if (d)
			closedir(d);
	}
	fputc(']', f);
	fputc('}', f);
	fclose(f);
	respond(fd, 200, "application/json; charset=utf-8", out, strlen(out));
}

static void send_meter(int fd)
{
	size_t n;
	char *b;
	int w = open(METER_WANT, O_WRONLY | O_CREAT, 0666);

	if (w >= 0) {
		/* a fresh mtime is what tells x45ui somebody is looking */
		utimensat(AT_FDCWD, METER_WANT, NULL, 0);
		close(w);
	}
	{
		struct stat st;

		/* a file the analyzer stopped writing is not a live reading */
		if (stat(METER_FILE, &st) != 0 || time(NULL) - st.st_mtime > 2) {
			respond(fd, 200, "application/json; charset=utf-8", "{\"ok\":false}", 12);
			return;
		}
	}
	b = slurp(METER_FILE, &n, 64 << 10);
	if (!b) {
		respond(fd, 200, "application/json; charset=utf-8", "{\"ok\":false}", 12);
		return;
	}
	respond(fd, 200, "application/json; charset=utf-8", b, n);
	free(b);
}

/* delete a file or folder below a read-write mount; the mount itself and the library
 * root links are never deleted */
static int rmtree(const char *path, int depth)
{
	struct stat st;
	DIR *d;
	struct dirent *e;
	char sub[1024];

	if (lstat(path, &st) != 0)
		return -1;
	if (!S_ISDIR(st.st_mode))
		return unlink(path);
	if (depth > 24)
		return -1;
	d = opendir(path);
	if (!d)
		return -1;
	while ((e = readdir(d))) {
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		/* a cut-short name could be another, existing path: never delete by one */
		if ((size_t)snprintf(sub, sizeof sub, "%s/%s", path, e->d_name) >= sizeof sub)
			continue;
		rmtree(sub, depth + 1);
	}
	closedir(d);
	return rmdir(path);
}

static char mount_dev[128];   /* the device of the mount mount_rw found */

static int mount_rw(const char *real, char *root, size_t sz)
{
	FILE *m = fopen("/proc/mounts", "r");
	char line[512], dev[128], mp[128], fs[32], opt[128];
	size_t best = 0;
	int rw = 0;

	if (!m)
		return 0;
	while (fgets(line, sizeof line, m)) {
		size_t l;

		if (sscanf(line, "%127s %127s %31s %127s", dev, mp, fs, opt) != 4 || strncmp(dev, "/dev/", 5))
			continue;
		l = strlen(mp);
		if (!strncmp(real, mp, l) && (real[l] == '/' || real[l] == '\0') && l > best && strcmp(mp, "/")) {
			best = l;
			rw = !strncmp(opt, "rw", 2);
			snprintf(root, sz, "%s", mp);
			snprintf(mount_dev, sizeof mount_dev, "%s", dev);
		}
	}
	fclose(m);
	return best && rw;
}

static void library_delete(int fd, const char *body, size_t len)
{
	char rel[600], full[900], real[PATH_MAX], root[160], base[PATH_MAX], parent[PATH_MAX];
	size_t n = len < sizeof rel - 1 ? len : sizeof rel - 1;
	char *e;
	int rw, rc, remounted = 0, hold = -1, lock;

	memcpy(rel, body, n);
	rel[n] = '\0';
	rel[strcspn(rel, "\r\n")] = '\0';
	if (!rel[0] || path_has_dotdot(rel) || rel[0] == '/') {
		respond_text(fd, 400, "bad path");
		return;
	}
	snprintf(full, sizeof full, "%s/%s", MEDIA_DIR, rel);
	if (!realpath(full, real)) {
		respond_text(fd, 404, "no such file");
		return;
	}
	/* the first path element is a link into a mount; it must not be deleted itself */
	e = strchr(rel, '/');
	if (!e || !e[1]) {
		respond_text(fd, 403, "that is a library root, not deletable");
		return;
	}
	{
		char top[700];

		snprintf(top, sizeof top, "%s/%.*s", MEDIA_DIR, (int)(e - rel), rel);
		if (!realpath(top, base) || !strcmp(base, real)) {
			respond_text(fd, 403, "that is a library root, not deletable");
			return;
		}
	}
	/* the file manager changes the disks under this lock; a deletion must not remount one under an upload */
	lock = open(LOCK_FILE, O_CREAT | O_RDWR | O_CLOEXEC, 0666);
	if (lock >= 0 && flock(lock, LOCK_EX | LOCK_NB) != 0) {
		close(lock);
		respond_text(fd, 503, "busy with another change");
		return;
	}
	root[0] = '\0';
	rw = mount_rw(real, root, sizeof root);
	if (!root[0] || !strcmp(root, real)) {
		if (lock >= 0)
			close(lock);
		respond_text(fd, 403, root[0] ? "that is a whole disk, not deletable" : "that is not a disk");
		return;
	}
	/* an attached disk is mounted read-only by the media helper: switch it for the deletion, like the panel does,
	 * and tell the helper not to put it back in the middle (it looks for the marker) */
	if (!rw) {
		char cmd[300];

		if (strncmp(mount_dev, "/dev/sd", 7) != 0 || strpbrk(root, "'\\")) {
			if (lock >= 0)
				close(lock);
			respond_text(fd, 403, "that storage is read only");
			return;
		}
		hold = open(HOLD_FILE, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
		snprintf(cmd, sizeof cmd, "mount -o remount,rw '%s' >/dev/null 2>&1", root);
		if (system(cmd) != 0) {
			if (hold >= 0) {
				close(hold);
				unlink(HOLD_FILE);
			}
			if (lock >= 0)
				close(lock);
			respond_text(fd, 403, "that storage is read only");
			return;
		}
		remounted = 1;
	}
	snprintf(parent, sizeof parent, "%s", real);
	e = strrchr(parent, '/');
	if (e && e != parent)
		*e = '\0';
	rc = rmtree(real, 0);
	if (rc == 0)
		prune_empty_dirs(real, base);   /* the album folder that just lost its last track goes with it */
	if (remounted) {
		char cmd[300];

		sync();
		snprintf(cmd, sizeof cmd, "mount -o remount,ro '%s' >/dev/null 2>&1", root);
		(void)system(cmd);
	}
	if (hold >= 0) {
		close(hold);
		unlink(HOLD_FILE);
	}
	if (lock >= 0)
		close(lock);   /* releases the lock */
	if (rc != 0)
		respond_text(fd, 503, "could not delete everything");
	else
		respond_text(fd, 200, "ok");
	/* the panel forgets the item at once and stops what played from it; MPD looks at the folder that lost something
	 * (a run of deletions is one scan). The folder that held it may be gone too (it was left empty): the nearest
	 * one that is still there is what libsync updates. */
	if (rc == 0)
		libsync_gone(rel);
	libsync_note(parent);
	libsync_kick(fd);
}

/* ---------------------------------------------------------------- favourites --- */
/* The station file has three kinds of line: comments and blank lines (the header), the block the FRITZ!Box
 * sync manages, and the owner's own stations "Name | url". The page edits the last kind only. */

/* GET: "name<TAB>url" per favourite */
static void favourites_get(int fd)
{
	FILE *f = fopen(STATIONS_FILE, "r");
	char line[1200], *out = malloc(1 << 16);
	size_t n = 0;
	int managed = 0;

	if (!out) {
		if (f)
			fclose(f);
		respond_text(fd, 500, "out of memory");
		return;
	}
	while (f && fgets(line, sizeof line, f)) {
		char *bar, *name, *url;

		if (strstr(line, "fritzbox begin")) {
			managed = 1;
			continue;
		}
		if (strstr(line, "fritzbox end")) {
			managed = 0;
			continue;
		}
		if (managed || line[0] == '#' || !(bar = strchr(line, '|')))
			continue;
		*bar = '\0';
		name = line + strspn(line, " \t");
		while (*name && name[strlen(name) - 1] == ' ')
			name[strlen(name) - 1] = '\0';
		url = bar + 1 + strspn(bar + 1, " \t");
		url[strcspn(url, "\r\n")] = '\0';
		while (*url && url[strlen(url) - 1] == ' ')
			url[strlen(url) - 1] = '\0';
		if (!name[0] || !url[0] || n + strlen(name) + strlen(url) + 4 > (1 << 16))
			continue;
		n += (size_t)snprintf(out + n, (1 << 16) - n, "%s\t%s\n", name, url);
	}
	if (f)
		fclose(f);
	respond(fd, 200, "text/plain; charset=utf-8", out, n);
	free(out);
}

/* POST: the whole list as "name<TAB>url" lines replaces the owner's stations; the header and the managed block stay */
static void favourites_put(int fd, const char *body, size_t len)
{
	FILE *f = fopen(STATIONS_FILE, "r");
	char line[1200], tmp[200];
	char *head = malloc(1 << 15), *mgd = malloc(1 << 15);
	size_t hn = 0, mn = 0;
	int managed = 0, count = 0;
	FILE *o;
	const char *p = body, *end = body + len;

	if (!head || !mgd) {
		free(head);
		free(mgd);
		if (f)
			fclose(f);
		respond_text(fd, 500, "out of memory");
		return;
	}
	while (f && fgets(line, sizeof line, f)) {
		size_t l = strlen(line);

		if (strstr(line, "fritzbox begin"))
			managed = 1;
		if (managed) {
			if (mn + l < (1 << 15))
				memcpy(mgd + mn, line, l), mn += l;
		} else if (line[0] == '#' || line[strspn(line, " \t\r\n")] == '\0') {
			if (hn + l < (1 << 15))
				memcpy(head + hn, line, l), hn += l;
		}
		if (strstr(line, "fritzbox end"))
			managed = 0;
	}
	if (f)
		fclose(f);
	snprintf(tmp, sizeof tmp, "%s.web", STATIONS_FILE);
	o = fopen(tmp, "w");
	if (!o) {
		free(head);
		free(mgd);
		respond_text(fd, 503, "cannot write the station list");
		return;
	}
	fwrite(head, 1, hn, o);
	while (p < end && count < 300) {
		const char *nl = memchr(p, '\n', (size_t)(end - p));
		size_t ll = nl ? (size_t)(nl - p) : (size_t)(end - p);
		char name[200], url[500];
		const char *tab = memchr(p, '\t', ll);
		size_t i;

		/* an address that does not fit is dropped, not cut: a shortened one would be another station's */
		if (tab && ll < 900 && ll - (size_t)(tab - p) - 1 <= 480) {
			snprintf(name, sizeof name, "%.*s", (int)(tab - p) > 150 ? 150 : (int)(tab - p), p);
			snprintf(url, sizeof url, "%.*s", (int)(ll - (size_t)(tab - p) - 1) > 480 ? 480 : (int)(ll - (size_t)(tab - p) - 1), tab + 1);
			for (i = 0; name[i]; i++)
				if ((unsigned char)name[i] < 32 || name[i] == '|')
					name[i] = ' ';
			for (i = 0; url[i]; i++)
				if ((unsigned char)url[i] <= 32 || url[i] == '"' || url[i] == '\\')
					break;
			if (!url[i] && (!strncmp(url, "http://", 7) || !strncmp(url, "https://", 8)) && name[0]) {
				fprintf(o, "%s | %s\n", name, url);
				count++;
			}
		}
		p += ll + (nl ? 1 : 0);
	}
	fwrite(mgd, 1, mn, o);
	if (fclose(o) != 0 || rename(tmp, STATIONS_FILE) != 0) {
		unlink(tmp);
		free(head);
		free(mgd);
		respond_text(fd, 503, "cannot write the station list");
		return;
	}
	free(head);
	free(mgd);
	respond_text(fd, 200, "ok");
}

/* the header value at p is exactly s (and not merely something that starts with it) */
static int hdr_is(const char *p, const char *s)
{
	size_t n = strlen(s);

	return !strncmp(p, s, n) && (p[n] == '\r' || p[n] == '\n' || p[n] == ' ' || p[n] == '\0');
}

static int authorized(const char *req, const char *hdr_end)
{
	char tok[128], want[160];
	const char *p;

	if (!conf_get("web_token", tok, sizeof tok) || !tok[0])
		return 1;
	snprintf(want, sizeof want, "Bearer %s", tok);
	p = strcasestr(req, "\r\nauthorization:");
	if (p && p < hdr_end) {
		p += 16;
		while (*p == ' ')
			p++;
		if (hdr_is(p, want))
			return 1;
	}
	p = strcasestr(req, "\r\nx-token:");
	if (p && p < hdr_end) {
		p += 10;
		while (*p == ' ')
			p++;
		if (hdr_is(p, tok))
			return 1;
	}
	return 0;
}

/* The file manager is open to the same people as the rest, but its GETs show the storage too: with a web_token
 * set they need it as well. A link or a picture cannot carry a header, so "tok=" in the query does for them. */
static int files_authorized(const char *req, const char *hdr_end, const char *query)
{
	char tok[128], got[160];
	const char *t = query;
	size_t n = 0;

	if (!conf_get("web_token", tok, sizeof tok) || !tok[0])
		return 1;
	if (authorized(req, hdr_end))
		return 1;
	while ((t = strstr(t, "tok=")) && t != query && t[-1] != '&')
		t += 4;
	if (!t)
		return 0;
	for (t += 4; *t && *t != '&' && n < sizeof got - 1; t++) {
		if (t[0] == '%' && isxdigit((unsigned char)t[1]) && isxdigit((unsigned char)t[2])) {
			char h[3] = { t[1], t[2], 0 };

			got[n++] = (char)strtol(h, NULL, 16);
			t += 2;
		} else {
			got[n++] = *t;
		}
	}
	got[n] = '\0';
	return !strcmp(got, tok);
}

#define REQ_MAX (64 * 1024)   /* headers and body of one request */

static void serve(int fd)
{
	static char req[REQ_MAX + 1];   /* one request per process: static keeps it off the stack */
	size_t have = 0;
	char method[8], path[2048 + 8];
	char *hdr_end = NULL, *p, *query = "";
	size_t clen = 0, body_have;
	long long clen_hdr = -1;
	int full_res = 0;
	struct timeval tv = { 5, 0 };

	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
	while (have < sizeof req - 1) {
		ssize_t r = read(fd, req + have, sizeof req - 1 - have);

		if (r <= 0)
			return;
		have += (size_t)r;
		req[have] = '\0';
		hdr_end = strstr(req, "\r\n\r\n");
		if (hdr_end)
			break;
	}
	if (!hdr_end || sscanf(req, "%7s %2047s", method, path) != 2) {
		respond_text(fd, 400, "bad request");
		return;
	}
	p = strcasestr(req, "\r\ncontent-length:");
	if (p && p < hdr_end) {
		clen_hdr = atoll(p + 17);
		clen = (size_t)atol(p + 17);
	}
	p = strchr(path, '?');
	if (p) {
		query = p + 1;
		full_res = strstr(p, "full=1") != NULL;
		*p = '\0';
	}
	if (!strncmp(path, "/api/v1/", 8) || !strcmp(path, "/api/v1"))
		memmove(path + 4, path + 7, strlen(path + 7) + 1);   /* /api/v1/x -> /api/x */
	if (!strcmp(method, "OPTIONS")) {
		respond(fd, 204, "text/plain", "", 0);
		return;
	}
	/* the file manager reads an upload from the socket itself: it is handled before the request is buffered whole */
	if (!strncmp(path, "/api/files/", 11)) {
		struct web_req rq;
		struct timeval slow = { 30, 0 };

		if (!files_authorized(req, hdr_end, query)) {
			respond_text(fd, 401, "token required");
			return;
		}
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &slow, sizeof slow);
		setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &slow, sizeof slow);
		rq.body = hdr_end + 4;
		rq.body_have = have - (size_t)(hdr_end + 4 - req);
		rq.clen = clen_hdr;
		if (clen_hdr >= 0 && (long long)rq.body_have > clen_hdr)
			rq.body_have = (size_t)clen_hdr;
		hdr_end[2] = '\0';   /* the headers end here; the body starts after the blank line */
		rq.hdrs = req;
		api_files(fd, method, path, query, &rq);
		return;
	}
	body_have = have - (size_t)(hdr_end + 4 - req);
	/* a body that does not fit is refused: cutting it would have the station editor save a shortened list */
	if (clen > REQ_MAX - (size_t)(hdr_end + 4 - req) - 1) {
		respond_text(fd, 413, "request too large");
		return;
	}
	while (body_have < clen && have < sizeof req - 1) {
		ssize_t r = read(fd, req + have, sizeof req - 1 - have);

		if (r <= 0)
			break;
		have += (size_t)r;
		body_have += (size_t)r;
	}
	req[have] = '\0';
	hdr_end = strstr(req, "\r\n\r\n");
	if (!hdr_end)
		return;
	if (body_have < clen) {   /* the sender stalled or left: half a command or a half station list is worse than none */
		respond_text(fd, 400, "incomplete request");
		return;
	}
	if (!strcmp(method, "POST") && !authorized(req, hdr_end)) {
		respond_text(fd, 401, "token required");
		return;
	}

	/* the endpoint modules (api_*.c), each owning one prefix */
	if (api_wifi(fd, method, path, hdr_end + 4, body_have < clen ? body_have : clen))
		return;
	if (!strcmp(method, "POST") && !strcmp(path, "/api/cmd")) {
		post_cmd(fd, hdr_end + 4, body_have < clen ? body_have : clen);
	} else if (!strcmp(method, "POST") && !strcmp(path, "/api/favourites")) {
		favourites_put(fd, hdr_end + 4, body_have < clen ? body_have : clen);
	} else if (!strcmp(method, "POST") && !strcmp(path, "/api/library/delete")) {
		library_delete(fd, hdr_end + 4, body_have < clen ? body_have : clen);
	} else if (strcmp(method, "GET") != 0 && strcmp(method, "HEAD") != 0) {
		respond_text(fd, 400, "method not supported");
	} else if (!strcmp(path, "/api/state")) {
		size_t n;
		char *b = slurp(STATE_FILE, &n, 1 << 20);

		if (!b) {
			respond_text(fd, 503, "the UI is not running");
			return;
		}
		respond(fd, 200, "application/json; charset=utf-8", b, n);
		free(b);
	} else if (!strcmp(path, "/api/favourites")) {
		favourites_get(fd);
	} else if (!strcmp(path, "/api/stream.mjpg")) {
		send_stream(fd, full_res);
	} else if (!strcmp(path, "/api/meter")) {
		send_meter(fd);
	} else if (!strcmp(path, "/api/system")) {
		send_system(fd);
	} else if (!strcmp(path, "/api/screen.bmp")) {
		send_screen(fd);
	} else if (!strcmp(path, "/api/slide")) {
		size_t n;
		char *b = slurp(SLIDE_FILE, &n, 4 << 20);

		if (!b) {
			respond_text(fd, 404, "no picture");
			return;
		}
		respond(fd, 200, n > 3 && (unsigned char)b[0] == 0x89 ? "image/png" : "image/jpeg", b, n);
		free(b);
	} else {
		char file[2400];
		size_t n;
		char *b;

		if (strstr(path, "..") || path[0] != '/') {
			respond_text(fd, 404, "not found");
			return;
		}
		snprintf(file, sizeof file, "%s%s%s", webdir, path, path[strlen(path) - 1] == '/' ? "index.html" : "");
		b = slurp(file, &n, 4 << 20);
		if (!b) {
			respond_text(fd, 404, "not found");
			return;
		}
		respond(fd, 200, mime(file), b, n);
		free(b);
	}
}

#define MAX_KIDS 40   /* connections served at once (a page opens about 6; streams and uploads stay for long) */

int main(int argc, char **argv)
{
	int port = 80, ls, one = 1, i, kids = 0;
	struct sockaddr_in a;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-p") && i + 1 < argc)
			port = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-d") && i + 1 < argc)
			webdir = argv[++i];
	}
	signal(SIGPIPE, SIG_IGN);
	ls = socket(AF_INET, SOCK_STREAM, 0);
	if (ls < 0) {
		perror("socket");
		return 1;
	}
	setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	memset(&a, 0, sizeof a);
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_ANY);
	a.sin_port = htons((uint16_t)port);
	if (bind(ls, (struct sockaddr *)&a, sizeof a) < 0 || listen(ls, 16) < 0) {
		perror("bind/listen");
		return 1;
	}
	for (;;) {
		int c = accept(ls, NULL, NULL);
		pid_t pid;

		if (c < 0) {
			if (errno == EINTR)
				continue;
			usleep(100000);
			continue;
		}
		/* one process per connection; the finished ones are collected here, and a flood of connections gets a plain
		 * refusal instead of using up the memory (a stalled sender is dropped after 5 s) */
		while (waitpid(-1, NULL, WNOHANG) > 0)
			kids--;
		if (kids >= MAX_KIDS) {
			respond_text(c, 503, "busy");
			close(c);
			continue;
		}
		pid = fork();
		if (pid == 0) {
			close(ls);
			serve(c);
			close(c);
			_exit(0);
		}
		if (pid > 0)
			kids++;
		close(c);
	}
}
