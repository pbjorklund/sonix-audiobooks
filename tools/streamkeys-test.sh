#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/build_gen" "$work/usr/resource/sonix/components"
printf '#define STREAMKEYS_KEY_LEN 0\n' > "$work/build_gen/streamkeys_key.h"
cd "$work"
player="$root/sonix-player"
gcc -Wall -Wextra -Werror -Wno-unused-parameter -DHOST_BUILD \
    "-DSTREAMKEYS_USERDATA_PATH=\"$work/userdata.ini\"" -I"$work" -I"$player" \
    "$root/tools/streamkeys-test.c" "$player/src/system/streaming/streamkeys.c" \
    "$player/src/system/core/config.c" -o "$work/test"
fixture() { printf '[tidal]\nclient_id = %s\nclient_secret = fixture-secret\n' "$2" > "$1"; }
firmware=usr/resource/sonix/components/streaming-keys.ini
./test none unused unused
fixture "$firmware" firmware
./test fallback "$firmware" firmware
fixture "$work/userdata.ini" userdata
./test userdata "$work/userdata.ini" userdata
fixture "$work/custom.ini" explicit
./test override "$work/custom.ini" explicit
printf 'Streaming keys: missing, firmware fallback, userdata, and explicit override passed.\n'
