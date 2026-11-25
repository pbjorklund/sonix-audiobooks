#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
WORK=$(mktemp -d "$ROOT/build/abs-podcast-smoke.XXXXXX")
RUN="$WORK/run"
CARD="$WORK/card"
DB="$CARD/.local/audiobooks.db"
MOCK= X= PLAYER= WINDOW=
cleanup() {
    for p in "$PLAYER" "$MOCK" "$X"; do
        if [[ -n $p ]]; then kill "$p" 2>/dev/null || true; wait "$p" 2>/dev/null || true; fi
    done
    printf 'Podcast smoke evidence: %s\n' "$WORK"
}
trap cleanup EXIT
fail() { printf 'Podcast smoke: %s\n' "$*" >&2; exit 1; }
wait_for() {
    local label=$1; shift
    for ((i=0; i<150; i++)); do if "$@"; then return 0; fi; sleep .1; done
    fail "timed out: $label"
}
request() { grep -qF "$1" "$WORK/progress.requests"; }
click() { xdotool mousemove --window "$WINDOW" "$1" "$2" click 1; sleep "${3:-.75}"; }
shot() { scrot -o "$WORK/$1.png"; }
grep_label() { tesseract "$WORK/$1.png" stdout 2>/dev/null | grep -F "$2" >/dev/null; }
assert_show_art() {
    ffmpeg -v error -i "$WORK/$1.png" -pix_fmt rgb24 -f rawvideo - | python3 -c '
import sys
pixels = sys.stdin.buffer.read()
assert sum(r > 225 and g < 40 and b > 155 for r, g, b in zip(pixels[::3], pixels[1::3], pixels[2::3])) >= int(sys.argv[1]), "show cover not rendered"
' "${2:-5000}"
}
mkdir -p "$RUN/usr/resource" "$CARD" "$WORK/media"
cp -a "$ROOT/sonix-packer/assets/R1/usr/resource/." "$RUN/usr/resource/"
cp "$ROOT/sonix-player/sonix_player_host" "$RUN/"
FORMAT=${SONIX_PODCAST_SMOKE_FORMAT:-mp3}
case "$FORMAT" in
    mp3) codec=libmp3lame ;;
    m4a) codec=aac ;; # ffmpeg's default MP4 layout writes the moov atom at the end.
    *) fail "unexpected episode format: $FORMAT" ;;
esac
MEDIA="$WORK/media/episode.$FORMAT"
mux=()
if [[ $FORMAT == m4a && ${SONIX_PODCAST_SMOKE_FASTSTART:-0} == 1 ]]; then mux=(-movflags +faststart); fi
ffmpeg -nostdin -loglevel error -f lavfi -i 'sine=frequency=440:duration=36' -c:a "$codec" -b:a 128k "${mux[@]}" "$MEDIA"
if [[ ${SONIX_PODCAST_SMOKE_NEXT:-0} == 1 ]]; then touch "$WORK/media/two-podcast-episodes"; fi
if [[ ${SONIX_PODCAST_SMOKE_SHOW_FILTERS:-0} == 1 ]]; then touch "$WORK/media/show-progress-fixture"; fi
if [[ ${SONIX_PODCAST_SMOKE_NO_DATE:-0} == 1 ]]; then touch "$WORK/media/podcast-no-date"; fi
if [[ ${SONIX_PODCAST_SMOKE_DETAIL_QUEUE:-0} == 1 ]]; then touch "$WORK/media/hold-podcast-download"; fi
if [[ ${SONIX_PODCAST_SMOKE_ART:-0} == 1 ]]; then
    ffmpeg -nostdin -loglevel error -f lavfi -i 'color=c=0xff00cc:s=192x192' -frames:v 1 -threads 1 "$WORK/media/podcast-cover.jpg"
fi
TOKEN=local-podcast-smoke
python3 "$ROOT/tools/audiobookshelf_mock.py" --media-dir "$WORK/media" --port-file "$WORK/port" \
    --progress-file "$WORK/progress.json" --token "$TOKEN" --podcast-smoke --throttle 258638762231958:130 \
    >"$WORK/mock.log" 2>&1 &
MOCK=$!
wait_for server test -s "$WORK/port"
PORT=$(<"$WORK/port")
python3 - "$PORT" "$TOKEN" <<'PY'
import sys
from urllib.request import Request, urlopen
url = f'http://127.0.0.1:{sys.argv[1]}/api/me/progress/pod-41/pod41-ep-001'
req = Request(url, method='PATCH', data=b'{"currentTime":0,"duration":36,"isFinished":false}',
              headers={'Authorization': 'Bearer ' + sys.argv[2], 'Content-Type': 'application/json'})
with urlopen(req) as response:
    assert response.status in (200, 204)
PY
cat >"$WORK/config.ini" <<EOF
[player]
volume = 15
remember_track = 0
[ui]
language = English
[power]
screen_off_seconds = 0
[clock]
last_seen = $(date +%s)
configured = 1
[audiobookshelf]
server = http://127.0.0.1:$PORT
token = $TOKEN
EOF
if [[ ${SONIX_PODCAST_SMOKE_SLEEP:-0} == 1 ]]; then
    # Exercise migration of the old Music timer in slot one.
    printf '\n[quickpanel]\nlayout = 4\norder = sleep_music,wifi\n' >>"$WORK/config.ini"
