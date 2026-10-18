#define _GNU_SOURCE
/* api_wifi - /api/wifi: the WiFi manager over HTTP. Same library as the panel and the command line, so all
 * three see the same saved networks. A saved network's key is never sent out.
 *
 *   GET  /api/wifi              state, the joined network, the saved networks
 *   GET  /api/wifi/scan         what the last scan heard (POST /api/wifi/scan starts a new one)
 *   POST /api/wifi/scan
 *   POST /api/wifi/connect      ssid=NAME  psk=KEY (empty: open)  hidden=0|1   - answers with the outcome
 *   POST /api/wifi/join         id=N        a saved network, now
 *   POST /api/wifi/forget       id=N
 *   POST /api/wifi/auto         id=N  on=0|1
 *   POST /api/wifi/disconnect
 *   POST /api/wifi/reconnect
 *   POST /api/wifi/power        on=0|1      (switching on takes up to half a minute: poll GET /api/wifi)
 *
 * Bodies are "key=value" lines. Answers to POSTs: {"ok":true,...} or {"ok":false,"error":"why"}.
 *
 * A page that reached the unit over its WiFi address (GET /api/wifi says "via_wifi":true) cannot change the link it is
 * on: everything but scan, the auto flag and switching WiFi on answers 409. Over the cable it can do all of it.
 */
#include "web.h"
#include "../libx45/wifi.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#define CONNECT_WAIT_MS 20000

/* the errors that mean "there is no wpa_supplicant to talk to" */
static int is_off(int r)
{
	return r == -ENOENT || r == -ECONNREFUSED || r == -ENOTCONN;
}

static const char *const state_name[] = { "off", "starting", "error", "idle", "scanning", "connecting", "connected" };

/* value of "key=" in the body lines, "" when absent */
static const char *field(const char *body, size_t len, const char *key, char *out, size_t sz)
{
	size_t kl = strlen(key), i = 0;

	out[0] = '\0';
	while (i < len) {
		size_t e = i;

		while (e < len && body[e] != '\n')
			e++;
		if (e - i > kl && !strncmp(body + i, key, kl) && body[i + kl] == '=') {
			size_t n = e - i - kl - 1;

			if (n && body[i + kl + 1 + n - 1] == '\r')
				n--;
			if (n >= sz)
				n = sz - 1;
			memcpy(out, body + i + kl + 1, n);
			out[n] = '\0';
			return out;
		}
		i = e + 1;
	}
	return out;
}

static void answer(int fd, int code, int ok, const char *err)
{
	char b[160];

	if (ok)
		snprintf(b, sizeof b, "{\"ok\":true}");
	else
		snprintf(b, sizeof b, "{\"ok\":false,\"error\":\"%.100s\"}", err);
	respond_json(fd, code, b);
}

/* 1 when this request came in over the unit's own WiFi address: whatever changes or drops that link would cut the page off */
static int via_wifi(int fd, const struct x45_wifi_status *st)
{
	struct sockaddr_in a;
	socklen_t l = sizeof a;
	char ip[INET_ADDRSTRLEN];

	if (!st->ip[0] || getsockname(fd, (struct sockaddr *)&a, &l) != 0 || a.sin_family != AF_INET)
		return 0;
	return inet_ntop(AF_INET, &a.sin_addr, ip, sizeof ip) && !strcmp(ip, st->ip);
}

#define VIA_WIFI_WHY "this page runs over the unit's WiFi; that would cut it off (use the cable or the panel)"

static void put_status(FILE *f, const struct x45_wifi_status *st)
{
	fprintf(f, "\"state\":\"%s\",\"enabled\":%s,\"id\":%d", state_name[st->state], st->enabled ? "true" : "false", st->id);
	fputs(",\"ssid\":", f);
	jesc(f, st->ssid);
	fprintf(f, ",\"bssid\":\"%s\",\"ip\":\"%s\",\"freq\":%d,\"rssi\":%d,\"wpa_state\":\"%s\"", st->bssid, st->ip, st->freq,
		st->rssi, st->wpa_state);
	fputs(",\"step\":", f);
	jesc(f, st->step);
	fputs(",\"error\":", f);
	jesc(f, st->error);
}

