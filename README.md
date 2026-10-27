# Breakbeat generator for the Daisy Seed3

A standalone breakbeat generator for the Electrosmith **Daisy Seed3**. Put drum breaks
on an SD card, press a button, and the Seed slices the break and stitches a new
random breakbeat from it. It plays the result in a loop and saves it to the card.

It is based on a monome norns script ("the original" below). Its Lua code (`lib/dnb.lua`) runs the
`sox` and `aubioonset` command-line tools hundreds of times per beat, and
SuperCollider plays the result. None of that can run on a microcontroller.
This project reimplements all of it in plain C++ with no OS, no heap and no
external tools, then checks the result against the original sample-for-sample.

> Runs on a Seed3: generating and playback have been used on hardware. Focus
> mode (D15/D16) is new and so far only tested on the host. The engine and
> player pass host tests. The USB log shows what it is doing.

## Hardware

| Seed pin | Connect to | Purpose |
|---|---|---|
| D15 | button to GND | **Focus:** loop the 4-bar part (quarter) playing now; press again to go back to the whole loop |
| D16 | button to GND | **New version** of the quarter in focus (only in focus) |
| D17 | button to GND | **New beat** from the same break (not in focus) |
| D18 | button to GND | **Next break** on the card, and a new beat from it (not in focus) |
| D19–D22 | 10 k pots (3V3 / AGND; 4.7 k works too) | how varied D16's new versions are (see below). `kPots = false` in `src/main.cpp` stops reading them |
| D23, D24, D25, D28 | 3V3 → LED → resistor (e.g. 330 Ω) → pin (lit when the pin is low; for LEDs to GND set `kQledLow = false`) | **Quarter LEDs:** the quarter playing, or the one in focus |
| D1–D6 | micro-SD socket | D1 DAT3, D2 DAT2, D3 DAT1, D4 DAT0, D5 CMD, D6 CLK |
| Audio out L/R | | stereo out, 48 kHz |

**Tie AGND to DGND** (Seed pins 20 and 40). The Seed doesn't join them, and
with them apart the pots read wrong values.

Buttons use the internal pull-ups. The SD card needs 10–47 k pull-ups to 3V3
on CMD and DAT0–3; most micro-SD breakout boards already have them. Avoid
boards with 5 V level shifters. The button and pot pins match the
wiring of the `MyProjects/test` project.

## Controls

At boot the first break loads and a beat starts playing (`auto_generate`).
The loop is split into 4 quarters. With `beats = 64` (16 bars) each quarter is
4 bars; with the default `beats = 16` each is one bar. `beats` has to divide
by 4 for focus mode.

- **D15, focus:** the quarter playing when you press it starts looping on its
  own, seamlessly from the next beat (pressed on its last beat: from its
  start). Press D15 again to go back to the whole loop: it carries on from
  the next beat, with the version you were hearing spliced into its quarter
  (5 ms crossfades at the edges).
- **D16, new version (focus only):** makes a new random version of the
  quarter in focus: new chops, stutters, reverses and so on, at the same tempo
  and length. It carries on from where the quarter starts in the break, so
  kicks and snares stay on the same bar positions. It swaps in on the next beat
  (2 ms crossfade). Press again for another; the last one you hear is kept when
  you leave focus. Pressing D15 while it is still working stops it.
- **D17, new beat:** a whole new loop from the same break, swapped in on the
  next beat. Pressing D17 or D18 while it works abandons it and does that
  press instead.
- **D18, next break:** loads the next `.wav` in `/MBB/SAMPLES` (wrapping
  round; the folder is rescanned, so new files show up) and makes a new beat
  from it. Several presses while it is busy skip that many breaks.
- D17 and D18 do nothing in focus (the log says to press D15 first), and D16
  does nothing outside it.

Every new beat is saved to `/MBB/OUT` (`save = 1`); D16's versions are not
saved on their own.

**Pots (D19–D22), for D16:** each pot controls a group of the effect
probabilities in `config.txt`. **Fully left** (0) uses the `config.txt` values,
so new versions sound like a normal beat. Turning right moves the group
steadily away from them, up to the maximum shown at **fully right** (100). A
value set higher than that in `config.txt` stays as it is. The pots are read
when you press D16, and with `kPots = false` D16 uses `config.txt` as it is.

