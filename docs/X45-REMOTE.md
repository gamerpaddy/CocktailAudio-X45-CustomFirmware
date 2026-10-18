# The infrared remote (learned 2026-10-08)

Receiver `gpio_ir_recv`, NEC protocol, the stock keytable (66 entries, held in the kernel). Each button sends a raw scancode
(`0x657xx`, reported as `MSC_SCAN`) and, when the keytable knows it, a Linux key code. The vendor re-used the
function-key codes, so what is printed on a button has nothing to do with the code's name: the table below was
learned by pressing the buttons one by one, with `x45irtest` (src/tools/x45irtest.c) holding the receiver
exclusively so that no button did anything meanwhile.

`ir-keytable -r` on the unit lists all 66 scancode / key-code pairs. A held button sends `down`, then NEC repeat frames
(the same scancode again, about every 108 ms), then `up` about 250 ms after the last one.

## Buttons named by the owner (name as printed on the remote)

| Button | Scancode | Key code | Linux name | Function in the manual (X45 user guide, 1-3) |
|---|---|---|---|---|
| Power | 0x65710 | 116 | KEY_POWER | POWER: power on / off |
| Clock | 0x6570f | 359 | KEY_TIME | CLOCK: digital clock display / OFF screen |
| Radio | 0x6575d | 385 | KEY_RADIO | RADIO: go to the DAB+/FM radio screen |
| i-service | 0x6571d | 150 | KEY_WWW | i-SERVICE: go to the i-Service screen |
| input | 0x65760 | 390 | KEY_AUX | INPUT: go to the input screen |
| screen | 0x6570b | 375 | KEY_SCREEN | SCREEN: switch LCD or HDMI screen |
| home | 0x6571c | 102 | KEY_HOME | HOME: go to the home screen |
| search | 0x6574c | 67 | KEY_F9 | SEARCH: pop up the search menu |
| setup | 0x65740 | 141 | KEY_SETUP | SETUP: go to setup |
| mute | 0x6575e | 60 | KEY_F2 | MUTE: mute / unmute |
| 1 | 0x65700 | 513 | KEY_NUMERIC_1 | number / letter entry; seek on the playing screen |
| 2 | 0x65701 | 514 | KEY_NUMERIC_2 | number / letter entry; seek on the playing screen |
| 3 | 0x65702 | 515 | KEY_NUMERIC_3 | number / letter entry; seek on the playing screen |
| 4 | 0x65703 | 516 | KEY_NUMERIC_4 | number / letter entry; seek on the playing screen |
| 5 | 0x65704 | 517 | KEY_NUMERIC_5 | number / letter entry; seek on the playing screen |
| 6 | 0x65705 | 518 | KEY_NUMERIC_6 | number / letter entry; seek on the playing screen |
| 7 | 0x65706 | 519 | KEY_NUMERIC_7 | number / letter entry; seek on the playing screen |
| 8 | 0x65707 | 520 | KEY_NUMERIC_8 | number / letter entry; seek on the playing screen |
| 9 | 0x65708 | 521 | KEY_NUMERIC_9 | number / letter entry; seek on the playing screen |
| backspace | 0x65715 | 472 | KEY_FN_F7 | back arrow: delete the typed letter |
| 0 | 0x65709 | 512 | KEY_NUMERIC_0 | number / letter entry; seek on the playing screen |
| cap/num | 0x6571b | 373 | KEY_MODE | CAP/NUM: toggle numeric keys, alphabet keys, special characters |
| Now playing | 0x6574d | 66 | KEY_F8 | NOW PLAYING: go to the playing screen |
| REC | 0x6574a | 65 | KEY_F7 | REC: record |
| Lyrics | 0x6572d | 370 | KEY_SUBTITLE | LYRICS: show lyrics on the screen |
| Vol+ | 0x65751 | 62 | KEY_F4 | VOL+: volume up |
| Menu | 0x65750 | 68 | KEY_F10 | MENU: pop-up menu screen |
| Stop | 0x65714 | 64 | KEY_F6 | STOP: stop playing |
| Playpause | 0x65712 | 63 | KEY_F5 | PLAY/PAUSE: toggle play and pause |
| Vol- | 0x65752 | 61 | KEY_F3 | VOL-: volume down |
| Up | 0x6570a | 103 | KEY_UP | ARROW: cursor up |
| Right | 0x6570d | 106 | KEY_RIGHT | ARROW: cursor right |
| down | 0x6570c | 108 | KEY_DOWN | ARROW: cursor down |
| left | 0x6570e | 105 | KEY_LEFT | ARROW: cursor left |
| OK | 0x6571a | 28 | KEY_ENTER | OK: confirmation |
| Return | 0x6571e | 1 | KEY_ESC | RETURN: back to the upper level |
| Text/art | 0x6576e | 442 | KEY_IMAGES | TEXT/ART: switch text or art mode |
| Vol balance | 0x6572b | 466 | KEY_FN_F1 | VOL BAL: volume balance |
| Info | 0x65711 | 59 | KEY_F1 | INFO: more detailed information |
| Previous | 0x65713 | 412 | KEY_PREVIOUS | PREVIOUS: play the previous music |
| Next | 0x65717 | 407 | KEY_NEXT | NEXT: play the next music |
| Seek -15 | 0x6571f | 168 | KEY_REWIND | SEEK/-15: seek back 15 s |
| Seek +15 | 0x65716 | 208 | KEY_FASTFORWARD | SEEK/+15: seek forward 15 s |
| Rip CD | 0x65719 | 383 | KEY_CD | RIP: pop up the ripping menu |
| Playlist | 0x6574b | 467 | KEY_FN_F2 | PLAYLIST: go to the playlist screen |
| add to playlist (ADMYPL) | 0x65764 | 468 | KEY_FN_F3 | AD MY PL: make an advanced My Playlist |
| my playlist (MYPL) | 0x65765 | 469 | KEY_FN_F4 | MY PL: make My Playlist |
| Sort | 0x65718 | 470 | KEY_FN_F5 | SORT: sort the file list |
| Bookmark | 0x65742 | 87 | KEY_F11 | BOOK MARK: make a bookmark |
| Favourite | 0x65743 | 88 | KEY_F12 | FAVORITE: add an iRadio station or song to the Favorites folder |
| Shuffle | 0x65744 | 410 | KEY_SHUFFLE | SHUFFLE: change the play mode |
| Font Size | 0x65745 | 471 | KEY_FN_F6 | FONT SIZE: change the text line number (size) |
| Tag edit | 0x65746 | 176 | KEY_EDIT | TAG EDIT: pop up the tag edit screen |
| Sleep | 0x65748 | 142 | KEY_SLEEP | SLEEP: go to sleep mode |
| Eject CD | 0x65761 | 161 | KEY_EJECTCD | EJECT: eject the CD |
| Red | 0x6572a | 398 | KEY_RED | (reserved in the manual; here: marks the radio song that plays now in the song log) |
| Green (also labelled brightness) | 0x6572c | 399 | KEY_GREEN | BRIGHTNESS: control the screen brightness |
| Yellow | 0x6572e | 400 | KEY_YELLOW | (reserved in the manual; here: opens the song log) |
| Blue | 0x6573d | 401 | KEY_BLUE | (reserved in the manual; here: opens the signal path screen, read only) |

