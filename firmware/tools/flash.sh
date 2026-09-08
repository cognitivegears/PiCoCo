#!/bin/sh
# Copies the UF2 to a Pico 2 in BOOTSEL mode (hold BOOTSEL while plugging in).
set -e
UF2=${1:-build-pico/picoco.uf2}
VOL=/Volumes/RP2350
[ -d "$VOL" ] || { echo "no $VOL mounted: hold BOOTSEL while plugging the Pico in"; exit 1; }
cp "$UF2" "$VOL/" && echo "flashed $UF2"
