#define _GNU_SOURCE
/* x45art - finds the picture for what is playing, for the panel and the web page.
 *
 *   x45art track   OUT URI ARTIST ALBUM   a file of the library: the picture MPD has for it (embedded in the tags, or
 *                                          cover.jpg and the like in its folder); failing that, with an artist and an
 *                                          album, the album cover from the iTunes catalogue (only if both match)
 *   x45art station OUT URL NAME           a radio station's logo: radio-browser.info by stream address, then by name
 *   x45art song    OUT ARTIST TITLE       the cover of the song a radio stream announces (iTunes catalogue, the
 *                                          song must match the artist and the title)
 *
 * The picture is written to OUT as a BMP of at most 300 pixels on its longer side (what was found is checked to decode
 * as a picture and not to be a speck). Exit 0:
 * written. Exit 1: nothing found (the answer is final: the caller remembers it). Exit 2: usage or a failure that may
 * go away (no network): the caller may ask again later.
 *
 * Nothing runs through a shell: every address and name reaches curl as an argument of its own. The unit's curl is
 * used for https (the certificate bundle of the replacement software, as x45rb does). Old pictures beside OUT are
 * pruned so that /tmp does not fill up. Names that are looked up (artist, title, station) leave the unit only
 * through these requests. The address of a stream is sent to the directory only when it holds no login, no key in its
 * query and no host of the unit's own network; a picture address of the directory is not fetched when it points at
 * such a host, and a redirect of it may only go to https.
 */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#pragma GCC diagnostic ignored "-Wcast-function-type"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_MAX_DIMENSIONS 4096
#include "../libx45/stb_image.h"
#pragma GCC diagnostic pop

#define CA "/home/root/x45new/etc/ssl/cacert.pem"
#define MAXIMG (4 << 20)        /* what the panel's loader accepts */
#define KEEP 24                 /* pictures kept beside OUT (a small BMP each: /tmp is memory) */

static const char *const rb_servers[] = { "de1", "de2", "nl1", "at1" };

/* ------------------------------------------------------------------ files --- */

static void slurp_free(char *p) { free(p); }

