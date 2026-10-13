/* x45ctl  -  drive the Cocktail Audio X45 audio board from the command line.
 *
 * This is the hardware-abstraction layer for an x20 replacement: everything
 * the vendor application does to the audio board, it does through the same
 * /dev/x45 ioctls this wraps. Built static against musl so it has no
 * dependency on the device's rootfs.
 */
#define _GNU_SOURCE
#include "../libx45/x45.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SYSFS_DRV "/sys/bus/platform/drivers/imx-x45"

static int verbose;

static void die(const char *what, int err)
{
	fprintf(stderr, "x45ctl: %s: %s\n", what, strerror(err < 0 ? -err : err));
	exit(1);
}

static void chk(const char *what, int r)
{
	if (r < 0)
		die(what, r);
	if (verbose)
		fprintf(stderr, "  %s: ok\n", what);
}

static int parse_onoff(const char *s)
{
	if (!s)
		return -1;
	if (!strcmp(s, "on") || !strcmp(s, "1") || !strcmp(s, "yes") ||
	    !strcmp(s, "true") || !strcmp(s, "enable"))
		return 1;
	if (!strcmp(s, "off") || !strcmp(s, "0") || !strcmp(s, "no") ||
	    !strcmp(s, "false") || !strcmp(s, "disable"))
		return 0;
	return -1;
}

/* Read one of the five writable status latches the machine driver exposes.
 * They read "detected" or "cleared". */
static const char *read_sysfs(const char *name, char *buf, size_t n)
{
	char path[256];
	FILE *f;
	size_t got;

	snprintf(path, sizeof path, "%s/%s", SYSFS_DRV, name);
	f = fopen(path, "r");
	if (!f)
		return "(absent)";
	got = fread(buf, 1, n - 1, f);
	fclose(f);
	buf[got] = 0;
	while (got && (buf[got - 1] == '\n' || buf[got - 1] == ' '))
		buf[--got] = 0;
	return buf[0] ? buf : "(empty)";
}

/* clock [<rate>|44|48]: show or select the XMOS master-clock family. A rate in
 * Hz picks the family it belongs to. Must be done before a stream opens; see
 * x45.h for why (the driver never does it, and 96 kHz at the wrong family plays
 * 8 % slow). */
static int cmd_clock(const char *arg)
{
	unsigned rate;
	int r, now = x45_clock_get();

	if (!arg) {
		if (now < 0) {
			fprintf(stderr, "x45ctl: clock: %s\n", strerror(-now));
			return 1;
		}
		printf("master clock: %s kHz family (FS_4844=%d)\n", now ? "48" : "44.1", now);
		return 0;
	}
	rate = (unsigned)strtoul(arg, NULL, 10);
	if (rate == 44)
		rate = 44100;
	else if (rate == 48)
		rate = 48000;
	if (x45_clock_family(rate) < 0) {
		fprintf(stderr, "x45ctl: clock: '%s' is not a sample rate or 44|48\n", arg);
		return 2;
	}
	r = x45_clock_select(rate);
	if (r < 0) {
		fprintf(stderr, "x45ctl: clock: %s\n", strerror(-r));
		return 1;
	}
	printf("master clock: %s kHz family (for %u Hz)\n",
	       x45_clock_family(rate) ? "48" : "44.1", rate);
	return 0;
}