fi
cat >"$WORK/asound.conf" <<'EOF'
pcm.!default { type null }
ctl.!default { type hw card 0 }
EOF
if [[ ${SONIX_PODCAST_SMOKE_HANDOFF:-0} == 1 ]]; then
    cat >>"$WORK/asound.conf" <<EOF
pcm.!default {
    type file
    slave.pcm { type null }
    file "$WORK/handoff.raw"
    format raw
}
EOF
fi
Xvfb -nolisten unix -listen tcp -ac -displayfd 3 -screen 0 600x900x24 >"$WORK/x.log" 2>&1 3>"$WORK/display" &
X=$!
wait_for Xvfb test -s "$WORK/display"
export DISPLAY="localhost:$(<"$WORK/display")" SDL_VIDEODRIVER=x11
start_player() {
    local name=$1
    (
        cd "$RUN"
        exec env SONIX_SD_ROOT="$CARD" SONIX_CONFIG="$WORK/config.ini" SONIX_LOG="$WORK/$name.log" \
            ALSA_CONFIG_PATH="$WORK/asound.conf" SONIX_AUDIOBOOKSHELF_PROFILES="$WORK/profiles.ini" SONIX_CONFIRM_DUMP=1 \
            ./sonix_player_host >"$WORK/$name.stdout" 2>&1
    ) &
    PLAYER=$!
    wait_for 'simulator log' test -s "$WORK/$name.log"
    wait_for 'simulator event loop' grep -q 'Entering main event loop' "$WORK/$name.log"
    for ((i=0; i<120; i++)); do
        WINDOW=$(xdotool search --onlyvisible --pid "$PLAYER" 2>/dev/null | tail -n 1 || true)
        if [[ -n $WINDOW ]]; then sleep .8; return; fi
        sleep .1
    done
    fail 'simulator window did not appear'
}
if [[ ${SONIX_PODCAST_SMOKE_FROM_MUSIC:-0} == 1 ]]; then
    mkdir -p "$CARD/.local" "$CARD/Music"
    music_cover=0xff00cc
    if [[ ${SONIX_PODCAST_SMOKE_ART:-0} == 1 ]]; then music_cover=0x00ee00; fi
    ffmpeg -nostdin -loglevel error -f lavfi -i "color=c=$music_cover:s=192x192" -frames:v 1 -threads 1 "$CARD/Music/cover.jpg"
    ffmpeg -nostdin -loglevel error -f lavfi -i 'sine=frequency=220:duration=90' -metadata title='Fixture music before podcast' "$CARD/Music/fixture.mp3"
    python3 - "$CARD" <<'PY'
import sqlite3, sys
from pathlib import Path
card = Path(sys.argv[1])
with sqlite3.connect(card / '.local/library.db') as db:
    db.execute('CREATE TABLE PLAYBACK_STATE(id INT PRIMARY KEY, path TEXT, position REAL, updated_at INT)')
    db.execute('INSERT INTO PLAYBACK_STATE VALUES(0, ?, 1.0, 0)', (str(card / 'Music/fixture.mp3'),))
PY
    sed -i 's/^remember_track = 0$/remember_track = 1/' "$WORK/config.ini"
    printf '\n[music]\nnowplaying_at_boot = 1\n' >>"$WORK/config.ini"
fi
start_player first
if [[ ${SONIX_PODCAST_SMOKE_FROM_MUSIC:-0} == 1 ]]; then
    click 240 732 1
    wait_for 'music actually playing' grep -q 'PCM open on' "$WORK/first.log"
    if [[ ${SONIX_PODCAST_SMOKE_ART:-0} != 1 ]]; then
        shot music-before-podcast
        assert_show_art music-before-podcast
    fi
    click 43 56 1
fi
if [[ ${SONIX_PODCAST_SMOKE_SLEEP:-0} == 1 ]]; then
    printf 'swipe 240 18 240 640 450\n' >/tmp/sonix-control
    sleep 1
    shot sleep-control-centre
    click 103 118 .5
    grep -Eq '^enabled[[:space:]]*=[[:space:]]*1$' "$WORK/config.ini" || fail 'Control Center tap did not enable the timer'
    click 103 118 .5
    grep -Eq '^enabled[[:space:]]*=[[:space:]]*0$' "$WORK/config.ini" || fail 'Control Center tap did not disable the timer'
    xdotool mousemove --window "$WINDOW" 103 118 mousedown 1 sleep .8 mouseup 1
    sleep 1
    shot sleep-settings
    tesseract "$WORK/sleep-settings.png" stdout 2>/dev/null >"$WORK/sleep-settings.txt"
    for label in 'Sleep timer' 'Always on' 'Daily schedule' 'Duration'; do
        grep -Fi "$label" "$WORK/sleep-settings.txt" >/dev/null || fail "missing timer choice: $label"
    done
    click 415 462 .6
    shot sleep-schedule
    tesseract "$WORK/sleep-schedule.png" stdout 2>/dev/null >"$WORK/sleep-schedule.txt"
    grep -Fi 'Start time' "$WORK/sleep-schedule.txt" >/dev/null || fail 'schedule did not show start time'
    grep -Fi 'End time' "$WORK/sleep-schedule.txt" >/dev/null || fail 'schedule did not show end time'
    click 184 706 .6
    grep -Eq '^start[[:space:]]*=[[:space:]]*1380$' "$WORK/config.ini" || fail 'schedule did not save the selected start hour'
    click 415 462 .6
    click 250 356 .6
    grep -Eq '^minutes[[:space:]]*=[[:space:]]*60$' "$WORK/config.ini" || fail '60-minute preset was not saved'
    click 73 356 .6
    click 415 200 .6
    grep -Eq '^minutes[[:space:]]*=[[:space:]]*15$' "$WORK/config.ini" || fail 'timer preset was not saved'
    grep -Eq '^enabled[[:space:]]*=[[:space:]]*1$' "$WORK/config.ini" || fail 'timer was not enabled'
    click 43 88 .7