/* the whole of a file, or NULL; at most cap bytes (more: NULL) */
static char *slurp(const char *path, size_t *len, size_t cap)
{
	FILE *f = fopen(path, "rb");
	char *b;
	long n;

	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (n <= 0 || (size_t)n > cap || !(b = malloc((size_t)n + 1))) {
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

/* is `path` a picture the panel can show, and big enough to be worth showing */
static int picture_ok(const char *path, int min_px)
{
	size_t n;
	char *b = slurp(path, &n, MAXIMG);
	int w = 0, h = 0, comp = 0, ok;

	if (!b)
		return 0;
	ok = stbi_info_from_memory((const unsigned char *)b, (int)n, &w, &h, &comp) && w >= min_px && h >= min_px &&
	     w <= 4096 && h <= 4096 && w * 3 >= h && h * 3 >= w;   /* not a banner or a strip */
	slurp_free(b);
	return ok;
}

/* The picture at `path` made smaller than OUTMAX on its longer side (never enlarged), as a 24 bit BMP written over the
 * same file. A cover can be 3000 pixels wide: the panel and the web page get a few hundred, so that showing one
 * costs neither time nor memory there. 0 when done, -1 when the file is not a picture after all. */
#define OUTMAX 300

static int shrink_to_bmp(const char *path)
{
	size_t n;
	char *b = slurp(path, &n, MAXIMG);
	unsigned char *px, *row;
	int sw, sh, comp, dw, dh, x, y, pad;
	uint32_t fsz;
	char tmp[600];
	FILE *f;
	unsigned char hdr[54];

	if (!b)
		return -1;
	px = stbi_load_from_memory((const unsigned char *)b, (int)n, &sw, &sh, &comp, 4);
	slurp_free(b);
	if (!px)
		return -1;
	dw = sw;
	dh = sh;
	if (dw > OUTMAX || dh > OUTMAX) {
		if (dw >= dh) {
			dh = (int)((long)dh * OUTMAX / dw);
			dw = OUTMAX;
		} else {
			dw = (int)((long)dw * OUTMAX / dh);
			dh = OUTMAX;
		}
	}
	if (dw < 1)
		dw = 1;
	if (dh < 1)
		dh = 1;
	pad = (4 - (dw * 3) % 4) % 4;
	row = malloc((size_t)dw * 3 + 4);
	if (!row) {
		stbi_image_free(px);
		return -1;
	}
	snprintf(tmp, sizeof tmp, "%s.%ld.bmp", path, (long)getpid());
	f = fopen(tmp, "wb");
	if (!f) {
		free(row);
		stbi_image_free(px);
		return -1;
	}
	fsz = 54u + (uint32_t)(dw * 3 + pad) * (uint32_t)dh;
	memset(hdr, 0, sizeof hdr);
	hdr[0] = 'B';
	hdr[1] = 'M';
	hdr[2] = (unsigned char)fsz;
	hdr[3] = (unsigned char)(fsz >> 8);
	hdr[4] = (unsigned char)(fsz >> 16);
	hdr[5] = (unsigned char)(fsz >> 24);
	hdr[10] = 54;
	hdr[14] = 40;
	hdr[18] = (unsigned char)dw;
	hdr[19] = (unsigned char)(dw >> 8);
	hdr[22] = (unsigned char)dh;
	hdr[23] = (unsigned char)(dh >> 8);
	hdr[26] = 1;
	hdr[28] = 24;
	fwrite(hdr, 1, sizeof hdr, f);
	for (y = dh - 1; y >= 0; y--) {   /* a BMP is written from the bottom row up */
		int y0 = (int)((long)y * sh / dh), y1 = (int)((long)(y + 1) * sh / dh);

		if (y1 <= y0)
			y1 = y0 + 1;
		for (x = 0; x < dw; x++) {
			int x0 = (int)((long)x * sw / dw), x1 = (int)((long)(x + 1) * sw / dw);
			unsigned long r = 0, g = 0, bl = 0, cnt = 0;
			int xx, yy;

			if (x1 <= x0)
				x1 = x0 + 1;
			for (yy = y0; yy < y1 && yy < sh; yy++)
				for (xx = x0; xx < x1 && xx < sw; xx++) {
					const unsigned char *q = px + ((size_t)yy * sw + xx) * 4;
					int a = q[3];

					/* see-through parts (a logo) are laid over a light ground */
					r += (unsigned long)(q[0] * a + 240 * (255 - a)) / 255;
					g += (unsigned long)(q[1] * a + 240 * (255 - a)) / 255;
					bl += (unsigned long)(q[2] * a + 240 * (255 - a)) / 255;
					cnt++;
				}
			if (!cnt)
				cnt = 1;
			row[x * 3] = (unsigned char)(bl / cnt);
			row[x * 3 + 1] = (unsigned char)(g / cnt);
			row[x * 3 + 2] = (unsigned char)(r / cnt);
		}
		memset(row + dw * 3, 0, (size_t)pad);
		fwrite(row, 1, (size_t)(dw * 3 + pad), f);
	}
	free(row);
	stbi_image_free(px);
	if (fclose(f) != 0 || rename(tmp, path) < 0) {
		unlink(tmp);
		return -1;
	}
	return 0;
}

/* ------------------------------------------------------------------ fetch --- */

static int g_https_redirects;   /* picture addresses come from a public directory: a redirect may only go to https */

static int run_curl(const char *url, const char *out_file, char **mem, size_t *memlen, size_t memcap)
{
	int pfd[2] = { -1, -1 };
	pid_t pid;
	int status;
	char *buf = NULL;
	size_t n = 0;

	if (mem && pipe(pfd) < 0)
		return -1;
	pid = fork();
	if (pid < 0) {
		if (mem) {
			close(pfd[0]);
			close(pfd[1]);
		}
		return -1;
	}
	if (pid == 0) {
		const char *argv[32];
		int k = 0, nul = open("/dev/null", O_RDWR);

		if (nul >= 0) {
			dup2(nul, 0);
			dup2(nul, 2);
		}
		if (mem) {
			dup2(pfd[1], 1);
			close(pfd[0]);
			close(pfd[1]);
		} else if (nul >= 0) {
			dup2(nul, 1);
		}
		if (out_file) {   /* --max-filesize does not stop a body of unknown length: the disk limit does */
			struct rlimit rl = { MAXIMG + 65536, MAXIMG + 65536 };

			setrlimit(RLIMIT_FSIZE, &rl);
		}
		argv[k++] = "curl";
		argv[k++] = "-s";
		argv[k++] = "-g";   /* no [1-999999999] or {a,b} expansion of an address into thousands of requests */
		argv[k++] = "-f";
		argv[k++] = "-L";
		argv[k++] = "--max-redirs";
		argv[k++] = "3";
		argv[k++] = "--proto";
		argv[k++] = "=http,https";
		argv[k++] = "--proto-redir";
		argv[k++] = g_https_redirects ? "=https" : "=http,https";
		argv[k++] = "--connect-timeout";
		argv[k++] = "6";
		argv[k++] = "-m";
		argv[k++] = "14";
		argv[k++] = "--max-filesize";
		argv[k++] = "4194304";
		argv[k++] = "--cacert";
		argv[k++] = CA;
		argv[k++] = "-A";
		argv[k++] = "x45/1.0";
		if (out_file) {
			argv[k++] = "-o";
			argv[k++] = out_file;
		}
		argv[k++] = "--";
		argv[k++] = url;
		argv[k] = NULL;
		execvp("curl", (char *const *)argv);
		_exit(127);
	}
	if (mem) {
		size_t cap = 1 << 16;

		close(pfd[1]);
		buf = malloc(cap + 1);
		if (!buf) {
			close(pfd[0]);
			kill(pid, SIGKILL);
			waitpid(pid, NULL, 0);
			return -1;
		}
		for (;;) {
			ssize_t r;

			if (n + 8192 > cap && cap < memcap) {
				char *nb;

				cap *= 2;
				if (cap > memcap)
					cap = memcap;
				nb = realloc(buf, cap + 1);
				if (!nb)
					break;
				buf = nb;
			}
			if (n >= cap)
				break;
			r = read(pfd[0], buf + n, cap - n > 8192 ? 8192 : cap - n);
			if (r <= 0)
				break;
			n += (size_t)r;
		}
		close(pfd[0]);
	}
	if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		free(buf);
		return -1;
	}
	if (mem) {
		buf[n] = '\0';
		*mem = buf;
		*memlen = n;
	}
	return 0;
}

static int fetch_mem(const char *url, char **mem, size_t *len, size_t cap)
{
	return run_curl(url, NULL, mem, len, cap);
}

/* An address with a name and a password in it, or a host of the unit's own network (a loopback, private or link-local
 * number, a name without a dot, .local, .lan): nothing the directory can know, and nothing to fetch for it. */
static int url_private(const char *u)
{
	const char *h = strstr(u, "://"), *e;
	char host[260];
	size_t n;
	unsigned a, b, c, d;

	if (!h)
		return 1;
	h += 3;
	e = h + strcspn(h, "/?#");
	if (memchr(h, '@', (size_t)(e - h)) || *h == '[')
		return 1;
	n = (size_t)strcspn(h, "/?#:");
	if (n == 0 || n >= sizeof host)
		return 1;
	memcpy(host, h, n);
	host[n] = '\0';
	if (!strchr(host, '.') || !strcasecmp(host, "localhost"))
		return 1;
	n = strlen(host);
	if ((n > 6 && !strcasecmp(host + n - 6, ".local")) || (n > 4 && !strcasecmp(host + n - 4, ".lan")) ||
	    (n > 4 && !strcasecmp(host + n - 4, ".home")) || (n > 9 && !strcasecmp(host + n - 9, ".fritz.box")))
		return 1;
	if (sscanf(host, "%u.%u.%u.%u", &a, &b, &c, &d) == 4)
		return a == 0 || a == 10 || a == 127 || (a == 169 && b == 254) || (a == 172 && b >= 16 && b <= 31) ||
		       (a == 192 && b == 168);
	return 0;
}

/* a key or a login in the query of a stream address: it does not go to a directory */
static int url_secret(const char *u)
{
	const char *q = strchr(u, '?');
	static const char *const words[] = { "token", "key=", "pass", "auth", "sig", "secret", "session", "login", "user=" };
	unsigned i;

	if (!q)
		return 0;
	for (i = 0; i < sizeof words / sizeof *words; i++)
		if (strcasestr(q, words[i]))
			return 1;
	return 0;
}

/* a picture from an address into OUT (through a temporary file), when it is one */
static int fetch_picture(const char *url, const char *out, int min_px)
{
	char tmp[600];
	int rc;

	if (strncmp(url, "http://", 7) && strncmp(url, "https://", 8))
		return 0;
	if (strpbrk(url, "[]{}") || url_private(url))   /* a station entry must not point the unit at its own network */
		return 0;
	snprintf(tmp, sizeof tmp, "%s.%ld.part", out, (long)getpid());
	g_https_redirects = 1;
	rc = run_curl(url, tmp, NULL, NULL, 0);
	g_https_redirects = 0;
	if (rc < 0 || !picture_ok(tmp, min_px)) {
		unlink(tmp);
		return 0;
	}
	if (rename(tmp, out) < 0) {
		unlink(tmp);
		return 0;
	}
	return 1;
}

/* percent-encodes everything that is not unreserved; false if it does not fit */
static int urlenc(const char *s, char *out, size_t cap)
{
	size_t n = 0;

	for (; *s; s++) {
		unsigned char c = (unsigned char)*s;

		if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
			if (n + 2 > cap)
				return 0;
			out[n++] = (char)c;
		} else {
			if (n + 4 > cap)
				return 0;
			n += (size_t)snprintf(out + n, cap - n, "%%%02X", c);
		}
	}
	out[n] = '\0';
	return 1;
}

