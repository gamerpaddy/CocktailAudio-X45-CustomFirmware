/* libx45  -  see x45.h. */
#define _GNU_SOURCE
#include "x45.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define X45_DEV "/dev/x45"

/* The board needs time to settle between power down and power up. The driver
 * itself gives no completion signal, and I2C stays dead until the rails and
 * the chips' own reset sequences finish. 2 s is what the vendor's own
 * sequencing waits and what was verified on hardware. */
#define X45_POWER_SETTLE_MS 2000

static void msleep(long ms)
{
	struct timespec ts;
	ts.tv_sec = ms / 1000;
	ts.tv_nsec = (ms % 1000) * 1000000L;
	while (nanosleep(&ts, &ts) == -1 && errno == EINTR)
		;
}

int x45_open(void)
{
	int fd = open(X45_DEV, O_RDWR | O_CLOEXEC);
	return fd < 0 ? -errno : fd;
}

void x45_close(int fd)
{
	if (fd >= 0)
		close(fd);
}

int x45_set(int fd, int cmd, int32_t value1, int32_t value2)
{
	struct x45_cfg c;
	c.cmd = cmd;
	c.value0 = 0;
	c.value1 = value1;
	c.value2 = value2;
	if (ioctl(fd, X45_IOC_SET, &c) < 0)
		return -errno;
	return 0;
}

int x45_get(int fd, int cmd, int32_t *out)
{
	struct x45_cfg c;
	c.cmd = cmd;
	c.value0 = 0;
	c.value1 = 0;
	c.value2 = 0;
	if (ioctl(fd, X45_IOC_GET, &c) < 0)
		return -errno;
	if (out)
		*out = c.value1;
	return 0;
}

/* ---------------------------------------------------------------- bring-up - */

int x45_power(int fd, int on)
{
	return x45_set(fd, X45_SET_AUDIO_POWER, on ? 1 : 0, 0);
}

int x45_power_cycle(int fd)
{
	int r = x45_power(fd, 0);
	if (r < 0)
		return r;
	msleep(X45_POWER_SETTLE_MS);
	return x45_power(fd, 1);
}

int x45_bringup(int fd, enum x45_input input)
{
	int r;

	if ((r = x45_power_cycle(fd)) < 0)
		return r;
	if ((r = x45_input(fd, input)) < 0)
		return r;
	if ((r = x45_out_lineout(fd, 1)) < 0)
		return r;
	if ((r = x45_out_xlr(fd, 1)) < 0)
		return r;
	if ((r = x45_outputs_apply(fd)) < 0)
		return r;
	return x45_mute(fd, 0);
}

/* ------------------------------------------------------------------ paths --- */

int x45_input(int fd, enum x45_input mode)
{
	return x45_set(fd, X45_SET_INPUT_MODE, (int32_t)mode, 0);
}

int x45_mute(int fd, int on)
{
	return x45_set(fd, X45_SET_MUTE, on ? 1 : 0, 0);
}

int x45_volume(int fd, int vol)
{
	if (vol < 0)
		vol = 0;
	if (vol > 100)
		vol = 100;
	return x45_set(fd, X45_SET_OUTPUT_VOLUME, vol, 0);
}

void x45_balance_levels(int vol, int balance, int *left, int *right)
{
	int l, r, steps;

	if (vol < 0)
		vol = 0;
	if (vol > 100)
		vol = 100;
	if (balance < -X45_BALANCE_MAX)
		balance = -X45_BALANCE_MAX;
	if (balance > X45_BALANCE_MAX)
		balance = X45_BALANCE_MAX;
	steps = balance < 0 ? -balance : balance;
	l = r = vol;
	if (vol > 0 && steps) {
		int hi = vol + (steps + 1) / 2;   /* the louder side goes up by the larger half, the other down by the smaller */
		int lo = vol - steps / 2;

		if (hi > 100) {          /* the difference stays: the quieter side gives way instead */
			lo -= hi - 100;
			hi = 100;
		} else if (lo < 1) {     /* 0 would be mute; the louder side makes up for it */
			hi += 1 - lo;
			lo = 1;
		}
		if (balance > 0) {
			r = hi;
			l = lo;
		} else {
			l = hi;
			r = lo;
		}
	}
	if (left)
		*left = l;
	if (right)
		*right = r;
}

static int clamp100(int v)
{
	return v < 0 ? 0 : v > 100 ? 100 : v;
}

int x45_volume_lr(int fd, int left, int right, int digital)
{
	int r = x45_set(fd, X45_SET_ES9018_VOLUME, clamp100(left), clamp100(right));

	if (r < 0)
		return r;
	/* the SRC4392 takes one level for both of its channels (value 2 is not read by the driver) */
	return x45_set(fd, X45_SET_SRC4392_VOLUME, clamp100(digital), clamp100(digital));
}

int x45_volume_mode(int fd, int fixed)
{
	int r = x45_set(fd, X45_SET_ANALOG_VOL_TYPE, fixed ? 1 : 0, 0);

	if (r < 0)
		return r;
	return x45_set(fd, X45_SET_DIGITAL_VOL_TYPE, fixed ? 1 : 0, 0);
}

int x45_out_lineout(int fd, int on)   { return x45_set(fd, X45_SET_LINEOUT,   !!on, 0); }
int x45_out_xlr(int fd, int on)       { return x45_set(fd, X45_SET_XLROUT,    !!on, 0); }
int x45_out_headphone(int fd, int on) { return x45_set(fd, X45_SET_HEADPHONE, !!on, 0); }
int x45_out_aes_ebu(int fd, int on)   { return x45_set(fd, X45_SET_AES_EBU,   !!on, 0); }
int x45_out_spdif(int fd, int on)     { return x45_set(fd, X45_SET_SPDIF,     !!on, 0); }
int x45_out_optical(int fd, int on)   { return x45_set(fd, X45_SET_OPTICAL,   !!on, 0); }

