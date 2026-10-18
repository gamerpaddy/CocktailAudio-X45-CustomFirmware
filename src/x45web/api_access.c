#define _GNU_SOURCE
/* api_access - /api/access: the access token of the web interface.
 *
 *   GET  /api/access    {"protected":true|false}   (true: web_token is set in etc/x45ui.conf)
 *   POST /api/access    text/plain: the new token (4..64 of A-Z a-z 0-9 . _ ~ -), or nothing to remove it
 *
 * With a token set everybody can still look (the GETs are open: a guest sees the state, the lists and the logs of
 * the unit, apart from the file manager and the logs, which ask for the token too), but every change - a command, a
 * setting, a delete, an upload - needs it. main.c has already checked the old token on the POST when it gets here.
 * The conf file is rewritten as it is, but for the one line.
 */
#include "web.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CONF_FILE "/home/root/x45new/etc/x45ui.conf"

static int is_key_line(const char *l)
{
	while (*l == ' ' || *l == '\t')
		l++;
	if (strncmp(l, "web_token", 9))
		return 0;
	l += 9;
	while (*l == ' ' || *l == '\t')
		l++;
	return *l == '=';
}

static int token_set(void)
{
	char line[256];
	FILE *f = fopen(CONF_FILE, "r");
	int set = 0;

	if (!f)
		return 0;
	while (fgets(line, sizeof line, f))
		if (is_key_line(line)) {
			const char *v = strchr(line, '=') + 1;

			while (*v == ' ' || *v == '\t')
				v++;
			set = *v && *v != '\r' && *v != '\n';
		}
	fclose(f);
	return set;
}

static int valid(const char *t)
{
	size_t n = strlen(t), i;

	if (n < 4 || n > 64)
		return 0;
	for (i = 0; i < n; i++)
		if (!isalnum((unsigned char)t[i]) && !strchr("._~-", t[i]))
			return 0;
	return 1;
}

static int write_token(const char *tok)   /* "" removes it */
{
	char line[1100], tmp[96];
	FILE *in = fopen(CONF_FILE, "r"), *out;
	struct stat st;
	int ok, nl = 1;

	snprintf(tmp, sizeof tmp, "%s.tmp", CONF_FILE);
	out = fopen(tmp, "w");
	if (!out) {
		if (in)
			fclose(in);
		return -1;
	}
	if (in) {
		while (fgets(line, sizeof line, in))
			if (!is_key_line(line)) {
				fputs(line, out);
				nl = line[strlen(line) - 1] == 10;   /* a last line without a newline would swallow the token line */
			}
		fstat(fileno(in), &st);
		fchmod(fileno(out), st.st_mode & 0777);
		fclose(in);
	} else {
		fchmod(fileno(out), 0600);
	}
	if (tok[0] && !nl)
		fputc(10, out);
	if (tok[0])
		fprintf(out, "web_token = %s\n", tok);
	ok = fflush(out) == 0 && !ferror(out);
	fclose(out);
	if (!ok || rename(tmp, CONF_FILE) != 0) {
		unlink(tmp);
		return -1;
	}
	return 0;
}

int api_access(int fd, const char *method, const char *path, const char *body, size_t len)
{
	char tok[80];
	size_t n;

	if (strcmp(path, "/api/access"))
		return 0;
	if (!strcmp(method, "GET") || !strcmp(method, "HEAD")) {
		respond_json(fd, 200, token_set() ? "{\"protected\":true}" : "{\"protected\":false}");
		return 1;
	}
	if (strcmp(method, "POST")) {
		respond_text(fd, 405, "GET or POST");
		return 1;
	}
	n = len < sizeof tok - 1 ? len : sizeof tok - 1;
	memcpy(tok, body, n);
	tok[n] = '\0';
	while (n && isspace((unsigned char)tok[n - 1]))
		tok[--n] = '\0';
	if (len > sizeof tok - 1 || (tok[0] && !valid(tok))) {
		respond_json(fd, 400, "{\"ok\":false,\"error\":\"4 to 64 letters, digits or . _ ~ -\"}");
		return 1;
	}
	if (write_token(tok) != 0) {
		respond_json(fd, 500, "{\"ok\":false,\"error\":\"could not save\"}");
		return 1;
	}
	respond_json(fd, 200, tok[0] ? "{\"ok\":true,\"protected\":true}" : "{\"ok\":true,\"protected\":false}");
	return 1;
}