/* ------------------------------------------------------------------- json --- */
/* Just enough for flat objects: string values by key, unescaped to UTF-8. */

static void put8(char **o, char *end, unsigned c)
{
	char *d = *o;

	if (c < 0x80 && d + 1 < end) {
		*d++ = (char)c;
	} else if (c >= 0x80 && c < 0x800 && d + 2 < end) {
		*d++ = (char)(0xc0 | c >> 6);
		*d++ = (char)(0x80 | (c & 0x3f));
	} else if (c >= 0x800 && c < 0x10000 && d + 3 < end) {
		*d++ = (char)(0xe0 | c >> 12);
		*d++ = (char)(0x80 | ((c >> 6) & 0x3f));
		*d++ = (char)(0x80 | (c & 0x3f));
	} else if (c >= 0x10000 && d + 4 < end) {
		*d++ = (char)(0xf0 | c >> 18);
		*d++ = (char)(0x80 | ((c >> 12) & 0x3f));
		*d++ = (char)(0x80 | ((c >> 6) & 0x3f));
		*d++ = (char)(0x80 | (c & 0x3f));
	}
	*o = d;
}

static int hex4(const char *p, unsigned *v)
{
	unsigned r = 0;
	int i;

	for (i = 0; i < 4; i++) {
		char c = p[i];

		if (c >= '0' && c <= '9')
			r = r * 16 + (unsigned)(c - '0');
		else if (c >= 'a' && c <= 'f')
			r = r * 16 + (unsigned)(c - 'a' + 10);
		else if (c >= 'A' && c <= 'F')
			r = r * 16 + (unsigned)(c - 'A' + 10);
		else
			return 0;
	}
	*v = r;
	return 1;
}

