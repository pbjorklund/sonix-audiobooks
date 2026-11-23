#!/usr/bin/env bash
# Calibration + smoke harness for the Audiobookshelf profiles UI.
#
# Boots the host player on an Xvfb with a seeded profile store, drives the real
# UI through devcontrol (the /tmp/sonix-control FIFO: tap/swipe commands that
# inject input straight into LVGL, in panel coordinates), and walks every
# profile action: add, select and remove (with its confirm dialog). Each step
# is checked against the player log's PROFNAV lines and the on-disk store file;
# framebuffer screenshots are kept for visual review.
#
# The tap coordinates below were calibrated from PICKERDUMP/KBDUMP output and
# screenshots in this same harness (the panel is 480x800):
#   - both the home screen and the streaming page are 2-column tile grids,
#     NOT lists: the streaming tile is the right column of row one (352,172),
#     and the Audiobookshelf tile is the left column of the streaming grid's
#     last row (122,690). x=239 is the vertical gap between the columns and a
#     tap there does nothing.
#   - settingsrow pages: content top y=130, first row centre y=199, rows are
#     100px tall with an 8px gap (centre of row i is 199 + 108*i), x centre 239.
#   - the editor uses absolute layout: field centres at y=160/230/300, x=239;
#     the keyboard accept key sits at (429,754).
#   - picker row buttons: pencil centre (336,199), trash centre (412,199) on
#     the first row.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
PLAYER="$ROOT/sonix-player"
WORK="${SONIX_ABS_UI_CAL_DIR:-$ROOT/build/abs-ui-cal}"
RUN="$WORK/run"
SD="$WORK/sd"
CONFIG="$WORK/device_config.ini"
ASOUND="$WORK/asound.conf"
PORT_FILE="$WORK/mock-port"
PROGRESS_FILE="$WORK/progress.json"
# The profile store is a file of its own; on the host it only exists when this
# points somewhere (otherwise the store is ephemeral and dies with the process).
PROFILES_FILE="$WORK/abs-profiles.ini"
TOKEN="smoketoken1234567890"
PLAYER_PID=""
SERVER_PID=""
SERVER_B_PID=""
XVFB_PID=""
FAILURES=0

# Hard overall bound: a stuck run kills itself instead of burning hours. Check
# from tap()/ctrl(), the two highest-frequency actions, so any wedged step ends
# the run within one action's cost. Override with SONIX_ABS_UI_TIMEOUT.
ABS_UI_TIMEOUT="${SONIX_ABS_UI_TIMEOUT:-900}"
DEADLINE=$((SECONDS + ABS_UI_TIMEOUT))
check_deadline() {
	if (( SECONDS > DEADLINE )); then
		echo "calibration: exceeded the ${ABS_UI_TIMEOUT}s overall bound; aborting" >&2
		exit 1
	fi
}

cleanup() {
	for pid in "$PLAYER_PID" "$SERVER_PID" "$SERVER_B_PID" "$XVFB_PID"; do
		if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then kill -TERM "$pid" 2>/dev/null || true; fi
	done
}
trap cleanup EXIT INT TERM

wait_for() {
	local description=$1 attempts=$2
	shift 2
	# `n`, not `i`: caller loops (type_letters and friends) index the typed
	# string with a global i -- a wait_for/ctrl i would rewind or stall it (the
	# original hang: type_letters re-tapped the same key forever).
	local n=0
	for ((; n < attempts; n++)); do
		if "$@"; then return 0; fi
		sleep 0.1
	done
	echo "calibration: timed out waiting for $description" >&2
	exit 1
}

# ---------------------------------------------------------------------------
# devcontrol: the player's own UI automation FIFO (panel coordinates, input is
# injected into LVGL directly -- no X event path). Every command answers in
# /tmp/sonix-control.status; ctrl removes it first so a stale answer can never
# be mistaken for a fresh one.
CTRL_FIFO=/tmp/sonix-control
CTRL_STATUS=/tmp/sonix-control.status

