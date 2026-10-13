#!/bin/bash
# Copy the package to a USB stick and check the copy, then show what to do on the unit.
#
#   ./install.sh                  lists drives that look like USB sticks and asks which one to use
#   ./install.sh /media/me/STICK  uses this folder (the root of the stick)
#   ./install.sh --remove         the same with the package that takes the new software off the unit again
#
# The stick should be formatted FAT32. The file is copied as firmware.pkg, the name the unit's updater looks for.
# The copy is compared byte by byte with the original, because a damaged package is worse than none.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

MODE=install
if [ "${1:-}" = --remove ]; then MODE=remove; shift; fi
if [ "$MODE" = remove ]; then
	PKG=$(ls -t "$HERE"/dist/X45-REMOVE-*.pkg 2>/dev/null | head -1 || true)
else
	PKG=$(ls -t "$HERE"/dist/X45-CA-*.pkg 2>/dev/null | head -1 || true)
fi
[ -n "$PKG" ] || { echo "No package found in $HERE/dist. Run ./make-pkg.sh first." >&2; exit 1; }
echo "Package: $PKG"
echo "SHA256 : $(sha256sum "$PKG" | cut -d' ' -f1)"
echo

DEST="${1:-}"
if [ -z "$DEST" ]; then
	echo "Drives that may be your USB stick:"
	cands=()
	for d in /media/"$USER"/* /run/media/"$USER"/* /mnt/[a-z] /Volumes/*; do
		[ -d "$d" ] && [ -w "$d" ] && cands+=("$d")
	done
	if [ ${#cands[@]} -eq 0 ]; then
		echo "  (none found)"
		echo
		echo "Plug the stick in, make sure it is mounted, and run:  ./install.sh /path/to/the/stick"
		echo "Under WSL a Windows drive shows up as /mnt/e (for E:); on Windows it is easier to use install.bat."
		exit 1
	fi
	i=0
	for d in "${cands[@]}"; do
		i=$((i + 1))
		printf '  %d) %s   (%s free)\n' "$i" "$d" "$(df -h "$d" | awk 'NR==2 {print $4}')"
	done
	printf 'Number of the stick (anything else cancels): '
	read -r n
	case "$n" in
	'' | *[!0-9]*) echo "cancelled"; exit 1 ;;
	esac
	[ "$n" -ge 1 ] && [ "$n" -le ${#cands[@]} ] || { echo "cancelled"; exit 1; }
	DEST="${cands[$((n - 1))]}"
fi

[ -d "$DEST" ] && [ -w "$DEST" ] || { echo "$DEST is not a folder I can write to." >&2; exit 1; }
need=$(( $(stat -c%s "$PKG") / 1024 + 1024 ))
free=$(df -k "$DEST" | awk 'NR==2 {print $4}')
[ "$free" -ge "$need" ] || { echo "Not enough free space on $DEST (need ${need} kB, have ${free} kB)." >&2; exit 1; }

if [ -e "$DEST/firmware.pkg" ]; then
	printf '%s/firmware.pkg exists. Replace it? [y/N] ' "$DEST"
	read -r a
	case "$a" in y | Y) ;; *) echo "cancelled"; exit 1 ;; esac
fi

echo "Copying (this can take a minute) ..."
cp -f "$PKG" "$DEST/firmware.pkg"
sync
if cmp -s "$PKG" "$DEST/firmware.pkg"; then
	echo "Copy verified."
else
	rm -f "$DEST/firmware.pkg"
	echo "The copy differs from the original. The stick may be damaged; I removed the bad file. Try another stick." >&2
	exit 1
fi

if [ "$MODE" = remove ]; then
	cat <<'EOF'

Now on the unit:
  1. Eject the stick properly, then plug it into the USB port of the unit.
  2. Open the unit's normal firmware update (Setup menu), pick firmware.pkg from the stick and confirm.
  3. Wait until the unit has restarted by itself. It starts the original software again.

This removes the new software from the unit. Your settings in /home/root/x45new/etc stay, so installing again picks
them up. See README.md.
EOF
	exit 0
fi

cat <<'EOF'

Now on the unit:
  1. Eject the stick properly, then plug it into the USB port of the unit.
  2. Open the unit's normal firmware update: the Setup menu, then the firmware update entry. Pick firmware.pkg
     from the stick and confirm. (The exact wording depends on the language and version of the stock firmware.)
  3. Wait until the unit has restarted by itself. The first start of the new software takes a little longer.

If the unit does not offer a firmware update (for example because it does not start), the updater can also be
started by holding the MENU key on the unit while switching the power on, with the stick plugged in. It looks for a
file named firmware.pkg.

Going back: the installation only adds files, the original software stays on the unit. After ./make-pkg.sh,
./install.sh --remove puts the removal package on a stick; use it like the install package. (If the new software
fails to start three times in a row, the unit goes back to the original software by itself.) See README.md.
EOF