/* p is just after the opening quote of a string; copies it unescaped */
static void read_str(const char *p, char *out, size_t cap)
{
	char *o = out, *end = out + cap - 1;

	while (*p && *p != '"') {
		if (*p == '\\' && p[1]) {
			p++;
			switch (*p) {
			case 'n': case 't': case 'r': put8(&o, end, ' '); p++; break;
			case 'u': {
				unsigned v, lo;

				if (hex4(p + 1, &v)) {
					p += 5;
					if (v >= 0xd800 && v < 0xdc00 && p[0] == '\\' && p[1] == 'u' && hex4(p + 2, &lo) &&
					    lo >= 0xdc00 && lo < 0xe000) {
						v = 0x10000 + ((v - 0xd800) << 10) + (lo - 0xdc00);
						p += 6;
					}
					put8(&o, end, v);
				} else {
					p++;
				}
				break;
			}
			default:
				put8(&o, end, (unsigned char)*p);   /* \" \\ \/ */
				p++;
			}
		} else {
			if (o < end)
				*o++ = *p;
			p++;
		}
	}
	*o = '\0';
}

/* the string value of "key" in obj (up to len bytes); 1 when there is one */
static int jstr(const char *obj, const char *objend, const char *key, char *out, size_t cap)
{
	char pat[48];
	const char *p = obj;
	size_t kl;

	out[0] = '\0';
	kl = (size_t)snprintf(pat, sizeof pat, "\"%s\"", key);
	if (kl >= sizeof pat)
		return 0;
	while ((p = strstr(p, pat)) != NULL && p < objend) {
		const char *q = p + kl;

		while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r')
			q++;
		if (*q == ':') {
			q++;
			while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r')
				q++;
			if (*q == '"') {
				read_str(q + 1, out, cap);
				return out[0] != '\0';
			}
		}
		p += kl;
	}
	return 0;
}

