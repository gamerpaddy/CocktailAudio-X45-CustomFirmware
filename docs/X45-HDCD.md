# HDCD decoding in the CD player

Added 2026-10-09. `x45cd` decodes HDCD (High Definition Compatible Digital) discs. A plain disc plays as it did before.

## What HDCD is, in the part that matters here

An HDCD disc is an ordinary 16-bit CD. The encoder hides a code stream in the least significant bit of every sample, in each channel. A decoder that knows about it finds control packets in that stream and then does up to three things:

* **peak extend**: samples above about -3 dBFS (16-bit value 0x5981) were compressed by the encoder; the decoder stretches them back through a fixed table. The stretched peaks reach twice the plain full scale (+6 dB),
* **gain**: the encoder boosted quiet material by up to 7.5 dB (to use more of the 16 bits); the decoder takes it back, in 0.5 dB steps. The change is fast when the gain rises and slow when it falls,
* **transient filter**: a flag that is read and counted, the decoder does nothing with it (as in the reference).

A packet is valid when a 32-bit window of the LSB stream, scrambled with `w ^ w>>5 ^ w>>23`, matches `0x7e0fa005` (type A, 8-bit code) or `0x7e0fa006` (type B, 8-bit code and its complement as a check). The code stays in force for 2 s after its packet (the "code detect timer"); when no new packet comes it resets to "nothing".

A disc without HDCD has random LSBs. The window almost never matches (2^-32 per position), so nothing happens and the samples are not changed.

## Where the code comes from

`src/x45cd/hdcd.c` is a port of FFmpeg's `libavfilter/af_hdcd.c` (stereo, 16-bit, no analyze mode, no libav). The three tables (`peaktab`, `readaheadtab`, `gaintab`) are copied from that file by script, not retyped; the control logic follows it function by function. The file keeps the original copyright notice and licence (BSD, Chris Moeller 2010; see [THIRD-PARTY.md](THIRD-PARTY.md)).

## Output and level

The DAC is opened as `S32_LE` for every CD (it supports it: altset 1). A decoded sample is `sample << 15` (the decoder's own scale, one bit of headroom for the stretched peaks).

* **No HDCD**: output = decoded x 2 = `sample << 16`, bit for bit the data on the disc.
* **HDCD in effect** (peak extend or gain signalled): output = decoded x 1. The whole disc is then 6 dB lower than a plain one. That is the decoder's own scale: with peak extend the stretched peaks reach twice the plain full scale and have to fit, so nothing clips. (Same behaviour as every software HDCD decoder; hardware HDCD players give the headroom with an analogue stage.)
* The first track is scanned for 3 s (stops at the first effective code) *before* it plays, so the level is right from the first sample. If the codes only show up later, the level glides down by 6 dB over 3 s. Later tracks of the same disc start at the lower level at once.
* Codes that are present but change nothing (all packets say "no gain, no peak extend") count as HDCD without effect: the level stays plain.

`X45CD_HDCD=off` in the environment of `x45cd` turns the decoder off (exact data, plain level). `X45CD_HDCD=loud` decodes but keeps the plain level: quieter material is then 6 dB louder than the HDCD-correct level, and stretched peaks above full scale clip.

## Status and display

`/tmp/x45cd.status` now has four more numbers: `<hdcd> <peak_extend> <max_gain> <active>`.

| field | values |
|---|---|
| hdcd | 0 no code seen, 1 codes seen that change nothing, 2 HDCD in effect (sticky for the disc once seen) |
| peak_extend | 0 never, 1 in some packets, 2 in all packets |
| max_gain | the largest attenuation signalled, in 0.5 dB steps (8 = 4.0 dB) |
| active | 1 while codes are arriving in both channels |

The panel shows it on the CD now-playing screen as `HDCD   peak extend   -4.0 dB` (classic skin: under the progress bar; the other skins: in the artist/sub line, labelled DECODER in the ledger skin), and `GET /state` has `cd.hdcd`, `cd.hdcd_pe`, `cd.hdcd_gain`.

`x45cd` logs to stderr when HDCD takes effect and, at the end of each track that had codes, one line: `track N: hdcd 2, P packets, peak extend 2, gain 0.0 dB, E errors`.

## Looking at a disc without playing

```
x45cd hdcd <track> [seconds] [dev]
```

reads the start of a track (10 s by default) through the decoder and prints what it found: detection, packets, when the first one came, peak extend, largest gain, transient filter, errors. Use it while nothing else is reading the drive.

## Tests

`src/tests/hdcd_test.c` (`make nativetest`) builds HDCD streams itself, with an encoder that runs the scrambler forwards, and checks:

* plain audio with random LSBs: no packet, output exactly `sample << 15`,
* a type A packet with peak extend and a 4 dB gain code in both channels: found, counted, gain applied (ratio to 10^(-4/20) within 1 %),
* peak-extended samples exceed the plain range and stay inside int32,
* the control resets after the 2 s timer and the expiry is counted,
* the output is identical for chunks of 7644, 997, 588 and 1 frames and for one big block.

There is no reference decoder on the build machine, so no bit comparison with FFmpeg; the port was checked against the algorithm instead.

## Checked on a real disc (2026-10-09)

The 24-track disc the user inserted is HDCD-encoded: `x45cd hdcd` finds about 10 valid packets per second in each channel on every track (all 24 scanned for 90 s, five of them complete), with no errors except 2 in one track of 4168. **All codes are "do nothing"**: peak extend never on, gain 0.0 dB, transient filter off. So that disc decodes as HDCD without effect: the output is bit-exact plain, the level does not drop, and the panel shows `HDCD   no effect`. The peak extend and gain paths are therefore only covered by the synthetic test, not by a real disc yet.

## Not done

* Type B packets are decoded but only type A is exercised by the test.
* 20/24-bit HDCD (data from other sources) and sampling rates other than 44.1 kHz are not handled: the CD player is 16-bit/44.1 kHz.
* Switching HDCD off from the panel: only through the environment variable for now.
