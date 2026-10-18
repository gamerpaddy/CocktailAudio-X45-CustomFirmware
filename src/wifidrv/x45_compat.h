/* Pulled into every fdrv source with -include: things the 3.14 kernel headers lack. */
#ifndef X45_COMPAT_H
#define X45_COMPAT_H
#include <linux/ktime.h>
#include <linux/ieee80211.h>
#include <linux/etherdevice.h>

#ifndef ktime_ms_delta
#define ktime_ms_delta(later, earlier) ktime_to_ms(ktime_sub((later), (earlier)))
#endif

#ifndef WLAN_CIPHER_SUITE_GCMP_256
#define WLAN_CIPHER_SUITE_GCMP_256	0x000FAC09
#define WLAN_CIPHER_SUITE_CCMP_256	0x000FAC0A
#define WLAN_CIPHER_SUITE_BIP_GMAC_128	0x000FAC0B
#define WLAN_CIPHER_SUITE_BIP_GMAC_256	0x000FAC0C
#define WLAN_CIPHER_SUITE_BIP_CMAC_256	0x000FAC0D
#endif
#endif
