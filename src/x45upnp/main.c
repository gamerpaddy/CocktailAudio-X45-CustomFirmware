/* x45upnp - finds UPnP/DLNA media servers and lists what they hold.
 *
 *   x45upnp discover [ms]            one line per server:  name<TAB>control URL
 *   x45upnp browse <control URL> <object id>
 *                                    one line per child:   D<TAB>id<TAB>title       (folder)
 *                                                          F<TAB>url<TAB>title      (audio file)
 *
 * Plain sockets, HTTP/1.1 with Connection: close, no dependencies. Used by x45ui for the
 * "network" folder of the library; the track URLs are played by MPD directly.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define MAXBODY (4 << 20)
#define MAXITEMS 256

/* a count of milliseconds on a clock that does not jump (a long is 32 bits here: only differences mean anything) */
static long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* "http://host:port/path" -> parts; port defaults to 80 */
static int split_url(const char *url, char *host, size_t hl, int *port, char *path, size_t pl)
{
	const char *p = url, *slash, *colon;
	size_t n;

	if (strncasecmp(p, "http://", 7))
		return -1;
	p += 7;
	slash = strchr(p, '/');
	n = slash ? (size_t)(slash - p) : strlen(p);
	colon = memchr(p, ':', n);
	if (colon) {
		*port = atoi(colon + 1);
		n = (size_t)(colon - p);
	} else {
		*port = 80;
	}
	if (n == 0 || n >= hl)
		return -1;
	memcpy(host, p, n);
	host[n] = '\0';
	snprintf(path, pl, "%s", slash ? slash : "/");
	return 0;
}

static int dial(const char *host, int port, int timeout_ms)
{
	struct addrinfo hints, *ai, *a;
	char ps[8];
	int fd = -1;

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	snprintf(ps, sizeof ps, "%d", port);
	if (getaddrinfo(host, ps, &hints, &ai))
		return -1;
	for (a = ai; a; a = a->ai_next) {
		fd_set w;
		struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
		int err = 0;
		socklen_t el = sizeof err;

		fd = socket(a->ai_family, a->ai_socktype, 0);
		if (fd < 0)
			continue;
		fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
		if (connect(fd, a->ai_addr, a->ai_addrlen) < 0 && errno == EINPROGRESS) {
			FD_ZERO(&w);
			FD_SET(fd, &w);
			if (select(fd + 1, NULL, &w, NULL, &tv) == 1 &&
			    getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) == 0 && !err) {
				fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
				break;
			}
		} else if (errno == 0) {
			fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
			break;
		}
		close(fd);
		fd = -1;
	}
	freeaddrinfo(ai);
	if (fd >= 0) {
		struct timeval rt = { 8, 0 };

		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rt, sizeof rt);
		setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &rt, sizeof rt);
	}
	return fd;
}

