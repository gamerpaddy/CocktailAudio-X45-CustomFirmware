#!/bin/bash
# Cross-build the optional Bluetooth parts: x45bt (an A2DP audio sink on top of BTstack, talking to a USB Bluetooth
# stick through libusb), x45btsrc (the A2DP source that sends the unit's sound to a headset) and the two small tools
# that wake the stick up (aicload, sgsend).
#
#   scripts/bt.sh
#
# Output: $OUT/x45bt, $OUT/x45btsrc, $OUT/aicload, $OUT/sgsend
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
	# the third patch adds the Bluetooth output daemon (a new source file, its make rule, a log line in the HFP code)
	patch -p1 <"$PATCHES/btstack-e385539-x45btsrc.patch" >/dev/null
fi

log "x45bt"
cd port/libusb
rm -f x45bt x45btsrc
CFLAGS="-DX45_NO_USB_RESET -O2 $ARCHFLAGS -I$SYSROOT/include -w" LDFLAGS="-static -L$SYSROOT/lib" \
	make x45bt x45btsrc CC="$CC" >/dev/null
mkdir -p "$OUT"
"$STRIP" -o "$OUT/x45bt" x45bt
"$STRIP" -o "$OUT/x45btsrc" x45btsrc

log "aicload, sgsend"
make -C "$SRCDIR" OUT="$OUT" CROSS="$CROSS" "$OUT/aicload" "$OUT/sgsend" >/dev/null
ls -la "$OUT/x45bt" "$OUT/x45btsrc" "$OUT/aicload" "$OUT/sgsend"
