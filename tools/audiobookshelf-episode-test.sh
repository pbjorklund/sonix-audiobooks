#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
WORK=$(mktemp -d)
trap 'rm -rf -- "$WORK"' EXIT
cd "$ROOT/sonix-player"
gcc -O1 -g -Wall -Werror -fsanitize=address,undefined -I. \
    "$ROOT/tools/audiobookshelf-episode-test.c" src/system/streaming/abs_episode_progress.c \
    src/system/streaming/abs_history.c src/system/streaming/abs_show_reader.c \
    src/system/streaming/abs_episode_card.c src/system/streaming/abs_episode_playback.c src/system/core/sha256.c \
    src/system/core/json.c -lsqlite3 -pthread -lm -o "$WORK/test"
"$WORK/test" "$WORK/episode.db"
gcc -O1 -g -Wall -Werror -fsanitize=address,undefined -ffunction-sections -fdata-sections -I. \
    "$ROOT/tools/mp4-progressive-test.c" src/system/decode/mp4.c src/system/decode/growfile.c \
    -Wl,--gc-sections -pthread -o "$WORK/mp4-test"
"$WORK/mp4-test" "$WORK/sample.m4a"
gcc -O1 -g -Wall -Werror -fsanitize=address,undefined -I. \
    "$ROOT/tools/speed-handoff-test.c" src/system/audio/speed.c -o "$WORK/speed-test"
"$WORK/speed-test"
