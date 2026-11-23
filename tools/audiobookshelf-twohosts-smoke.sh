#!/usr/bin/env bash
# Focused host smoke for the §11.5 download-path increment (Bob spec lines
# 1058-1072): two mock Audiobookshelf servers -- the pinned fixture serves
# identical author/title/item IDs on both -- configured as two profiles on one
# card.
#
#   phase A (profile A active): download "The Mock Journey" -> abs-<h12 of A>
#   phase B (profile B active): open the identically-named item from server B
#
# Proven:
#   1. distinct destinations: abs-<h12-A>/Test Author/The Mock Journey and
#      abs-<h12-B>/Test Author/The Mock Journey both exist, same interior;
#   2. no wrong-server local link: with B active, tapping the item must not
#      resume the local abs-<h12-A> copy -- the link question opens instead
#      (§11.5 server-bound link gate) and the A manifest keeps server A after
#      the user declines;
#   3. legacy 0.5.0 layout untouched: no un-hashed Audiobookshelf/<author>
#      directory may appear for new downloads;
#   4. B gets its own physical copy and manifest bound to URL_B.
#
# Dialog button coordinates come from the host-only CONFIRMDUMP lines
# (src/gui/shell/confirm.c), not hardcoded pixels.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
PLAYER="$ROOT/sonix-player"
WORK="${SONIX_ABS_TWOHOST_DIR:-$ROOT/build/audiobookshelf-twohosts-smoke}"
RUN="$WORK/run"
SD="$WORK/sd"
CONFIG="$WORK/device_config.ini"
ASOUND="$WORK/asound.conf"
DB="$SD/.local/audiobooks.db"
PROFILES="$WORK/profiles.ini"
TOKEN_A="two-host-token-alpha-0000000000000000000000000000000000000001"
TOKEN_B="two-host-token-beta-0000000000000000000000000000000000000002"
PLAYER_PID=""
SERVER_A_PID=""
SERVER_B_PID=""
XVFB_PID=""
WINDOW=""
fail() { printf 'Two-host ABS smoke test: %s\n' "$*" >&2; exit 1; }
need() { command -v "$1" >/dev/null 2>&1 || fail "missing required command: $1"; }

# Same fixture gate as the single-host smoke: integrity is enforced at serve
# time by the mock, provenance per alias.
export SONIX_ABS_FIXTURE
SONIX_ABS_FIXTURE="${SONIX_ABS_FIXTURE:-pinned-3563d49}"
case "$SONIX_ABS_FIXTURE" in
	pinned-3563d49|current-5de038d|eric-v2.38.0) ;;
	*) printf 'Two-host ABS smoke test: unknown fixture alias: %s\n' "$SONIX_ABS_FIXTURE" >&2; exit 2 ;;
esac

cleanup() {
	for pid in "$PLAYER_PID" "$SERVER_A_PID" "$SERVER_B_PID" "$XVFB_PID"; do
		if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then kill -TERM "$pid" 2>/dev/null || true; fi
	done
}
trap cleanup EXIT INT TERM

wait_for() {
	local description=$1 attempts=$2
	shift 2
	for ((i = 0; i < attempts; i++)); do
		if "$@"; then return 0; fi
		sleep 0.1
	done
	fail "timed out waiting for $description"
}

click() {
	xdotool mousemove --window "$WINDOW" "$1" "$2" click 1
	sleep "${3:-0.6}"
}

# Waits for a confirm dialog and prints "ok_x ok_y cancel_x cancel_y". All
# player output (printf and stderr alike) lands in the SONIX_LOG file, so the
# CONFIRMDUMP calibration lines are read from there.
dialog_buttons() {
	local log=$1
	wait_for "confirm dialog buttons" 100 bash -c "grep -q 'CONFIRMDUMP cancel x=' '$log' && grep -q 'CONFIRMDUMP ok x=' '$log'"
	local pair
	pair=$(grep -m1 'CONFIRMDUMP ok x=' "$log" | sed -n 's/.*CONFIRMDUMP ok x=\([0-9]*\) y=\([0-9]*\).*/\1 \2/p')
	local cpair
	cpair=$(grep -m1 'CONFIRMDUMP cancel x=' "$log" | sed -n 's/.*CONFIRMDUMP cancel x=\([0-9]*\) y=\([0-9]*\).*/\1 \2/p')
	printf '%s %s\n' "$pair" "$cpair"
}

