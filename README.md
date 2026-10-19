# Breakbeat generator for the Daisy Seed3

A standalone breakbeat generator for the Electrosmith **Daisy Seed3**. Put drum breaks
on an SD card, press a button, and the Seed slices the break and stitches a new
random breakbeat from it. It plays the result in a loop and saves it to the card.

It is based on a monome norns script ("the original" below). Its Lua code (`lib/dnb.lua`) runs the
`sox` and `aubioonset` command-line tools hundreds of times per beat, and
SuperCollider plays the result. None of that can run on a microcontroller.
This project reimplements all of it in plain C++ with no OS, no heap and no
external tools, then checks the result against the original sample-for-sample.

> Runs on a Seed3: generating, playback, CHOP mode and LIVE quarters have
> been used on hardware. The engine and player also pass host tests. The USB
> log shows what it is doing.

## Hardware

| Seed pin | Connect to | Purpose |
|---|---|---|
| D15 | button to GND | **Generate** a new beat (hold ≥ 0.6 s: **CHOP mode**) |
| D16 | button to GND | **Play / stop** (norns K3) |
| D17 | button to GND | **Next break** (hold ≥ 0.6 s: **LIVE mode**) |
| D18 | button to GND | **Previous saved beat** (hold: next) |
| D19–D22 | 10 k pots (3V3 / AGND) | **Currently switched off** (`kPots` in `src/main.cpp`). When on: `use_pots = 1` sets tempo, beats, chaos, volume; LIVE mode effects always |
| D23, D24, D25, D28 | 3V3 → LED → resistor (e.g. 330 Ω) → pin (lit when the pin is low; for LEDs to GND set `kQledLow = false`) | **Quarter LEDs:** lit while quarter 1, 2, 3, 4 of the loop plays |
| D1–D6 | micro-SD socket | D1 DAT3, D2 DAT2, D3 DAT1, D4 DAT0, D5 CMD, D6 CLK |
| Audio out L/R | | stereo out, 48 kHz |

Buttons use the internal pull-ups. The SD card needs 10–47 k pull-ups to 3V3
on CMD and DAT0–3; most micro-SD breakout boards already have them. Avoid
boards with 5 V level shifters. The button and pot pins match the
wiring of the `MyProjects/test` project.

**Pots** (`use_pots = 1`):
- **D19 tempo:** fully left keeps the break's tempo, otherwise 60–200 BPM.
- **D20 beats:** 4/8/12/16/24/32/48/64.
- **D21 chaos:** scales the effect probabilities by 0–2×.
- **D22 volume.**

**LED:**
- **Slow blink:** booting or busy (analysing, generating, loading).
- **Fast blink:** error. Check the USB log.
- **Flashes on every beat** while playing, with a longer flash at the start of the loop.
- **Solid:** a beat is loaded but stopped.
- **Off:** nothing loaded.
- **In CHOP mode:** on = current chop selected, off = not selected, with a
  short blip once a second.
- **In LIVE mode:** the beat flashes are inverted: mostly on, briefly off on every beat.

**Quarter LEDs (D23, D24, D25, D28):** the loop is split into 4 quarters (one
bar each in a 16-beat loop). The LED of the quarter being played is lit, so they
step 1 → 2 → 3 → 4 round the loop. All are off when stopped. In LIVE mode, the
LED of a quarter being made again blinks fast.

## CHOP mode: choosing what goes into the beat

This is an addition; the original has no equivalent. Hold **D15** for 0.6 s to
enter. The loop stops so you can hear each chop on its own. All four buttons
change role, and every action is also printed to the USB log
(e.g. `chop 3/12 SNARE 168 ms ON`).

| Button | Short press | Hold ≥ 0.6 s |
|---|---|---|
| D15 | **generate** from the selected chops, leave CHOP mode and play | leave without generating (the old loop resumes) |
| D16 | **play the current chop** (on press, like a pad) | change how the break is chopped: at the hits → 8 → 16 → 32 equal chops |
| D17 | **next chop** (and play it) | previous chop |
| D18 | **current chop on/off** | select all → kicks → kicks+snares → snares → other |

- **Chops:**
  - By default the break is chopped at its hits, as in the original; each chop
    is labelled kick, snare or other.
  - Equal chops are capped at one per 16th note. A 2-bar break gives at most 32.
- **Selection:**
  - At least one chop always stays on.
  - The selection is kept for later beats, including normal D15 presses.
  - Loading another break or changing the chopping resets it to all.
- **Generating:**
  - New beats are built only from the selected chops. The kick and snare
    placement also uses only selected kicks and snares.
  - With every chop selected, the output is exactly what the original makes.
  - With a selection or equal chops, chops are placed on a 16th-note grid
    instead of an 8th-note grid, so short chops keep their length.

## LIVE mode: playing with the loop

This is an addition; the original has no equivalent. Hold **D17** for 0.6 s to
enter it (outside CHOP mode). The loop keeps playing. LIVE mode splits the loop
into 4 quarters; with a 16-beat loop, each quarter is one bar.

| Button (tap) | Pot | Quarter |
|---|---|---|
| D15 | D19 | 1st |
| D16 | D20 | 2nd |
| D17 | D21 | 3rd |
| D18 | D22 | 4th |

