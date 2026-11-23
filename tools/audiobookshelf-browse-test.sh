#!/usr/bin/env bash
# Focused §10.3 host tests for the M2 WS4 query work (Bob spec §10.1).
#
#   * offline: tools/audiobookshelf-browse-test.c with a stubbed http_request
#     pins the exact request mapping (sort fields, desc literal, zero-based
#     pages, limit/minified/collapseseries) and the padded-Base64 + URL-encode
#     filter construction; response/truncation/ownership run on canned JSON.
#
#   * live: the same binary compiled against the real HTTP stack, driven once
#     per checked-in fixture alias (pinned-3563d49, current-5de038d,
#     eric-v2.38.0) against tools/audiobookshelf_mock.py. The mock enforces
#     provenance at load, so a pass proves the queries against verified
#     fixture bytes; the request log is then grepped for the exact query
#     strings the offline run pinned.
#
# No GUI, no Xvfb, no ffmpeg: this exercises the backend API functions only.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
PLAYER="$ROOT/sonix-player"
WORK="${SONIX_ABS_BROWSE_DIR:-$ROOT/build/audiobookshelf-browse-test}"
UNIT_WORK=$(mktemp -d)
ALIAS=""
PORT_FILE=""
MOCK_PID=""
trap 'rm -rf -- "$UNIT_WORK"; [[ -n "$MOCK_PID" ]] && kill -TERM "$MOCK_PID" 2>/dev/null || true' EXIT

fail() { printf 'browse test: %s\n' "$*" >&2; exit 1; }
need() { command -v "$1" >/dev/null 2>&1 || fail "missing required command: $1"; }
need gcc
need python3

# §14.6 alias gate, matching the other ABS smokes: SONIX_ABS_FIXTURE picks one
# alias, the default run walks all three.
ALIASES="pinned-3563d49 current-5de038d eric-v2.38.0"
if [[ -n "${SONIX_ABS_FIXTURE:-}" ]]; then
	case "$SONIX_ABS_FIXTURE" in
		pinned-3563d49|current-5de038d|eric-v2.38.0) ALIASES="$SONIX_ABS_FIXTURE" ;;
		*) fail "unknown SONIX_ABS_FIXTURE: $SONIX_ABS_FIXTURE" ;;
	esac
fi

COMPILE_FLAGS=(-O1 -g -DHOST_BUILD=1 -D_FILE_OFFSET_BITS=64 -I. -Ilvgl
	-DLV_CONF_INCLUDE_SIMPLE=1 -ffunction-sections -fdata-sections
	-fsanitize=address,undefined -fno-omit-frame-pointer)
LINK_FLAGS=(-Wl,--gc-sections -lm -lpthread -ldl)