static void usage(void)
{
	puts(
"x45ctl  -  control the X45 audio board via /dev/x45\n"
"\n"
"usage: x45ctl [-v] <command> [args]\n"
"\n"
"bring-up\n"
"  init [<input>]          power-cycle the board, select input (default network),\n"
"                          enable line-out + XLR, apply, unmute\n"
"  power on|off|cycle      SET 20. 'cycle' is what clears an I2C wedge;\n"
"                          'on' alone does not\n"
"\n"
"paths\n"
"  input <name|0-9>        network fmradio pcusb aes-ebu coaxial optical\n"
"                          linein auxin phono pcusb2\n"
"  volume <0-100>          master; drives both ES9018s and the SRC4392. Changes nothing\n"
"                          while the volume is fixed, which is how the driver starts\n"
"  balance <-24..24> [vol] master volume vol (default 100) with the right side louder by that many\n"
"                          steps (negative: the left); like the volume it needs the analogue stage\n"
"                          (SET 6) variable. x45ui overwrites it with its own setting at the next volume write\n"
"  voltype variable|fixed  the volume stages (SET 6 + 22); x45ui sets variable at start\n"
"  mute on|off\n"
"  out <which> on|off      lineout xlr headphone aes-ebu spdif optical\n"
"                          (implies apply; spdif = coax+optical together)\n"
"  clock [<rate>|44|48]    show / select the XMOS master-clock family (needs no\n"
"                          /dev/x45); set it before each stream\n"
"  apply                   SET 28  -  makes pending output enables take effect\n"
"  filter <n>              ES9018 digital filter select\n"
"  trim <0-21>             NJU72341 analogue gain, -12..+9 dB\n"
"\n"
"inspect\n"
"  status                  everything readable, in one shot\n"
"  get <19|29|35|36>       raw GET\n"
"  set <cmd> <v1> [v2]     raw SET  -  escape hatch for anything not above\n"
"  table [src4392|es9018]  print the vendor volume curve\n"
"\n"
"notes\n"
"  Input switching is a no-op while the board is powered down, and\n"
"  re-selecting the current input is a no-op.\n"
"  Output enables only store driver state until 'apply' runs.\n"
"  Power-cycling re-enumerates the XMOS on USB, so ALSA card indices move.");
}

static int cmd_status(int fd)
{
	static const char *const latches[] = {
		"analog_in_overflow", "digital_input", "headphone",
		"pcusb", "tca9554_intr"
	};
	char buf[64];
	int32_t v;
	size_t i;
	int r;

	puts("/dev/x45 GET:");
	for (i = 0; i < 4; i++) {
		static const int gets[] = { X45_GET_HEADPHONE, X45_GET_CS8422_RATE,
					    X45_GET_PCUSB, X45_GET_CACHED };
		int g = gets[i];
		r = x45_get(fd, g, &v);
		if (r < 0)
			printf("  %-20s GET %-2d  error: %s\n",
			       x45_get_name(g), g, strerror(-r));
		else
			printf("  %-20s GET %-2d  = %d (0x%08x)\n",
			       x45_get_name(g), g, v, (unsigned)v);
	}

	printf("\n%s:\n", SYSFS_DRV);
	for (i = 0; i < sizeof latches / sizeof *latches; i++)
		printf("  %-20s = %s\n", latches[i],
		       read_sysfs(latches[i], buf, sizeof buf));
	return 0;
}

static void cmd_table(const char *which)
{
	int both = !which || !strcmp(which, "all");
	int src = both || !strcmp(which, "src4392");
	int es  = both || !strcmp(which, "es9018");
	int i;

	printf("%-5s", "idx");
	if (src) printf("  %-8s %-9s", "SRC4392", "dB");
	if (es)  printf("  %-8s %-9s", "ES9018", "dB");
	putchar('\n');

	for (i = 0; i <= 100; i++) {
		printf("%-5d", i);
		if (src) {
			if (i == 0) printf("  0x%02X     %-9s", x45_src4392_vol_reg(i), "mute");
			else        printf("  0x%02X     %-9.1f", x45_src4392_vol_reg(i), x45_src4392_vol_db(i));
		}
		if (es) {
			if (i == 0) printf("  0x%02X     %-9s", x45_es9018_vol_reg(i), "mute");
			else        printf("  0x%02X     %-9.1f", x45_es9018_vol_reg(i), x45_es9018_vol_db(i));
		}
		putchar('\n');
	}
}

