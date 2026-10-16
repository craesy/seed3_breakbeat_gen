#!/bin/sh
# Copies the default /MBB folder (breaks, kick/snare, config) to an SD card.
# Existing files on the card (your config, saved beats) are never overwritten.
#   tools/prepare_sd.sh /Volumes/SDCARD
set -e
DEST=${1:?usage: prepare_sd.sh <sd card mount point, e.g. /Volumes/SDCARD>}
SRC=$(cd "$(dirname "$0")/../sdcard" && pwd)
[ -d "$DEST" ] || { echo "no such directory: $DEST" >&2; exit 1; }
mkdir -p "$DEST/MBB/SAMPLES" "$DEST/MBB/OUT"
for f in config.txt kick.wav snare.wav SAMPLES/amen_resampled.wav; do
    if [ -e "$DEST/MBB/$f" ]; then
        echo "keep  MBB/$f"
    else
        cp "$SRC/MBB/$f" "$DEST/MBB/$f" && echo "copy  MBB/$f"
    fi
done
# macOS litters FAT cards with ._* files; the firmware skips them, but tidy up.
command -v dot_clean >/dev/null 2>&1 && dot_clean -m "$DEST" 2>/dev/null || true
echo "done - put more 48 kHz .wav breaks into $DEST/MBB/SAMPLES"
