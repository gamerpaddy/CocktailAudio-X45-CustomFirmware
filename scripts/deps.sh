#!/bin/bash
# Cross-build the libraries the firmware needs: static, for the unit's ARM CPU, against musl.
#
#   scripts/deps.sh            build everything that is not built yet
#   scripts/deps.sh clean      remove the build tree (the downloads stay)
#   scripts/deps.sh NAME       build just one library (see the usage line at the end of this file)
#
# Everything is installed into $SYSROOT with --enable-static --disable-shared, so the finished programs do not depend
# on anything on the unit.
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"
need_tools make gcc python3 perl patch tar bzip2 xz
[ -x "$CROSS"gcc ] || die "the cross compiler is missing (run scripts/toolchain.sh first)"
[ -x "$HOSTROOT/bin/pkg-config" ] || die "the helper tools are missing (run scripts/hosttools.sh first)"

# alsa-lib bakes its configuration path in at build time and reads it at run time, so it has to be the path on the
# unit, not the build path.
ALSA_CONFIGDIR=$DEVROOT/share/alsa
ALSA_PLUGINDIR=$DEVROOT/lib/alsa-lib

cross_env

extract() { # extract <tarball-glob> <expected-dir-prefix>
	local tb dir
	tb=$(ls "$DL"/$1 2>/dev/null | head -1)
	[ -n "$tb" ] || { echo "missing tarball: $1 in $DL" >&2; return 1; }
	dir=$(ls -d "$WORK"/$2* 2>/dev/null | head -1 || true)
	if [ -z "$dir" ]; then
		tar xf "$tb" -C "$WORK"
		dir=$(ls -d "$WORK"/$2* | head -1)
	fi
	echo "$dir"
}

build_ogg() {
	have ogg && { echo "ogg: already built"; return; }
	log "libogg"
	local d; d=$(extract 'libogg-*.tar.*' 'libogg-')
	cd "$d"
	./configure --host=$HOST --prefix="$SYSROOT" \
		--enable-static --disable-shared >/dev/null
	make -j"$JOBS" >/dev/null
	make install >/dev/null
	mark ogg
	echo "ogg: ok"
}

build_flac() {
	have flac && { echo "flac: already built"; return; }
	log "libFLAC"
	local d; d=$(extract 'flac-*.tar.*' 'flac-')
	cd "$d"
	# no CLI tools, no docs, no examples: we only want the decoder library.
	./configure --host=$HOST --prefix="$SYSROOT" \
		--enable-static --disable-shared \
		--disable-programs --disable-examples --disable-cpplibs \
		--disable-doxygen-docs --disable-thorough-tests \
		--with-ogg="$SYSROOT" >/dev/null
	make -j"$JOBS" >/dev/null
	make install >/dev/null
	mark flac
	echo "flac: ok"
}

build_alsa() {
	have alsa && { echo "alsa: already built"; return; }
	log "alsa-lib"
	local d; d=$(extract 'alsa-lib-*.tar.*' 'alsa-lib-')
	cd "$d"
	# alsa-lib bakes its config path in at build time and reads it at runtime.
	# The default would be $SYSROOT/share/alsa, which does not exist on the
	# device; the first attempt failed with "Cannot access file .../share/alsa/alsa.conf".
	# The device does have /usr/share/alsa, but it is a 2016 alsa-lib 1.1.x
	# tree and ours is 1.2.11. Rather than mix versions, point at our own
	# directory under the parallel userspace and ship the matching config
	# there (stage.sh copies it into the package).
	#
	# Python bindings and the alisp interpreter are dead weight here.
	./configure --host=$HOST --prefix="$SYSROOT" \
		--with-configdir="$ALSA_CONFIGDIR" \
		--with-plugindir="$ALSA_PLUGINDIR" \
		--enable-static --disable-shared \
		--disable-python --disable-alisp --disable-old-symbols \
		--disable-topology --with-pcm-plugins=all >/dev/null
	make -j"$JOBS" >/dev/null
	# configdir/plugindir point at the DEVICE path, which the build host
	# cannot write (mkdir /home/root: Permission denied). Stage the install,
	# then fold it into the sysroot: libs/headers keep their $SYSROOT path,
	# and the config tree lands in $SYSROOT/share/alsa, from where the package builder takes it.
	local stage="$WORK/alsa-stage"
	rm -rf "$stage"
	make install DESTDIR="$stage" >/dev/null
	cp -a "$stage$SYSROOT"/. "$SYSROOT"/
	mkdir -p "$SYSROOT/share/alsa"
	cp -a "$stage$ALSA_CONFIGDIR"/. "$SYSROOT/share/alsa"/
	mark alsa
	echo "alsa: ok"
}

