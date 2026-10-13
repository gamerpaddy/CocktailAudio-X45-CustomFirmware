# Panel options and controls (x45ui)

Hold MENU for one second on the player to open the **options**: four groups (Sound, Display,
Visualizers, Controls), OK opens one. In a group each row shows its name on the left and its
value on the right; OK changes it (a list steps to its next value and wraps). Everything is
saved in `etc/x45ui.conf` and applied at start.

Every screen has a row of key hints along its foot: a key as a small keycap, what it does
beside it. A message that appears under the content (`saved`, `queued 3 tracks`, an error)
goes away by itself after 8 seconds.

| Row | Values | Config key |
|---|---|---|
| Sound: Volume | fixed / variable | `volume_mode` |
| Sound: Balance | left 24 .. centre .. right 24 (steps of one volume index) | `balance` |
| Sound: Trim (analog inputs) | -12 .. +9 dB | `trim_<input>` |
| Sound: RCA out / XLR out | on / off | `lineout`, `xlr` |
| Display: Brightness | knob adjusts, OK keeps | `brightness` |
| Display: Look | classic, carousel, tiles, ledger, matrix, split, poster (see X45-SKINS.md) | `skin` |
| Display: Accent colour | green, blue, amber, violet, rose, ice, red, orange, gold, lime, teal, magenta, white | `theme` |
| Display: Background | skin's own, black, graphite, slate, navy, teal, forest, plum, wine, coffee, paper, sand | `ground` |
| Display: Text colour | skin's own, white, warm, green, amber, cyan, rose, grey | `ink` |
| Display: Text | pixels / smooth / font | `font_smooth` |
| Display: Screen off | never, 15 s .. 1 h idle | `screen_off` (seconds) |
| Display: Input in the top bar | icon / name | `input_icons` |
| Visualizers: Visualizers | same on all inputs / per input | `vis_scope` (`all`/`input`), `vis_all_set` (bit mask), `vis_<input>` |
| Visualizers: Spectrum style | bars with peaks, LED cells, mirrored, curve (the four spectrum visuals of before, now one) | `spec_style` (0..3) |
| Visualizers: Spectrum bands | 16, 24, 32, 48, 64, 96, 128 bands | `bands` |
| Visualizers: Bar colours | accent colour, green-yellow-red soft / bright, fire, ocean, rainbow, amber, green phosphor, violet, sunset, neon, ice, white, red | `spec_color` |
| Visualizers: Stereo field | Marantz scope (round green-phosphor tube with afterglow, the default), plain | `gon_look` |
| Visualizers: Stereo field colour | the look's own, green phosphor, amber, ice blue, white, red, violet, accent colour, by distance (bar colours by distance from the middle) | `gon_color` |
| Visualizers: Stereo field bars | correlation and balance bars beside the scope, or left out (the scope then sits in the middle) | `gon_nobars` |
| Visualizers: Stereo field size | beside the text (the skin's own page), or almost fullscreen: the scope fills the height under the bar at the right edge and the text becomes a small column at the left (title, artist, album, format, time, progress, state), nothing overlaps; it shows the stereo field alone, whatever else is on, and turns the bars off (turning the bars on ends it) | `gon_big` |
| Controls: Input switch | after 3 .. 10 s, or only with OK | `in_auto` (0 = only OK) |
| Controls: Knob skip | 1 (every click skips) .. 10 clicks | `rb_steps` |
| Controls: Resume at start | last input and radio station at start | `resume`, `input`, `last_radio`, `last_radio_url` |
| Controls: CD rip format | FLAC (lossless, the default), MP3 320 kbps (constant), MP3 V0 (variable, LAME -V0, about 220-260 kbps); only for ripping an audio CD, the video DVD sound and the recordings of inputs stay FLAC | `rip_format` (`flac`, `mp3`, `mp3v0`) |

The Visualizers group lists only what belongs to the visuals that are switched on right now (for this input, or for all
of them): *Spectrum bands* with the spectrum or the waterfall, *Spectrum style* and *Bar colours* with the spectrum (the
colours also with the stereo field while its colour is "by distance", and with the stereo width), the four *Stereo field* rows with the stereo field. The web's Look tab does the same. Nothing is lost: the values stay and come back when the
visual is switched on again.

Resume: the unit comes back on the input it was left on (PC USB, FM at the last frequency, DAB at
the last station, Bluetooth, CD ...), and on the network input the last internet radio station
starts again. (Until 2026-10-07 the input part did nothing: the saved name was compared with its
line break still attached.)

The screen "off" is brightness 0. Any key, any web command or a new track wakes it, at the
brightness that was set. The panel is lit again whenever x45ui hands it back.

## Visualizers

RETURN on the player opens the RETURN menu. Its first row, **visualizers**, opens a sub menu
with one switch for each visual (level bars, analog VU, oscilloscope, stereo field, the spectrum in its four styles,
waterfall, loudness history, record check, tonal balance, key and chord, stereo width; see X45-VISUALS.md), two on at the most: a third switches the one that was on first off. They apply to the current
input or to all inputs, depending on the *visualizers* option. The spectrum has falling peak
lines; the waterfall shows the last ~160 analyzer frames (newest on top), with as many columns
as the *spectrum* band setting.

The balance (**Options > Sound > Balance**, the RETURN menu's *balance* row, or the remote's Vol balance key) has a screen of
its own: a bar from the middle towards the louder side, the setting in words (CENTRE, LEFT 3, RIGHT 12) and the two levels
that result at the current volume. The knob and Left / Right step it, 0 centres it, OK or RETURN is done; it is heard at once
and kept when the screen closes. With the volume *fixed* it still works: the louder side stays at full level and the other is
turned down (the analogue stage is switched to variable for as long as the balance is off centre, the digital outputs are never
touched). The same arithmetic as the stock firmware's.

The second row, **visualizer look**, opens the Options > Visualizers group (bar colours, the stereo field's look,
colour, bars and size) from the player without the way through the options; RETURN comes back to the RETURN menu.

The same menu holds, depending on the input: FM: save station, scan; DAB: scan; Bluetooth:
pair a new device, forget all devices. Internet radio stations are saved from the library
side (the FRITZ!Box list), not from the panel. A scan asks "are you sure?" first.

## Other controls

* **Hold STOP (1 s)** opens a delete question for the playing local track, or for the entry
  under the cursor in the library. Yes deletes it, but only on a storage that is mounted
  read-write (the same rules as the web interface: never a library root or a whole disk). A
  short press still stops (or ejects on an idle CD screen).
* **Rubber band** (knob on the player skips tracks/stations): a bar in the top bar grows from
  the middle, green to red, and fades out when the knob rests. The number of clicks for a
  skip is the *knob skip* option.
* **Top bar**: the PC USB symbol when a computer is connected, the position of the playing
  station (`3/12`) for DAB, FM presets and internet radio, mute, MPD warning, REC time.
* Long titles, artists, albums and station names run (rest, glide, rest, repeat) instead of
  being cut off; in the library the selected row runs.
* **Library > Network devices** keeps its device list; "refresh" is the last row. A server folder
  with more than 256 entries shows the first 256 and says so ("first 256 shown").
* Stream addresses and library paths up to 511 characters are kept whole (rows beyond that are left out
  instead of being cut short).