ctrl() { # ctrl COMMAND EXPECTED_STATUS_SUBSTRING
	check_deadline
	rm -f "$CTRL_STATUS" 2>/dev/null || true
	# timeout guards against a dead player: with no reader left, the FIFO open
	# would block forever.
	timeout 5 bash -c "printf '%s\n' \"\$1\" > '$CTRL_FIFO'" _ "$1" || {
		echo "ctrl: cannot write to $CTRL_FIFO (player gone?)" >&2
		return 1
	}
	# `n`, not `i` -- see wait_for(): this loop must not touch the caller's i.
	local n=0
	for ((; n < 50; n++)); do
		if [[ -f "$CTRL_STATUS" ]] && grep -q "$2" "$CTRL_STATUS"; then return 0; fi
		sleep 0.1
	done
	echo "ctrl: no '$2' for command '$1'" >&2
	return 1
}

tap() { # tap X Y [HOLD_MS] -- the status says "accepted", not "processed";
	# the gesture itself runs for HOLD ms, so settle before the next action.
	check_deadline
	local hold=${3:-150}
	ctrl "tap $1 $2 $hold" "ok tap" || return 1
	sleep 0.4
}

shot() { # shot NAME -- X screenshot with a non-blank sanity check. SDL's first
	# present can lag the window by a few seconds, so retry while it is black;
	# without PIL the check is skipped and the shot is taken as-is.
	local out="$WORK/$1.png" i colors=""
	for ((i = 0; i < 8; i++)); do
		scrot "$out" 2>/dev/null || sleep 1
		if [[ ! -s "$out" ]]; then sleep 1; continue; fi
		colors=$(python3 -c "from PIL import Image; print(len(Image.open('$out').getcolors(maxcolors=100000)))" \
			2>/dev/null) || true
		if [[ -z "$colors" ]]; then
			echo "shot: $1 taken (no PIL, blank check skipped)" >&2
			return 0
		fi
		if [[ "$colors" =~ ^[0-9]+$ ]] && [[ "$colors" -gt 50 ]]; then return 0; fi
		sleep 1
	done
	echo "shot: $1 looks blank (distinct colors: ${colors:-unknown})" >&2
	return 1
}

# expect LOG_SUBSTRING DESCRIPTION -- assert the player log already contains a
# line; this is how each UI action proves it happened (the PROFNAV lines are
# emitted by the module itself, host builds only).
expect() {
	if grep -q "$1" "$WORK/player.log"; then
		echo "ok: $2"
	else
		echo "FAIL: $2 (missing log line: $1)" >&2
		FAILURES=$((FAILURES + 1))
	fi
}

# expect_count LOG_SUBSTRING MIN_COUNT DESCRIPTION -- like expect, but the line
# must appear at least MIN_COUNT times (picker_draw also runs at init, so a
# single occurrence proves nothing about navigation).
expect_count() {
	local count
	count=$(grep -c "$1" "$WORK/player.log" || true)
	if [[ "$count" -ge "$2" ]]; then
		echo "ok: $3 (seen $count x)"
	else
		echo "FAIL: $3 (saw $count, need $2: $1)" >&2
		FAILURES=$((FAILURES + 1))
	fi
}

# ---------------------------------------------------------------------------
# count_cred -- how many authenticated requests a mock has logged so far.
# The mocks write one line per request to <progress-file stem>.requests (the
# .json suffix is replaced, not extended) with a non-secret credential alias;
# cred=credential-a is the --token credential both profiles use, and unknown
# tokens log as credential-none instead of reaching a route.
# The harness runs TWO mock instances: the seeded p1 points at the first, and
# p2 is typed against the second. The store rejects two profiles for the same
# server URL, so p2 must have its own server to be addable at all.
REQUEST_LOG_A="${PROGRESS_FILE%.json}.requests"
REQUEST_LOG_B="$WORK/progress-b.requests"

count_cred() {
	local log=$1
	if [[ -f "$log" ]]; then
		grep -c "cred=credential-a" "$log" || true
	else
		echo 0
	fi
}

