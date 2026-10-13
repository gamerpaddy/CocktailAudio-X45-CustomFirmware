/* wifi - see wifi.h. Plain wpa_supplicant control-socket protocol, nothing else. */
#define _GNU_SOURCE
#include "wifi.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define IFNAME "wlan0"
#define STATUS_FILE "/tmp/x45wifi.status"
#define REPLY_MAX 4096
#define AP_MAX 128

static int path_ctr;

/* ------------------------------------------------------------------ plumbing --- */

static int ctl_open(char *path, size_t n, const char *tag)
{
	struct sockaddr_un a, s;
	int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);

	if (fd < 0)
		return -errno;
	memset(&a, 0, sizeof a);
	a.sun_family = AF_UNIX;
	snprintf(path, n, "/tmp/x45wifi-%d-%s%d", (int)getpid(), tag, path_ctr++);
	unlink(path);
	snprintf(a.sun_path, sizeof a.sun_path, "%s", path);
	if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0) {
		int e = errno;

		close(fd);
		return -e;
	}
	chmod(path, 0600);
	memset(&s, 0, sizeof s);
	s.sun_family = AF_UNIX;
	snprintf(s.sun_path, sizeof s.sun_path, "%s/%s", X45_WIFI_CTL_DIR, IFNAME);
	if (connect(fd, (struct sockaddr *)&s, sizeof s) < 0) {
		int e = errno;

		close(fd);
		unlink(path);
		return -e;
	}
	return fd;
}

/* one request on a socket: returns the reply length, or -errno */
static int ctl_request(int fd, const char *cmd, char *reply, int n, int timeout_ms)
{
	struct pollfd p = { .fd = fd, .events = POLLIN };
	char junk[REPLY_MAX];
	int r;

	while (recv(fd, junk, sizeof junk, MSG_DONTWAIT) > 0)   /* a late answer to an earlier request */
		;
	if (send(fd, cmd, strlen(cmd), 0) < 0)
		return -errno;
	r = poll(&p, 1, timeout_ms);
	if (r < 0)
		return -errno;
	if (r == 0)
		return -ETIMEDOUT;
	r = recv(fd, reply, n - 1, 0);
	if (r < 0)
		return -errno;
	reply[r] = '\0';
	while (r > 0 && (reply[r - 1] == '\n' || reply[r - 1] == '\r'))
		reply[--r] = '\0';
	return r;
}

int x45_wifi_open(struct x45_wifi *w, int events)
{
	char buf[32];
	int fd;

	memset(w, 0, sizeof *w);
	w->req = w->mon = -1;
	w->fail_id = -1;
	fd = ctl_open(w->req_path, sizeof w->req_path, "r");
	if (fd < 0)
		return fd;
	w->req = fd;
	if (events) {
		fd = ctl_open(w->mon_path, sizeof w->mon_path, "m");
		if (fd >= 0) {
			if (ctl_request(fd, "ATTACH", buf, sizeof buf, 1000) > 0 && !strcmp(buf, "OK"))
				w->mon = fd;
			else {
				close(fd);
				unlink(w->mon_path);
			}
		}
	}
	return 0;
}

void x45_wifi_close(struct x45_wifi *w)
{
	if (w->mon >= 0) {
		char buf[32];

		ctl_request(w->mon, "DETACH", buf, sizeof buf, 200);
		close(w->mon);
		unlink(w->mon_path);
	}
	if (w->req >= 0) {
		close(w->req);
		unlink(w->req_path);
	}
	w->req = w->mon = -1;
}

int x45_wifi_fd(const struct x45_wifi *w)
{
	return w->mon;
}

