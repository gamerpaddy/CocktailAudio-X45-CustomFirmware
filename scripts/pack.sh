#!/bin/bash
# Turn the staged tree into the installable package.
#
#   scripts/pack.sh         -> $PKGDIR/X45-CA-1.0.0.rNNNN.pkg
#
# The package is a romfs image, the format the unit's own updater reads. It holds:
#   version          the version string the updater checks (it must end in a number of 1516 or higher)
#   updateFW.sh      the installer (scripts/payload.sh); it adds the program tree to the unit
#   rootfs.sh        does nothing on purpose
#   x45new.tar       the program tree, with x45new.tar.md5
#   x45new.init      the start script that goes to /etc/init.d
#   images/          four small pictures for the updater screen
#
# It does NOT contain rootfs.tgz, kernel.tgz or the updateFW program, so it cannot rewrite the kernel or the system
# partition. Nothing of the original system is replaced or deleted. This script checks that before it finishes.
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"
need_tools genromfs python3 tar md5sum sha256sum

REV="${X45_REV:-9995}"
case "$REV" in [0-9][0-9][0-9][0-9]) ;; *) die "X45_REV must be four digits" ;; esac
[ "$REV" -ge 1516 ] || die "X45_REV must be 1516 or higher"
[ -d "$STAGE/x45new/bin" ] || die "nothing staged (run scripts/stage.sh first)"

P="$X45_BUILD/pkgwork"
rm -rf "$P"
mkdir -p "$P/pkg/images" "$PKGDIR"
umask 022

# the tree, owned by root, in a fixed order
tar cf "$P/pkg/x45new.tar" --owner=0 --group=0 --numeric-owner --sort=name -C "$STAGE/x45new" .
(cd "$P/pkg" && md5sum x45new.tar >x45new.tar.md5)

sed 's/\r$//' "$SCRIPTS/payload.sh" >"$P/pkg/updateFW.sh"
sed 's/\r$//' "$X45_ROOT/deploy/x45new" >"$P/pkg/x45new.init"
printf 'X45-CA-1.0.0.r%s\n' "$REV" >"$P/pkg/version"
printf '#!/bin/sh\n# intentionally does nothing\nexit 0\n' >"$P/pkg/rootfs.sh"
python3 "$SCRIPTS/make_images.py" "$P/pkg/images"
chmod 755 "$P/pkg/updateFW.sh" "$P/pkg/rootfs.sh" "$P/pkg/images"
chmod 644 "$P/pkg/version" "$P"/pkg/x45new.tar* "$P/pkg/x45new.init" "$P"/pkg/images/*

PKG="$PKGDIR/X45-CA-1.0.0.r$REV.pkg"
rm -f "$PKG"
genromfs -V X20_PKG -f "$PKG" -d "$P/pkg"

log "checking the package"
LIST=$(python3 "$SCRIPTS/romfs.py" "$PKG" list)
echo "$LIST" | sed 's/^/    /'
NAMES=$(echo "$LIST" | awk '{print $4}')
TARLIST=$(tar tf "$P/pkg/x45new.tar")
# in_list LIST ITEM: is ITEM one of the lines of LIST (no pipes, so no trouble with early-closing readers)
in_list() { local l; while IFS= read -r l; do [ "$l" = "$2" ] && return 0; done <<<"$1"; return 1; }
bad() { rm -f "$PKG"; die "$1"; }
for forbidden in /rootfs.tgz /kernel.tgz /updateFW; do
	if in_list "$NAMES" "$forbidden"; then bad "the package contains $forbidden"; fi
done
for need in /version /updateFW.sh /rootfs.sh /x45new.tar /x45new.tar.md5 /x45new.init; do
	in_list "$NAMES" "$need" || bad "the package lacks $need"
done
if echo "$TARLIST" | grep -Ei 'authorized_keys|setup\.xml|id_(rsa|ed25519)|\.pem$' | grep -v 'etc/ssl/cacert.pem' >/dev/null; then
	bad "a file that looks like a key or a credential is in the program tree"
fi
for f in bin/x45stack bin/x45ui bin/mpd bin/x45web; do
	in_list "$TARLIST" "./$f" || bad "the program tree lacks $f"
done
rm -rf "$P"

echo
echo "package: $PKG"
echo "size   : $(stat -c%s "$PKG") bytes"
echo "sha256 : $(sha256sum "$PKG" | cut -d' ' -f1)"
