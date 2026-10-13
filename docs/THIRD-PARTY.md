# Third party software

None of the code below is stored in this repository. `scripts/fetch.py` downloads it from the original source
when you run `./setup.sh`, and checks every file against the SHA256 sum in `scripts/sources.txt`. If a sum does
not match, the build stops. The exact versions are the ones listed there.

The files in `patches/` are small changes to some of these programs for this unit (clock handling, a seek fix,
DSD conversion, bounds checks). Patched code stays under the terms of the original.

Nothing from the original firmware of the unit is used or shipped. The build does not need it.

## Used to build (not part of the package)

| Name | Version | Used for | Terms |
|---|---|---|---|
| Bootlin armv7-eabihf musl toolchain | stable-2021.11-1 | the ARM cross compiler (GCC 10.3, binutils, musl C library, kernel headers) | GPL / LGPL for the tools, MIT for musl |
| Meson | 1.4.1 | build system of MPD | Apache 2.0 |
| Ninja | 1.12.1 | build tool | Apache 2.0 |
| pkgconf | 2.2.0 | finds libraries | ISC |
| m4, autoconf, automake, libtool | 1.4.19, 2.71, 1.16.5, 2.4.7 | build libraries that use autotools | GPL |
| genromfs, GCC, make, Python 3, Perl | from your distribution | build and pack | various |
| Rust | 1.99.0 (+ the armv7 musl standard library) | builds librespot (only if Spotify is built) | MIT or Apache 2.0 |

## Linked into the programs on the unit

Everything is linked statically, so the package does not depend on the libraries of the original firmware.

| Name | Version | Used for | Terms |
|---|---|---|---|
| MPD | 0.23.15 | the music player daemon | GPL 2 or later |
| Boost, fmt | 1.83.0, 10.1.1 | needed by MPD | Boost licence, MIT |
| alsa-lib | 1.2.11 | sound output | LGPL 2.1 |
| libogg, libvorbis | 1.3.5, 1.3.7 | Ogg Vorbis | BSD 3 clause |
| FLAC | 1.4.3 | FLAC decoding and encoding | BSD 3 clause (libraries) |
| Opus, opusfile | 1.4, 0.12 | Opus | BSD 3 clause |
| libsndfile | 1.2.2 | other audio formats | LGPL 2.1 or later |
| libmad, libid3tag | 0.15.1b | MP3 decoding and tags | GPL 2 or later |
| LAME | 3.100 | MP3 encoding when ripping CDs | LGPL 2 or later |
| zlib | 1.3.1 | compression | zlib licence |
| Mbed TLS | 3.6.2 | TLS for web radio and curl | Apache 2.0 or GPL 2 or later |
| curl | 8.10.1 | HTTP streams | curl licence (MIT style) |
| CA certificate bundle | from curl.se | trust list for HTTPS | MPL 2.0 |
| expat | 2.6.3 | XML | MIT |
| stb_image, stb_truetype | pinned commits | pictures and fonts in the panel | MIT (the files also offer a second, unrestricted option) |
| Liberation Sans | 2.1.5 | font of the panel | SIL Open Font License 1.1 |
| liba52 (a52dec) | 0.7.4 | Dolby Digital audio of video DVDs | GPL 2 or later |
| HDCD decoder | n/a | CD player, see below | BSD 3 clause |

Optional parts (leave them out with `X45_SKIP`):

| Part | Name | Version | Terms |
|---|---|---|---|
| AirPlay | shairport-sync | 4.3.4 | MIT (with additional notes of the project) |
| AirPlay | libconfig, popt | 1.7.3, 1.19 | LGPL 2.1, MIT |
| AirPlay | ALAC decoder | commit 5d8c5db | Apache 2.0 |
| UPnP | upmpdcli | 1.8.18 | GPL 2 or later |
| UPnP | libupnpp, libnpupnp | 0.26.8, 6.1.3 | LGPL 2.1 or later |
| UPnP | libmicrohttpd, jsoncpp, libmpdclient | 1.0.1, 1.9.5, 2.22 | LGPL 2.1 or later, MIT, BSD 3 clause |
| Spotify Connect | librespot and its Rust crates | 0.8.0 | MIT, plus the terms of each crate (mostly MIT or Apache 2.0) |
| Wi-Fi | libnl, iw, wpa_supplicant | 3.9.0, 6.9, 2.11 | LGPL 2.1, ISC, BSD 3 clause |
| Bluetooth | BTstack | commit e385539 | BlueKitchen licence, see below |
| Bluetooth | libusb | 1.0.27 | LGPL 2.1 or later |
| Bluetooth | AIC8800D80 firmware files | radxa-pkg/aic8800 | the vendor's redistribution terms |

## Things to know

**HDCD decoder.** `src/x45cd/hdcd.c` is a port of the `af_hdcd` filter of FFmpeg to plain C. The tables and the
decoding logic come from there. The original notice follows, as its terms require.

```
Copyright (C) 2010, Chris Moeller,
All rights reserved.
Optimizations by Gumboot
Additional work by Burt P.
Original code reverse engineered from HDCD decoder library by Christopher Key,
which was likely reverse engineered from Windows Media Player.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:
  1. Redistributions of source code must retain the above copyright
     notice, this list of conditions and the following disclaimer.
  2. Redistributions in binary form must reproduce the above copyright
     notice, this list of conditions and the following disclaimer in the
     documentation and/or other materials provided with the distribution.
  3. The names of its contributors may not be used to endorse or promote
     products derived from this software without specific prior written
     permission.
THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

**Dolby Digital.** liba52 (copyright Michel Lespinasse and Aaron Holtzman) is released under the GNU General
Public License, version 2 or later. It is downloaded, patched (two shifts by 32 bits in `bitstream.c` and
`bitstream.h`) and linked statically into `x45cd`. If you give a built package to other people, you have to offer
them the source of the GPL parts in it, which for this library is the archive that `fetch.py` downloads plus
`patches/a52dec-0.7.4-x45.patch`.

**Bluetooth.** BTstack is free for non-commercial use only. Selling a device or package that contains it needs a
commercial licence from BlueKitchen. The headless Bluetooth sink `x45bt` is built from BTstack's
`a2dp_sink_demo.c` with `patches/btstack-e385539-x45bt.c.patch` applied (see `scripts/bt.sh`), so it is a changed
copy of BTstack code. Bluetooth can be left out of the build with `X45_SKIP=bluetooth`.

`src/tools/aicload.c` follows the firmware download sequence of the Linux driver for the AIC8800 chips, which is
public. It contains no code of the driver beyond the protocol constants. The firmware blobs themselves are never in
this repository; `fetch.py` downloads them from a public repository and `stage.sh` puts them next to the loader.

**Streaming services.** The package contains the open receivers for AirPlay, UPnP and Spotify Connect. No key,
token or SDK of any streaming service is included or needed, and none is used to get around a service's rules.

**Trademarks.** Names such as Dolby, HDCD, AirPlay, Spotify, Bluetooth and Wi-Fi belong to their owners and are
used only to say what a feature reads or talks to. This project is not affiliated with the maker of the unit and
is not endorsed by anyone named here.

If you hand a built package to other people, you also take on the terms of the parts inside it. They are
free software for the most part, with the exceptions named above.
