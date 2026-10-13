/* hdcd.h - HDCD decoder (stereo, 16-bit, any 44.1/48 kHz stream).
 * A port of FFmpeg's af_hdcd filter, see hdcd.c for the origin and licence.
 *
 * hdcd_decode() takes interleaved stereo frames as 16-bit values in int32_t
 * and turns them in place into the decoder's own scale: a plain sample comes out
 * as sample << 15, and peak-extended samples reach up to +/- 2^31 (twice the
 * plain full scale, so that the peaks above -3 dBFS that HDCD stretches have room).
 * A caller that wants the same level as a plain CD multiplies by two with
 * saturation, one that wants the decoder's headroom leaves it alone.
 */
#ifndef X45_HDCD_H
#define X45_HDCD_H

#include <stdint.h>

enum { HDCD_NONE = 0, HDCD_NO_EFFECT = 1, HDCD_EFFECTUAL = 2 };
enum { HDCD_PE_NEVER = 0, HDCD_PE_INTERMITTENT = 1, HDCD_PE_PERMANENT = 2 };

struct hdcd_ch {
	uint64_t window;
	unsigned char readahead;
	uint8_t arg, control;          /* control: bits 0-3 gain, 4 peak extend, 5 transient filter */
	unsigned sustain, sustain_reset;   /* code detect timer, in frames */
	int running_gain;
	int code_counterA, code_counterA_almost;
	int code_counterB, code_counterB_checkfails;
	int code_counterC;
	int count_peak_extend, count_transient_filter;
	int max_gain;                  /* in 0.5 dB steps (0 .. 15) */
	int count_sustain_expired;
};

struct hdcd {
	struct hdcd_ch ch[2];
	int val_target_gain;
	int detected;                  /* HDCD_NONE / HDCD_NO_EFFECT / HDCD_EFFECTUAL, sticky */
};

struct hdcd_info {
	int detected;                  /* as above */
	int active;                    /* codes are coming in right now, in both channels */
	int peak_extend;               /* HDCD_PE_* over the packets seen so far */
	int max_gain;                  /* largest attenuation signalled, in 0.5 dB steps */
	int transient_filter;
	int packets;                   /* valid packets, both channels */
	int errors;                    /* packets that looked like codes but failed a check */
};

void hdcd_init(struct hdcd *h, unsigned rate, unsigned cdt_ms);
void hdcd_decode(struct hdcd *h, int32_t *frames, int nframes);
void hdcd_info(const struct hdcd *h, struct hdcd_info *out);

#endif