| Pot | Group | Probabilities (max at fully right) |
|---|---|---|
| D19 | **shuffle:** how far chops jump around, extra drums | `p_deviation` 100, `p_kick` 100, `p_snare` 100 |
| D20 | **stutter:** repeats and cut-offs | `p_stutter` 100, `p_trunc` 50 |
| D21 | **pitch:** reverse, pitch-up, half speed | `p_reverse` 80, `p_pitch` 60, `p_half` 40 |
| D22 | **space:** reverb and time-stretch | `p_reverb` 60, `p_stretch` 40 |

D17 and D18 use `config.txt` as it is.

**LED (on the Seed):**
- **Slow blink:** booting or busy (analysing, generating, loading).
- **Fast blink:** error. Check the USB log.
- **Flashes on every beat** while playing, with a longer flash at the start of the loop.
- **Off:** nothing loaded.

**Quarter LEDs (D23, D24, D25, D28):** the LED of the quarter being played is
lit, so they step 1 → 2 → 3 → 4 round the loop. In focus only that quarter's
LED is lit; it blinks fast while D16 is making a new version.

**USB log:** every action is printed, e.g. `focus on quarter 2 (16 beats)`,
`quarter 2 version 3 (pots 50 80 10 50) in 640 ms` (pot positions 0–100) and
`back to the loop, with the new quarter 2`.
In focus mode the log also shows the pots when focus starts and whenever one
moves (at most about 7 times a second), with the probabilities D16 would use:
`pots D19 0 D20 35 D21 100 D22 12: dev 10 kick 50 snare 20, stutter 48 trunc 19, ...`.
**Pot not responding:** if its number stays put while you turn it, its wiper
or 3V3/GND isn't connected. If the numbers are wrong or jumpy, check that
AGND and DGND are tied together.

## SD card

Use a card formatted as **FAT32** (or FAT16). libDaisy's FatFs is built
without exFAT, and cards larger than 32 GB usually come formatted exFAT, so
reformat those first. On macOS: Disk Utility → Erase → MS-DOS (FAT).

```
MBB/
  config.txt        settings (written with defaults if missing)
  kick.wav          extra kick layered on the beat (p_kick)
  snare.wav         extra snare (p_snare)
  SAMPLES/*.wav     your breaks
  OUT/<bpm>_<n>.wav every generated beat (same naming as norns)
```

`sdcard/MBB` has a default amen break, kick and snare (MIT licensed; the licence
is included in this repository). To copy them onto a mounted card:

```sh
tools/prepare_sd.sh /Volumes/<CARD>   # never overwrites existing files
```

**Breaks:**
- **Format:** `.wav`, 8/16/24/32-bit integer or 32/64-bit float, mono or stereo.
- **Sample rate:** use 48 kHz. Like the original, other rates are re-labelled
  as 48 kHz, so they play at a different pitch and speed. Set
  `legacy_rate = 0` to resample them instead.
- **Length:** up to 30 s (longer files are cut).
- **Tempo:** detected automatically, like aubio in the original. If detection
  gets it wrong, set `input_tempo`.

Everything in `config.txt` is commented. The main keys:

| key | default | meaning |
|---|---|---|
| `beats` | 16 | loop length in beats |
| `tempo` | 0 | output BPM, 0 = the break's tempo |
| `input_tempo` | 0 | break BPM, 0 = detect |
| `sample` | | break to load first |
| `chop` | 0 | how the break is chopped: 0 = at the hits, 8/16/32 = equal chops |
| `p_deviation … p_snare`, `kick_db`, `snare_db` | norns defaults | effect probabilities, as in the norns PARAMS menu (the pots' fully-left values for D16) |
| `seed` | 0 | 0 = random; N = reproducible (beat k uses N+k−1) |
| `save` | 1 | write each beat to `/MBB/OUT` |
| `auto_generate` | 1 | make a beat as soon as the first break loads |
| `legacy_rate`, `legacy_channels` | 1 | copy the original's sox quirks (see below) |

## Build and flash

You need the Daisy toolchain (`arm-none-eabi-gcc` 10.3) and `dfu-util`. This
project is pinned to **libDaisy v9.0.0**. Older libDaisy versions such as v5.x
don't recognise the Seed3 and won't drive its audio codec.

```sh
git submodule update --init      # libDaisy v9.0.0
make libdaisy                    # once: builds libDaisy with -Os
make                             # builds the firmware in build/
```

To flash: hold **BOOT**, tap **RESET**, release BOOT, then run `make program-dfu`.

The firmware has to fit the Seed's 128 KB internal flash, so both libDaisy and
the project are built with `-Os`, and the project also with link-time
optimisation (`-flto`). Only `src/engine/dsp.cpp`, where generating spends its
time, is built with `-O2` (about 2.4 KB more). A libDaisy built at the default
`-O3` won't fit. The
`lto-wrapper: warning: Options to Xassembler do not match` message is harmless:
it only affects assembler listing files. `src/main.cpp` also stubs out
libDaisy's USB host and UART drivers, which this firmware doesn't use; that
saves about 9 KB.

| | used | of |
|---|---|---|
| FLASH | 121,912 B | 131,072 B (93.0%) |
| SRAM (AXI/D2) | 243,096 B | 524,288 B |
| SDRAM | 66,280,000 B | 67,108,864 B: 49.0 MB engine + 2 × 45 s play buffers |

**USB log:** connect to the Seed's USB serial port (for example
`screen /dev/tty.usbmodem* 115200`) to see:
- the detected tempo and onsets;
- generation progress and timing;
- file names and errors.

If no terminal is attached, a log line is dropped after at most 20 ms, so
unplugging the terminal can't hang the firmware.

**`cannot save … (FatFs error N, SD error 0x…)`:** the card can be read but
not written. The firmware resets the card and turns saving off until the next
restart; the loaded break keeps working even if the card then stops answering.
To find out why, flash the SD write test in `tools/sdtest`
(`cd tools/sdtest && make && make program-dfu`) and open the USB log: it tries
several bus settings and reports which ones can write. Common values of N:

| N | meaning | fix |
|---|---|---|
| 1 | disk error | wiring: add 10–47 k pull-ups on CMD and DAT0–3, shorten the wires, or try another card |
| 3 | not ready | card not answering; check the wiring |
| 5 | no path | create the `MBB/OUT` folder |
| 7 | denied | card full |
| 10 | write protected | slide the card's lock switch off |

To run without saving, set `save = 0`.

**`SD error reading … (FatFs error N, SD error 0x…)`:** the card stopped
answering part-way through a file. The firmware restarts the card and does not
use the partly read break; press D18 to load the next break (with only one
break on the card, the same one again). The values of N are the same
as above. If it keeps happening, check the card on a computer (Disk Utility →
First Aid) and the wiring.

## How it maps to the original

| original | port |
|---|---|
| `dnb.lua`: slicing, effects, beat assembly | `src/engine/engine.cpp` (same RNG call order) |
| `sox` effects used by dnb.lua (trim, speed, tempo, pitch, reverse, reverb, fade, gain, pad, splice, mix, rate/channels) | `src/engine/dsp.cpp`: ports of the sox algorithms (WSOLA tempo, freeverb-style reverb, splice best-overlap search, etc.) |
| `aubioonset` (tempo detection and slicing) | `src/engine/analysis.cpp`: aubio's default HFC onset detection and peak picking, and dnb.lua's tempo estimate |
| the SuperCollider engine (two Phasors, ToggleFF, Lag, RLPF) and the norns lattice clock | `src/engine/player.cpp` |
| norns params menu | `config.txt` and the pots |
| `PARAMS > generate` | button D17 |
| files saved on the norns | `/MBB/OUT/<bpm>_<n>.wav` |

The port copies these quirks of the original exactly:
- **Channel and sample-rate quirk.** dnb.lua passes `-c 2 -r 48000` as *input*
  options to sox. That makes sox re-label the data instead of converting it:
  a non-48 kHz break changes pitch and speed, and a mono kick or snare plays at
  double speed (`legacy_rate`, `legacy_channels`).
- **Stutter effect order.** sox applies its automatic `rate` stage after
  `pitch … trim`, which shifts where the stutter is cut.
- **Rounding.** Durations are written as `%g` text, so they're rounded to 6
  significant digits.

Deliberate differences:
- **Float maths:** float/double instead of sox's 32-bit integer intermediates
  (≈ −60 dB difference).
- **No dither.** sox adds random TPDF dither to every 16-bit temporary file.
  That makes the original itself produce slightly different splices from run to
  run (< 2 ms).