static void event_parse(struct x45_wifi *w, const char *m)
{
	const char *p;

	if (*m == '<' && (p = strchr(m, '>')))
		m = p + 1;
	if (!strncmp(m, "CTRL-EVENT-SCAN-RESULTS", 23)) {
		w->scan_seq++;
		return;
	}
	if (!strncmp(m, "CTRL-EVENT-BSS-", 15) || !strncmp(m, "CTRL-EVENT-SIGNAL", 17) ||
	    !strncmp(m, "CTRL-EVENT-SCAN-STARTED", 23))
		return;
	snprintf(w->last_event, sizeof w->last_event, "%.95s", m);
	if (!strncmp(m, "CTRL-EVENT-CONNECTED", 20)) {
		w->conn_seq++;
		w->fail[0] = '\0';
	} else if (!strncmp(m, "CTRL-EVENT-DISCONNECTED", 23)) {
		w->lost_seq++;
	} else if (!strncmp(m, "CTRL-EVENT-SSID-TEMP-DISABLED", 29)) {
		if ((p = strstr(m, " id=")))
			w->fail_id = atoi(p + 4);
		p = strstr(m, "reason=");
		snprintf(w->fail, sizeof w->fail, "%s", p ? p + 7 : "UNKNOWN");
		w->fail_seq++;
	} else if (!strncmp(m, "CTRL-EVENT-NETWORK-NOT-FOUND", 28)) {
		w->notfound_seq++;
	} else if (!strncmp(m, "CTRL-EVENT-ASSOC-REJECT", 23)) {
		snprintf(w->fail, sizeof w->fail, "ASSOC_REJECT");
		w->fail_seq++;
	} else if (!strncmp(m, "CTRL-EVENT-AUTH-REJECT", 22)) {
		snprintf(w->fail, sizeof w->fail, "AUTH_REJECT");
		w->fail_seq++;
	}
}

int x45_wifi_pump(struct x45_wifi *w)
{
	static time_t retry;
	char buf[REPLY_MAX];
	int n = 0, r;

	if (w->mon < 0) {
		/* wpa_supplicant went away and came back (or had not started yet): attach again, now and then */
		struct timespec ts;
		time_t now;

		clock_gettime(CLOCK_MONOTONIC, &ts);   /* not the wall clock: it can be set backwards when the time arrives */
		now = ts.tv_sec;
		if (now - retry >= 2) {
			struct x45_wifi t;

			retry = now;
			if (x45_wifi_open(&t, 1) == 0 && t.mon >= 0) {
				t.scan_seq = w->scan_seq;
				t.conn_seq = w->conn_seq;
				t.lost_seq = w->lost_seq;
				t.fail_seq = w->fail_seq;
				t.notfound_seq = w->notfound_seq;
				x45_wifi_close(w);
				*w = t;
			} else if (t.req >= 0) {
				x45_wifi_close(&t);
			}
		}
		return 0;
	}
	while ((r = recv(w->mon, buf, sizeof buf - 1, MSG_DONTWAIT)) > 0) {
		buf[r] = '\0';
		event_parse(w, buf);
		n++;
	}
	if (r < 0 && errno != EAGAIN && errno != EINTR) {
		/* the peer is gone: close both, the next call opens them afresh */
		x45_wifi_close(w);
	}
	return n;
}

int x45_wifi_cmd(struct x45_wifi *w, const char *cmd, char *reply, int n)
{
	int r, tries;

	for (tries = 0; tries < 2; tries++) {
		if (w->req < 0) {
			int fd = ctl_open(w->req_path, sizeof w->req_path, "r");

			if (fd < 0)
				return fd;
			w->req = fd;
		}
		r = ctl_request(w->req, cmd, reply, n, 1000);
		if (r >= 0)
			return r;
		if (r == -ECONNREFUSED || r == -ENOENT || r == -ENOTCONN) {
			/* wpa_supplicant was restarted: our socket points at the old one */
			close(w->req);
			unlink(w->req_path);
			w->req = -1;
			if (w->mon >= 0) {
				close(w->mon);
				unlink(w->mon_path);
				w->mon = -1;
			}
			continue;
		}
		return r;
	}
	return -ECONNREFUSED;
}

/* ------------------------------------------------------------------- helpers --- */

/* wpa_supplicant prints an SSID with \xNN, \\, \" , \n etc. escapes; undo them */
static void ssid_decode(const char *s, char *out, size_t n)
{
	size_t o = 0;

	while (*s && o + 1 < n) {
		if (*s == '\\' && s[1]) {
			s++;
			if (*s == 'x' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
				char h[3] = { s[1], s[2], 0 };

				out[o++] = (char)strtol(h, NULL, 16);
				s += 3;
				continue;
			}
			out[o++] = *s == 'n' ? '\n' : *s == 'r' ? '\r' : *s == 't' ? '\t' : *s == 'e' ? 27 : *s;
			s++;
			continue;
		}
		out[o++] = *s++;
	}
	out[o] = '\0';
}

static void hex_encode(const char *s, char *out)
{
	static const char hx[] = "0123456789abcdef";

	for (; *s; s++) {
		*out++ = hx[(unsigned char)*s >> 4];
		*out++ = hx[(unsigned char)*s & 15];
	}
	*out = '\0';
}