Child lock: while it is on, none of these keys act. Hold OK for three seconds to unlock (X45-RELIABILITY.md). Clock (359)
opens the clock, and on the clock puts the backlight down; Sleep (142) opens the sleep timer and steps through its times.

## Letters on the number keys (multi-tap, as printed)

| Key | Letters |
|---|---|
| 1 | `.` `Q` `Z` |
| 2 | A B C |
| 3 | D E F |
| 4 | G H I |
| 5 | J K L |
| 6 | M N O |
| 7 | P R S |
| 8 | T U V |
| 9 | W X Y |
| 0 | space |

This is not the telephone layout: Q and Z sit on the 1 key with the full stop, and the 7 key has P R S without Q.
`cap/num` switches between capitals and digits; `backspace` deletes.

## Keytable entries no button was pressed for

59 buttons were pressed (Power last, on its own, because it can switch the unit off; the owner says that is every
key on the remote); the keytable has 66 entries. The other seven were never sent, so they belong to no button of this
remote, or to a variant of it:

| Scancode | Key code | Linux name |
|---|---|---|
| 0x65730 | 148 | KEY_PROG1 |
| 0x65731 | 149 | KEY_PROG2 |
| 0x65747 | 243 | KEY_BRIGHTNESS_CYCLE |
| 0x65762 | 236 | KEY_BATTERY |
| 0x65763 | 218 | KEY_CONNECT |
| 0x65766 | 183 | KEY_F13 |
| 0x65767 | 227 | KEY_SWITCHVIDEOMODE |

The Green button, which the owner says is also labelled brightness, sends KEY_GREEN (0x6572c), not
KEY_BRIGHTNESS_CYCLE. A scancode that is not in the keytable is still reported by the receiver (MSC_SCAN) but makes
no key event; `x45irtest` logs those too, and none turned up.

## What the buttons do now (deployed 2026-10-08)

Implemented in `handle_ir` / `handle_ir_hold` in `src/x45ui/main.c`. The function follows the label and the manual; buttons
nobody has a function for do nothing.

