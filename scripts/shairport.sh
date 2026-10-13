#!/bin/bash
# Cross-build shairport-sync (classic AirPlay receiver) for the unit: static musl.
#
#   scripts/shairport.sh        -> $WORK/shairport-sync-4.3.4/shairport-sync.stripped
#
# Classic AirPlay only. AirPlay 2 needs OpenSSL, Avahi, FFmpeg and nqptp, none of which this build carries; iOS and
# macOS still stream to classic AirPlay. The built-in mDNS responder advertises the receiver, so no Avahi is needed.
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"
export PATH="$HOSTROOT/bin:$PATH"
need_tools make gcc autoconf patch tar
[ -f "$SYSROOT/.done-alac" ] || die "run scripts/deps.sh first"
cross_env

# musl's 128 KiB thread stacks are too small for shairport-sync (written for glibc's 8 MiB); the force-included
# header gives every thread 1 MiB. It is copied next to the build tree so the path has no spaces in it.
cp -f "$PATCHES/x45-pthread-stack.h" "$WORK/x45-pthread-stack.h"
# not part of CFLAGS during configure: pulling <pthread.h> into every probe makes autoconf's own tests fail
X45_INCLUDE="-include $WORK/x45-pthread-stack.h"
# X45_CRASH=1 scripts/shairport.sh reconf : add a crash reporter (patches/x45-crash.h) and debug info
if [ "${X45_CRASH:-}" = 1 ]; then
	cp -f "$PATCHES/x45-crash.h" "$WORK/x45-crash.h"
	X45_INCLUDE="$X45_INCLUDE -g -DX45_CRASH_TRACE -include $WORK/x45-crash.h"
fi
export LDFLAGS="-L$SYSROOT/lib -static"

SRC=$(unpack shairport-sync-4.3.4.tar.gz)
cd "$SRC"
[ -f configure ] || autoreconf -fi >/dev/null 2>&1

if [ ! -f Makefile ] || [ "${1:-}" = reconf ]; then
	# static libmbedtls needs its siblings after it; configure's checks link one library at a time and each prepends
	# its own -l, so what is in LIBS here ends up trailing
	LIBS="-lmbedx509 -lmbedcrypto -lm -ldl -lpthread -lstdc++" \
	./configure --host=$HOST --prefix="$DEVROOT" --sysconfdir="$DEVROOT/etc" \
		--with-alsa --with-ssl=mbedtls --with-tinysvcmdns --with-pkg-config \
		--with-apple-alac \
		--with-piddir=/var/run >/dev/null
fi

# command line CFLAGS replace the Makefile's, so they restate the base flags; touch the sources so a change of the
# header or of these flags is picked up
find . -maxdepth 1 \( -name '*.c' -o -name '*.cpp' \) -exec touch {} +
make -j"$JOBS" CFLAGS="$CFLAGS $X45_INCLUDE" CXXFLAGS="$CXXFLAGS $X45_INCLUDE" >/dev/null
"$STRIP" -o shairport-sync.stripped shairport-sync
ls -la shairport-sync.stripped
