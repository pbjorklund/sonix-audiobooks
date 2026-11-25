#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
WORK=$(mktemp -d "$ROOT/build/tidal-downloads-ui.XXXXXX")
CARD=$WORK/card
PLAYER='' X='' MOCK=''
cleanup() {
    for pid in "$PLAYER" "$X" "$MOCK"; do
        if [[ -n $pid ]]; then kill "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true; fi
    done
    printf 'TIDAL Downloads UI evidence: %s\n' "$WORK"
}
trap cleanup EXIT
mkdir -p "$WORK/run/usr/resource" "$CARD/.local/tidal-cache"
cp -a "$ROOT/sonix-packer/assets/R1/usr/resource/." "$WORK/run/usr/resource/"
if [[ ${1:-} != empty && ${1:-} != favourites && ${1:-} != saved && ${1:-} != local && ${1:-} != cache && ${1:-} != quality ]]; then
    for id in 21 22; do
        album=$CARD/Music/Tidal/Fixture/Album-$id
        mkdir -p "$album"
        ffmpeg -nostdin -v error -f lavfi -i 'sine=duration=1' "$album/Track.mp3"
        printf 'title=Track %s\nartist=Fixture\nalbum_artist=Fixture\nalbum=Album %s\nalbum_id=%s\ntidal_id=%s\ntrack=1\n' "$id" "$id" "$id" "$id" >"$album/Track.mp3.tags"
        printf 'Fixture/Album-%s/Track.mp3\n' "$id" >"$CARD/.local/tidal-cache/$id.keep"
    done
    ffmpeg -nostdin -v error -f lavfi -i 'color=c=0xff00cc:s=192x192' -frames:v 1 -threads 1 "$CARD/Music/Tidal/Fixture/Album-21/cover.jpg"
    album=$CARD/Music/Tidal/Fixture/Album-21
    cp "$album/Track.mp3" "$album/Second.mp3"
    printf 'title=Second fixture track\nartist=Fixture\nalbum=Album 21\nalbum_id=21\ntidal_id=25\ntrack=2\n' >"$album/Second.mp3.tags"
    printf 'Fixture/Album-21/Second.mp3\n' >"$CARD/.local/tidal-cache/25.keep"
fi
cat >"$WORK/config.ini" <<INI
[ui]
language = English
[clock]
last_seen = $(date +%s)
configured = 1
INI
if [[ ${1:-} == favourites || ${1:-} == saved || ${1:-} == local || ${1:-} == cache || ${1:-} == quality ]]; then
    ffmpeg -nostdin -v error -f lavfi -i 'sine=duration=2' "$WORK/fixture.flac"
    python3 "$ROOT/tools/tidal-favourites-fixture.py" "$WORK" >"$WORK/mock.log" 2>&1 &
    MOCK=$!
    for ((i=0; i<100; i++)); do [[ ! -s $WORK/port ]] || break; sleep .1; done
    [[ -s $WORK/port ]]
    printf '\n[streaming]\nkeys_file = %s/keys.ini\n[tidal]\nbase_url = http://127.0.0.1:%s\ncollection_url = http://127.0.0.1:%s\n' "$WORK" "$(<"$WORK/port")" "$(<"$WORK/port")" >>"$WORK/config.ini"
    printf '[tidal]\nclient_id=fixture\nclient_secret=fixture\n' >"$WORK/keys.ini"
    printf 'access_token=fixture\nrefresh_token=fixture\nexpires_at=%s\ncountry=SE\nuser_id=1\n' "$(( $(date +%s) + 3600 ))" >"$WORK/tidal_session.ini"
fi
Xvfb -nolisten unix -listen tcp -ac -displayfd 3 -screen 0 600x900x24 >"$WORK/x.log" 2>&1 3>"$WORK/display" &
X=$!
for ((i=0; i<100; i++)); do
    [[ ! -s $WORK/display ]] || break
    sleep .1