/* the value of "key=" at the start of a line of a key=value block */
static const char *kv(const char *blk, const char *key, char *out, size_t n)
{
	size_t kl = strlen(key);
	const char *p = blk;

	while (p && *p) {
		if (!strncmp(p, key, kl) && p[kl] == '=') {
			const char *e = strchr(p + kl + 1, '\n');
			size_t l = e ? (size_t)(e - p - kl - 1) : strlen(p + kl + 1);

			if (l >= n)
				l = n - 1;
			memcpy(out, p + kl + 1, l);
			out[l] = '\0';
			return out;
		}
		p = strchr(p, '\n');
		if (p)
			p++;
	}
	out[0] = '\0';
	return NULL;
}

static void read_progress(struct x45_wifi_status *st, char *state, size_t sn)
{
	char buf[256];
	FILE *f = fopen(STATUS_FILE, "r");
	size_t r;

	state[0] = '\0';
	if (!f)
		return;
	r = fread(buf, 1, sizeof buf - 1, f);
	fclose(f);
	buf[r] = '\0';
	kv(buf, "state", state, sn);
	kv(buf, "step", st->step, sizeof st->step);
	kv(buf, "error", st->error, sizeof st->error);
}

/* ------------------------------------------------------------------- queries --- */

int x45_wifi_status(struct x45_wifi *w, struct x45_wifi_status *st)
{
	char reply[REPLY_MAX], v[160], prog[16];   /* a 32-byte name of umlauts is 128 characters of \xNN escapes */
	int r;

	memset(st, 0, sizeof *st);
	st->id = -1;
	st->enabled = access(X45_WIFI_STATE_DIR "/enabled", F_OK) == 0;
	read_progress(st, prog, sizeof prog);
	r = x45_wifi_cmd(w, "STATUS", reply, sizeof reply);
	if (r < 0) {
		st->state = !strcmp(prog, "starting") ? X45_WIFI_STARTING :
			    !strcmp(prog, "error") ? X45_WIFI_ERROR : X45_WIFI_OFF;
		return r;
	}
	kv(reply, "wpa_state", st->wpa_state, sizeof st->wpa_state);
	if (kv(reply, "ssid", v, sizeof v))
		ssid_decode(v, st->ssid, sizeof st->ssid);
	kv(reply, "bssid", st->bssid, sizeof st->bssid);
	kv(reply, "ip_address", st->ip, sizeof st->ip);
	if (kv(reply, "id", v, sizeof v))
		st->id = atoi(v);
	if (kv(reply, "freq", v, sizeof v))
		st->freq = atoi(v);
	if (!strcmp(st->wpa_state, "COMPLETED"))
		st->state = st->ip[0] ? X45_WIFI_CONNECTED : X45_WIFI_CONNECTING;
	else if (!strcmp(st->wpa_state, "SCANNING"))
		st->state = X45_WIFI_SCANNING;
	else if (!strcmp(st->wpa_state, "ASSOCIATING") || !strcmp(st->wpa_state, "ASSOCIATED") ||
		 !strcmp(st->wpa_state, "AUTHENTICATING") || !strcmp(st->wpa_state, "4WAY_HANDSHAKE") ||
		 !strcmp(st->wpa_state, "GROUP_HANDSHAKE"))
		st->state = X45_WIFI_CONNECTING;
	else
		st->state = X45_WIFI_IDLE;
	if (st->state == X45_WIFI_CONNECTED &&
	    x45_wifi_cmd(w, "SIGNAL_POLL", reply, sizeof reply) > 0 && kv(reply, "RSSI", v, sizeof v))
		st->rssi = atoi(v);
	return 0;
}

int x45_wifi_scan(struct x45_wifi *w)
{
	char reply[64];
	int r = x45_wifi_cmd(w, "SCAN", reply, sizeof reply);

	if (r < 0)
		return r;
	/* "FAIL-BUSY" while a scan runs already: the result is coming either way */
	return 0;
}