# Generic autotools library: build_auto <stamp> <tarball-glob> <dir-prefix> [configure args...]
build_auto() {
	local stamp=$1 tb=$2 pre=$3 d
	shift 3
	have "$stamp" && { echo "$stamp: already built"; return; }
	log "$stamp"
	d=$(extract "$tb" "$pre")
	cd "$d"
	./configure --host=$HOST --prefix="$SYSROOT" \
		--enable-static --disable-shared "$@" >/dev/null
	make -j"$JOBS" >/dev/null
	make install >/dev/null
	mark "$stamp"
	echo "$stamp: ok"
}

build_vorbis() {
	build_ogg
	build_auto vorbis 'libvorbis-*.tar.*' 'libvorbis-' --disable-examples --disable-docs
}

build_opus() {
	build_auto opus 'opus-1*.tar.*' 'opus-1' --disable-doc --disable-extra-programs \
		--enable-fixed-point --disable-intrinsics
}

build_opusfile() {
	build_ogg; build_opus
	# --disable-http: the HTTP reader would drag in openssl; MPD fetches streams itself.
	build_auto opusfile 'opusfile-*.tar.*' 'opusfile-' --disable-http --disable-examples --disable-doc
}

build_sndfile() {
	# WAV/AIFF/etc. FLAC/Vorbis/Opus come from the dedicated libs, so the
	# external-library glue (which would be circular here) is switched off.
	build_auto sndfile 'libsndfile-*.tar.*' 'libsndfile-' --disable-external-libs \
		--disable-mpeg --disable-alsa --disable-sqlite --disable-full-suite \
		--disable-programs
}

build_zlib() {
	have zlib && { echo "zlib: already built"; return; }
	log "zlib"
	local d; d=$(extract 'zlib-*.tar.*' 'zlib-')
	cd "$d"
	./configure --prefix="$SYSROOT" --static >/dev/null
	make -j"$JOBS" >/dev/null
	make install >/dev/null
	mark zlib
	echo "zlib: ok"
}

# mbedTLS's Makefile installs no pkg-config files, but libcurl.pc says
# "Requires.private: mbedtls" and MPD resolves curl through pkg-config.
write_mbedtls_pc() {
	local n
	mkdir -p "$SYSROOT/lib/pkgconfig"
	for n in mbedcrypto mbedx509 mbedtls; do
		cat >"$SYSROOT/lib/pkgconfig/$n.pc" <<PC
prefix=$SYSROOT
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: $n
Description: mbed TLS ($n)
Version: 3.6.2
Libs: -L\${libdir} -lmbedtls -lmbedx509 -lmbedcrypto
Cflags: -I\${includedir}
PC
	done
}

# mbedTLS for HTTPS (internet radio, podcasts). Built as a library only and
# installed by hand: its `make install` also builds ~100 sample programs and
# tests that nothing here needs.
build_mbedtls() {
	have mbedtls && { echo "mbedtls: already built"; return; }
	log "mbedtls"
	local d; d=$(extract 'mbedtls-*.tar.*' 'mbedtls-')
	cd "$d"
	make -j"$JOBS" lib CC="$CC" AR="$AR" CFLAGS="$CFLAGS" >/dev/null
	mkdir -p "$SYSROOT/include" "$SYSROOT/lib"
	cp -a include/mbedtls include/psa "$SYSROOT/include/"
	cp -a library/libmbedtls.a library/libmbedx509.a library/libmbedcrypto.a "$SYSROOT/lib/"
	write_mbedtls_pc
	mark mbedtls
	echo "mbedtls: ok"
}