done
[[ -s $WORK/display ]]
DISPLAY="localhost:$(<"$WORK/display")"
export DISPLAY SDL_VIDEODRIVER=x11
(cd "$WORK/run"; exec env SONIX_SD_ROOT="$CARD" SONIX_CONFIG="$WORK/config.ini" SONIX_LOG="$WORK/player.log" SONIX_CONFIRM_DUMP=1 "$ROOT/sonix-player/sonix_player_host") >"$WORK/player.stdout" 2>&1 &
PLAYER=$!
WINDOW=
for ((i=0; i<100; i++)); do
    WINDOW=$(xdotool search --onlyvisible --pid "$PLAYER" 2>/dev/null | tail -n1 || true)
    [[ -z $WINDOW ]] || break
    sleep .1
done
[[ -n $WINDOW ]]
sleep 1
click() { xdotool mousemove --window "$WINDOW" "$1" "$2" click 1; sleep .8; }
shot() { scrot -o "$WORK/$1.png"; }
assert_label() {
    tesseract "$WORK/$1.png" stdout 2>/dev/null | grep -F "$2" >/dev/null
}
click 356 160
click 125 219
shot home
if [[ ${1:-} == quality ]]; then
    folder="$CARD/Music/Tidal/Fixture Artist/Fixture Album [21]"
    mkdir -p "$folder"
    old="$folder/01 - Fixture Artist - Fixture Track 21.m4a"
    ffmpeg -nostdin -v error -f lavfi -i 'sine=duration=2' -c:a aac "$old"
    printf 'title=Fixture Track 21\nartist=Fixture Artist\nalbum=Fixture Album\nalbum_id=21\ntidal_id=21\ntrack=1\ndownload_quality=0\n' >"$old.tags"
    printf 'Fixture Artist/Fixture Album [21]/%s\n' "${old##*/}" >"$CARD/.local/tidal-cache/21.keep"
    cp "$WORK/fixture.flac" "$folder/import.flac"
    printf '!Fixture Artist/Fixture Album [21]/import.flac\n' >"$CARD/.local/tidal-cache/22.keep"
    confirm_click() {
        local button=$1 x y
        read -r x y < <(awk -v button="$button" 'NF >= 4 && $(NF-3) == "CONFIRMDUMP" && $(NF-2) == button {
            split($(NF-1), x, "="); split($NF, y, "="); last = x[2] " " y[2]
        } END { if (last) print last }' "$WORK/player.log")
        click "$x" "$y"
    }
    click 180 524
    click 427 194
    shot quality-album-menu
    download_y=$(tesseract "$WORK/quality-album-menu.png" stdout tsv 2>/dev/null | awk -F '\t' '$12 == "Download" && $7 > 240 { y = $8 + $10 / 2 - 50 } END { if (y) print int(y) }')
    [[ -n $download_y ]]
    click 290 "$download_y"
    shot quality-confirm
    assert_label quality-confirm 'Upgrade downloads?'
    confirm_click cancel
    [[ -s $old ]]
    if grep -q 'playbackinfopostpaywall' "$WORK/mock.log"; then exit 1; fi
    click 427 194
    click 290 "$download_y"
    confirm_click ok
    for ((i=0; i<80; i++)); do [[ ! -e $WORK/active-21 ]] || break; sleep .1; done
    [[ -e $WORK/active-21 && -s $old ]]
    touch "$WORK/release-21"
    for ((i=0; i<80; i++)); do [[ -e $old ]] || break; sleep .1; done
    [[ ! -e $old ]]
    upgraded="$CARD/Music/Tidal/$(<"$CARD/.local/tidal-cache/21.keep")"
    grep -qx 'download_quality=2' "$upgraded.tags"
    grep -q 'audioquality=LOSSLESS' "$WORK/mock.log"
    grep -qx '!Fixture Artist/Fixture Album \[21\]/import.flac' "$CARD/.local/tidal-cache/22.keep"
    cmp "$WORK/fixture.flac" "$folder/import.flac"
    if grep -q 'tracks/22/playbackinfopostpaywall' "$WORK/mock.log"; then exit 1; fi
    sleep 3
    click 43 88
    click 437 88
    shot download-quality-settings
    assert_label download-quality-settings 'Download quality'
    ffmpeg -nostdin -v error -f lavfi -i 'sine=duration=2' -c:a aac -movflags +faststart "$WORK/fixture.m4a"
    click 145 607
    grep -Eq '^download_quality *= *0$' "$WORK/config.ini"
    click 43 88
    click 180 418
    click 427 194
    shot quality-track-menu
    track_y=$(tesseract "$WORK/quality-track-menu.png" stdout tsv 2>/dev/null | awk -F '\t' '$12 == "Download" && $7 > 240 { y = $8 + $10 / 2 - 50 } END { if (y) print int(y) }')
    [[ -n $track_y ]]
    click 290 "$track_y"
    shot quality-downgrade
    assert_label quality-downgrade 'Downgrade downloads?'
    confirm_click ok
    for ((i=0; i<80; i++)); do [[ -e $upgraded ]] || break; sleep .1; done
    [[ ! -e $upgraded ]]
    downgraded="$CARD/Music/Tidal/$(<"$CARD/.local/tidal-cache/21.keep")"
    [[ $downgraded == *.m4a ]]
    grep -qx 'download_quality=0' "$downgraded.tags"
    grep -q 'audioquality=LOW' "$WORK/mock.log"
    printf 'TIDAL quality prompts: album cancel and upgrade, track downgrade, old copy retained in flight, linked import untouched.\n'
    exit 0
