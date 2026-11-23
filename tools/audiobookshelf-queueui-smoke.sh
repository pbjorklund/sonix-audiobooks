#!/usr/bin/env bash
# The download-queue GUI smoke (spec §8.1, §8.5 lines 579/587/594).
#
# Drives the real host simulator (Xvfb + xdotool) against the provenance
# mock, and proves the queue screen end to end:
#
#   * structural asserts first: the queue GUI file stays free of HTTP/SQLite,
#     polls the coalesced snapshot at 4 Hz, and the browse page's download
#     action enqueues instead of calling the legacy synchronous downloader;
#   * enqueue via begin_download() -> the queue row appears;
#   * pause the ACTIVE job from the row menu (the throttled file-2 transfer
#     leaves a deterministic window): PAUSED + .part retained + completed
#     audio retained;
#   * Remove the paused job: the job-owned .part goes; completed media stays
#     (§8.1 line 466);
#   * fresh enqueue -> worker -> durable queue DONE (ack_reset_done=1), the
#     per-job-completion follow-up scan, and the import coordinator's resume;
#   * duplicate enqueue of an unfinished row dedupes (§8.1): same row count;
#   * the wrong-length fixture row parks in WAITING_NETWORK per §8.4: cancel
#     from the row menu, then retry (re-arms, fails honestly again);
#   * clear-finished frees the terminal rows; media and imported progress
#     survive it; the non-terminal row does not go; Remove then empties the
#     queue completely;
#   * the coalescing proof: on screen close the log line reports the poll
#     ticks (4 Hz) and the repaints (only on change).
#
# The legacy single-download behavior (one-item queue, same file/sidecar/
# manifest formats) keeps its own host smoke (audiobookshelf-host-smoke.sh):
# that suite exercises the same enqueue path in its download mode now.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
PLAYER="$ROOT/sonix-player"
WORK="${SONIX_ABS_QUEUEUI_DIR:-$ROOT/build/audiobookshelf-queueui}"
RUN="$WORK/run"
SD="$WORK/sd"
CONFIG="$WORK/device_config.ini"
ASOUND="$WORK/asound.conf"
DB="$SD/.local/audiobooks.db"
QDB="$SD/.local/audiobookshelf-downloads.db"
PROFILES="$WORK/audiobookshelf-profiles.ini"
QUEUE_ROOT="$SD/Audiobooks/Audiobookshelf"
PORT_FILE="$WORK/mock-port"
PROGRESS_FILE="$WORK/progress.json"
TOKEN=$(printf 't%.0s' {1..208})
PLAYER_PID=""
SERVER_PID=""
XVFB_PID=""
WINDOW=""

fail() { printf 'Audiobookshelf queue smoke: %s\n' "$*" >&2; exit 1; }
need() { command -v "$1" >/dev/null 2>&1 || fail "missing required command: $1"; }

# --------------------------------------------------------------- structure --
# §8.5 line 579: the queue screen holds no HTTP and no SQLite; the coalesced
# snapshot is its only state read. line 587: begin_download() enqueues; no UI
# path calls the legacy synchronous downloader; the singleton download state
# and its modal are gone. line 446: 4 Hz (250 ms) cadence, no event per read.
QUEUEGUI="$PLAYER/src/gui/streaming/audiobookshelfqueue.c"
PAGE="$PLAYER/src/gui/streaming/audiobookshelfpage.c"
INIFILE="$ROOT/sonix-packer/assets/R1/usr/resource/sonix/language/English.ini"
fail_s() { fail "structure: $*"; }
[[ -f "$QUEUEGUI" ]] || fail_s "audiobookshelfqueue.c is missing"
if grep -qE 'gui_post|http_|sqlite3_|audiobookshelf_download\(|audiobookdb_' "$QUEUEGUI"; then
	fail_s "the queue GUI file reached into HTTP/SQLite/engine internals (§8.5 line 579 boundary)"
fi
grep -q 'abs_list_create' "$QUEUEGUI" || fail_s "the queue screen must render through abslist (pooled rows, §8.5 line 893)"
grep -q '#define ABS_QUEUE_POLL_MS 250' "$QUEUEGUI" || fail_s "the 4 Hz snapshot-poll constant drifted (§8.1 line 446)"
[[ $(grep -c 'abs_download_snapshot(' "$QUEUEGUI") -eq 1 ]] || fail_s "exactly one snapshot poll site expected"
grep -q 'abs_download_enqueue' "$QUEUEGUI" &&
	fail_s "enqueuing belongs to the browse page, not the queue screen"
