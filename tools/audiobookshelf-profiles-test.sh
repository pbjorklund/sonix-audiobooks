#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
WORK=$(mktemp -d)
trap 'rm -rf -- "$WORK"' EXIT
cd "$ROOT/sonix-player"
gcc -O1 -g -DHOST_BUILD=1 -D_FILE_OFFSET_BITS=64 -I. -Ilvgl -DLV_CONF_INCLUDE_SIMPLE=1 \
    -ffunction-sections -fdata-sections -fsanitize=address,undefined -fno-omit-frame-pointer \
    "$ROOT/tools/audiobookshelf-profiles-test.c" src/system/streaming/abs_profiles.c \
    src/system/core/pathlimits.c src/system/core/config.c \
    -Wl,--gc-sections -lm -lpthread -ldl -o "$WORK/test"
"$WORK/test" "$WORK"