static void sec_from_flags(const char *fl, struct x45_wifi_ap *ap)
{
	int wpa = strstr(fl, "WPA-") != NULL, rsn = strstr(fl, "WPA2-") != NULL || strstr(fl, "RSN-") != NULL;
	int psk = strstr(fl, "-PSK") != NULL, sae = strstr(fl, "SAE") != NULL, eap = strstr(fl, "-EAP") != NULL;
	int wep = strstr(fl, "WEP") != NULL, owe = strstr(fl, "OWE") != NULL;

	ap->secure = wpa || rsn || wep || sae;
	if (!ap->secure)
		snprintf(ap->sec, sizeof ap->sec, "open");
	else if (wep && !wpa && !rsn)
		snprintf(ap->sec, sizeof ap->sec, "WEP");
	else if (eap)
		snprintf(ap->sec, sizeof ap->sec, "WPA-EAP");
	else if (sae && !psk)
		snprintf(ap->sec, sizeof ap->sec, "WPA3");
	else if (sae)
		snprintf(ap->sec, sizeof ap->sec, "WPA2/3");
	else if (wpa && rsn)
		snprintf(ap->sec, sizeof ap->sec, "WPA/WPA2");
	else if (rsn)
		snprintf(ap->sec, sizeof ap->sec, "WPA2");
	else
		snprintf(ap->sec, sizeof ap->sec, "WPA");
	/* this build joins open, WEP-less WPA/WPA2 personal networks; WPA3-only and enterprise need more */
	ap->unsupported = (sae && !psk) || eap || owe || (wep && !wpa && !rsn);
	if (owe && !ap->secure)
		snprintf(ap->sec, sizeof ap->sec, "OWE");
}

static int ap_cmp(const void *a, const void *b)
{
	return ((const struct x45_wifi_ap *)b)->rssi - ((const struct x45_wifi_ap *)a)->rssi;
}

