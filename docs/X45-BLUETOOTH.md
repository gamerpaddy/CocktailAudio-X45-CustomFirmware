# Bluetooth on the X45

The kernel of the unit (3.14) has no Bluetooth support at all: no `AF_BLUETOOTH`, no HCI, no `btusb`. So the
Bluetooth stack runs in userspace, over libusb, talking to a USB stick. The stack is BTstack, the stick is one
specific kind of cheap "WiFi 6" stick (see below). Bluetooth is optional and work in progress.

What works: the unit shows up as a Bluetooth speaker ("CocktailAudio-X45"), a phone pairs with it (numeric
comparison, shown on the panel, confirmed with OK), and A2DP audio (SBC) plays through the normal DAC path.
AVRCP works, so play and pause from the phone are seen. Only phones that were paired before can connect.

The unit can also be the source: it sends its sound to a paired headset or speaker (A2DP, SBC), pairing from the
panel, auto-connect, and the main outputs are muted while the headset is connected.

Not done: anything but that one kind of stick, codecs other than SBC (no aptX or LDAC), and Bluetooth LE. The stick
needs a bring-up after every power on (see below), which `x45bt-run` does.

## The stick

It is sold as "WiFi 6 USB adapter" with the AIC 8800D80 chip. Properties that matter:

1. After plugging it in, it is a USB mass storage device (a small disk with a Windows installer on it), USB id
   `1111:1111`, SCSI model `WIFI6`. A normal SCSI eject does not change anything.
2. Two vendor SCSI commands (16 byte CDB, byte 0 is `FD`, bytes 1 to 14 are zero, byte 15 is the command) talk to
   it: `F3` reads five bytes (the chip name, "88M80") and `F2` makes the stick drop off the bus and return as
   `a69c:8d80`, the boot ROM download interface (one vendor interface, bulk endpoint 1 out and 2 in).
   `src/tools/sgsend.c` is a small SG_IO tool that sends them.
3. In that mode the firmware has to be loaded over the bulk endpoints. `src/tools/aicload.c` does this in
   userspace, following the sequence of the Linux aic8800 driver: read the chip id, write the address table and
   the patch files, apply the patch table, write the main firmware to 0x120000, start it.
4. The stick then returns as `a69c:8d81` ("AIC 8800D80") with a standard Bluetooth HCI interface and a vendor
   interface for Wi-Fi. HCI answers plain commands; no extra vendor commands are needed for Bluetooth.

All of this is RAM only. After every power on the sequence is: `sgsend` (flip), `aicload` (firmware), wait for
`a69c:8d81`, start `x45bt`. `deploy/x45bt-run` does exactly that and writes its state to `/tmp/x45bt.status`.

The firmware files are not in this repository. `scripts/fetch.py` downloads them (see `sources.txt`) and the
package puts them in `/home/root/x45new/bt/aicfw`.

## Things that took a while to find

* **Never reset the stick and never set its configuration.** `libusb_reset_device` and
  `libusb_set_configuration` both make the chip fall back to `a69c:8d80` with its RAM firmware gone. BTstack does
  the reset when it opens the device, so the build defines `X45_NO_USB_RESET` and the patch compiles those calls
  out. BTstack also has to be told that this device is allowed: it is class `ef/02/01`, not `e0/01/01`.
* **Pairing worked but nothing connected: EDR packets were never acknowledged.** Packets up to about 48 bytes (a
  single 2-DH1) were fine, larger ones stalled. The fix is to allow BR packet types only (`0xcc18` when creating a
  connection and `0xff1e` after every ACL connection opens).
* **Stuttering was the link role.** If the unit connects out to the phone, it becomes master of the link, and the
  stick as master polled the phone only every 70 ms or so. `x45bt` now asks for the slave role on every new
  connection. With that, a healthy stream is 250 packets per 5 seconds and no underruns. `/tmp/x45bt.history`
  has one line per 5 seconds (packets, lost, late, underruns), which is how this was found.
* **Wrong pitch and distortion was the clock family.** The DAC board has to be told whether the 44.1 kHz or the
  48 kHz family is in use (see `src/libx45/x45clock.c`). The ALSA backend of `x45bt` selects it before opening the
  device and again every 300 ms while it plays.
* **The sink needs the DAC for itself.** Selecting the Bluetooth input stops MPD first.
* **Never kill the sink while it sets up the PCM.** A SIGTERM at the wrong moment left the USB controller in a
  state that only a reboot cleared. Stop it with SIGINT and wait.

## Files

| File | What |
|---|---|
| `src/tools/sgsend.c` | sends the two vendor SCSI commands |
| `src/tools/aicload.c` | loads the firmware into the stick |
| `patches/btstack-e385539-x45.patch` | changes to BTstack (libusb port with ALSA backend, no USB reset, packet types, whitelist, clock family) |
| `patches/btstack-e385539-x45bt.c.patch` | turns BTstack's A2DP sink demo into the headless `x45bt` |
| `scripts/bt.sh` | builds `x45bt`, `sgsend` and `aicload` (libusb comes from `scripts/deps.sh`) |
| `deploy/x45bt-run` | bring-up on the unit and supervision of `x45bt` |
| `src/x45ui/` | the Bluetooth input (pairing screen, "pair new" and "forget all" in the RETURN menu) |

BTstack is free for non-commercial use only; see [THIRD-PARTY.md](THIRD-PARTY.md).
