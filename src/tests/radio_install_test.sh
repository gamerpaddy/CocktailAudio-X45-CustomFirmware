#!/bin/sh
# x45radio-install against a fake unit root and a fake USB stick: the pinned hashes of the package decide.
# Run from src/: sh tests/radio_install_test.sh
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
INST=$HERE/../deploy/x45radio-install
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fails=0
check() { if [ "$1" = ok ]; then echo "ok   $2"; else echo "FAIL $2"; fails=$((fails + 1)); fi; }

ROOT=$T/root; MNT=$T/mnt; KIT=$MNT/usb/x45-radio-kit
mkdir -p "$ROOT/lib" "$ROOT/bin" "$KIT/bin" "$KIT/wifi/mod"
printf 'bin/a\nwifi/mod/m.ko\n' > "$ROOT/lib/radio-files.txt"
printf 'AAA\n' > "$KIT/bin/a"; printf 'MMM\n' > "$KIT/wifi/mod/m.ko"
uname -r > "$KIT/kernel-release"
# the "package" pins the real hashes; the kit carries a checksum list of its own that must be ignored
( cd "$KIT" && sha256sum bin/a wifi/mod/m.ko ) > "$ROOT/lib/radio-kit.sha256"
( cd "$KIT" && sha256sum bin/a wifi/mod/m.ko ) > "$KIT/SHA256SUMS"
run() { X45_ROOT=$ROOT X45_MOUNTS=$MNT X45_INSTALL_STATUS=$T/status sh "$INST" install; }

run; [ $? -eq 0 ] && [ -x "$ROOT/bin/a" ] && [ "$(cat "$ROOT/wifi/mod/m.ko")" = MMM ] && check ok "installs from a matching kit" || check bad "installs from a matching kit"
run; grep -q 'already installed' "$T/status" && check ok "second run finds nothing to do" || check bad "second run finds nothing to do"

# a tampered file with a consistent list of its own on the stick is refused
printf 'EVIL\n' > "$KIT/bin/a"; ( cd "$KIT" && sha256sum bin/a wifi/mod/m.ko ) > "$KIT/SHA256SUMS"
rm -f "$ROOT/bin/a"
run; [ $? -ne 0 ] && grep -q 'Checksum failed' "$T/status" && [ ! -e "$ROOT/bin/a" ] && check ok "a stick's own checksum list is not trusted" || check bad "a stick's own checksum list is not trusted"
printf 'AAA\n' > "$KIT/bin/a"

# without pinned hashes nothing is installed
mv "$ROOT/lib/radio-kit.sha256" "$T/pinned"
run; [ $? -ne 0 ] && [ ! -e "$ROOT/bin/a" ] && check ok "no pinned hashes: refused" || check bad "no pinned hashes: refused"
mv "$T/pinned" "$ROOT/lib/radio-kit.sha256"

# an outdated installed file is replaced by the pinned one
printf 'OLD\n' > "$ROOT/bin/a"; chmod 755 "$ROOT/bin/a"
run; [ "$(cat "$ROOT/bin/a")" = AAA ] && check ok "an outdated file is updated" || check bad "an outdated file is updated"

# a kit for another kernel is not used
echo 0.0.0-other > "$KIT/kernel-release"; rm -f "$ROOT/bin/a"
run; [ $? -ne 0 ] && [ ! -e "$ROOT/bin/a" ] && check ok "kit for another kernel is skipped" || check bad "kit for another kernel is skipped"

[ "$fails" -eq 0 ] && echo "radio_install_test: all passed" || { echo "radio_install_test: $fails failed"; exit 1; }
