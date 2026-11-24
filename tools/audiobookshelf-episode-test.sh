#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
WORK=$(mktemp -d)
trap 'rm -rf -- "$WORK"' EXIT
cd "$ROOT/sonix-player"
gcc -O1 -g -Wall -Werror -fsanitize=address,undefined -I. \
    "$ROOT/tools/audiobookshelf-episode-test.c" src/system/streaming/abs_episode_progress.c \
    src/system/streaming/abs_history.c src/system/streaming/abs_show_reader.c \
    src/system/core/json.c -lsqlite3 -o "$WORK/test"
"$WORK/test" "$WORK/episode.db"