int main(int argc, char **argv)
{
	int fd, r, argi = 1;
	const char *cmd;

	if (argc > 1 && !strcmp(argv[1], "-v")) {
		verbose = 1;
		argi++;
	}
	if (argc <= argi) {
		usage();
		return 2;
	}
	cmd = argv[argi++];

	if (!strcmp(cmd, "-h") || !strcmp(cmd, "--help") || !strcmp(cmd, "help")) {
		usage();
		return 0;
	}
	/* table needs no device */
	if (!strcmp(cmd, "table")) {
		cmd_table(argi < argc ? argv[argi] : NULL);
		return 0;
	}
	/* neither does clock: it is a GPIO write through /proc/x45/gpio */
	if (!strcmp(cmd, "clock"))
		return cmd_clock(argi < argc ? argv[argi] : NULL);

	fd = x45_open();
	if (fd < 0)
		die("open /dev/x45", fd);

	if (!strcmp(cmd, "init")) {
		int in = X45_IN_NETWORK;
		if (argi < argc) {
			in = x45_input_from_name(argv[argi]);
			if (in < 0) {
				in = atoi(argv[argi]);
				if (in < 0 || in > 9) {
					fprintf(stderr, "x45ctl: unknown input '%s'\n", argv[argi]);
					return 2;
				}
			}
		}
		fprintf(stderr, "x45ctl: bringing up (input=%s), ~2 s ...\n",
			x45_input_name(in));
		chk("bringup", x45_bringup(fd, in));
		puts("up");
	} else if (!strcmp(cmd, "power")) {
		if (argi >= argc) { usage(); return 2; }
		if (!strcmp(argv[argi], "cycle")) {
			fprintf(stderr, "x45ctl: power cycling, ~2 s ...\n");
			chk("power cycle", x45_power_cycle(fd));
		} else {
			int on = parse_onoff(argv[argi]);
			if (on < 0) { usage(); return 2; }
			chk("power", x45_power(fd, on));
		}
	} else if (!strcmp(cmd, "input")) {
		int in;
		if (argi >= argc) { usage(); return 2; }
		in = x45_input_from_name(argv[argi]);
		if (in < 0) {
			in = atoi(argv[argi]);
			if (in < 0 || in > 9) {
				fprintf(stderr, "x45ctl: unknown input '%s'\n", argv[argi]);
				return 2;
			}
		}
		chk("input", x45_input(fd, in));
		printf("input = %s (%d)\n", x45_input_name(in), in);
	} else if (!strcmp(cmd, "volume")) {
		int v;
		char *end;
		if (argi >= argc) { usage(); return 2; }
		v = (int)strtol(argv[argi], &end, 10);
		if (*end || end == argv[argi] || v < 0 || v > 100) {
			fprintf(stderr, "x45ctl: volume is 0-100, not '%s'\n", argv[argi]);
			return 2;
		}
		chk("volume", x45_volume(fd, v));
		printf("volume = %d  (SRC4392 0x%02X / %.1f dB, ES9018 0x%02X / %.1f dB)\n",
		       v, x45_src4392_vol_reg(v), x45_src4392_vol_db(v),
		       x45_es9018_vol_reg(v), x45_es9018_vol_db(v));
	} else if (!strcmp(cmd, "balance")) {
		int b, v = 100, l, r;
		char *end;
		if (argi >= argc) { usage(); return 2; }
		b = (int)strtol(argv[argi], &end, 10);
		if (*end || end == argv[argi] || b < -X45_BALANCE_MAX || b > X45_BALANCE_MAX) {
			fprintf(stderr, "x45ctl: balance is -%d..%d, not '%s'\n", X45_BALANCE_MAX, X45_BALANCE_MAX, argv[argi]);
			return 2;
		}
		if (argi + 1 < argc) {
			v = (int)strtol(argv[argi + 1], &end, 10);
			if (*end || end == argv[argi + 1] || v < 0 || v > 100) {
				fprintf(stderr, "x45ctl: volume is 0-100, not '%s'\n", argv[argi + 1]);
				return 2;
			}
		}
		x45_balance_levels(v, b, &l, &r);
		chk("volume", x45_volume(fd, v));
		chk("balance", x45_volume_lr(fd, l, r, v));
		printf("balance = %d at volume %d: left %d (ES9018 0x%02X), right %d (ES9018 0x%02X)\n",
		       b, v, l, x45_es9018_vol_reg(l), r, x45_es9018_vol_reg(r));
	} else if (!strcmp(cmd, "voltype")) {
		if (argi >= argc || (strcmp(argv[argi], "variable") && strcmp(argv[argi], "fixed"))) {
			usage();
			return 2;
		}
		chk("voltype", x45_volume_mode(fd, !strcmp(argv[argi], "fixed")));
	} else if (!strcmp(cmd, "mute")) {
		int on;
		if (argi >= argc) { usage(); return 2; }
		on = parse_onoff(argv[argi]);
		if (on < 0) { usage(); return 2; }
		chk("mute", x45_mute(fd, on));
	} else if (!strcmp(cmd, "out")) {
		const char *which;
		int on;
		if (argi + 1 >= argc) { usage(); return 2; }
		which = argv[argi++];
		on = parse_onoff(argv[argi]);
		if (on < 0) { usage(); return 2; }
		if      (!strcmp(which, "lineout"))    chk("lineout", x45_out_lineout(fd, on));
		else if (!strcmp(which, "xlr"))        chk("xlr", x45_out_xlr(fd, on));
		else if (!strcmp(which, "headphone")) chk("headphone", x45_out_headphone(fd, on));
		else if (!strcmp(which, "aes-ebu"))    chk("aes-ebu", x45_out_aes_ebu(fd, on));
		else if (!strcmp(which, "spdif"))      chk("spdif", x45_out_spdif(fd, on));
		else if (!strcmp(which, "optical"))    chk("optical", x45_out_optical(fd, on));
		else { fprintf(stderr, "x45ctl: unknown output '%s'\n", which); return 2; }
		chk("apply", x45_outputs_apply(fd));
	} else if (!strcmp(cmd, "apply")) {
		chk("apply", x45_outputs_apply(fd));
	} else if (!strcmp(cmd, "filter")) {
		if (argi >= argc) { usage(); return 2; }
		chk("filter", x45_set(fd, X45_SET_ES9018_FILTER, atoi(argv[argi]), 0));
	} else if (!strcmp(cmd, "trim")) {
		char *end;
		long v;
		if (argi >= argc) { usage(); return 2; }
		v = strtol(argv[argi], &end, 10);
		if (*end || end == argv[argi] || v < 0 || v > 21) {
			fprintf(stderr, "x45ctl: trim is 0-21, not '%s'\n", argv[argi]);
			return 2;
		}
		chk("trim", x45_set(fd, X45_SET_NJU72341_VOLUME, (int32_t)v, 0));
	} else if (!strcmp(cmd, "status")) {
		return cmd_status(fd);
	} else if (!strcmp(cmd, "get")) {
		int32_t v;
		int g;
		if (argi >= argc) { usage(); return 2; }
		g = atoi(argv[argi]);
		r = x45_get(fd, g, &v);
		if (r < 0) die("get", r);
		printf("%d\n", v);
	} else if (!strcmp(cmd, "set")) {
		int c, v1 = 0, v2 = 0;
		if (argi >= argc) { usage(); return 2; }
		c = atoi(argv[argi++]);
		if (argi < argc) v1 = atoi(argv[argi++]);
		if (argi < argc) v2 = atoi(argv[argi++]);
		r = x45_set(fd, c, v1, v2);
		if (r < 0) die("set", r);
		if (verbose) {
			const char *n = x45_set_name(c);
			fprintf(stderr, "  SET %d (%s) %d %d: ok\n", c, n ? n : "?", v1, v2);
		}
	} else {
		fprintf(stderr, "x45ctl: unknown command '%s'\n\n", cmd);
		usage();
		x45_close(fd);
		return 2;
	}

	x45_close(fd);
	return 0;
}
