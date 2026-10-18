#!/bin/bash
# Build the programs of this repository (everything below src/): the panel UI, the web server, the player helpers,
# the tuner and CD tools and the small command line tools. Static ARM binaries.
#
#   scripts/panel.sh
#
# Output: $OUT/x45ui, $OUT/x45web, ...
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"
need_tools make
[ -x "${CROSS}gcc" ] || die "the cross compiler is missing (run scripts/toolchain.sh)"
[ -f "$SYSROOT/.done-lame" ] || die "the libraries are missing (run scripts/deps.sh)"
"$SCRIPTS/prepare-src.sh"

log "programs"
TARGETS="x45ctl x45fb x45in x45mpc x45ui x45play x45loop x45cd x45rec x45tuner x45web x45upnp x45rb x45wifi x45wd x45art"
if ! skipped bluetooth; then TARGETS="$TARGETS aicload sgsend"; fi
mk=()
for t in $TARGETS; do mk+=("$OUT/$t"); done
make -C "$SRCDIR" -j"$JOBS" OUT="$OUT" CROSS="$CROSS" DEPS="$SYSROOT" "${mk[@]}" | grep -v '^---\|^-rwx\|Class\|Machine\|Type' || true
for t in $TARGETS; do
	[ -x "$OUT/$t" ] || die "$t was not built"
done
echo "built: $TARGETS"
