#!/bin/sh
# ---------------------------------------------------------------------------
# X45 - REMOVE THE x20 REPLACEMENT (x45new)
#
# Runs as root in the updater environment (mmcblk3p2) with cwd = mounted pkg.
#
# Takes away what payload.sh added to the main rootfs (p3):
#   /home/root/x45new/              the replacement userspace, except etc/
#   /etc/init.d/x45new              start script
#   /etc/rc5.d/S98x45new            symlink to it
#
# /home/root/x45new/etc (settings, station lists) is kept, so that installing
# again picks the settings up. Nothing else on the unit is touched: the
# original software was never changed and starts again at the next boot.
#
# Every exit path is `exit 0`: the launcher reboots on anything but 1.
# ---------------------------------------------------------------------------

LOG=/tmp/x45-remove.log
DEST=/tmp/p3/home/root/x45new

exec >"$LOG" 2>&1
echo "=== X45 x45new remove payload ==="
echo "date : $(date 2>&1)"

finish() {
    cp "$LOG" /tmp/devFW/x45-remove.log 2>/dev/null
    sync
    exit 0
}

mkdir -p /tmp/p3
if ! mount -t ext4 /dev/mmcblk3p3 /tmp/p3 && ! mount /dev/mmcblk3p3 /tmp/p3; then
    echo "FATAL: cannot mount /dev/mmcblk3p3 - no changes made"
    finish
fi

# is this really the X45 rootfs?
if [ ! -f /tmp/p3/usr/sbin/sshd ] || [ ! -f /tmp/p3/home/root/bin/x20 ] || [ ! -d /tmp/p3/etc/rc5.d ]; then
    echo "FATAL: p3 does not look like the X45 rootfs - aborting with NO changes"
    umount /tmp/p3
    finish
fi
echo "--- rootfs version: $(cat /tmp/p3/version 2>&1)"

# the start hook first, so that nothing starts the stack while it is taken apart
rm -f /tmp/p3/etc/rc5.d/S98x45new
rm -f /tmp/p3/etc/init.d/x45new

if [ -d "$DEST" ]; then
    rm -f "$DEST/ENABLED"
    for e in "$DEST"/* "$DEST"/.[!.]*; do
        [ -e "$e" ] || continue
        [ "$e" = "$DEST/etc" ] && continue
        rm -rf "$e"
    done
    echo "--- removed the program tree, kept etc/"
else
    echo "--- there was no program tree"
fi

ls -la /tmp/p3/home/root /tmp/p3/etc/init.d/x45new /tmp/p3/etc/rc5.d/S98x45new 2>&1
cp "$LOG" /tmp/p3/mnt/x45-remove.log 2>/dev/null
sync
umount /tmp/p3
echo "=== done ==="
finish