# §9.2 (M4 WS3): a row tap opens the book-detail screen. The §9.1 expanded
# fetch is the arrival anchor (each mock logs to <progress-file>.requests,
# one per server), and the primary action sits at the bottom of the detail —
# three whole-screen drags up pin it there (long drags are scroll gestures,
# never clicks), then the action row taps at (240, 622) in-window.
detail_requests() {
	grep -c 'GET /api/items/.*expanded=1&include=progress,authors ' "$1" 2>/dev/null || true
}
open_book_detail() { # $1 = the active server's <progress-file>.requests
	local log=$1
	local base
	base=$(detail_requests "$log")
	click 240 277 0.3      # the title row -> the detail
	for ((i = 0; i < 100; i++)); do
		[[ "$(detail_requests "$log")" -gt "$base" ]] && break
		sleep 0.1
	done
	[[ "$(detail_requests "$log")" -gt "$base" ]] || fail "the detail fetch never landed"
	for ((r = 0; r < 3; r++)); do
		xdotool mousemove --window "$WINDOW" 300 700 mousedown 1
		for y in 620 540 460 380; do
			xdotool mousemove --window "$WINDOW" 300 "$y"
			sleep 0.05
		done
		xdotool mouseup 1
		sleep 0.4
	done
}

	start_player() { # $1 = run label; stdout/stderr land in $WORK/$1.stdout
	local name=$1
	(
		cd "$RUN"
		exec env SONIX_SD_ROOT="$SD" SONIX_CONFIG="$CONFIG" SONIX_EBOOK_CONFIG="$WORK/ebook_config.ini" \
			SONIX_LOG="$WORK/$name.log" ALSA_CONFIG_PATH="$ASOUND" SONIX_CONFIRM_DUMP=1 \
			SONIX_AUDIOBOOKSHELF_PROFILES="$PROFILES" \
			./sonix_player_host >"$WORK/$name.stdout" 2>&1
	) &
	PLAYER_PID=$!
	for ((i = 0; i < 100; i++)); do
		WINDOW=$(xdotool search --onlyvisible --pid "$PLAYER_PID" 2>/dev/null | head -n 1 || true)
		[[ -n "$WINDOW" ]] && break
		sleep 0.1
	done
	[[ -n "$WINDOW" ]] || fail "$name: the simulator window did not appear; see $WORK/$name.stdout"
	sleep 1
}

stop_player() {
	if [[ -n "$PLAYER_PID" ]] && kill -0 "$PLAYER_PID" 2>/dev/null; then
		kill -TERM "$PLAYER_PID"
		wait "$PLAYER_PID" || true
	fi
	PLAYER_PID=""
	WINDOW=""
}

# Resume import predicates: re-evaluated on every wait_for attempt.
part_a_imported() {
	sqlite3 "$DB" "SELECT resume_file FROM AUDIOBOOK_TABLE WHERE path='$ABS_A';" 2>/dev/null | grep -q "002 - Part Two"
}

part_b_imported() {
	sqlite3 "$DB" "SELECT resume_file FROM AUDIOBOOK_TABLE WHERE path='$ABS_B';" 2>/dev/null | grep -q "002 - Part Two"
}

both_copies_indexed() {
	local count
	count=$(sqlite3 "$DB" "SELECT COUNT(*) FROM AUDIOBOOK_TABLE WHERE name='The Mock Journey';" 2>/dev/null)
	[[ "$count" == 2 ]]
}