# ---------------------------------------------------------------------------
# Keyboard coordinates (KBDUMP, letter mode). The symbol pages reuse the same
# key slots: after MODE ("123"), row one is "1234567890" and row three starts
# with ". , : ;"; SHIFT then swaps in the second page where "/" sits on the a
# slot. type_letters types lowercase letters; type_url and type_token handle
# the mode switches for the server address and the credential token.
K_Q=(28 526); K_W=(75 526); K_E=(122 526); K_R=(169 526); K_T=(216 526)
K_Y=(263 526); K_U=(310 526); K_I=(357 526); K_O=(404 526); K_P=(451 526)
K_A=(30 602); K_S=(82 602); K_D=(134 602); K_F=(186 602); K_G=(238 602)
K_H=(291 602); K_J=(343 602); K_K=(396 602); K_L=(448 602)
K_SHIFT=(35 678); K_Z=(90 678); K_X=(140 678); K_C=(190 678); K_V=(240 678)
K_B=(290 678); K_N=(339 678); K_M=(389 678); K_DEL=(444 678)
	K_MODE=(49 754); K_ACCEPT=(429 754)
	# Symbol page one: the dot/z and colon/c slots of row three (KBDUMP).
	K_DOT=(90 678)

key() { # key KEY_ARRAY_NAME -- tap the centre of a keyboard key
	local -n k=$1
	tap "${k[0]}" "${k[1]}"
}

type_letters() {
	local ch
	for ((i = 0; i < ${#1}; i++)); do
		ch=${1:i:1}
		case $ch in
			a) key K_A ;; b) key K_B ;; c) key K_C ;; d) key K_D ;; e) key K_E ;;
			f) key K_F ;; g) key K_G ;; h) key K_H ;; i) key K_I ;; j) key K_J ;;
			k) key K_K ;; l) key K_L ;; m) key K_M ;; n) key K_N ;; o) key K_O ;;
			p) key K_P ;; q) key K_Q ;; r) key K_R ;; s) key K_S ;; t) key K_T ;;
			u) key K_U ;; v) key K_V ;; w) key K_W ;; x) key K_X ;; y) key K_Y ;;
			z) key K_Z ;; *) echo "type_letters: unsupported '$ch'" >&2; return 1 ;;
		esac
	done
}

# type_url http://127.0.0.1:$PORT -- letters, then the symbol pages for "://"
# and the digits. On the symbol page row one is "1234567890", the dot is the
# z slot and the colon the c slot of row three, so "127.0.0.1:" types without
# another page switch. Ends with MODE, restoring the letters page: the mode
# toggle survives a field change (the focus move does NOT reset it), so a
# later type_token would otherwise type its letters against the symbol page.
type_url() {
	type_letters http
	key K_MODE # -> symbols 1
	key K_C	 # ':'
	key K_SHIFT # -> symbols 2
	key K_A; key K_A # '/' '/'
	key K_SHIFT # -> symbols 1
	# "127.0.0.1:" -- digits on row one, dot/colon on row three, one page.
	key K_Q;   key K_W;  key K_U # 1 2 7
	key K_DOT                      # .
	key K_P                        # 0
	key K_DOT                      # .
	key K_P                        # 0
	key K_DOT                      # .
	key K_Q                        # 1
	key K_C                        # :
	local d
	for ((i = 0; i < ${#1}; i++)); do
		d=${1:i:1}
		case $d in
			1) key K_Q ;; 2) key K_W ;; 3) key K_E ;; 4) key K_R ;; 5) key K_T ;;
			6) key K_Y ;; 7) key K_U ;; 8) key K_I ;; 9) key K_O ;; 0) key K_P ;;
		esac
	done
	key K_MODE # -> letters, for the next typing function
}

# type_token $TOKEN -- the letters, then MODE and the ten digits. Assumes the
# letters page: type_url and type_token both restore it on the way out.
type_token() {
	type_letters "${1:0:10}"
	key K_MODE
	local d
	for ((i = 0; i < ${#1}; i++)); do
		d=${1:i:1}
		case $d in
			1) key K_Q ;; 2) key K_W ;; 3) key K_E ;; 4) key K_R ;; 5) key K_T ;;
			6) key K_Y ;; 7) key K_U ;; 8) key K_I ;; 9) key K_O ;; 0) key K_P ;;
		esac
	done
	key K_MODE # -> letters
}