fi
if [[ ${1:-} == cache ]]; then
    touch "$WORK/fixture-cover"
    mkdir -p "$CARD/.local/qobuz-art"
    cover_key=$(printf %s 'https://resources.tidal.com/images/fixture/cover/id/320x320.jpg' | md5sum | cut -d ' ' -f 1)
    ffmpeg -nostdin -v error -f lavfi -i 'color=c=0xff00cc:s=192x192' -frames:v 1 -threads 1 -c:v mjpeg -f image2 "$CARD/.local/qobuz-art/$cover_key.img"
    wait_started() {
        for ((i=0; i<60; i++)); do [[ ! -e $WORK/favourites-refresh-started ]] || return 0; sleep .1; done
        return 1
    }
    click 180 524
    shot cache-first
    assert_label cache-first 'Fixture Album'
    click 43 88
    touch "$WORK/hold-favourites" "$WORK/updated-favourites"
    click 180 524
    wait_started
    shot cache-while-held
    assert_label cache-while-held 'Fixture Album'
    if assert_label cache-while-held 'Updated Fixture'; then exit 1; fi
    ffmpeg -v error -i "$WORK/cache-while-held.png" -pix_fmt rgb24 -f rawvideo - | python3 -c '
import sys
p = sys.stdin.buffer.read()
assert sum(min(p[(600*y+x)*3:(600*y+x)*3+3]) > 150 for y in range(124,150) for x in range(483,510)) > 20
r, g, b = p[(600 * 245 + 120) * 3:(600 * 245 + 120) * 3 + 3]
assert r > 225 and g < 40 and b > 155, "cached cover missing while catalogue request was blocked"
'
    rm "$WORK/hold-favourites" "$WORK/favourites-refresh-started"
    sleep 1
    shot cache-updated
    assert_label cache-updated 'Updated Fixture'
    click 180 195
    shot cached-album-tracks
    assert_label cached-album-tracks 'Fixture Track 21'
    grep -q 'GET /albums/21/tracks?' "$WORK/mock.log"
    if grep -q 'GET /albums/21?' "$WORK/mock.log"; then
        printf 'Opening an album with known artwork and artist refetched its metadata.\n' >&2
        exit 1
    fi
    click 43 88
    click 43 88
    touch "$WORK/hold-favourites" "$WORK/reject-favourites"
    click 180 524
    wait_started
    rm "$WORK/hold-favourites" "$WORK/favourites-refresh-started"
    sleep 1
    shot cache-on-error
    assert_label cache-on-error 'Updated Fixture'
    if assert_label cache-on-error 'Fixture offline'; then exit 1; fi
    click 43 88
    touch "$WORK/hold-favourites"
    rm "$WORK/reject-favourites"
    click 180 524
    wait_started
    click 43 88
    click 190 200
    rm "$WORK/hold-favourites"
    sleep 1
    shot cache-left-view
    assert_label cache-left-view Downloads
    if assert_label cache-left-view 'Updated Fixture'; then exit 1; fi
    printf 'TIDAL cached lists: immediate rows, small spinner, update, server error and stale response passed.\n'
    exit 0
