#!/bin/bash
# The one click solution: set everything up, build everything, pack the installable package.
#
#   ./make-pkg.sh
#
# Result: dist/X45-CA-1.0.0.r9995.pkg (the install package) and dist/X45-REMOVE-1.0.0.r9994.pkg (takes it off the unit
# again), each with a .sha256 file. Then run ./install.sh (or install.bat on Windows) to put one of them on a USB stick.
#
# X45_SKIP="spotify bluetooth" ./make-pkg.sh   leaves out optional parts (airplay spotify upnp wifi bluetooth)
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$HERE/scripts/env.sh"

"$HERE/build.sh"
"$SCRIPTS/pack.sh"
"$SCRIPTS/pack-remove.sh"

mkdir -p "$X45_ROOT/dist"
PKG=$(ls -t "$PKGDIR"/X45-CA-*.pkg | head -1)
RPKG=$(ls -t "$PKGDIR"/X45-REMOVE-*.pkg | head -1)
for f in "$PKG" "$RPKG"; do
	cp -f "$f" "$X45_ROOT/dist/"
	(cd "$X45_ROOT/dist" && sha256sum "$(basename "$f")" >"$(basename "$f").sha256")
done

echo
echo "Done. The packages are here:"
echo "    $X45_ROOT/dist/$(basename "$PKG")      install"
echo "    $X45_ROOT/dist/$(basename "$RPKG")  take it off the unit again"
echo "Next step: ./install.sh (Linux) or install.bat (Windows) puts one on a USB stick."
