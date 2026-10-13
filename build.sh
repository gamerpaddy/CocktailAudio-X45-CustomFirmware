#!/bin/bash
# Build all programs and assemble the file tree for the package.
#
#   ./build.sh                 build everything (runs ./setup.sh first when it has not been run)
#   ./build.sh deps panel      build only the named steps; steps: deps panel mpd airplay upnp spotify wifi bluetooth stage
#   ./build.sh clean           remove the build directory (the downloads are kept)
#
# Optional parts can be left out:  X45_SKIP="spotify bluetooth" ./build.sh
#
# The first full build takes a while (20 minutes to an hour depending on the computer, most of it libraries and the
# Rust compiler; roughly 1 GB is downloaded and about 10 GB of disk is used); later runs only redo what changed.
# Everything is built below $X45_BUILD (default: .build, or ~/x45-build when this folder is on a Windows drive under
# WSL).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$HERE/scripts/env.sh"
S="$SCRIPTS"

if [ "${1:-}" = clean ]; then
	rm -rf "$WORK" "$SYSROOT" "$OUT" "$STAGE" "$PKGDIR" "$X45_BUILD/rust" "$X45_BUILD/pkgwork"
	rm -f "$SRCDIR"/x45cd/a52/.prepared
	echo "removed the build directories below $X45_BUILD (downloads and toolchain are kept)"
	exit 0
fi

# first run: do the setup now
if [ ! -x "${CROSS}gcc" ] || [ ! -x "$HOSTROOT/bin/meson" ]; then
	"$HERE/setup.sh"
fi

steps=("$@")
if [ ${#steps[@]} -eq 0 ]; then
	steps=(deps panel mpd)
	skipped airplay || steps+=(airplay)
	skipped upnp || steps+=(upnp)
	skipped spotify || steps+=(spotify)
	skipped wifi || steps+=(wifi)
	skipped bluetooth || steps+=(bluetooth)
	steps+=(stage)
fi

for s in "${steps[@]}"; do
	case "$s" in
	deps) log "libraries (alsa, flac, curl, ...)"; "$S/deps.sh" all ;;
	panel) log "panel, web server and helper programs"; "$S/panel.sh" ;;
	mpd) log "MPD"; "$S/mpd.sh" ;;
	airplay) log "AirPlay receiver"; "$S/shairport.sh" ;;
	upnp) log "UPnP renderer"; "$S/upnp.sh" ;;
	spotify) log "Spotify Connect receiver"; "$S/librespot.sh" ;;
	wifi) log "Wi-Fi tools"; "$S/wifitools.sh" ;;
	bluetooth) log "Bluetooth"; "$S/bt.sh" ;;
	stage) log "assembling the program tree"; "$S/stage.sh" ;;
	*) die "unknown step: $s" ;;
	esac
done

echo
echo "Build finished. Next: ./make-pkg.sh packs it (or run it right away, it builds first if needed)."