fi
if [[ ${1:-} == local ]]; then
    touch "$WORK/bulk-albums"
    folder="$CARD/Music/Tidal/Fixture Artist/Fixture Album [21]"
    second="$CARD/Music/Tidal/Fixture Artist/Second Album [31]"
    mkdir -p "$folder" "$second"
    cp "$WORK/fixture.flac" "$folder/01 - Fixture Artist - Fixture Track 21.flac"
    cp "$WORK/fixture.flac" "$second/01 - Fixture Artist - Fixture Track 31.flac"
    click 180 524
    shot local-found
    assert_label local-found 'Folder found on SD'
    [[ ! -e $CARD/.local/tidal-cache/21.keep ]]
    click 430 88
    shot grouping-menu
    grouping_y=$(tesseract "$WORK/grouping-menu.png" stdout tsv 2>/dev/null | awk -F '\t' '$12 == "Group" && $7 > 240 { print int($8+$10/2-50); exit }')
    [[ -n $grouping_y ]]
    click 320 "$grouping_y"
    shot grouping-before-link
    assert_label grouping-before-link 'Not downloaded'
    click 430 88
    shot local-menu
    assert_label local-menu 'Auto-link albums'
    click 295 163
    for ((i=0; i<60; i++)); do [[ ! -e $CARD/.local/tidal-cache/21.keep ]] || break; sleep .1; done
    grep -F '!Fixture Artist/Fixture Album [21]/' "$CARD/.local/tidal-cache/21.keep" >/dev/null
    [[ ! -e $CARD/.local/tidal-cache/22.keep && ! -e $CARD/.local/tidal-cache/41.keep ]]
    grep -F '!Fixture Artist/Second Album [31]/' "$CARD/.local/tidal-cache/31.keep" >/dev/null
    cmp "$WORK/fixture.flac" "$folder/01 - Fixture Artist - Fixture Track 21.flac"
    cmp "$WORK/fixture.flac" "$second/01 - Fixture Artist - Fixture Track 31.flac"
    if grep -Eq 'GET /(audio/|tracks/.*/playbackinfopostpaywall|albums/(21|31)\?|albums/41)' "$WORK/mock.log"; then exit 1; fi
    sleep 3
    shot grouped-local
    tesseract "$WORK/grouped-local.png" stdout tsv 2>/dev/null | awk -F '\t' '
        $12 == "Second" { full = $8 }
        $12 == "Fixture" && $10 > 13 && !partial { partial = $8 }
        $12 == "Missing" { missing = $8 }
        END { exit !(full && partial > full && missing > partial) }
    '
    assert_label grouped-local 'Partly downloaded'
    assert_label grouped-local 'Not downloaded'
    click 430 88
    click 320 "$grouping_y"
    shot ungrouped-local
    if assert_label ungrouped-local 'Partly downloaded'; then exit 1; fi
    click 43 88
    click 190 200
    shot local-downloads
    assert_label local-downloads Downloads
    assert_label local-downloads 'Fixture Artist'
    click 240 320
    shot local-linked-track
    assert_label local-linked-track 'Fixture Track 21'
    if assert_label local-linked-track 'Fixture Track 22'; then exit 1; fi
    printf 'Favourite Albums: auto-link added matching on-card tracks without media requests.\n'
    exit 0
