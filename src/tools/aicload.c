/* aicload <usbdev path> <fwdir> [chipinfo | stage 1..4]
 * Downloads the AIC8800D80 firmware over the boot-ROM bulk interface (a69c:8d80),
 * a userspace port of aicfw_download_fw_8800d80() from the Linux aic8800 driver.
 * With "chipinfo" it only reads the chip id and stops. */
#include <errno.h>
#include <fcntl.h>
#include <linux/usbdevice_fs.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int fd;
static const char *fwdir;
#define EP_OUT 0x01
#define EP_IN 0x82

static int bulk(int ep, void *buf, int len, int tmo)
{
	struct usbdevfs_bulktransfer b = { .ep = ep, .len = len, .timeout = tmo, .data = buf };

	return ioctl(fd, USBDEVFS_BULK, &b);
}

static void put16(uint8_t *p, unsigned v) { p[0] = v; p[1] = v >> 8; }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }
static uint32_t get32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* send a DBG task message; wait for cfm id when cfm != 0, copy cfm param to out */
static int msg(unsigned id, const void *par, unsigned plen, unsigned cfm, uint8_t *out)
{
	static uint8_t tx[2048];
	uint8_t rx[2048];
	unsigned len = plen + 8, n;
	int r, tries;

	memset(tx, 0, sizeof tx);
	put16(tx, len + 4);
	tx[1] &= 0x0f;
	tx[2] = 0x11;
	put16(tx + 8, id);
	put16(tx + 10, 1);	/* TASK_DBG */
	put16(tx + 12, 100);	/* DRV_TASK_ID */
	put16(tx + 14, plen);
	memcpy(tx + 16, par, plen);
	n = len + 8;
	r = bulk(EP_OUT, tx, (int)n, 3000);
	if (r < 0)
		return fprintf(stderr, "tx id %x: %s\n", id, strerror(errno)), -1;
	if (!cfm)
		return 0;
	for (tries = 0; tries < 8; tries++) {
		r = bulk(EP_IN, rx, sizeof rx, 3000);
		if (r < 0)
			return fprintf(stderr, "rx for %x: %s\n", cfm, strerror(errno)), -1;
		if (r >= 16 && (rx[2] & 0x7f) == 0x11 /* USB_TYPE_CFG_CMD_RSP, see aic_txrxif.h */ ) {
			unsigned rid = rx[4] | rx[5] << 8, pl = rx[10] | rx[11] << 8;

			if (rid == cfm) {
				if (out)
					memcpy(out, rx + 16, pl > 32 ? 32 : pl);
				return 0;
			}
		}
	}
	fprintf(stderr, "no cfm %x\n", cfm);
	return -1;
}

static int mem_read(uint32_t a, uint32_t *v)
{
	uint8_t p[4], o[32];

	put32(p, a);
	if (msg(0x400, p, 4, 0x401, o))
		return -1;
	*v = get32(o + 4);
	return 0;
}

static int mem_write(uint32_t a, uint32_t v)
{
	uint8_t p[8];

	put32(p, a);
	put32(p + 4, v);
	return msg(0x402, p, 8, 0x403, NULL);
}

static uint8_t *readfile(const char *name, long *sz)
{
	char path[512];
	FILE *f;
	uint8_t *b;

	snprintf(path, sizeof path, "%s/%s", fwdir, name);
	f = fopen(path, "rb");
	if (!f)
		return fprintf(stderr, "open %s\n", path), NULL;
	fseek(f, 0, SEEK_END);
	*sz = ftell(f);
	rewind(f);
	b = malloc(*sz + 4);
	if (fread(b, 1, *sz, f) != (size_t)*sz)
		return NULL;
	fclose(f);
	return b;
}

static int upload(uint32_t addr, const char *name)
{
	long sz, i;
	uint8_t *b = readfile(name, &sz), p[1032];

	if (!b)
		return -1;
	fprintf(stderr, "upload %s @%x size=%ld\n", name, addr, sz);
	for (i = 0; i < sz; i += 1024) {
		long n = sz - i > 1024 ? 1024 : sz - i;

		memset(p, 0, sizeof p);
		put32(p, addr + (uint32_t)i);
		put32(p + 4, (uint32_t)n);
		memcpy(p + 8, b + i, n);
		if (msg(0x40b, p, 1032, 0x40c, NULL))
			return fprintf(stderr, "block fail at %lx\n", (long)addr + i), -1;
	}
	free(b);
	return 0;
}