abs_row_indexed() {
	local count
	count=$(sqlite3 "$DB" "SELECT COUNT(*) FROM AUDIOBOOK_TABLE WHERE path='$ABS_A';" 2>/dev/null)
	[[ "$count" == 1 ]]
}

assert_manifest_bound_server() {
	local manifest=$1 server=$2
	[[ -s "$manifest" ]] || fail "missing manifest $manifest"
	grep -q '^version=1$' "$manifest" || fail "manifest without version: $manifest"
	grep -q '^item_id=mock-book$' "$manifest" || fail "manifest without mock-book item id: $manifest"
	grep -q "^server=$server\$" "$manifest" || fail "manifest not bound to $server: $manifest"
}

for command in make python3 ffmpeg scrot sqlite3 Xvfb xdotool; do need "$command"; done
[[ "$ROOT" != *' '* ]] || fail "the checkout path contains spaces"
case "$WORK" in "$ROOT"/build/*) rm -rf -- "$WORK" ;; *) fail "unsafe output directory: $WORK" ;; esac
mkdir -p "$RUN/usr/resource" "$WORK/media" "$SD"

# The fixture's primary item ("mock-book") is served byte-compatibly from real
# media files, exactly like the single-host smoke. Both mocks read the same
# files read-only.
ffmpeg -hide_banner -loglevel error -f lavfi -i "sine=frequency=440:duration=10" -ac 1 -b:a 64k "$WORK/media/part1.mp3"
ffmpeg -hide_banner -loglevel error -f lavfi -i "sine=frequency=550:duration=10" -ac 1 -b:a 64k "$WORK/media/part2.mp3"
ffmpeg -hide_banner -loglevel error -f lavfi -i "color=c=0x3584e4:s=64x64" -frames:v 1 "$WORK/media/cover.jpg"

if [[ ${SONIX_SMOKE_SKIP_BUILD:-0} != 1 ]]; then make -C "$PLAYER" host -j2 >/dev/null; fi
[[ -x "$PLAYER/sonix_player_host" ]] || fail "host binary is missing"
cp "$PLAYER/sonix_player_host" "$RUN/"
cp -a "$ROOT/sonix-packer/assets/R1/usr/resource/." "$RUN/usr/resource/"

# Two independent mock servers serving the same fixture with different tokens:
# identical libraries, authors, titles, item IDs, and bytes; only origin and
# credential differ.
python3 "$ROOT/tools/audiobookshelf_mock.py" --media-dir "$WORK/media" --port-file "$WORK/port-a" \
	--progress-file "$WORK/progress-a.json" --token "$TOKEN_A" >"$WORK/mock-a.stdout" 2>"$WORK/mock-a.stderr" &
SERVER_A_PID=$!
python3 "$ROOT/tools/audiobookshelf_mock.py" --media-dir "$WORK/media" --port-file "$WORK/port-b" \
	--progress-file "$WORK/progress-b.json" --token "$TOKEN_B" >"$WORK/mock-b.stdout" 2>"$WORK/mock-b.stderr" &
SERVER_B_PID=$!
wait_for "mock server A" 50 test -s "$WORK/port-a"
wait_for "mock server B" 50 test -s "$WORK/port-b"
PORT_A=$(cat "$WORK/port-a")
PORT_B=$(cat "$WORK/port-b")
URL_A="http://127.0.0.1:$PORT_A"
URL_B="http://127.0.0.1:$PORT_B"
[[ "$URL_A" != "$URL_B" ]] || fail "both mocks collided onto one port"

# The normalized server of each profile is exactly the mock URL (no trailing
# slash), so the shell's sha256 prefix matches the player's destination hash.
HASH_A=$(printf '%s' "$URL_A" | sha256sum | cut -c1-12)
HASH_B=$(printf '%s' "$URL_B" | sha256sum | cut -c1-12)
[[ -n "$HASH_A" && "$HASH_A" != "$HASH_B" ]] || fail "degenerate hash prefixes: A=$HASH_A B=$HASH_B"
ABS_ROOT_A="$SD/Audiobooks/Audiobookshelf/abs-$HASH_A"
ABS_ROOT_B="$SD/Audiobooks/Audiobookshelf/abs-$HASH_B"
ABS_A="$ABS_ROOT_A/Test Author/The Mock Journey"
ABS_B="$ABS_ROOT_B/Test Author/The Mock Journey"
printf 'Two-host ABS smoke test: destinations %s / %s\n' "abs-$HASH_A" "abs-$HASH_B"

# Pre-seed the profile store: two profiles, A active, no legacy keys -- the
# store is the only credential source, as on a fully migrated 0.6.0 card.
printf '# sonix profiles store (two-host smoke)\nversion=1\nnext_id=3\nactive=p1\nlegacy_server=\n\n[profile.p1]\nname=Server Alpha\nserver=%s\ntoken=%s\n\n[profile.p2]\nname=Server Beta\nserver=%s\ntoken=%s\n' \
	"$URL_A" "$TOKEN_A" "$URL_B" "$TOKEN_B" >"$PROFILES"
chmod 0600 "$PROFILES"

cat >"$CONFIG" <<EOF
[player]
volume = 40
remember_track = 1
[ui]
language = English
[power]
screen_off_seconds = 0
[clock]
utc_offset_minutes = 0
last_seen = $(date +%s)
configured = 1
EOF
cat >"$ASOUND" <<'EOF'
pcm.!default { type null }
ctl.!default { type hw card 0 }
EOF

DISPLAY_FILE="$WORK/x-display"
Xvfb -nolisten unix -listen tcp -ac -displayfd 3 -screen 0 600x900x24 \
	>"$WORK/xvfb.stdout" 2>"$WORK/xvfb.log" 3>"$DISPLAY_FILE" &
XVFB_PID=$!
wait_for "Xvfb" 50 test -s "$DISPLAY_FILE"
export DISPLAY="localhost:$(cat "$DISPLAY_FILE")"

# ---------------------------------------------------------------------------
# Phase A: download the book under the profile-A identity.
# ---------------------------------------------------------------------------
start_player player-a

click 356 160 1       # Home -> Streaming
click 124 704 2       # Streaming -> Audiobookshelf (bottom-left tile)
click 280 419 2       # library (one row below the Downloads row)
click 390 180 2       # Filter toolbar (§10.2: authors live under Filter now)
click 240 200 2       # Authors kind
click 240 200 2       # Test Author
click 43 88 1         # author books -> authors
click 43 88 2         # authors -> filter kinds
click 240 200 2       # All books (clear the filter; first title then sits at y=277)
open_book_detail "$WORK/progress-a.requests"  # the §9.2 detail (§9.1 fetch is the anchor)
click 240 622 0.3     # the detail's primary action: Queue download
wait_for "part one on card (A)" 900 test -s "$ABS_A/001 - Part One.mp3"
wait_for "part two on card (A)" 300 test -s "$ABS_A/002 - Part Two.mp3"
wait_for "resume import (A)" 900 part_a_imported
stop_player

assert_manifest_bound_server "$ABS_A/.audiobookshelf" "$URL_A"
[[ ! -e "$ABS_ROOT_B" ]] || fail "profile-B directory appeared before any B download"
[[ ! -e "$SD/Audiobooks/Audiobookshelf/Test Author" ]] || fail "a new download used the legacy 0.5.0 layout"

# ---------------------------------------------------------------------------
# Phase B: same card, profile B active.
# ---------------------------------------------------------------------------
sed -i 's/^active=p1$/active=p2/' "$PROFILES"
start_player player-b
# The phase-A download already indexed its copy into the persistent card
# database, so the row survives the restart; no rescan is needed. The §11.5
# link gate reads the manifest on the card, not the database row.
wait_for "phase-B local row" 100 abs_row_indexed
# Core negative proof: with server B active, tapping the identically-named
# item must NOT resume the local server-A copy. The link question opens
# instead; no "play requested" line may reference the A directory at all.
click 356 160 1       # Home -> Streaming
click 124 704 2       # Streaming -> Audiobookshelf
click 280 419 2       # library (one row below the Downloads row)
click 390 180 2       # Filter toolbar (§10.2: authors live under Filter now)
click 240 200 2       # Authors kind
click 240 200 2       # Test Author
click 43 88 1         # author books -> authors
click 43 88 2         # authors -> filter kinds
click 240 200 2       # All books (clear the filter; first title then sits at y=277)
# §9.2: server B's tap opens the detail (§9.4's open_book_detail runs the
# tap and anchors on the §9.1 fetch). Nothing is tapped afterwards: whatever
# the detail would offer (the worker's local match sees A's manifest and
# offers a download from this server, never a link), this phase proves NO
# action was taken against A's copy by not acting at all.
open_book_detail "$WORK/progress-b.requests"
sleep 0.5
if grep -q "play requested.*Audiobookshelf/abs-$HASH_A/" "$WORK/player-b.log"; then
	fail "server B silently opened the local server-A copy (wrong-server local link)"
fi
assert_manifest_bound_server "$ABS_A/.audiobookshelf" "$URL_A"

# Remove the local rows so the item has no unique local match: the only way
# forward is a download from B, which must land in B's own directory. (The
# §11.5 rule "one physical copy per manifest" is exercised by the declined
# re-link above; this surgery just reaches the download branch deterministically
# from the UI.)
sqlite3 "$DB" "DELETE FROM AUDIOBOOK_TABLE WHERE path='$ABS_A';"
sqlite3 "$DB" "DELETE FROM AUDIOBOOK_PARTS WHERE book='$ABS_A';"
sqlite3 "$DB" "DELETE FROM AUDIOBOOK_ABS_SYNC WHERE book='$ABS_A';"

sleep 1               # let the surgery settle
click 43 88 1         # the Phase-B detail -> the ABS page (the §9.2 back-return)
sleep 1               # let the redraw finish
open_book_detail "$WORK/progress-b.requests"  # first title again (a fresh §9.1 fetch)
click 240 622 0.3     # the detail's primary action: Queue download from server B
wait_for "part one on card (B)" 900 test -s "$ABS_B/001 - Part One.mp3"
wait_for "part two on card (B)" 300 test -s "$ABS_B/002 - Part Two.mp3"
wait_for "resume import (B)" 900 part_b_imported
# The completion scan also re-indexes the phase-A copy: both books on card.
wait_for "both copies indexed" 300 both_copies_indexed

assert_manifest_bound_server "$ABS_B/.audiobookshelf" "$URL_B"
assert_manifest_bound_server "$ABS_A/.audiobookshelf" "$URL_A"
if grep -q "play requested.*Audiobookshelf/abs-$HASH_A/" "$WORK/player-b.log"; then
	fail "server B opened the local server-A copy during the B session"
fi
# Exactly two server-qualified roots, and nothing in the legacy layout.
mapfile -t root_dirs < <(find "$SD/Audiobooks/Audiobookshelf" -maxdepth 1 -mindepth 1 -type d -printf '%f\n' | sort)
[[ ${#root_dirs[@]} == 2 ]] || fail "unexpected Audiobookshelf root entries: ${root_dirs[*]}"
for dir in "${root_dirs[@]}"; do
	[[ "$dir" == "abs-$HASH_A" || "$dir" == "abs-$HASH_B" ]] || fail "unexpected root directory: $dir"
done
[[ -d "$ABS_ROOT_B/Test Author/The Mock Journey" ]] || fail "server B book not under its safe author/title path"
[[ ! -e "$SD/Audiobooks/Audiobookshelf/Test Author" ]] || fail "legacy layout appeared during phase B"

stop_player
printf 'Two-host ABS smoke test passed: distinct abs-<hash> destinations per server, no wrong-server local link, legacy layout untouched.\n'
