#!/usr/bin/env bash
# §14.7: SONIX_SANITIZER selects the sanitizer build. The functional default is
# address+undefined; thread is a DISTINCT TSan build (never combined with
# ASan). An unknown mode fails the run.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
SAN_MODE="${SONIX_SANITIZER:-address,undefined}"
case "$SAN_MODE" in
	address,undefined) SANITIZERS=(-fsanitize=address,undefined -fno-omit-frame-pointer) ;;
	thread) SANITIZERS=(-fsanitize=thread -fno-omit-frame-pointer) ;;
	*) printf 'audiobookshelf-outbox-test.sh: unknown SONIX_SANITIZER: %s (address,undefined|thread)\n' "$SAN_MODE" >&2
		exit 2 ;;
esac
WORK=$(mktemp -d)
trap 'rm -rf -- "$WORK"' EXIT
cd "$ROOT/sonix-player"
gcc -O1 -g -DHOST_BUILD=1 -D_FILE_OFFSET_BITS=64 -I. -Ilvgl -DLV_CONF_INCLUDE_SIMPLE=1 \
    -ffunction-sections -fdata-sections "${SANITIZERS[@]}" \
    "$ROOT/tools/audiobookshelf-outbox-test.c" src/system/library/audiobookdb.c \
    src/system/core/utils.c src/system/db/sqlite3_impl.c -Wl,--gc-sections -lm -lpthread -ldl -o "$WORK/test"
"$WORK/test" "$WORK/card-one.db" "$WORK/card-two.db"