case "$WORK" in "$ROOT"/build/*) rm -rf -- "$WORK" ;; *) fail "unsafe output directory: $WORK" ;; esac
mkdir -p "$WORK/media"

cd "$PLAYER"

# ---------------------------------------------------------------------------
# Offline: construction, Base64/URL encoding, response, truncation, ownership
# ---------------------------------------------------------------------------
# The stub replaces http_request()/http_last_error(), but the REAL
# http_url_encode() must be exercised: the "+ / /" alphabet and '=' padding
# surviving %-encoding is the §10.1 contract under test. objcopy keeps the
# encoder from the compiled http.o (gc-sections then prunes the rest), so the
# stub and the real object link together without duplicate symbols.
gcc "${COMPILE_FLAGS[@]}" -c src/system/net/http.c -o "$WORK/http_full.o"
objcopy --keep-global-symbol=http_url_encode "$WORK/http_full.o" "$WORK/http_url.o"
gcc "${COMPILE_FLAGS[@]}" "$ROOT/tools/audiobookshelf-browse-test.c" \
	src/system/streaming/audiobookshelf.c src/system/streaming/abs_profiles.c \
	src/system/core/config.c src/system/core/json.c \
	"${LINK_FLAGS[@]}" "$WORK/http_url.o" -o "$WORK/browse-test"
"$WORK/browse-test" "$UNIT_WORK"

# ---------------------------------------------------------------------------
# Live: all three provenance-verified fixture aliases
# ---------------------------------------------------------------------------
for ALIAS in $ALIASES; do
	alias_dir="$WORK/$ALIAS"
	port_file="$alias_dir/port"
	progress_file="$alias_dir/progress.json"
	mkdir -p "$alias_dir"
	# Empty media dir on purpose: these tests never fetch audio bytes.
	python3 "$ROOT/tools/audiobookshelf_mock.py" --media-dir "$WORK/media" \
		--port-file "$port_file" --progress-file "$progress_file" \
		--token "sonix-browse-$ALIAS" >"$alias_dir/mock.stdout" 2>"$alias_dir/mock.stderr" &
	MOCK_PID=$!
	for ((i = 0; i < 50; i++)); do
		[[ -s "$port_file" ]] && break
		sleep 0.1
		kill -0 "$MOCK_PID" 2>/dev/null || { cat "$alias_dir/mock.stderr" >&2; fail "mock for $ALIAS died"; }
	done
	[[ -s "$port_file" ]] || fail "mock for $ALIAS never opened a port"
	PORT=$(cat "$port_file")

	# The live binary uses the real http stack (no stub; -DABS_BROWSE_TEST_LIVE).
	gcc "${COMPILE_FLAGS[@]}" -DABS_BROWSE_TEST_LIVE "$ROOT/tools/audiobookshelf-browse-test.c" \
		src/system/streaming/audiobookshelf.c src/system/streaming/abs_profiles.c \
		src/system/core/config.c src/system/core/json.c src/system/net/http.c \
		src/system/net/tls.c "${LINK_FLAGS[@]}" -o "$WORK/browse-live"
	SONIX_AUDIOBOOKSHELF_SERVER="http://127.0.0.1:$PORT" SONIX_AUDIOBOOKSHELF_TOKEN="sonix-browse-$ALIAS" \
		"$WORK/browse-live" "$alias_dir"

	log="$alias_dir/progress.requests"
	# The exact §10.1 mapping reached the server for this alias: sort fields and
	# the desc literal, pages, limit, minified/collapseseries, and filters.
	grep -q 'sort=media.metadata.title&desc=0&minified=0&collapseseries=0' "$log" || fail "$ALIAS: title sort missing"
	grep -q 'sort=media.metadata.authorNameLF&desc=0' "$log" || fail "$ALIAS: author sort field missing"
	grep -q 'sort=addedAt&desc=1' "$log" || fail "$ALIAS: addedAt desc missing"
	grep -q 'sort=addedAt&desc=0' "$log" || fail "$ALIAS: addedAt toggle missing"
	grep -q 'sort=media.duration&desc=0' "$log" || fail "$ALIAS: duration sort missing"
	grep -q 'sort=sequence&desc=0' "$log" || fail "$ALIAS: series position sort missing"
	grep -q 'limit=40&page=1&' "$log" || fail "$ALIAS: second book page not requested"
	grep -q 'filter=authors.YXV0aG9yLWZhLTE%3D' "$log" || fail "$ALIAS: author filter missing"
	grep -q 'filter=series.c2VyaWVzLWZhLTE%3D' "$log" || fail "$ALIAS: series filter missing"
	# §10.1: genre/tag filter values are the NAME strings. With the fixture's
	# name-shaped data these round-trip to rows (see the positive check below);
	# the id-spelled forms are now negative probes on the wire -- still
	# constructed and logged, matching no rows at all.
	grep -q 'filter=tags.Rml4dHVyZSBUYWcgQWxwaGE%3D' "$log" || fail "$ALIAS: tag name filter missing"
	grep -q 'filter=genres.RmljdGlvbg%3D%3D' "$log" || fail "$ALIAS: genre name filter missing"
	grep -q 'filter=tags.dGEtZmEtMQ%3D%3D' "$log" || fail "$ALIAS: id-spelled tag probe missing"
	grep -q 'filter=genres.Z2UtZmEtMQ%3D%3D' "$log" || fail "$ALIAS: id-spelled genre probe missing"
	# §10.5 search contract on the wire: 101 requested, never 100.
	grep -q '/search?q=overflow&limit=101' "$log" || fail "$ALIAS: search 101 request missing"
	grep -q '/search?q=hundred&limit=101' "$log" || fail "$ALIAS: search ceiling probe missing"

	# §10.1/§10.3 positive round-trip, proven against the checked-in fixture
	# bytes with the mock's own decode/filter helpers: the genre/tag NAME
	# filters select the same rows the recorded expectations pin, and the
	# id-spelled values select nothing. This closes the fixture-gap wave from
	# the M2 backend report (name filters used to answer zero rows).
	python3 - "$ROOT/tools/fixtures/audiobookshelf/$ALIAS" <<'PY' || fail "$ALIAS: name/filter round-trip mismatch"
import base64, json, sys
from pathlib import Path

# The fixture dir's third parent is tools/, where the mock module lives.
sys.path.insert(0, str(Path(sys.argv[1]).parents[2]))
from audiobookshelf_mock import decode_filter, filter_rows  # noqa: E402

dataset = json.load(open(Path(sys.argv[1]) / "scenario" / "dataset.json", encoding="utf-8"))
rows = dataset["items"]["lib-list-1"]

def wire(group, name):
    value = base64.b64encode(name.encode()).decode()
    decoded = decode_filter(f"{group}.{value}")
    assert decoded == (group, name), decoded
    return decoded

def selected(group, name):
    got, value = wire(group, name)
    return filter_rows(rows, got, value)

def expect(group, name, count, first, last):
    got = selected(group, name)
    assert len(got) == count, (group, name, len(got))
    if count:
        assert got[0]["id"] == first and got[-1]["id"] == last, (group, name, got[0]["id"], got[-1]["id"])

# Positive: the NAME filters round-trip to the recorded expected rows.
expect("genres", "Fiction", 15, "pg-bk-001", "pg-bk-071")
expect("genres", "Nonfiction", 60, "pg-bk-002", "pg-bk-075")
expect("tags", "Fixture Tag Alpha", 40, "pg-bk-001", "pg-bk-040")
expect("tags", "Fixture Tag Beta", 35, "pg-bk-041", "pg-bk-075")
# Negative: the fixture-internal ids are not filter values (§10.1).
expect("genres", "ge-fa-1", 0, "", "")
expect("tags", "ta-fa-1", 0, "", "")

# The filterdata shape agrees with the row values (bare strings, §10.1).
fd = dataset["filterdata"]["lib-list-1"]
assert fd["genres"] == ["Fiction", "Nonfiction"], fd["genres"]
assert fd["tags"] == ["Fixture Tag Alpha", "Fixture Tag Beta"], fd["tags"]
row = rows[0]
assert row["genres"] == ["Fiction"] and "genre_ids" not in row
print("name/filter round-trip: positive + negative expectations verified")
PY

	kill -TERM "$MOCK_PID"
	wait "$MOCK_PID" 2>/dev/null || true
	MOCK_PID=""
	printf 'browse test: %s live matrix passed\n' "$ALIAS"
done

printf 'browse test passed: query construction + %s fixture alias/es (offline + live)\n' "$(wc -w <<<"$ALIASES")"