fi
if [[ ${1:-} == saved ]]; then
    mkdir -p "$CARD/.local"
    sqlite3 "$CARD/.local/tidal-saved.db" "CREATE TABLE saved(id TEXT UNIQUE NOT NULL,title TEXT,artist TEXT,cover TEXT,released TEXT,tracks INTEGER); INSERT INTO saved VALUES('22','Local Bookmark','','','',0);"
    touch "$WORK/reject-saved"
    click 180 309
    shot saved-rejected
    assert_label saved-rejected 403
    rm "$WORK/reject-saved"
    sleep 2
    click 180 309
    shot account-saved
    assert_label account-saved 'Account Saved'
    assert_label account-saved 'Saved Artist'
    assert_label account-saved 'Local Bookmark'
    album="$CARD/Music/Tidal/Saved Artist/Account Saved Album [21]"
    mkdir -p "$album"
    cp "$WORK/fixture.flac" "$album/Imported.flac"
    printf 'tidal_id=21\nalbum_id=21\n' >"$album/Imported.flac.tags"
    click 43 88
    click 180 309
    sleep 1
    shot saved-folder
    assert_label saved-folder 'Folder found on SD'
    click 430 88
    shot saved-link-menu
    assert_label saved-link-menu 'Auto-link albums'
    click 320 156
    sleep 1
    [[ -s "$CARD/.local/tidal-cache/21.keep" ]] || exit 1
    grep -Fx '!Saved Artist/Account Saved Album [21]/Imported.flac' "$CARD/.local/tidal-cache/21.keep"
    if grep -Eq 'GET /audio/|GET /users/1/favorites/albums' "$WORK/mock.log"; then exit 1; fi
    sleep 2 # Let the linked-count notification dismiss before tapping a row.
    click 235 180
    shot saved-album-tracks
    assert_label saved-album-tracks 'Fixture Track 21'
    click 43 88
    click 427 194
    shot saved-menu
    click 295 328
    shot saved-removed
    assert_label saved-removed 'Local Bookmark'
    if assert_label saved-removed 'Account Saved'; then exit 1; fi
    click 43 88
    click 180 524
    shot saved-favourite-unchanged
    assert_label saved-favourite-unchanged 'Fixture Album'
    click 427 194
    shot saved-add-menu
    click 295 355
    sleep 2
    click 43 88
    click 180 309
    shot saved-added
    assert_label saved-added 'Account Saved'
    assert_label saved-added 'Local Bookmark'
    [[ $(grep -c 'POST /userCollectionSaveForLaters/me/relationships/items' "$WORK/mock.log") == 1 ]]
    [[ $(grep -c 'DELETE /userCollectionSaveForLaters/me/relationships/items' "$WORK/mock.log") == 1 ]]
    if grep -Eq '(POST|DELETE) /users/.*/favorites|GET /audio/' "$WORK/mock.log"; then exit 1; fi
    printf 'Saved for later: paginated account albums, local bookmarks, add/remove and API failure passed.\n'
    exit 0
fi
if [[ ${1:-} == favourites ]]; then
    click 180 418
    shot favourites
    click 417 178
    shot track-menu
    click 290 353
    for ((i=0; i<80; i++)); do [[ ! -e $WORK/active-21 ]] || break; sleep .1; done
    [[ -e $WORK/active-21 ]]
    sleep 2
    click 427 290
    shot second-menu
    click 290 449
    sleep 2
    shot queued
    ffmpeg -v error -i "$WORK/queued.png" -pix_fmt rgb24 -f rawvideo - | python3 -c '