/* One request, whole response body (de-chunked) in a malloc'd NUL-terminated buffer. */
static char *http(const char *method, const char *url, const char *soapaction, const char *body)
{
	char host[128], path[512], hdr[1024];
	int port, fd, n, cap = 65536, len = 0;
	long t0;
	char *buf, *b, *he;

	if (split_url(url, host, sizeof host, &port, path, sizeof path) < 0)
		return NULL;
	fd = dial(host, port, 4000);
	if (fd < 0)
		return NULL;
	n = snprintf(hdr, sizeof hdr, "%s %s HTTP/1.1\r\nHost: %s:%d\r\nUser-Agent: x45upnp/1\r\nConnection: close\r\n",
		     method, path, host, port);
	if (body)
		n += snprintf(hdr + n, sizeof hdr - n,
			      "Content-Type: text/xml; charset=\"utf-8\"\r\nSOAPACTION: \"%s\"\r\nContent-Length: %zu\r\n",
			      soapaction, strlen(body));
	n += snprintf(hdr + n, sizeof hdr - n, "\r\n");
	if (write(fd, hdr, n) != n || (body && write(fd, body, strlen(body)) != (ssize_t)strlen(body))) {
		close(fd);
		return NULL;
	}
	buf = malloc(cap + 1);
	if (!buf) {
		close(fd);
		return NULL;
	}
	t0 = now_ms();
	while ((n = read(fd, buf + len, cap - len)) > 0) {
		len += n;
		if (now_ms() - t0 > 15000)   /* the 8 s receive limit is per read: a server that drips bytes must not hold us */
			break;
		if (len == cap) {
			char *nb;

			if (cap >= MAXBODY)
				break;
			cap *= 2;
			nb = realloc(buf, cap + 1);
			if (!nb)
				break;
			buf = nb;
		}
	}
	close(fd);
	buf[len] = '\0';
	he = strstr(buf, "\r\n\r\n");
	if (!he || strncmp(buf, "HTTP/1.", 7) || buf[9] != '2') {
		free(buf);
		return NULL;
	}
	b = he + 4;
	hdr[0] = '\0';
	if (strcasestr(buf, "transfer-encoding: chunked") && strcasestr(buf, "transfer-encoding: chunked") < he) {
		/* sized by what was received, not by strlen: a NUL inside the body would make that too small */
		char *out = malloc((size_t)(buf + len - b) + 1), *o = out, *p = b;

		if (!out) {
			free(buf);
			return NULL;
		}
		for (;;) {
			long cl = strtol(p, NULL, 16);
			char *nl = strstr(p, "\r\n");
			size_t left;

			if (!nl || cl <= 0)
				break;
			p = nl + 2;
			left = (size_t)(buf + len - p);   /* a chunk the server announced but did not send is cut short */
			if ((size_t)cl > left)
				cl = (long)left;
			memcpy(o, p, (size_t)cl);
			o += cl;
			p += cl;
			if (p + 2 > buf + len)
				break;
			p += 2;
		}
		*o = '\0';
		free(buf);
		return out;
	}
	memmove(buf, b, strlen(b) + 1);
	return buf;
}

/* ---- tiny XML helpers (the documents are small and regular) ---- */

/* s escaped for use as element text; truncated when it does not fit */
static void xml_escape(char *out, size_t cap, const char *s)
{
	size_t o = 0;

	for (; *s && o + 7 < cap; s++) {
		const char *r = *s == '&' ? "&amp;" : *s == '<' ? "&lt;" : *s == '>' ? "&gt;" : NULL;

		if (r) {
			memcpy(out + o, r, strlen(r));
			o += strlen(r);
		} else {
			out[o++] = *s;
		}
	}
	out[o] = '\0';
}

static void unescape(char *s)
{
	char *r = s, *w = s;

	while (*r) {
		if (*r == '&') {
			if (!strncmp(r, "&lt;", 4)) { *w++ = '<'; r += 4; continue; }
			if (!strncmp(r, "&gt;", 4)) { *w++ = '>'; r += 4; continue; }
			if (!strncmp(r, "&amp;", 5)) { *w++ = '&'; r += 5; continue; }
			if (!strncmp(r, "&quot;", 6)) { *w++ = '"'; r += 6; continue; }
			if (!strncmp(r, "&apos;", 6)) { *w++ = '\''; r += 6; continue; }
		}
		*w++ = *r++;
	}
	*w = '\0';
}

/* text between <tag ...> and </tag> inside [from, to); copied into out */
static int tag_text(const char *from, const char *to, const char *tag, char *out, size_t cap)
{
	char open[64], close_[64];
	const char *a, *b, *gt;
	size_t n;

	snprintf(open, sizeof open, "<%s", tag);
	snprintf(close_, sizeof close_, "</%s>", tag);
	for (a = from; (a = strstr(a, open)) && a < to; a++) {
		char c = a[strlen(open)];

		if (c == '>' || c == ' ')
			break;
	}
	if (!a || a >= to)
		return 0;
	gt = strchr(a, '>');
	if (!gt || gt >= to)
		return 0;
	b = strstr(gt, close_);
	if (!b || b > to)
		return 0;
	n = (size_t)(b - gt - 1);
	if (n >= cap)
		n = cap - 1;
	memcpy(out, gt + 1, n);
	out[n] = '\0';
	return 1;
}

