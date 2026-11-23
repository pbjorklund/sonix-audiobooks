#!/usr/bin/env bash
# Live §8.2/§8.5 host smoke for the ABS download queue's REAL transfer adapter
# (tools/audiobookshelf-queue-transfer-test.c, -DABS_QUEUE_TRANSFER_LIVE).
#
#   * offline first: the same test binary built with the canned-JSON stub pins
#     the metadata gates (explicit >256 tracks, cross-origin contentUrl, path
#     overflow, retention and conflict rules) without any HTTP. The offline
#     stream stubs poison any probe that touches a connection — there are no
#     fake transfer callbacks in either shape.
#
#   * live per alias: the real HTTP stack (http.c + tls.c) against
#     tools/audiobookshelf_mock.py, once per checked-in fixture alias
#     (pinned-3563d49, current-5de038d, eric-v2.38.0, §14.6): server-hash
#     destination vs an independently computed SHA-256, cancelled .part and
#     restart-from-zero, byte-exact transfers read back from the card,
#     engine-driven retention with zero media requests, a finalize-only retry,
#     the §8.1 mid-transfer pause and resume, episode/auth refusals, and
#     token-leak scans over the card, the queue database and the mock's
#     request log — asserted inside the binary AND re-asserted here from the
#     shell, so nothing under test can hide from both at once.
#
# The queue schema/state-machine tests stay in tools/audiobookshelf-queue-test.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
PLAYER="$ROOT/sonix-player"
WORK="${SONIX_QUEUE_TRANSFER_DIR:-$ROOT/build/queue-transfer-smoke}"
ALIASES="pinned-3563d49 current-5de038d eric-v2.38.0"
if [[ -n "${SONIX_ABS_FIXTURE:-}" ]]; then
	case "$SONIX_ABS_FIXTURE" in
		pinned-3563d49|current-5de038d|eric-v2.38.0) ALIASES="$SONIX_ABS_FIXTURE" ;;
		*) printf 'queue-transfer smoke: unknown SONIX_ABS_FIXTURE: %s\n' "$SONIX_ABS_FIXTURE" >&2
			exit 2 ;;
	esac
fi

fail() { printf 'queue-transfer smoke: %s\n' "$*" >&2; exit 1; }
need() { command -v "$1" >/dev/null 2>&1 || fail "missing required command: $1"; }
need gcc
need python3
for cmd in objcopy strip; do need "$cmd"; done