**Tap a button** to make its quarter again:
- The new quarter uses new random chops, stutters, reverses and so on.
- Tempo and length stay the same, and the rest of the loop is untouched.
- It carries on from where the quarter starts. The chops continue from that
  point in the break, and kicks and snares fall on the same bar positions as in
  a whole beat.
- Making a quarter takes about a second (the LED and that quarter's LED blink). It swaps in on the
  next beat, crossfaded over 5 ms at the quarter's edges and 2 ms at the swap.

**Turn a pot** (pots are currently switched off, so quarters stay clean; set
`kPots = true` in `src/main.cpp` to use them) to put an effect on its quarter, for every loop until you change
it. Each fifth of the pot's travel is one effect:

| Pot, left → right | Effect on the quarter |
|---|---|
| 0–20% | clean |
| 20–40% | **stutter fill:** the 2nd half repeats the start of the quarter's last beat in 8ths, then 16ths |
| 40–60% | **reverse:** every beat plays backwards |
| 60–80% | **half-time:** half speed (an octave down), restarting every 2 beats |
| 80–100% | **tape stop:** slows to a halt by the end of the quarter |

- Effects change on the next 16th note, and jumps are crossfaded over 2 ms.
- A pot does nothing until you turn it after entering LIVE mode. Every quarter
  starts clean, and a pot that isn't connected can't switch effects on by
  drifting.

Hold a button for ≥ 0.6 s:
- **D15:** generate a whole new beat. The pot effects keep running on it.
- **D16:** play / stop.
- **D17:** leave LIVE mode. The effects switch off.
- **D18:** undo / redo. Swaps back to the loop as it was before the last new
  quarter or beat. Hold again to swap forward.

**Pots:**
- The pots set effects even with `use_pots = 0`.
- In LIVE mode the pots don't set tempo, beats, chaos or volume. Those keep the
  values they had when you entered. New quarters and a D15 hold use them.
- When you leave, the volume jumps to D22's position.

**USB log:**
- Every change is printed, e.g. `live: quarter 2 reverse` and `new quarter 3 in 812 ms`.
- In LIVE mode the log also prints the pot positions (0–100) whenever one
  moves, e.g. `pots D19 3 D20 51 D21 98 D22 60`.
- **Pot noise:** readings are smoothed, and an effect only changes once the pot
  is 4% past its fifth.
- **Pot not responding:** if it stays near 60 while you turn it, its wiper or
  3V3/GND isn't connected.

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
| `chop` | 0 | how CHOP mode starts: 0 = at the hits, 8/16/32 = equal chops |
| `p_deviation … p_snare`, `kick_db`, `snare_db` | norns defaults | effect probabilities, as in the norns PARAMS menu |
| `seed` | 0 | 0 = random; N = reproducible (beat k uses N+k−1) |
| `save` | 1 | write each beat to `/MBB/OUT` |
| `auto_generate`, `auto_play` | 1 | generate and play as soon as a break loads |
| `use_pots` | 0 | read pots D19–D22 |
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
optimisation (`-flto`). A libDaisy built at the default `-O3` won't fit. The
`lto-wrapper: warning: Options to Xassembler do not match` message is harmless:
it only affects assembler listing files.

| | used | of |
|---|---|---|
| FLASH | 125,752 B | 131,072 B (95.9%) |
| SRAM (AXI/D2) | 247,996 B | 524,288 B |
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
use the partly read break; press D17 to try again. The values of N are the same
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
| norns `K3`, `PARAMS > generate` | buttons D16, D15 |
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
- **New controls.** The pot mappings, chaos scaling and `seed` key are new.

## Verification

```sh
make -f Makefile.host test MBB_SOX=/path/to/sox   # engine + player tests
make -f Makefile.host cli                          # build-host/mbb: the generator on a computer
tools/ab_run.sh <original>/lib 7 165 17           # needs lua 5.4, sox, aubio on PATH
```

To try out chop selections on a computer:

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
- LIVE mode:
  - with every quarter clean, the output is identical to the original;
  - changes wait for the next 16th note;
  - every effect reads its quarter at the right speed and direction and leaves
    the rest of the loop unchanged;
  - the quarter being played (for the quarter LEDs) is right on every sample;
  - a new quarter replaces only its quarter, with smooth seams. Starting a
    beat part-way changes its first chop;
  - a beat with pots turned and quarters made again at random has no clicks.
  - The random run is written to `tests/out/live_demo.wav`.

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

A 17–65 beat loop takes 20–130 ms to generate on an Apple M-series Mac. On
the Seed3's 480 MHz Cortex-M7 it should take roughly 0.5–5 s, plus the time to
write it to SD. Audio keeps playing while it generates.

## Limits

- **Breaks:** at most 64 files, each up to 30 s long, `.wav` only.
- **Loops:** up to 40 s at the break's tempo and 45 s at the output tempo.
  `beats` is reduced to fit.
- **Saved beats:** up to 512 in `/MBB/OUT`.
- **Memory:** uses all but about 0.8 MB of SDRAM, and only about 5.2 KB of flash is left (about 0.9 KB with the pots on).
  Keep everything at `-Os`.
