/* x45ui - the state of the front-panel UI and the services main.c offers to the screens in ext/.
 *
 * main.c holds the built-in screens (player, library, radio, CD, tuner, inputs, options). A new screen
 * does not go into main.c: it is a file in ext/ that fills a struct ui_ext (ext/ext.h) and is listed in
 * ext/ext.c. Everything such a screen may use of the UI is declared here.
 */
#ifndef X45UI_H
#define X45UI_H

#include "../libx45/gfx.h"
#include "../libx45/image.h"
#include "../libx45/mpdc.h"
#include "../libx45/stations.h"
#include "../libx45/x45.h"
#include "../libx45/x45in.h"

#include "vis/mem.h"

#include <pthread.h>
#include <stdint.h>
#include <sys/types.h>

struct ui_ext;

enum screen { SCR_PLAYER = 0, SCR_BROWSE, SCR_RADIO, SCR_CD, SCR_TUNER, SCR_MAIN, SCR_INPUTS, SCR_OPTIONS, SCR_VISUALS, SCR_VISSUB, SCR_CONFIRM, SCR_OPTCAT, SCR_EXT, SCR_COUNT };

#define WF_ROWS 160   /* waterfall history rows */

/* the visuals that are on (vis/vis.h): their bits, at most two, and which of the two was switched on first */
struct vis_sel { unsigned short on, first; };

struct theme {
	uint32_t bg, panel, line, text, dim, accent, warn, barfg, barbg;
	uint32_t card;      /* a surface between bg and panel (tiles, cards) */
};

struct ui {
	struct x45_gfx g;
	struct x45_indev in;
	struct theme t;
	int x45fd;

	enum screen screen;
	int sel;          /* highlighted row on list screens */
	int volume;
	int balance;      /* -X45_BALANCE_MAX..+X45_BALANCE_MAX, positive = right louder (see x45_balance_levels) */
	int bal_live;     /* the DAC may hold unequal left/right levels: the next apply_volume() sends them (equal ones too) */
	int bal_var;      /* volume_mode is fixed, and the analogue stage is variable anyway for as long as the balance is off centre */
	int muted;
	int input;
	int out_lineout, out_xlr, out_hp;
	int hp_seen, spk_volume;
	int dirty;

	/* live hardware readings, refreshed once a second */
	int32_t hp_present, pcusb, cs_rate;
	char status[160];