/* the next object that starts with `marker` at or after p; sets *end to where it ends (the next one or the end) */
static const char *next_obj(const char *p, const char *marker, const char **end)
{
	const char *s = strstr(p, marker), *e;

	if (!s)
		return NULL;
	e = strstr(s + strlen(marker), marker);
	*end = e ? e : s + strlen(s);
	return s;
}

/* ------------------------------------------------------------------ names --- */

/* lower case letters and digits, one space between words, what is in () or [] left out */
static void norm(const char *s, char *out, size_t cap)
{
	size_t n = 0;
	int depth = 0, sp = 1;

	for (; *s && n + 2 < cap; s++) {
		unsigned char c = (unsigned char)*s;

		if (c == '(' || c == '[') {
			depth++;
			continue;
		}
		if (c == ')' || c == ']') {
			if (depth > 0)
				depth--;
			continue;
		}
		if (depth)
			continue;
		if (isalnum(c) || c >= 0x80) {
			out[n++] = (char)tolower(c);
			sp = 0;
		} else if (!sp) {
			out[n++] = ' ';
			sp = 1;
		}
	}
	while (n > 0 && out[n - 1] == ' ')
		n--;
	out[n] = '\0';
}

/* the same name: equal, or one holds the other and is at least 70% of its length */
static int same_name(const char *a, const char *b)
{
	char na[300], nb[300];
	size_t la, lb;

	norm(a, na, sizeof na);
	norm(b, nb, sizeof nb);
	la = strlen(na);
	lb = strlen(nb);
	if (!la || !lb)
		return 0;
	if (!strcmp(na, nb))
		return 1;
	if (la >= lb && lb * 10 >= la * 7 && strstr(na, nb))
		return 1;
	if (lb > la && la * 10 >= lb * 7 && strstr(nb, na))
		return 1;
	return 0;
}

/* the artist that was asked for is among the ones the catalogue lists ("A, B & C") */
static int artist_in(const char *want, const char *listed)
{
	char nw[300], nl[300];

	norm(want, nw, sizeof nw);
	norm(listed, nl, sizeof nl);
	return nw[0] && nl[0] && (strstr(nl, nw) || strstr(nw, nl));
}

/* ----------------------------------------------------------------- itunes --- */

/* iTunes catalogue search; `entity` is "song" or "album". A result whose artist and name match is fetched at 300 px. */
static int itunes(const char *out, const char *entity, const char *artist, const char *name)
{
	char term[400], enc[1300], url[1500], *body = NULL;
	size_t len, tl;
	const char *o, *end, *p;
	int found = 0, fetched_any = 0;

	tl = (size_t)snprintf(term, sizeof term, "%s %s", artist, name);
	if (tl >= sizeof term || !urlenc(term, enc, sizeof enc))
		return 0;
	snprintf(url, sizeof url, "https://itunes.apple.com/search?term=%s&media=music&entity=%s&limit=10", enc, entity);
	if (fetch_mem(url, &body, &len, 1 << 20) < 0)
		return -1;   /* no network, or the service is down: not an answer */
	fetched_any = 1;
	p = body;
	while (!found && (o = next_obj(p, "{\"wrapperType\"", &end)) != NULL) {
		char ra[300], rn[300], art[600];
		int is_song = !strcmp(entity, "song");

		p = end;
		if (!jstr(o, end, "artistName", ra, sizeof ra) ||
		    !jstr(o, end, is_song ? "trackName" : "collectionName", rn, sizeof rn) ||
		    !jstr(o, end, "artworkUrl100", art, sizeof art))
			continue;
		if (!artist_in(artist, ra) || !same_name(name, rn))
			continue;
		{
			char *sz = strstr(art, "100x100bb");

			if (sz)
				memcpy(sz, "300x300bb", 9);   /* the catalogue serves any size under the same path */
		}
		if (fetch_picture(art, out, 100))
			found = 1;
	}
	free(body);
	return fetched_any ? found : -1;
}

