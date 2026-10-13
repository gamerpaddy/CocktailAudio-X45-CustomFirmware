#!/bin/bash
# Cross-build librespot (Spotify Connect receiver) for the unit: static musl, ALSA backend.
#
#   scripts/librespot.sh
#
# The Rust compiler and the crates librespot needs come from the download cache (scripts/fetch.py puts them there),
# so the build itself runs without network access.
#
# Output: $SYSROOT/bin/librespot (stripped)
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"
need_tools tar xz gcc
[ -f "$SYSROOT/.done-alsa" ] || die "run scripts/deps.sh first"
[ -d "$DL/cargo-vendor" ] || die "the Rust crates are not downloaded yet (run: python3 scripts/fetch.py)"

RUSTROOT="$X45_BUILD/rust"
SRC="$WORK/librespot-0.8.0"

if [ ! -x "$RUSTROOT/bin/cargo" ]; then
	log "installing the Rust compiler"
	for t in rust-1.99.0-x86_64-unknown-linux-gnu rust-std-1.99.0-armv7-unknown-linux-musleabihf; do
		[ -f "$DL/$t.tar.xz" ] || die "missing download: $t.tar.xz (run: python3 scripts/fetch.py)"
		tar -C "$WORK" -xf "$DL/$t.tar.xz"
		(cd "$WORK/$t" && ./install.sh --prefix="$RUSTROOT" --disable-ldconfig >/dev/null)
	done
fi
export PATH="$RUSTROOT/bin:$TC/bin:$PATH"

rm -rf "$SRC"
tar -C "$WORK" -xzf "$DL/librespot-0.8.0.tar.gz"
cp -r "$DL/cargo-vendor" "$SRC/vendor"
rm -f "$SRC/rust-toolchain.toml"

mkdir -p "$SRC/.cargo"
cat > "$SRC/.cargo/config.toml" <<CARGOEOF
[source.crates-io]
replace-with = "vendored"
[source.vendored]
directory = "vendor"
[build]
target = "armv7-unknown-linux-musleabihf"
[target.armv7-unknown-linux-musleabihf]
linker = "$HOST-gcc"
rustflags = ["-C", "target-feature=+crt-static", "-C", "target-cpu=cortex-a9"]
[env]
CC_armv7_unknown_linux_musleabihf = "$HOST-gcc"
AR_armv7_unknown_linux_musleabihf = "$HOST-ar"
CFLAGS_armv7_unknown_linux_musleabihf = "-march=armv7-a -mfpu=neon -mfloat-abi=hard"
PKG_CONFIG_ALLOW_CROSS = "1"
PKG_CONFIG_PATH = "$SYSROOT/lib/pkgconfig"
PKG_CONFIG_LIBDIR = "$SYSROOT/lib/pkgconfig"
PKG_CONFIG_SYSROOT_DIR = "$SYSROOT"
CARGOEOF

cd "$SRC"
cargo build --release --offline --no-default-features \
	--features "rustls-tls-webpki-roots alsa-backend with-libmdns" 2>&1 | tail -40
mkdir -p "$SYSROOT/bin"
"${CROSS}strip" -o "$SYSROOT/bin/librespot" target/armv7-unknown-linux-musleabihf/release/librespot
ls -l "$SYSROOT/bin/librespot"