	/* MPD */
	struct mpdc mpd;
	int mpd_port;
	int vol_fixed;          /* config: line-out volume fixed (preamp use) */
	struct vis_sel vis[12]; /* the panel visuals that are on per input (vis/vis.h): at most two */
	int trim[12];           /* NJU72341 gain index 0..21 per input (12 = 0 dB); only the analogue ones count */
	int tuner_off;          /* config override: no DAB/FM module fitted (never set from the UI) */
	int cfg_vol, cfg_input; /* restored at start-up: volume, last input (-1 none) */
	int vol_dirty, usbpc_pc;/* volume to save once it settles; XMOS currently handed to the PC */
	long cd_watch_t;        /* next look for a disc on the CD input */
	pid_t cd_probe_pid;     /* `x45cd id` running to see whether the disc is still the one listed */
	int cd_probe_fd, cd_probe_len;
	long cd_probe_t0;
	char cd_probe_buf[40];
	int cd_sig_n, cd_sig_total;   /* "<tracks> <frames>" of the disc that was listed */
	int cd_dvd;             /* the listed disc is a video DVD whose PCM sound plays as chapters */
	char cd_note[24];       /* a video DVD whose sound cannot be played: its coding ("Dolby Digital", "DTS") */
	int cd_rate, cd_bits;   /* of the DVD sound: Hz, bits */
	int tu_listrev;         /* x45tuner list revision last loaded */
	struct mpdc_status st;
	struct mpdc_song song;
	int have_song;
	long last_poll, last_try, last_vol_change, vol_t;
	long last_song;         /* last currentsong read, for stream title updates */
	struct mpdc_entry net_cache[32];   /* the media servers found, kept: going back must not search again */
	int net_cache_n, net_force;       /* net_force: search again, whatever is cached */
	int net_more;                     /* the server folder just listed holds more entries than were fetched */
	long in_sw_t, start_t;  /* when the input last changed, when the UI started */
	/* MPD's state is read by a thread of its own: while MPD is busy (a UPnP server that opens a stream slowly
	 * keeps it from answering for seconds) the panel keeps running on the last state it was given */
	pthread_t p_thr;
	pthread_mutex_t p_mx;
	int p_started, p_alive, p_valid, p_stop;
	struct { char url[MPDC_PATH_MAX]; char name[96]; } *ctx;   /* the stations of the folder the playing one came from */
	int ctx_n;
	char play_url[512], play_name[96];   /* the station chosen last: MPD has no name for it until the stream connects */
	volatile int p_refresh;   /* the owner just changed what plays: the poller reads the song again at once */
	long song_hold;           /* until then the snapshots are older than that change: ignored */
	unsigned p_seq, p_seen;
	struct mpdc_status p_st;
	struct mpdc_song p_song;
	int mpd_alive;          /* the last look at MPD got an answer */
	long vol_again_t;       /* send the volume once more at this time */
	int tu_retries;         /* automatic restarts of a tuner that did not come up */
	int tu_unclean;         /* the last tuner process was cut off before it was up */
	int scrolling;           /* the frame just drawn has a text that runs: keep redrawing */
	long stop_down;         /* time STOP went down, 0 when up */
	int stop_fired;         /* the hold already opened the delete question */
	int cf_act;             /* what the yes/no question is about (CF_*) */
	int cf_ret;             /* where it returns to */
	char cf_what[160];      /* the question, shown big */
	char cf_path[MPDC_PATH_MAX];   /* what to delete, relative to the library */
	int opt_cat;            /* the options group that is open */
	int look_from_menu;     /* it was opened from the RETURN menu of the player (Visualizer look): RETURN goes back there */
	int in_icons;           /* the top bar names the input with its icon (when there is one) */
	struct x45_icon icons[40];   /* loaded on first use, from ICON_DIR */
	char icon_names[40][24];
	int nicons;
	long menu_down;         /* time MENU went down, 0 when up */
	int menu_fired;         /* the hold already did its job */
	float vol_a;            /* volume overlay opacity 0..255 */
	int air;                /* an AirPlay session is live (marker file present) */
	long power_armed;       /* time of the first POWER press, 0 if none */
	int ir_code;            /* the remote button that went down last (debounce of the list keys) */
	long ir_down;           /* when it went down */
	long ir_step;           /* when a held list key last moved the cursor */
	int ir_bounce;          /* the press being handled repeats the previous one of the same button too soon */
	/* the output is muted in hardware while a stream starts and its clock settles (settle_* in main.c) */
	volatile long settle_until;   /* mute until then, 0 when not */
	long settle_t0;               /* when it began */
	int settle_open;              /* the stream is up: settle_until is the end of the half second after it */
	int quit_code;          /* process exit status: 3 = shut the unit down */
	int seen_pos;           /* queue position the cached song belongs to */
	char seen_state[8];
	int lib_updating;       /* MPD was updating its database at the last look */
	long lib_reload_t;      /* load the library list again at this time (0: none): a rescan was asked for */

	/* library browser */
	char dir[MPDC_PATH_MAX];
	struct mpdc_entry *rows;
	int nrows, truncated, top, bsel;   /* bsel: the browser has its own cursor */
	char dir_err[160];
	/* network folder: UPnP/DLNA servers browsed through x45upnp. Their rows carry the whole
	 * address in .path ("@u|<control url>|<object id>" folders, "@f|<url>" tracks); the stack
	 * remembers where each folder was entered from, for RETURN. */
	char net_stack[16][MPDC_PATH_MAX];
	char net_names[16][64];
	int net_depth;
	char net_busy[48];      /* "searching..." shown while x45upnp runs */
	char net_title[64];     /* name of the folder being shown */

