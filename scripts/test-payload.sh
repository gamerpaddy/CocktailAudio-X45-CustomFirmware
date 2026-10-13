#!/bin/bash
# Test the installer scripts inside the packages without a unit.
#
#   scripts/test-payload.sh [install.pkg [remove.pkg]]     (default: the newest ones in dist/)
#
# The installers expect to run as root in the unit's updater with the main partition at /tmp/p3. Here a made-up
# root file system takes its place (mount, umount and chown are replaced by do-nothing stubs), and the installers run
# from the unpacked packages exactly as the updater would run them. Nothing outside /tmp is touched. It checks:
#   - a fresh install puts the program tree and the start hook in place and touches nothing else
#   - a second install keeps the settings
#   - a damaged package changes nothing
#   - a root file system that does not look like the unit's changes nothing
#   - the removal package takes the new software away and keeps the settings
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$HERE/env.sh"

PKG="${1:-$(ls -t "$X45_ROOT"/dist/X45-CA-*.pkg 2>/dev/null | head -1)}"
RPKG="${2:-$(ls -t "$X45_ROOT"/dist/X45-REMOVE-*.pkg 2>/dev/null | head -1)}"
[ -f "$PKG" ] || die "no install package (run ./make-pkg.sh first)"
[ -f "$RPKG" ] || die "no removal package (run ./make-pkg.sh first)"
for d in /tmp/p3 /tmp/devFW; do
	[ ! -e "$d" ] || die "$d exists; this test needs to create it. Remove it first if it is a leftover."
done

W=$(mktemp -d /tmp/x45-test.XXXXXX)
trap 'rm -rf "$W" /tmp/p3 /tmp/devFW /tmp/x45-install.log /tmp/x45-remove.log' EXIT
mkdir -p "$W/stubs" "$W/inst" "$W/rem"
for c in mount umount chown; do printf '#!/bin/sh\nexit 0\n' >"$W/stubs/$c"; chmod 755 "$W/stubs/$c"; done
python3 "$SCRIPTS/romfs.py" "$PKG" extract "$W/inst" 2>/dev/null
python3 "$SCRIPTS/romfs.py" "$RPKG" extract "$W/rem" 2>/dev/null

pass=0; fail=0
check() {   # check "what" command...
	local what="$1"; shift
	if "$@" >/dev/null 2>&1; then pass=$((pass + 1)); printf '  ok    %s\n' "$what"
	else fail=$((fail + 1)); printf '  FAIL  %s\n' "$what"; fi
}
fake_root() {
	rm -rf /tmp/p3 /tmp/devFW
	mkdir -p /tmp/p3/usr/sbin /tmp/p3/home/root/bin /tmp/p3/etc/init.d /tmp/p3/etc/rc5.d /tmp/p3/mnt /tmp/devFW
	: >/tmp/p3/usr/sbin/sshd
	: >/tmp/p3/home/root/bin/x20
	echo "X45-CA-1.0.0.r1789" >/tmp/p3/version
	echo "original" >/tmp/p3/etc/init.d/original-script
}
run() {   # run DIR: the installer of that unpacked package
	( cd "$1" && PATH="$W/stubs:$PATH" sh ./updateFW.sh >/dev/null 2>&1 ); echo $? >"$W/rc"
}
snapshot() { ( cd /tmp/p3 && find . | sort && find . -type f -exec md5sum {} + | sort ) 2>/dev/null; }
D=/tmp/p3/home/root/x45new

echo "install package: $PKG"
echo "removal package: $RPKG"
echo
echo "fresh install"
fake_root
run "$W/inst"
check "installer exits with 0" test "$(cat "$W/rc")" = 0
check "program tree is there" test -x "$D/bin/x45ui" -a -x "$D/bin/mpd" -a -x "$D/bin/x45stack"
check "start script is there" test -x /tmp/p3/etc/init.d/x45new
check "start hook is a link to it" test "$(readlink /tmp/p3/etc/rc5.d/S98x45new)" = ../init.d/x45new
check "switch file exists" test -f "$D/ENABLED"
check "boot counter is 0" test "$(cat "$D/bootcount")" = 0
check "the original start script is untouched" test "$(cat /tmp/p3/etc/init.d/original-script)" = original
check "the installer log was copied" test -s /tmp/devFW/x45-install.log

echo "second install keeps the settings"
echo "# mine" >>"$D/etc/x45ui.conf"
run "$W/inst"
check "installer exits with 0" test "$(cat "$W/rc")" = 0
check "settings are kept" grep -q '^# mine' "$D/etc/x45ui.conf"
check "program tree is still there" test -x "$D/bin/x45ui"

echo "damaged package changes nothing"
fake_root
cp -r "$W/inst" "$W/bad"
echo "00000000000000000000000000000000  x45new.tar" >"$W/bad/x45new.tar.md5"
snapshot >"$W/before"
run "$W/bad"
snapshot >"$W/after"
check "installer exits with 0" test "$(cat "$W/rc")" = 0
check "root file system unchanged" cmp -s "$W/before" "$W/after"
rm -rf "$W/bad"

echo "a root file system that is not the unit's changes nothing"
fake_root
rm -f /tmp/p3/home/root/bin/x20
snapshot >"$W/before"
run "$W/inst"
snapshot >"$W/after"
check "installer exits with 0" test "$(cat "$W/rc")" = 0
check "root file system unchanged" cmp -s "$W/before" "$W/after"

echo "removal"
fake_root
run "$W/inst"
echo "# mine" >>"$D/etc/x45ui.conf"
run "$W/rem"
check "removal exits with 0" test "$(cat "$W/rc")" = 0
check "program tree is gone" test ! -e "$D/bin" -a ! -e "$D/ENABLED" -a ! -e "$D/web"
check "start script and hook are gone" test ! -e /tmp/p3/etc/init.d/x45new -a ! -L /tmp/p3/etc/rc5.d/S98x45new
check "settings are kept" grep -q '^# mine' "$D/etc/x45ui.conf"
check "the original start script is untouched" test "$(cat /tmp/p3/etc/init.d/original-script)" = original
check "the original software is untouched" test -e /tmp/p3/home/root/bin/x20 -a -e /tmp/p3/usr/sbin/sshd
run "$W/rem"
check "removing twice is harmless" test "$(cat "$W/rc")" = 0

echo
echo "passed: $pass   failed: $fail"
[ "$fail" = 0 ]