int x45_wifi_networks(struct x45_wifi *w, struct x45_wifi_net *out, int max)
{
	char reply[REPLY_MAX], *line, *save;
	int n = 0, r = x45_wifi_cmd(w, "LIST_NETWORKS", reply, sizeof reply);

	if (r < 0)
		return r;
	for (line = strtok_r(reply, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		char *f[4], *p = line, key[48], km[48];
		int i;
		struct x45_wifi_net *nw;

		if (!isdigit((unsigned char)*line))
			continue;   /* the header row */
		for (i = 0; i < 4; i++) {   /* id \t ssid \t bssid \t flags */
			f[i] = p;
			p = strchr(p, '\t');
			if (!p) {
				i++;
				break;
			}
			*p++ = '\0';
		}
		if (i < 3 || n >= max)
			continue;
		nw = &out[n];
		memset(nw, 0, sizeof *nw);
		nw->id = atoi(f[0]);
		ssid_decode(f[1], nw->ssid, sizeof nw->ssid);
		nw->current = i > 3 && strstr(f[3], "[CURRENT]") != NULL;
		nw->auto_join = !(i > 3 && strstr(f[3], "[DISABLED]"));
		/* open or keyed: the key itself is never read back */
		snprintf(key, sizeof key, "GET_NETWORK %d key_mgmt", nw->id);
		nw->open = x45_wifi_cmd(w, key, km, sizeof km) > 0 && !strcmp(km, "NONE");
		n++;
	}
	return n;
}

int x45_wifi_aps(struct x45_wifi *w, struct x45_wifi_ap *out, int max)
{
	struct x45_wifi_net nets[16];
	struct x45_wifi_ap *all;
	char reply[REPLY_MAX], cmd[48], v[256];
	int i, k, n = 0, nn = x45_wifi_networks(w, nets, 16);

	if (nn < 0)
		return nn;
	all = calloc(AP_MAX, sizeof *all);
	if (!all)
		return -ENOMEM;
	for (i = 0; i < AP_MAX; i++) {
		struct x45_wifi_ap ap;
		int r;

		snprintf(cmd, sizeof cmd, "BSS %d MASK=0x1a86", i);
		r = x45_wifi_cmd(w, cmd, reply, sizeof reply);
		if (r < 0) {
			free(all);
			return r;
		}
		if (r == 0)
			break;
		memset(&ap, 0, sizeof ap);
		ap.saved = -1;
		if (kv(reply, "age", v, sizeof v) && atoi(v) > 60)
			continue;   /* not heard in the last minute */
		if (!kv(reply, "ssid", v, sizeof v))
			continue;
		ssid_decode(v, ap.ssid, sizeof ap.ssid);
		if (!ap.ssid[0] || ap.ssid[0] == '\n')
			continue;   /* hidden */
		kv(reply, "bssid", ap.bssid, sizeof ap.bssid);
		if (kv(reply, "freq", v, sizeof v))
			ap.freq = atoi(v);
		if (kv(reply, "level", v, sizeof v))
			ap.rssi = atoi(v);
		kv(reply, "flags", v, sizeof v);
		sec_from_flags(v, &ap);
		/* one row per name: keep the strongest access point */
		for (k = 0; k < n; k++)
			if (!strcmp(all[k].ssid, ap.ssid))
				break;
		if (k < n) {
			if (ap.rssi > all[k].rssi)
				all[k] = ap;
		} else {
			all[n++] = ap;
		}
	}
	for (k = 0; k < n; k++)
		for (i = 0; i < nn; i++)
			if (!strcmp(all[k].ssid, nets[i].ssid))
				all[k].saved = nets[i].id;
	qsort(all, n, sizeof *all, ap_cmp);
	if (n > max)
		n = max;
	memcpy(out, all, n * sizeof *out);
	free(all);
	return n;
}

/* ------------------------------------------------------------------- actions --- */

static int ok_reply(struct x45_wifi *w, const char *cmd)
{
	char reply[256];
	int r = x45_wifi_cmd(w, cmd, reply, sizeof reply);

	if (r < 0)
		return r;
	return strncmp(reply, "OK", 2) ? -EINVAL : 0;
}

static int save(struct x45_wifi *w)
{
	return ok_reply(w, "SAVE_CONFIG");
}

/* a priority above every saved network's, so that the one chosen last is the one joined first */
static int top_priority(struct x45_wifi *w)
{
	struct x45_wifi_net nets[16];
	char cmd[64], v[32];
	int i, n = x45_wifi_networks(w, nets, 16), top = 0;

	for (i = 0; i < n; i++) {
		snprintf(cmd, sizeof cmd, "GET_NETWORK %d priority", nets[i].id);
		if (x45_wifi_cmd(w, cmd, v, sizeof v) > 0 && atoi(v) > top)
			top = atoi(v);
	}
	return top + 1;
}

int x45_wifi_join(struct x45_wifi *w, int id)
{
	char cmd[96];
	int r;

	snprintf(cmd, sizeof cmd, "SET_NETWORK %d priority %d", id, top_priority(w));
	if ((r = ok_reply(w, cmd)) < 0)
		return r;
	snprintf(cmd, sizeof cmd, "ENABLE_NETWORK %d", id);
	if ((r = ok_reply(w, cmd)) < 0)
		return r;
	save(w);
	/* REASSOCIATE leaves the current network and picks the best one again: the one just prioritised */
	return ok_reply(w, "REASSOCIATE");
}

int x45_wifi_connect(struct x45_wifi *w, const char *ssid, const char *psk, int hidden)
{
	struct x45_wifi_net nets[16];
	char cmd[256], reply[64], hex[2 * X45_WIFI_SSID + 2];
	int i, n, id = -1, r, open = !psk || !*psk, plen = open ? 0 : (int)strlen(psk);

	if (!ssid || !*ssid || strlen(ssid) > 32)
		return -EINVAL;
	if (!open) {
		int hexkey = plen == 64;

		for (i = 0; hexkey && i < 64; i++)
			if (!isxdigit((unsigned char)psk[i]))
				hexkey = 0;
		if (!hexkey && (plen < 8 || plen > 63 || strchr(psk, '\n') || strchr(psk, '\r')))
			return -EINVAL;
	}
	n = x45_wifi_networks(w, nets, 16);
	if (n < 0)
		return n;
	for (i = 0; i < n; i++)
		if (!strcmp(nets[i].ssid, ssid))
			id = nets[i].id;
	if (id < 0) {
		r = x45_wifi_cmd(w, "ADD_NETWORK", reply, sizeof reply);
		if (r < 0)
			return r;
		id = atoi(reply);
		hex_encode(ssid, hex);
		snprintf(cmd, sizeof cmd, "SET_NETWORK %d ssid %s", id, hex);
		if ((r = ok_reply(w, cmd)) < 0)
			goto fail;
	}
	if (open) {
		snprintf(cmd, sizeof cmd, "SET_NETWORK %d key_mgmt NONE", id);
	} else if (plen == 64) {
		snprintf(cmd, sizeof cmd, "SET_NETWORK %d key_mgmt WPA-PSK", id);
		if ((r = ok_reply(w, cmd)) < 0)
			goto fail;
		snprintf(cmd, sizeof cmd, "SET_NETWORK %d psk %s", id, psk);
	} else {
		snprintf(cmd, sizeof cmd, "SET_NETWORK %d key_mgmt WPA-PSK", id);
		if ((r = ok_reply(w, cmd)) < 0)
			goto fail;
		snprintf(cmd, sizeof cmd, "SET_NETWORK %d psk \"%s\"", id, psk);
	}
	if ((r = ok_reply(w, cmd)) < 0)
		goto fail;
	snprintf(cmd, sizeof cmd, "SET_NETWORK %d scan_ssid %d", id, hidden ? 1 : 0);
	ok_reply(w, cmd);
	if ((r = x45_wifi_join(w, id)) < 0)
		goto fail;
	return id;
fail:
	if (n >= 0) {
		int known = 0;

		for (i = 0; i < n; i++)
			known |= nets[i].id == id;
		if (!known) {   /* half-made: do not leave it behind */
			snprintf(cmd, sizeof cmd, "REMOVE_NETWORK %d", id);
			ok_reply(w, cmd);
			save(w);   /* x45_wifi_join may have written it to the file already */
		}
	}
	return r;
}

int x45_wifi_forget(struct x45_wifi *w, int id)
{
	char cmd[48];
	int r;

	snprintf(cmd, sizeof cmd, "REMOVE_NETWORK %d", id);
	if ((r = ok_reply(w, cmd)) < 0)
		return r;
	return save(w);
}

int x45_wifi_set_auto(struct x45_wifi *w, int id, int on)
{
	char cmd[48];
	int r;

	snprintf(cmd, sizeof cmd, "%s_NETWORK %d", on ? "ENABLE" : "DISABLE", id);
	if ((r = ok_reply(w, cmd)) < 0)
		return r;
	return save(w);
}

int x45_wifi_disconnect(struct x45_wifi *w)
{
	return ok_reply(w, "DISCONNECT");
}

int x45_wifi_reconnect(struct x45_wifi *w)
{
	return ok_reply(w, "RECONNECT");
}

void x45_wifi_mark(const struct x45_wifi *w, struct x45_wifi_mark *m)
{
	m->conn = w->conn_seq;
	m->fail = w->fail_seq;
	m->notfound = w->notfound_seq;
}

int x45_wifi_wait(struct x45_wifi *w, const struct x45_wifi_mark *m, const char *ssid, int ms, char *why, int n)
{
	struct timespec t0, t;

	clock_gettime(CLOCK_MONOTONIC, &t0);
	if (why && n > 0)
		why[0] = '\0';
	for (;;) {
		struct x45_wifi_status st;
		struct pollfd p = { .fd = w->mon, .events = POLLIN };
		long used;

		x45_wifi_pump(w);
		if (w->fail_seq != m->fail && (!strcmp(w->fail, "WRONG_KEY") || !strcmp(w->fail, "AUTH_FAILED") ||
					       !strcmp(w->fail, "CONN_FAILED"))) {
			if (why && n > 0)
				snprintf(why, (size_t)n, "%s", x45_wifi_fail_text(w->fail));
			return -1;
		}
		if (w->notfound_seq != m->notfound) {
			if (why && n > 0)
				snprintf(why, (size_t)n, "not in range");
			return -1;
		}
		if (w->conn_seq != m->conn && x45_wifi_status(w, &st) == 0 && st.state == X45_WIFI_CONNECTED &&
		    !strcmp(st.ssid, ssid))
			return 1;
		clock_gettime(CLOCK_MONOTONIC, &t);
		used = (t.tv_sec - t0.tv_sec) * 1000L + (t.tv_nsec - t0.tv_nsec) / 1000000L;
		if (used >= ms)
			return 0;
		if (w->mon >= 0)
			poll(&p, 1, 300);
		else
			usleep(300000);
	}
}

int x45_wifi_power(int on)
{
	pid_t p = fork();

	if (p < 0)
		return -errno;
	if (p == 0) {
		int nul = open("/dev/null", O_RDWR);

		setsid();
		if (nul >= 0) {
			dup2(nul, 0);
			dup2(nul, 1);
			dup2(nul, 2);
		}
		/* a grandchild, so that nobody has to wait for it */
		if (fork() == 0)
			execl(X45_WIFI_RUN, "x45wifi-run", on ? "on" : "off", (char *)NULL);
		_exit(0);
	}
	{
		int st;

		while (waitpid(p, &st, 0) < 0 && errno == EINTR)   /* the intermediate child */
			;
	}
	return 0;
}

const char *x45_wifi_fail_text(const char *reason)
{
	if (!strcmp(reason, "WRONG_KEY"))
		return "wrong password";
	if (!strcmp(reason, "AUTH_FAILED"))
		return "authentication failed";
	if (!strcmp(reason, "ASSOC_REJECT") || !strcmp(reason, "AUTH_REJECT"))
		return "the router refused us";
	if (!strcmp(reason, "CONN_FAILED"))
		return "could not connect";
	return reason[0] ? "could not connect" : "";
}