fi
click 356 160
click 124 704 1.4
shot root
# Open the podcast library beneath the Audiobooks group.
click 250 736 1
shot library
click 250 200 1.4
wait_for 'show list' request 'GET /api/libraries/lib-pod-2/items?'
shot shows
click_show_control() {
    case "$1" in
        Oldest) click 125 228 .6 ;;
        Newest) click 350 228 .6 ;;
        Unplayed) click 78 286 .6 ;;
        Sort) click 410 286 .6 ;;
        *) fail "Unknown show control: $1" ;;
    esac
}
if [[ ${SONIX_PODCAST_SMOKE_SHOW_FILTERS:-0} == 1 ]]; then
    click 250 393 1
    wait_for 'show episode list' request 'GET /api/items/pod-41?expanded=1 '
    sleep 1
    click_show_control Unplayed
    shot started-and-finished
    tesseract "$WORK/started-and-finished.png" stdout 2>/dev/null >"$WORK/filtered.txt"
    grep -F 'Episode 002' "$WORK/filtered.txt"
    grep -F 'Episode 003' "$WORK/filtered.txt"
    if grep -F 'Episode 001' "$WORK/filtered.txt"; then fail 'Unplayed episode passed Started + Finished filter'; fi
    printf 'Podcast show filters: combining Started and Finished excludes Unplayed.\n'
    exit 0
fi
if [[ ${SONIX_PODCAST_SMOKE_NEXT:-0} == 1 ]]; then
    click 250 393 1
    wait_for 'show episode list' request 'GET /api/items/pod-41?expanded=1 '
    sleep 1
    shot next-actions
    click_show_control Oldest
    wait_for 'oldest episode streaming' request 'GET /api/items/pod-41/file/258638762231958 '
    wait_for 'played episode download' request 'GET /api/items/pod-41/file/258638762231958/download '
    wait_for 'same-episode local handoff' grep -q 'continued Audiobookshelf episode from verified local audio' "$WORK/first.log"
    both_saved() { [[ $(find "$CARD/Podcast/ABS" -name .abs-episode | wc -l) == 2 ]]; }
    wait_for 'both episodes downloaded' both_saved
    [[ $(grep -c 'play requested' "$WORK/first.log") == 1 ]] || fail 'prefetch started playback'
    click 43 56 1
    click_show_control Newest
    wait_for 'newest episode opened from the card' awk '/play requested.*Podcast\/ABS\// {seen=1} END {exit !seen}' "$WORK/first.log"
    [[ $(grep -c 'GET /api/items/pod-41/file/.*download ' "$WORK/progress.requests") == 2 ]] || fail 'local replay requested another download'
    printf 'Play next: oldest streamed and switched to local, both cached, newest played locally.\n'
    exit 0
fi
if [[ ${SONIX_PODCAST_SMOKE_SHOWS:-0} == 1 ]]; then
    assert_show_art shows 3000
    pick_sort() {
        local row=$1 label=$2 y
        click 160 "$row" .5
        shot sort-menu
        y=$(tesseract "$WORK/sort-menu.png" stdout tsv 2>/dev/null | awk -F '\t' -v label="$label" '$12 == label && $7 > 115 {print int($8+$10/2-50); exit}')
        [[ -n $y ]] || fail "Missing sort option $label"
        click 220 "$y" 1
    }
    pick_sort 281 Recently
    wait_for 'ascending date ordering requested' request 'sort=addedAt&minified=1&desc=0 '
    pick_sort 281 Descending
    wait_for 'descending date ordering requested' request 'sort=addedAt&minified=1&desc=1 '
    pick_sort 281 Title
    wait_for 'descending title ordering requested' request 'sort=media.metadata.title&minified=1&desc=1 '
    pick_sort 281 Ascending
    click 250 393 1
    wait_for 'opened show episode list' request 'GET /api/items/pod-41?expanded=1 '
    shot show-newest
    click_show_control Sort
    shot show-sort-menu
    sort_y=$(tesseract "$WORK/show-sort-menu.png" stdout tsv 2>/dev/null | awk -F '\t' '$12=="Ascending" {print int($8+$10/2-50); exit}')
    [[ -n $sort_y ]] || fail 'could not find Ascending'
    click 220 "$sort_y" .6
    shot show-oldest
    shot show-filter-controls
    printf 'Podcast show artwork and episode controls exercised.\n'
    exit 0
