#define _GNU_SOURCE
/* ext/path - the signal path, read only.
 *
 * Shows what the sound goes through between the source and the output right now, from the state the unit already
 * holds: the input, what in software touches the signal (nothing, for the inputs that play in hardware), the format that
 * arrives, the level stage and the outputs. The last lines say whether the path is bit-perfect, and when it is not, why.
 * Nothing on this screen changes anything; the blue key on the remote opens it, the web page shows the same model.
 */
#include "ext.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const struct ui_ext ext_path;

#define KEY_BLUE 401
#define NSTAGE 5
#define NNOTE 4

struct stage {
	char title[16], a[72], b[96];
};

struct model {
	struct stage st[NSTAGE];
	int perfect;          /* 1 yes, 0 no, -1 not for the unit to say (the signal is not handled in software) */
	char verdict[100];
	char note[NNOTE][110];
	int nnote;
};

static void rate_text(char *out, size_t cap, int hz)
{
	if (hz <= 0)
		snprintf(out, cap, "-");
	else if (hz % 1000 == 0)
		snprintf(out, cap, "%d kHz", hz / 1000);
	else
		snprintf(out, cap, "%.1f kHz", hz / 1000.0);
}

static void note(struct model *m, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void note(struct model *m, const char *fmt, ...)
{
	va_list ap;

	if (m->nnote >= NNOTE)
		return;
	va_start(ap, fmt);
	vsnprintf(m->note[m->nnote++], sizeof m->note[0], fmt, ap);
	va_end(ap);
}

static int is_spdif(int in)
{
	return in == X45_IN_AES_EBU || in == X45_IN_COAXIAL || in == X45_IN_OPTICAL;
}

static int is_analog(int in)
{
	return in == X45_IN_LINEIN || in == X45_IN_AUXIN || in == X45_IN_PHONO;
}

static void build(struct ui *u, struct model *m)
{
	int in = u->input, bt = u->bt_swap, rate = 0, bits = 0, dsd = 0, hw_vol, playing;
	const char *btn = ui_btout_connected_name();
	char r[24], buf[96];
	struct stage *s = m->st;

	memset(m, 0, sizeof *m);
	m->perfect = -1;
	snprintf(s[0].title, sizeof s[0].title, "SOURCE");
	snprintf(s[1].title, sizeof s[1].title, "PROCESSING");
	snprintf(s[2].title, sizeof s[2].title, "FORMAT");
	snprintf(s[3].title, sizeof s[3].title, "LEVEL");
	snprintf(s[4].title, sizeof s[4].title, "OUTPUT");

	/* what arrives */
	playing = in == X45_IN_NETWORK && strcmp(u->st.state, "stop") != 0;
	if (playing) {
		if (!strncmp(u->st.audio, "dsd", 3)) {
			dsd = 1;
			rate = 0;
		} else if (sscanf(u->st.audio, "%d:%d", &rate, &bits) != 2) {
			rate = bits = 0;
		}
	}

	/* ---- source ---- */
	snprintf(s[0].a, sizeof s[0].a, "%s", ui_input_label(u));
	switch (in) {
	case X45_IN_NETWORK:
		if (u->have_song && u->song.file[0]) {
			const char *name = ui_station_name(u);
			const char *base = strrchr(u->song.file, '/');

			if (!strncmp(u->song.file, "http", 4))
				snprintf(s[0].b, sizeof s[0].b, "stream: %s", name[0] ? name : u->song.name);
			else
				snprintf(s[0].b, sizeof s[0].b, "file: %s", base ? base + 1 : u->song.file);
		} else {
			snprintf(s[0].b, sizeof s[0].b, "nothing playing");
		}
		break;
	case X45_IN_FMRADIO:
		snprintf(s[0].b, sizeof s[0].b, "%s: %s", u->tu_mode == 1 ? "DAB+" : "FM", u->tu_mode == 1 ? u->tu_svc : u->tu_ps);
		break;
	case X45_IN_PCUSB:
	case X45_IN_PCUSB2:
		snprintf(s[0].b, sizeof s[0].b, "%s", u->pcusb ? "computer connected to the USB-DAC port" : "no computer connected");
		break;
	case X45_IN_AES_EBU:
	case X45_IN_COAXIAL:
	case X45_IN_OPTICAL:
		snprintf(s[0].b, sizeof s[0].b, "digital cable");
		break;
	default:
		snprintf(s[0].b, sizeof s[0].b, "%s", is_analog(in) ? "analogue signal" : "");
		break;
	}

	/* ---- processing, format ---- */
	if (in == X45_IN_NETWORK) {
		if (bt) {
			snprintf(s[1].a, sizeof s[1].a, "MPD, resampled for Bluetooth");
			snprintf(s[1].b, sizeof s[1].b, "44.1 kHz / 16 bit to the headset");
		} else {
			snprintf(s[1].a, sizeof s[1].a, "MPD");
			snprintf(s[1].b, sizeof s[1].b, "no resampling, no format or channel change, no software volume");
		}
		if (dsd) {
			snprintf(s[2].a, sizeof s[2].a, "%s", u->st.audio);
			snprintf(s[2].b, sizeof s[2].b, "native DSD, passed to the DAC as it is");
		} else if (rate > 0) {
			rate_text(r, sizeof r, rate);
			snprintf(s[2].a, sizeof s[2].a, "%s / %d bit", r, bits);
			if (u->st.bitrate > 0)
				snprintf(s[2].b, sizeof s[2].b, "as played, %d kbit/s coded", u->st.bitrate);
			else
				snprintf(s[2].b, sizeof s[2].b, "the file's own format");
		} else {
			snprintf(s[2].a, sizeof s[2].a, "-");
		}
	} else if (in == X45_IN_FMRADIO) {
		snprintf(s[1].a, sizeof s[1].a, "tuner module");
		snprintf(s[1].b, sizeof s[1].b, "%s", u->tu_mode == 1 ? "DAB+ decoder in the module" : "FM demodulator in the module");
		snprintf(s[2].a, sizeof s[2].a, "set by the module");
	} else if (in == X45_IN_PCUSB || in == X45_IN_PCUSB2) {
		snprintf(s[1].a, sizeof s[1].a, "USB audio bridge");
		snprintf(s[1].b, sizeof s[1].b, "the computer's stream goes to the DAC, the unit's software is not in it");
		snprintf(s[2].a, sizeof s[2].a, "what the computer sends");
	} else if (is_spdif(in)) {
		snprintf(s[1].a, sizeof s[1].a, "S/PDIF receiver");
		snprintf(s[1].b, sizeof s[1].b, "in hardware, the unit's software is not in the signal");
		if (u->cs_rate > 0) {
			rate_text(r, sizeof r, (int)u->cs_rate);
			snprintf(s[2].a, sizeof s[2].a, "%s", r);
			snprintf(s[2].b, sizeof s[2].b, "locked to the cable");
		} else {
			snprintf(s[2].a, sizeof s[2].a, "no lock");
		}
	} else if (is_analog(in)) {
		int idx = u->trim[in], db = idx - 12;

		snprintf(s[1].a, sizeof s[1].a, "analogue gain, A/D converter");
		snprintf(s[1].b, sizeof s[1].b, "input trim %+d dB", db);
		snprintf(s[2].a, sizeof s[2].a, "analogue");
		snprintf(s[2].b, sizeof s[2].b, "digitised in hardware; the panel meter listens in, the sound does not go through software");
	} else {
		snprintf(s[1].a, sizeof s[1].a, "-");
		snprintf(s[2].a, sizeof s[2].a, "-");
	}
	if (u->input == X45_IN_NETWORK && !strcmp(u->st.state, "stop"))
		snprintf(s[2].a, sizeof s[2].a, "-");

	/* ---- level ---- */
	hw_vol = !bt;
	if (ui_volume_locked(u)) {
		snprintf(s[3].a, sizeof s[3].a, "fixed, full level");
		snprintf(s[3].b, sizeof s[3].b, "the volume keys do not change the output (for use with an amplifier)");
	} else {
		snprintf(s[3].a, sizeof s[3].a, "volume %d%s", u->volume, u->muted ? "  (muted)" : "");
		snprintf(s[3].b, sizeof s[3].b, "%s", bt ? "the headset's own level" : "in the DAC and its analogue stage; samples are not scaled");
	}
	if (u->balance) {
		snprintf(buf, sizeof buf, "   balance %s %d", u->balance > 0 ? "right" : "left", u->balance > 0 ? u->balance : -u->balance);
		snprintf(s[3].a + strlen(s[3].a), sizeof s[3].a - strlen(s[3].a), "%s", buf);
	}

	/* ---- output ---- */
	if (bt) {
		snprintf(s[4].a, sizeof s[4].a, "Bluetooth: %s", btn ? btn : "headset");
		snprintf(s[4].b, sizeof s[4].b, "the DAC is not used while the headset plays");
	} else {
		snprintf(s[4].a, sizeof s[4].a, "%s%s%s%s%s", u->out_lineout ? "RCA" : "", u->out_lineout && (u->out_xlr || u->out_hp) ? " + " : "",
			 u->out_xlr ? "XLR" : "", u->out_xlr && u->out_hp ? " + " : "", u->out_hp ? "headphones" : "");
		if (!s[4].a[0])
			snprintf(s[4].a, sizeof s[4].a, "no output on");
		snprintf(s[4].b, sizeof s[4].b, "two ES9018 DACs, dual mono%s", u->hp_present ? "; headphones plugged in" : "");
	}

	/* ---- verdict ---- */
	if (in == X45_IN_NETWORK) {
		if (!playing) {
			snprintf(m->verdict, sizeof m->verdict, "Nothing is playing.");
		} else if (bt) {
			m->perfect = 0;
			snprintf(m->verdict, sizeof m->verdict, "Not bit-perfect: the Bluetooth link carries 44.1 kHz / 16 bit.");
		} else {
			m->perfect = 1;
			snprintf(m->verdict, sizeof m->verdict, "Bit-perfect: the samples reach the DAC unchanged.");
		}
		if (u->balance && !bt)
			note(m, "Balance is off centre: it changes the level of one side in the DAC, not the samples.");
		if (!hw_vol)
			note(m, "Volume is applied in software on this output.");
		if (!bt && u->muted)
			note(m, "The output is muted.");
		if (u->st.state[0] == 'p' && u->st.state[1] == 'a')
			note(m, "Paused.");
	} else if (in == X45_IN_FMRADIO) {
		snprintf(m->verdict, sizeof m->verdict, "The tuner module decodes the station; only its output reaches the DAC.");
	} else {
		snprintf(m->verdict, sizeof m->verdict, "Not handled in software: this input plays in hardware, so nothing here can change it.");
		if (is_analog(in) && u->trim[in] != 12)
			note(m, "The input trim is not 0 dB.");
	}
	if (u->settle_until)
		note(m, "A rate change is settling: the output is muted for a moment.");
}

/* ------------------------------------------------------------------- screen --- */

static void draw(struct ui *u)
{
	struct x45_surface *s = &u->g.back;
	struct model m;
	int i, y = 76;

	build(u, &m);
	x45_fill_rect(s, 36, y + 22, 2, NSTAGE * 66 - 40, u->t.line);
	for (i = 0; i < NSTAGE; i++) {
		x45_fill_rect(s, 31, y + 17, 12, 12, u->t.accent);
		x45_text(s, 64, y + 16, m.st[i].title, 2, u->t.dim);
		text_fold(s, 250, y + 6, m.st[i].a[0] ? m.st[i].a : "-", 3, s->w - 250 - 24, u->t.text);
		if (m.st[i].b[0])
			text_fold(s, 250, y + 34, m.st[i].b, 2, s->w - 250 - 24, u->t.dim);
		y += 66;
	}
	y += 10;
	x45_hline(s, 24, y - 6, s->w - 48, u->t.line);
	text_fold(s, 24, y + 4, m.verdict, 3, s->w - 48, m.perfect == 1 ? u->t.accent : m.perfect == 0 ? u->t.warn : u->t.text);
	for (i = 0; i < m.nnote; i++)
		text_fold(s, 24, y + 38 + i * 26, m.note[i], 2, s->w - 48, u->t.dim);
	draw_hints(u, "RETURN|back");
}

static void poll(struct ui *u)
{
	static long t;
	long now = ext_now_ms();

	if (u->screen == SCR_EXT && u->ext == &ext_path && ext_since(now, t) >= 1000) {
		t = now;
		u->dirty = 1;
	}
}

static void ok(struct ui *u)
{
	ui_ext_close(u);
}

static void move(struct ui *u, int d)
{
	(void)u;
	(void)d;
}

static int ir(struct ui *u, int code)
{
	if (code == KEY_BLUE) {   /* the key that opened it closes it */
		if (!u->ir_bounce)
			ui_ext_close(u);
		return 1;
	}
	return 0;
}

static void jstr(FILE *f, const char *s)
{
	fputc('"', f);
	for (; *s; s++) {
		if (*s == '"' || *s == '\\')
			fputc('\\', f);
		if ((unsigned char)*s >= 0x20)
			fputc(*s, f);
	}
	fputc('"', f);
}

static void json(struct ui *u, FILE *f)
{
	struct model m;
	int i;

	build(u, &m);
	fputs("{\"stages\":[", f);
	for (i = 0; i < NSTAGE; i++) {
		fprintf(f, "%s{\"name\":", i ? "," : "");
		jstr(f, m.st[i].title);
		fputs(",\"a\":", f);
		jstr(f, m.st[i].a);
		fputs(",\"b\":", f);
		jstr(f, m.st[i].b);
		fputc('}', f);
	}
	fprintf(f, "],\"perfect\":%d,\"verdict\":", m.perfect);
	jstr(f, m.verdict);
	fputs(",\"notes\":[", f);
	for (i = 0; i < m.nnote; i++) {
		if (i)
			fputc(',', f);
		jstr(f, m.note[i]);
	}
	fputs("]}", f);
}

static int cmd(struct ui *u, const char *args)
{
	if (!strncmp(args, "open", 4)) {
		ui_ext_open_name(u, "path");
		return 1;
	}
	return 0;
}

const struct ui_ext ext_path = {
	.name = "path",
	.title = "SIGNAL PATH",
	.menu = "Signal path",
	.draw = draw,
	.ok = ok,
	.move = move,
	.ir = ir,
	.poll = poll,
	.json = json,
	.cmd = cmd,
};