rm -rf -- "$WORK"
mkdir -p "$RUN/usr/resource" "$SD" "$WORK/media"

cp "$PLAYER/sonix_player_host" "$RUN/"
cp -a "$ROOT/sonix-packer/assets/R1/usr/resource/." "$RUN/usr/resource/"

python3 "$ROOT/tools/audiobookshelf_mock.py" --media-dir "$WORK/media" --port-file "$PORT_FILE" \
	--progress-file "$PROGRESS_FILE" --token "$TOKEN" >"$WORK/mock.stdout" 2>"$WORK/mock.stderr" &
SERVER_PID=$!
wait_for "mock server" 50 test -s "$PORT_FILE"
PORT=$(cat "$PORT_FILE")
echo "mock port: $PORT"

# A second instance for p2: the store's duplicate-server rule makes two
# profiles on one URL unaddable, and p2 needs a reachable server of its own so
# its typed credentials can be checked for real.
PORT_FILE_B="$WORK/mock-port-b"
PROGRESS_FILE_B="$WORK/progress-b.json"
python3 "$ROOT/tools/audiobookshelf_mock.py" --media-dir "$WORK/media" --port-file "$PORT_FILE_B" \
	--progress-file "$PROGRESS_FILE_B" --token "$TOKEN" >"$WORK/mock-b.stdout" 2>"$WORK/mock-b.stderr" &
SERVER_B_PID=$!
wait_for "second mock server" 50 test -s "$PORT_FILE_B"
PORT_B=$(cat "$PORT_FILE_B")
echo "mock B port: $PORT_B"

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

# SEED=1 starts with one profile already in the store, so the picker has a row
# to edit and remove. The store is its own file (SONIX_AUDIOBOOKSHELF_PROFILES),
# not part of device_config.ini.
if [[ "${SEED:-0}" == "1" ]]; then
	cat >"$PROFILES_FILE" <<EOF
version=1
next_id=2
active=p1

[profile.p1]
name=Home
server=http://127.0.0.1:$PORT
token=$TOKEN
EOF
fi
cat >"$ASOUND" <<'EOF'
pcm.!default { type null }
ctl.!default { type hw card 0 }
EOF

# The display is 480x800 (the host panel); the Xvfb matches it exactly. Input
# no longer travels through X at all (devcontrol), but SDL still needs a screen.
DISPLAY_FILE="$WORK/x-display"
Xvfb -nolisten unix -listen tcp -ac -displayfd 3 -screen 0 480x800x24 \
	>"$WORK/xvfb.stdout" 2>"$WORK/xvfb.log" 3>"$DISPLAY_FILE" &
XVFB_PID=$!
wait_for "Xvfb" 50 test -s "$DISPLAY_FILE"
export DISPLAY="localhost:$(cat "$DISPLAY_FILE")"

rm -f "$CTRL_FIFO" "$CTRL_STATUS"
(
	cd "$RUN"
	exec env SONIX_SD_ROOT="$SD" SONIX_CONFIG="$CONFIG" SONIX_EBOOK_CONFIG="$WORK/ebook_config.ini" \
		SONIX_LOG="$WORK/player.log" ALSA_CONFIG_PATH="$ASOUND" SONIX_KB_DUMP=1 SONIX_CONFIRM_DUMP=1 \
		SONIX_PROFNAV=1 SONIX_PICKER_DUMP=1 SONIX_AUDIOBOOKSHELF_PROFILES="$PROFILES_FILE" \
		./sonix_player_host >"$WORK/player.stdout" 2>&1
) &
PLAYER_PID=$!
wait_for "devcontrol FIFO" 50 test -p "$CTRL_FIFO"
wait_for "main event loop" 100 grep -q "Entering main event loop" "$WORK/player.log"
ctrl "status" "ok ready"
sleep 2 # let the first frame present to X before the first screenshot

# ---------------------------------------------------------------------------
# Navigate home -> streaming -> Audiobookshelf page -> profile picker.
# ---------------------------------------------------------------------------
tap 352 172 # home: the Streaming tile (right column, row one -- x=239 is the column gap)
shot 01-streaming
	tap 122 690 # streaming grid: the Audiobookshelf tile (left column, last row)
