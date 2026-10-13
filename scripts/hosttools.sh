#!/bin/bash
# Build the helper programs that run on THIS computer during the build: pkgconf, ninja, meson and the GNU autotools.
#
# Kept apart from deps.sh because that script sets a cross compiler for the whole run, which would make these tools
# produce ARM code. Nothing here needs root: everything is installed below $HOSTROOT.
#
#   scripts/hosttools.sh
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"
need_tools make gcc python3 perl tar xz

native_env

if [ ! -x "$HOSTROOT/bin/pkgconf" ]; then
	log "pkgconf"
	d=$(unpack pkgconf-2.2.0.tar.xz)
	cd "$d"
	./configure --prefix="$HOSTROOT" --with-system-libdir=/usr/lib --with-system-includedir=/usr/include >/dev/null
	make -j"$JOBS" >/dev/null
	make install >/dev/null
fi
# cross builds look for a pkg-config by that name
ln -sf pkgconf "$HOSTROOT/bin/pkg-config"

if [ ! -x "$HOSTROOT/bin/ninja" ]; then
	log "ninja (built from source)"
	d=$(unpack ninja-1.12.1.tar.gz)
	cd "$d"
	python3 configure.py --bootstrap >/dev/null
	install -m755 ninja "$HOSTROOT/bin/ninja"
fi

if [ ! -x "$HOSTROOT/bin/meson" ]; then
	log "meson (pure python, runs from its unpacked tree)"
	d=$(unpack meson-1.4.1.tar.gz)
	printf '#!/bin/sh\nexec python3 "%s/meson.py" "$@"\n' "$d" >"$HOSTROOT/bin/meson"
	chmod 755 "$HOSTROOT/bin/meson"
fi

# Some projects ship without a generated configure script, so they need the GNU autotools. Order matters:
# autoconf needs m4, automake needs autoconf, libtool needs m4.
build_gnu() { # build_gnu TOOL-BINARY TARBALL
	if [ ! -x "$HOSTROOT/bin/$1" ]; then
		log "$1"
		local d; d=$(unpack "$2")
		cd "$d"
		./configure --prefix="$HOSTROOT" >/dev/null
		make -j"$JOBS" >/dev/null
		make install >/dev/null
	fi
}
build_gnu m4 m4-1.4.19.tar.xz
build_gnu autoconf autoconf-2.71.tar.xz
build_gnu automake automake-1.16.5.tar.xz
build_gnu libtoolize libtool-2.4.7.tar.xz

log "helper tools ready"
for t in pkg-config ninja meson m4 autoconf automake libtoolize; do
	printf '  %-11s %s\n' "$t" "$("$HOSTROOT/bin/$t" --version | head -1)"
done
