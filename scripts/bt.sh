#!/bin/bash
# Cross-build the optional Bluetooth parts: x45bt (an A2DP audio sink on top of BTstack, talking to a USB Bluetooth
# stick through libusb) and the two small tools that wake the stick up (aicload, sgsend).
#
#   scripts/bt.sh
#
# Output: $OUT/x45bt, $OUT/aicload, $OUT/sgsend
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"
need_tools make gcc patch tar
[ -f "$SYSROOT/.done-libusb" ] || die "libusb is not built (run scripts/deps.sh libusb)"
cross_env
export PATH="$HOSTROOT/bin:$PATH"

d=$(unpack btstack-e385539.tar.gz)
cd "$d"
if [ ! -f example/x45bt.c ]; then
	log "patching BTstack"
	# x45bt starts as a copy of BTstack's own A2DP sink demo, which our patch then rewrites; the second patch changes
	# BTstack itself (USB reset handling, audio, a few hooks)
	cp example/a2dp_sink_demo.c example/x45bt.c
	patch -p1 <"$PATCHES/btstack-e385539-x45bt.c.patch" >/dev/null
	patch -p1 <"$PATCHES/btstack-e385539-x45.patch" >/dev/null
fi

log "x45bt"
cd port/libusb
rm -f x45bt
CFLAGS="-DX45_NO_USB_RESET -O2 $ARCHFLAGS -I$SYSROOT/include -w" LDFLAGS="-static -L$SYSROOT/lib" \
	make x45bt CC="$CC" >/dev/null
mkdir -p "$OUT"
"$STRIP" -o "$OUT/x45bt" x45bt

log "aicload, sgsend"
make -C "$SRCDIR" OUT="$OUT" CROSS="$CROSS" "$OUT/aicload" "$OUT/sgsend" >/dev/null
ls -la "$OUT/x45bt" "$OUT/aicload" "$OUT/sgsend"
