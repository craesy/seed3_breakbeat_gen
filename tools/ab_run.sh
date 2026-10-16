#!/bin/sh
# End-to-end A/B: run the ORIGINAL dnb.lua (with the port's PCG32 RNG
# injected) and the port's CLI on the same seed and settings, then compare.
# Needs lua 5.4 plus sox and aubioonset on PATH, and `make -f Makefile.host cli`.
#   tools/ab_run.sh <makebreakbeat/lib> <seed> <tempo> <beats> [probability args...]
# e.g. tools/ab_run.sh ../makebreakbeat/lib 7 165 17 --stutter 20 --pitch 5
set -e
# (dnb.lua's -i parser joins following words up to the next "-" option, so
#  -i must be followed by another option.)
# Dither makes the original non-deterministic run to run (it breaks ties in
# the splice search at random), so turn it off for a repeatable comparison.
export SOX_OPTS="${SOX_OPTS--D}"
LIB=$1; SEED=$2; TEMPO=$3; BEATS=$4; shift 4
# dnb.lua's own defaults differ from the norns menu (and the port), so pass
# the norns defaults to both sides; extra arguments override them.
NORNS="--deviation 10 --reverse 10 --stutter 20 --pitch 5 --trunc 2 --half 1 --reverb 5 --stretch 2 --kick 50 --snare 20 --snare-mix -6 --kick-mix -6"
HERE=$(cd "$(dirname "$0")/.." && pwd)
OUT=${AB_OUT:-/tmp/mbb_ab}; mkdir -p "$OUT"
${LUA:-lua} "$HERE/tools/ab_original.lua" "$LIB/dnb.lua" "$SEED" --no-logo \
  --snare-file "$LIB/snare.wav" --kick-file "$LIB/kick.wav" -t "$TEMPO" -b "$BEATS" \
  $NORNS "$@" -i "$LIB/amen_resampled.wav" -o "$OUT/original.wav" > "$OUT/original.log" 2>&1
"$HERE/build-host/mbb" --seed "$SEED" -t "$TEMPO" -b "$BEATS" --kick-file "$LIB/kick.wav" \
  --snare-file "$LIB/snare.wav" $NORNS "$@" -i "$LIB/amen_resampled.wav" -o "$OUT/port.wav" > /dev/null
python3 "$HERE/tools/wavcmp.py" "$OUT/original.wav" "$OUT/port.wav" | sed -n '1,3p'
