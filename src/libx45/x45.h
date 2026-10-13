/* libx45  -  userspace control of the Cocktail Audio X45 audio board.
 *
 * Wraps the /dev/x45 character device, whose ABI was recovered by
 * disassembling the in-kernel driver out of /dev/mem.
 *
 * The device is a single ioctl handler carrying a 16-byte struct. Everything
 * the audio board can do goes through it.
 */
#ifndef X45_H
#define X45_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ ABI --- */

/* The vendor built these macros with `int` while the handler copies 16 bytes
 * unconditionally. Reproduced verbatim so the numbers match. */
#define X45_IOC_SET 0x40047101u /* _IOW(0x71, 1, int) */
#define X45_IOC_GET 0x80047102u /* _IOR(0x71, 2, int) */

struct x45_cfg {
	int32_t cmd;    /* word 0  -  sub-command */
	int32_t value0; /* word 1  -  never read by any SET; echoed back on GET */
	int32_t value1; /* word 2  -  the value; receives the result on GET */
	int32_t value2; /* word 3  -  second value (right channel); zeroed on GET */
};

/* SET sub-commands. Names follow the driver's own symbols. */
enum x45_set {
	X45_SET_OUTPUT_RESET      = 1,  /* x45_src4392_output_reset */
	X45_SET_DSD_BITRATE       = 2,  /* indirect via g+0x184; ignores its arg */
	X45_SET_PCM_RATE          = 3,  /* ignores its arg; re-apply trigger */
	X45_SET_ES9018_VOLUME     = 5,  /* L=value1 R=value2 */
	X45_SET_ANALOG_VOL_TYPE   = 6,  /* variable / fixed line-out */
	X45_SET_MUTE              = 7,
	X45_SET_VOL_QUALITY       = 11, /* touches no registers */
	X45_SET_INPUT_MODE        = 12, /* 0..9 */
	X45_SET_SRC4392_VOLUME    = 13, /* L=value1 R=value2 */
	X45_SET_AES_EBU           = 17,
	X45_SET_SPDIF             = 18, /* coax AND optical together */
	X45_SET_AUDIO_POWER       = 20, /* 1 = up, 0 = down */
	X45_SET_DIGITAL_VOL_TYPE  = 22,
	X45_SET_OPTICAL           = 23, /* optical only */
	X45_SET_LINEOUT           = 25,
	X45_SET_OUTPUT_VOLUME     = 26, /* master 0..100, same path as ALSA Master */
	X45_SET_NJU72341_VOLUME   = 27, /* range index 0..21, -12..+9 dB */
	X45_SET_SRC4392_OUTPUT    = 28, /* applies the pending output enables */
	X45_SET_XLROUT            = 34,
	X45_SET_HEADPHONE         = 37,
	X45_SET_IS_MQA            = 38, /* touches no registers */
	X45_SET_ES9018_FILTER     = 39,
	X45_SET_AUDIO_MODE        = 49  /* touches no registers */
};

/* GET sub-commands. */
enum x45_get {
	X45_GET_HEADPHONE   = 19, /* jack present */
	X45_GET_CS8422_RATE = 29, /* incoming S/PDIF sample rate */
	X45_GET_PCUSB       = 35, /* PC plugged into the USB-DAC port */
	X45_GET_CACHED      = 36  /* g_x45+0x178, maintained from the USB DAC */
};

/* Input modes, as x45_input_mode()'s 10-way switch orders them.
 *
 * Mode 9 shares mode 2's body plus extra mux work. Switching is a no-op while
 * the board is powered down, and re-selecting the current mode is a no-op. */
enum x45_input {
	X45_IN_NETWORK = 0, /* x45_input_normal  -  network/local playback */
	X45_IN_FMRADIO = 1, /* DAB+/FM tuner */
	X45_IN_PCUSB   = 2, /* PC-USB / DSD */
	X45_IN_AES_EBU = 3,
	X45_IN_COAXIAL = 4,
	X45_IN_OPTICAL = 5,
	X45_IN_LINEIN  = 6,
	X45_IN_AUXIN   = 7,
	X45_IN_PHONO   = 8,
	X45_IN_PCUSB2  = 9
};

/* ------------------------------------------------------------- lifecycle --- */

/* Opens /dev/x45. Returns a fd >= 0, or -errno. */
int x45_open(void);
void x45_close(int fd);

/* Raw access. Both return 0, or -errno. x45_get() stores the result in *out. */
int x45_set(int fd, int cmd, int32_t value1, int32_t value2);
int x45_get(int fd, int cmd, int32_t *out);

/* ---------------------------------------------------------------- bring-up - */

/* Power the analogue+digital audio board.
 *
 * The board can sit with its GPIOs reading "powered" while every I2C access
 * times out at ~510 ms. SET 20,1 alone does not clear that  -  no GPIO
 * transitions occur. A full down/up cycle does. x45_power_cycle() performs
 * that cycle with the settle delay, and is what you want after a wedge.
 *
 * Note the XMOS re-enumerates on USB when the board is power-cycled, so ALSA
 * card indices move. Never hardcode them. */
int x45_power(int fd, int on);
int x45_power_cycle(int fd);

/* The verified bring-up sequence: power cycle, select `input`, enable line-out
 * and XLR, apply the output config, unmute. This is the minimum that gets
 * sound out of the box with x20 not running. */
int x45_bringup(int fd, enum x45_input input);

/* ------------------------------------------------------------------ paths --- */

int x45_input(int fd, enum x45_input mode);
int x45_mute(int fd, int on);

/* Master volume, 0..100 raw. Do NOT drive this as a percentage of the dB scale
 * ALSA advertises: the TLV claims DB_SCALE(min=-1000, step=10), i.e. -10 dB
 * total, but the real span is -64 dB (SRC4392) / -74 dB (ES9018). */