fi
click 43 88
click 250 307 1.2
wait_for latest request 'GET /api/libraries/lib-pod-2/recent-episodes?'
if [[ ${SONIX_PODCAST_SMOKE_FILTERS:-0} == 1 ]]; then
    shot latest-unheard
    tesseract "$WORK/latest-unheard.png" stdout 2>/dev/null | grep -F 'Episode 001' >/dev/null ||
        fail 'Latest To hear did not render the unfinished episode'
    PORT="$PORT" TOKEN="$TOKEN" python3 - <<'PY'
import os
from urllib.request import Request, urlopen
url = f"http://127.0.0.1:{os.environ['PORT']}/api/me/progress/pod-41/pod41-ep-001"
with urlopen(Request(url, method='PATCH', data=b'{"currentTime":36,"duration":36,"isFinished":true}', headers={
    'Authorization': 'Bearer ' + os.environ['TOKEN'], 'Content-Type': 'application/json'})) as response:
    assert response.status in (200, 204)
PY
    click 180 195 .5
    shot latest-filter-menu
    click 220 359 1
    wait_for 'heard episode loaded outside the recent-unheard endpoint' request 'GET /api/podcasts/pod-41/episode/pod41-ep-001 '
    shot latest-heard
    tesseract "$WORK/latest-heard.png" stdout 2>/dev/null | grep -F 'Episode 001' >/dev/null ||
        fail 'Latest Heard did not render the completed episode'
    click 180 195 .5
    click 220 289 1
    shot latest-unheard-after-mark
    unheard_text=$(tesseract "$WORK/latest-unheard-after-mark.png" stdout 2>/dev/null)
    [[ $unheard_text == *'To hear'* && $unheard_text != *'Episode 001'* ]] ||
        fail 'Latest To hear did not exclude the completed episode'
    printf 'Podcast Latest moved a completed episode from To hear to Heard.\n'
    exit 0
fi
if [[ ${SONIX_PODCAST_SMOKE_ART:-0} == 1 ]]; then
    shot latest-art
    assert_show_art latest-art 3000
fi
if [[ ${SONIX_PODCAST_SMOKE_ROW_DOWNLOAD:-0} == 1 ]]; then
    shot latest-before-row-download
    latest_text=$(tesseract "$WORK/latest-before-row-download.png" stdout 2>/dev/null)
    [[ $latest_text == *'Episode 001'* ]] || fail 'Latest did not render the fixture episode'
    if [[ ${SONIX_PODCAST_SMOKE_NO_DATE:-0} == 1 ]]; then
        [[ $latest_text != *2023-11-15* && $latest_text != *1970-* ]] || fail 'Latest rendered a date for an undated episode'
    else
        [[ $latest_text == *2023-11-15* ]] || fail 'Latest did not render the release date'
    fi
    click 426 314 .5
    wait_for 'permission-checked download from the episode row' request 'GET /api/items/pod-41/file/258638762231958/download '
    shot latest-after-row-download
    tesseract "$WORK/latest-after-row-download.png" stdout 2>/dev/null | grep -F 'To hear' >/dev/null ||
        fail 'download did not stay on the Latest list'
    if grep -q 'play requested' "$WORK/first.log"; then fail 'download started playback'; fi
    click 250 282 1.5
    shot latest-row-detail
    tesseract "$WORK/latest-row-detail.png" stdout 2>/dev/null | grep -F 'Mark as heard' >/dev/null ||
        fail 'tapping the episode did not open its details'
    printf 'Podcast row action queued a download without starting playback; tapping the row opened details.\n'
    exit 0
fi
if [[ ${SONIX_PODCAST_SMOKE_DETAIL_QUEUE:-0} == 1 ]]; then
    click 426 434 .5
    held_file=$(python3 -c 'import hashlib; print(int(hashlib.sha256(b"pod-41:2").hexdigest()[:12], 16))')
    wait_for 'first transfer held open' request "GET /api/items/pod-41/file/$held_file/download "
    click 250 282 1.5
    click 250 489 .9
    shot detail-queued
    grep_label detail-queued Queued || fail 'detail did not show Queued behind another transfer'
    click 43 88 .8
    shot latest-queued-and-active
    click 250 282 .8
    rm "$WORK/media/hold-podcast-download"
    wait_for 'second episode started transferring' request 'GET /api/items/pod-41/file/258638762231958/download '
    sleep 1
    shot detail-downloading
    grep_label detail-downloading Downloading || fail 'detail did not show Downloading'
    episode_done() {
        [[ $(sqlite3 -cmd '.timeout 500' "$CARD/.local/audiobookshelf-downloads.db" \
            "SELECT state FROM DOWNLOAD_JOB WHERE episode_id='pod41-ep-001'") == DONE ]]
    }
    wait_for 'second episode verified on card' episode_done
    sleep 1
    shot detail-downloaded
    grep_label detail-downloaded Downloaded && grep_label detail-downloaded 'Play local copy' ||
        fail 'detail did not show the verified local copy'
    if grep -q 'play requested' "$WORK/first.log"; then fail 'download started playback'; fi
    printf 'Podcast details showed Queued, Downloading and Downloaded without starting playback.\n'
    exit 0
