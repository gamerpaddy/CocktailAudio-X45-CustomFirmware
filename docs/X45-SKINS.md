# Skins (x45ui)

A **skin** is a look, not a colour: it decides how the panel is laid out. Lists become other
things (a row of symbols, a wheel, a board of tiles, a table), the now-playing page is
arranged differently, the bar and the key hints change. The accent colour stays the owner's
own choice (Options > Display: Accent colour, Background, Text colour) and works with every skin.

| Skin | What it does |
|---|---|
| `classic` | the original look: dark bar on top, plain rows, a bar under the cursor, keycaps along the foot |
| `carousel` | everything centred, one thing at a time: the inputs are a row of symbols with the chosen one large in the middle, every other list is a wheel, the player is a poster |
| `tiles` | light paper cards: the inputs are a 4x3 board, names and folders are cards in two columns, settings are cards with the value in a pill, now playing has a progress ring beside the words |
| `ledger` | a table like a terminal: numbered rows with dotted leaders, now playing as named fields (SOURCE, STATION, TIME ...), progress as blocks |
| `matrix` | an LED dot-matrix display: every word is lit dots over the dim dots that are off; long text runs like a ticker; a setting's value sits in a framed window; progress is a row of LEDs |
| `split` | a list on the left, and on the right the row under the cursor in full: the whole name of a track, the large picture of an input, a setting's value in big type (the now-playing page is the classic one) |
| `poster` | large type for reading across the room: six big rows to a page, now playing with the title as large as it will go and a thick progress bar |

Choose one in Options > Display > **Look** (OK steps through them), on the web page under the
**Look** tab, or with the bridge command `skin NAME`. The choice is kept (`skin = NAME` in
`etc/x45ui.conf`); the colours are `theme = N` (accent), `ground = N` (background) and `ink = N` (text), with the
bridge commands `theme N`, `ground N`, `ink N`.

## How it is built

```
src/x45ui/skins/
  skin.h       the contract: struct ui_skin, struct ui_list, struct ui_item, struct ui_np
  skins.c      the registry (one line per skin), the palette, shared drawing helpers
  classic.c    the original pieces (public, so that other skins can keep the ones they like)
  colours.c    the colours the owner chooses (accent, background, text) and how they combine with a skin's palette
  carousel.c   one file per skin
  tiles.c
  ledger.c
  matrix.c
  split.c
  poster.c
```

`main.c` draws none of this itself. A screen says **what there is to show** and the skin lays it out:

* a **list** (`struct ui_list`): kind (`LK_TEXT` names, `LK_ICONS` things with a picture,
  `LK_SETTINGS` labels with values), the count, the cursor, the row in force (`current`), a
  title, a message for the empty list, the key hints, and an `item()` callback that fills one
  row (`struct ui_item`: label, a lead column, a value, an icon, folder / command flags, a mark).
  Browser, radio stations, CD tracks, tuner stations, inputs, options and the ext/ screens all
  go through it (`skin_list()`).
* **now playing** (`struct ui_np`): where it comes from (kicker), title / sub / extra lines,
  the audio format, time, progress (below 0 when a stream has none), state, flags, the DAB
  picture, a message, the hint. `fill_np()` in `main.c` collects the facts for internet
  radio, library tracks, CD, FM, DAB, Bluetooth and PC USB; states with a plain page of their
  own (nothing playing, scanning, missing module, level meter) stay classic.
* the **bar** on top and the **hints** along the foot.

What a skin leaves `NULL` falls back to the classic look, so a skin may change only its
palette, or everything.

The visuals (bars, dials, spectrum, waterfall; see X45-VISUALS.md) are placed by the skin:
`draw_visuals_rect(u, x, y, w, h)` draws what the owner switched on into any rectangle and keeps the
partial-redraw bookkeeping, so a skin only has to say where they go.

## Adding a skin

1. `src/x45ui/skins/NAME.c` with a `const struct ui_skin skin_NAME` (name, label, one-line
   `about` for the web page, the palette as 0..255 triples, `tint` (how far the palette is
   pulled towards the accent), `rows` (how many rows a scrolling list shows) and the hooks it
   wants to replace).
2. One line in `skins/skins.c`: `extern const struct ui_skin skin_NAME;` and the pointer in `all[]`.

The Makefile compiles every `skins/*.c`; the Options row, the config key, the bridge command
and the web tab pick the new skin up by themselves.

## Colours

A skin only brings the colours it starts from. On top of them the owner chooses three things
(`skins/colours.c`, one line in a table adds a colour):

| Choice | Values | Config / command |
|---|---|---|
| accent (highlights, the row under the cursor, the bars; with its own warning colour) | green, blue, amber, violet, rose, ice, red, orange, gold, lime, teal, magenta, white | `theme`, `theme N` |
| background (the panels, lines and cards are derived from it) | skin's own, black, graphite, slate, navy, teal, forest, plum, wine, coffee, paper, sand | `ground`, `ground N` |
| text | skin's own, white, warm, green, amber, cyan, rose, grey | `ink`, `ink N` |

`skin_palette()` builds the screen's colours from these and the skin's palette. With the skin's own
background, its colours keep their brightness and are pulled towards the accent's hue by `tint`;
`accent_pct` darkens the accent for a light skin that needs a deeper one. A light background (paper,
sand) gets dark text and a deeper accent by itself, so every combination can be read.

## Settings and values

A setting's value is never drawn as plain text beside its name: it is in a chip of its own (a tinted
patch with the value in the accent colour; dark on the cursor bar, in a window in `matrix`, in big type
in `split`, a two-column table in `carousel`), so that the name and the value cannot be mistaken for
one another (`skin_value_chip()` in `skins/skins.c`).

## Icons

The icons are drawn from one vector description (`tools/make_icons.py`, PIL) at two sizes: 24 px glyphs
(`NAME.png`, for lists and the top bar) and 128 px pictures (`NAME@128.png`). `skin_icon()` draws the picture
for anything larger than a glyph and shrinks it with every pixel counted (a box filter), so the large icons of
the input board and the carousel stay sharp. A missing picture falls back on the glyph.

## Taking screenshots

`x45ui` draws to `/dev/fb0` (1024x600, 32 bit). `cat /dev/fb0` over SSH and a few lines to
turn the raw frame into a PNG are all that is needed to look at a skin without standing in
front of the unit; the web page shows the same live under *Panel*.