int x45_volume(int fd, int vol);

/* Balance, as the stock firmware does it: a setting of -X45_BALANCE_MAX..+X45_BALANCE_MAX steps (positive = the
 * right side louder), each step one volume index on the left or right of the 0..100 scale (half a step per side:
 * at +2 the right is one index up and the left one down). x45_balance_levels() gives the left and right index for
 * a master volume; whichever side would pass 100 pins there and the other side gives way instead, and a side never
 * goes below 1 (0 is mute) unless the master itself is 0. Pure arithmetic, no hardware. */
#define X45_BALANCE_MAX 24
void x45_balance_levels(int vol, int balance, int *left, int *right);

/* Writes the per-channel levels to the DAC (SET 5: value 1 the left ES9018, value 2 the right one) and the level of
 * the digital outputs (SET 13: one level for both sides, as the SRC4392 attenuates its AES/S-PDIF transmitter as a
 * pair; the master volume is the right value, so the digital outputs do not follow the balance). The left/right levels
 * override the equal levels the master volume just wrote, so call this after every master write; a balance of 0
 * restores L = R. SET 5 reaches the chip only while the analogue stage is variable (SET 6 = 0), SET 13 only while the
 * digital stage is (SET 22 = 0); in the fixed state the driver takes the command and changes nothing.
 * Returns 0 or -errno. */
int x45_volume_lr(int fd, int left, int right, int digital);

/* The two volume stages (analogue SET 6, digital SET 22): fixed = 0 variable, 1 fixed. The driver starts with
 * both fixed, and then x45_volume() changes nothing; whoever wants the level to follow x45_volume() sets 0 first
 * (x45ui does at start-up, and again when the volume mode is changed in Options). After x45_bringup() set it
 * again. Returns 0 or -errno. */
int x45_volume_mode(int fd, int fixed);

/* Output enables. These only store state in the driver  -  no register write
 * happens until the path is reconfigured. x45_outputs_apply() issues SET 28,
 * which is what makes them take effect. */
int x45_out_lineout(int fd, int on);
int x45_out_xlr(int fd, int on);
int x45_out_headphone(int fd, int on);
int x45_out_aes_ebu(int fd, int on);
int x45_out_spdif(int fd, int on);   /* coax + optical */
int x45_out_optical(int fd, int on); /* optical alone */
int x45_outputs_apply(int fd);

/* ------------------------------------------------------------ master clock --- */

/* FS_4844 (GPIO1.7) selects the XMOS master-clock family: 0 = 44.1 kHz family,
 * 1 = 48 kHz family. Nothing in the kernel driver sets it - it only *prints*
 * MCLK=[44] or MCLK=[48] from the requested rate - so a stream in the other
 * family plays at the wrong speed. Measured on hardware from the USB feedback
 * endpoint (Momentary freq in /proc/asound/cardN/stream0):
 *
 *     96 kHz requested, FS_4844=0  ->  88205 Hz   (8.1 % slow, ~1.4 semitones flat)
 *     96 kHz requested, FS_4844=1  ->  96005 Hz
 *
 * An earlier note here called this line "purpose unknown" because the driver's
 * log lines did not follow it. The log lines come from the requested rate and
 * say nothing about the clock actually running.
 *
 * Set it BEFORE opening the PCM. It is a plain GPIO write via /proc/x45/gpio,
 * so it needs no /dev/x45 handle. */
#define X45_CLOCK_44 0
#define X45_CLOCK_48 1

/* Family for a rate in Hz (PCM, or DSD expressed in frames/s). Returns
 * X45_CLOCK_44, X45_CLOCK_48, or -1 for a rate that belongs to neither. */
int x45_clock_family(unsigned rate);
int x45_clock_select(unsigned rate);  /* write the line for `rate`; 0 or -errno */
int x45_clock_get(void);              /* current line state 0/1, or -errno */

/* ------------------------------------------------------------- DAC ownership --- */

/* The audio chain has one PCM, so only one source can hold it. The rule is that
 * the newest action wins. AirPlay (shairport-sync) marks a live session with
 * /tmp/x45-airplay-active (deploy/x45-airplay-begin); anything about to open the
 * DAC for itself calls this first.
 *
 * If a session is live: terminate the receiver, remove the marker, and wait (up
 * to 3 s) until the process has actually exited so the PCM is free. Returns 1
 * if a session was cut, 0 if there was nothing to do. The supervisor restarts
 * the receiver, which is idle until the next session, so AirPlay stays
 * available. Matching is on the process name, never on a command line. */
int x45_airplay_preempt(void);

/* The same, with the marker path, process name and wait time given. Exists so
 * the logic can be tested with a stand-in process. */
int x45_preempt_ex(const char *marker, const char *process, int wait_ms);

/* ----------------------------------------------------------------- tables --- */

/* The vendor's volume curves, read out of kernel .data. Index 0..100.
 * Both taper: 1.0 dB per index at the bottom, 0.5 dB over the top ~35 dB.
 * Returned for display and for anything that needs the true dB value. */
uint8_t x45_src4392_vol_reg(int idx);
uint8_t x45_es9018_vol_reg(int idx);
double  x45_src4392_vol_db(int idx);
double  x45_es9018_vol_db(int idx);

/* ------------------------------------------------------------------ names --- */

const char *x45_input_name(enum x45_input mode);
int         x45_input_from_name(const char *name); /* -1 if unknown */
const char *x45_set_name(int cmd);                 /* NULL if unimplemented */
const char *x45_get_name(int cmd);

#ifdef __cplusplus
}
#endif
#endif /* X45_H */
