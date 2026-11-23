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

# §8.3/§8.5 busy gate: the Wi-Fi-in-use and player-busy decisions in power.c
# must consult the queue's busy bit directly. power.c compiles no standalone
# unit (it is bound to LVGL and the whole device), so the consult is asserted
# structurally: both decision functions name abs_download_busy() inside their
# own bodies, and the header that declares it is included. The truth table of
# the bit itself is the runtime test above (§10, lifecycle: busy during
# FETCHING/DOWNLOADING/PAUSING/FINALIZING, idle for QUEUED/PAUSED/terminal).
POWER="$ROOT/sonix-player/src/system/device/power.c"
fail_assert() { printf 'audiobookshelf-queue-test.sh: %s\n' "$*" >&2; exit 1; }
grep -q 'src/system/streaming/abs_download.h' "$POWER" || fail_assert "power.c no longer includes abs_download.h"

busy_window() { # file, start-line, end-line
	sed -n "$2,$3p" "$1"
}
WIFI_LINE=$(grep -n 'static bool wifi_in_use(void)' "$POWER" | cut -d: -f1)
BUSY_LINE=$(grep -n 'static bool player_is_busy(bool playing' "$POWER" | cut -d: -f1)
[ -n "$WIFI_LINE" ] || fail_assert "power.c wifi_in_use() moved: busy-gate assertion needs re-anchoring"
[ -n "$BUSY_LINE" ] || fail_assert "power.c player_is_busy() moved: busy-gate assertion needs re-anchoring"
busy_window "$POWER" "$WIFI_LINE" "$((WIFI_LINE + 20))" | grep -q 'abs_download_busy()' ||
	fail_assert "power.c wifi_in_use() must consult abs_download_busy() (§8.3/§8.5)"
busy_window "$POWER" "$BUSY_LINE" "$((BUSY_LINE + 20))" | grep -q 'abs_download_busy()' ||
	fail_assert "power.c player_is_busy() must consult abs_download_busy() (§8.3/§8.5)"

printf 'audiobookshelf-queue-test.sh: power busy-gate consult asserted (wifi_in_use, player_is_busy)\n'