static int attr(const char *tag_start, const char *tag_end, const char *name, char *out, size_t cap)
{
	char key[48];
	const char *p, *q;
	size_t n;

	snprintf(key, sizeof key, " %s=\"", name);
	p = strstr(tag_start, key);
	if (!p || p >= tag_end)
		return 0;
	p += strlen(key);
	q = memchr(p, '"', (size_t)(tag_end - p));
	if (!q)
		return 0;
	n = (size_t)(q - p);
	if (n >= cap)
		n = cap - 1;
	memcpy(out, p, n);
	out[n] = '\0';
	return 1;
}

static void clean(char *s)
{
	for (; *s; s++)
		if (*s == '\t' || *s == '\n' || *s == '\r')
			*s = ' ';
}

/* ---- discover ---- */

static int discover(int wait_ms)
{
	static const char msearch[] =
		"M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\n"
		"ST: urn:schemas-upnp-org:device:MediaServer:1\r\n\r\n";
	struct sockaddr_in dst;
	char locs[16][256];
	int nloc = 0, fd, i, ttl = 4;
	long end;

	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0)
		return 1;
	setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
	memset(&dst, 0, sizeof dst);
	dst.sin_family = AF_INET;
	dst.sin_port = htons(1900);
	inet_pton(AF_INET, "239.255.255.250", &dst.sin_addr);
	sendto(fd, msearch, sizeof msearch - 1, 0, (struct sockaddr *)&dst, sizeof dst);
	end = now_ms() + wait_ms;
	for (i = 0; ; ) {
		fd_set r;
		struct timeval tv;
		long left = end - now_ms();
		char pkt[2048], *l, *e;
		ssize_t n;
		int j;

		if (left <= 0)
			break;
		if (i == 0 && left < wait_ms - 600) {   /* UDP is lossy: ask a second time */
			sendto(fd, msearch, sizeof msearch - 1, 0, (struct sockaddr *)&dst, sizeof dst);
			i = 1;
		}
		tv.tv_sec = left / 1000;
		tv.tv_usec = (left % 1000) * 1000;
		FD_ZERO(&r);
		FD_SET(fd, &r);
		if (select(fd + 1, &r, NULL, NULL, &tv) <= 0)
			continue;
		n = recv(fd, pkt, sizeof pkt - 1, 0);
		if (n <= 0)
			continue;
		pkt[n] = '\0';
		l = strcasestr(pkt, "\r\nlocation:");
		if (!l)
			continue;
		l += 11;
		while (*l == ' ')
			l++;
		e = strpbrk(l, "\r\n");
		if (!e)
			continue;
		*e = '\0';
		for (j = 0; j < nloc; j++)
			if (!strcmp(locs[j], l))
				break;
		if (j == nloc && nloc < 16)
			snprintf(locs[nloc++], sizeof locs[0], "%s", l);
	}
	close(fd);
	for (i = 0; i < nloc; i++) {
		char *d = http("GET", locs[i], NULL, NULL), name[128] = "", ctrl[256] = "", host[128], path[8], base[300];
		char *sv, *end_;
		int port;

		if (!d)
			continue;
		if (!tag_text(d, d + strlen(d), "friendlyName", name, sizeof name))
			snprintf(name, sizeof name, "media server");
		unescape(name);
		clean(name);
		sv = d;
		while ((sv = strstr(sv, "<service>")) != NULL) {
			end_ = strstr(sv, "</service>");
			if (!end_)
				break;
			if (memmem(sv, (size_t)(end_ - sv), "ContentDirectory", 16)) {
				tag_text(sv, end_, "controlURL", ctrl, sizeof ctrl);
				break;
			}
			sv = end_;
		}
		free(d);
		if (!ctrl[0] || split_url(locs[i], host, sizeof host, &port, path, sizeof path) < 0)
			continue;
		unescape(ctrl);
		if (!strncmp(ctrl, "http://", 7))
			snprintf(base, sizeof base, "%s", ctrl);
		else if (ctrl[0] == '/')
			snprintf(base, sizeof base, "http://%s:%d%s", host, port, ctrl);
		else
			snprintf(base, sizeof base, "http://%s:%d/%s", host, port, ctrl);
		printf("%s\t%s\n", name, base);
	}
	return 0;
}

/* ---- browse ---- */

