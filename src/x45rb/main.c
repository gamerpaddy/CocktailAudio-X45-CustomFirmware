/* x45rb - browse the community radio directory (radio-browser.info) for the panel and the web page.
 *
 *   x45rb root                 the top menu (directories, one per line)
 *   x45rb top | trend          the best voted / the currently rising stations
 *   x45rb countries            countries with the most stations
 *   x45rb country <CC>         stations of a country (ISO code, DE ...)
 *   x45rb tags                 the most used genres
 *   x45rb tag <name>           stations of a genre
 *   x45rb languages | lang <name>
 *   x45rb search <words>       stations by name (used by the web page)
 *
 * Output, tab separated, one entry a line:
 *   D <path> <title>           a directory; the path is what to give this tool through the panel ("@r|...")
 *   S <stream url> <title>     a station, title "Name  [MP3 128k DE]"
 *
 * The directory is a free public service with no key. The tool fetches with the unit's curl
 * (https, the certificate bundle of the replacement software) and tries a few of its servers.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define CA "/home/root/x45new/etc/ssl/cacert.pem"
#define LIMIT 300   /* asked for; the AAC and HLS streams this player cannot decode are left out of the list */

static const char *const servers[] = { "de1", "de2", "nl1", "at1" };

/* ------------------------------------------------------------------ fetch --- */

static char *fetch(const char *path_query, size_t *len)
{
	char cmd[1200];
	size_t cap = 1 << 16, n = 0;
	char *buf = malloc(cap);
	unsigned s;

	if (!buf)
		return NULL;
	for (s = 0; s < sizeof servers / sizeof *servers; s++) {
		FILE *p;

		snprintf(cmd, sizeof cmd, "curl -s -f -m 12 --cacert " CA " -A x45/1.0 'https://%s.api.radio-browser.info%s' 2>/dev/null",
			 servers[s], path_query);
		p = popen(cmd, "r");
		if (!p)
			continue;
		n = 0;
		for (;;) {
			size_t r;

			if (n + 8192 + 1 > cap) {
				char *nb = realloc(buf, cap * 2);

				if (!nb) {
					pclose(p);
					free(buf);
					return NULL;
				}
				buf = nb;
				cap *= 2;
			}
			r = fread(buf + n, 1, 8192, p);
			if (r == 0)
				break;
			n += r;
		}
		pclose(p);
		if (n > 2) {
			buf[n] = '\0';
			*len = n;
			return buf;
		}
	}
	free(buf);
	return NULL;
}

/* ------------------------------------------------------------------- json --- */
/* Just enough for the directory's answers: an array of flat objects. Values of the wanted keys are
 * copied (strings unescaped to UTF-8, numbers as text); everything else is skipped. */

struct obj {
	char name[300], url[600], url_resolved[600], countrycode[8], codec[24], iso[8], bitrate[16], count[16], hls[8];
};

static const char *skip_ws(const char *p)
{
	while (*p && isspace((unsigned char)*p))
		p++;
	return p;
}

static void put_utf8(char **o, char *end, unsigned c)
{
	char *d = *o;

	if (c < 0x80 && d + 1 < end) {
		*d++ = (char)c;
	} else if (c < 0x800 && d + 2 < end) {
		*d++ = (char)(0xc0 | c >> 6);
		*d++ = (char)(0x80 | (c & 0x3f));
	} else if (d + 3 < end) {
		*d++ = (char)(0xe0 | c >> 12);
		*d++ = (char)(0x80 | ((c >> 6) & 0x3f));
		*d++ = (char)(0x80 | (c & 0x3f));
	}
	*o = d;
}

/* p at the opening quote; copies into out (cap), returns after the closing quote */
static const char *read_string(const char *p, char *out, size_t cap)
{
	char *o = out, *end = out + cap - 1;

	p++;
	while (*p && *p != '"') {
		if (*p == '\\' && p[1]) {
			p++;
			switch (*p) {
			case 'n': case 'r': case 't': put_utf8(&o, end, ' '); p++; break;
			case 'u': {
				unsigned c = 0;
				int i;

				for (i = 1; i <= 4 && isxdigit((unsigned char)p[i]); i++)
					c = c * 16 + (unsigned)(isdigit((unsigned char)p[i]) ? p[i] - '0' : (tolower((unsigned char)p[i]) - 'a' + 10));
				p += i;   /* past the 'u' and the digits there were: a cut-off escape must not skip the end of the text */
				if (c >= 0xd800 && c < 0xdc00 && p[0] == '\\' && p[1] == 'u') {   /* a surrogate pair */
					for (i = 2; i <= 5 && isxdigit((unsigned char)p[i]); i++)
						;
					p += i;
					put_utf8(&o, end, '?');   /* the panel font has no symbols anyway */
				} else {
					put_utf8(&o, end, c);
				}
				break;
			}
			default: put_utf8(&o, end, (unsigned char)*p); p++; break;
			}
		} else {
			if (o < end)
				*o++ = *p;
			p++;
		}
	}
	*o = '\0';
	return *p == '"' ? p + 1 : p;
}

