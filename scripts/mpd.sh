#!/bin/bash
# Cross-build MPD 0.23 for the unit: fully static musl, only the plugins that are fed by the libraries from deps.sh.
#
#   scripts/mpd.sh           configure (first time) and build
#   scripts/mpd.sh reconf    throw the build directory away and configure again
#
# Output: $WORK/mpd-build/mpd.stripped
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"
need_tools make gcc python3 patch tar xz
[ -x "$HOSTROOT/bin/meson" ] || die "run scripts/hosttools.sh first"
[ -f "$SYSROOT/.done-alsa" ] || die "run scripts/deps.sh first"

SRC="$WORK/mpd-0.23.15"
BLD="$WORK/mpd-build"

export PATH="$HOSTROOT/bin:$TC/bin:$PATH"
export BOOST_ROOT="$WORK/boost_1_83_0"
unset CC CXX AR RANLIB STRIP CFLAGS CXXFLAGS LDFLAGS

[ -d "$SRC" ] || tar xf "$DL/mpd-0.23.15.tar.xz" -C "$WORK"
[ -d "$BOOST_ROOT" ] || die "the boost headers are not unpacked: run scripts/deps.sh boost"

# patch: pick the XMOS master clock family before the ALSA PCM opens. Each patch is applied once.
if ! grep -q x45_clock_select "$SRC/src/output/plugins/AlsaOutputPlugin.cxx"; then
	patch -d "$SRC" -p1 <"$PATCHES/mpd-0.23.15-x45-clock.patch"
fi

# patch: a FLAC decoder that is being stopped must not seek (a UPnP stream stalled the whole of MPD on it)
if ! grep -q "x45: a decoder that has been told to stop" "$SRC/src/decoder/plugins/FlacInput.cxx"; then
	patch -d "$SRC" -p1 <"$PATCHES/mpd-0.23.15-x45-flac-stop-seek.patch"
fi

# patch: DSD256 and faster is advertised by the DAC chip and refused, so it is played as PCM at 352.8 kHz through a
# real decimating FIR. Native DSD64 and DSD128 are unchanged.
if ! grep -q X45DsdAsPcm "$SRC/src/output/Filtered.cxx"; then
	patch -d "$SRC" -p1 <"$PATCHES/mpd-0.23.15-x45-dsd-pcm.patch"
fi

# ...which needs libx45clock.a in the sysroot (same toolchain as MPD).
make -C "$SRCDIR" clocklib OUT="$OUT" DEPS="$SYSROOT" CROSS="$CROSS" >/dev/null

# MPD finds libmad with find_library(), which asks the compiler for its search directories and ignores the -L in the
# cross file, so name the sysroot explicitly.
MESON_DEC="$SRC/src/decoder/plugins/meson.build"
if ! grep -q "dirs: \['$SYSROOT/lib'\]" "$MESON_DEC"; then
	sed -i "s|c_compiler.find_library('mad', required: get_option('mad'))|c_compiler.find_library('mad', required: get_option('mad'), dirs: ['$SYSROOT/lib'])|" "$MESON_DEC"
fi
grep -q "dirs: \['$SYSROOT/lib'\]" "$MESON_DEC" || die "could not point find_library at the sysroot"

# fmt comes from MPD's own wrap; hand meson the files the wrap names (it still checks them against the hashes in the wrap)
mkdir -p "$SRC/subprojects/packagecache"
cp -u "$DL/fmt-10.1.1.tar.gz" "$DL/fmt_10.1.1-1_patch.zip" "$SRC/subprojects/packagecache/"

CROSSFILE="$WORK/x45-cross.ini"
write_cross_file "$CROSSFILE" "'-I$BOOST_ROOT'" "'-lx45clock'"

[ "${1:-}" = reconf ] && rm -rf "$BLD"

if [ ! -d "$BLD" ]; then
	# Everything is off unless named. Each "enabled" turns a missing dependency into a configure error instead of a
	# silently absent plugin.
	meson setup "$BLD" "$SRC" --cross-file "$CROSSFILE" \
		--buildtype=release -Db_ndebug=true \
		-Dauto_features=disabled \
		-Ddocumentation=disabled -Dhtml_manual=false -Dmanpages=false \
		-Dsystemd=disabled -Dsyslog=enabled \
		-Dalsa=enabled \
		-Dflac=enabled -Dvorbis=enabled -Dopus=enabled -Dmad=enabled -Dmpg123=disabled -Dsndfile=enabled \
		-Did3tag=enabled -Dcurl=enabled -Dexpat=enabled -Dzlib=enabled \
		-Ddsd=true \
		-Dupnp=disabled -Dzeroconf=disabled -Dneighbor=false \
		-Dhttpd=false -Dsnapcast=false -Dfifo=true -Dpipe=true -Drecorder=false \
		-Dtest=false
fi

ninja -C "$BLD"
"${CROSS}strip" -o "$BLD/mpd.stripped" "$BLD/mpd"
ls -la "$BLD/mpd.stripped"