static void send_status(int fd, struct x45_wifi *w)
{
	static char out[8192];
	FILE *f = fmemopen(out, sizeof out, "w");
	struct x45_wifi_status st;
	struct x45_wifi_net nets[16];
	int n, i;

	if (!f) {
		answer(fd, 503, 0, "no memory");
		return;
	}
	x45_wifi_status(w, &st);
	fputc('{', f);
	put_status(f, &st);
	fprintf(f, ",\"via_wifi\":%s", via_wifi(fd, &st) ? "true" : "false");
	fputs(",\"networks\":[", f);
	n = x45_wifi_networks(w, nets, 16);
	for (i = 0; i < n; i++) {
		fprintf(f, "%s{\"id\":%d,\"ssid\":", i ? "," : "", nets[i].id);
		jesc(f, nets[i].ssid);
		fprintf(f, ",\"current\":%s,\"auto\":%s,\"open\":%s}", nets[i].current ? "true" : "false",
			nets[i].auto_join ? "true" : "false", nets[i].open ? "true" : "false");
	}
	fputs("]}", f);
	fclose(f);
	respond_json(fd, 200, out);
}

static void send_scan(int fd, struct x45_wifi *w)
{
	static char out[24576];
	static struct x45_wifi_ap aps[64];
	FILE *f = fmemopen(out, sizeof out, "w");
	int n, i;

	if (!f) {
		answer(fd, 503, 0, "no memory");
		return;
	}
	n = x45_wifi_aps(w, aps, 64);
	if (n < 0)
		n = 0;
	fputs("{\"aps\":[", f);
	for (i = 0; i < n; i++) {
		fprintf(f, "%s{\"ssid\":", i ? "," : "");
		jesc(f, aps[i].ssid);
		fprintf(f, ",\"bssid\":\"%s\",\"freq\":%d,\"rssi\":%d,\"secure\":%s,\"unsupported\":%s,\"saved\":%d,\"sec\":\"%s\"}",
			aps[i].bssid, aps[i].freq, aps[i].rssi, aps[i].secure ? "true" : "false",
			aps[i].unsupported ? "true" : "false", aps[i].saved, aps[i].sec);
	}
	fputs("]}", f);
	fclose(f);
	respond_json(fd, 200, out);
}

static void do_connect(int fd, struct x45_wifi *w, const char *body, size_t len)
{
	char ssid[X45_WIFI_SSID + 8], psk[96], hid[8], why[48], reply[160];
	struct x45_wifi_mark mark;
	struct x45_wifi_net nets[16];
	int id, r, n, i, existed = 0;

	field(body, len, "ssid", ssid, sizeof ssid);
	field(body, len, "psk", psk, sizeof psk);
	field(body, len, "hidden", hid, sizeof hid);
	if (!ssid[0] || strlen(ssid) > 32) {   /* 32 bytes is what a network name can be */
		answer(fd, 400, 0, "the network name is missing or too long");
		return;
	}
	n = x45_wifi_networks(w, nets, 16);
	for (i = 0; i < n; i++)
		existed |= !strcmp(nets[i].ssid, ssid);
	x45_wifi_mark(w, &mark);
	id = x45_wifi_connect(w, ssid, psk, hid[0] == '1');
	if (id < 0) {
		if (is_off(id))   /* the same wording as the other endpoints */
			answer(fd, 503, 0, "WiFi is off");
		else
			answer(fd, 400, 0, id == -EINVAL ? "that password cannot be right (8 to 63 characters)" :
				id == -ENOSPC ? "16 networks are saved: forget one first" : "could not save the network");
		return;
	}
	x45_wifi_scan(w);   /* so that "is it in range" below is answered from a fresh look */
	r = x45_wifi_wait(w, &mark, ssid, CONNECT_WAIT_MS / 2, why, sizeof why);
	if (r == 0 && hid[0] != '1') {
		static struct x45_wifi_ap aps[64];
		int na = x45_wifi_aps(w, aps, 64), seen = 0;

		for (i = 0; i < na; i++)
			seen |= !strcmp(aps[i].ssid, ssid);
		if (!seen)   /* nothing of that name was heard: wpa_supplicant keeps looking, but there is nothing to wait for */
			r = -1;
		else
			r = x45_wifi_wait(w, &mark, ssid, CONNECT_WAIT_MS / 2, why, sizeof why);
	}
	if (r < 0 && w->fail_seq != mark.fail) {   /* the router refused the key: say so, and do not keep a network that never worked */
		if (!existed)
			x45_wifi_forget(w, id);
		snprintf(reply, sizeof reply, "{\"ok\":false,\"result\":\"failed\",\"error\":\"%s\"}", why);
		respond_json(fd, 400, reply);
		return;
	}
	/* joined; saved but out of range right now; or still trying */
	snprintf(reply, sizeof reply, "{\"ok\":true,\"id\":%d,\"result\":\"%s\"}", id,
		 r > 0 ? "connected" : r < 0 ? "not_in_range" : "pending");
	respond_json(fd, 200, reply);
}

