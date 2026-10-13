#!/bin/bash
# Put the third party source files the panel programs need into the source tree. They are not part of this
# repository: they are downloaded (see scripts/sources.txt) and copied here, where src/Makefile expects them.
#
#   scripts/prepare-src.sh
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"
need_tools patch tar

# stb_image.h and stb_truetype.h: single file libraries (image and font loading) by Sean Barrett
for f in stb_image.h stb_truetype.h; do
	[ -f "$DL/$f" ] || die "missing download: $f (run: python3 scripts/fetch.py)"
	cmp -s "$DL/$f" "$SRCDIR/libx45/$f" 2>/dev/null || cp -f "$DL/$f" "$SRCDIR/libx45/$f"
done

# liba52 (Dolby Digital decoder) from a52dec 0.7.4, with our small patch applied
A52="$SRCDIR/x45cd/a52"
if [ ! -f "$A52/.prepared" ]; then
	log "liba52"
	tmp="$WORK/a52prep"
	rm -rf "$tmp"; mkdir -p "$tmp"
	tar xf "$DL/a52dec-0.7.4.tar.gz" -C "$tmp"
	d="$tmp/a52dec-0.7.4"
	patch -d "$d" -p1 <"$PATCHES/a52dec-0.7.4-x45.patch" >/dev/null
	mkdir -p "$A52"
	for f in bit_allocate.c bitstream.c bitstream.h downmix.c imdct.c parse.c a52.h a52_internal.h tables.h mm_accel.h attributes.h; do
		src="$d/liba52/$f"
		[ -f "$src" ] || src="$d/include/$f"
		[ -f "$src" ] || die "a52dec 0.7.4 has no $f"
		cp -f "$src" "$A52/$f"
	done
	touch "$A52/.prepared"
fi
echo "third party sources in place"
