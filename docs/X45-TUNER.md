# The DAB+/FM tuner

One module does both bands. It sits on a UART, speaks a small framed binary protocol, and hands the audio to the
analogue input path (the UART carries control, text and slideshow images only). `src/x45tuner/main.c` drives it;
`src/x45ui` has the FM and DAB screens (`draw_tuner*`, `np_tuner`), the web page the same.

| | |
|---|---|
| Module | KSM-AEBH020S (presumably the stock firmware's `QKseRadioSerialPort`) |
| Port | `/dev/ttymxc1` (UART2 @ 0x021E8000), 38400 8N1 raw; other rates give nothing |
| Reset | GPIO5.15 (143), active low; `x45tuner reset` pulses it through /dev/mem |
| Version | reply `F4` carries `S1.9.4A1.5` |
| Audio | not on the UART; the radio inputs select input mode 1 (`x45ctl input fmradio`), the XMOS plays nothing |

## Wire format

```
5A A5  <len hi> <len lo>  <payload: opcode first, len bytes>  <chk = XOR of the payload bytes>
```

Every command is answered with `FF <opcode>`. Replies and unsolicited reports use the same framing.

Commands (host to module), as seen in the stock firmware's traffic:

| Opcode | Payload | Meaning |
|---|---|---|
| `24` | `01` | start of the common init |
| `1C` | `02 00 22 2E 2A 30` | FM band edges 87.50 - 108.00 MHz (10 kHz units) |
| `0C`, `01`(x9 `00`), `1D`, `1B 02`, `23` | | rest of the init, sent in one write |
| `02` | `01` / `02` | mode: 01 = FM, 02 = DAB |
| `03` | `hh ll` | FM tune, frequency in 10 kHz (8880 = 88.80 MHz) |
| `08` | `00 00` | FM seek up; the module streams `F1` reports with the running frequency until its found flag is set |
| `04` | `01 01` | DAB full scan |
| `05` | `00 00 <SId hi lo> 00 <comp> <type>` | DAB select a service |
| `0A` | | send the service list again |
| `21` | | end / commit (after a scan, to stop it) |

Reports (module to host):

| Opcode | Meaning |
|---|---|
| `F4` | version string |
| `F1 01 <freq BE> <found> ...` | FM tuning report (frequency, lock flag) |
| `F1 02 00 <type> 01 00 <label 16> <SId 2> 00 <comp>` | DAB: the service that is playing |
| `C8 <rssi> <snr> <flag>` | FM level and signal to noise, about every 2 s |
| `D2 00 <PS 8>` | RDS programme service name |
| `D3 <flag> <text up to 64>` | RDS RadioText |
| `D5 xx` | FM tuning status (stereo?) |
| `D1 <n> <band>` | DAB scan progress (about 8 per second); `band` indexes the channel table below |
| `BE 30 <idx> 00 <type> 00 00 <SId 2> 00 <comp> <label 16>` | DAB service list entry |
| `CC <ensemble label 16>` | DAB ensemble name |
| `CD <n>` | DAB sync: 0 = the decoder has lost the ensemble (the audio breaks up), 01..0F locked |
| `C5 00 <DLS text>` | DAB dynamic label (the "now playing" line) |
| `C6 <y-1900> <m> <d> <h> <mi> <s>` | ensemble time |
| `C9 <a> <b> <c> <q> 00 <d> <e> 04` | DAB reception report, every 2 s. `q` (byte 4) is the strength, 0..100: 0x60-0x64 on a strong ensemble, 0x04-0x2a on one that drops out. `a`..`e` differ per ensemble (`a` looks like the channel, `e` is 01 on weak and 02 on strong) and are not understood |
| `CB 73/74 ...` | DAB MOT slideshow segments (header, then data segments by index; the carousel repeats an object every few seconds) |
| `FE FE` | end marker (the end of a scan) |

DAB text is EBU Latin (`ebu[]` in x45tuner converts it). A weak ensemble still reports `F1` and `CC`, but `CD 00`
means the decoder has lost sync: the audio cuts out. The panel shows the `C9` strength as four bars in the top bar
and as a percentage with STRONG / GOOD / FAIR / WEAK / SYNC LOST on the DAB screen (and in the web page's tuner
readout, `tuner.strength`).

## What the stock firmware contains (`/home/root/bin/x20`)

* **Two module families.** `QAbstractFmDabSerialPort` is the base class with two implementations, `QKseRadioSerialPort`
  and `QPnpRadioSerialPort`. This unit's module answers the KSE one; the PNP protocol is not seen in any trace and
  is not implemented here.
* **Settings** (strings in x20): `SY_FM_DAB` (FM radio or DAB), `SY_FM_RADIO_FREQ` (the FM band: Europe
  and North Asia), `SY_FM_RADIO_RDS_SEEK` ("use RDS when seek all"). The lists are kept in `/etc/config/dab.json`
  (`c_strDabListPath`) and `/etc/config/fmradio.json`; ours are `etc/dab.list` and `etc/fm.list`.
* **The DAB channel table** `g_aiDabFreq`: 41 frequencies in kHz, and the index is what `dab.json` calls `FreqID` and
  what `D1` reports as the scan band:

  ```
   0 5A 174928   1 5B 176640   2 5C 178352   3 5D 180064   4 6A 181936   5 6B 183648   6 6C 185360   7 6D 187072
   8 7A 188928   9 7B 190640  10 7C 192352  11 7D 194064  12 8A 195936  13 8B 197648  14 8C 199360  15 8D 201072
  16 9A 202928  17 9B 204640  18 9C 206352  19 9D 208064  20 10A 209936 21 10B 211648 22 10C 213360 23 10D 215072
  24 10N 210096 25 11A 216928  26 11B 218640 27 11C 220352 28 11D 222064 29 11N 217088 30 12A 223936 31 12B 225648
  32 12C 227360 33 12D 229072  34 12N 224096 35 13A 230784 36 13B 232496 37 13C 234208 38 13D 235776 39 13E 237488
  40 13F 239200
  ```

  (the 38 standard Band III channels and the three N channels, in the order the stock scan walks them).
* **The programme types** `g_aszDabPty` (30 names, index 0 none: News, Current Affairs, Information, Sport, Education,
  Drama, Culture, Science, Varied, Pop Music, Rock Music, Easy Listening Music, Light Classical, Serious Classical,
  Other Music, Weather / Meteorology, Finance / Business, Children's Programmes, Social Affairs, Religion, Phone In,
  Travel, Leisure, Jazz Music, Country Music, National Music, Oldies Music, Folk Music, Documentary). The stock list
  has the typo "Financa / Businesse". The module's reports used here do not carry a PTY yet; where it does, this is
  the table.
* **Slideshow:** `QwSlideShow` is the stock's picture player (it also shows the DAB MOT images, kept under
  `/tmp/DabArt`); ours assembles the segments itself and writes `/tmp/x45-slide.img`.
