#!/bin/bash
# One time setup: installs what the build needs and prepares it for this project.
#
#   ./setup.sh
#
# What it does:
#   1. installs the few ordinary programs the build uses (compiler, make, python, ...) with apt, if they are missing
#   2. downloads every source archive and file the build needs, and checks each against its recorded SHA256
#   3. unpacks the ARM cross compiler and builds the helper tools (pkgconf, ninja, meson, autotools) from source
#
# Works on Debian and Ubuntu, also inside WSL on Windows. On other systems install the programs named below with
# your package manager and run ./setup.sh again.
#
# Optional parts can be left out to save time and disk space:  X45_SKIP="spotify bluetooth" ./setup.sh
# (names: airplay spotify upnp wifi bluetooth). Use the same X45_SKIP for ./build.sh.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$HERE/scripts/env.sh"

[ "$(uname -s)" = Linux ] || die "this needs Linux. On Windows run make-pkg.bat, which uses WSL."
[ "$(id -u)" != 0 ] || echo "note: running as root is not needed; the build only writes below $X45_BUILD"

log "checking the programs the build uses"
# command -> apt package
declare -A PKG=(
	[gcc]=build-essential [make]=build-essential [python3]=python3 [perl]=perl [patch]=patch
	[bzip2]=bzip2 [xz]=xz-utils [tar]=tar [genromfs]=genromfs [sed]=sed [awk]=gawk
)
missing=()
for c in "${!PKG[@]}"; do
	command -v "$c" >/dev/null 2>&1 || missing+=("${PKG[$c]}")
done
if command -v dpkg >/dev/null 2>&1 && ! dpkg -s ca-certificates >/dev/null 2>&1; then missing+=(ca-certificates); fi
if [ ${#missing[@]} -gt 0 ]; then
	pkgs=$(printf '%s\n' "${missing[@]}" | sort -u | tr '\n' ' ')
	if command -v apt-get >/dev/null 2>&1; then
		echo "installing with apt: $pkgs"
		SUDO=""
		if [ "$(id -u)" != 0 ]; then
			command -v sudo >/dev/null 2>&1 || die "sudo is not available. Install as root: apt-get install $pkgs"
			SUDO="sudo"
		fi
		$SUDO apt-get update
		$SUDO apt-get install -y $pkgs
	else
		die "please install these with your package manager, then run ./setup.sh again: $pkgs"
	fi
else
	echo "all there"
fi

log "downloading sources"
python3 "$SCRIPTS/fetch.py"

log "cross compiler"
"$SCRIPTS/toolchain.sh"

log "helper tools"
"$SCRIPTS/hosttools.sh"

echo
echo "Setup finished. Next: ./build.sh (or ./make-pkg.sh to build and pack in one go)."
