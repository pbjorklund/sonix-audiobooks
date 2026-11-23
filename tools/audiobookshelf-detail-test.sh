#!/usr/bin/env bash
# M4 WS3 detail tests (Bob spec §9.1–§9.4), ASan+UBSan.
#
#   * offline: tools/audiobookshelf-detail-test.c with a stubbed http_request
#     AND a stubbed streaming entry pins the §9.1 request mapping, the pure
#     description policy (shared with metadata.c), descriptionPlain/HTML/entity
#     equivalence, chapter bounds 0/1/400/401 + overflow + invalid times, long
#     strings, malformed JSON, ownership/freedom of the §9.3 model, the
#     two-server detail slice — and, wave 2, the authenticated cover: the
#     §9.1 cover endpoint under the pinned Bearer, the hashed card-only cache
#     name, the §9.1 2 MiB encoded cap, cancellation/interruption, the §9.3
#     LRU ceilings (8 files / 16 MiB, by use), card-close quiescence, the
#     REAL decode the detail and full-screen viewers run (cover_load_image_file
#     via the objcopy-pruned cover object), and the forty-cycle leak gate.
#
#   * live: the same binary compiled against the real HTTP stack, once per
#     checked-in fixture alias (pinned-3563d49, current-5de038d, eric-v2.38.0):
#     the REAL expanded detail bytes the fixtures record (full/minimal books,
#     the 0/1/400/401-chapter corpus, mock-book) parse with every §9.1 field,
#     and the cover fetches land in the real cache with the fixture's exact
#     bytes — the authenticated request exactly once, then served from cache.
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
# The §9.2 screen strings exist in the R1 language file, and the waves' file
# layout matches §9.4: the description helper is a separate pure unit, the
# cover subsystem its own.
INIFILE="$ROOT/sonix-packer/assets/R1/usr/resource/sonix/language/English.ini"
for key in audiobookshelf_detail audiobookshelf_detail_loading audiobookshelf_detail_failed \
	audiobookshelf_detail_author audiobookshelf_detail_narrator audiobookshelf_detail_duration \
	audiobookshelf_detail_series audiobookshelf_detail_finished audiobookshelf_detail_progress \
	audiobookshelf_detail_about audiobookshelf_detail_no_description audiobookshelf_detail_summary \
	audiobookshelf_detail_chapters audiobookshelf_detail_chapters_none \
	audiobookshelf_detail_chapters_overflow audiobookshelf_detail_queue \
	audiobookshelf_detail_play_local audiobookshelf_link audiobookshelf_downloads \
	audiobookshelf_detail_cover_unavailable; do
	grep -q "^$key = " "$INIFILE" || fail "English.ini lost detail key $key"
done
[[ -f "$PLAYER/src/system/library/description.h" && -f "$PLAYER/src/system/library/description.c" ]] ||
	fail "§9.4: the shared description helper is missing"
[[ -f "$PLAYER/src/system/streaming/abs_cover.h" && -f "$PLAYER/src/system/streaming/abs_cover.c" ]] ||
	fail "§9.4: the authenticated cover unit is missing"
# §9.4 ownership rows for wave 2: slot 108 exists for the detail and the slot
# table grew to 109.
grep -q "COVERLOADER_SLOTS 109" "$PLAYER/src/gui/nowplaying/coverloader.h" ||
	fail "§9.4: the detail slot never raised COVERLOADER_SLOTS to 109"
grep -q "define COVERLOADER_ABS_DETAIL 108" "$PLAYER/src/gui/nowplaying/coverloader.h" ||
	fail "§9.4: the detail slot number is missing from coverloader.h"
# qobuzart stays unauthenticated, per §9.4: no ABS integration, no token/ABS
# URL surface may reach it.
if grep -qiE "abs_cover|audiobookshelf|Bearer|Authorization" "$PLAYER/src/gui/streaming/qobuzart.c"; then
	fail "§9.4: qobuzart received ABS integration it must never get"
fi
# The full-screen viewer stays a local-path page: it never networked, and the
# §9.3 ownership rule (hash-named cache files, no tokens) holds there too.
if grep -qE "http_|Bearer|Authorization|token" "$PLAYER/src/gui/library/imageview.c"; then
	fail "§9.4: imageview grew networking; it reads local paths only"