	/* recording to disk (x45rec): rec_pid > 0 while it runs */
	pid_t rec_pid;
	int rec_cd;             /* ripping the disc rather than capturing an input */
	long rec_note_t;        /* the large "recording started" card is up until REC_NOTE_MS after this (0: none) */
	long rec_poll;
	char rec_state[12];
	long rec_secs;
	int rec_track, rec_tracks;

	/* analyzer feed for the web meters when the panel meter is not running */
	pid_t sp_pid;
	int sp_mode;            /* 0 none, 1 radio capture, 2 MPD fifo (network), 3 the SSI capture at 192 kHz */
	unsigned sp_rate;
	long sp_retry;
	int sp_fails;           /* SSI captures in a row that died without ever writing a frame */
	time_t sp_spawn;        /* when the running analyzer was started */
	int sp_ok;              /* the SSI capture has been seen writing */
	const char *sp_why;     /* why there is no analyzer, for the web page */
	/* debug: the load of the unit, sampled once a second while the status page is up (sys_poll) */
	long sys_t;             /* when it was last sampled, 0: not yet */
	int sys_cpu;            /* busy share of all cores in tenths of a percent, -1 until there are two samples */
	int sys_cores;
	long sys_mem_total_kb, sys_mem_free_kb, sys_mem_cache_kb;
	struct { char name[16]; int pct; } sys_top[3];   /* the busiest processes, percent of one core */
	/* the analyzer state as the panel draws it (read back from METER_FILE) */
	float sp_b[128];       /* band levels, dBFS */
	float sp_l[2];          /* channel peak, dBFS */
	float sp_r[2];          /* channel RMS, dBFS */
	float sp_pkh[2];        /* peak-hold marker, dBFS */
	float vu_n[2];          /* analog VU needle position, dB (smoothed) */
	int dirty_vis;          /* only the visuals strip needs drawing again */
	int vf_ok;              /* the last full frame left a strip that can be redrawn alone */
	int vf_x, vf_y, vf_w, vf_h;
	uint32_t *vf_bg;        /* what was behind the strip */
	uint32_t *vf_bg2;       /* the same with the dials' scales drawn: they never change */
	int vf_dials;           /* vf_bg2 is valid */
	int dial_pass;          /* 0 draw all, 1 only what a dial never changes, 2 only what moves */
	long vu_t;              /* when it was last advanced */
	long sp_pkt[2];
	unsigned sp_seq;
	int sp_have;
	long sp_read_t;

	/* level meter for the external inputs (capture via arecord on hw:1,0) */
	pid_t m_pid;
	int m_fd;
	long m_retry, m_last, m_sig_t, m_clip_t, m_pk_t[2];
	float m_lvl[2], m_pk[2];     /* dBFS */
	unsigned char m_carry[4];
	int m_ncarry;

	/* internet radio presets (etc/stations.txt) */
	struct x45_station stations[64];
	int nstations, rsel, rtop, stations_skipped;
	char stations_path[160];
	char stations_err[224];

	/* built-in CD drive (x45cd child) */
	pid_t cd_pid;
	int cd_ntracks, cd_sel, cd_top, cd_track, cd_pos, cd_tlen, cd_playing;
	int cd_hdcd, cd_hdcd_pe, cd_hdcd_gain, cd_hdcd_on;   /* x45cd status: HDCD seen (0/1/2), peak extend 0-2, gain in 0.5 dB, codes arriving */
	int cd_len[100];          /* seconds, -1 for a data track */
	long cd_lastpoll;
	long cd_skip_t;           /* a skip was asked for at this time: the old status file is not believed for a moment */
	char cd_err[64];
	enum screen ret_screen;   /* where the input list returns to */
	char cd_title[100][40];   /* CD-Text, empty when the disc has none */
	char cd_album[64];        /* "album - artist" */