fi
click 250 282 1.5
wait_for episode request 'GET /api/podcasts/pod-41/episode/pod41-ep-001'
shot detail
if [[ ${SONIX_PODCAST_SMOKE_INTERRUPT:-0} == 1 ]]; then
    [[ $FORMAT == mp3 ]] || fail 'interrupted-stream regression requires MP3'
    touch "$WORK/media/interrupt-podcast"
fi
if [[ ${SONIX_PODCAST_SMOKE_SYNC:-0} == 1 ]]; then touch "$WORK/progress.lose-session-reply"; fi
if [[ ${SONIX_PODCAST_SMOKE_ART:-0} == 1 ]]; then
    click 250 525 .4
else
    click 250 386 .4
fi
wait_for 'streaming audio request' request 'GET /api/items/pod-41/file/258638762231958 '
if [[ ${SONIX_PODCAST_SMOKE_FROM_MUSIC:-0} == 1 ]]; then
    wait_for 'episode replaced music' grep -Eq 'starting .*abs-episode-stream' "$WORK/first.log"
    sleep 1
    shot music-to-podcast
    tesseract "$WORK/music-to-podcast.png" stdout 2>/dev/null | grep -F 'Fixture Forty-One Podcast' >/dev/null ||
        fail 'Now Playing did not immediately show the podcast title'
    if [[ ${SONIX_PODCAST_SMOKE_ART:-0} == 1 ]]; then
        assert_show_art music-to-podcast
    else
        ffmpeg -v error -i "$WORK/music-to-podcast.png" -pix_fmt rgb24 -f rawvideo - | python3 -c '
import sys
pixels = sys.stdin.buffer.read()
assert not any(r > 225 and g < 40 and b > 155 for r, g, b in zip(pixels[::3], pixels[1::3], pixels[2::3])), "previous music cover still rendered"
'
    fi
    printf 'Podcast replaced music in Now Playing without reopening the player.\n'
    exit 0
fi
wait_for 'streaming decoder' grep -q 'PCM open' "$WORK/first.log"
cache=$(find "$CARD/.local" -name "abs-episode-stream-*.$FORMAT" -print -quit)
[[ -n $cache ]] || fail 'temporary episode audio is missing'
cached_bytes=$(stat -c %s "$cache")
media_bytes=$(stat -c %s "$MEDIA")
if [[ $FORMAT == mp3 || ${SONIX_PODCAST_SMOKE_FASTSTART:-0} == 1 ]]; then
    [[ $cached_bytes -lt $media_bytes ]] ||
        fail 'the decoder did not open before the episode finished transferring'
else
    cmp "$cache" "$MEDIA" || fail 'AAC playback began before the tail metadata arrived'
fi
printf '%s: %d of %d audio bytes present at decoder startup\n' "$FORMAT" "$cached_bytes" "$media_bytes"
awk '/^VmHWM:/ { print "Simulator peak RSS at decoder startup:", $2, $3 }' "/proc/$PLAYER/status"
if [[ ${SONIX_PODCAST_SMOKE_SLEEP:-0} == 1 ]]; then
    wait_for 'Audiobookshelf timer started' grep -q 'sleeptimer: 15 min' "$WORK/first.log"
    xdotool key --window "$WINDOW" n
    wait_for 'episode paused' grep -q 'paused (stream dropped' "$WORK/first.log"
    xdotool key --window "$WINDOW" n
    wait_for 'Audiobookshelf timer restarted after pause and play' awk '
        /sleeptimer: 15 min/ {starts++} END {exit(starts != 2)}' "$WORK/first.log"
    printf 'System sleep settings: all timer presets shown, selection saved, and timer restarted after pause and play.\n'
fi
if [[ ${SONIX_PODCAST_SMOKE_HANDOFF:-0} == 1 ]]; then
    if request 'GET /api/items/pod-41/file/258638762231958/download '; then fail 'streaming implicitly queued offline audio'; fi
    sleep .6 # Let the player sheet finish opening before dismissing it.
    click 43 56 1
    shot handoff-detail
    click 250 489 1
    shot handoff-download
    wait_for 'download requested while playing' request 'GET /api/items/pod-41/file/258638762231958/download '
    wait_for 'local decoder handoff' grep -q 'continued Audiobookshelf episode from verified local audio' "$WORK/first.log"
    handoff_position=$(awk '/continued Audiobookshelf episode from verified local audio/ {print $(NF-1); exit}' "$WORK/first.log")
    grep 'continued Audiobookshelf episode from verified local audio' "$WORK/first.log"
    [[ $(grep -c 'PCM open on' "$WORK/first.log") == 1 && $(grep -c 'play requested' "$WORK/first.log") == 1 ]] ||
        fail 'handoff restarted the device or track'
    xdotool mousemove --window "$WINDOW" 465 360 mousedown 1 sleep .15 mousemove --window "$WINDOW" 130 360 sleep .1 mouseup 1
    sleep 1
    shot handoff-player
    click 240 732 1
    wait_for 'paused after handoff' grep -q 'paused (stream dropped' "$WORK/first.log"
    wait_for 'episode history upload' test -s "$WORK/progress.sessions.json"
    grep -Eq 'format +: S16_LE' "$WORK/first.log"
    grep -Eq 'channels +: 1$' "$WORK/first.log"
    grep -Eq 'rate +: 44100$' "$WORK/first.log"
    python3 - "$WORK/handoff.raw" "$handoff_position" <<'PY'