import sys
pixels = sys.stdin.buffer.read()
def icon(x1, y1, x2, y2):
    return sum(min(pixels[(600*y+x)*3:(600*y+x)*3+3]) > 150
               for y in range(y1,y2) for x in range(x1,x2))
assert icon(480, 228, 510, 260) > 20, "active spinner not visible"
assert icon(475, 322, 512, 350) > 20, "pending queue icon not visible"
'
    touch "$WORK/release-21"
    for ((i=0; i<80; i++)); do [[ ! -e $WORK/active-22 ]] || break; sleep .1; done
    [[ -e $WORK/active-22 ]]
    sleep 1
    shot first-downloaded
    assert_label first-downloaded Downloaded
    touch "$WORK/release-22"
    for ((i=0; i<80; i++)); do [[ ! -s $CARD/.local/tidal-cache/22.keep ]] || break; sleep .1; done
    [[ -s $CARD/.local/tidal-cache/22.keep ]]
    sleep 2
    shot both-downloaded
    [[ $(tesseract "$WORK/both-downloaded.png" stdout 2>/dev/null | grep -c Downloaded) == 2 ]]
    click 427 194
    shot track-remove-menu
    assert_label track-remove-menu 'Remove download'
    click 290 353
    read -r x y < <(awk '/CONFIRMDUMP ok / {split($(NF-1), x, "="); split($NF, y, "="); print x[2], y[2]; exit}' "$WORK/player.log")
    click "$x" "$y"
    shot track-removed
    [[ ! -e $CARD/.local/tidal-cache/21.keep && -s $CARD/.local/tidal-cache/22.keep ]]
    [[ $(tesseract "$WORK/track-removed.png" stdout 2>/dev/null | grep -c Downloaded) == 1 ]]
    [[ $(grep -c 'GET /users/1/favorites/tracks?' "$WORK/mock.log") == 1 ]]
    click 43 88
    click 180 524
    shot album-partial
    tesseract "$WORK/album-partial.png" stdout 2>/dev/null | grep -E '1 *tracks downloaded' >/dev/null
    click 427 194
    shot album-menu
    assert_label album-menu 'Download album'
    rm "$WORK/active-21" "$WORK/release-21"
    download_y=$(tesseract "$WORK/album-menu.png" stdout tsv 2>/dev/null | awk -F '\t' '$12 == "Download" && $7 > 240 { y = $8 + $10 / 2 - 50 } END { if (y) print int(y) }')
    [[ -n $download_y ]]
    click 290 "$download_y"
    for ((i=0; i<80; i++)); do [[ ! -e $WORK/active-21 ]] || break; sleep .1; done
    [[ -e $WORK/active-21 ]]
    sleep 2
    shot album-active
    sleep 1
    click 220 185
    shot album-track-active
    assert_label album-track-active 'Fixture Track 21'
    ffmpeg -v error -i "$WORK/album-track-active.png" -pix_fmt rgb24 -f rawvideo - | python3 -c '
import sys
p = sys.stdin.buffer.read()
assert sum(min(p[(600*y+x)*3:(600*y+x)*3+3]) > 150 for y in range(228,260) for x in range(480,513)) > 20
'
    touch "$WORK/release-21"
    for ((i=0; i<80; i++)); do [[ ! -s $CARD/.local/tidal-cache/21.keep ]] || break; sleep .1; done
    [[ -s $CARD/.local/tidal-cache/21.keep ]]
    sleep 1.5
    click 43 88
    shot album-complete
    assert_label album-complete Downloaded
    [[ $(grep -c 'GET /users/1/favorites/albums?' "$WORK/mock.log") == 2 ]]
    click 43 88
    cp "$WORK/mock.log" "$WORK/before-download-browse.log"
    click 190 200
    click 240 320
    shot fixture-album-tracks
    assert_label fixture-album-tracks 'Fixture Track 21'
    cmp "$WORK/before-download-browse.log" "$WORK/mock.log"
    click 430 88
    click 245 164
    sleep 1
    shot fixture-album-refreshed
    assert_label fixture-album-refreshed 'Fixture Track 21'
    [[ $(grep -c 'GET /albums/21/tracks?' "$WORK/mock.log") == 3 ]]
    [[ $(grep -c 'GET /audio/' "$WORK/mock.log") == 3 ]]
    printf 'TIDAL favourites and album refresh passed; refreshing did not fetch audio.\n'
    exit 0
