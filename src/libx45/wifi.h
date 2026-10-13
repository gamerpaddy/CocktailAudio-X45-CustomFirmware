/* wifi - client for the WiFi manager of the USB radio stick.
 *
 * The manager itself is wpa_supplicant (started by deploy/x45wifi-run, which also does DHCP through a
 * wpa_cli hook). This library speaks its control socket, so the panel, the web server and the x45wifi
 * command line all see the same networks and the same state, and nothing else keeps a copy of it:
 *
 *   saved networks   wpa_supplicant's own config (wifi/state/wpa.conf, written by SAVE_CONFIG)
 *   autoconnect      a saved network that is enabled is joined whenever it is in range
 *   connect now      the chosen network is given the top priority and the link is re-associated
 *   forget           the network is removed from the config
 *
 * Every call that talks to the socket returns 0 / a count, or -errno; -ENOENT / -ECONNREFUSED mean
 * "wpa_supplicant is not running" (WiFi is off or still starting; x45_wifi_status then reads the
 * progress file of x45wifi-run). No call blocks for longer than about a second.
 */
#ifndef X45_WIFI_H
#define X45_WIFI_H

#ifdef __cplusplus
extern "C" {
#endif

#define X45_WIFI_SSID 40
#define X45_WIFI_CTL_DIR "/var/run/x45wifi"
#define X45_WIFI_STATE_DIR "/home/root/x45new/wifi/state"
#define X45_WIFI_RUN "/home/root/x45new/bin/x45wifi-run"

enum x45_wifi_state {
	X45_WIFI_OFF,         /* switched off, or the manager is not running */
	X45_WIFI_STARTING,    /* x45wifi-run is bringing the stick and the driver up */
	X45_WIFI_ERROR,       /* it gave up: see .error */
	X45_WIFI_IDLE,        /* up, not joined to anything (no saved network in range, or none saved) */
	X45_WIFI_SCANNING,
	X45_WIFI_CONNECTING,  /* authenticating / associating / handshake / waiting for an address */
	X45_WIFI_CONNECTED,
};

struct x45_wifi_status {
	enum x45_wifi_state state;
	int enabled;                     /* the persistent switch (wifi/state/enabled) */
	char step[48], error[64];        /* progress / failure text while starting */
	char wpa_state[24];              /* wpa_supplicant's own word: COMPLETED, SCANNING, ... */
	char ssid[X45_WIFI_SSID];
	char bssid[18];
	char ip[16];                     /* empty until DHCP has answered */
	int id;                          /* saved network in use, -1 none */
	int freq;                        /* MHz */
	int rssi;                        /* dBm, 0 unknown */
};

struct x45_wifi_ap {
	char ssid[X45_WIFI_SSID];
	char bssid[18];                  /* of the strongest access point of this name */
	int freq, rssi;
	int secure;                      /* needs a key */
	int unsupported;                 /* WPA3-only (SAE without PSK), EAP: this build cannot join it */
	int saved;                       /* id of the saved network with this name, -1 none */
	char sec[12];                    /* "open", "WEP", "WPA2", "WPA/WPA2", "WPA3", ... */
};

struct x45_wifi_net {
	int id;
	char ssid[X45_WIFI_SSID];
	int current;                     /* joined right now */
	int auto_join;                   /* enabled: joined when in range */
	int open;                        /* no key */
};

struct x45_wifi {
	int req, mon;                    /* control sockets, -1 when closed */
	char req_path[64], mon_path[64];
	/* learned from the event socket by x45_wifi_pump() */
	unsigned scan_seq;               /* + 1 for every CTRL-EVENT-SCAN-RESULTS */
	unsigned conn_seq;               /* + 1 for every connection made */
	unsigned lost_seq;               /* + 1 for every connection lost */
	char fail[24];                   /* WRONG_KEY, AUTH_FAILED, ... of the last failed attempt; "" none */
	int fail_id;
	unsigned fail_seq;
	unsigned notfound_seq;           /* + 1 when a selected network was not in range */
	char last_event[96];
};

/* events != 0 also attaches to the event stream (the panel does; one-shot callers need not) */
int x45_wifi_open(struct x45_wifi *w, int events);
void x45_wifi_close(struct x45_wifi *w);
/* poll() this to wake up for events; -1 when closed */
int x45_wifi_fd(const struct x45_wifi *w);
/* read what the event socket holds (never blocks); returns the number of events seen */
int x45_wifi_pump(struct x45_wifi *w);

/* one control command and its reply (NUL-terminated, trailing newline cut) */
int x45_wifi_cmd(struct x45_wifi *w, const char *cmd, char *reply, int n);

int x45_wifi_status(struct x45_wifi *w, struct x45_wifi_status *st);   /* works with the socket closed too */
int x45_wifi_scan(struct x45_wifi *w);                                 /* start a scan; results come as an event */
/* what the last scans saw (seen in the last minute), strongest first, one row per name */
int x45_wifi_aps(struct x45_wifi *w, struct x45_wifi_ap *out, int max);
int x45_wifi_networks(struct x45_wifi *w, struct x45_wifi_net *out, int max);

/* save a network and join it now. psk: NULL / "" for an open one, 8..63 characters or 64 hex digits
 * otherwise. An existing network with this name has its key replaced. Returns the network id. */
int x45_wifi_connect(struct x45_wifi *w, const char *ssid, const char *psk, int hidden);
int x45_wifi_join(struct x45_wifi *w, int id);              /* join a saved network now */
int x45_wifi_forget(struct x45_wifi *w, int id);
int x45_wifi_set_auto(struct x45_wifi *w, int id, int on);  /* autoconnect on/off (kept in the config) */
int x45_wifi_disconnect(struct x45_wifi *w);                /* leave, and do not rejoin by itself */
int x45_wifi_reconnect(struct x45_wifi *w);                 /* rejoin the best saved network */

/* Waiting for the outcome of a connect/join, for callers that live for one request (the web server, the
 * command line). Open the handle with events, x45_wifi_mark() it, ask for the connection, then wait:
 * 1 joined and holding an address, 0 still trying after `ms`, -1 refused or not in range (`why` says how). */
struct x45_wifi_mark {
	unsigned conn, fail, notfound;
};
void x45_wifi_mark(const struct x45_wifi *w, struct x45_wifi_mark *m);
int x45_wifi_wait(struct x45_wifi *w, const struct x45_wifi_mark *m, const char *ssid, int ms, char *why, int n);

/* switch WiFi on/off persistently; runs x45wifi-run in the background (on takes up to ~30 s) */
int x45_wifi_power(int on);

/* the text for a failure reason ("WRONG_KEY" -> "wrong password") */
const char *x45_wifi_fail_text(const char *reason);

#ifdef __cplusplus
}
#endif
#endif /* X45_WIFI_H */
