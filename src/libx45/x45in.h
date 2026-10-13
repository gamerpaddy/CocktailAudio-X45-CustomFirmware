/* libx45/input  -  front panel and IR input for the Cocktail Audio X45.
 *
 * Two evdev nodes, both EV_KEY only:
 *
 *   gpio-keys.19    front panel: 9 controls over 11 key codes
 *   gpio_ir_recv    IR, NEC, stock keytable, 66 codes
 *
 * The knobs. There is no EV_REL axis - CONFIG_INPUT_GPIO_ROTARY_ENCODER is
 * unset - so each knob appears as two gpio-keys entries:
 *
 *   volume knob   KEY 474 (A, "Volume Up", pin 23)   KEY 475 (B, "Volume Down", pin 22)
 *   scroll wheel  KEY 476 (A, "Scroll Up", pin 25)   KEY 477 (B, "Scroll Down", pin 24)
 *
 * THE LINES ARE QUADRATURE PHASES, NOT DIRECTIONS. The device-tree labels read
 * like one line per direction, and an earlier revision of this file trusted
 * them (X45_DECODE_DIRECTION). It was wrong, and it made the knob dead: a real
 * recording of the volume knob shows both lines toggling in turn - states
 * (A,B) = 10 11 01 00 10 11 01 00 ... - the Gray-code sequence of an ordinary
 * incremental encoder, four edges per full cycle. Counting each press as +/-1
 * by label made the two lines cancel, so no turn ever registered.
 *
 * Hence X45_DECODE_QUADRATURE is the default. Which way is "up": A leads B on a
 * clockwise turn (A rises first), which is the sequence above, and fits the
 * label "Volume Up" being on A. quad_tbl counts the opposite sense as positive,
 * so x45_input_open() sets `invert`. That polarity rests on the assumption that
 * the recording was a clockwise turn; if the knob turns the wrong way, flip
 * knob_invert in etc/x45ui.conf.
 */
#ifndef X45IN_H
#define X45IN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Front panel key codes, as the device tree's gpio-keys node assigns them. */
#define X45_KEY_RETURN     1   /* 0x001  GPIO4.28 */
#define X45_KEY_VOL_PUSH   60  /* 0x03c  GPIO4.21  "Volume Mute" */
#define X45_KEY_MENU       68  /* 0x044  GPIO4.30 */
#define X45_KEY_POWER      116 /* 0x074  GPIO3.29 */
#define X45_KEY_STOP       128 /* 0x080  GPIO4.29 */
#define X45_KEY_INPUT      395 /* 0x18b  GPIO4.27 */
#define X45_KEY_OK         473 /* 0x1d9  GPIO4.26  scroll wheel push, "Ok Pause" */
#define X45_ENC_VOL_UP     474 /* 0x1da  pin 23, DT label "Volume Up"   */
#define X45_ENC_VOL_DOWN   475 /* 0x1db  pin 22, DT label "Volume Down" */
#define X45_ENC_SCROLL_UP  476 /* 0x1dc  pin 25, DT label "Scroll Up"   */
#define X45_ENC_SCROLL_DN  477 /* 0x1dd  pin 24, DT label "Scroll Down" */

/* kept as aliases: in the quadrature reading, "up" is phase A and "down" B */
#define X45_ENC_VOL_A      X45_ENC_VOL_UP
#define X45_ENC_VOL_B      X45_ENC_VOL_DOWN
#define X45_ENC_SCROLL_A   X45_ENC_SCROLL_UP
#define X45_ENC_SCROLL_B   X45_ENC_SCROLL_DN

enum x45_ev_kind {
	X45_EV_NONE = 0,
	X45_EV_KEY,     /* front panel button: code + pressed */
	X45_EV_ENCODER, /* knob moved: which + ticks (signed) */
	X45_EV_IR       /* remote: code + pressed (1 pressed, 0 released, 2 the button is held: a repeat frame) */
};

enum x45_knob {
	X45_KNOB_VOLUME = 0,
	X45_KNOB_SCROLL = 1
};

/* How to turn the two per-knob lines into signed ticks. See the note above. */
enum x45_decode {
	X45_DECODE_DIRECTION = 0, /* default: up line = +1, down line = -1 */
	X45_DECODE_QUADRATURE = 1 /* Gray-coded phase pair */
};