# libcurl, TLS via mbedTLS, only the protocols a radio/podcast client needs.
# The CA bundle path is baked in at build time, like alsa-lib's config dir, so
# the package builder puts cacert.pem at $DEVROOT/etc/ssl/cacert.pem on the unit.
build_curl() {
	have curl && { echo "curl: already built"; return; }
	build_zlib; build_mbedtls
	log "curl"
	local d; d=$(extract 'curl-*.tar.*' 'curl-')
	cd "$d"
	./configure --host=$HOST --prefix="$SYSROOT" --enable-static --disable-shared \
		--with-mbedtls="$SYSROOT" --with-zlib="$SYSROOT" \
		--with-ca-bundle="$DEVROOT/etc/ssl/cacert.pem" --without-ca-path \
		--without-libpsl --without-brotli --without-zstd --without-libidn2 \
		--without-nghttp2 --without-librtmp --without-libssh2 \
		--disable-ldap --disable-ldaps --disable-rtsp --disable-dict \
		--disable-telnet --disable-tftp --disable-pop3 --disable-imap \
		--disable-smtp --disable-gopher --disable-mqtt --disable-smb \
		--disable-ftp --disable-manual --disable-docs --disable-ntlm \
		--disable-curldebug --disable-verbose >/dev/null
	make -j"$JOBS" -C lib >/dev/null
	make -C lib install >/dev/null
	make -C include install >/dev/null
	install -m644 libcurl.pc "$SYSROOT/lib/pkgconfig/"
	mark curl
	echo "curl: ok"
}

build_expat() {
	build_auto expat 'expat-*.tar.*' 'expat-' --without-docbook --without-xmlwf \
		--without-examples --without-tests
}

# libid3tag: MP3 tags. Upstream ships no pkg-config file, which MPD's build
# looks for, so write one. The old autoconf does not know "musleabihf", but the
# compiler is passed explicitly so the triplet only has to parse.
build_id3tag() {
	have id3tag && { echo "id3tag: already built"; return; }
	build_zlib
	log "libid3tag"
	local d; d=$(extract 'libid3tag-*.tar.*' 'libid3tag-')
	cd "$d"
	./configure --host=arm-linux-gnu --prefix="$SYSROOT" --enable-static \
		--disable-shared --disable-debugging >/dev/null
	make -j"$JOBS" >/dev/null
	make install >/dev/null
	mkdir -p "$SYSROOT/lib/pkgconfig"
	cat >"$SYSROOT/lib/pkgconfig/id3tag.pc" <<PC
prefix=$SYSROOT
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: id3tag
Description: ID3 tag manipulation library
Version: 0.15.1b
Libs: -L\${libdir} -lid3tag -lz
Cflags: -I\${includedir}
PC
	mark id3tag
	echo "id3tag: ok"
}

# libmad: the MP3 decoder MPD actually wants. Unlike its mpg123 plugin (local
# files only, no streaming, no ID3), the mad plugin decodes HTTP radio and is
# MPD's built-in fallback for streams that do not name their type.
#  * 0.15.1b's configure passes -fforce-mem, which gcc >= 4.3 rejects; strip it.
#  * --enable-fpm=64bit: accurate 64-bit multiply (compiles to smull on ARM)
#    rather than the lossy 'default' 32-bit one.
#  * upstream installs no pkg-config file; MPD asks for one.
build_mad() {
	have mad && { echo "mad: already built"; return; }
	log "libmad"
	local d; d=$(extract 'libmad-*.tar.*' 'libmad-')
	cd "$d"
	sed -i 's/-fforce-mem//g; s/-fthread-jumps//g; s/-fcse-follow-jumps//g; s/-fcse-skip-blocks//g; s/-fregmove//g; s/-fexpensive-optimizations//g; s/-fschedule-insns2//g' configure
	./configure --host=arm-linux-gnu --prefix="$SYSROOT" --enable-static --disable-shared \
		--enable-fpm=64bit --disable-debugging --disable-aso >/dev/null
	make -j"$JOBS" >/dev/null
	make install >/dev/null
	mkdir -p "$SYSROOT/lib/pkgconfig"
	cat >"$SYSROOT/lib/pkgconfig/mad.pc" <<PC
prefix=$SYSROOT
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: mad
Description: MPEG audio decoder
Version: 0.15.1b
Libs: -L\${libdir} -lmad
Cflags: -I\${includedir}
PC
	mark mad
	echo "mad: ok"
}

# AirPlay support libraries (shairport-sync).
build_libconfig() {
	build_auto libconfig 'libconfig-*.tar.*' 'libconfig-' --disable-cxx --disable-examples 		--disable-tests --disable-doc
}

