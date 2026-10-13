# x45radio.sh - shared bring-up of the USB radio stick (AIC 88M80: WiFi + Bluetooth), sourced by
# x45bt-run and x45wifi-run. Needs ROOT, BIN, BT (the firmware directory) and the caller's own
# `status <state> <step>` and `fail <message>` functions.
#
# The stick comes up as a mass-storage device (1111:1111). A vendor SCSI command flips it into a boot
# ROM (a69c:8d80), the firmware is downloaded to it over USB, and it re-enumerates (a69c:8d81) with
# Bluetooth on interfaces 0/1 and WiFi on 2. All of that is RAM only, so it is redone after every
# boot or replug; once a69c:8d81 exists, radio_up has nothing left to do.

find_dev() {	# usb device dir with idVendor:idProduct $1:$2
	for d in /sys/bus/usb/devices/[0-9]*-[0-9]*; do
		[ -f $d/idVendor ] 2>/dev/null || continue
		[ "$(cat $d/idVendor 2>/dev/null):$(cat $d/idProduct 2>/dev/null)" = "$1:$2" ] && echo $d && return 0
	done
	return 1
}

wait_dev() {	# wait up to $3 s for $1:$2
	n=0
	while [ $n -lt $(($3 * 2)) ]; do
		find_dev $1 $2 >/dev/null && return 0
		sleep 0.5
		n=$((n + 1))
	done
	return 1
}

# radio_up: leave the stick running its firmware. Safe to call when it already does. A lock keeps the
# WiFi and Bluetooth starters from flipping it at the same time. Returns 0, or calls fail.
radio_up() {
	find_dev a69c 8d81 >/dev/null && return 0
	LOCK=/tmp/x45radio.lock
	n=0
	while ! mkdir $LOCK 2>/dev/null; do
		# a lock left by a starter that is gone is broken after 60 s
		[ $n -ge 120 ] && { rmdir $LOCK 2>/dev/null; n=0; }
		find_dev a69c 8d81 >/dev/null && return 0
		sleep 0.5
		n=$((n + 1))
	done
	trap 'rmdir $LOCK 2>/dev/null' EXIT
	if find_dev a69c 8d81 >/dev/null; then
		rmdir $LOCK 2>/dev/null
		trap - EXIT   # the lock is let go: a later exit of this script must not remove somebody else's
		return 0
	fi
	if find_dev 1111 1111 >/dev/null; then
		SG=
		for s in /sys/class/scsi_generic/sg*; do
			grep -q WIFI6 $s/device/model 2>/dev/null && SG=/dev/$(basename $s)
		done
		[ -n "$SG" ] || fail "radio stick not found"
		status starting "switching the stick on"
		$BIN/sgsend $SG fd0000000000000000000000000000f2
	fi
	if ! find_dev a69c 8d80 >/dev/null; then
		status starting "waiting for the stick"
		wait_dev a69c 8d80 15 || fail "radio stick not found, is it plugged in?"
	fi
	sleep 1
	U=$(find_dev a69c 8d80)
	DEV=/dev/bus/usb/$(printf %03d $(cat $U/busnum))/$(printf %03d $(cat $U/devnum))
	status starting "loading the firmware"
	$BIN/aicload $DEV $BT/aicfw > /tmp/aicload.log 2>&1 || fail "firmware load failed"
	wait_dev a69c 8d81 15 || fail "the stick did not start"
	rmdir $LOCK 2>/dev/null
	trap - EXIT
	RADIO_FRESH=1	# the stick was started just now (the Bluetooth controller wants a moment)
	return 0
}
