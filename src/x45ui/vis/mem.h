/* mem - what the visuals that keep a history keep between analyzer frames (loudness, record check, tonal balance, key).
 *
 * It lives in struct ui (ui.h: `vm`), not in static variables of the visual's file: the pictures of the visuals menu are drawn
 * on a copy of the ui that holds made-up music (vis/preview.c), and a copy has a history of its own. The real one is never touched.
 * Every visual starts afresh (mem_reset in its file) when the input changes or when the analyzer was away for a while: playback stopped
 * and started again, another source. */
#ifndef X45UI_VIS_MEM_H
#define X45UI_VIS_MEM_H

#define LOUD_HIST    240     /* one short-term loudness a second: four minutes */
#define LOUD_BINS    141     /* -70 .. 0 LUFS in half LU: the histograms of the gated measurements */
#define LOUD_FEEDS   128     /* analyzer frames kept to work out the momentary and short-term loudness */
#define REC_BLOCKS   60      /* one second each: the last minute */
#define MEM_BANDS    128     /* NSB of vis.h */

/* loudness.c: loudness in LUFS, estimated from the analyzer (the channel levels, weighted by the band spectrum) */
struct vis_loud {
	int input, t_ok;
	long t;                              /* ms clock of the last frame */
	float z[LOUD_FEEDS];                 /* K-weighted mean square of both channels in each frame ... */
	short dt[LOUD_FEEDS];                /* ... and how many ms it stands for */
	int fh, fn;                          /* ring: next slot, frames in it */
	float mom, st;                       /* momentary (last 0.4 s) and short-term (last 3 s), LUFS; -100: not enough yet */
	float hist[LOUD_HIST];               /* short-term loudness, once a second, oldest first ... */
	int hh, hn;                          /* ... next slot, points in it */
	long hist_ms;                        /* ms since the last point */
	double ienergy[LOUD_BINS];           /* gated blocks (the momentary values), by loudness: their energy ... */
	unsigned icount[LOUD_BINS];          /* ... and their number */
	unsigned lcount[LOUD_BINS];          /* short-term values by loudness, for the loudness range */
	float integ, lra;                    /* integrated loudness (LUFS) and loudness range (LU); integ -100: none yet */
	float tpk;                           /* highest sample peak, dBFS */
	long slow_ms;                        /* ms since integ and lra were worked out */
};

/* reccheck.c: the numbers by which a recording or a transfer is judged */
struct vis_rec {
	int input, t_ok;
	long t;
	float peak;                          /* highest sample peak, dBFS (-140: none yet) */
	int clips, clip_run;                 /* stretches at full scale */
	long blk_ms;                         /* the second being collected: its length ... */
	double blk_z[2];                     /* ... mean square of each channel x ms ... */
	float blk_pk, blk_c;                 /* ... highest peak and correlation x ms */
	float b_rms[REC_BLOCKS], b_pk[REC_BLOCKS];   /* the last minute in seconds: level of the pair, peak (dBFS) */
	int bh, bn;
	float floor_db, top_db;              /* the quietest and the loudest second so far (RMS, dBFS) */
	double lr_z[2];                      /* energy of each channel since the start */
	double corr_sum;                     /* correlation x seconds, for its mean */
	float corr_min;                      /* lowest correlation of a second */
	long corr_ms;
	float sub;                           /* energy below 32 Hz against 100 .. 250 Hz, dB, smoothed */
	float hum;                           /* the 50 / 60 Hz line against its surroundings, dB, in the quiet; -140: no quiet seen */
	int sub_ok;
};

/* tonal.c: the long-term spectrum */
struct vis_tonal {
	int input, t_ok;
	long t;
	float pw[MEM_BANDS];                 /* band power (linear), averaged over the last ~10 s of music */
	long warm_ms;                        /* how long it has been collecting (the average is a plain mean until 10 s) */
};

/* chroma.c: pitch classes, and from them the key and the chord */
struct vis_chroma {
	int input, t_ok;
	long t;
	float slow[12], fast[12];            /* strength of C, C#, D ... B: over ~6 s (the key) and ~0.4 s (the chord) */
	long play_ms;                        /* time of music in the slow one */
	int key, minor;                      /* the key now: tonic 0..11, 1 minor; key -1 when it is not clear */
	float key_r;                         /* how well the profile fits, 0..1 */
	float shown[12];                     /* smoothed for drawing */
	int chord, chord_minor;              /* the chord: root 0..11, -1: none */
	float chord_r;
};

/* width.c: how much of the sound is side signal, smoothed */
struct vis_width {
	int input, t_ok;
	long t;
	float reg[3];                        /* side power / (mid + side), 0..1: below 120 Hz, 120 Hz .. 2 kHz, above 2 kHz */
	int ok;                              /* reg holds a measurement */
};

struct vis_mem {
	struct vis_loud loud;
	struct vis_rec rec;
	struct vis_tonal tonal;
	struct vis_chroma chroma;
	struct vis_width width;
};

#endif /* X45UI_VIS_MEM_H */