for key in audiobookshelf_downloads audiobookshelf_queue_none audiobookshelf_queue_pause \
	audiobookshelf_queue_resume audiobookshelf_queue_cancel audiobookshelf_queue_remove \
	audiobookshelf_queue_retry audiobookshelf_queue_clear_finished \
	audiobookshelf_queue_file_of audiobookshelf_queue_storage_paused; do
	grep -q "^$key = " "$INIFILE" || fail_s "English.ini lost queue key $key"
done
BD_LINE=$(grep -n 'static void begin_download' "$PAGE" | cut -d: -f1)
[[ -n "$BD_LINE" ]] || fail_s "begin_download() moved: enqueue assertion needs re-anchoring"
sed -n "$((BD_LINE + 1)),$((BD_LINE + 70))p" "$PAGE" | grep -q 'abs_download_enqueue_book(' ||
	fail_s "begin_download() no longer enqueues (§8.5 line 587)"
if grep -qE 'resume_import_cb|download_progress\(|gui_modal_progress|audiobookshelf_download\(' "$PAGE"; then
	fail_s "the page still carries the legacy sync-download/modal path (§8.5 line 587)"
fi
if grep -q 'downloaded_resume_file' "$PAGE"; then
	fail_s "the page still owns downloaded_* singletons (§8.5 line 587)"
fi
grep -q 'audiobookshelfqueue_open' "$PAGE" || fail_s "the page never opens the queue"
grep -q 'audiobookdb_scan_start' "$PAGE" || fail_s "no per-job-completion follow-up scan in the page (§8.5 line 587)"

# §9.2 (M4 WS3): the tapped row opens the detail; the whole-catalog local
# scan is the worker's job, never the click handler's. The detail screen
# itself holds no HTTP/SQLite and no queue commands; its actions run through
# the page's delegate, and the model is released on unload.
DETAILGUI="$PLAYER/src/gui/streaming/audiobookshelfdetail.c"
[[ -f "$DETAILGUI" ]] || fail_s "audiobookshelfdetail.c is missing (§9.4)"
if grep -qE 'http_|sqlite3_|abs_download_|audiobookdb_|audiobookshelf_book\(|unique_local_match' "$DETAILGUI"; then
	fail_s "the detail screen reached into HTTP/SQLite/engine internals (§9.2)"
fi
# The row tap opens the detail and runs no catalog scan itself: the first
# lines after the handler hold the open call, and no unique_local_match
# anywhere below the handler's line is left in it (the scan moved to JOB_DETAIL).
ICLICK_LINE=$(grep -n 'static void item_click_cb' "$PAGE" | tail -1 | cut -d: -f1)
[[ -n "$ICLICK_LINE" ]] || fail_s "item_click_cb moved: the §9.2 assertions need re-anchoring"
sed -n "$((ICLICK_LINE)),$((ICLICK_LINE + 30))p" "$PAGE" | grep -q 'unique_local_match' &&
	fail_s "the row tap still runs the whole-catalog local match on the LVGL thread (§9.2)"
sed -n "$((ICLICK_LINE)),$((ICLICK_LINE + 30))p" "$PAGE" | grep -q 'audiobookshelfdetail_open' ||
	fail_s "the row tap no longer opens the detail (§9.2)"
grep -q 'JOB_DETAIL' "$PAGE" || fail_s "the page has no JOB_DETAIL (§9.2)"
grep -q 'audiobookshelfdetail_receive' "$PAGE" || fail_s "the worker never hands the model to the detail screen"
grep -q 'audiobookshelf_detail_free(done.payload)' "$PAGE" ||
	fail_s "a stale detail completion is not freed in the GUI consumer (§9.3)"
for key in audiobookshelf_detail audiobookshelf_detail_loading audiobookshelf_detail_failed \
	audiobookshelf_detail_summary audiobookshelf_detail_chapters audiobookshelf_detail_queue \
	audiobookshelf_detail_play_local audiobookshelf_detail_chapters_overflow; do
	grep -q "^$key = " "$INIFILE" || fail_s "English.ini lost detail key $key"
done

