# X45 control API

Base URL `http://<unit>/api/v1/...` (`/api/...` is the same thing). Cross-site use from a web page is off: no CORS
headers, a foreign `Host` and a POST/PUT with a foreign `Origin` answer 403 (curl and apps send no `Origin` and are not
affected); `web_cross_site = on` in `etc/x45ui.conf` brings the open CORS back. A request has 20 s to arrive in full
(slow senders are dropped), an address is at most 2047 bytes (414), `POST /cmd` at most 4094 bytes of body (413),
`/stream.mjpg` at most 4 viewers at once.
Optional token: put `web_token = SECRET` in `etc/x45ui.conf`; every POST then needs
`Authorization: Bearer <token>` (or `X-Token: <token>`). GETs stay open.

## Read
| Request | Returns |
|---|---|
| `GET /state` | everything the UI knows: input, volume (`volume_hidden` when fixed), `balance` (-24..24, `balance_max` 24), player, CD (`cd.hdcd` 0/1/2, `cd.hdcd_pe`, `cd.hdcd_gain` in 0.5 dB: see X45-HDCD.md), tuner + lists, stations, library listing, options (outputs, per-input `trim` in dB, `rip_format`), `hw` (headphone, PC USB cable/routing, S/PDIF rate), `rec`, `meter` info |
| `GET /system` | uptime, load, memory, temperatures, network, disks (free space, rw), service status, USB devices |
| `GET /meter` | live analyzer `{"seq","rate","l":[rms,peak],"r":[rms,peak],"b":[128 bands dBFS, 20 Hz-20 kHz log-spaced]}` or `{"ok":false}`. Calling it switches the analyzer on (stays on ~6 s after the last call). Available on line in/aux/phono/AES/coax/optical, FM/DAB, and network playback, PC USB and CD; AirPlay/Spotify unverified |
| `GET /screen.bmp` | what the panel shows |
| `GET /slide` | current DAB slideshow picture |
| `GET /art` | the picture of what plays (a cover or a station logo, a BMP of at most 300 px), 404 when there is none; `/state` carries `art`, a counter that changes with the picture (0 when there is none) |

## Write
`POST /cmd` with one or more command lines in the body:

```
key menu|return|input|ok|stop|power|mute
key menuhold|stophold      what holding MENU (options) / STOP (delete question, or end a recording) does
ir up|down|left|right|enter|esc|home|next|prev|ffwd|rew|shuffle|rec|menu|setup|info|mute|input|radio|playlist|
   play|stop|volup|voldown|nowplaying|favourite|eject|iservice|search|backspace|capnum|balance|0..9   what the remote button of that name does (rec asks first)
knob vol|scroll <ticks>
screen <0 player|1 library|2 radio|3 cd|4 tuner|5 status|6 input|7 options>
sel <n>                    select a row on the current screen
vol <0-100>                input <0-11>      bl <brightness>
balance <-24..24>          the balance, negative = the left side louder, 0 = centre (kept; works with the volume fixed too)
ext <name>                 open a screen of src/x45ui/ext by name (wifi, search, balance)
trim <input 6|7|8> <0-21>  per-input trim, 12 = 0 dB
out lineout|xlr <0|1>      opt volume fixed|variable
mpd play|pause|stop|next|previous|random 0|1|repeat 0|1|single 0|1|consume 0|1|clear|update
mpd seekcur <s>            mpd play <pos>    mpd delete <pos>
lib add|play <path>        lib update [path]          lib up (one folder up in the browser, whatever screen the panel shows)
lib gone <path>            the file manager / library delete removed this library path: stops what plays from it, drops the rows now
tuner tune <10 kHz units>  tuner step <+-units>  tuner seek up|down  tuner fmscan
tuner fmadd                tuner fmdel <10 kHz units>  tuner scan (DAB)  tuner select <n>  tuner cancel (stop a scan)
rec toggle                 cd eject
ripfmt flac|mp3|mp3v0     the file format of CD rips (kept); /state: options.rip_format, options.rip_formats [{name,label}]
artwork 0|1|2             where the picture of what plays may come from: 0 off, 1 the library only, 2 also the internet (kept); /state: options.artwork, options.artwork_modes
bt pair|forget|yes|no      Bluetooth input only: open for pairing, forget all devices, accept/refuse a pairing request
btout on|off|scan|stopscan|connect ADDR|disconnect|forget ADDR   Bluetooth output: MPD's sound to a headset (X45-BLUETOOTH.md); /state: btout {on, active, state starting|ready|scanning|connecting|connected|streaming, name, target, auto, connected, streaming, scanning, routed, error, devices [{addr,name,known,connected,rssi,heard}]}
```

