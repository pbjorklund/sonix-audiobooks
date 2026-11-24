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
ffmpeg -nostdin -loglevel error -f lavfi -i 'sine=frequency=440:duration=36' -c:a "$codec" -b:a 128k "$MEDIA"
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
cat >"$WORK/asound.conf" <<'EOF'
pcm.!default { type null }
ctl.!default { type hw card 0 }
EOF
Xvfb -nolisten unix -listen tcp -ac -displayfd 3 -screen 0 600x900x24 >"$WORK/x.log" 2>&1 3>"$WORK/display" &
X=$!
wait_for Xvfb test -s "$WORK/display"
export DISPLAY="localhost:$(<"$WORK/display")" SDL_VIDEODRIVER=x11
start_player() {
    local name=$1
    (
        cd "$RUN"
        exec env SONIX_SD_ROOT="$CARD" SONIX_CONFIG="$WORK/config.ini" SONIX_LOG="$WORK/$name.log" \
            ALSA_CONFIG_PATH="$WORK/asound.conf" SONIX_AUDIOBOOKSHELF_PROFILES="$WORK/profiles.ini" \
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
start_player first
click 356 160
click 124 704 1.4
shot root
# Open the podcast library beneath the Audiobooks group.
click 250 736 1
shot library
click 250 200 1.4
wait_for 'show list' request 'GET /api/libraries/lib-pod-2/items?'
shot shows
click 43 88
click 250 307 1.2
wait_for latest request 'GET /api/libraries/lib-pod-2/recent-episodes?'
click 250 177 1.5
wait_for episode request 'GET /api/podcasts/pod-41/episode/pod41-ep-001'
shot detail
if [[ ${SONIX_PODCAST_SMOKE_INTERRUPT:-0} == 1 ]]; then
    [[ $FORMAT == mp3 ]] || fail 'interrupted-stream regression requires MP3'
    touch "$WORK/media/interrupt-podcast"
fi
if [[ ${SONIX_PODCAST_SMOKE_SYNC:-0} == 1 ]]; then touch "$WORK/progress.lose-session-reply"; fi
click 250 386 .4
wait_for 'streaming audio request' request 'GET /api/items/pod-41/file/258638762231958 '
wait_for 'streaming decoder' grep -q 'PCM open' "$WORK/first.log"
cache=$(find "$CARD/.local" -name "abs-episode-stream-*.$FORMAT" -print -quit)
[[ -n $cache ]] || fail 'temporary episode audio is missing'
cached_bytes=$(stat -c %s "$cache")
media_bytes=$(stat -c %s "$MEDIA")
if [[ $FORMAT == mp3 ]]; then
    [[ $cached_bytes -lt $media_bytes ]] ||
        fail 'the MP3 decoder did not open before the episode finished transferring'
else
    cmp "$cache" "$MEDIA" || fail 'AAC playback began before the tail metadata arrived'
fi
printf '%s: %d of %d audio bytes present at decoder startup\n' "$FORMAT" "$cached_bytes" "$media_bytes"
awk '/^VmHWM:/ { print "Simulator peak RSS at decoder startup:", $2, $3 }' "/proc/$PLAYER/status"
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
        [[ $(sqlite3 "$DB" "select count(*) from ABS_HISTORY where id='$(<"$WORK/progress.session-reply-lost")'") == 0 ]]
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
click 43 56 1
shot return-detail
click 250 489 1.1
shot download-action
wait_for 'permission-checked download' request 'GET /api/items/pod-41/file/258638762231958/download '
downloaded() { [[ -n $(find "$CARD/Podcast/ABS" -name .abs-episode -print -quit 2>/dev/null) ]]; }
wait_for 'published local episode' downloaded
shot download
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
click 250 178 1.2
shot offline-detail
click 250 385 1.2
wait_for 'offline playback from saved position' grep -Eq 'play requested .*Podcast/ABS/.* from [1-9][0-9]*\.[0-9]+ s' "$WORK/offline.log"
wait_for 'offline decoder' grep -q 'PCM open' "$WORK/offline.log"
sleep 2
click 240 732 1
wait_for 'offline position persisted' saved_position
shot offline-playing
if [[ ${SONIX_PODCAST_SMOKE_SYNC:-0} == 1 ]]; then
    wait_for 'offline checkpoint pending' bash -c '[[ $(sqlite3 "$1" "select dirty from ABS_EPISODE_PROGRESS") == 1 ]]' _ "$DB"
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
    click 250 178 1.2
    shot reconnect-detail
    click 250 277 1
    expect_remote 36 1
    click 250 277 1
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
    click 250 385 1.2
    wait_for 'other client position on local Play' grep -Eq 'play requested .*Podcast/ABS/.* from 12\.0 s' "$WORK/reconnect.log"
    click 240 732 .6
    kill "$PLAYER"; wait "$PLAYER" || true; PLAYER=
    start_player removal
    click 356 160
    click 124 704 1.2
    click 250 418 1.2
    click 250 178 1.2
    click 250 489 1
    wait_for 'download unpublished' bash -c '! find "$1" -type f -name .abs-episode | grep -q .' _ "$CARD/Podcast/ABS"
    sqlite3 "$DB" 'select seconds from ABS_EPISODE_PROGRESS' | awk '$1>=12{ok=1} END{exit !ok}' ||
        fail 'removing the download lost the episode checkpoint'
    printf 'Podcast sync passed: lost history reply, offline upload, heard/unheard, position from another client, and removal preserving progress.\n'
fi
if [[ ${SONIX_PODCAST_SMOKE_RSS:-0} == 1 ]]; then
    click 43 56
    for _ in 1 2 3; do click 43 88; done
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