/* skips one value of any kind */
static const char *skip_value(const char *p)
{
	int depth = 0;

	p = skip_ws(p);
	if (*p == '"') {
		char dummy[2];

		return read_string(p, dummy, sizeof dummy);
	}
	if (*p == '{' || *p == '[') {
		while (*p) {
			if (*p == '"') {
				char dummy[2];

				p = read_string(p, dummy, sizeof dummy);
				continue;
			}
			if (*p == '{' || *p == '[')
				depth++;
			if (*p == '}' || *p == ']') {
				depth--;
				if (depth == 0)
					return p + 1;
			}
			p++;
		}
		return p;
	}
	while (*p && *p != ',' && *p != '}' && *p != ']')
		p++;
	return p;
}

/* parses the object at p ('{'); returns after its '}' */
static const char *read_obj(const char *p, struct obj *o)
{
	memset(o, 0, sizeof *o);
	p = skip_ws(p + 1);
	while (*p && *p != '}') {
		char key[48];

		p = skip_ws(p);
		if (*p != '"')
			break;
		p = read_string(p, key, sizeof key);
		p = skip_ws(p);
		if (*p == ':')
			p++;
		p = skip_ws(p);
		{
			char *dst = NULL;
			size_t cap = 0;

			if (!strcmp(key, "name")) { dst = o->name; cap = sizeof o->name; }
			else if (!strcmp(key, "url")) { dst = o->url; cap = sizeof o->url; }
			else if (!strcmp(key, "url_resolved")) { dst = o->url_resolved; cap = sizeof o->url_resolved; }
			else if (!strcmp(key, "countrycode")) { dst = o->countrycode; cap = sizeof o->countrycode; }
			else if (!strcmp(key, "iso_3166_1")) { dst = o->iso; cap = sizeof o->iso; }
			else if (!strcmp(key, "codec")) { dst = o->codec; cap = sizeof o->codec; }
			else if (!strcmp(key, "bitrate")) { dst = o->bitrate; cap = sizeof o->bitrate; }
			else if (!strcmp(key, "stationcount")) { dst = o->count; cap = sizeof o->count; }
			else if (!strcmp(key, "hls")) { dst = o->hls; cap = sizeof o->hls; }
			if (dst && *p == '"') {
				p = read_string(p, dst, cap);
			} else if (dst) {
				size_t n = 0;

				while (*p && *p != ',' && *p != '}' && n + 1 < cap && !isspace((unsigned char)*p))
					dst[n++] = *p++;
				dst[n] = '\0';
			} else {
				p = skip_value(p);
			}
		}
		p = skip_ws(p);
		if (*p == ',')
			p++;
	}
	return *p == '}' ? p + 1 : p;
}

static void clean(char *s)
{
	char *o = s;
	int sp = 1;

	for (; *s; s++) {
		if (*s == '\t' || *s == '\n' || *s == '\r' || *s == ' ') {
			if (!sp)
				*o++ = ' ';
			sp = 1;
		} else {
			*o++ = *s;
			sp = 0;
		}
	}
	while (o > s && o[-1] == ' ')
		o--;
	*o = '\0';
}

