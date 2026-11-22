#!/usr/bin/env bash
# The Japanese input modules' pure host tests: kana.c and jpdict.c, with the
# dictionary they ship. No LVGL, no audio, no card; both run under the
# address/undefined sanitizers, because the point of the boundary tests is
# that an out-of-bounds byte has a place to be caught.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
WORK=$(mktemp -d)
trap 'rm -rf -- "$WORK"' EXIT
cd "$ROOT/sonix-player"
FLAGS=(-O1 -g -DHOST_BUILD=1 -D_FILE_OFFSET_BITS=64 -I. -ffunction-sections -fdata-sections
       -fsanitize=address,undefined -fno-omit-frame-pointer -Wl,--gc-sections)
gcc "${FLAGS[@]}" "$ROOT/tools/kana-test.c" src/system/input/kana.c -o "$WORK/kana-test"
gcc "${FLAGS[@]}" "$ROOT/tools/jpdict-test.c" src/system/input/jpdict.c src/system/input/jpdict_data.c \
    -o "$WORK/jpdict-test"
"$WORK/kana-test"
"$WORK/jpdict-test"
