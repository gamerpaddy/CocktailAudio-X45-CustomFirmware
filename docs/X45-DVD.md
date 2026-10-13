# Sound from video DVDs (and the drive's door)

Added 2026-10-09. The CD input plays the sound of a video DVD when that sound is plain PCM or Dolby Digital (AC-3). No video.

## What plays

* A disc whose only track is a data track and that carries `VIDEO_TS/VTS_nn_0.IFO` files. `x45cd` mounts it read-only (UDF, then ISO 9660) on `/tmp/x45dvd`, reads the IFOs and takes **the longest title that has a playable audio stream**: a linear-PCM stream (16, 20 or 24 bit; 48 or 96 kHz) if the title has one, else a Dolby Digital stream (always 48 kHz). With several Dolby Digital streams the one with the fewest channels is used (a disc with a 2.0 and a 5.1 track plays the 2.0 one).
* The chapters of that title are the tracks: `x45cd toc` prints `<n> <frames> dvd <chapters> <Hz> <bits>`, then `<k> audio <75 x seconds>` per chapter. The panel and `/api/v1/state` show them as tracks (the panel says "chapter"/"DVD").
* `x45cd play <chapter>` reads the VOB files sector by sector, picks the private-stream-1 packets of substream `0xA0 + stream` (PCM) or `0x80 + stream` (Dolby Digital), unpacks the PCM (a sample group cut by a packet boundary is put back together) or decodes the AC-3 frames, and writes it to the DAC as 32-bit at the disc's rate. Next / previous / pause / stop work as on a CD (previous within 3 s of the start goes back a chapter).
* PCM: channels beyond the second are dropped, mono is doubled.
* Dolby Digital: decoded with liba52 (a52dec 0.7.4, `src/x45cd/a52/`, GPL v2+, see `THIRD-PARTY.md`) to stereo; a 5.1 stream is folded down by the decoder's own stereo downmix (centre and surrounds mixed in, level adjusted). Dynamic range compression is off (full dynamics, like the PCM discs). Decoding costs about 2 % of one core. The panel's format line says "48.0 kHz  Dolby Digital".

Checked on the disc the user put in (a concert, 18 chapters, 79:51, 16 bit / 48 kHz / 2 ch, not scrambled): the list comes out right, chapter 3 plays at 48 kHz (the DAC reported 48003 Hz), the position advances in real time, stop unmounts. Not heard by ear yet.

The second sample disc (a concert, 23 chapters, 103 min, Dolby Digital 2.0 + 5.1, not scrambled) plays from the shell: the 2.0 stream is chosen, chapters, next/previous, pause and stop work, the decoded waveform is identical on the host and on the unit, and the 5.1 downmix correlates with the 2.0 stream (the 2.0 stream on that disc has identical left and right). Not heard by ear yet either.

## What does not play

* **DTS, MPEG audio**: there is no decoder here. A disc whose longest title has only those is listed as one data track and does not play. `x45cd toc` tells it apart (`<n> <frames> dvdx DTS`, read from the first audio stream of the longest title), and the panel says "Video DVD: DTS sound   not supported" and refuses to start it, instead of a bare data track.
* **Dolby Digital at other rates or with 7.1 / E-AC-3**: DVD-Video only has 48 kHz AC-3 up to 5.1, which is all that is handled.
* **Scrambled (CSS) discs**: not descrambled, and that is not going to be added. This disc is not scrambled (PES scrambling bits are clear in every packet); a scrambled one would produce noise.
* DVD-Audio (MLP), several titles / angles / interleaved cells, menus, video.
* 20-bit PCM and PCM with more than two channels are written to the spec but were never tried on a disc; only 16-bit stereo is tested.
* No chapter titles (a video DVD has none in the IFO); the album line is blank.

## Start-up time

Reading the disc's IFOs is slow: each one costs two seeks, about 0.6 s on this drive, and a disc has one per title set (the second sample disc has 12). `dvd_open` reads all of them to find the longest title, which took 8-10 s on that disc. The verdict is now kept in `/tmp/x45dvd.cache` (`<toc signature> <title set> <other format>`), keyed by a hash of the table of contents (track count and every start sector); the second start of a disc (`x45cd play` after `x45cd toc`) reads only that one IFO and takes 0.7 s. A cache that does not fit the disc (the title set will not parse) is dropped and the whole disc is scanned again. The first listing of a disc still takes the full scan.

## The drive's own eject button

While a disc plays, the drive's button did nothing. The kernel mount (and apparently the drive) held the door locked. `x45cd play` now sends PREVENT ALLOW MEDIUM REMOVAL (allow) when it starts and at the start of each track, so the button works. When the disc is taken out under a playing `x45cd`, it stops by itself ("disc removed", exit 7): two failed reads in a row plus a drive that reports no disc (or a changed disc), or 40 failed reads in a row. While paused it looks once a second. A stale "media changed" flag from the drive (it can be reported while the same disc is still there) is not trusted alone: the table of contents is read again and only a different one, or a failed read, counts as the disc being gone.

## Changing the disc

The CD input now asks the drive every 2 s (`x45cd id`, a child process, never waited for) while nothing plays: a disc that is gone is forgotten (track list, HDCD flag), a different disc (other track count or length) is listed again. Before, the list of the old disc stayed until a track was started. `x45cd id` prints `<ntracks> <frames>` (exit 3: tray open, no disc).

`x45cd eject` unmounts a DVD that a killed player left behind before it opens the tray.

## Files

`src/x45cd/dvd.c`, `dvd.h` (IFO reader, mount, VOB reader, LPCM unpacker, AC-3 glue), `src/x45cd/a52/` (liba52, with `config.h` ours and two shifts patched in `bitstream.c`), the DVD path in `src/x45cd/main.c` (`cmd_play_dvd`, `cmd_toc`), `src/tests/dvd_test.c` (`make nativetest`; given sample IFO and VOB files from a disc of your own it checks the chapter list and the waveform of a few seconds of real audio for steps and joins, and it decodes Dolby Digital streams and compares them; the discs used for development are not part of the repository).

## Enhanced CD (CD-Extra): an audio session followed by a data session

The data track of such a disc is a second session. The table of contents counts a gap of 11400 sectors (lead-out,
lead-in and pre-gap) between the last audio track and the data track, but nothing can be read there: ripping or
playing the last audio track up to the next track's start fails at the end (read errors, a stall). `track_end()` in
`x45cd/main.c` and `x45rec/main.c` therefore ends an audio track that is followed by a data track 11400 sectors early
(the same rule cdparanoia uses), for the length shown by `x45cd toc`, for `play` and `hdcd`, and for the rip. A normal
disc is not affected (the rule needs a data track after the audio, and a track longer than the gap). **Not tried on a real
Enhanced CD yet** (none was at hand); the logic is covered by reading only.
