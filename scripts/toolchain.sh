#!/bin/bash
# Unpack the ARM cross compiler (Bootlin armv7-eabihf, musl 1.2.2, gcc 10.3) into $X45_BUILD/toolchain.
#
#   scripts/toolchain.sh
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"
need_tools tar bzip2

if [ -x "${CROSS}gcc" ] && [ -f "$TC/.relocated" ]; then
	echo "toolchain: already in place ($TC)"
	exit 0
fi
tb="$DL/armv7-eabihf--musl--stable-2021.11-1.tar.bz2"
[ -f "$tb" ] || die "missing download: $(basename "$tb") (run: python3 scripts/fetch.py)"
log "unpacking the cross compiler"
mkdir -p "$TCDIR"
rm -rf "$TC"
tar xf "$tb" -C "$TCDIR"
# the toolchain was built for another path; this rewrites its internal paths to where it is now
(cd "$TC" && ./relocate-sdk.sh >/dev/null)
touch "$TC/.relocated"
"${CROSS}gcc" --version | head -1