static int browse(const char *ctrl, const char *id)
{
	char body[2048], eid_x[1200], *resp, *res, *p;
	int n = 0, total = 0;

	/* an object id may hold & or < (a folder called "Simon & Garfunkel"): in the request it is element text */
	xml_escape(eid_x, sizeof eid_x, id);
	snprintf(body, sizeof body,
		 "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
		 "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body>"
		 "<u:Browse xmlns:u=\"urn:schemas-upnp-org:service:ContentDirectory:1\"><ObjectID>%s</ObjectID>"
		 "<BrowseFlag>BrowseDirectChildren</BrowseFlag><Filter>*</Filter><StartingIndex>0</StartingIndex>"
		 "<RequestedCount>%d</RequestedCount><SortCriteria></SortCriteria></u:Browse></s:Body></s:Envelope>",
		 eid_x, MAXITEMS);
	resp = http("POST", ctrl, "urn:schemas-upnp-org:service:ContentDirectory:1#Browse", body);
	if (!resp) {
		fprintf(stderr, "no answer from the server\n");
		return 1;
	}
	res = strstr(resp, "<Result>");
	if (!res) {
		fprintf(stderr, "server refused the request\n");
		free(resp);
		return 1;
	}
	res += 8;
	p = strstr(res, "</Result>");
	if (p) {
		char tm[24] = "";

		*p = '\0';
		if (tag_text(p + 1, p + 1 + strlen(p + 1), "TotalMatches", tm, sizeof tm))
			total = atoi(tm);
	}
	unescape(res);   /* the DIDL document, itself escaped inside the SOAP reply */
	/* folders first, then files, each in server order */
	for (int pass = 0; pass < 2; pass++) {
		const char *kind = pass ? "<item" : "<container";
		const char *close_ = pass ? "</item>" : "</container>";

		int seen = 0;   /* entries that print nothing count too: a hostile reply of empty containers is bounded */

		p = res;
		while ((p = strstr(p, kind)) != NULL && n < MAXITEMS * 2 && seen++ < MAXITEMS * 4) {
			char c = p[strlen(kind)];
			const char *tend = strchr(p, '>'), *end;
			char eid[300] = "", title[300] = "", url[600] = "", proto[200], cls[100] = "";

			if (c != ' ' || !tend)
				break;
			end = strstr(tend, close_);
			if (!end)
				end = res + strlen(res);
			attr(p, tend, "id", eid, sizeof eid);
			tag_text(tend, end, "dc:title", title, sizeof title);
			unescape(eid);
			unescape(title);
			clean(title);
			if (!title[0])
				snprintf(title, sizeof title, "(untitled)");
			if (!pass) {
				if (eid[0]) {
					printf("D\t%s\t%s\n", eid, title);
					n++;
				}
			} else {
				const char *r = tend;

				tag_text(tend, end, "upnp:class", cls, sizeof cls);
				while ((r = strstr(r, "<res ")) != NULL && r < end) {
					const char *rt = strchr(r, '>');
					const char *re = rt ? strstr(rt, "</res>") : NULL;

					if (!rt || !re || re > end)
						break;
					proto[0] = '\0';
					attr(r, rt, "protocolInfo", proto, sizeof proto);
					if ((strstr(proto, "audio/") || strstr(cls, "audioItem")) && !strstr(proto, "video/")) {
						size_t ul = (size_t)(re - rt - 1);

						if (ul >= sizeof url)
							ul = sizeof url - 1;
						memcpy(url, rt + 1, ul);
						url[ul] = '\0';
						unescape(url);
						break;
					}
					r = re;
				}
				if (url[0] && strlen(url) < 500) {
					printf("F\t%s\t%s\n", url, title);
					n++;
				}
			}
			p = (char *)end;
		}
	}
	if (total > n)
		printf("T\t%d\n", total);   /* the folder holds more than was asked for: the panel says so */
	free(resp);
	return 0;
}

int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	alarm(40);   /* a silent or dripping server must not hold the panel for minutes (lines already printed are out) */
	if (argc >= 2 && !strcmp(argv[1], "discover"))
		return discover(argc >= 3 ? atoi(argv[2]) : 2500);
	if (argc >= 4 && !strcmp(argv[1], "browse"))
		return browse(argv[2], argv[3]);
	fprintf(stderr, "usage: x45upnp discover [ms] | browse <control url> <object id>\n");
	return 2;
}
