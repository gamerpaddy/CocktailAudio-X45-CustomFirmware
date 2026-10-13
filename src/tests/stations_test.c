/* x45_stations_parse / x45_stations_load: the hand-edited radio list. */
#define _GNU_SOURCE
#include "../libx45/stations.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int fails, checks;

#define CHECK(cond, ...) do { \
	checks++; \
	if (!(cond)) { fails++; printf("FAIL line %d: ", __LINE__); \
		printf(__VA_ARGS__); printf("\n"); } \
} while (0)

int main(void)
{
	struct x45_station st[16];
	int n, skipped;

	/* the normal case, with comments, blanks and odd spacing */
	n = x45_stations_parse(
		"# my stations\n"
		"\n"
		"Groove Salad | https://ice1.somafm.com/groovesalad-128-mp3\n"
		"   Swiss Jazz|http://stream.srg-ssr.ch/m/rsj/mp3_128   \n"
		"\t# indented comment\n",
		st, 16, &skipped);
	CHECK(n == 2 && skipped == 0, "basic: n=%d skipped=%d", n, skipped);
	CHECK(!strcmp(st[0].name, "Groove Salad"), "name0 '%s'", st[0].name);
	CHECK(!strcmp(st[0].url, "https://ice1.somafm.com/groovesalad-128-mp3"), "url0 '%s'", st[0].url);
	CHECK(!strcmp(st[1].name, "Swiss Jazz") && !strcmp(st[1].url, "http://stream.srg-ssr.ch/m/rsj/mp3_128"),
	      "row1 '%s' '%s'", st[1].name, st[1].url);

	/* CRLF (the file will be edited on Windows) and no final newline */
	n = x45_stations_parse("A | http://a.example/s\r\nB | https://b.example/s", st, 16, &skipped);
	CHECK(n == 2 && skipped == 0, "crlf: n=%d", n);
	CHECK(n == 2 && !strcmp(st[0].url, "http://a.example/s") && !strcmp(st[1].url, "https://b.example/s"),
	      "crlf urls must not keep the CR: '%s'", st[0].url);

	/* rejected rows are counted and never reach the list */
	n = x45_stations_parse(
		"no bar here http://x.example/\n"
		"File | file:///etc/passwd\n"
		"Ftp | ftp://x.example/s\n"
		"Empty |\n"
		"Quote | http://x.example/\"; clear\n"
		"Back | http://x.example/a\\b\n"
		"Space | http://x.example/a b\n"
		"Ctl | http://x.example/\x01\n"
		"OK | http://ok.example/s\n",
		st, 16, &skipped);
	CHECK(n == 1 && skipped == 8, "rejects: n=%d skipped=%d", n, skipped);
	CHECK(n == 1 && !strcmp(st[0].name, "OK"), "survivor");

	/* a bare scheme is not a stream */
	n = x45_stations_parse("X | http://\nY | https://\n", st, 16, &skipped);
	CHECK(n == 0 && skipped == 2, "bare scheme: n=%d skipped=%d", n, skipped);

	/* no name: the URL stands in, clipped */
	n = x45_stations_parse(" | http://only.example/stream\n", st, 16, &skipped);
	CHECK(n == 1 && !strcmp(st[0].name, "http://only.example/stream"), "nameless: '%s'", st[0].name);

	/* an over-long name is clipped; an over-long URL skips the row */
	{
		char big[900], longname[200], longurl[400];
		memset(longname, 'n', sizeof longname - 1); longname[sizeof longname - 1] = 0;
		memset(longurl, 'u', sizeof longurl - 1); longurl[sizeof longurl - 1] = 0;
		snprintf(big, sizeof big, "%s | http://a.example/s\nbad | http://%s\n", longname, longurl);
		n = x45_stations_parse(big, st, 16, &skipped);
		CHECK(n == 1 && skipped == 1, "long: n=%d skipped=%d", n, skipped);
		CHECK(n == 1 && strlen(st[0].name) == X45_STATION_NAME - 1, "name clipped to %zu", strlen(st[0].name));
	}

	/* a URL of exactly the maximum accepted length, and one over */
	{
		char row[400], pad[300];
		size_t room = X45_STATION_URL - 1 - 7;           /* "http://" + room == 255 */
		memset(pad, 'p', room); pad[room] = 0;
		snprintf(row, sizeof row, "edge | http://%s\n", pad);
		n = x45_stations_parse(row, st, 16, &skipped);
		CHECK(n == 1 && strlen(st[0].url) == X45_STATION_URL - 1, "255-char URL kept: n=%d", n);
		pad[room] = 'p'; pad[room + 1] = 0;
		snprintf(row, sizeof row, "edge | http://%s\n", pad);
		n = x45_stations_parse(row, st, 16, &skipped);
		CHECK(n == 0 && skipped == 1, "256-char URL refused: n=%d skipped=%d", n, skipped);
	}

	/* the output bound */
	n = x45_stations_parse("A|http://a.example/\nB|http://b.example/\nC|http://c.example/\n", st, 2, &skipped);
	CHECK(n == 2, "max respected: %d", n);
	n = x45_stations_parse("A|http://a.example/\n", st, 0, &skipped);
	CHECK(n == 0, "max 0: %d", n);
	n = x45_stations_parse("", st, 16, &skipped);
	CHECK(n == 0 && skipped == 0, "empty text");
	n = x45_stations_parse("A|http://a.example/\n", st, 16, NULL);
	CHECK(n == 1, "NULL skipped pointer is allowed");

	/* load: missing file is an error, not an empty list */
	n = x45_stations_load("/nonexistent/stations.txt", st, 16, &skipped);
	CHECK(n == -ENOENT, "missing file: %d", n);

	/* load: a real file, and one with an embedded NUL */
	{
		char path[] = "/tmp/stations_test_XXXXXX";
		int fd = mkstemp(path);
		const char good[] = "One | http://one.example/s\nTwo | https://two.example/s\n";
		const char nul[] = "One | http://one.example/s\n\0Two | http://two.example/s\n";

		if (fd < 0) {
			printf("FAIL mkstemp\n");
			fails++;
		} else {
			write(fd, good, sizeof good - 1);
			close(fd);
			n = x45_stations_load(path, st, 16, &skipped);
			CHECK(n == 2 && !strcmp(st[1].name, "Two"), "load: n=%d", n);
			fd = open(path, O_WRONLY | O_TRUNC);
			write(fd, nul, sizeof nul - 1);
			close(fd);
			n = x45_stations_load(path, st, 16, &skipped);
			CHECK(n == -EILSEQ, "embedded NUL must be refused, got %d", n);
			unlink(path);
		}
	}

	printf("stations_test: %d/%d pass\n", checks - fails, checks);
	return fails ? 1 : 0;
}