# Apple's reference ALAC decoder, as packaged for shairport-sync. The decoder
# built into shairport-sync (a hand port) crashed the receiver on the first
# stream I tried; this one is what the format's own authors wrote. The upstream
# archive is a git snapshot with no generated configure, hence autoreconf.
build_alac() {
	have alac && { echo "alac: already built"; return; }
	log "alac (Apple ALAC decoder)"
	local d; d=$(extract 'alac-*.tar.*' 'alac-')
	cd "$d"
	# Upstream trusts a sample count read from the packet and writes that many
	# samples into buffers sized for frameLength: a remote buffer overflow on a
	# receiver listening on the LAN. Reproduced with a real client (SIGSEGV in
	# memset <- dyn_decomp). The patch bounds-checks both places it is read.
	if ! grep -q 'numSamples <= mConfig.frameLength' codec/ALACDecoder.cpp; then
		patch -p1 <"$PATCHES/alac-numsamples-bounds.patch" >/dev/null
	fi
	[ -f configure ] || autoreconf -fi >/dev/null 2>&1
	./configure --host=$HOST --prefix="$SYSROOT" --enable-static --disable-shared >/dev/null
	make -j"$JOBS" >/dev/null
	make install >/dev/null
	mark alac
	echo "alac: ok"
}

build_popt() {
	build_auto popt 'popt-*.tar.*' 'popt-' --disable-nls
}

# Boost is headers-only for MPD; point BOOST_ROOT at the unpacked tree.
build_boost() {
	have boost && { echo "boost: already unpacked"; return; }
	log "boost headers"
	[ -d "$WORK/boost_1_83_0" ] || tar xf "$DL/boost_1_83_0.tar.bz2" -C "$WORK"
	mark boost
	echo "boost: ok ($WORK/boost_1_83_0)"
}


# LAME: the MP3 encoder for the "rip as MP3" option (library only; the unit has decoders for MP3, no encoder). No CLI, no
# decoder, no x86 assembly.
build_lame() {
	build_auto lame 'lame-3.*.tar.*' 'lame-3' --disable-frontend --disable-decoder --disable-gtktest --disable-nasm \
		--disable-analyzer-hooks
}

# libusb: userspace USB for the Bluetooth stack (BTstack HCI transport). Netlink hotplug, no udev.
build_libusb() {
	build_auto libusb 'libusb-*.tar.*' 'libusb-' --disable-udev
}

case "${1:-all}" in
clean) rm -rf "$WORK" "$SYSROOT"; echo "removed $WORK and $SYSROOT"; exit 0 ;;
ogg)   build_ogg ;;
flac)  build_ogg; build_flac ;;
alsa)  build_alsa ;;
vorbis) build_vorbis ;;
opus)  build_opus ;;
opusfile) build_opusfile ;;
sndfile) build_sndfile ;;
boost) build_boost ;;
zlib)  build_zlib ;;
mbedtls) build_mbedtls ;;
curl)  build_curl ;;
expat) build_expat ;;
id3tag) build_id3tag ;;
mad)   build_mad ;;
libconfig) build_libconfig ;;
popt)  build_popt ;;
alac)  build_alac ;;
libusb) build_libusb ;;
lame)  build_lame ;;
codecs) build_ogg; build_flac; build_vorbis; build_opusfile; build_sndfile ;;
all)   build_ogg; build_flac; build_alsa; build_vorbis; build_opusfile
       build_sndfile; build_boost
       build_curl; build_expat; build_id3tag; build_mad; build_lame
       skipped airplay || { build_libconfig; build_popt; build_alac; }
       skipped bluetooth || build_libusb ;;
*)     echo "usage: deps.sh [all|clean|ogg|flac|alsa|vorbis|opus|opusfile|sndfile|boost|zlib|mbedtls|curl|expat|id3tag|mad|libconfig|popt|alac|libusb|lame|codecs]" >&2; exit 2 ;;
esac

log "sysroot contents"
echo "  $SYSROOT"
ls -1 "$SYSROOT/lib"/*.a 2>/dev/null | sed 's|.*/|    |' || echo "    (no static libs yet)"
echo "  pkg-config files:"
ls -1 "$SYSROOT/lib/pkgconfig"/*.pc 2>/dev/null | sed 's|.*/|    |' || echo "    (none)"
echo "  headers:"
ls -1d "$SYSROOT/include"/* 2>/dev/null | sed 's|.*/|    |' || echo "    (none)"