case "$WORK" in "$ROOT"/build/*) rm -rf -- "$WORK" ;; *) fail "unsafe output directory: $WORK" ;; esac
mkdir -p "$WORK/offline"

# ---------------------------------------------------------------------------
# Sanitizer build flags shared by both shapes
# ---------------------------------------------------------------------------
cd "$PLAYER"
FLAGS=(-O1 -g -DHOST_BUILD=1 -D_FILE_OFFSET_BITS=64 -I. -Ilvgl -DLV_CONF_INCLUDE_SIMPLE=1
	-ffunction-sections -fdata-sections -fsanitize=address,undefined -fno-omit-frame-pointer)
LINK=(-Wl,--gc-sections -lm -lpthread -ldl)
SOURCES=(../tools/audiobookshelf-queue-transfer-test.c
	src/system/streaming/audiobookshelf.c src/system/streaming/abs_profiles.c
	src/system/streaming/abs_download.c
	src/system/library/audiobookdb.c src/system/library/library.c
	src/system/core/config.c src/system/core/json.c src/system/core/pathlimits.c
	src/system/core/sha256.c src/system/core/utils.c src/system/db/sqlite3_impl.c
	src/system/core/lang.c)

# ---------------------------------------------------------------------------
# Offline: real production objects, a stubbed buffered API, poisoned streams
# ---------------------------------------------------------------------------
# The offline probes stub http_request()/http_stream_open_ex() but the REAL
# http_url_encode() must be exercised (the request paths are built with it);
# objcopy keeps just that symbol from the compiled http.o so the stub and the
# real object link together (same pattern as tools/audiobookshelf-browse-test).
gcc "${FLAGS[@]}" -c src/system/net/http.c -o "$WORK/http_full.o"
objcopy --keep-global-symbol=http_url_encode "$WORK/http_full.o" "$WORK/http_url.o"
gcc "${FLAGS[@]}" "${SOURCES[@]}" "${LINK[@]}" "$WORK/http_url.o" -o "$WORK/offline/test" \
	|| fail "offline build failed"
mkdir -p "$WORK/offline/run"
"$WORK/offline/test" "$WORK/offline/run" || fail "offline gates failed"

# ---------------------------------------------------------------------------
# Live: the real HTTP stack against the provenance-checked mock, per alias
# ---------------------------------------------------------------------------
gcc "${FLAGS[@]}" -DABS_QUEUE_TRANSFER_LIVE "${SOURCES[@]}" \
	src/system/net/http.c src/system/net/tls.c "${LINK[@]}" -o "$WORK/live" \
	|| fail "live build failed"

MOCK_PID=""
cleanup() {
	[[ -n "$MOCK_PID" ]] && kill -TERM "$MOCK_PID" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

for ALIAS in $ALIASES; do
	ADIR="$WORK/$ALIAS"
	mkdir -p "$ADIR/media"
	# Exact-size media the mock must serve through the fixture's legacy
	# mock-book route; part1 is big enough to make the §8.1 mid-transfer pause
	# deterministic (see the watcher note in the test).
	python3 - "$ADIR/media" <<'PY' || fail "media generation failed"
import os, sys
media = sys.argv[1]
with open(os.path.join(media, "part1.mp3"), "wb") as f:
	left = 16 * 1024 * 1024
	while left > 0:
		n = min(1 << 20, left)
		f.write(os.urandom(n))
		left -= n
with open(os.path.join(media, "part2.mp3"), "wb") as f:
	f.write(os.urandom(4096))
with open(os.path.join(media, "cover.jpg"), "wb") as f:
	f.write(os.urandom(1024))
PY
	TOKEN="trq-$(python3 -c 'import secrets;print(secrets.token_hex(20))')"
	printf '%s' "$TOKEN" >"$ADIR/token.txt"
	python3 "$ROOT/tools/audiobookshelf_mock.py" --fixture "$ALIAS" \
		--media-dir "$ADIR/media" --port-file "$ADIR/mock-port" \
		--progress-file "$ADIR/progress.json" --token "$TOKEN" \
		>"$ADIR/mock.stdout" 2>"$ADIR/mock.stderr" &
	# Wait until the provenance gate accepts and the port file lands.
	for _ in $(seq 1 100); do
		[[ -s "$ADIR/mock-port" ]] && break
		kill -0 $! 2>/dev/null || { cat "$ADIR/mock.stderr" >&2; fail "$ALIAS: mock died"; }
		sleep 0.1
	done
	[[ -s "$ADIR/mock-port" ]] || fail "$ALIAS: mock never opened a port"
	MOCK_PID=$!
	PORT=$(cat "$ADIR/mock-port")
	printf 'http://127.0.0.1:%s' "$PORT" >"$ADIR/server.txt"

	# Fresh profile store per alias; the binary seeds p1 through the real
	# profile surface.
	env SONIX_AUDIOBOOKSHELF_PROFILES="$ADIR/profiles.ini" \
		SONIX_ABS_FIXTURE="$ALIAS" \
		"$WORK/live" "$ADIR" || fail "$ALIAS: live transfer matrix failed"

	# --- shell-level re-assertions the binary could not see -----------------
	# The queue database exists and the finalize wave committed its artifact
	# set: manifest (server- and item-bound), tags/description sidecars, cover,
	# and a DONE row with the acknowledgement flags.
	[[ -f "$ADIR/card/.local/audiobookshelf-downloads.db" ]] || fail "$ALIAS: queue database missing"
	if find "$ADIR/card" -name .audiobookshelf | grep -qv .; then
		fail "$ALIAS: no manifest survived the finalize wave"
	fi
	DEST="$ADIR/card/Audiobooks/Audiobookshelf/abs-$(python3 -c "import hashlib,sys;print(hashlib.sha256(sys.argv[1].encode()).hexdigest()[:12])" \
		"http://127.0.0.1:$PORT")/Test Author/The Mock Journey"
	[[ -s "$DEST/001 - Part One.mp3" ]] || fail "$ALIAS: part one not on the card"
	[[ -s "$DEST/002 - Part Two.mp3" ]] || fail "$ALIAS: part two not on the card"
	cmp -s "$ADIR/media/part1.mp3" "$DEST/001 - Part One.mp3" || fail "$ALIAS: part one bytes differ"
	cmp -s "$ADIR/media/part2.mp3" "$DEST/002 - Part Two.mp3" || fail "$ALIAS: part two bytes differ"
	[[ -s "$DEST/001 - Part One.mp3.tags" ]] || fail "$ALIAS: track-one tags missing"
	grep -aq "abs_item_id=mock-book" "$DEST/001 - Part One.mp3.tags" || fail "$ALIAS: tags unbound"
	grep -aq "series=Fixture Series" "$DEST/001 - Part One.mp3.tags" || fail "$ALIAS: series metadata missing"
	grep -aq "track=2" "$DEST/002 - Part Two.mp3.tags" || fail "$ALIAS: track numbering missing"
	[[ -s "$DEST/001 - Part One.mp3.description" ]] || fail "$ALIAS: description sidecar missing"
	[[ -s "$DEST/cover.jpg" ]] || fail "$ALIAS: cover sidecar missing"
	[[ -s "$DEST/.audiobookshelf" ]] || fail "$ALIAS: manifest missing"
	grep -aq "item_id=mock-book" "$DEST/.audiobookshelf" || fail "$ALIAS: manifest item unbound"
	grep -aq "server=http://127.0.0.1:$PORT" "$DEST/.audiobookshelf" || fail "$ALIAS: manifest server unbound"
	grep -aq "version=1" "$DEST/.audiobookshelf" || fail "$ALIAS: manifest version missing"
	if [[ $(grep -ac "^track=" "$DEST/.audiobookshelf") != 2 ]]; then
		fail "$ALIAS: manifest track lines != 2"
	fi
	# §8.2 step 8: exactly one DONE row, acknowledgement + import flags set.
	python3 - "$ADIR/card/.local/audiobookshelf-downloads.db" <<PY || fail "$ALIAS: DONE row wrong"
import sqlite3, sys
db = sqlite3.connect(sys.argv[1])
state, ack, index_pending = db.execute(
    "SELECT state, ack_reset_done, index_pending FROM DOWNLOAD_JOB WHERE item_id='mock-book'"
    " ORDER BY id DESC LIMIT 1").fetchone()
assert (state, ack, index_pending) == ("DONE", 1, 1), (state, ack, index_pending)
PY
	if find "$ADIR/card" -name "*.part" | grep -q .; then
		fail "$ALIAS: a .part survived the finished run"
	fi

	# Token hygiene, straight from the artifacts the binary produced.
	if LC_ALL=C grep -aqrF "$TOKEN" "$ADIR/card"; then
		fail "$ALIAS: the token leaked into the card or queue database"
	fi
	if LC_ALL=C grep -aqF "$TOKEN" "$ADIR/progress.requests"; then
		fail "$ALIAS: the token leaked into the mock's request log"
	fi
	if LC_ALL=C grep -aqF "Authorization" "$ADIR/progress.requests"; then
		fail "$ALIAS: an Authorization header leaked into the request log"
	fi
	# §8.2: identity fetches use the expanded API; media went through /file/;
	# the finalize fetches the server cover.
	grep -aq "expanded=1&include=progress,authors" "$ADIR/progress.requests" \
		|| fail "$ALIAS: no expanded identity request reached the server"
	[[ $(grep -acF "/api/items/mock-book/file/" "$ADIR/progress.requests") -ge 5 ]] ||
		fail "$ALIAS: fewer media transfers than the phases require"
	grep -aq "/api/items/mock-book/cover" "$ADIR/progress.requests" \
		|| fail "$ALIAS: the finalize never fetched the server cover"

	kill -TERM "$MOCK_PID" 2>/dev/null || true
	wait "$MOCK_PID" 2>/dev/null || true
	MOCK_PID=""
	printf 'queue-transfer smoke: %s passed\n' "$ALIAS"
done

printf 'queue-transfer smoke passed: offline gates + %s live alias/es, byte-exact, token-clean\n' \
	"$(wc -w <<<"$ALIASES")"
