# The web file manager (`/api/files`, `files.html`)

An Explorer-style page for the unit's storage: browse the attached disks, upload, download, rename, move, copy,
delete, make folders, search, zip a selection, view and edit text files. Served by `x45web`
(`src/x45web/api_files.c`; the page is `web/files.html` with `web/tabs/files.js`).

## What it may touch

Only the attached `/dev/sd*` disks (and the internal music folder). Every path is resolved with `realpath` and must
lie inside one of those roots, so `..` and links out of the disks are refused (403). A whole disk can be browsed but
not renamed, moved or deleted. Names containing `/`, control characters, or that are exactly `.`/`..` are refused
(a name like `Wait...` is fine). A link inside a copied folder is copied as a link, never followed.

## Reading

```
GET  /api/files/roots                          [{name,path,fs,rw,total,free}]
GET  /api/files/list?p=DIR                     {path,root,rw,total,free,items:[{n,t,s,m,l?}],cut}
GET  /api/files/get?p=FILE[&dl=1]              the file (Range supported); text/audio/images inline, the rest as a download
GET  /api/files/size?p=A&p=B                   {files,dirs,bytes,cut}
GET  /api/files/find?p=DIR&q=TEXT              names below DIR containing TEXT (at most 1000, 20 s, 300000 entries)
POST /api/files/zip?p=A&p=B[&name=N.zip]       the selection as one stored zip, streamed (zip64 when needed)
                                               (GET works too, so a plain link can carry the selection; add tok= with a web_token)
```

Paths are absolute and percent-encoded with `encodeURIComponent` (a space is `%20`; `+` stays a plus, so a client
that encodes spaces as `+` gets "no such file").

## Writing

```
PUT  /api/files/put?p=FILE[&over=1]            the body becomes the file
POST /api/files/op?cmd=mkdir&p=PARENT&name=N
POST /api/files/op?cmd=rename&p=PATH&name=N
POST /api/files/op?cmd=move|copy&p=A&p=B&to=DIR   (p repeats; across disks a move copies, then deletes)
POST /api/files/op?cmd=delete&p=A&p=B
```

`op` answers `{"done":n,"failed":n,"error":"first reason"}`. With a `web_token` set every request needs it
(`Authorization: Bearer`, or `tok=` in the query for links).

How a change is made:

* **One at a time.** A write takes `/tmp/x45files.lock` (`flock`, non-blocking); a second one gets 503 "busy with
  another change". Reads are never blocked. The library delete of the panel and of `POST /api/library/delete` takes
  the same lock and the same hold marker, so a delete cannot remount a disk under an upload.
* **Disks are read-only normally** (x45media keeps them so). For a change the disk is remounted read-write, the
  marker `/tmp/x45media.hold` stops x45media from putting it back, and afterwards `sync` + remount read-only.
* **Uploads go to `.NAME.x45up`** beside the target, are `fsync`ed, then renamed over the target (`over=1`) or into
  place. A refused upload (exists, no room: 507, locked: 503) is answered before the body is read.
* **A killed upload cleans up.** `SIGTERM`/`SIGINT`/`SIGHUP` to the request's process (an x45web restart, `pkill`)
  unlink the temp file and the hold marker and exit; the lock goes with the process and x45media puts the disk back
  to read-only on its next pass. A client that disappears or stalls for 30 s gets the same. After a hard kill
  (power, `SIGKILL`) a `.NAME.x45up` can remain: the next upload into that folder removes every `*.x45up` in it
  while it holds the write lock (nothing else can be uploading then).
* **`Expect: 100-continue`** is answered as soon as the request is known to be acceptable, before the disk is
  switched to read-write. (Without it curl waited a second for the go-ahead on every body over a megabyte.)