# ------------------------------------------------------------- environment --
for command in ffmpeg make python3 scrot sqlite3 Xvfb xdotool; do need "$command"; done
[[ "$ROOT" != *' '* ]] || fail "the checkout path contains spaces"
case "$WORK" in "$ROOT"/build/*) rm -rf -- "$WORK" ;; *) fail "unsafe output directory: $WORK" ;; esac
mkdir -p "$RUN/usr/resource" "$WORK/media" "$SD"

if [[ ${SONIX_SMOKE_SKIP_BUILD:-0} != 1 ]]; then make -C "$PLAYER" host -j2; fi
[[ -x "$PLAYER/sonix_player_host" ]] || fail "host binary is missing"
cp "$PLAYER/sonix_player_host" "$RUN/"
cp -a "$ROOT/sonix-packer/assets/R1/usr/resource/." "$RUN/usr/resource/"

# §14.6: this queue flow is pinned to the legacy real-media fixture (the
# "mock-book" item serves the real part1/part2 bytes, host-smoke shape). The
# Fixture Search Range row comes from the same fixture's synthetic body, which
# deliberately mismatches its advertised size: the committed wrong-length
# guard refuses it into WAITING_NETWORK (§8.4) -- the deterministic
# cancel/retry fixture. The mock enforces full provenance at load either way.
export SONIX_ABS_FIXTURE
SONIX_ABS_FIXTURE="${SONIX_ABS_FIXTURE:-pinned-3563d49}"
case "$SONIX_ABS_FIXTURE" in
	pinned-3563d49) ;;
	*) fail "the queue smoke is pinned to pinned-3563d49 (got: $SONIX_ABS_FIXTURE)" ;;
esac

cleanup() {
	for pid in "$PLAYER_PID" "$SERVER_PID" "$XVFB_PID"; do
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
	sleep "${3:-0.7}"
}

# The title-bar back button pops pure screen history (the ABS page's own
# back-guard belongs to the swipe gesture, and a tap may never be relied on
# to know which view the page holds underneath). The navigation rule of this
# smoke: to reach the ABS library view from anywhere, walk back to Streaming
# with the title bar and re-enter through the Audiobookshelf tile -- the page
# reload fetches the libraries and then draws the library view. The reload is
# observable in the mock's request log, so the next tap waits for it.
REQUESTS="$WORK/progress.requests"
libraries_requests() {
	local n
	n=$(grep -c "GET /api/libraries " "$REQUESTS" 2>/dev/null) || true
	[[ "$n" =~ ^[0-9]+$ ]] || n=0
	printf '%s' "$n"
}
enter_abs_libraries() {
	local base
	base=$(libraries_requests)
	click 124 660 0.4                   # the Audiobookshelf tile from the Streaming grid
	for ((i = 0; i < 100; i++)); do
		[[ "$(libraries_requests)" -gt "$base" ]] && break
		sleep 0.1
	done
	sleep 1                             # the fetch published -> draw_libraries
}
reopen_abs_libraries() {
	# The walk is one screen longer than the §8.5 shape: the §9.2 detail sits
	# between the queue and the ABS page. The page's own back-guard turns one
	# chevron into the in-page books->libraries step once the detail-return
	# redraw landed the books view (§9.2), so the walk's chevron count is the
	# call site's knowledge: a session that never opened a detail (P1) needs
	# three chevrons to reach Streaming, every later one needs four.
	local chevrons=${1:-4}
	local i
	for ((i = 0; i < chevrons; i++)); do
		click 43 88 0.8
	done
	enter_abs_libraries
}
# §9.2 item activation: the tap opens the detail; the §11.5 decision tree
# (the whole-catalog local match) runs in the worker behind it, and the
# "Queue download" action enqueues. The detail's action rows sit at the
# bottom of the page (below the description, §9.2's sketch order), so the
# flow scrolls to the last view before tapping the primary action.
detail_fetches() {
	grep -c 'GET /api/items/.*expanded=1&include=progress,authors ' "$REQUESTS" 2>/dev/null || true
}
scroll_actions_visible() {
	# Three whole-screen drags up pin the page at the bottom, where the
	# primary action row is the second-to-last row: (240, 672) in a 480x800
	# window. The long drag is never a click.
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
open_book_detail_and_queue() {
	local base
	base=$(detail_fetches)
	click 240 277 1.1                   # the book row -> the §9.2 detail
	for ((i = 0; i < 100; i++)); do
		[[ "$(detail_fetches)" -gt "$base" ]] && break
		sleep 0.1
	done
	[[ "$(detail_fetches)" -gt "$base" ]] || fail "the detail fetch never landed"
	scroll_actions_visible
	click 240 622 0.4                   # the primary action (Queue download)
}

# The queue database is the durable intent; the UI assertions below read it,
# never screen pixels. These run as wait_for predicates (no subshell).
sql() { sqlite3 "$QDB" "$1" 2>/dev/null || true; }
job_count() { sql "SELECT count(*) FROM DOWNLOAD_JOB;"; }
state_at() { sql "SELECT state FROM DOWNLOAD_JOB ORDER BY seq LIMIT 1 OFFSET $(( $1 - 1 ));"; }
is_state() { [[ "$(state_at "$1")" == "$2" ]]; }
ack_committed() { [[ "$(sql "SELECT ack_reset_done FROM DOWNLOAD_JOB WHERE state='DONE';")" == "1" ]]; }
terminal_clear() { [[ "$(sql "SELECT count(*) FROM DOWNLOAD_JOB WHERE state='DONE' OR state='FAILED' OR state='CANCELLED';")" == "0" ]]; }
count_is() { [[ "$(job_count)" == "$1" ]]; }
resume_30s() { sqlite3 "$DB" "SELECT resume_pos FROM AUDIOBOOK_TABLE WHERE name='The Mock Journey';" 2>/dev/null |
	awk '{ exit !($1 > 2.5 && $1 < 3.5) }'; }
# one path from a glob (ls, evaluated outside [[ ]] where globs do not expand)
one_file() { find "$(dirname "$1")" -maxdepth 4 -name "$(basename "$1")" 2>/dev/null | head -n 1; }
shot() { scrot -o "$1" 2>/dev/null || true; }

ffmpeg -hide_banner -loglevel error -f lavfi -i "sine=frequency=440:duration=10" -ac 1 -b:a 64k "$WORK/media/part1.mp3"
ffmpeg -hide_banner -loglevel error -f lavfi -i "sine=frequency=550:duration=10" -ac 1 -b:a 64k "$WORK/media/part2.mp3"
ffmpeg -hide_banner -loglevel error -f lavfi -i "color=c=0x3584e4:s=64x64" -frames:v 1 "$WORK/media/cover.jpg"

# The provenance mock; --throttle 2:3000 keeps the second file streaming for
# ~15 s (5 x 16 KiB chunks), the deterministic window the pause lands in
# (§8.5 line 595: slow responses).
python3 "$ROOT/tools/audiobookshelf_mock.py" --media-dir "$WORK/media" --port-file "$PORT_FILE" \
	--progress-file "$PROGRESS_FILE" --token "$TOKEN" --throttle 2:3000 \
	>"$WORK/mock.stdout" 2>"$WORK/mock.stderr" &
SERVER_PID=$!
wait_for "mock server" 50 test -s "$PORT_FILE"
PORT=$(cat "$PORT_FILE")

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
[audiobookshelf]
server = http://127.0.0.1:$PORT
token = $TOKEN
EOF
cat >"$ASOUND" <<'EOF'
pcm.!default { type null }
ctl.!default { type hw card 0 }
EOF

Xvfb -nolisten unix -listen tcp -ac -displayfd 3 -screen 0 600x900x24 \
	>"$WORK/xvfb.stdout" 2>"$WORK/xvfb.log" 3>"$WORK/x-display" &
XVFB_PID=$!
wait_for "Xvfb" 50 test -s "$WORK/x-display"
export DISPLAY="localhost:$(cat "$WORK/x-display")"

(
	cd "$RUN"
	exec env SONIX_SD_ROOT="$SD" SONIX_CONFIG="$CONFIG" SONIX_EBOOK_CONFIG="$WORK/ebook_config.ini" \
		SONIX_LOG="$WORK/player.log" ALSA_CONFIG_PATH="$ASOUND" \
		SONIX_AUDIOBOOKSHELF_PROFILES="$PROFILES" \
		./sonix_player_host >"$WORK/player.stdout" 2>&1
) &
PLAYER_PID=$!
for ((i = 0; i < 100; i++)); do
	WINDOW=$(xdotool search --onlyvisible --pid "$PLAYER_PID" 2>/dev/null | head -n 1 || true)
	[[ -n "$WINDOW" ]] && break
	sleep 0.1
done
[[ -n "$WINDOW" ]] || fail "the simulator window did not appear"
sleep 1

# ------------- P0: the Downloads row opens an empty queue ---------------
click 356 160 1                     # Home -> Streaming
enter_abs_libraries                 # Streaming -> the ABS page (fetch + library view)
click 236 305 1                     # the Downloads row -> the queue screen
shot "$WORK/queue-empty.png"
count_is 0 || fail "the queue started non-empty"

# ------------- P1: enqueue via begin_download -> the row appears --------
reopen_abs_libraries 2              # the fresh session: queue -> the ABS page -> Streaming
click 236 417 1.2                   # Mock Audiobooks
open_book_detail_and_queue          # the book -> detail -> Queue download -> the queue screen
wait_for "begin_download to enqueue a job" 40 count_is 1
wait_for "the worker to take the job" 110 is_state 1 DOWNLOADING
sleep 2                             # mid file 2 (the throttled one)
shot "$WORK/queue-row-downloading.png"
is_state 1 DOWNLOADING || fail "the job is not DOWNLOADING: $(state_at 1)"

# ------------- P2: pause the ACTIVE job from the row menu ---------------
pause_active() {
	click 236 200 0.3                  # the row (clear-finished is hidden: no terminal rows)
	sleep 0.4
	click 236 308 0.3                  # the popover: Pause
	for ((i = 0; i < 36; i++)); do
		[[ "$(state_at 1)" == "PAUSED" ]] && return 0
		sleep 0.25
	done
	return 1
}
pause_active || pause_active || pause_active ||
	fail "the active job never reached PAUSED from the row menu (state: $(state_at 1))"
shot "$WORK/queue-paused.png"
is_state 1 PAUSED || fail "pause did not stick"
PART_FILE=$(find "$QUEUE_ROOT" -name '*.part' 2>/dev/null | head -n 1)
[[ -s "$PART_FILE" && "$PART_FILE" == *"002 - Part Two"* ]] ||
	fail "the paused job's .part is not the current file's (§8.1: pause retains it, resume truncates it)"
FILE_ONE=$(find "$QUEUE_ROOT" -name '*001*' -name '*.mp3' 2>/dev/null | head -n 1)
[[ -s "$FILE_ONE" ]] || fail "a completed file vanished while the job is paused"

# ------------- P3: Remove the paused job: only the .part goes -----------
click 236 200 0.3                   # the PAUSED row
sleep 0.4
click 236 365 0.5                   # the popover: Remove (a PAUSED row = Resume + Remove)
wait_for "the paused job to be removed" 40 count_is 0
[[ -e "$PART_FILE" ]] && fail "Remove left the job-owned .part on the card"
[[ -e "$FILE_ONE" ]] || fail "Remove deleted completed audio (§8.1 line 466: only the queue row + .part may go)"
shot "$WORK/queue-after-remove.png"

# ------------- P4: fresh enqueue -> DONE -> follow-up scan -> import ----
reopen_abs_libraries                # from the emptied queue back to the library view
click 236 417 1.2                   # Mock Audiobooks
open_book_detail_and_queue
wait_for "durable queue DONE" 400 is_state 1 DONE
wait_for "the acknowledgement commit" 100 ack_committed
shot "$WORK/queue-done-row.png"
wait_for "the per-job follow-up scan" 200 grep -q "audiobooks: scanning" "$WORK/player.log"
wait_for "the coordinator's resume import" 200 grep -q "imported download resume" "$WORK/player.log"
DEST=$(find "$QUEUE_ROOT" -maxdepth 1 -type d -name 'abs-*' 2>/dev/null | head -n 1)
[[ -d "$DEST" ]] || fail "the download never produced its abs-<hash> destination"
FILE_ONE=$(find "$QUEUE_ROOT" -name '*001*' -name '*.mp3' 2>/dev/null | head -n 1)
FILE_TWO=$(find "$QUEUE_ROOT" -name '*002*' -name '*.mp3' 2>/dev/null | head -n 1)
MANIFEST=$(find "$QUEUE_ROOT" -name '.audiobookshelf' 2>/dev/null | head -n 1)
COVER=$(find "$QUEUE_ROOT" -name 'cover.jpg' 2>/dev/null | head -n 1)
[[ -s "$FILE_ONE" && -s "$FILE_TWO" ]] || fail "downloaded audio files missing"
[[ -s "$COVER" ]] || fail "the cover is missing"
[[ -s "$MANIFEST" ]] || fail "the manifest is missing (no job is done without its mandatory manifest, §8.1)"
[[ $(sqlite3 "$DB" "SELECT count(*) FROM AUDIOBOOK_TABLE WHERE name='The Mock Journey';" 2>/dev/null) == 1 ]] ||
	fail "the imported download never reached the audiobook index"
wait_for "the imported 3.0 s checkpoint" 30 resume_30s

# ------------- P5: duplicate enqueue of an unfinished row dedupes -------
# The Fixture Search Range item advertises a size it does not serve: the
# transfer refuses the wrong length and the row parks in WAITING_NETWORK
# (§8.4, honest retry-on-network-change). While it is unfinished, a repeated
# tap dedupes instead of queueing a second copy (§8.1).
reopen_abs_libraries
click 236 527 1.3                   # Fixture Search Range
open_book_detail_and_queue          # first fixture title -> detail -> Queue download
wait_for "the wrong-length row to park" 100 is_state 2 WAITING_NETWORK
shot "$WORK/queue-waiting-row.png"
# Re-tap the same book: re-entering the ABS page re-runs its load job, which
# redraws the library view -- no books view persists underneath a pop, so the
# dedupe walk goes explicitly through the library view again.
reopen_abs_libraries
click 236 527 1.2                   # Fixture Search Range
open_book_detail_and_queue          # the same book again (dedupe path)
sleep 1
[[ "$(job_count)" == "2" ]] || fail "a duplicate enqueue of an unfinished row created a second copy (§8.1 dedup): $(sql 'SELECT id,state,title FROM DOWNLOAD_JOB;')"
is_state 2 WAITING_NETWORK || fail "the deduped row changed state: $(state_at 2)"
shot "$WORK/queue-dedup.png"

# ------------- P6: cancel + retry on the WAITING row --------------------
# Layout with a terminal row present: the clear-finished action row (row 0)
# sits above the queue rows; row 1 = the DONE book, row 2 = the WAITING one.
# The popover anchored under row 2 lists its two entries at 537/593.
click 236 430 0.4                   # row 2 = the WAITING fixture row
sleep 0.5
click 236 593 0.5                   # the popover: Cancel
wait_for "the cancelled row" 40 is_state 2 CANCELLED
shot "$WORK/queue-cancelled-row.png"
click 236 430 0.4
sleep 0.5
click 236 537 0.5                   # the popover: Retry (first entry)
S=""
for ((r = 0; r < 60; r++)); do
	S=$(state_at 2)
	[[ "$S" != "CANCELLED" ]] && break
	sleep 0.25
done
[[ "$S" != "CANCELLED" ]] || fail "the retried row never left CANCELLED"
wait_for "the retried row parks back in WAITING_NETWORK" 150 is_state 2 WAITING_NETWORK

# ------------- P7: clear finished; media and progress survive; empty ----
click 236 200 0.4                   # the clear-finished action row
wait_for "clear finished to free the terminal rows" 40 terminal_clear
[[ -s "$FILE_ONE" && -s "$FILE_TWO" ]] ||
	fail "clear-finished touched completed user media (§8.4: never delete user media automatically)"
[[ -s "$MANIFEST" ]] || fail "clear-finished removed the manifest"
wait_for "the imported checkpoint to survive the clear" 10 resume_30s
# The wrong-length row (WAITING_NETWORK) is non-terminal and survives the
# clear; cancel it, then Remove it: the queue is empty again.
click 236 200 0.4                   # the WAITING row is now the top row
sleep 0.5
click 236 365 0.5                   # the popover: Cancel
wait_for "the wait row cancelled" 40 is_state 1 CANCELLED
click 236 200 0.4
sleep 0.5
click 236 365 0.5                   # the popover: Remove
wait_for "an empty queue" 40 count_is 0
shot "$WORK/queue-cleared.png"

# ------------- P8: close the screen; the coalescing line ----------------
click 43 88 0.5                     # back: the screen unloads and logs its counts
wait_for "the coalescing log line" 60 grep -q "queue screen closed after" "$WORK/player.log"
LINE=$(grep "queue screen closed after" "$WORK/player.log" | tail -n 1)
TICKS=$(sed -E 's/.* after ([0-9]+) poll ticks.*/\1/' <<<"$LINE")
REPAINTS=$(sed -E 's/.*\(([0-9]+) repaints\).*/\1/' <<<"$LINE")
[[ "$TICKS" -ge 40 && "$REPAINTS" -gt 0 && "$REPAINTS" -lt "$TICKS" ]] ||
	fail "coalescing implausible: ticks=$TICKS repaints=$REPAINTS (§8.1 line 446)"
printf 'Audiobookshelf queue smoke passed: enqueue/pause/remove/retry/clear + durable DONE and the resume import, %s poll ticks for %s repaints.\n' \
	"$TICKS" "$REPAINTS"
