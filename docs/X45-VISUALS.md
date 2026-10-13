# Visuals (x45ui)

The level bars, the analog VU dials, the oscilloscope, the stereo field, the spectrum in four forms, the waterfall, the loudness history,
the record check, the tonal balance, the key and chord and the stereo width
are **visuals**: one file each in `src/x45ui/vis/`, listed in a registry. The owner switches them on and off per
input (or for all inputs) in the RETURN menu of the player > *Visualizers*, and what is on shares the box a screen
gives them. **At most two are on at once** (`VIS_MAX_ON`): switching a third on turns the one that was switched on
first off, and the panel says so.

| Visual | File | Bit | Notes |
|---|---|---|---|
| Level bars | `vis/bars.c` | 1 | RMS and held peak on a dB scale; also runs on the panel meter of the external inputs |
| Spectrum | `vis/spectrum.c` | 2 | the analyzer's bands in one of four styles (Options > Visualizers > *Spectrum style*, `spec_style`): bars with a falling peak line and the frequencies underneath (`spectrum.c`), LED cells (`led.c`), mirrored (`mirror.c`), curve (`area.c`); band count and bar colours are Options too |
| Analog VU | `vis/dials.c` | 4 | two dials, quick rise / slow fall needle, held peak mark; the scale is drawn once |
| Waterfall | `vis/waterfall.c` | 8 | spectrum over time, newest row on top; history is kept while it is switched off |
| Oscilloscope | `vis/scope.c` | 16 | the last 25 ms of both channels over a graticule, cut at a rising zero crossing so that a tone stands still; the gain follows the level (shown in the corner); needs the analyzer's wave |
| Stereo field | `vis/gonio.c` | 32 | a vectorscope turned 45 degrees counter-clockwise (left along the horizontal axis towards the left, right along the vertical towards the top): mono is the diagonal to the upper left, out-of-phase the one to the upper right, wide stereo a cloud; as large as the box it gets (up to 440 px square); beside it the correlation of the channels and their balance; needs the wave; with the bars left out and *Stereo field size* set to almost fullscreen (`gon_big`) it takes the page: a square of the whole height under the bar at the right edge (up to 544 px, `vis/scopepage.c`), the text a small column at the left, the other visuals hidden |
| Loudness history | `vis/loudness.c` | 512 | short-term loudness (LUFS, 3 s) as a column graph over the last four minutes, marks at -14 / -23 / -30 (streaming, EBU R128, a quiet master), the momentary value as a tick at the right edge; beside it SHORT-TERM, MOMENTARY, INTEGRATED (gated), RANGE (LU) and the highest peak. **An estimate** (about 1-2 LU): the K-weighting is taken from the band spectrum, not filtered from the samples |
| Record check | `vis/reccheck.c` | 1024 | eight lamps (green / amber / red) for judging a recording or a transfer: PEAK (headroom), CLIPS (stretches at full scale), DYNAMICS (crest over the loudest fifth of the last minute), NOISE FLOOR (the quietest second, once there was one), L / R LEVEL, PHASE (mean correlation of the seconds), SUBSONIC (20-32 Hz against 100-250 Hz) and MAINS HUM (50/60 Hz, in a quiet passage only). Counts from the start of the playback; a lamp that cannot know yet reads "--" |
| Tonal balance | `vis/tonal.c` | 2048 | the long-term spectrum (about 10 s of music) in eight regions, sub to air, as bars above / below a guide that falls 4.5 dB per octave, lined up to 150 Hz - 5 kHz; a verdict in the corner (bright, dark, "bass +4 dB", balanced). The treble bands hold more bins than the bass ones, which is corrected for |
| Key and chord | `vis/chroma.c` | 4096 | the 12 pitch classes (E3 - G#6, from the band that holds each note's frequency) on the circle of fifths, C at the top; the key (Krumhansl-Kessler profiles, 24 keys, from the last ~6 s, needs 3 s of music) with its fit, the relative key, and the chord (triads, last ~0.4 s) beside it; in a low box 12 columns and the words. Poor below about 200 Hz at 44.1 kHz |
| Stereo width | `vis/width.c` | 8192 | mid (what both channels share) rising, side (what separates them) hanging down, on the spectrum's frequencies and dB scale, with a dotted 120 Hz mark; above it the share of the side signal in bass / mids / highs (the bass goes green up to 10%, amber to 25%, red above). Needs `sb` of the analyzer (x45loop of 2026-10-10 on) |

No bit is free after 8192: the mask is 16 bits (`on | first << 16` in a 32-bit word), and 64, 128 and 256 are retired. A further visual would
need the saved selection widened first.

Four of them keep a history, which lives in `struct ui` (`u->vm`, `struct vis_mem` in `vis/mem.h`), not in a static of the visual: the
menu's pictures draw into a copy of the ui, and the real history must not be touched by them. `VIS_MEM_STEP(u, m, dt)` gives the
milliseconds since the visual's last frame, and -1 (the struct zeroed) after an input change or a gap of more than 3 s, so every
measure starts again with a new playback. The clock is `u->sp_read_t`. Fields of `struct ui_vis` they use: `analyzer` (needs the analyzer's
data, but not the *Spectrum* band-count row), `side` (needs `sb`), `demo(u)` (fills the history for the picture in the menu).

The LED, mirror and curve spectra were visuals of their own until 2026-10-10 (bits 64, 128, 256). They are now styles of the spectrum: a
saved selection that has one of them is read as "spectrum on" and the style it had becomes `spec_style` (once, if there is
none yet); `vis led|mirror|area` still work as commands (spectrum, in that style). Those three bits stay taken.

The bit is what is saved in `vis_<input>` / `vis_all_set` of `etc/x45ui.conf` (the selection is `on | first << 16`,
`first` being the one switched on first): it must never change or be reused.

## How it is built

```
src/x45ui/vis/
  vis.h       the contract: struct ui_vis and what visuals share (levels, clock, dB position)
  vis.c       the registry (one line per visual), the layout of what is on, draw_visuals_rect()
  bars.c      one file per visual
  dials.c
  spectrum.c
  waterfall.c
```

A visual is a `const struct ui_vis`:

* `name`, `label` (what the menu says), `bit`, `weight` (its share of the height) and `cap(w)` (the tallest it
  needs in a box that wide; without it the visual takes whatever it gets),
* `bands`: needs the analyzer's bands (only shown when there is a stream to analyse); `analyzer` the same without the band-count option; `side` also the side bands (`sb`),
* `look`: the options of its own in Options > Visualizers (`VIS_LOOK_COLOURS` the bar colours, `VIS_LOOK_STYLE` the spectrum style, `VIS_LOOK_FIELD` the
  stereo field's four); `bands` also brings the *Spectrum* (number of bands) row. That list, and the web's Look tab,
  show only the options of the visuals that are switched on (`vis_looks(u)`); the stereo field's colour "by distance"
  also brings the bar colours in,
* `draw(u, x, y, w, h)`: draws into the box,
* optional `feed(u)`: once per analyzer frame, shown or not (the waterfall keeps its history this way),
* optional `frame(u, now, on)`: once per drawn frame before drawing (the dials move the needles here),
* optional `two_pass`: a part that never changes. `draw()` is first called with `u->dial_pass == 1` (only that
  part, kept as a background), then every frame with `2` (only what moves). A visual that skips the
  `dial_pass` tests is simply redrawn completely.

`main.c` and the skins call `draw_visuals_rect(u, x, y, w, h)` and never know which visuals exist: that
function keeps what was behind the box so that a frame in which only the visuals moved costs a few rows, and
lays out what is on. The Options row, the menu, the saved masks and the layout all read the registry.

## Adding a visual

1. `src/x45ui/vis/NAME.c` with a `const struct ui_vis vis_NAME` (a new `bit`: none is free any more, see above; the mask is 16 bits).
2. In `vis/vis.c`: `extern const struct ui_vis vis_NAME;` and the pointer in `all[]`.

The Makefile compiles every `vis/*.c`. The menu (up to 16 rows, `VISSUB_MAX` in `main.c`), the web page's Look tab
and the bridge command `vis NAME` pick it up by itself. A visual that needs the wave (`wave = 1`) is only offered
when `x45loop` writes it; one that needs the bands (`bands = 1`) only when there is a stream to analyse.

## The bar colours

The visuals that draw bars, columns or cells (level bars, spectrum, LED, mirror, waterfall) share one colour
scheme, chosen in Options > Visualizers > *Bar colours* (`spec_color`, web `bars N`):

| Name | Look |
|---|---|
| accent colour | the owner's accent colour, the top of the scale in the accent's warning colour |
| green-yellow-red, soft | a pastel green through cream to red, by height |
| green-yellow-red, bright | saturated green through yellow to red, by height |
| fire | dark red through orange to pale yellow |
| ocean | deep blue through teal to foam |
| rainbow | blue at the foot to red at the top |

## Where the analyzer's data comes from

The bars, the spectrum and the waterfall are fed by `x45loop`, which writes `/tmp/x45meter.json` (about 20 times a
second; the UI reads it while the file is at most 2 s old). `spec_poll()` in `main.c` starts it in one of three modes:

| Mode | Source | Used for |
|---|---|---|
| 1 | the radio capture (44100 Hz, 16 bit) | FM / DAB |
| 2 | the MPD fifo output `meter` (`/tmp/x45mpd.fifo`, the stream's own rate, 16 bit) | network playback when the SSI capture is dead |
| 3 | the SSI capture `hw:1,0` at 192000 Hz, 24 bit | the external inputs, and network playback while it works |

The SSI capture gets no clocks on a unit whose SSI has been disturbed, so `x45loop`
waits for samples and dies about every 10 s. `spec_poll()` counts these failures (`sp_fails`: a child that dies
without having written the meter file since it was spawned, or one that has written nothing 6 s after it started),
and after two of them the network input switches to mode 2, which needs only MPD. The count is cleared as soon as
mode 3 delivers, so a unit after a clean boot goes back to the SSI capture on its own. Verified on the unit
2026-10-08: two failures, then a stable `x45loop -r 44100 -f /tmp/x45mpd.fifo`, real spectrum and bars.