* **Never restart x45web while the lock is held** (`flock -n /tmp/x45files.lock true` fails): the upload in flight is
  cut. This is how the last piece of an album upload was lost on 2026-10-09. The deploy snippet:
  `exec 9>/tmp/x45files.lock; flock -n 9 || exit 1; cp bin/x45web bin/x45web.prev; mv up/x45web.new bin/x45web; chmod +x bin/x45web; pkill -x x45web`
  (the stack's loop starts the new one 3 s later; a binary that came over `scp` has no execute bit).

## The library follows (`libsync.c`)

MPD only knows files it has scanned. After a successful upload, rename, move, copy, mkdir or delete the request
notes the folders it touched (`/tmp/x45libsync.dirs`) and starts a detached worker that waits 6 s; if no newer
change came in it turns the folders into `lib update <uri>` lines for x45ui (`/tmp/x45ui.cmd`). A thirteen-track
album upload is one scan. URIs come from the links x45media keeps in `/home/root/x45new/media` (`HDD4 -> /mnt/HDD4/My Music`);
a deleted folder is replaced by the nearest one still there; folders outside every link (backups, firmware files)
are ignored; more than 8 folders, or a path longer than the panel's command line, updates everything. x45ui then
reloads the list on the panel and the web page when MPD reports the update finished.

Verified 2026-10-09: upload of an mp3 into `Recordings` showed up in `lsinfo HDD4/Recordings` within 9 s; deleting it
removed the (then empty) folder from MPD again.

## Why uploads over the wire are slow (2026-10-09)

Measured from the PC, same files, same minute:

| path | speed |
|---|---|
| download from the unit, `eth0` | 30+ MB/s |
| raw TCP **to** the unit's `eth0`, no x45web, no disk | 0.9-1.2 MB/s |
| raw TCP to the unit's WiFi | 13 MB/s |
| raw TCP to a sink that writes to the HDD (WiFi) | 10.9 MB/s (disk write 15 MB: 0.4 s, fsync 0.2 s) |
| x45web upload, WiFi address, 15 MB | about 2.4 s |

So the disk and x45web are not the limit: the **receive side of the wired port** is. `eth0` negotiates 1000 Mbit/s full
duplex; while a PC sends to it, `/proc/net/dev` shows rx `frame` errors (0.3 % of packets: 27 per 10500) and
`TCPOFOQueue` shows about a third of the segments arriving out of order, so TCP keeps collapsing its window. Sending
from the unit is unaffected.

**The fix: hold the port at 100 Mbit (2026-10-09).** `ethtool -s eth0 speed 100 duplex full autoneg on` (the PHY then
advertises 100 Mbit only; the switch still negotiates the link itself): 15 MB uploads at 7-9 MB/s, three runs, and the
rx error counter did not move. Persistent through `etc/eth-speed` (contains `100`; empty or missing = leave the port
alone), applied by `eth_speed` in `x45stack` once the link is up (`x45stack.log`: "wired port held at 100 Mbit"). To go
back to gigabit: empty the file and `ethtool -s eth0 autoneg on advertise 0x03f`. The switch will show 100 Mbit.

The PHY is an Atheros AR8035 (ID `0x004dd072`, address 1) driven by the generic PHY driver. Debug registers as found
(read with a 40-line `SIOCGMIIREG` tool, `0x1d` address / `0x1e` data): `0x00` = `0x82ee` (RGMII receive clock delay
on), `0x05` = `0x2d47` (transmit delay on), `0x1f` = `0xb008`. At 1000 Mbit with those values: 573 receive errors in
two 15 MB uploads. **Do not poke these over the wire**: receive delay off made the port unusable for TCP, transmit delay
off made the unit stop answering ARP; and because `wlan0` shares the subnet, its replies leave through `eth0` too, so
the WiFi address was dead as well, and nothing could be reached remotely to undo it: the unit needed a power cycle (a replugged cable did not bring it back). After the reboot: 100 Mbit,
9 MB/s uploads, no receive errors. A real gigabit fix would need the board's RGMII skew set up in the kernel (not possible from userspace
without a rooted PHY fixup at boot); 100 Mbit is far more than the unit needs.

A library delete (web page or panel) also sends `lib gone <path>` at once: x45ui stops what plays from the deleted path,
drops its rows from the lists without waiting for MPD, and the folders the delete left empty are removed up to (not
including) the disk's library root, because MPD shows no folder without music and such a folder could not be seen, let
alone deleted, from the library. The library root of a disk is its "My Music" folder; a folder beside it (`HDD4/audio-test`)
is only reachable from the file manager.