- **Loop swaps on the beat.** A new loop replaces the playing one on the next
  beat, rather than whenever the file finishes loading. Reader positions are
  scaled when the tempo changes, and the bar count restarts if the loop length
  changes.
- **Internal clock only.** The clock is the Seed's own; there's no MIDI or link sync.
- **New controls.** Focus mode (D15/D16), the pots and the `seed` key are
  new. Playback can't be stopped (norns K3).

## Verification

```sh
make -f Makefile.host test MBB_SOX=/path/to/sox   # engine + player tests
make -f Makefile.host cli                          # build-host/mbb: the generator on a computer
tools/ab_run.sh <original>/lib 7 165 17           # needs lua 5.4, sox, aubio on PATH
```

The engine can also build beats from a selection of chops (the firmware no
longer has a mode for it). To try it on a computer:

```sh
build-host/mbb -i amen.wav -o out.wav -v --only kick,snare   # kick and snare chops only
build-host/mbb -i amen.wav -o out.wav -v --chop 16 --use 1,5-8,13
```

`-v` prints the numbered chops.

The host tests check:
- onset and tempo detection against aubio (≤ 2 samples);
- each sox effect against real sox;
- config parsing;
- the player's beat, loop-restart, swap, stop/start and crossfade timing, and chop playback;
- chop selection:
  - selecting every chop leaves the output unchanged;
  - beats use only the selected chops and still have the right length;
  - equal chops cover the whole break.
- focus mode:
  - the quarter being played (for the quarter LEDs) is right on every sample;
  - focus carries on seamlessly from the next beat and loops just its
    quarter; pressed on the quarter's last beat it starts from the top of it;
  - a new version swaps in on the next beat at the same place;
  - leaving carries on from the next beat of the whole loop (after the last
    quarter: from the start), with the version spliced in place: only its
    quarter changes, with smooth seams;
  - versions start at the quarter's place in the beat and have the right
    length, and a version stopped part-way (D15 pressed) fails cleanly;
  - the pots scale their probability groups as described above;
  - a real beat with random focus, versions (random pots), releases and new
    beats has no clicks. The run is written to `tests/out/focus_demo.wav`.

`tools/ab_run.sh` runs the original `dnb.lua` itself, with the port's PCG32 RNG
injected, and compares its output with the port's. All cases matched
with the same length and frame alignment (sox dither off):

| seed | tempo | beats | settings | SNR |
|---|---|---|---|---|
| 7 | 165 | 17 | norns defaults | 64.9 dB |
| 1 | 177 | 17 | norns defaults | 61.6 dB |
| 2 | 140 | 33 | norns defaults | 62.0 dB |
| 3 | 90 | 9 | norns defaults | 60.0 dB |
| 4 | 177 | 65 | norns defaults | 63.6 dB |
| 11 | 165 | 17 | chaos* | 65.4 dB |
| 12 | 200 | 33 | chaos* | 59.3 dB |
| 21 | 177 | 33 | chaos* | 68.6 dB |
| 22 | 120 | 17 | chaos* | 62.6 dB |

\* `--deviation 30 --reverse 30 --stutter 40 --pitch 20 --trunc 10 --half 10 --reverb 15 --stretch 10 --kick 70 --snare 50`

The remaining difference, around −60 dB, comes from float versus integer
arithmetic. The harness always passes the norns defaults, because dnb.lua's
own command-line defaults are different.

A 17–65 beat loop takes 10–260 ms to generate on an Apple M-series Mac, and a
4-beat quarter about 6 ms. On the Seed3's 480 MHz Cortex-M7 the hot DSP loops
use single-precision maths (the M7's double precision and libm's `sin` are
slow) and skip hopeless candidates in the splice and tempo searches early.
Before that a one-bar quarter took 0.4–2.6 s on hardware; the D16 log line
shows the time now. Whole beats take longer, plus the time to write them to
SD. Audio keeps playing while it generates.

## Limits

- **Breaks:** at most 64 files, each up to 30 s long, `.wav` only.
- **Loops:** up to 40 s at the break's tempo and 45 s at the output tempo.
  `beats` is reduced to fit.
- **Saved beats:** up to 512 in `/MBB/OUT`.
- **Memory:** uses all but about 0.8 MB of SDRAM, and about 9 KB of flash is left.
  Keep everything else at `-Os`.