struct x45_event {
	enum x45_ev_kind kind;
	int code;    /* key/IR code; for ENCODER, the enum x45_knob */
	int pressed; /* KEY/IR: 1 press, 0 release */
	int ticks;   /* ENCODER: signed detents, negative = counter-clockwise */
	const char *name; /* human-readable label, never NULL */
	struct timeval_compat {
		long sec;
		long usec;
	} when;
};

/* One quadrature channel pair. */
struct x45_quad {
	uint8_t state;   /* 2-bit (A<<1)|B */
	int8_t  accum;   /* sub-detent accumulator */
	int     seen;    /* have we established an initial state yet */
};

/* One record as this kernel (3.14, 32-bit ARM) writes it to /dev/input/eventN:
 * 16 bytes, with a timestamp of two 32-bit words.
 *
 * Do NOT use <linux/input.h>'s struct input_event for reading. Built against
 * musl, its embedded `struct timeval` has a 64-bit seconds field, which makes
 * the struct 24 bytes - but the kernel still hands out 16-byte records. A
 * reader that asks for sizeof(struct input_event) gets a short read of 16 bytes,
 * and a check of `got != sizeof iev` then throws away *every* event. That is
 * exactly what made the front panel completely dead: the device, the driver,
 * the decoder and the key map were all right, and nothing ever arrived. */
struct x45_input_event {
	uint32_t sec;
	uint32_t usec;
	uint16_t type;
	uint16_t code;
	int32_t value;
};
_Static_assert(sizeof(struct x45_input_event) == 16,
	       "the kernel's input record is 16 bytes on 32-bit ARM");

struct x45_indev {
	int fd_panel;
	int fd_ir;
	struct x45_quad knob[2];
	enum x45_decode decode;
	int ticks_per_detent; /* quadrature only: 4 for full, 1 to report every edge */
	int invert;           /* quadrature only: negate every tick (hardware polarity) */
	int fd_fast;          /* read end of the knob sampler's pipe, -1 if not running */
	/* the remote: a held button sends the key once, then only bare NEC repeat frames (a scancode with no key
	 * event, about every 108 ms); remembered so those become repeat events */
	uint32_t ir_scan;     /* scancode of the packet being read */
	int ir_scan_new;      /* ir_scan was set in this packet */
	int ir_key_new;       /* this packet carried a key event */
	uint32_t ir_last_scan;
	int ir_last_code;     /* key code of the button held now, 0 if none */
	long ir_last_ms;      /* when its last frame arrived */
};

/* gpio-keys only reads a pin's level in a deferred work item (kernel HZ=100),
 * so a fast turn collapses several edges into one and the decoder loses its
 * place. When /dev/mem is available, x45_input_open() therefore samples the
 * GPIO4 pad-status register itself, ~4 kHz, in a thread, and the knob events
 * from evdev are ignored. The panel buttons still come from evdev. */

/* Opens both devices by *name*, not by event index  -  the numbering is not
 * guaranteed. Looks for "gpio-keys" and "gpio_ir_recv" under
 * /sys/class/input/eventN, reading each device's 'name' attribute.
 *
 * Returns 0 if at least one opened, -errno if neither did. A missing device
 * leaves its fd at -1 and is simply skipped. */
int x45_input_open(struct x45_indev *in);
void x45_input_close(struct x45_indev *in);

/* Waits up to timeout_ms (-1 = forever) for the next decoded event.
 * Returns 1 on an event, 0 on timeout, -errno on error.
 *
 * Raw channel transitions that do not complete a detent return 0 with
 * ev->kind == X45_EV_NONE, so a caller can simply loop. */
int x45_input_next(struct x45_indev *in, struct x45_event *ev, int timeout_ms);

/* Runs one channel edge through the quadrature model. Exposed so a diagnostic
 * can score both models side by side against the same event stream. */
int x45_quad_feed(struct x45_quad *q, int chan_b, int value, int per_detent);

/* Labels. Both always return a printable string. */
const char *x45_key_name(int code);
const char *x45_ir_name(int code);

#ifdef __cplusplus
}
#endif
#endif /* X45IN_H */