int main(int argc, char **argv)
{
	uint32_t v;
	unsigned chip, ifn = 0;
	long sz;
	uint8_t *pt;

	if (argc < 3)
		return fprintf(stderr, "usage: aicload /dev/bus/usb/BBB/DDD fwdir [chipinfo | stage 1..4]\n"), 2;
	fwdir = argv[2];
	fd = open(argv[1], O_RDWR);
	if (fd < 0)
		return perror("open"), 1;
	if (ioctl(fd, USBDEVFS_CLAIMINTERFACE, &ifn) < 0)
		perror("claim");
	if (mem_read(0x40500000, &v))
		return 1;
	chip = (uint8_t)(v >> 16);
	fprintf(stderr, "reg 40500000=%08x chip_id=%x mcu_id=%d\n", v, chip, !((v >> 25) & 1));
	if (argc > 3 && !strcmp(argv[3], "chipinfo"))
		return 0;
	if (chip != 3 && chip != 7) {
		fprintf(stderr, "chip id %x: this port only handles U02/U03 files\n", chip);
		return 1;
	}
	{
		uint32_t base = 0x120000, w[3], cfgbase, strbase, ver, pbuf, startaddr = 0x001D7000;
		uint8_t *p, *end;
		uint32_t *inf = NULL, addr_adid, addr_patch, extn = 0;
		int stage = argc > 3 ? atoi(argv[3]) : 99;
		char name[64];
		int i;

		(void)w;
		pt = readfile("fw_patch_table_8800d80_u02.bin", &sz);
		if (!pt || memcmp(pt, "AICBT_PT_TAG", 12))
			return fprintf(stderr, "bad patch table\n"), 1;
		end = pt + sz;
		p = pt + 16;
		/* INF record: 4 base pairs (+1 pair ext nb if len > 4), then ext patch (id,addr) pairs */
		inf = (uint32_t *)(void *)(p + 24);
		addr_adid = inf[1];
		addr_patch = inf[3];
		if (get32(p + 20) > 4)
			extn = inf[9];
		fprintf(stderr, "adid@%x patch@%x ext=%u\n", addr_adid, addr_patch, extn);

		if (upload(addr_adid, "fw_adid_8800d80_u02.bin") || upload(addr_patch, "fw_patch_8800d80_u02.bin"))
			return 1;
		for (i = 0; i < (int)extn; i++) {
			snprintf(name, sizeof name, "fw_patch_8800d80_u02_ext%u.bin", inf[10 + 2 * i]);
			if (upload(inf[11 + 2 * i], name))
				return 1;
		}
		if (stage < 2)
			return fprintf(stderr, "stopped after patch uploads\n"), 0;

		/* apply the patch-table records */
		while (p < end) {
			uint32_t type = get32(p + 16), len = get32(p + 20);
			uint32_t *d = (uint32_t *)(void *)(p + 24);

			if (type >= 1000 || len == 0) {
				len = 0;
			}
			if (type == 0 && get32(p + 20) > 4)
				len = 4;	/* INF: only the base pairs are written */
			if (type == 3 && len >= 9) {	/* BTMODE */
				d[1] = 1;		/* hwinfo < 0 */
				d[3] = 0xffffffffu;	/* hwinfo */
				d[5] = 0;		/* cpmode work */
				d[7] = getenv("AICBTMODE") ? (uint32_t)atoi(getenv("AICBTMODE")) : 5;	/* default BT_ONLY_COANT */
				d[9] = 1;		/* port: mailbox */
				d[11] = 1500000;	/* uart baud */
				d[13] = 1;		/* flow control */
				d[15] = 0;		/* lpm */
				d[17] = 0x6F2F;		/* tx power */
			}
			fprintf(stderr, "table %-16.16s type %u len %u\n", (char *)p, type, len);
			if (type != 6) {
				for (i = 0; i < (int)len; i++)
					if (mem_write(d[2 * i], d[2 * i + 1]))
						return fprintf(stderr, "table write %x fail\n", d[2 * i]), 1;
			}
			if (type == 4)
				usleep(100000);
			p += 24 + (type >= 1000 || get32(p + 20) == 0 ? 0 : get32(p + 20) * 8);
		}
		if (stage < 3)
			return fprintf(stderr, "stopped after table\n"), 0;

		if (upload(base, (chip & 0xC0) == 0xC0 ? "fmacfw_8800d80_h_u02.bin" : "fmacfw_8800d80_u02.bin"))
			return 1;

		/* wifi patch config (aicwf_patch_config_8800d80) */
		if (mem_read(base + 0x198, &cfgbase) || mem_read(base + 0x1a0, &strbase) || mem_read(base + 0x1c, &ver))
			return 1;
		fprintf(stderr, "cfgbase=%x strbase=%x ver=%08x\n", cfgbase, strbase, ver);
		if (ver > 0x06090100) {
			if (mem_read(base + 0x198 + 12, &pbuf))
				return 1;
			startaddr = pbuf;
		}
		/* aic_patch_t: magic, pair_start, magic2, pair_count, block_dst[4], block_src[4], block_size[4] */
		if (mem_write(strbase + 0, 0x48435450) || mem_write(strbase + 8, 0x50544348) ||
		    mem_write(strbase + 4, startaddr) || mem_write(strbase + 12, 3))
			return 1;
		{
			static const uint32_t t[3][2] = { { 0x00b4, 0xf3010000 }, { 0x0170, 0x0001000A }, { 0x0188, 0x1 } };

			for (i = 0; i < 3; i++)
				if (mem_write(startaddr + 8 * i, t[i][0] + cfgbase) || mem_write(startaddr + 8 * i + 4, t[i][1]))
					return 1;
		}
		for (i = 0; i < 4; i++)
			if (mem_write(strbase + 16 + 16 + 16 + 4 * i - 0, 0))	/* block_size[i] at +0x20..: see struct */
				return 1;
		if (stage < 4)
			return fprintf(stderr, "stopped before start_app\n"), 0;
		{
			uint8_t pa[8];

			put32(pa, base);
			put32(pa + 4, 1);
			if (msg(0x40d, pa, 8, 0, NULL))
				return 1;
		}
		fprintf(stderr, "START_APP sent\n");
	}
	return 0;
}
