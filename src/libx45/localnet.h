#ifndef X45_LOCALNET_H
#define X45_LOCALNET_H
#include <stdint.h>
/* IPv4 in host byte order. Inspect the socket peer, never proxy headers. */
static inline int x45_local_ipv4(uint32_t ip)
{
    return (ip >> 24) == 10 || (ip >> 24) == 127 ||
           (ip & 0xfff00000U) == 0xac100000U ||
           (ip & 0xffff0000U) == 0xc0a80000U ||
           (ip & 0xffff0000U) == 0xa9fe0000U;
}
#endif