	/* DAB+/FM module (x45tuner child) */
	pid_t tu_pid;
	int tu_mode;                 /* 0 fm, 1 dab, of the running child */
	long tu_lastpoll;
	char tu_state[12], tu_ps[16], tu_svc[24], tu_ens[24], tu_text[140];
	int tu_npreset, tu_freq, tu_rssi, tu_snr, tu_found, tu_nsvc, tu_sel, tu_q, tu_sq, tu_scan, tu_fmscan, tu_up;
	char tu_rt[72], tu_time[40];   /* RDS radiotext, ensemble clock */
	int tu_slide, slide_seen;      /* x45tuner's picture counter, and the one loaded */
	struct x45_image slide;        /* current DAB slideshow picture */
	int cd_loading;              /* the disc is being read (shown instead of a frozen screen) */
	int bl_edit;                 /* options: the knob is adjusting the brightness */
	long in_t;                   /* input list: last activity; the highlighted input is chosen 3 s later */
	int rb;                      /* knob "rubber band": clicks stretched towards next (+) / previous (-) */
	long rb_t;                   /* time of the last stretching click */
	int bright;                  /* backlight level 2..bl_max, 0 = leave as found */
	int tl_n, tl_sel, tl_top, tl_val[128];
	char tl_name[128][20];

	/* Bluetooth input (x45bt child; the radio is only on while this input is) */
	pid_t bt_pid;
	int bt_wfd;                  /* write end of its stdin: one letter per command */
	long bt_lastpoll, bt_started;
	char bt_state[12];           /* starting ready connected playing pairing */
	char bt_name[64], bt_confirm[12], bt_confirm_name[64];
	char bt_title[96], bt_artist[96], bt_album[96];
	int bt_bonds, bt_open, bt_rate, bt_avrcp_play;
	char bt_step[40];
	unsigned bt_seq;

	/* options */
	int vis_all;                /* 1: one visualizer setting for every input */
	struct vis_sel vis_shared;  /* that setting */
	int bands;                  /* analyzer bands shown, 16..128 */
	int spec_col;               /* the colours of the bars: 0 one colour .. 5 rainbow (vis/vis.h, vis_bar_colour) */
	int spec_style;             /* the spectrum's style: 0 bars, 1 LED cells, 2 mirrored, 3 curve (vis/spectrum.c) */
	int gon_look;               /* the stereo field: 0 the Marantz scope (round, phosphor), 1 plain (vis/gonio.c) */
	int gon_col;                /* the colour of its trace: 0 the look's own, then vis_gonio_colour_name() */
	int gon_nobars;             /* 1: its correlation and balance bars are left out */
	int gon_big;                /* 1: with the bars left out, the stereo field fills the screen and the text is a small column beside it
	                             * (vis/scopepage.c; vis_big() says when it is in force) */
	int theme_id;               /* the accent colour (skins/colours.c) */
	int ground_id;              /* the background: 0 the skin's own, then the tones of skins/colours.c */
	int ink_id;                 /* the text colour: 0 the skin's own, then the tones of skins/colours.c */
	int skin_id;                /* the look: palette and layouts (skins/) */
	int in_auto_s;              /* input list: seconds before the cursor's input is chosen, 0 = only with OK */
	int rb_steps;               /* knob clicks that stretch the rubber band, 1 = every click skips */
	int scr_off_s;              /* screen off after this many idle seconds, 0 = never */
	int scr_dark;               /* the backlight is down */
	long act_t;                 /* last activity */
	int resume;                 /* come back on the last input and radio station at start-up */
	int rip_fmt;                /* file format of a CD rip: 0 FLAC, 1 MP3 320 kbps, 2 MP3 V0 (rip_fmt_arg) */
	char resume_url[500];       /* its stream address */
	char resume_name[96];       /* the internet radio station to put back on */
	long resume_t;
	/* what the network input was doing when another input was chosen: chosen again, it picks the music up there */
	struct {
		int valid;              /* the input was left at least once since start-up */
		int was;                /* 0 nothing playing, 1 playing, 2 paused */
		int pos, dur, elapsed;  /* queue position, length and position of the song, seconds */
		char file[MPDC_PATH_MAX];
		char name[96];
	} net;
	char last_url[500];         /* the internet radio station played last (what to bring back when nothing else is known) */
	char last_name[96];
	long net_t;                 /* when to put the music back (0 = nothing pending) */
	long net_end;               /* the follow-up (seek, pause again) is given up at this time */
	int net_seek, net_pause;    /* after the song has started: jump to this second, pause again */
	float sp_c;                 /* correlation of the channels, -1 (opposite) .. 1 (the same) */
	short sp_wl[200], sp_wr[200];   /* 25 ms of each channel, hundredths of a percent of full scale (vis/scope.c, gonio.c) */
	int sp_wave;                /* sp_wl/sp_wr hold a picture */
	float sp_pkb[128];          /* held peak per displayed band, 0..1 */
	long sp_pk_t;
	int sp_pk_nb;
	unsigned char wf[WF_ROWS][128];   /* waterfall history, newest row at wf_head - 1 */
	int wf_head, wf_n;
	float sp_sb[128];           /* the bands of the side signal (L - R) / 2, dBFS (x45loop's "sb"; vis/width.c) */
	int sp_side;                /* sp_sb holds a picture: the analyzer is new enough to write it */
	unsigned sp_frate;          /* the sample rate the analyzer reports, which lays out the bands (0: not known, 44100) */
	struct vis_mem vm;          /* what the history visuals keep (vis/mem.h) */
	char bt_err[64];

