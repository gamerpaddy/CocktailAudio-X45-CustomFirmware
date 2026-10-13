# Shared settings for every build script. Source it, do not run it:  . "$(dirname "$0")/env.sh"
#
# Everything the build creates lives under X45_BUILD, never inside the source tree:
#   $X45_BUILD/dl         downloaded source archives (checked against scripts/sources.txt)
#   $X45_BUILD/toolchain  the ARM cross compiler
#   $X45_BUILD/host       native helper tools (pkgconf, ninja, meson, autotools)
#   $X45_BUILD/sysroot    static libraries built for the unit
#   $X45_BUILD/work       unpacked sources and build directories
#   $X45_BUILD/out        finished programs of the panel and web code
#   $X45_BUILD/stage      the file tree that goes into the package
#   $X45_BUILD/pkg        the finished .pkg file
#
# Environment variables you may set before running a script:
#   X45_BUILD   where all of the above goes (default: <repo>/.build, or ~/x45-build when the repo is on a Windows drive)
#   X45_DL      download cache (default: $X45_BUILD/dl); can be shared between checkouts
#   X45_SKIP    space separated list of optional parts to leave out: airplay spotify upnp wifi bluetooth
#   X45_JOBS    number of parallel compile jobs (default: number of CPU cores)

if [ -z "${X45_ROOT:-}" ]; then
	X45_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
fi
SCRIPTS="$X45_ROOT/scripts"
SRCDIR="$X45_ROOT/src"
PATCHES="$X45_ROOT/patches"

if [ -z "${X45_BUILD:-}" ]; then
	case "$X45_ROOT" in
	/mnt/[a-z]/*) X45_BUILD="$HOME/x45-build" ;;   # Windows drives are slow and lose file modes: build on the Linux side
	*) X45_BUILD="$X45_ROOT/.build" ;;
	esac
fi
DL="${X45_DL:-$X45_BUILD/dl}"
WORK="$X45_BUILD/work"
SYSROOT="$X45_BUILD/sysroot"
HOSTROOT="$X45_BUILD/host"
OUT="$X45_BUILD/out"
STAGE="$X45_BUILD/stage"
PKGDIR="$X45_BUILD/pkg"
TCDIR="$X45_BUILD/toolchain"
TC="$TCDIR/armv7-eabihf--musl--stable-2021.11-1"
HOST=arm-buildroot-linux-musleabihf
CROSS="$TC/bin/$HOST-"
JOBS="${X45_JOBS:-$(nproc 2>/dev/null || echo 2)}"

# where the program tree lives on the unit; alsa-lib, curl and others bake paths below it into the binaries
DEVROOT=/home/root/x45new

ARCHFLAGS="-march=armv7-a -mtune=cortex-a9 -mfpu=neon -mfloat-abi=hard"

mkdir -p "$DL" "$WORK" "$SYSROOT" "$HOSTROOT/bin" "$OUT" "$STAGE"

log() { printf '\n==> %s\n' "$*"; }
die() { printf '\nERROR: %s\n' "$*" >&2; exit 1; }
skipped() { case " ${X45_SKIP:-} " in *" $1 "*) return 0 ;; *) return 1 ;; esac; }
have() { [ -f "$SYSROOT/.done-$1" ]; }
mark() { touch "$SYSROOT/.done-$1"; }

# unpack TARBALL-NAME  ->  prints the top directory of the unpacked tree (unpacks only once)
unpack() {
	local tb="$DL/$1" top
	[ -f "$tb" ] || die "missing download: $1 (run: python3 scripts/fetch.py)"
	top=$(tar tf "$tb" 2>/dev/null | head -1 | cut -d/ -f1) || true   # head ends the pipe early: tar may see SIGPIPE
	[ -n "$top" ] || die "cannot read $1"
	[ -d "$WORK/$top" ] || tar xf "$tb" -C "$WORK"
	echo "$WORK/$top"
}

# cross_env: compiler and flags for building for the unit. Our pkgconf must come before the toolchain's own wrapper,
# which ignores PKG_CONFIG_PATH.
cross_env() {
	export PATH="$HOSTROOT/bin:$TC/bin:$PATH"
	export CC="${CROSS}gcc" CXX="${CROSS}g++" AR="${CROSS}ar" RANLIB="${CROSS}ranlib" STRIP="${CROSS}strip"
	export PKG_CONFIG_PATH="$SYSROOT/lib/pkgconfig" PKG_CONFIG_LIBDIR="$SYSROOT/lib/pkgconfig" PKG_CONFIG_SYSROOT_DIR=""
	export CFLAGS="-O2 $ARCHFLAGS -I$SYSROOT/include"
	export CXXFLAGS="$CFLAGS"
	export LDFLAGS="-L$SYSROOT/lib"
}

# native_env: forget every cross setting (for the helper tools that run on this computer)
native_env() {
	unset CC CXX AR RANLIB STRIP CFLAGS CXXFLAGS LDFLAGS PKG_CONFIG_PATH PKG_CONFIG_LIBDIR PKG_CONFIG_SYSROOT_DIR
	export PATH="$HOSTROOT/bin:$PATH"
}

# need_tools CMD...: stop with a clear message when a program the build needs is missing
need_tools() {
	local c miss=""
	for c in "$@"; do command -v "$c" >/dev/null 2>&1 || miss="$miss $c"; done
	[ -z "$miss" ] || die "missing programs:$miss  (run ./setup.sh, or install them with your package manager)"
}

# meson cross file shared by the meson based builds (extra c/cpp args via $1 and $2)
write_cross_file() { # write_cross_file FILE [extra cpp args, e.g. "'-I/x'"] [extra cpp link args]
	cat >"$1" <<CROSSEOF
[binaries]
c = '${CROSS}gcc'
cpp = '${CROSS}g++'
ar = '${CROSS}ar'
strip = '${CROSS}strip'
pkg-config = '$HOSTROOT/bin/pkg-config'

[host_machine]
system = 'linux'
cpu_family = 'arm'
cpu = 'armv7'
endian = 'little'

[built-in options]
c_args = ['-O2', '-march=armv7-a', '-mtune=cortex-a9', '-mfpu=neon', '-mfloat-abi=hard', '-I$SYSROOT/include']
cpp_args = ['-O2', '-march=armv7-a', '-mtune=cortex-a9', '-mfpu=neon', '-mfloat-abi=hard', '-I$SYSROOT/include'${2:+, $2}]
c_link_args = ['-static', '-L$SYSROOT/lib']
cpp_link_args = ['-static', '-L$SYSROOT/lib'${3:+, $3}]
pkg_config_path = ['$SYSROOT/lib/pkgconfig']
prefer_static = true
default_library = 'static'

[properties]
needs_exe_wrapper = true
CROSSEOF
}