sleep 1
shot 02-abs-page
tap 239 199 # ABS page: the top row opens the picker
sleep 1
shot 03-picker
# The init-time draw already printed one line, so navigation needs a second.
expect_count "picker_draw entries=1" 2 "picker shows the seeded profile"

if [[ "${SEED:-0}" == "1" ]]; then
	# ---------------------------------------------------------------------
	# Add a second profile through the editor. Adding makes it active, which
	# is what lets the next step select p1 as a real switch instead of a no-op.
	# ---------------------------------------------------------------------
	tap 239 307 # picker: the "Add a server" row (second row)
	expect "editor_add" "add-mode editor opened"

	tap 239 160 # name field
	type_letters second
	tap 239 230 # server field
	type_url "$PORT_B"
	tap 239 300 # token field
	type_token "$TOKEN"
	shot 04-editor-filled

	# Sample the baseline BEFORE the save: the save itself starts the p2
	# library load (adding makes the new profile active), and that request is
	# what the check below must see -- a baseline taken after the save would
	# race it at 1 == 1 and wait forever for a second request.
	req_before=$(count_cred "$REQUEST_LOG_B")
	tap "${K_ACCEPT[0]}" "${K_ACCEPT[1]}" # the check key saves and returns to the picker
	sleep 1
	# The keyboard has no shift in this walk: the typed name is lowercase.
	expect "save_added id=p2 name=second" "second profile saved (name field typed correctly)"
	expect "picker_draw entries=2" "picker now lists two profiles"

	# Adding makes p2 active, so the ABS page behind the picker loads its
	# libraries with p2's typed server and token: authenticated mock requests
	# prove both fields were entered correctly (a wrong URL or token would log
	# as credential-none or never arrive at all).
	for ((i = 0; i < 100; i++)); do
		req_after=$(count_cred "$REQUEST_LOG_B")
		[[ "$req_after" -gt "$req_before" ]] && break
		sleep 0.1
	done
	if [[ "${req_after:-0}" -gt "$req_before" ]]; then
		echo "ok: p2's typed server and token authenticated against mock B ($req_before -> $req_after)"
	else
		echo "FAIL: no authenticated requests with p2's credentials (typed URL or token wrong?)" >&2
		FAILURES=$((FAILURES + 1))
	fi
	shot 05-picker-two

	# ---------------------------------------------------------------------
	# Select p1 from the picker: a real switch (p2 is active after the add),
	# and it makes the ABS page load libraries with p1's credential, which the
	# mock logs -- new authenticated lines prove the round trip.
	# ---------------------------------------------------------------------
	req_before=$(count_cred "$REQUEST_LOG_A")
	tap 239 199 # picker row one: p1 "Home"
	expect "select id=p1 name=Home" "p1 selected from the picker (switched away from p2)"
	for ((i = 0; i < 50; i++)); do
		req_after=$(count_cred "$REQUEST_LOG_A")
		[[ "$req_after" -gt "$req_before" ]] && break
		sleep 0.1
	done
	if [[ "${req_after:-0}" -gt "$req_before" ]]; then
		echo "ok: authenticated library requests followed the selection ($req_before -> $req_after)"
	else
		echo "FAIL: no new authenticated mock requests after selecting p1" >&2
		FAILURES=$((FAILURES + 1))
	fi
	if grep -q "^active=p1$" "$PROFILES_FILE"; then
		echo "ok: store file has active=p1"
	else
		echo "FAIL: store file does not have active=p1" >&2
		FAILURES=$((FAILURES + 1))
	fi
	shot 06-picker-selected

	# ---------------------------------------------------------------------
	# Remove p2 (the non-active one) through its confirm dialog. Row two's
	# trash button sits at the same x, one row pitch lower.
	# ---------------------------------------------------------------------
	tap 412 307 # picker row two: the trash button
	expect "remove_confirm id=p2" "remove dialog opened for p2"
	shot 07-confirm

	# The OK button's centre comes from CONFIRMDUMP (the card is centred on the
	# screen; its buttons are dumped with their laid-out coordinates). The dump
	# timer stamps its line 600ms after the dialog shows, so poll for it here
	# instead of racing the first tick.
	for ((n = 0; n < 30; n++)); do
		grep -q "CONFIRMDUMP ok x=" "$WORK/player.log" 2>/dev/null && break
		sleep 0.1
	done
	# grep can come up empty here (the dialog never opened); with pipefail an
	# unguarded pipeline would silently kill the script before its own FAIL
	# report -- so let the empty case fall through to the -z check below. The
	# dump prints BOTH axes on one line per button ("CONFIRMDUMP ok x=..y=.."),
	# so both values are cut from the same line.
	ok_line=$(grep "CONFIRMDUMP ok x=" "$WORK/player.log" | tail -n 1 || true)
	ok_x=$(printf '%s' "$ok_line" | sed -n 's/.*x=\([0-9]*\).*/\1/p' 2>/dev/null || true)
	ok_y=$(printf '%s' "$ok_line" | sed -n 's/.*y=\([0-9]*\).*/\1/p' 2>/dev/null || true)
	if [[ -z "${ok_x:-}" || -z "${ok_y:-}" ]]; then
		echo "FAIL: confirm dialog OK button not found in CONFIRMDUMP" >&2
		FAILURES=$((FAILURES + 1))
	else
		tap "$ok_x" "$ok_y"
		expect "remove_confirmed id=p2" "p2 removed after confirming"
		if grep -q "\[profile.p2\]" "$PROFILES_FILE"; then
			echo "FAIL: store file still lists p2" >&2
			FAILURES=$((FAILURES + 1))
		else
			echo "ok: store file no longer lists p2"
		fi
		if grep -q "^active=p1$" "$PROFILES_FILE"; then
			echo "ok: removing the non-active profile kept active=p1"
		else
			echo "FAIL: active id changed when removing a non-active profile" >&2
			FAILURES=$((FAILURES + 1))
		fi
	fi
	shot 08-picker-after-remove

	# ---------------------------------------------------------------------
	# Blank-token retention (§11.6) with a real edit: open p1's editor, save
	# without typing a token; the stored token must survive. The field starts
	# blank and nothing is typed here -- the save is the whole point. p1 is
	# active, so the save also runs the page's in-place validation against
	# mock A (a second authenticated request).
	# ---------------------------------------------------------------------
	tap 336 199 # picker row one: the pencil button
	expect "editor_edit id=p1" "edit-mode editor opened for p1"
	shot 09-editor-edit
	sleep 0.5 # let the editor's first render settle before the save
	# Baseline before the save: the validation request leaves the player at
	# once, and the check below must count it (no growth == self-raced check).
	req_before=$(count_cred "$REQUEST_LOG_A")
	tap "${K_ACCEPT[0]}" "${K_ACCEPT[1]}" # save as-is: blank token keeps
	sleep 1
	expect "save_edited id=p1 name=Home" "p1 saved unchanged (blank token)"
	if grep -q "^token=$TOKEN$" "$PROFILES_FILE"; then
		echo "ok: blank token kept the stored secret"
	else
		echo "FAIL: the stored token did not survive a blank-token save" >&2
		FAILURES=$((FAILURES + 1))
	fi
	for ((i = 0; i < 100; i++)); do
		req_after=$(count_cred "$REQUEST_LOG_A")
		[[ "$req_after" -gt "$req_before" ]] && break
		sleep 0.1
	done
	if [[ "${req_after:-0}" -gt "$req_before" ]]; then
		echo "ok: the edit ran the active profile's validation against the mock ($req_before -> $req_after)"
	else
		echo "FAIL: no authenticated request ran after saving the active profile" >&2
		FAILURES=$((FAILURES + 1))
	fi
	shot 10-editor-after-blankeep
fi

echo "calibration done; screenshots in $WORK"
if [[ "$FAILURES" -gt 0 ]]; then
	echo "SMOKE: $FAILURES failure(s)" >&2
	exit 1
fi
echo "SMOKE: all checks passed"
