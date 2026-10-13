#!/bin/sh
# ---------------------------------------------------------------------------
# X45 - INSTALL THE x20 REPLACEMENT (x45new)
#
# Runs as root in the updater environment (mmcblk3p2) with cwd = mounted pkg.
#
# Everything is ADDITIVE: nothing in the vendor rootfs is overwritten or
# deleted. Added to the main rootfs (p3):
#   /home/root/x45new/              the replacement userspace (from x45new.tar)
#   /etc/init.d/x45new              start script (copy of x45new.init)
#   /etc/rc5.d/S98x45new            symlink to it
#   /home/root/x45new/ENABLED       switch: remove it to boot the vendor UI
#
# Reverting = remove /home/root/x45new/ENABLED, or delete the paths above. The
# vendor UI stays installed and is the fallback: three boots in a row where the
# new UI never became healthy disable the stack by themselves (see x45new.init).
#
# An existing installation keeps its own config (etc/*) and state; only
# binaries and scripts are replaced.
#
# Every exit path is `exit 0`: the launcher reboots on anything but 1.
# ---------------------------------------------------------------------------

PKG=$(pwd)
LOG=/tmp/x45-install.log
DEST=/tmp/p3/home/root/x45new

exec >"$LOG" 2>&1
echo "=== X45 x45new install payload ==="
echo "date : $(date 2>&1)"

finish() {
    cp "$LOG" /tmp/devFW/x45-install.log 2>/dev/null
    sync
    exit 0
}

# ---- verify the payload BEFORE touching anything ---------------------------
if [ ! -f "$PKG/x45new.tar" ] || [ ! -f "$PKG/x45new.tar.md5" ]; then
    echo "FATAL: payload files missing - no changes made"
    finish
fi
want=$(cut -d' ' -f1 "$PKG/x45new.tar.md5")
have=$(md5sum "$PKG/x45new.tar" | cut -d' ' -f1)
if [ "$want" != "$have" ]; then
    echo "FATAL: x45new.tar checksum mismatch ($have != $want) - no changes made"
    finish
fi
if ! tar tf "$PKG/x45new.tar" >/dev/null 2>&1; then
    echo "FATAL: x45new.tar does not list cleanly - no changes made"
    finish
fi
echo "--- payload verified ($have)"

# ---- mount the main rootfs read-write --------------------------------------
mkdir -p /tmp/p3
if ! mount -t ext4 /dev/mmcblk3p3 /tmp/p3 && ! mount /dev/mmcblk3p3 /tmp/p3; then
    echo "FATAL: cannot mount /dev/mmcblk3p3 - no changes made"
    finish
fi

# ---- sanity check: is this really the X45 rootfs? ---------------------------
if [ ! -f /tmp/p3/usr/sbin/sshd ] || [ ! -f /tmp/p3/home/root/bin/x20 ] || [ ! -d /tmp/p3/etc/rc5.d ]; then
    echo "FATAL: p3 does not look like the X45 rootfs - aborting with NO changes"
    umount /tmp/p3
    finish
fi
echo "--- rootfs version: $(cat /tmp/p3/version 2>&1)"

# enough room? (the tree is about 9 MB; require 40 MB free)
free_kb=$(df -k /tmp/p3 | awk 'NR==2 {print $4}')
if [ -n "$free_kb" ] && [ "$free_kb" -lt 40960 ] 2>/dev/null; then
    echo "FATAL: only ${free_kb} kB free on the rootfs - no changes made"
    umount /tmp/p3
    finish
fi

# ---- 1. the tree, keeping an existing installation's config ----------------
mkdir -p "$DEST"
if [ -d "$DEST/etc" ]; then
    rm -rf "$DEST/etc.keep"
    cp -a "$DEST/etc" "$DEST/etc.keep"
fi
if ! tar xf "$PKG/x45new.tar" -C "$DEST"; then
    echo "WARN: tar reported errors"
fi
if [ -d "$DEST/etc.keep" ]; then
    cp -a "$DEST/etc.keep/." "$DEST/etc/"
    rm -rf "$DEST/etc.keep"
    echo "--- kept the existing etc/"
fi
mkdir -p "$DEST/mpd/playlists" "$DEST/media"
chown -R 0:0 "$DEST"
for f in "$DEST"/bin/*; do chmod 755 "$f"; done
echo 0 > "$DEST/bootcount"

# ---- 2. boot hook ------------------------------------------------------------
cp "$PKG/x45new.init" /tmp/p3/etc/init.d/x45new
chmod 755 /tmp/p3/etc/init.d/x45new
chown 0:0 /tmp/p3/etc/init.d/x45new
ln -sf ../init.d/x45new /tmp/p3/etc/rc5.d/S98x45new

# ---- 3. switch on -------------------------------------------------------------
touch "$DEST/ENABLED"

echo "--- installed:"
ls -la "$DEST" /tmp/p3/etc/init.d/x45new /tmp/p3/etc/rc5.d/S98x45new
cp "$LOG" /tmp/p3/mnt/x45-install.log 2>/dev/null
sync
umount /tmp/p3
echo "=== done ==="
finish