fi
click 190 200
shot albums
assert_empty_below_tabs() {
    assert_label "$1" 'No results'
    tesseract "$WORK/$1.png" stdout tsv 2>/dev/null | awk -F '\t' '
$12 ~ /^results/ { empty = $8 }
END { exit !(empty > 318) }' # The second tab row ends at 318 in this capture.
}
if [[ ${1:-} == empty ]]; then
    assert_empty_below_tabs albums
    for xy in '205 179' '335 179' '77 235'; do
        read -r x y <<<"$xy"
        click "$x" "$y"
        shot "empty-$x"
        assert_empty_below_tabs "empty-$x"
    done
    click 43 88
    shot final-back
    assert_label final-back 'Saved for later'
    printf 'TIDAL Downloads: empty-state placement passed on all four tabs.\n'
    exit 0
fi
assert_label albums 'Album 21'
assert_label albums 'Album 22'
ffmpeg -v error -i "$WORK/albums.png" -pix_fmt rgb24 -f rawvideo - | python3 -c '
import sys
p = sys.stdin.buffer.read()
def pixel(x, y):
    at = (600 * y + x) * 3
    return tuple(p[at:at + 3])
def is_cover(rgb):
    r, g, b = rgb
    return r > 225 and g < 40 and b > 155
assert is_cover(pixel(120, 370)) and not is_cover(pixel(120, 466))
'
click 335 179
shot empty
assert_empty_below_tabs empty
click 77 235
shot tracks
assert_label tracks 'Track 21'
if assert_label tracks 'No results'; then exit 1; fi
click 205 179
shot artists
assert_label artists Fixture
click 240 321
shot artist-albums
assert_label artist-albums 'Album 21'
click 43 88
shot artist-back
assert_label artist-back Artists
click 80 179
covers_before=$(grep -c 'cover: decoded jpeg\|cover: decoded progressive jpeg' "$WORK/player.log" || true)
click 240 320
shot album-tracks
covers_after=$(grep -c 'cover: decoded jpeg\|cover: decoded progressive jpeg' "$WORK/player.log" || true)
[[ $((covers_after - covers_before)) == 1 ]] || { printf 'Expected one decode for the shared album cover.\n' >&2; exit 1; }
assert_label album-tracks 'Track 21'
ffmpeg -v error -i "$WORK/album-tracks.png" -pix_fmt rgb24 -f rawvideo - | python3 -c '
import sys
p = sys.stdin.buffer.read()
for y in (245, 341):
    i = (600 * y + 120) * 3
    r, g, b = p[i:i+3]
    assert r > 225 and g < 40 and b > 155, "downloaded tracks did not share the on-card album art"
'
if assert_label album-tracks 'Artists' || assert_label album-tracks 'Playlists'; then
    printf 'Downloads filters leaked into the album track list.\n' >&2
    exit 1
fi
click 430 88
shot album-refresh-menu
if ! assert_label album-refresh-menu Refresh; then
    ffmpeg -v error -i "$WORK/album-refresh-menu.png" -vf 'crop=280:74:247:176' "$WORK/refresh-label.png"
    assert_label refresh-label Refresh
fi
click 230 510
click 43 88
shot album-back
assert_label album-back 'Album 21'
click 43 88
shot final-back
assert_label final-back 'Saved for later'
printf 'TIDAL Downloads: local art and placeholder, empty-state placement, and Back navigation passed.\n'
