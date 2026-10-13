#!/bin/bash
# Cross-build the Wi-Fi userspace for the unit: libnl, iw and wpa_supplicant (nl80211, access point mode, no SAE),
# static musl.
#
#   scripts/wifitools.sh        -> $SYSROOT/bin/{wpa_supplicant,wpa_cli,iw}
#
# This is only the userspace. The kernel drivers for the USB radio are not built here (see README, "What does not work").
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"
need_tools make gcc tar patch
[ -x "$HOSTROOT/bin/pkg-config" ] || die "run scripts/hosttools.sh first"
cross_env
mkdir -p "$SYSROOT/bin"

build_libnl() {
	[ -f "$SYSROOT/.done-libnl" ] && return
	log "libnl"
	local d; d=$(unpack libnl-3.9.0.tar.gz)
	cd "$d"
	# the release tarball carries the generated lexer and parser sources; configure only insists on the tools
	mkdir -p "$WORK/stubs"
	local t
	for t in flex bison; do
		printf '#!/bin/sh\necho "%s 3.0.0"\nexit 0\n' "$t" >"$WORK/stubs/$t"
		chmod +x "$WORK/stubs/$t"
	done
	PATH="$WORK/stubs:$PATH" ./configure --host=$HOST --prefix="$SYSROOT" --enable-static --disable-shared \
		--disable-cli --disable-debug >/dev/null
	# only the core and generic netlink libraries are needed (nl80211); libnl-route needs flex/bison output
	make -j"$JOBS" lib/libnl-3.la lib/libnl-genl-3.la >/dev/null
	install -d "$SYSROOT/lib/pkgconfig" "$SYSROOT/include/libnl3"
	install lib/.libs/libnl-3.a lib/.libs/libnl-genl-3.a "$SYSROOT/lib"
	install libnl-3.0.pc libnl-genl-3.0.pc "$SYSROOT/lib/pkgconfig"
	cp -r include/netlink "$SYSROOT/include/libnl3/"
	touch "$SYSROOT/.done-libnl"
}

build_iw() {
	log "iw"
	local d; d=$(unpack iw-6.9.tar.xz)
	cd "$d"
	make clean >/dev/null 2>&1 || true
	# pkg-config is the native one; ask it for the static link line
	make -j"$JOBS" CC="$CC" PKG_CONFIG="pkg-config --static" LDFLAGS="$LDFLAGS -static" >/dev/null
	"$STRIP" -o "$SYSROOT/bin/iw" iw
}

build_wpa() {
	log "wpa_supplicant"
	local d; d=$(unpack wpa_supplicant-2.11.tar.gz)
	cd "$d/wpa_supplicant"
	cp "$PATCHES/wpa_supplicant-2.11.config" .config
	make clean >/dev/null 2>&1 || true
	# CFLAGS and LDFLAGS go in through the environment: the Makefile appends its own include paths to them, a command
	# line CFLAGS= would replace them and lose -I../src
	CFLAGS="$CFLAGS -I$SYSROOT/include/libnl3" LDFLAGS="$LDFLAGS -static" \
		make -j"$JOBS" CC="$CC" PKG_CONFIG="pkg-config --static" wpa_supplicant wpa_cli >/dev/null
	"$STRIP" -o "$SYSROOT/bin/wpa_supplicant" wpa_supplicant
	"$STRIP" -o "$SYSROOT/bin/wpa_cli" wpa_cli
}

build_libnl
build_iw
build_wpa
ls -la "$SYSROOT"/bin/iw "$SYSROOT"/bin/wpa_supplicant "$SYSROOT"/bin/wpa_cli
