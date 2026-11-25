#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
player="$root/sonix-player"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# The queue's per-track fetch is injected by the test, so no HTTP stack is
# linked: the http/tidal symbols tidalcache references are satisfied by the
# stubs defined at the bottom of the test file itself.
gcc -Wall -Wextra -Werror -Wno-unused-parameter -DHOST_BUILD=1 \
	-I"$player" "$root/tools/tidalkeep-test.c" \
	"$player/src/system/streaming/tidalkeep.c" \
	"$player/src/system/streaming/tidalcache.c" \
	"$player/src/system/streaming/streamturn.c" \
	"$player/src/system/decode/growfile.c" \
	"$player/src/system/library/mp4flac.c" \
	"$player/src/system/library/jpeginfo.c" \
	"$player/src/system/core/utils.c" \
	-Wl,--wrap=fopen -lpthread -o "$work/tidalkeep-test"
mkdir -p "$work/cache"
"$work/tidalkeep-test" "$work/cache"

gcc -Wall -Wextra -Werror -I"$player" "$root/tools/tidalsaved-test.c" \
	"$player/src/system/streaming/tidalsaved.c" -lsqlite3 -o "$work/tidalsaved-test"
mkdir "$work/saved"
"$work/tidalsaved-test" "$work/saved"

# Every string the download UI shows must exist in every language file: a
# missing key would be displayed as its own tag.
"$root/tools/tidal-language-test.sh" "$@"