int api_wifi(int fd, const char *method, const char *path, const char *body, size_t len)
{
	struct x45_wifi w;
	char a[32], b[32];
	int post = !strcmp(method, "POST"), id, r = 0;

	if (strncmp(path, "/api/wifi", 9) || (path[9] && path[9] != '/'))
		return 0;
	path += 9;
	x45_wifi_open(&w, !strcmp(path, "/connect"));   /* status needs no socket; the connect waits for events */
	if (post && strcmp(path, "/scan") && strcmp(path, "/auto")) {   /* a scan or the auto flag leaves the link as it is */
		struct x45_wifi_status st;

		if (!strcmp(path, "/power"))
			field(body, len, "on", a, sizeof a);
		else
			a[0] = '\0';
		x45_wifi_status(&w, &st);
		if ((strcmp(path, "/power") || a[0] != '1') && via_wifi(fd, &st)) {
			x45_wifi_close(&w);
			answer(fd, 409, 0, VIA_WIFI_WHY);
			return 1;
		}
	}
	if (!post) {
		if (!strcmp(path, ""))
			send_status(fd, &w);
		else if (!strcmp(path, "/scan"))
			send_scan(fd, &w);
		else
			respond_text(fd, 404, "not found");
	} else if (!strcmp(path, "/connect")) {
		do_connect(fd, &w, body, len);
	} else if (!strcmp(path, "/power")) {
		field(body, len, "on", a, sizeof a);
		r = x45_wifi_power(a[0] == '1');
		answer(fd, r < 0 ? 503 : 200, r >= 0, "could not run the WiFi script");
	} else {
		field(body, len, "id", a, sizeof a);
		{
			char *end;
			long v = strtol(a, &end, 10);

			id = a[0] && !*end && v >= 0 && v < 1000 ? (int)v : -1;   /* "id=abc" is not network 0 */
		}
		if (id < 0 && (!strcmp(path, "/join") || !strcmp(path, "/forget") || !strcmp(path, "/auto"))) {
			x45_wifi_close(&w);
			answer(fd, 400, 0, "id is missing or not a number");
			return 1;
		}
		if (!strcmp(path, "/scan"))
			r = x45_wifi_scan(&w);
		else if (!strcmp(path, "/disconnect"))
			r = x45_wifi_disconnect(&w);
		else if (!strcmp(path, "/reconnect"))
			r = x45_wifi_reconnect(&w);
		else if (!strcmp(path, "/join") && id >= 0)
			r = x45_wifi_join(&w, id);
		else if (!strcmp(path, "/forget") && id >= 0)
			r = x45_wifi_forget(&w, id);
		else if (!strcmp(path, "/auto") && id >= 0) {
			field(body, len, "on", b, sizeof b);
			r = x45_wifi_set_auto(&w, id, b[0] == '1');
		} else {
			x45_wifi_close(&w);
			respond_text(fd, 404, "not found");
			return 1;
		}
		answer(fd, r < 0 ? 503 : 200, r >= 0, is_off(r) ? "WiFi is off" : "the request failed");
	}
	x45_wifi_close(&w);
	return 1;
}