import array, pathlib, sys
samples = array.array('h', pathlib.Path(sys.argv[1]).read_bytes())
if sys.byteorder != 'little':
    samples.byteswap()
rate = 44100
center = int(float(sys.argv[2]) * rate)
assert center >= rate // 2 and len(samples) >= center + rate // 2, 'short PCM capture around handoff'
window = samples[center - rate // 2:center + rate // 2]
for start in range(0, len(window), rate // 100):
    assert max(map(abs, window[start:start + rate // 100])) > 10, '10 ms dropout around handoff'
assert max(abs(b-a) for a,b in zip(window, window[1:])) < 20, 'sample discontinuity around handoff'
PY
    python3 - "$WORK/progress.sessions.json" <<'PY'
import json, sys
sessions = list(json.load(open(sys.argv[1])).values())
assert len(sessions) == 1 and sessions[0]['episodeId'] == 'pod41-ep-001', sessions
PY
    printf 'Podcast %s handoff kept continuous captured PCM and one listening session.\n' "$FORMAT"
    exit 0
fi
if [[ ${SONIX_PODCAST_SMOKE_INTERRUPT:-0} != 1 ]]; then
    sleep 4
    click 240 732 1
fi
saved_position() {
    sqlite3 "$DB" "select seconds from ABS_EPISODE_PROGRESS where episode='pod41-ep-001'" 2>/dev/null | awk '$1>1 && $1<36{ok=1} END{exit !ok}'
}
wait_for 'streamed position persisted' saved_position
if [[ ${SONIX_PODCAST_SMOKE_INTERRUPT:-0} == 1 ]]; then
    wait_for 'interrupted cache unpublished' test ! -e "$cache"
    [[ $(sqlite3 "$DB" "select finished from ABS_EPISODE_PROGRESS where episode='pod41-ep-001'") == 0 ]] ||
        fail 'the interrupted episode was marked played'
    printf 'Podcast smoke passed: streaming disconnect retained an unfinished checkpoint.\n'
    exit 0
fi
wait_for 'episode progress upload' request 'PATCH /api/me/progress/pod-41/pod41-ep-001 '
wait_for 'episode history upload' test -s "$WORK/progress.sessions.json"
python3 - "$WORK/progress.sessions.json" <<'PY'
import json, sys
sessions = list(json.load(open(sys.argv[1])).values())
assert any(s['episodeId'] == 'pod41-ep-001' and s['mediaType'] == 'podcast'
           and 0.1 < s['timeListening'] < 8 for s in sessions), sessions
PY
if [[ ${SONIX_PODCAST_SMOKE_SYNC:-0} == 1 ]]; then
    wait_for 'history committed before simulated lost reply' test -s "$WORK/progress.session-reply-lost"
    history_acked() {
        [[ $(sqlite3 -cmd '.timeout 500' "$DB" "select count(*) from ABS_HISTORY where id='$(<"$WORK/progress.session-reply-lost")'") == 0 ]]
    }
    wait_for 'original session acknowledged after retry' history_acked
    python3 - "$WORK" <<'PY'
import json, sys
from pathlib import Path
p = Path(sys.argv[1])
assert list(json.loads((p / 'progress.sessions.json').read_text())) == [(p / 'progress.session-reply-lost').read_text()]
PY
fi
shot streamed
if [[ ${SONIX_PODCAST_SMOKE_ART:-0} == 1 ]]; then assert_show_art streamed; fi
click 43 56 1
shot return-detail
play_requests_before_download=$(grep -c 'play requested' "$WORK/first.log")
if [[ ${SONIX_PODCAST_SMOKE_ART:-0} == 1 ]]; then
    click 250 630 1.1
else
    click 250 489 1.1
fi
shot download-action
wait_for 'permission-checked download' request 'GET /api/items/pod-41/file/258638762231958/download '
downloaded() { [[ -n $(find "$CARD/Podcast/ABS" -name .abs-episode -print -quit 2>/dev/null) ]]; }
wait_for 'published local episode' downloaded
episode_job_done() {
    [[ $(sqlite3 -cmd '.timeout 500' "$CARD/.local/audiobookshelf-downloads.db" "SELECT state FROM DOWNLOAD_JOB WHERE episode_id='pod41-ep-001'") == DONE ]]
}
wait_for 'completed episode job' episode_job_done
sleep .6
[[ $(grep -c 'play requested' "$WORK/first.log") == "$play_requests_before_download" ]] ||
    fail 'the explicit download action started playback'
shot download
if [[ ${SONIX_PODCAST_SMOKE_LABELS:-0} == 1 ]]; then
    marker=$(find "$CARD/Podcast/ABS" -name .abs-episode -print -quit)
    grep -Fx 'show_title=Fixture Forty-One Podcast' "$marker" >/dev/null
    sed -i 's/^show_title=.*/show_title=/' "$marker"
    # Return from Latest to Shows and select the show containing the local copy.
    click 43 88 1
    click 43 88 1
    click 250 200 1.4
    click 250 393 1.4
    wait_for 'show episode list' request 'GET /api/items/pod-41?expanded=1 '
    click_show_control Sort
    shot label-sort-menu
    sort_y=$(tesseract "$WORK/label-sort-menu.png" stdout tsv 2>/dev/null | awk -F '\t' '$12=="Ascending" {print int($8+$10/2-50); exit}')
    [[ -n $sort_y ]] || fail 'could not find Ascending'
    click 220 "$sort_y" 1
    shot downloaded-in-show
    tesseract "$WORK/downloaded-in-show.png" stdout 2>/dev/null | grep -F 'Downloaded' >/dev/null || fail 'show row did not mark the download'
    click 43 88 1
    click 43 88 1
    click 43 88 1
    cp "$WORK/progress.requests" "$WORK/before-download-browse.requests"
    click 250 418 1.4
    shot legacy-show-title
    cmp "$WORK/before-download-browse.requests" "$WORK/progress.requests" || fail 'opening Downloads contacted ABS'
    grep_label legacy-show-title 'Unknown show' || fail 'legacy download did not use the local fallback'
    click 250 282 1.2
    shot online-local-show
    click 250 178 1.2
    shot online-local-detail
    grep_label online-local-detail 'Play local copy' || fail 'download detail did not offer local playback'
    click 250 385 1.2
    wait_for 'download played from the card while connected' grep -Eq 'play requested .*Podcast/ABS/' "$WORK/first.log"
    cmp "$WORK/before-download-browse.requests" "$WORK/progress.requests" || fail 'browsing or starting a downloaded episode contacted ABS'
    printf 'Podcast Downloads: local show and episode detail, legacy fallback, and local playback without browse requests.\n'
    exit 0
fi
if [[ ${SONIX_PODCAST_SMOKE_ART:-0} == 1 ]]; then
    sidecar=$(find "$CARD/Podcast/ABS" -name .abs-show.jpg -print -quit)
    cmp "$WORK/media/podcast-cover.jpg" "$sidecar"
    # Prove the offline player reads the download's art, not the browse cache.
    rm "$CARD"/.local/abs-cover-cache/*-detail.jpg
fi
kill "$PLAYER"; wait "$PLAYER" || true; PLAYER=
if [[ ${SONIX_PODCAST_SMOKE_SYNC:-0} == 1 ]]; then
    touch "$WORK/progress.offline"
else
    kill "$MOCK"; wait "$MOCK" || true; MOCK=
fi
mkdir -p "$CARD/Podcast/Native Podcast"
cp "$MEDIA" "$CARD/Podcast/Native Podcast/Native Episode.$FORMAT"
# Reopen the same card with the server unreachable. Remember Track is off.
start_player offline
click 356 160
click 124 704 1.2
click 250 418 1.2
shot offline-downloads
click 250 282 1.2
shot offline-show
click 250 178 1.2
shot offline-detail
if [[ ${SONIX_PODCAST_SMOKE_ART:-0} == 1 ]]; then
    click 250 525 1.2
else
    click 250 385 1.2
fi
wait_for 'offline playback from saved position' grep -Eq 'play requested .*Podcast/ABS/.* from [1-9][0-9]*\.[0-9]+ s' "$WORK/offline.log"
wait_for 'offline decoder' grep -q 'PCM open' "$WORK/offline.log"
if [[ ${SONIX_PODCAST_SMOKE_SLEEP:-0} == 1 ]]; then
    wait_for 'timer started on the downloaded episode offline after restart' grep -q 'sleeptimer: 15 min' "$WORK/offline.log"
fi
sleep 2
click 240 732 1
wait_for 'offline position persisted' saved_position
shot offline-playing
if [[ ${SONIX_PODCAST_SMOKE_ART:-0} == 1 ]]; then assert_show_art offline-playing; fi
if [[ ${SONIX_PODCAST_SMOKE_SYNC:-0} == 1 ]]; then
    wait_for 'offline checkpoint pending' bash -c '[[ $(sqlite3 -cmd ".timeout 500" "$1" "select dirty from ABS_EPISODE_PROGRESS") == 1 ]]' _ "$DB"
    position=$(sqlite3 "$DB" 'select seconds from ABS_EPISODE_PROGRESS')
    kill "$PLAYER"; wait "$PLAYER" || true; PLAYER=
    rm "$WORK/progress.offline"
    start_player reconnect
    expect_remote() {
        PORT="$PORT" TOKEN="$TOKEN" POSITION="$1" FINISHED="$2" python3 - <<'PY'
import json, os, time
from urllib.request import Request, urlopen
url = f"http://127.0.0.1:{os.environ['PORT']}/api/me/progress/pod-41/pod41-ep-001"
req = Request(url, headers={'Authorization': 'Bearer ' + os.environ['TOKEN']})
for _ in range(240):
    with urlopen(req, timeout=3) as f:
        state = json.load(f)
    if (state['isFinished'] == (os.environ['FINISHED'] == '1')
            and abs(state['currentTime'] - float(os.environ['POSITION'])) < 0.03):
        break
    time.sleep(.1)
else:
    raise AssertionError(f"remote checkpoint did not converge: {state}")
PY
    }
    expect_remote "$position" 0
    click 356 160
    click 124 704 1.2
    click 250 418 1.2
    click 250 282 1.2
    click 250 178 1.2
    shot reconnect-detail
    art_offset=0
    if [[ ${SONIX_PODCAST_SMOKE_ART:-0} == 1 ]]; then art_offset=140; fi
    click 250 "$((277 + art_offset))" 1
    expect_remote 36 1
    click 250 "$((277 + art_offset))" 1
    expect_remote 0 0
    PORT="$PORT" TOKEN="$TOKEN" python3 - <<'PY'
import os
from urllib.request import Request, urlopen
url = f"http://127.0.0.1:{os.environ['PORT']}/api/me/progress/pod-41/pod41-ep-001"
req = Request(url, method='PATCH', data=b'{"currentTime":12,"duration":36,"isFinished":false}',
              headers={'Authorization': 'Bearer ' + os.environ['TOKEN'], 'Content-Type': 'application/json'})
with urlopen(req, timeout=3) as response:
    assert response.status in (200, 204)
PY
    # A Downloads action uses the last local checkpoint, not a fresh read
    # from the server. The preceding reconnect/mark checks still cover sync.
    click 250 "$((385 + art_offset))" 1.2
    wait_for 'local Play uses the reset checkpoint' grep -Eq "play requested '.*Podcast/ABS/[^']+'$" "$WORK/reconnect.log"
    if grep -Eq 'play requested .*Podcast/ABS/.* from 12\.0 s' "$WORK/reconnect.log"; then
        fail 'local Play adopted the server position'
    fi
    click 240 732 .6
    checkpoint_before_removal=$(sqlite3 -cmd '.timeout 500' "$DB" 'select seconds from ABS_EPISODE_PROGRESS')
    awk -v seconds="$checkpoint_before_removal" 'BEGIN { exit !(seconds > 0 && seconds < 12) }' ||
        fail 'local playback did not save its own checkpoint'
    history_before_removal=$(sqlite3 -cmd '.timeout 500' "$DB" 'select count(*) from ABS_HISTORY')
    local_media=$(find "$CARD/Podcast/ABS" -type f -name "*.$FORMAT" -print -quit)
    [[ -n $local_media && -s $local_media ]] || fail 'expected the published episode before removal'
    kill "$PLAYER"; wait "$PLAYER" || true; PLAYER=
    start_player removal
    click 356 160
    click 124 704 1.2
    click 250 418 1.2
    click 427 314 1
    shot removal-menu
    click 350 463 1.1
    shot removal-confirmation
    wait_for 'bulk removal confirmation' grep -q 'CONFIRMDUMP ok ' "$WORK/removal.log"
    read -r confirm_x confirm_y < <(awk '/CONFIRMDUMP ok / {split($(NF-1),x,"="); split($NF,y,"="); print x[2],y[2]; exit}' "$WORK/removal.log")
    click "$confirm_x" "$confirm_y" 1
    wait_for 'download unpublished' bash -c '! find "$1" -type f -name .abs-episode | grep -q .' _ "$CARD/Podcast/ABS"
    wait_for 'episode audio removed' test ! -e "$local_media"
    shot removal-empty
    tesseract "$WORK/removal-empty.png" stdout 2>/dev/null | grep -iF 'No results' >/dev/null ||
        fail 'empty podcast Downloads view did not render after removal'
    [[ -s "$CARD/Podcast/Native Podcast/Native Episode.$FORMAT" ]] || fail 'removing ABS downloads deleted native media'
    history_after_removal=$(sqlite3 -cmd '.timeout 500' "$DB" 'select count(*) from ABS_HISTORY')
    [[ $history_after_removal -ge $history_before_removal ]] || fail 'removing ABS downloads deleted listening history'
    if [[ ${SONIX_PODCAST_SMOKE_ART:-0} == 1 ]]; then
        wait_for 'downloaded art removed with the episode' test ! -e "$sidecar"
    fi
    checkpoint_after_removal=$(sqlite3 -cmd '.timeout 500' "$DB" 'select seconds from ABS_EPISODE_PROGRESS')
    [[ $checkpoint_after_removal == "$checkpoint_before_removal" ]] ||
        fail 'removing the download changed the episode checkpoint'
    printf 'Podcast sync passed: lost history reply, offline upload, heard/unheard, local resume, and removal preserving progress.\n'
fi
if [[ ${SONIX_PODCAST_SMOKE_RSS:-0} == 1 ]]; then
    click 43 56
    for _ in 1 2 3 4; do click 43 88; done
    click 360 500 1
    shot native-podcasts
    click 240 156 1
    shot native-card
    click 250 309 1
    shot native-show
    click 250 370 1
    wait_for 'native podcast playback' grep -Eq 'play requested .*Podcast/Native Podcast/Native Episode' "$WORK/offline.log"
    [[ $(sqlite3 "$DB" 'select count(*) from ABS_EPISODE_PROGRESS') == 1 ]] ||
        fail 'native podcast wrote an Audiobookshelf checkpoint'
    printf 'Native on-card podcast playback passed without an extra ABS checkpoint.\n'
fi
printf 'Podcast smoke passed (%s): catalogue, Wi-Fi playback, queued download, offline restart/playback.\n' "$FORMAT"