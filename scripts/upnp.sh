#!/bin/bash
# Cross-build upmpdcli (UPnP/DLNA renderer in front of MPD) and the libraries it needs, static against musl.
#
#   scripts/upnp.sh
#
# Output: $SYSROOT/bin-upmpdcli (stripped)
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"
need_tools make gcc python3 patch tar
[ -x "$HOSTROOT/bin/meson" ] || die "run scripts/hosttools.sh first"
[ -f "$SYSROOT/.done-curl" ] || die "run scripts/deps.sh first"

export PATH="$HOSTROOT/bin:$TC/bin:$PATH"
export PKG_CONFIG_PATH="$SYSROOT/lib/pkgconfig" PKG_CONFIG_LIBDIR="$SYSROOT/lib/pkgconfig"
unset CC CXX AR RANLIB STRIP CFLAGS CXXFLAGS LDFLAGS

CROSSFILE="$WORK/x45-cross-upnp.ini"
write_cross_file "$CROSSFILE"

meson_dep() { # meson_dep STAMP TARBALL [meson args...]
	local stamp=$1 tb=$2 d
	shift 2
	have "$stamp" && { echo "$stamp: already built"; return; }
	log "$stamp"
	d=$(unpack "$tb")
	cd "$d"
	rm -rf build-x45
	meson setup build-x45 --cross-file "$CROSSFILE" --prefix "$SYSROOT" \
		--libdir lib --buildtype release -Ddefault_library=static "$@"
	ninja -C build-x45 -j"$JOBS"
	ninja -C build-x45 install
	mark "$stamp"
}

# libmicrohttpd: plain autotools, no TLS, no extras
if ! have mhd; then
	log libmicrohttpd
	d=$(unpack libmicrohttpd-1.0.1.tar.gz)
	cd "$d"
	CFLAGS="-O2 $ARCHFLAGS -I$SYSROOT/include" ./configure --host=$HOST --prefix="$SYSROOT" \
		--enable-static --disable-shared --disable-https --disable-doc \
		--disable-examples --disable-curl --disable-nls --disable-httpupgrade >/dev/null
	make -j"$JOBS" >/dev/null
	make install >/dev/null
	mark mhd
fi

meson_dep jsoncpp jsoncpp-1.9.5.tar.gz -Dtests=false
meson_dep mpdclient libmpdclient-2.22.tar.xz -Dtest=false -Ddocumentation=false
meson_dep npupnp libnpupnp-6.1.3.tar.gz -Dtestmains=false
meson_dep upnpp libupnpp-0.26.8.tar.gz

# upmpdcli itself
if ! have upmpdcli; then
	log upmpdcli
	d=$(unpack upmpdcli-1.8.18.tar.gz)
	cd "$d"
	# patch: the unit has no `upmpdcli` user, so keep running as root instead of refusing
	sed -i 's/if (geteuid() == 0) {/if (geteuid() == 0 \&\& pass) {/' src/main.cxx
	grep -q 'geteuid() == 0 && pass' src/main.cxx
	# patch: a UPnP Stop does not end what the panel or the web page started after the control point did
	if ! grep -q stopOwn src/mpdcli.cxx; then
		patch -p1 <"$PATCHES/upmpdcli-stop-own.patch"
	fi
	rm -rf build-x45
	meson setup build-x45 --cross-file "$CROSSFILE" --prefix "$SYSROOT" \
		--libdir lib --buildtype release \
		-Dcpp_args="-O2 $ARCHFLAGS -I$SYSROOT/include -I$SYSROOT/include/libupnpp"
	ninja -C build-x45 -j"$JOBS" upmpdcli
	"${CROSS}strip" build-x45/upmpdcli -o "$SYSROOT/bin-upmpdcli"
	mark upmpdcli
fi
ls -l "$SYSROOT/bin-upmpdcli"
