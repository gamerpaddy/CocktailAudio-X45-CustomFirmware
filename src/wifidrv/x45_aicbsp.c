// SPDX-License-Identifier: GPL-2.0
/*
 * Minimal replacement for the vendor "aic_load_fw" module.
 *
 * The AIC8800 cfg80211 driver (aic8800_fdrv) imports a handful of helpers from the vendor's
 * firmware-loader module, which on a normal system also owns the boot-ROM USB interface and
 * downloads the firmware. On the X45 the firmware is downloaded from userspace (aicload,
 * shared with the Bluetooth bring-up), so only the helper functions are needed here.
 */
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>

typedef struct { int8_t enable, dsss, ofdmlowrate_2g4, ofdm64qam_2g4, ofdm256qam_2g4,
	ofdm1024qam_2g4, ofdmlowrate_5g, ofdm64qam_5g, ofdm256qam_5g, ofdm1024qam_5g; } txpwr_idx_conf_t;
typedef struct { int8_t enable, chan_1_4, chan_5_9, chan_10_13, chan_36_64, chan_100_120,
	chan_122_140, chan_142_165; } txpwr_ofst_conf_t;

static char fw_path[200] = "/home/root/x45new/wifi/fw";
module_param_string(fw_path, fw_path, sizeof(fw_path), 0644);
MODULE_PARM_DESC(fw_path, "directory with the AIC firmware / userconfig files");

static int testmode;
static txpwr_idx_conf_t txpwr_idx;	/* all zero: the chip's own defaults */
static txpwr_ofst_conf_t txpwr_ofst;

void get_fw_path(char *path)
{
	strlcpy(path, fw_path, 200);
}
EXPORT_SYMBOL(get_fw_path);

void set_testmode(int val) { testmode = val; }
EXPORT_SYMBOL(set_testmode);

int get_testmode(void) { return testmode; }
EXPORT_SYMBOL(get_testmode);

int get_adap_test(void) { return 0; }
EXPORT_SYMBOL(get_adap_test);

int get_flash_bin_size(void) { return 0; }
EXPORT_SYMBOL(get_flash_bin_size);

u32 get_flash_bin_crc(void) { return 0; }
EXPORT_SYMBOL(get_flash_bin_crc);

void get_userconfig_txpwr_idx(txpwr_idx_conf_t *p) { *p = txpwr_idx; }
EXPORT_SYMBOL(get_userconfig_txpwr_idx);

void get_userconfig_txpwr_ofst(txpwr_ofst_conf_t *p) { *p = txpwr_ofst; }
EXPORT_SYMBOL(get_userconfig_txpwr_ofst);

static void *txq_buf;
static size_t txq_size;

void *aicwf_prealloc_txq_alloc(size_t size)
{
	if (txq_buf && txq_size != size) {
		kfree(txq_buf);
		txq_buf = NULL;
	}
	if (!txq_buf) {
		txq_buf = kzalloc(size, GFP_KERNEL);
		txq_size = size;
	}
	return txq_buf;
}
EXPORT_SYMBOL(aicwf_prealloc_txq_alloc);

static void __exit x45_aicbsp_exit(void)
{
	kfree(txq_buf);
}
static int __init x45_aicbsp_init(void) { return 0; }

module_init(x45_aicbsp_init);
module_exit(x45_aicbsp_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("AIC8800 helper exports for the X45 (userspace firmware load)");
