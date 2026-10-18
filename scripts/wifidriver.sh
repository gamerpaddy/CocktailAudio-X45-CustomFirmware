#!/bin/bash
# Build the kernel driver for the AIC8800 Wi-Fi stick as modules for the unit's own kernel (3.14.52).
#
#   scripts/kernelsyms.sh root@ADDRESS-OF-THE-UNIT     once: reads two files from your unit (see that script)
#   scripts/wifidriver.sh                              builds the modules and the firmware folder
#   ./build.sh stage ; ./make-pkg.sh                   the package now carries them (stage.sh picks them up)
#
# Output: $OUT/wifi/mod/x45_aicbsp.ko, $OUT/wifi/mod/aic8800_fdrv.ko and $OUT/wifi/fw/ (firmware and settings files
# of the chip, copied from the driver sources).
#
# Why it is a separate step: the modules only load into a kernel with the same configuration and the same symbol
# checksums, and those cannot be downloaded. The kernel configuration and the checksums come from your own unit. The
# kernel sources used for the build are NXP's public linux-imx 3.14.52 (the unit runs a slightly changed copy of it);
# only its headers and build rules are used, the kernel itself is not rebuilt.
#
# What gets built:
#   x45_aicbsp     src/wifidrv/x45_aicbsp.c: the few helper functions the stick's driver imports from a loader module of
#                  the original driver set; here the firmware is sent to the stick by aicload (scripts/bt.sh), so the
#                  loader module is not needed
#   aic8800_fdrv   the cfg80211 driver of the stick (radxa-pkg/aic8800, pinned commit), with patches/aic8800-d13d079-x45.patch
#                  and src/wifidrv/x45_compat.h (things the 3.14 headers lack)
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"
need_tools make gcc patch tar python3
[ -x "${CROSS}gcc" ] || die "the cross compiler is missing (run scripts/toolchain.sh)"

KDATA="${X45_KERNEL_DATA:-$X45_BUILD/kernel-data}"
for f in unit.config Module.symvers; do
	[ -f "$KDATA/$f" ] || die "missing $KDATA/$f (run: scripts/kernelsyms.sh root@ADDRESS-OF-THE-UNIT)"
done
case "$X45_BUILD" in *" "*) die "the build folder path contains a space ($X45_BUILD): the kernel build cannot handle that, set X45_BUILD" ;; esac

log "downloads"
python3 "$SCRIPTS/fetch.py" --file sources-wifidriver.txt

K=$(unpack linux-imx-rel_imx_3.14.52_1.1.0_ga.tar.gz)
if [ ! -f "$K/.x45-prepared" ] || ! cmp -s "$KDATA/unit.config" "$K/.x45-config-source"; then
	log "preparing the kernel tree"
	cp "$KDATA/unit.config" "$K/.config"
	cp "$KDATA/unit.config" "$K/.x45-config-source"
	# the unit reports 3.14.52-1.1.0_ga+g5f6f0a5: the same release text is needed in the module (vermagic)
	sed -i 's/^CONFIG_LOCALVERSION=.*/CONFIG_LOCALVERSION="-1.1.0_ga+g5f6f0a5"/; s/^CONFIG_LOCALVERSION_AUTO=y/# CONFIG_LOCALVERSION_AUTO is not set/' "$K/.config"
	# a 3.14 tree has no header for newer gcc versions; the gcc 5 one is good enough
	for v in 6 7 8 9 10 11 12; do
		[ -f "$K/include/linux/compiler-gcc$v.h" ] || cp "$K/include/linux/compiler-gcc5.h" "$K/include/linux/compiler-gcc$v.h"
	done
	cp "$KDATA/Module.symvers" "$K/Module.symvers"
	export PATH="$TC/bin:$PATH"
	make -C "$K" ARCH=arm CROSS_COMPILE="$CROSS" olddefconfig >/dev/null
	make -C "$K" ARCH=arm CROSS_COMPILE="$CROSS" HOSTCFLAGS="-fcommon -O1" modules_prepare 2>&1 | tail -5
	touch "$K/.x45-prepared"
fi
cp "$KDATA/Module.symvers" "$K/Module.symvers"
echo "kernel release: $(cat "$K/include/config/kernel.release")"

D="$WORK/wifidrv"
rm -rf "$D"
mkdir -p "$D/bsp" "$D/fdrv" "$OUT/wifi/mod"

log "x45_aicbsp"
cp "$SRCDIR/wifidrv/x45_aicbsp.c" "$D/bsp/"
echo 'obj-m := x45_aicbsp.o' >"$D/bsp/Makefile"
make -C "$K" M="$D/bsp" ARCH=arm CROSS_COMPILE="$CROSS" KCFLAGS="-std=gnu89 -fno-pic" modules >"$D/bsp.log" 2>&1 \
	|| { grep -E "error|Error" "$D/bsp.log" | head -20; die "x45_aicbsp did not build"; }

log "aic8800_fdrv"
A=$(unpack aic8800-d13d079.tar.gz)
cp -r "$A/src/USB/driver_fw/drivers/aic8800/aic8800_fdrv/." "$D/fdrv/"
(cd "$D/fdrv" && patch -p1 <"$PATCHES/aic8800-d13d079-x45.patch" >/dev/null)
cp "$SRCDIR/wifidrv/x45_compat.h" "$D/fdrv/"
make -C "$K" M="$D/fdrv" ARCH=arm CROSS_COMPILE="$CROSS" KBUILD_EXTRA_SYMBOLS="$D/bsp/Module.symvers" \
	KCFLAGS="-include $D/fdrv/x45_compat.h -std=gnu89 -fno-pic -Wno-error -Wno-implicit-fallthrough -Wno-misleading-indentation -Wno-address-of-packed-member -Wno-stringop-truncation" \
	modules -j"$JOBS" >"$D/fdrv.log" 2>&1 \
	|| { grep -E "error|Error" "$D/fdrv.log" | head -20; die "aic8800_fdrv did not build"; }
if grep -q 'undefined!' "$D/fdrv.log"; then
	grep 'undefined!' "$D/fdrv.log"
	die "the driver needs symbols that the kernel (Module.symvers) does not export"
fi

cp "$D/fdrv/aic8800_fdrv.ko" "$D/bsp/x45_aicbsp.ko" "$OUT/wifi/mod/"
"${CROSS}strip" --strip-debug "$OUT/wifi/mod/aic8800_fdrv.ko" "$OUT/wifi/mod/x45_aicbsp.ko"

log "checking the symbol checksums against the unit"
cat "$KDATA/Module.symvers" "$D/bsp/Module.symvers" >"$D/all.symvers"   # the helper module exports 9 of the symbols
python3 "$SCRIPTS/chkversions.py" "$D/all.symvers" "$OUT/wifi/mod/aic8800_fdrv.ko" | tail -3 | tee "$D/chk.log"
grep -q ' bad 0 missing 0$' "$D/chk.log" || die "a checksum differs from the unit's kernel: the module would be refused"

log "firmware and settings of the chip"
rm -rf "$OUT/wifi/fw"
mkdir -p "$OUT/wifi/fw"
cp "$A"/src/USB/driver_fw/fw/aic8800D80/* "$OUT/wifi/fw/"

echo
ls -la "$OUT/wifi/mod"