| Button | Function |
|---|---|
| Power | the power question: press, then again within 3 s (same as the front panel; every request is written to `power.log`) |
| Return | back one level and nothing more |
| Menu | the player menu (visualizers, record, save the station, scan); again closes it. This is what the front panel's RETURN opens |
| Setup | the settings; again leaves them |
| Info | the status page; again leaves it |
| Home | the top of the current input's list: the library root (disks, Radio, Network) on the network input, the disc's tracks on CD, the station list on DAB+/FM; the inputs without a list (S/PDIF, line in, PC USB, Bluetooth) show the player screen |
| Now playing | the player screen |
| Search | the station search (network radio, DAB+, FM); again closes it. See "Station search" below |
| number keys | on the playing screen: seek to a tenth of the track (0 = start); in the station search: letters (multi-tap) or digits |
| Backspace | in the station search: deletes the last character (held, it goes on deleting) |
| Cap/num | in the station search: the number keys type digits instead of letters; again, letters |
| input | the input list; pressed again it steps to the next entry |
| i-service | the network input (the playing screen when already on it) |
| Radio | the radio screen of the current radio input (DAB+ / FM / internet radio) |
| Playlist | the library / list screen |
| Mute | mute / unmute |
| Vol balance | the balance screen (see "Balance" below); again closes it |
| Vol+ / Vol- | volume +-2; held, it runs |
| Up / Down | move in a list, one row per press (see below); in the brightness editor adjusts it; on the player, volume +-2 (held, it runs) |
| Left / Right | previous / next track; in the input list they move through it like Up / Down; in the library up / enter folder; on the FM input tune down / up (held, it sweeps the band) |
| OK | the same as the knob press, except on the playing screen of the network, CD and tuner inputs: there it goes to the list (the remote has Play/pause of its own) |
| Play/pause, Stop | as printed; Stop is the front panel STOP |
| Next / Previous | next / previous track (not repeated while held) |
| Seek +-15 | `seekcur +-15` |
| Shuffle | random on / off |
| 0..9 | on the playing screen: seek to the n-th tenth of the track (0 = start) |
| REC | asks "Record ...? Yes / No" (the cursor is on No); REC again, or OK on Yes, starts it; Return or No cancels |
| Rip CD | the same question, only on the CD input |
| Eject CD | ejects the disc |
| Favourite | on the network input: opens the favourites folder (the browser at `@fav`); on FM: saves the tuned frequency |
| Bookmark | saves what plays as a new favourite: an internet-radio station of the network input, a frequency on FM |
| Green (brightness) | steps the backlight |

A held button is reported by libx45 as `X45_EV_IR` with `pressed == 2` (the kernel never sends key value 2 for this receiver:
a held button is one down, then bare scancode packets about every 108 ms). Only the volume keys, the arrows (and left /
right on FM) act on it; in the station search Backspace does too.

### Balance (2026-10-12)

Vol balance (code 466, `KEY_FN_F1`) opens the balance screen from anywhere (it is also reached through the player menu and
Options > Sound > Balance); on the screen Left / Right and the knob step the balance by one, held they go on (about five a
second), 0 centres it, OK, Return or Vol balance again close it. The volume keys keep working, so the effect can be heard
while it is set. One step is one volume index taken from one side and given to the other.
Web / test: `ir balance` presses the key, `balance <-24..24>` sets it, `ext balance` opens the screen.

### Station search (2026-10-10)

Type a piece of a station's name and the stations that contain it are listed as you go ("suns" lists Sunshine live,
Radio Sunset Lounge ...). It searches the favourites and the router's list, and on the network input also the community
directory (`x45rb search`, asked about 0.65 s after the typing stops, from 3 letters; its answers are added below the
own stations). Opened from a radio-directory list (top voted, a country, a genre ...) it also searches the stations of that list, which come first. On DAB+ and FM it searches the station list. Source: `src/x45ui/ext/search.c`, the matching in
`ext/srchcore.c` (unit test `tests/srch_test.c`).

Ways in (all on the front panel too, no remote needed):

* the player menu (front panel RETURN, remote Menu) has a last row "Search stations" on the network, DAB+ and FM inputs;
* the last row of the favourites, the router stations and the radio directory lists, and of the DAB+ / FM station list,
  is "Search stations" (turn the knob one step back from the first row to land on it);
* the remote's Search key, from any screen on those inputs.

The keys: one row of letters that the knob turns through; OK types the letter under the cursor. The row offers only the
letters that lead to a station (after "su" a handful, not twenty-six) and the cursor waits on the likeliest one; "all"
shows the whole alphabet, "del" / "clr" delete, "list" (or knob down / remote Down) moves to the matches, where OK plays
and RETURN goes back to the keys. On the remote the number keys type the printed letters (see above; the same key again
within a second goes on to the next letter), Backspace deletes, Cap/Num switches to digits. After typing on the remote,
OK goes to the list. Search or RETURN (on the keys) closes the screen.

Web / test: `ext search` opens the screen; `ir search`, `ir backspace`, `ir capnum` and `ir 0` .. `ir 9` send those keys.

**List keys are debounced.** A finger stays on a button longer than the 108 ms between two frames, so an ordinary tap
arrived as a press plus a repeat and moved the cursor two rows or more. In `list_key_ok()` (`src/x45ui/main.c`) a repeat
counts only after the button was held 550 ms and then once per 200 ms; the input list takes no repeat at all, so one press
is one row there; a second press of the same button within 140 ms is dropped. The volume keys keep their immediate repeat.

## Not assigned

Clock, screen, Lyrics, Text/art, ADMYPL, MYPL, Sort,
Font Size, Tag edit, Sleep, Red, Yellow, Blue.
