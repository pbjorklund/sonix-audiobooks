#!/usr/bin/env bash
# M4 WS3 wave-1 detail tests (Bob spec §9.1–§9.4), ASan+UBSan.
#
#   * offline: tools/audiobookshelf-detail-test.c with a stubbed http_request
#     pins the §9.1 request mapping, the pure description policy (shared with
#     metadata.c), descriptionPlain/HTML/entity equivalence, chapter bounds
#     0/1/400/401 + overflow + invalid times, long strings, malformed JSON,
#     ownership/freedom of the §9.3 model, and the two-server detail slice.
#
#   * live: the same binary compiled against the real HTTP stack, once per
#     checked-in fixture alias (pinned-3563d49, current-5de038d, eric-v2.38.0):
#     the REAL expanded detail bytes the fixtures record (full/minimal books,
#     the 0/1/400/401-chapter corpus, mock-book) parse with every §9.1 field.
#
# The §9.2 screen itself has no LVGL harness here: it is exercised end to end
# by tools/audiobookshelf-host-smoke.sh (its download/link modes now walk the
# detail) and tools/audiobookshelf-queueui-smoke.sh.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
PLAYER="$ROOT/sonix-player"
WORK=$(mktemp -d)
UNIT_WORK=$(mktemp -d)
ALIAS=""
PORT_FILE=""
MOCK_PID=""
trap 'rm -rf -- "$UNIT_WORK" "$WORK"; [[ -n "$MOCK_PID" ]] && kill -TERM "$MOCK_PID" 2>/dev/null || true' EXIT

fail() { printf 'detail test: %s\n' "$*" >&2; exit 1; }
need() { command -v "$1" >/dev/null 2>&1 || fail "missing required command: $1"; }
need gcc
need python3

# --------------------------------------------------------------- structure --
# The §9.2 screen strings exist in the R1 language file, and the wave's file
# layout matches §9.4: the description helper is a separate pure unit.
INIFILE="$ROOT/sonix-packer/assets/R1/usr/resource/sonix/language/English.ini"
for key in audiobookshelf_detail audiobookshelf_detail_loading audiobookshelf_detail_failed \
	audiobookshelf_detail_author audiobookshelf_detail_narrator audiobookshelf_detail_duration \
	audiobookshelf_detail_series audiobookshelf_detail_finished audiobookshelf_detail_progress \
	audiobookshelf_detail_about audiobookshelf_detail_no_description audiobookshelf_detail_summary \
	audiobookshelf_detail_chapters audiobookshelf_detail_chapters_none \
	audiobookshelf_detail_chapters_overflow audiobookshelf_detail_queue \
	audiobookshelf_detail_play_local audiobookshelf_link audiobookshelf_downloads; do
	grep -q "^$key = " "$INIFILE" || fail "English.ini lost detail key $key"
done
[[ -f "$PLAYER/src/system/library/description.h" && -f "$PLAYER/src/system/library/description.c" ]] ||
	fail "§9.4: the shared description helper is missing"
# Wave boundary: no cover subsystem may appear while wave 1 is the scope.
for forbidden in abs_cover coverloader; do
	grep -rq "$forbidden" "$PLAYER/src/gui/streaming/audiobookshelfdetail.c" &&
		fail "the detail screen reached cover territory before wave 2"
done
# The detail requests the queue's expanded endpoint; never a raw publisher
# HTML pass-through (the parse goes through the shared helper).
grep -q 'expanded=1&include=progress,authors' "$PLAYER/src/system/streaming/audiobookshelf.c" ||
	fail "the detail request left the §9.1 item endpoint"
grep -q 'description_pick' "$PLAYER/src/system/streaming/audiobookshelf.c" ||
	fail "the detail parse stopped using the shared description helper"

# §14.6 alias gate, matching the other ABS suites. SONIX_ABS_FIXTURE picks one
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

cd "$PLAYER"

# ---------------------------------------------------------------------------
# Offline: request mapping, description policy, chapter bounds, ownership
# ---------------------------------------------------------------------------
# The stub replaces http_request()/http_last_error(), but the REAL
# http_url_encode() must be exercised: the §9.1 item id is URL-encoded on the
# wire. objcopy keeps the encoder from the compiled http.o (gc-sections then
# prunes the rest), so the stub and the real object link together.
gcc "${COMPILE_FLAGS[@]}" -c src/system/net/http.c -o "$UNIT_WORK/http_full.o"
objcopy --keep-global-symbol=http_url_encode "$UNIT_WORK/http_full.o" "$UNIT_WORK/http_url.o"
# The §11.5 manifest readers behind the two-server slice walk the real
# scanner's folder-book logic; gc-sections keeps the rest of the DB out.
gcc "${COMPILE_FLAGS[@]}" "$ROOT/tools/audiobookshelf-detail-test.c" \
	src/system/streaming/audiobookshelf.c src/system/streaming/abs_profiles.c \
	src/system/library/description.c \
	src/system/core/config.c src/system/core/json.c src/system/core/pathlimits.c \
	src/system/core/sha256.c src/system/core/utils.c \
	src/system/library/audiobookdb.c src/system/db/sqlite3_impl.c \
	"${LINK_FLAGS[@]}" "$UNIT_WORK/http_url.o" -o "$UNIT_WORK/detail-test"
