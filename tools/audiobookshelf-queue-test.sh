#!/usr/bin/env bash
# WS2 download-queue foundation tests (bob §8.1–8.2 / §8.5), ASan+UBSan.
# Offline: the media surface is fake transfer callbacks (tools/
# audiobookshelf-queue-test.c); no HTTP, no GUI, no mock server.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
WORK=$(mktemp -d)
trap 'rm -rf -- "$WORK"' EXIT
cd "$ROOT/sonix-player"
gcc -O1 -g -DHOST_BUILD=1 -D_FILE_OFFSET_BITS=64 -I. -Ilvgl -DLV_CONF_INCLUDE_SIMPLE=1 \
    -ffunction-sections -fdata-sections -fsanitize=address,undefined -fno-omit-frame-pointer \
    "$ROOT/tools/audiobookshelf-queue-test.c" src/system/streaming/abs_download.c \
    src/system/core/pathlimits.c src/system/core/utils.c src/system/db/sqlite3_impl.c \
    -Wl,--gc-sections -lpthread -lm -ldl -o "$WORK/test"
"$WORK/test" "$WORK/card"