fi
# The cover request stays the §9.1 endpoint with the reviewed parameters.
grep -q "format=jpeg&width=900&height=1280" "$PLAYER/src/system/streaming/abs_cover.c" ||
	fail "§9.1: the cover endpoint left the reviewed parameters"
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
# Offline: request mapping, description policy, chapter bounds, ownership,
# cover subsystem
# ---------------------------------------------------------------------------
# The stubs replace http_request()/http_stream_open_ex()/http_stream_read/
# http_stream_close/http_last_error(), but the REAL http_url_encode() must be
# exercised: the §9.1 item id is URL-encoded on the wire. objcopy keeps the
# encoder from the compiled http.o (gc-sections then prunes the rest), so the
# stub and the real object link together.
gcc "${COMPILE_FLAGS[@]}" -c src/system/net/http.c -o "$UNIT_WORK/http_full.o"
objcopy --keep-global-symbol=http_url_encode "$UNIT_WORK/http_full.o" "$UNIT_WORK/http_url.o"
# The decode side of the cover tests runs the REAL loader, cover
#_load_image_file(), with its transient encoded allocation: objcopy keeps
# just that entry point and cover_free() from the compiled cover object, and
# gc-sections then prunes the thumbnail cache and every other dependency so
# the image decoders and utils are all that remain.
gcc "${COMPILE_FLAGS[@]}" -c src/gui/nowplaying/cover.c -o "$UNIT_WORK/cover_full.o"
objcopy --keep-global-symbol=cover_load_image_file --keep-global-symbol=cover_free \
	"$UNIT_WORK/cover_full.o" "$UNIT_WORK/cover_img.o"
gcc "${COMPILE_FLAGS[@]}" -c src/system/image/stb_image_impl.c -o "$UNIT_WORK/stb_image_impl.o"
gcc "${COMPILE_FLAGS[@]}" -c src/system/image/art_shrink.c -o "$UNIT_WORK/art_shrink.o"
gcc "${COMPILE_FLAGS[@]}" -c src/system/image/jpeg_scaled.c -o "$UNIT_WORK/jpeg_scaled.o"
gcc "${COMPILE_FLAGS[@]}" -c src/system/image/png_scaled.c -o "$UNIT_WORK/png_scaled.o"
IMAGE_OBJS=("$UNIT_WORK/cover_img.o" "$UNIT_WORK/stb_image_impl.o" "$UNIT_WORK/art_shrink.o" \
	"$UNIT_WORK/jpeg_scaled.o" "$UNIT_WORK/png_scaled.o")
# The §11.5 manifest readers behind the two-server slice walk the real
# scanner's folder-book logic; gc-sections keeps the rest of the DB out.
gcc "${COMPILE_FLAGS[@]}" "$ROOT/tools/audiobookshelf-detail-test.c" \
	src/system/streaming/audiobookshelf.c src/system/streaming/abs_profiles.c \
	src/system/streaming/abs_cover.c \
	src/system/library/description.c \
	src/system/core/config.c src/system/core/json.c src/system/core/pathlimits.c \
	src/system/core/sha256.c src/system/core/utils.c \
	src/system/library/audiobookdb.c src/system/db/sqlite3_impl.c \
	"${IMAGE_OBJS[@]}" \
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

	# The live binary uses the real http stack (no stub; -DABS_DETAIL_TEST_LIVE),
	# plus the cover unit and the pruned decode object.
	gcc "${COMPILE_FLAGS[@]}" -DABS_DETAIL_TEST_LIVE "$ROOT/tools/audiobookshelf-detail-test.c" \
		src/system/streaming/audiobookshelf.c src/system/streaming/abs_profiles.c \
		src/system/streaming/abs_cover.c \
		src/system/library/description.c \
		src/system/core/config.c src/system/core/json.c src/system/core/utils.c \
		src/system/core/sha256.c src/system/net/http.c \
		src/system/net/tls.c "${IMAGE_OBJS[@]}" "${LINK_FLAGS[@]}" -o "$UNIT_WORK/detail-live"
	export SONIX_DETAIL_FIXTURE_COVER="$ROOT/tools/fixtures/audiobookshelf/$ALIAS/scenario/media/covers/fixture-cover.jpg"
	[[ -s "$SONIX_DETAIL_FIXTURE_COVER" ]] || fail "$ALIAS: the fixture cover is missing"
	SONIX_AUDIOBOOKSHELF_SERVER="http://127.0.0.1:$PORT" SONIX_AUDIOBOOKSHELF_TOKEN="sonix-detail-$ALIAS" \
		"$UNIT_WORK/detail-live" "$alias_dir"
	unset SONIX_DETAIL_FIXTURE_COVER

	# §9.1 on the wire: the exact expanded-item endpoint, per alias.
	grep -q 'GET /api/items/bk-full?expanded=1&include=progress,authors ' "$alias_dir/progress.requests" ||
		fail "$ALIAS: the expanded detail request never reached the server"

	# §9.1 wave 2 on the wire: the authenticated cover exactly once — the
	# second fetch is a cache hit and asks the server for nothing.
	for item in bk-full bk-minimal; do
		count=$(grep -c "GET /api/items/$item/cover?format=jpeg&width=900&height=1280 " \
			"$alias_dir/progress.requests" || true)
		[[ "$count" == 1 ]] ||
			fail "$ALIAS: the cover request for $item did not arrive exactly once (count $count)"
	done

	kill -TERM "$MOCK_PID"
	wait "$MOCK_PID" 2>/dev/null || true
	MOCK_PID=""
	printf 'detail test: %s live matrix passed\n' "$ALIAS"
done

printf 'detail test passed: parse/helper matrix + %s fixture alias/es (offline + live)\n' "$(wc -w <<<"$ALIASES")"
