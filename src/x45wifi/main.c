/* x45wifi - command line for the WiFi manager (see libx45/wifi.h). Never prints a key.
 *
 *   x45wifi status | scan | aps | list | events
 *   x45wifi add SSID [PSK|-]     save + join (PSK "-" reads it from stdin; none = open network)
 *   x45wifi join ID|SSID | forget ID|SSID | auto ID|SSID on|off
 *   x45wifi disconnect | reconnect | on | off
 */
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../libx45/wifi.h"

static const char *const state_name[] = { "off", "starting", "error", "idle", "scanning", "connecting", "connected" };

static int die(const char *what, int err)
{
	fprintf(stderr, "x45wifi: %s: %s\n", what, strerror(err < 0 ? -err : err));
	return 1;
}

/* an id given as a number, or as the name of a saved network */
static int find_net(struct x45_wifi *w, const char *s)
{
	struct x45_wifi_net n[16];
	int i, c = x45_wifi_networks(w, n, 16);
	char *e;
	long v = strtol(s, &e, 10);

	for (i = 0; i < c; i++)
		if (!strcmp(n[i].ssid, s))
			return n[i].id;
	if (*s && !*e)
		for (i = 0; i < c; i++)
			if (n[i].id == v)
				return n[i].id;
	return -1;
}

/* a network name is any 32 bytes an access point cares to broadcast: a terminal must not obey them */
static const char *vis(const char *s)
{
	static char b[96];
	size_t i;

	for (i = 0; s[i] && i < sizeof b - 1; i++)
		b[i] = (unsigned char)s[i] < 32 || s[i] == 127 ? '?' : s[i];
	b[i] = 0;
	return b;
}

static int cmd_status(struct x45_wifi *w)
{
	struct x45_wifi_status st;

	x45_wifi_status(w, &st);
	printf("state=%s\nenabled=%d\n", state_name[st.state], st.enabled);
	if (st.step[0])
		printf("step=%s\n", st.step);
	if (st.error[0])
		printf("error=%s\n", st.error);
	if (st.wpa_state[0])
		printf("wpa_state=%s\n", st.wpa_state);
	if (st.ssid[0])
		printf("ssid=%s\nfreq=%d\nbssid=%s\n", vis(st.ssid), st.freq, st.bssid);
	if (st.ip[0])
		printf("ip=%s\nrssi=%d\n", st.ip, st.rssi);
	return 0;
}

static int cmd_aps(struct x45_wifi *w)
{
	struct x45_wifi_ap ap[48];
	int i, n = x45_wifi_aps(w, ap, 48);

	if (n < 0)
		return die("aps", n);
	for (i = 0; i < n; i++)
		printf("%4d dBm %5d MHz  %-9s%s%s  %s\n", ap[i].rssi, ap[i].freq, ap[i].sec,
		       ap[i].saved >= 0 ? " saved" : "", ap[i].unsupported ? " unsupported" : "", vis(ap[i].ssid));
	return 0;
}

static int cmd_scan(struct x45_wifi *w)
{
	unsigned seq = w->scan_seq;
	struct pollfd p = { .fd = x45_wifi_fd(w), .events = POLLIN };
	int left = 100;   /* 10 s */

	if (x45_wifi_scan(w) < 0)
		return die("scan", ENOTCONN);
	while (left-- > 0 && w->scan_seq == seq) {
		poll(&p, 1, 100);
		x45_wifi_pump(w);
	}
	return cmd_aps(w);
}

static int cmd_list(struct x45_wifi *w)
{
	struct x45_wifi_net n[16];
	int i, c = x45_wifi_networks(w, n, 16);

	if (c < 0)
		return die("list", c);
	for (i = 0; i < c; i++)
		printf("%d  %-32s %s%s%s\n", n[i].id, vis(n[i].ssid), n[i].open ? "open" : "key",
		       n[i].auto_join ? "" : "  no-autoconnect", n[i].current ? "  [current]" : "");
	return 0;
}

static int cmd_events(struct x45_wifi *w)
{
	struct pollfd p = { .fd = x45_wifi_fd(w), .events = POLLIN };

	if (p.fd < 0)
		return die("events", ENOTCONN);
	for (;;) {
		unsigned c = w->conn_seq, l = w->lost_seq, f = w->fail_seq, s = w->scan_seq;

		poll(&p, 1, 1000);
		x45_wifi_pump(w);
		if (w->conn_seq != c || w->lost_seq != l || w->fail_seq != f || w->scan_seq != s) {
			printf("%s\n", w->last_event[0] ? w->last_event : "scan results");
			fflush(stdout);
		}
	}
}

int main(int argc, char **argv)
{
	struct x45_wifi w;
	const char *c = argc > 1 ? argv[1] : "status";
	int r, id;

	if (!strcmp(c, "on") || !strcmp(c, "off"))
		return x45_wifi_power(!strcmp(c, "on")) ? 1 : 0;
	r = x45_wifi_open(&w, !strcmp(c, "scan") || !strcmp(c, "events"));
	if (r < 0) {
		if (!strcmp(c, "status"))
			return cmd_status(&w);   /* reports off / starting / error from the progress file */
		return die("the WiFi manager is not running", r);
	}
	if (!strcmp(c, "status"))
		r = cmd_status(&w);
	else if (!strcmp(c, "scan"))
		r = cmd_scan(&w);
	else if (!strcmp(c, "aps"))
		r = cmd_aps(&w);
	else if (!strcmp(c, "list"))
		r = cmd_list(&w);
	else if (!strcmp(c, "events"))
		r = cmd_events(&w);
	else if (!strcmp(c, "add") && argc > 2) {
		char psk[128];
		const char *k = argc > 3 ? argv[3] : NULL;

		if (k && !strcmp(k, "-")) {
			if (!fgets(psk, sizeof psk, stdin))
				psk[0] = '\0';
			psk[strcspn(psk, "\r\n")] = '\0';
			k = psk;
		}
		id = x45_wifi_connect(&w, argv[2], k, 0);
		r = id < 0 ? die("add", id) : (printf("network %d, joining\n", id), 0);
		memset(psk, 0, sizeof psk);
	} else if (!strcmp(c, "join") && argc > 2) {
		id = find_net(&w, argv[2]);
		r = id < 0 ? die("join", ENOENT) : x45_wifi_join(&w, id) ? die("join", EIO) : 0;
	} else if (!strcmp(c, "forget") && argc > 2) {
		id = find_net(&w, argv[2]);
		r = id < 0 ? die("forget", ENOENT) : x45_wifi_forget(&w, id) ? die("forget", EIO) : 0;
	} else if (!strcmp(c, "auto") && argc > 3 && (!strcmp(argv[3], "on") || !strcmp(argv[3], "off"))) {
		id = find_net(&w, argv[2]);
		r = id < 0 ? die("auto", ENOENT) : x45_wifi_set_auto(&w, id, !strcmp(argv[3], "on")) ? die("auto", EIO) : 0;
	} else if (!strcmp(c, "disconnect"))
		r = x45_wifi_disconnect(&w) ? die("disconnect", EIO) : 0;
	else if (!strcmp(c, "reconnect"))
		r = x45_wifi_reconnect(&w) ? die("reconnect", EIO) : 0;
	else {
		fprintf(stderr, "usage: x45wifi status|scan|aps|list|events | add SSID [PSK|-] | join|forget ID|SSID |\n"
				"       auto ID|SSID on|off | disconnect | reconnect | on | off\n");
		r = 2;
	}
	x45_wifi_close(&w);
	return r;
}