Input numbers: 0 network, 1 fm, 2 pc usb, 3 aes ebu, 4 coaxial, 5 optical, 6 line in, 7 aux in, 8 phono, 9 cd, 10 dab, 11 bluetooth.
`/state` names the current input in `input_name` with exactly these words (`pc usb`, `aes ebu`, `line in` ...; also the
entries of `inputs`, in number order). Choosing bluetooth switches the radio stick on (about 30 s after a power-up); leaving the input switches it off. `/state` then carries a `bt` object: `active`, `state` (starting|ready|pairing|connected|playing|error), `name`, `confirm` (pairing code while a phone asks), `bonds`, `title`/`artist`/`album`, `rate`, `error`.

`POST /library/delete` body = library path (e.g. `HDD4/Recordings/Album`). Deletes the file or
folder from the disk, never a storage root or a whole disk. An attached disk is normally mounted
read-only (the media helper keeps it so); it is switched to read-write for the deletion and back,
the same as when the panel deletes, and under the same write lock as the file manager (503 "busy" while a
file-manager change or another delete is running). Only the folder that held the item is rescanned. A path may
contain `...` (`Wait...`); only a `..` component is refused. A body that is shorter than its Content-Length is
answered 400 "incomplete request" (commands, favourites, delete and the WiFi calls; file uploads are streamed and
have their own rules, see X45-FILES.md).