* **Traces:** `/home/root/bin/Gen/*.gen` are the stock's own recorded replies (25 FM `SetFreq_<10kHz>.gen`, six
  `DabCh_2_*.gen` with slideshow images, `GetDabDb.gen`); they matched what this implementation parses.
* **No tuner firmware** is stored on the unit: the module's firmware is inside the module (`F4` reads its version).
  The PHP of the stock web interface is SourceGuardian-encoded and was not decoded (it has nothing to teach about the
  UART in any case: the protocol lives in x20).

## What works and what does not

Works: FM tune, step, seek, scan with RDS names, station list; DAB scan, service list (labels, ensemble), select,
dynamic label, ensemble time, slideshow assembly, signal quality `CD`.

Open:

* **DAB cuts out** on a weak signal (reported by the user: it plays, then drops out, and used to show only "signal /
  no signal"). That is reception; the strength is now decoded and shown. (An earlier note here said DAB had no audio at
  all. That was wrong: the analyzer read silence because its SSI capture path is dead, and it was never a statement about the DAB sound.)
* The service names do not show while a DAB scan runs (the list arrives at the end; asking with `0A` during the scan
  is an idea that has not been tried).
* The meaning of the `C9` bytes other than the strength `q` is open.
* The MOT slideshow assembly is implemented from the segment layout but has not been seen on air.