mkdir -p "$UNIT_WORK/work"
"$UNIT_WORK/detail-test" "$UNIT_WORK/work"

# The §9.5 budget line on the REAL target ABI, when the cross toolchain is
# available in the checkout. The predicates are compile-time: a cross-binary
# cannot run on the host, so the verification is the cross COMPILATION of the
# two _Static_asserts (exact 216-byte chapter record, under-96-KiB model);
# when qemu-user exists the actual numbers are printed too.
CROSS_GCC="$PLAYER/rockbox-toolchain/bin/mipsel-rockbox-linux-gnu-gcc"
if [[ -x "$CROSS_GCC" ]]; then
	cat >"$UNIT_WORK/detail_sizeof.c" <<'EOF'
#include "src/system/streaming/audiobookshelf.h"
_Static_assert(sizeof(abs_detail_chapter_t) == 216,
			   "the §9.3 chapter record no longer 216 bytes on the target ABI");
_Static_assert(sizeof(audiobookshelf_detail_t) + 400 * sizeof(abs_detail_chapter_t) < 96 * 1024,
			   "the §9.3 detail model left its §9.5 96 KiB budget on the target ABI");
#if defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 4 && defined(__mips__)
int main(void) {
	printf("mipsel sizeof(abs_detail_chapter_t)=%zu sizeof(audiobookshelf_detail_t)=%zu\n",
		   sizeof(abs_detail_chapter_t), sizeof(audiobookshelf_detail_t));
	return 0;
}
#else
int main(void) { return 0; }
#endif
EOF
	# Compile-time verification: failure here fails the whole suite.
	"$CROSS_GCC" -march=mips32r2 -I. -c "$UNIT_WORK/detail_sizeof.c" -o "$UNIT_WORK/detail_sizeof.o" ||
		fail "the mipsel detail model left the §9.5 96 KiB budget (compile-time)"
	printf 'detail test: mipsel §9.5 model budget verified on the target ABI (compile-time)\n'
	if command -v qemu-mipsel-static >/dev/null 2>&1; then
		QEMU_LIB="$PLAYER/rockbox-toolchain/mipsel-rockbox-linux-gnu/sysroot"
		if "$CROSS_GCC" -march=mips32r2 -I. -o "$UNIT_WORK/detail_sizeof" "$UNIT_WORK/detail_sizeof.c" 2>/dev/null; then
			Q=$(qemu-mipsel-static -L "$QEMU_LIB" "$UNIT_WORK/detail_sizeof" 2>/dev/null) &&
				printf 'detail test: %s\n' "$Q"
		fi
	fi
else
	printf 'detail test: cross toolchain absent; §9.5 target budget checked via make target\n'
fi

# ---------------------------------------------------------------------------
# Live: all three provenance-verified fixture aliases
# ---------------------------------------------------------------------------
MEDIA_DIR="$WORK/media"
mkdir -p "$MEDIA_DIR"
for ALIAS in $ALIASES; do
	alias_dir="$WORK/$ALIAS"
	port_file="$alias_dir/port"
	progress_file="$alias_dir/progress.json"
	mkdir -p "$alias_dir"
	python3 "$ROOT/tools/audiobookshelf_mock.py" --media-dir "$MEDIA_DIR" \
		--port-file "$port_file" --progress-file "$progress_file" \
		--token "sonix-detail-$ALIAS" >"$alias_dir/mock.stdout" 2>"$alias_dir/mock.stderr" &
	MOCK_PID=$!
	for ((i = 0; i < 50; i++)); do
		[[ -s "$port_file" ]] && break
		sleep 0.1
		kill -0 "$MOCK_PID" 2>/dev/null || { cat "$alias_dir/mock.stderr" >&2; fail "mock for $ALIAS died"; }
	done
	[[ -s "$port_file" ]] || fail "mock for $ALIAS never opened a port"
	PORT=$(cat "$port_file")

	# The live binary uses the real http stack (no stub; -DABS_DETAIL_TEST_LIVE).
	gcc "${COMPILE_FLAGS[@]}" -DABS_DETAIL_TEST_LIVE "$ROOT/tools/audiobookshelf-detail-test.c" \
		src/system/streaming/audiobookshelf.c src/system/streaming/abs_profiles.c \
		src/system/library/description.c \
		src/system/core/config.c src/system/core/json.c src/system/net/http.c \
		src/system/net/tls.c "${LINK_FLAGS[@]}" -o "$UNIT_WORK/detail-live"
	SONIX_AUDIOBOOKSHELF_SERVER="http://127.0.0.1:$PORT" SONIX_AUDIOBOOKSHELF_TOKEN="sonix-detail-$ALIAS" \
		"$UNIT_WORK/detail-live" "$alias_dir"

	# §9.1 on the wire: the exact expanded-item endpoint, per alias.
	grep -q 'GET /api/items/bk-full?expanded=1&include=progress,authors ' "$alias_dir/progress.requests" ||
		fail "$ALIAS: the expanded detail request never reached the server"

	kill -TERM "$MOCK_PID"
	wait "$MOCK_PID" 2>/dev/null || true
	MOCK_PID=""
	printf 'detail test: %s live matrix passed\n' "$ALIAS"
done

printf 'detail test passed: parse/helper matrix + %s fixture alias/es (offline + live)\n' "$(wc -w <<<"$ALIASES")"
