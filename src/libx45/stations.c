/* stations - see stations.h. */
#define _GNU_SOURCE
#include "stations.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Trim ASCII space/tab from both ends of [*s, *e). */
static void trim(const char **s, const char **e)
{
	while (*s < *e && (**s == ' ' || **s == '\t'))
		(*s)++;
	while (*e > *s && ((*e)[-1] == ' ' || (*e)[-1] == '\t' || (*e)[-1] == '\r'))
		(*e)--;
}

static int has_control(const char *s, const char *e)
{
	for (; s < e; s++)
		if ((unsigned char)*s < 0x20 || *s == 0x7f)
			return 1;
	return 0;
}

int x45_stations_parse(const char *text, struct x45_station *out, int max,
		       int *skipped)
{
	int n = 0, bad = 0;
	const char *p = text;

	while (*p && n < max) {
		const char *eol = strchr(p, '\n');
		const char *line_end = eol ? eol : p + strlen(p);
		const char *ls = p, *le = line_end, *bar, *ns, *ne, *us, *ue;
		size_t nl, ul;

		p = eol ? eol + 1 : line_end;
		trim(&ls, &le);
		if (ls == le || *ls == '#')
			continue;
		bar = memchr(ls, '|', (size_t)(le - ls));
		if (!bar || has_control(ls, le)) {
			bad++;
			continue;
		}
		ns = ls; ne = bar; trim(&ns, &ne);
		us = bar + 1; ue = le; trim(&us, &ue);
		ul = (size_t)(ue - us);
		{
			int is_http = ul > 7 && !strncmp(us, "http://", 7);
			int is_https = ul > 8 && !strncmp(us, "https://", 8);

			if (!ul || ul >= X45_STATION_URL || (!is_http && !is_https)) {
				bad++;
				continue;
			}
		}
		/* a URL may not contain a quote or backslash either: MPD argument
		 * quoting is the UI's job, but there is no legitimate station that
		 * needs them and refusing is cheaper than getting that wrong */
		if (memchr(us, '"', ul) || memchr(us, '\\', ul) || memchr(us, ' ', ul)) {
			bad++;
			continue;
		}
		nl = (size_t)(ne - ns);
		if (nl >= X45_STATION_NAME)
			nl = X45_STATION_NAME - 1;
		if (!nl) {                         /* no name: show the URL instead */
			ns = us;
			nl = ul < X45_STATION_NAME ? ul : X45_STATION_NAME - 1;
		}
		memcpy(out[n].name, ns, nl);
		out[n].name[nl] = '\0';
		memcpy(out[n].url, us, ul);
		out[n].url[ul] = '\0';
		n++;
	}
	if (skipped)
		*skipped = bad;
	return n;
}

int x45_stations_load(const char *path, struct x45_station *out, int max,
		      int *skipped)
{
	FILE *f = fopen(path, "rb");
	char *buf;
	long len;
	size_t got;
	int n;

	if (!f)
		return -errno;
	if (fseek(f, 0, SEEK_END) != 0 || (len = ftell(f)) < 0 || len > 256 * 1024) {
		fclose(f);
		return -EFBIG;
	}
	rewind(f);
	buf = malloc((size_t)len + 1);
	if (!buf) {
		fclose(f);
		return -ENOMEM;
	}
	got = fread(buf, 1, (size_t)len, f);
	fclose(f);
	buf[got] = '\0';
	/* an embedded NUL would end parsing silently at that point; treat the file
	 * as bad rather than half-read */
	if (strlen(buf) != got) {
		free(buf);
		return -EILSEQ;
	}
	n = x45_stations_parse(buf, out, max, skipped);
	free(buf);
	return n;
}
