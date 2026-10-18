#!/bin/bash
# Assemble the file tree that the package installs on the unit, as it will look below /home/root/x45new.
#
#   scripts/stage.sh        -> $STAGE/x45new
#
# Everything comes from three places: the programs built by the other scripts, the files in this repository
# (deploy/, conf/) and a few downloaded data files (CA bundle, UI font, Bluetooth stick firmware).
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"
need_tools tar install

T="$STAGE/x45new"
rm -rf "$T"
mkdir -p "$T/bin" "$T/etc/ssl" "$T/icons" "$T/web/tabs" "$T/lib/sh" "$T/lib/alsa-lib" "$T/share" "$T/fonts"

put() { # put SOURCE NAME : install a program into bin/
	[ -f "$1" ] || die "not built: $1"
	install -m755 "$1" "$T/bin/$2"
}
put_optional() { # put_optional PART SOURCE NAME
	if skipped "$1"; then
		echo "  left out: $3 ($1 is in X45_SKIP)"
	else
		put "$2" "$3"
	fi
}

log "programs"
for p in x45ctl x45fb x45in x45mpc x45ui x45play x45loop x45cd x45rec x45tuner x45web x45upnp x45rb x45wifi x45wd x45art; do
	put "$OUT/$p" "$p"
done
put "$WORK/mpd-build/mpd.stripped" mpd

# shell scripts of the stack (start up, helpers for the receivers, the Wi-Fi hooks)
for f in x45stack x45media x45radio-sync x45ftp x45-airplay-begin x45-airplay-end x45-spotify-event \
	x45bt-run x45btout-run x45wifi-run x45wifi-dhcp x45wifi-event; do
	[ -f "$X45_ROOT/deploy/$f" ] || die "missing deploy/$f"
	sed 's/\r$//' "$X45_ROOT/deploy/$f" >"$T/bin/$f"
	chmod 755 "$T/bin/$f"
done

put_optional airplay "$WORK/shairport-sync-4.3.4/shairport-sync.stripped" shairport-sync
put_optional upnp "$SYSROOT/bin-upmpdcli" upmpdcli
put_optional spotify "$SYSROOT/bin/librespot" librespot
if ! skipped wifi; then
	put "$SYSROOT/bin/iw" iw
	put "$SYSROOT/bin/wpa_supplicant" wpa_supplicant
	put "$SYSROOT/bin/wpa_cli" wpa_cli
fi
if ! skipped bluetooth; then
	put "$OUT/x45bt" x45bt
	put "$OUT/aicload" aicload
	put "$OUT/sgsend" sgsend
	mkdir -p "$T/bt/aicfw"
	for f in "$DL"/aic-*.bin; do
		[ -f "$f" ] || die "the Bluetooth stick firmware is not downloaded (run: python3 scripts/fetch.py)"
		install -m644 "$f" "$T/bt/aicfw/$(basename "$f" | sed 's/^aic-//')"
	done
fi

log "configuration"
for f in mpd.conf shairport-sync.conf upmpdcli.conf stations.txt; do
	install -m644 "$X45_ROOT/conf/$f" "$T/etc/$f"
done
install -m644 "$X45_ROOT/conf/x45ui.conf.example" "$T/etc/x45ui.conf"
[ -f "$DL/cacert.pem" ] || die "missing download: cacert.pem (run: python3 scripts/fetch.py)"
install -m644 "$DL/cacert.pem" "$T/etc/ssl/cacert.pem"

log "icons, web pages, scripts, fonts"
install -m644 "$X45_ROOT"/deploy/icons/*.png "$T/icons/"
install -m644 "$X45_ROOT"/deploy/web/*.html "$T/web/"
install -m644 "$X45_ROOT"/deploy/web/tabs/*.js "$T/web/tabs/"
for f in "$X45_ROOT"/deploy/lib/sh/*; do
	sed 's/\r$//' "$f" >"$T/lib/sh/$(basename "$f")"
	chmod 644 "$T/lib/sh/$(basename "$f")"
done
tmp="$WORK/liberation-fonts"
rm -rf "$tmp"; mkdir -p "$tmp"
tar xf "$DL/liberation-fonts-ttf-2.1.5.tar.gz" -C "$tmp"
install -m644 "$tmp"/liberation-fonts-ttf-*/LiberationSans-Regular.ttf "$T/fonts/ui.ttf"

log "data of the libraries"
[ -d "$SYSROOT/share/alsa" ] || die "the ALSA configuration is missing (run scripts/deps.sh alsa)"
cp -a "$SYSROOT/share/alsa" "$T/share/alsa"
if ! skipped upnp; then
	d=$(unpack upmpdcli-1.8.18.tar.gz)
	mkdir -p "$T/share/upmpdcli"
	for f in AVTransport.xml ConnectionManager.xml OHCredentials.xml OHInfo.xml OHPlaylist.xml OHProduct.xml OHRadio.xml \
		OHReceiver.xml OHTime.xml OHVolume.xml RenderingControl.xml description.xml icon.png presentation.html protocolinfo.txt; do
		install -m644 "$d/src/$f" "$T/share/upmpdcli/$f"
	done
fi

# modes: directories 755, data 644, programs 755 (done above)
find "$T" -type d -exec chmod 755 {} +

echo
echo "staged: $T ($(du -sk "$T" | cut -f1) kB, $(find "$T" -type f | wc -l) files)"