`lib update` (the web page's rescan button, and the file manager after it changed something: see X45-FILES.md)
makes the panel's list and the page's library listing load again when MPD reports the update finished (and once
more 2.5 s after the command, for an update that was over before it was seen). A disk that holds no music yet is
listed too: MPD only knows folders with music in them, so the empty ones come from the `media/` links.

Anything that starts playback (`mpd play`, `pause 0`, the OK key, IR play, `lib play`) while another input is selected
switches to the network input first and waits for the DAC to come back; it never writes to a DAC that is on the
computer's USB port. While PC USB is the input x45ui keeps the DAC's clock family (44.1k / 48k) in step with the
computer's stream by reading the driver's `SR=[...]` log lines (`src/libx45/x45clock.c`).

`/state` also carries `dir_label`, where the library browser is as the panel writes it
(`/radio/favourites`, `/network/Music`, `/HDD4/Recordings`); `dir` is the internal name (`@fav`, `@net`,
`@u|<control url>|<id>`, a path). A body larger than 64 KB is refused with 413 (it used to be cut at 12 KB,
which let the favourites editor save a shortened list).

Settings (volume, volume mode, balance, input, outputs, per-input trim, brightness) persist across restarts. The web page's Options tab has a
balance slider (and a centre button) next to the volume mode, and the Status tab shows it.

## Look (added 2026-10-08)

```
skin <name>                the layout: classic carousel tiles ledger matrix split poster (see X45-SKINS.md)
theme <n>                  the accent colour, by index in look.themes
ground <n>                 the background (0 = the skin's own), by index in look.grounds
ink <n>                    the text colour (0 = the skin's own), by index in look.inks
vis <name>|none            switch a visual on or off (bars dials gonio scope spectrum waterfall loudness reccheck tonal chroma width;
                           led, mirror and area still
                           work: the spectrum in that style);
                           at most two are on, a third turns the oldest off; `none` clears them
specstyle <n>             the spectrum's style (bars, LED cells, mirrored, curve), by index in look.styles
bars <n>                   the colour scheme of the bars, by index in look.bar_names
gonlook <n>                the stereo field's look, by index in look.gon.looks
goncol <n>                 the colour of its trace, by index in look.gon.colours
gonbars <0|1>              its correlation and balance bars: 1 shown, 0 left out (1 also ends gonbig)
gonbig <0|1>               the stereo field almost fullscreen with a small text column beside it: 1 on (also leaves the
                           bars out), 0 off; look.gon.big in /state
```

All of them are kept across restarts. `/state` carries them as the `look` object: `skin`, `skins` (`name`, `label`,
`about`), `theme` / `ground` / `ink` (the chosen index), `themes` / `grounds` / `inks` (`name` and `color`, `""` for
"the skin's own"), `vis` (`name`, `label`, `on`), `max_vis`, `style` and `styles` (the spectrum's), `looks` (a bit mask of the look options that the visuals which are on use:
2 the bar colours, 4 the stereo field's, 8 the spectrum style; the Look tab hides the others), `bars` and `bar_names`. The web page's Look tab is
built from it, so a skin, colour or visual added in the panel's source appears there by itself.

`GET /meter` also carries `c` (the correlation of the channels since the last file, -1 opposite .. 1 the same) and
`wl2` / `wr2` (200 samples each of the last 25 ms of the wave, for the oscilloscope and the stereo field) and, since 2026-10-10,
`sb` (128 bands of the side signal (L - R) / 2, dBFS, over the same frequencies as `b`, which is the mid signal (L + R) / 2: the stereo width
visual; the FFT is shared, so it costs nothing extra). `rate` is the sample rate the analyzer works at.

The `tuner` object of `/state` has `strength` (DAB reception 0..100, from the module's `C9` report; 0 until the first
report) beside `quality` (the module's `CD` sync flag: 0 = the decoder has lost the ensemble, the audio breaks up).

## Favourites (added 2026-10-07)

`GET /api/v1/favourites` returns the owner's own stations, one per line, `name<TAB>url`. `POST /api/v1/favourites`
with the whole list in the same format replaces them (entries without an http:// or https:// address are dropped;
the header comments and the block the FRITZ!Box sync manages are kept). The panel shows the same list under
Library > Radio > Favourites. `radio play <name> | <url>` starts a stream.

## Live panel view (added 2026-10-07)

`GET /api/v1/stream.mjpg` is the panel as a motion JPEG stream (`multipart/x-mixed-replace`, boundary `x45frame`):
put it in an `<img>` tag. Half size (512x300) by default, `?full=1` gives 1024x600. About 3 frames a second, only
when the screen changed (a frame at least every second). The encoder is `src/x45web/jpegenc.h` (baseline JPEG, no
library). `/api/v1/screen.bmp` is still there for a single picture. Each open stream is a process of its own and
uses about half a core while it runs.

## WiFi (added 2026-10-08)

The same manager as the panel's Network screen and the `x45wifi` command, so all three see the same saved networks.
A saved network's key is never sent out. POST bodies are `key=value` lines. Answers to POSTs are
`{"ok":true,...}` or `{"ok":false,"error":"why"}`.

```
GET  /api/v1/wifi                 state (off|starting|error|idle|scanning|connecting|connected), the joined network, saved networks
GET  /api/v1/wifi/scan            what the last scan heard
POST /api/v1/wifi/scan            start a new scan
POST /api/v1/wifi/connect         ssid=NAME  psk=KEY (empty: open)  hidden=0|1   -- waits up to 20 s for the outcome
POST /api/v1/wifi/join            id=N      a saved network, now
POST /api/v1/wifi/forget          id=N
POST /api/v1/wifi/auto            id=N  on=0|1
POST /api/v1/wifi/disconnect      POST /api/v1/wifi/reconnect
POST /api/v1/wifi/power           on=0|1    (switching on takes up to half a minute: poll GET /wifi)
```

`connect` answers `{"ok":true,"result":"connected"|"pending"|"not_in_range"}`; a key the router refuses is a 400 with
`"result":"failed"` and the network is not kept (unless it was saved before). 400 also for a name that is missing or
too long and for a key that cannot be right (8 to 63 characters). 503 `WiFi is off` when there is no supplicant to
talk to (switch it on with `power`); `join`/`forget`/`auto` with an `id` that is missing or not a number are 400.

`GET /wifi` also says `"via_wifi":true` when the request came in over the unit's own WiFi address. Such a client cannot
change the link it is on: `connect`, `join`, `forget`, `disconnect`, `reconnect` and `power on=0` answer 409 (scan, the
`auto` flag and switching WiFi on stay allowed). The web page's WiFi tab shows a note instead, and does everything when it
is opened over the cable. UPnP: a control point's Stop leaves a song alone that the panel or the web page queued after it
(patched upmpdcli, `patches/upmpdcli-stop-own.patch`).

## Clock, sleep timer, song log, signal path (added 2026-10-15)
`GET /state` carries a key for each screen of `src/x45ui/ext` that has state:

| Key | Content |
|---|---|
| `clock` | `{"valid","epoch","offset","zone","action":"standby\|off","bar","idle","h24","secs","sleep":{"minutes","running","fading","left","asleep"},"presets":[..],"idles":[..]}`; `valid` is 0 while the system clock was never set (a year before 2025) |
| `songlog` | `{"on","count","marked","last"}` (the lines themselves: `GET /songlog`) |
| `childlock` | `{"locked":0\|1,"volume":0\|1}` |
| `rec` | now also `bad` (sectors a rip could not read), `dir` (folder of the rip), `msg` |

Commands (`POST /cmd`, one line each, `<screen> <args>`):

```
clock sleep N|off          sleep timer in minutes (off cancels); the volume fades over its last 30 s
clock action standby|off   what the end of the timer does: stop and sleep (backlight down), or shut the unit down
clock bar 0|1              show the remaining time in the top bar
clock idle SECONDS         show the clock after this long idle (0 = never)
clock h24 0|1   clock secs 0|1
clock open | dark          open the clock screen / put the backlight down
clock set EPOCH            set the system clock
songlog mark ID | unmark ID | now | clear | log 0|1 | open
childlock 1|0              lock / unlock the panel and the remote (web page and apps are never locked out)
childlock volume 0|1       keep volume knob / remote volume keys working while locked
childlock probe KIND PRESSED   test aid: runs a hardware event through the filter (okkey okir power knob vol volir web);
                           the answer lands in /tmp/x45childlock.probe
```

| Request | Returns |
|---|---|
| `GET /songlog` | `{"entries":[{"id","t","mark","src":"net\|dab\|fm","station","text"}]}`, newest first; `t` is 0 when the clock was not set. Nothing is looked up anywhere |
| `GET /cd/meta` | the disc in the drive: id, tracks, `cdtext` (what the disc carries) and `user` (what was typed here) |
| `POST /cd/meta` | optional first line `id <disc id>`, then `K <track> <text>` lines (K = T P S C A M G U; track 0 = the disc); an empty body removes the user tags |
| `GET /cd/riplog` | `rip log.txt` of the last rip's folder |

## Health, logs, access (added 2026-10-15; see X45-RELIABILITY.md)
| Request | Returns |
|---|---|
| `GET /health` | `{"up","load","mem","temps":[{"zone","c"}],"disks":[{"mount","fs","total_mb","free_mb"}],"net","procs","wd":{..}\|null,"stack":{"bootcount","enabled","childlock","watchdog_off"},"crashes":[[time,who,what]],"boots":[[time,who,what]]}`, histories newest first |
| `GET /logs` | the logs there are, with sizes and which old runs (`.1`, `.2`) exist |
| `GET /logs/NAME[.1\|.2][/N]` | the last N lines of a log; NAME is one of stack, ui, web, wd, crashes, boots, mpd, upnp, airplay, spotify, or `kernel` (dmesg); any other name is 404 |
| `GET /access` | `{"protected":true\|false}` |
| `POST /access` | body = the new token (4..64 of `A-Za-z0-9._~-`), or empty to remove it; needs the old token as `Bearer` when one is set; 400 for an invalid token |

With a token set, `/logs*` needs it too (`Bearer` or `?tok=`); the other GETs stay open (a read-only guest view).
