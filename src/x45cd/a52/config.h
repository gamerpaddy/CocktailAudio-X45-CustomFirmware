/* config.h for liba52 (a52dec 0.7.4): no autoconf here. The other files in this directory are not in the
 * repository: scripts/prepare-src.sh unpacks the release archive that
 * scripts/fetch.py downloads and applies patches/a52dec-0.7.4-x45.patch. See docs/THIRD-PARTY.md. */
#ifndef X45_A52_CONFIG_H
#define X45_A52_CONFIG_H

/* the library predates today's compilers' warnings; the rest of x45cd is built with -Wall -Wextra */
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Warray-bounds"
#pragma GCC diagnostic ignored "-Wshift-negative-value"

/* a52.h uses uint32_t without including anything */
#include <stdint.h>

#endif
