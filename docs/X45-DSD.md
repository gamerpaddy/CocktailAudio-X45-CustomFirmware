# DSD and high-rate PCM on the replacement stack (2026-10-08)

Test material: one track as DSD64, DSD128 and DSD256 files and as a 384 kHz / 32 bit WAV, plus 45 s excerpts of each
(none of it is in the repository).

## What the stock firmware does (so what "natively" means on this unit)

Read from the stock player program (its strings and the sequence of its ALSA open call):

* **DSD64 and DSD128 go out natively.** The stock player tries the ALSA formats `DSD_U32_LE`, `DSD_U32_BE`, `DSD_U16_LE`,
  `DSD_U16_BE`, `DSD_U8`; the XMOS only offers `DSD_U32_BE` (altset 3), so it ends there with
  `snd_pcm_hw_params_set_rate(DSD bit rate / 32)`: **88 200 frames/s for DSD64, 176 400 for DSD128**. That is exactly what
  MPD does with `dop "no"`; nothing else is configured (no GPIO, no ES9018 register changes).
* **DSD256 is not native on the stock either.** Its Roon (RAAT) profile lists DSD 2 822 400 and 5 644 800
  only (`"supported_formats"`, `dsd_mode: native`), and the UI has the message *"DSD 256 does not support %1 output via
  USB. Please retry after connect USB to PC !!!"* (`TR_DSD256_Not_Supported_p1`). A native `aplay` at 352 800 frames/s fails
  at `hw_params` on this DAC, which is the same wall.
* What the stock does for DSD256 is the **DSD to PCM converter** (`dsdpcm_converter_{d,i,s}`, table FIRs of 8, 16 and 64
  to 1 followed by 2 to 1 stages; the factory at 0x43c4f8 takes a DSD rate and a PCM target of 88.2, 176.4 or 352.8 kHz).
* The stock has a setting for the signal type (`AUD_DSD_PLAY_SIGNAL_TYPE`: PCM / DSD native / DSD (DOP)). The replacement
  has no such setting yet: DSD64/128 are native, DSD256 is PCM, and a native open that fails falls back to PCM.

## What the replacement does

| File | Played as |
|---|---|
| DSD64, DSD128 | native, `DSD_U32_BE` at 88 200 / 176 400 frames/s (MPD status `audio: dsd64:2`) |
| DSD256 (and anything faster) | PCM, 32 bit, **352.8 kHz**, converted inside MPD (below). Panel text "DSD256  stereo  to PCM" |
| 384 kHz / 32 bit WAV | plain PCM `S32_LE` at 384 000; the 48 kHz clock family is selected by libx45clock before the PCM opens |
| a native DSD open that fails | retried once as PCM |

### The MPD patch (`patches/mpd-0.23.15-x45-dsd-pcm.patch`, applied by `scripts/mpd.sh`)

* `Filtered.cxx`: `X45_DSD_NATIVE_MAX = 705600` (MPD's rate for DSD128). Above it the output is opened as PCM; a failed
  native open is retried as PCM (stock MPD has no such retry).
* `Convert.cxx`, `PcmDsd.cxx`: the DSD to float conversion stays float up to the decimator. `X45_DSD_GAIN_DB` in the
  environment of MPD adds gain to DSD converted to PCM (default 0; see "level" below).
* `Dsd2Pcm.cxx/.hxx`: `TranslateFast`, the same 96 tap 8:1 filter as MPD's `dsd2pcm`, from a linear history of 11 bytes and
  two table sets (no bit reversal, no double precision); identical output after the first 11 samples.
* `FallbackResampler.cxx/.hxx`: MPD's built-in fallback resampler is nearest neighbour, which would alias everything the DSD
  noise shaping puts above 100 kHz into the audio band. It is replaced for the decimating case by a 127 tap Kaiser windowed
  sinc (beta 13) polyphase decimator with a NEON dot product.

The DSD256 chain is DSD256 (11.29 MHz) -> dsd2pcm 8:1 (1.41 MHz) -> decimator 4:1 -> **352.8 kHz**.

Measured on the host: flat to 130 kHz, at least 128 dB alias rejection, output independent of the chunk size the data
arrives in. On the unit (`nice 19`, a second MPD with a pipe output, a small test script that is not part of the repository):
the capture matches the host reference to **-132 dB** (float rounding), and MPD's output thread needs **0.62 cpu-seconds per
audio-second** (dsd2pcm 0.27 + decimator 0.31 in `armbench.sh`/`dsdbench`), against more than 1.1 for the first version.
That is about 60 % of one of the two cores. Real-time playback on the DAC is still to be confirmed (below).

### Level

dsd2pcm gives +-1.0 for a stream of all ones, i.e. 100 % modulation, whereas full scale DSD is 50 % modulation: PCM made from
DSD peaks 6 dB below PCM full scale. How loud the ES9018 plays *native* DSD relative to PCM is not measured (it needs a
level-meter on the output, or at least the same track in both forms). Until that is known the gain is 0 dB; if native DSD64
turns out 6 dB louder, set `X45_DSD_GAIN_DB=6` in `deploy/x45stack` for the converted case.

## Test method that does not touch the DAC

A second MPD (`/var/volatile/dsdtest/mpd.dsd`, port 6601) plays the file into a *pipe* output (`head -c N > out.raw`); the main
MPD and the DAC are untouched. A fifo output loses data (MPD drops on EAGAIN), so use the pipe. Compare with
a host conversion of the same file, using a small analysis script that finds the lag by
cross-correlation (neither script is part of the repository). `/var/volatile` is RAM: clean `dsdtest/` up when done. Do not use `pkill -f` with a pattern that is in
your own ssh command line.

## Not yet done

* Real DAC runs of DSD64, DSD128 (native), DSD256 (PCM, real time CPU margin with the UI and the analyzer running) and the
  384 kHz WAV: they need the DAC, i.e. the main MPD stopped, and a new MPD process (the MPD that `scripts/mpd.sh` builds
  contains the patch).
* Native versus PCM level (above); left/right of native DSD is assumed correct (the stock has a `dsd_native_lr_reversed`
  key for other DACs and does not set it here).