/* ----------------------------------------------------------------- station --- */

/* tries the favicons of the stations in a radio-browser answer; `name` (may be NULL) must match theirs */
static int rb_logos(const char *json, const char *name, const char *out)
{
	const char *o, *end, *p = json;
	int tries = 0;

	while (tries < 6 && (o = next_obj(p, "{\"changeuuid\"", &end)) != NULL) {
		char rn[300], fav[600];

		p = end;
		if (!jstr(o, end, "favicon", fav, sizeof fav))
			continue;
		if (name && (!jstr(o, end, "name", rn, sizeof rn) || !same_name(name, rn)))
			continue;
		tries++;
		if (fetch_picture(fav, out, 48))
			return 1;
	}
	return 0;
}

/* GET on the directory, trying its servers in turn; 0 and the body, or -1 when none answered */
static int rb_get(const char *path_query, char **body, size_t *len)
{
	unsigned s;
	char url[1300];

	for (s = 0; s < sizeof rb_servers / sizeof *rb_servers; s++) {
		snprintf(url, sizeof url, "https://%s.api.radio-browser.info%s", rb_servers[s], path_query);
		if (fetch_mem(url, body, len, 1 << 20) == 0) {
			if (*len > 1)
				return 0;
			free(*body);   /* an empty answer: the next server may know better */
			*body = NULL;
		}
	}
	return -1;
}

static int station(const char *out, const char *stream, const char *name)
{
	char enc[1100], pq[1200], *body = NULL;
	size_t len;
	int found = 0, answered = 0;

	/* the address of a stream with a login, a key or a LAN host in it stays on the unit (the name search still runs) */
	if (stream[0] && !url_private(stream) && !url_secret(stream) && urlenc(stream, enc, sizeof enc)) {
		snprintf(pq, sizeof pq, "/json/stations/byurl?url=%s", enc);
		if (rb_get(pq, &body, &len) == 0) {
			answered = 1;
			found = rb_logos(body, NULL, out);
			free(body);
		}
	}
	if (!found && name[0] && urlenc(name, enc, sizeof enc)) {
		snprintf(pq, sizeof pq, "/json/stations/search?name=%s&limit=20&hidebroken=true&order=votes&reverse=true", enc);
		if (rb_get(pq, &body, &len) == 0) {
			answered = 1;
			found = rb_logos(body, name, out);
			free(body);
		}
	}
	return found ? 1 : answered ? 0 : -1;
}

/* -------------------------------------------------------------------- mpd --- */

struct rd {
	int fd;
	char buf[8192];
	size_t pos, len;
};

static int rd_fill(struct rd *r)
{
	ssize_t n = read(r->fd, r->buf, sizeof r->buf);

	if (n <= 0)
		return -1;
	r->pos = 0;
	r->len = (size_t)n;
	return 0;
}

/* one line without its newline; -1 on error or a line longer than cap */
static int rd_line(struct rd *r, char *out, size_t cap)
{
	size_t n = 0;

	for (;;) {
		if (r->pos >= r->len && rd_fill(r) < 0)
			return -1;
		while (r->pos < r->len) {
			char c = r->buf[r->pos++];

			if (c == '\n') {
				out[n] = '\0';
				return 0;
			}
			if (n + 1 >= cap)
				return -1;
			out[n++] = c;
		}
	}
}