/* what is in the answer, as lines for the panel */
static int emit(const char *json, int kind, const char *cmd)
{
	const char *p = skip_ws(json);
	int n = 0;
	struct obj o;

	if (*p != '[')
		return 0;
	p = skip_ws(p + 1);
	while (*p == '{' && n < 256) {
		p = read_obj(p, &o);
		clean(o.name);
		if (kind == 'S') {
			const char *u = o.url_resolved[0] ? o.url_resolved : o.url;

			int playable = !strcasecmp(o.codec, "MP3") || !strcasecmp(o.codec, "FLAC") || !strcasecmp(o.codec, "OGG") ||
				       !strcasecmp(o.codec, "OPUS") || !o.codec[0];

			if (atoi(o.hls) != 0 || strstr(u, ".m3u8"))
				playable = 0;
			if (o.name[0] && !strncmp(u, "http", 4) && playable && n < 200) {
				char info[64] = "";

				if (o.codec[0] && atoi(o.bitrate) > 0)
					snprintf(info, sizeof info, "  [%s %sk%s%s]", o.codec, o.bitrate, o.countrycode[0] ? " " : "", o.countrycode);
				else if (o.countrycode[0])
					snprintf(info, sizeof info, "  [%s]", o.countrycode);
				printf("S\t%s\t%s%s\n", u, o.name, info);
				n++;
			}
		} else if (kind == 'C') {                /* a country */
			if (o.iso[0] && o.name[0]) {
				printf("D\t@r|country|%s\t%s (%s)\n", o.iso, o.name, o.count);
				n++;
			}
		} else if (o.name[0]) {                  /* a tag (T) or a language (L) */
			printf("D\t@r|%s|%s\t%s (%s)\n", cmd, o.name, o.name, o.count);
			n++;
		}
		p = skip_ws(p);
		if (*p == ',')
			p = skip_ws(p + 1);
	}
	return n;
}

/* url-encodes into out */
static void enc(const char *s, char *out, size_t cap)
{
	size_t n = 0;

	for (; *s && n + 4 < cap; s++) {
		if (isalnum((unsigned char)*s) || *s == '-' || *s == '_' || *s == '.')
			out[n++] = *s;
		else
			n += (size_t)snprintf(out + n, cap - n, "%%%02X", (unsigned char)*s);
	}
	out[n] = '\0';
}

int main(int argc, char **argv)
{
	char q[900], a[300], *json;
	size_t len = 0;
	const char *cmd = argc > 1 ? argv[1] : "root";
	const char *arg = argc > 2 ? argv[2] : "";
	int kind = 'S';

	if (!strcmp(cmd, "root")) {
		puts("D\t@fav\tFavourites");
		puts("D\t@rt\tRouter stations");
		puts("D\t@r|top\tTop voted stations");
		puts("D\t@r|trend\tTrending now");
		puts("D\t@r|country|DE\tGermany");
		puts("D\t@r|countries\tBy country");
		puts("D\t@r|tags\tBy genre");
		puts("D\t@r|languages\tBy language");
		return 0;
	}
	enc(arg, a, sizeof a);
	if (!strcmp(cmd, "top")) {
		snprintf(q, sizeof q, "/json/stations/search?hidebroken=true&order=votes&reverse=true&limit=%d", LIMIT);
	} else if (!strcmp(cmd, "trend")) {
		snprintf(q, sizeof q, "/json/stations/search?hidebroken=true&order=clicktrend&reverse=true&limit=%d", LIMIT);
	} else if (!strcmp(cmd, "country")) {
		snprintf(q, sizeof q, "/json/stations/search?hidebroken=true&countrycode=%s&order=votes&reverse=true&limit=%d", a, LIMIT);
	} else if (!strcmp(cmd, "tag")) {
		snprintf(q, sizeof q, "/json/stations/search?hidebroken=true&tag=%s&tagExact=true&order=votes&reverse=true&limit=%d", a, LIMIT);
	} else if (!strcmp(cmd, "lang")) {
		snprintf(q, sizeof q, "/json/stations/search?hidebroken=true&language=%s&languageExact=true&order=votes&reverse=true&limit=%d", a, LIMIT);
	} else if (!strcmp(cmd, "search")) {
		snprintf(q, sizeof q, "/json/stations/search?hidebroken=true&name=%s&order=votes&reverse=true&limit=%d", a, LIMIT);
	} else if (!strcmp(cmd, "countries")) {
		snprintf(q, sizeof q, "/json/countries?order=stationcount&reverse=true&hidebroken=true&limit=80");
		kind = 'C';
	} else if (!strcmp(cmd, "tags")) {
		snprintf(q, sizeof q, "/json/tags?order=stationcount&reverse=true&hidebroken=true&limit=80");
		kind = 'T';
		cmd = "tag";
	} else if (!strcmp(cmd, "languages")) {
		snprintf(q, sizeof q, "/json/languages?order=stationcount&reverse=true&hidebroken=true&limit=60");
		kind = 'L';
		cmd = "lang";
	} else {
		fprintf(stderr, "usage: x45rb root|top|trend|countries|country CC|tags|tag NAME|languages|lang NAME|search WORDS\n");
		return 2;
	}
	json = fetch(q, &len);
	if (!json) {
		fprintf(stderr, "the radio directory did not answer\n");
		return 1;
	}
	if (emit(json, kind, cmd) == 0) {
		free(json);
		return 1;
	}
	free(json);
	return 0;
}