int x45_outputs_apply(int fd)
{
	return x45_set(fd, X45_SET_SRC4392_OUTPUT, 0, 0);
}

/* ----------------------------------------------------------------- tables --- */

/* x45_SRC4392VolumeTable: 0 -> 0xFF mute; 1..30 -> 0x80..0x46 step -2;
 * 31..100 -> 0x45..0x00 step -1. Written to SRC4392 0x30/0x31. */
uint8_t x45_src4392_vol_reg(int idx)
{
	if (idx <= 0)
		return 0xFF;
	if (idx > 100)
		idx = 100;
	if (idx <= 30)
		return (uint8_t)(0x80 - 2 * (idx - 1));
	return (uint8_t)(0x45 - (idx - 31));
}

/* x45_ES9018VolumeTable: 0 -> 0xFF mute; 1..50 -> 0x94..0x32 step -2;
 * 51..100 -> 0x31..0x00 step -1. Written to 0x0F/0x10 on BOTH ES9018s. */
uint8_t x45_es9018_vol_reg(int idx)
{
	if (idx <= 0)
		return 0xFF;
	if (idx > 100)
		idx = 100;
	if (idx <= 50)
		return (uint8_t)(0x94 - 2 * (idx - 1));
	return (uint8_t)(0x31 - (idx - 51));
}

double x45_src4392_vol_db(int idx)
{
	if (idx <= 0)
		return -1000.0; /* mute */
	if (idx > 100)
		idx = 100;
	if (idx <= 30)
		return -64.0 + 1.0 * (idx - 1);
	return -34.5 + 0.5 * (idx - 31);
}

double x45_es9018_vol_db(int idx)
{
	if (idx <= 0)
		return -1000.0; /* mute */
	if (idx > 100)
		idx = 100;
	if (idx <= 50)
		return -74.0 + 1.0 * (idx - 1);
	return -24.5 + 0.5 * (idx - 51);
}

/* ------------------------------------------------------------------ names --- */

static const char *const input_names[] = {
	"network", "fmradio", "pcusb", "aes-ebu", "coaxial",
	"optical", "linein",  "auxin", "phono",   "pcusb2"
};

const char *x45_input_name(enum x45_input mode)
{
	if ((int)mode < 0 || (int)mode > 9)
		return "?";
	return input_names[(int)mode];
}

int x45_input_from_name(const char *name)
{
	int i;
	if (!name)
		return -1;
	for (i = 0; i <= 9; i++)
		if (!strcmp(name, input_names[i]))
			return i;
	/* accept a few aliases people actually type */
	if (!strcmp(name, "net") || !strcmp(name, "normal"))
		return X45_IN_NETWORK;
	if (!strcmp(name, "radio") || !strcmp(name, "fm") || !strcmp(name, "dab"))
		return X45_IN_FMRADIO;
	if (!strcmp(name, "usb") || !strcmp(name, "dsd"))
		return X45_IN_PCUSB;
	if (!strcmp(name, "aes") || !strcmp(name, "xlr-in"))
		return X45_IN_AES_EBU;
	if (!strcmp(name, "coax") || !strcmp(name, "spdif"))
		return X45_IN_COAXIAL;
	if (!strcmp(name, "toslink"))
		return X45_IN_OPTICAL;
	if (!strcmp(name, "line") || !strcmp(name, "line-in"))
		return X45_IN_LINEIN;
	if (!strcmp(name, "aux"))
		return X45_IN_AUXIN;
	return -1;
}

const char *x45_set_name(int cmd)
{
	switch (cmd) {
	case X45_SET_OUTPUT_RESET:     return "output_reset";
	case X45_SET_DSD_BITRATE:      return "dsd_bitrate";
	case X45_SET_PCM_RATE:         return "pcm_rate";
	case X45_SET_ES9018_VOLUME:    return "es9018_volume";
	case X45_SET_ANALOG_VOL_TYPE:  return "analog_vol_type";
	case X45_SET_MUTE:             return "mute";
	case X45_SET_VOL_QUALITY:      return "vol_quality";
	case X45_SET_INPUT_MODE:       return "input_mode";
	case X45_SET_SRC4392_VOLUME:   return "src4392_volume";
	case X45_SET_AES_EBU:          return "aes_ebu";
	case X45_SET_SPDIF:            return "spdif";
	case X45_SET_AUDIO_POWER:      return "audio_power";
	case X45_SET_DIGITAL_VOL_TYPE: return "digital_vol_type";
	case X45_SET_OPTICAL:          return "optical";
	case X45_SET_LINEOUT:          return "lineout";
	case X45_SET_OUTPUT_VOLUME:    return "output_volume";
	case X45_SET_NJU72341_VOLUME:  return "nju72341_volume";
	case X45_SET_SRC4392_OUTPUT:   return "src4392_output";
	case X45_SET_XLROUT:           return "xlrout";
	case X45_SET_HEADPHONE:        return "headphone";
	case X45_SET_IS_MQA:           return "is_mqa";
	case X45_SET_ES9018_FILTER:    return "es9018_filter";
	case X45_SET_AUDIO_MODE:       return "audio_mode";
	default:                       return NULL;
	}
}

const char *x45_get_name(int cmd)
{
	switch (cmd) {
	case X45_GET_HEADPHONE:   return "headphone_status";
	case X45_GET_CS8422_RATE: return "cs8422_sample_rate";
	case X45_GET_PCUSB:       return "pcusb_status";
	case X45_GET_CACHED:      return "cached_usbdac_word";
	default:                  return NULL;
	}
}