static int rd_bytes(struct rd *r, FILE *f, size_t n)
{
	while (n > 0) {
		size_t take;

		if (r->pos >= r->len && rd_fill(r) < 0)
			return -1;
		take = r->len - r->pos;
		if (take > n)
			take = n;
		if (fwrite(r->buf + r->pos, 1, take, f) != take)
			return -1;
		r->pos += take;
		n -= take;
	}
	return 0;
}

static int mpd_connect(void)
{
	struct sockaddr_in a;
	struct timeval tv = { 4, 0 };
	const char *port = getenv("MPD_PORT");
	int fd = socket(AF_INET, SOCK_STREAM, 0);

	if (fd < 0)
		return -1;
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
	memset(&a, 0, sizeof a);
	a.sin_family = AF_INET;
	a.sin_port = htons((uint16_t)(port && atoi(port) > 0 ? atoi(port) : 6600));
	inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
	if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static int send_all(int fd, const char *s)
{
	size_t n = strlen(s);

	while (n > 0) {
		ssize_t w = write(fd, s, n);

		if (w <= 0)
			return -1;
		s += w;
		n -= (size_t)w;
	}
	return 0;
}

/* MPD's picture commands (readpicture: embedded in the tags; albumart: a file in the folder). 1 = written to
 * `tmp`, 0 = MPD has none, -1 = could not ask */
static int mpd_picture(struct rd *r, const char *cmd, const char *uri, const char *tmp)
{
	char q[1100], line[600];
	size_t qn = 0, off = 0, total = 0;
	FILE *f = NULL;
	int rounds, ok = 0;
	const char *p;

	for (p = uri; *p && qn + 3 < sizeof q; p++) {
		if (*p == '"' || *p == '\\')
			q[qn++] = '\\';
		q[qn++] = *p;
	}
	if (*p)
		return 0;   /* an address that long is not a file of the library */
	q[qn] = '\0';
	for (rounds = 0; rounds < 800; rounds++) {
		long chunk = -1;
		char req[1300];

		snprintf(req, sizeof req, "%s \"%s\" %zu\n", cmd, q, off);
		if (send_all(r->fd, req) < 0)
			goto fail;   /* could not ask: not the same as "MPD has none" */
		for (;;) {
			if (rd_line(r, line, sizeof line) < 0)
				goto fail;
			if (!strcmp(line, "OK"))
				break;
			if (!strncmp(line, "ACK", 3))
				goto none;
			if (!strncmp(line, "size: ", 6)) {
				total = (size_t)strtoul(line + 6, NULL, 10);
			} else if (!strncmp(line, "binary: ", 8)) {
				chunk = strtol(line + 8, NULL, 10);
				if (chunk < 0 || chunk > (1 << 20) || off + (size_t)chunk > MAXIMG || total > MAXIMG)
					goto fail;
				if (!f && !(f = fopen(tmp, "wb")))
					goto fail;
				if (chunk > 0 && rd_bytes(r, f, (size_t)chunk) < 0)
					goto fail;
				off += (size_t)chunk;
				/* the data is followed by a newline, then OK */
			}
		}
		if (chunk < 0)
			goto none;        /* an answer with no binary part: MPD has no picture for this file */
		if (chunk == 0 || off >= total) {
			ok = off > 0 && off >= total;
			goto done;
		}
	}
fail:
	if (f)
		fclose(f);
	unlink(tmp);
	return -1;
none:
	if (f)
		fclose(f);
	unlink(tmp);
	return 0;
done:
	if (f && fclose(f) != 0)
		ok = 0;
	if (!ok)
		unlink(tmp);
	return ok;
}

/* the picture MPD has for a library file, into `out` */
static int mpd_art(const char *out, const char *uri)
{
	struct rd r;
	char tmp[600], line[200];
	const char *pw = getenv("MPD_PASSWORD");
	int res = 0, i;
	static const char *const cmds[] = { "readpicture", "albumart" };

	memset(&r, 0, sizeof r);
	r.fd = mpd_connect();
	if (r.fd < 0)
		return -1;
	if (rd_line(&r, line, sizeof line) < 0 || strncmp(line, "OK MPD", 6)) {
		close(r.fd);
		return -1;
	}
	if (pw && *pw) {
		char req[400];

		snprintf(req, sizeof req, "password \"%s\"\n", pw);
		if (strchr(pw, '"') || strchr(pw, '\\') || send_all(r.fd, req) < 0 || rd_line(&r, line, sizeof line) < 0 ||
		    strcmp(line, "OK")) {
			close(r.fd);
			return -1;
		}
	}
	snprintf(tmp, sizeof tmp, "%s.%ld.part", out, (long)getpid());
	for (i = 0; i < 2 && res <= 0; i++) {
		res = mpd_picture(&r, cmds[i], uri, tmp);
		if (res > 0 && !picture_ok(tmp, 64)) {
			unlink(tmp);
			res = 0;
		}
		if (res < 0)
			break;
	}
	close(r.fd);
	if (res > 0 && rename(tmp, out) < 0) {
		unlink(tmp);
		res = 0;
	}
	return res;
}

/* ------------------------------------------------------------------ main --- */

/* keeps the newest KEEP entries beside `out`, deletes the rest */
static void prune(const char *out)
{
	char dir[400];
	const char *slash = strrchr(out, '/');
	DIR *d;
	struct dirent *e;
	struct ent { char name[96]; time_t t; } *list = NULL;
	size_t n = 0, cap = 0, i, j;

	if (!slash || (size_t)(slash - out) >= sizeof dir)
		return;
	memcpy(dir, out, (size_t)(slash - out));
	dir[slash - out] = '\0';
	if (!strstr(dir, "x45art"))
		return;   /* only ever tidies a folder made for these pictures, whatever OUT was given as */
	d = opendir(dir);
	if (!d)
		return;
	while ((e = readdir(d)) != NULL) {
		struct stat st;
		char path[520];

		if (e->d_name[0] == '.' || !strcmp(e->d_name, "cur") || strlen(e->d_name) >= sizeof list->name)
			continue;
		snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
		if (stat(path, &st) < 0 || !S_ISREG(st.st_mode))
			continue;
		if (n == cap) {
			struct ent *nl = realloc(list, (cap ? cap * 2 : 64) * sizeof *list);

			if (!nl)
				break;
			list = nl;
			cap = cap ? cap * 2 : 64;
		}
		snprintf(list[n].name, sizeof list[n].name, "%s", e->d_name);
		list[n++].t = st.st_mtime;
	}
	closedir(d);
	if (n > KEEP) {
		for (i = 0; i < n; i++)         /* oldest first */
			for (j = i + 1; j < n; j++)
				if (list[j].t < list[i].t) {
					struct ent t = list[i];

					list[i] = list[j];
					list[j] = t;
				}
		for (i = 0; i < n - KEEP; i++) {
			char path[520];

			snprintf(path, sizeof path, "%s/%s", dir, list[i].name);
			unlink(path);
		}
	}
	free(list);
}

int main(int argc, char **argv)
{
	const char *out;
	int r = -1;

	if (argc < 4) {
		fprintf(stderr, "usage: x45art track OUT URI ARTIST ALBUM | station OUT URL NAME | song OUT ARTIST TITLE\n");
		return 2;
	}
	signal(SIGPIPE, SIG_IGN);
	out = argv[2];
	if (strlen(out) > 400)
		return 2;
	prune(out);
	if (!strcmp(argv[1], "track") && argc >= 5) {
		const char *uri = argv[3], *artist = argv[4], *album = argc > 5 ? argv[5] : "";

		if (strncmp(uri, "http", 4))
			r = mpd_art(out, uri);
		else
			r = 0;
		if (r == 0 && artist[0] && album[0])
			r = itunes(out, "album", artist, album);
	} else if (!strcmp(argv[1], "station") && argc >= 5) {
		r = station(out, argv[3], argv[4]);
	} else if (!strcmp(argv[1], "song") && argc >= 5) {
		r = itunes(out, "song", argv[3], argv[4]);
	} else {
		return 2;
	}
	if (r > 0 && shrink_to_bmp(out) < 0) {
		unlink(out);
		r = 0;
	}
	return r > 0 ? 0 : r == 0 ? 1 : 2;
}
