#!/bin/bash
# Build the small package that takes the replacement software off the unit again.
#
#   scripts/pack-remove.sh      -> $PKGDIR/X45-REMOVE-1.0.0.rNNNN.pkg
#
# Same format as the install package (see pack.sh), but its installer (scripts/payload-remove.sh) removes what the
# install package added, and it carries no program tree. It needs no build, so it is made in a second.
# The revision is X45_REV minus one (9994 by default), only so that the two files can be told apart on the unit.
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"
need_tools genromfs python3 sha256sum

REV=$(( ${X45_REV:-9995} - 1 ))
[ "$REV" -ge 1516 ] || die "X45_REV is too low"

P="$X45_BUILD/pkgwork-remove"
rm -rf "$P"
mkdir -p "$P/pkg/images" "$PKGDIR"
umask 022

sed 's/\r$//' "$SCRIPTS/payload-remove.sh" >"$P/pkg/updateFW.sh"
printf 'X45-CA-1.0.0.r%s\n' "$REV" >"$P/pkg/version"
printf '#!/bin/sh\n# intentionally does nothing\nexit 0\n' >"$P/pkg/rootfs.sh"
python3 "$SCRIPTS/make_images.py" "$P/pkg/images"
chmod 755 "$P/pkg/updateFW.sh" "$P/pkg/rootfs.sh" "$P/pkg/images"
chmod 644 "$P/pkg/version" "$P"/pkg/images/*

PKG="$PKGDIR/X45-REMOVE-1.0.0.r$REV.pkg"
rm -f "$PKG"
genromfs -V X20_PKG -f "$PKG" -d "$P/pkg"

NAMES=$(python3 "$SCRIPTS/romfs.py" "$PKG" list | awk '{print $4}')
for forbidden in /rootfs.tgz /kernel.tgz /updateFW; do
	if grep -qx "$forbidden" <<<"$NAMES"; then rm -f "$PKG"; die "the package contains $forbidden"; fi
done
for need in /version /updateFW.sh /rootfs.sh; do
	grep -qx "$need" <<<"$NAMES" || { rm -f "$PKG"; die "the package lacks $need"; }
done
rm -rf "$P"

echo "package: $PKG ($(stat -c%s "$PKG") bytes)"