	/* the screen of ext/ that is open while screen == SCR_EXT, and where RETURN leaves it to */
	const struct ui_ext *ext;
	enum screen ext_ret;
};

/* ---- services of main.c for the screens in ext/ ---- */
void say(struct ui *u, const char *fmt, ...) __attribute__((format(printf, 2, 3)));   /* a line under the content */
void enter_screen(struct ui *u, enum screen s);
void draw_hints(struct ui *u, const char *spec);          /* "KEY|what;KEY|what" along the foot */
/* a list of rows with optional values (right aligned) and icons; the highlighted row is u->sel */
void draw_rows(struct ui *u, const char *const *rows, const char *const *vals, const char *const *icons,
	       int n, int current, const char *hint);
void save_config_key(const char *key, const char *val);
/* the balance (main.c): clamped to the range, sent to the DAC at once; `save` keeps it in the configuration (a screen
 * that is stepped through saves once, when it is left, not on every click) */
void ui_set_balance(struct ui *u, int balance, int save);
int ui_volume_locked(const struct ui *u);   /* fixed outputs: the level is not the owner's to change (the balance still works) */

/* the station search (ext/search.c): what the current input knows by name, and playing one of it */
enum { STN_LIST, STN_FAV, STN_ROUTER, STN_DAB, STN_FM, STN_WEB };   /* STN_LIST: the radio list the search was opened from */
struct ui_stn {
	char name[72], url[256], info[28];   /* info: "MP3 128 DE" of a directory station, else empty */
	int kind;           /* STN_* */
	int val;            /* the tuner's number of it (DAB service, FM frequency in 10 kHz) */
};
int stations_collect(struct ui *u, struct ui_stn *out, int max);   /* favourites + router stations (network), the DAB or FM list */
void station_play(struct ui *u, const struct ui_stn *s);           /* plays / tunes it and shows the playing screen */
int stations_searchable(const struct ui *u);                       /* the current input has stations to search */

/* ---- drawing services for the skins (skins/) ---- */
/* UTF-8 text folded to what the font has: text_fold cuts it to max_w, text_scroll lets it run left instead
 * (slot 0..5 tells the running texts of one screen apart) */
void text_fold(struct x45_surface *s, int x, int y, const char *utf8, int scale, int max_w, uint32_t col);
void text_scroll(struct ui *u, int slot, int x, int y, const char *utf8, int scale, int max_w, uint32_t col);
const struct x45_icon *icon_get(struct ui *u, const char *name);   /* ICON_DIR/NAME.png, NULL when there is none */
/* The level bars / dials / spectrum / waterfall the owner switched on, laid out in the rectangle. Draw it last on a page:
 * a frame in which only the visuals move is made by putting back what was behind the rectangle (the full rows). */
void draw_visuals_rect(struct ui *u, int x, int y, int w, int h);   /* (vis/vis.c) */
int meter_wanted(const struct ui *u);   /* the input has the panel's own level meter page instead (external inputs) */

#endif /* X45UI_H */
