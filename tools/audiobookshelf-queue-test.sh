#!/usr/bin/env bash
# WS2 download-queue foundation tests (bob §8.1–8.2 / §8.5), ASan+UBSan.
# Offline: the media surface is fake transfer callbacks (tools/
# audiobookshelf-queue-test.c); no HTTP, no GUI, no mock server. The shared
# download turn (streamturn.c, §8.3 lines 558–559) is linked so the turn
# coexistence, abandon-table and §8.4 timer-backoff wave runs against the REAL
# primitives, with a 300 ms per-job backoff for a bounded harness (production
# never defines ABS_DL_WAIT_BACKOFF_MS).
#
# §14.7: SONIX_SANITIZER selects the sanitizer build. The functional default is
# address+undefined; thread is a DISTINCT TSan build (never combined with
# ASan). An unknown mode fails the run.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
SAN_MODE="${SONIX_SANITIZER:-address,undefined}"
case "$SAN_MODE" in
	address,undefined) SANITIZERS=(-fsanitize=address,undefined -fno-omit-frame-pointer) ;;
	thread) SANITIZERS=(-fsanitize=thread -fno-omit-frame-pointer) ;;
	*) printf 'audiobookshelf-queue-test.sh: unknown SONIX_SANITIZER: %s (address,undefined|thread)\n' "$SAN_MODE" >&2
		exit 2 ;;
esac
WORK=$(mktemp -d)
trap 'rm -rf -- "$WORK"' EXIT
cd "$ROOT/sonix-player"
gcc -O1 -g -DHOST_BUILD=1 -D_FILE_OFFSET_BITS=64 -I. -Ilvgl -DLV_CONF_INCLUDE_SIMPLE=1 \
    -ffunction-sections -fdata-sections "${SANITIZERS[@]}" \
    -DABS_DL_WAIT_BACKOFF_MS=300 \
    "$ROOT/tools/audiobookshelf-queue-test.c" src/system/streaming/abs_download.c \
    src/system/streaming/streamturn.c \
    src/system/core/pathlimits.c src/system/core/utils.c src/system/db/sqlite3_impl.c \
    -Wl,--gc-sections -lpthread -lm -ldl -o "$WORK/test"
"$WORK/test" "$WORK/card"

# §8.8/§14.8 WS2: the >2 GiB counter oracle is the runtime test above; this
# compile-time gate pins the target ABI it must survive. Every counter and
# byte-size field that can exceed 2^31 is 8 bytes wide on the 32-bit-`long`
# mipsel ABI, and the file offsets (the resume .part) keep 64-bit off_t. A
# regression to any 32-bit type fails the build, exactly like the detail
# suite's §9.5 sizeof gate.
CROSS_GCC="$ROOT/sonix-player/rockbox-toolchain/bin/mipsel-rockbox-linux-gnu-gcc"
fail_assert() { printf 'audiobookshelf-queue-test.sh: %s\n' "$*" >&2; exit 1; }
if [[ -x "$CROSS_GCC" ]]; then
	cat >"$WORK/counters_abi.c" <<'EOF'
#include "src/system/net/http.h"
#include "src/system/streaming/abs_download.h"
#include "src/system/db/sqlite3.h"
#include <sys/types.h>
_Static_assert(sizeof(long long) == 8, "the target lost 64-bit long long");
_Static_assert(sizeof(((abs_download_job_t *)0)->bytes_done) == 8,
			   "the §8.8 byte counter no longer 64-bit on the target ABI");
_Static_assert(sizeof(((abs_download_job_t *)0)->bytes_total) == 8,
			   "the §8.8 byte total no longer 64-bit on the target ABI");
_Static_assert(sizeof(((abs_download_file_t *)0)->expected_size) == 8,
			   "the per-file expected size no longer 64-bit on the target ABI");
_Static_assert(sizeof(((http_stream_t *)0)->content_length) == 8,
			   "the framing Content-Length is no longer long long on the target ABI");
#if defined(_FILE_OFFSET_BITS) && _FILE_OFFSET_BITS >= 64
_Static_assert(sizeof(off_t) == 8, "the resume offsets left 64-bit off_t");
#endif
_Static_assert(sizeof(sqlite3_int64) == 8, "the queue rows lost their int64 columns");
EOF
	"$CROSS_GCC" -march=mips32r2 -I. -D_FILE_OFFSET_BITS=64 -c "$WORK/counters_abi.c" \
		-o "$WORK/counters_abi.o" ||
		fail_assert "the mipsel target lost a 64-bit >2 GiB counter type (compile-time)"
	printf 'audiobookshelf-queue-test.sh: mipsel >2 GiB counter ABI verified (compile-time)\n'
else
	printf 'audiobookshelf-queue-test.sh: cross toolchain absent; the mipsel counter ABI gate is skipped\n'
fi

# §8.3/§8.5 busy gate: the Wi-Fi-in-use and player-busy decisions in power.c
# must consult the queue's busy bit directly. power.c compiles no standalone
# unit (it is bound to LVGL and the whole device), so the consult is asserted
# structurally: both decision functions name abs_download_busy() inside their
# own bodies, and the header that declares it is included. The truth table of
# the bit itself is the runtime test above (§10, lifecycle: busy during
# FETCHING/DOWNLOADING/PAUSING/FINALIZING, idle for QUEUED/PAUSED/terminal).
POWER="$ROOT/sonix-player/src/system/device/power.c"
fail_assert() { printf 'audiobookshelf-queue-test.sh: %s\n' "$*" >&2; exit 1; }
grep -q 'src/system/streaming/abs_download.h' "$POWER" || fail_assert "power.c no longer includes abs_download.h"

busy_window() { # file, start-line, end-line
	sed -n "$2,$3p" "$1"
}
WIFI_LINE=$(grep -n 'static bool wifi_in_use(void)' "$POWER" | cut -d: -f1)
BUSY_LINE=$(grep -n 'static bool player_is_busy(bool playing' "$POWER" | cut -d: -f1)
[ -n "$WIFI_LINE" ] || fail_assert "power.c wifi_in_use() moved: busy-gate assertion needs re-anchoring"
[ -n "$BUSY_LINE" ] || fail_assert "power.c player_is_busy() moved: busy-gate assertion needs re-anchoring"
busy_window "$POWER" "$WIFI_LINE" "$((WIFI_LINE + 20))" | grep -q 'abs_download_busy()' ||
	fail_assert "power.c wifi_in_use() must consult abs_download_busy() (§8.3/§8.5)"
busy_window "$POWER" "$BUSY_LINE" "$((BUSY_LINE + 20))" | grep -q 'abs_download_busy()' ||
	fail_assert "power.c player_is_busy() must consult abs_download_busy() (§8.3/§8.5)"

# §8.2 step 6 / §8.5 line 586 reconcile gate: the progress worker's
# remote-import path (the sync path that writes LOCAL progress) must check the
# FINALIZING sync barrier before writing. audiobookshelf.c compiles no
# standalone unit beside the browse tests (reconcile_progress is its static
# engine, driven live by the host smoke), so the call site is asserted
# structurally like the busy gate above — the runtime truth of the barrier bit
# itself is proven in the queue/transfer suites; the skip is proven live there.
SYNC="$ROOT/sonix-player/src/system/streaming/audiobookshelf.c"
SYNC_RECONCILE=$(grep -n 'static int reconcile_progress' "$SYNC" | cut -d: -f1)
SYNC_IMPORT=$(grep -n 'if (!audiobook_sync_import(checkpoint' "$SYNC" | cut -d: -f1)
SYNC_GATE=$(grep -n 'if (abs_download_sync_blocked(file))' "$SYNC" | cut -d: -f1)
[ -n "$SYNC_RECONCILE" ] || fail_assert "audiobookshelf.c reconcile_progress() moved: gate assertion needs re-anchoring"
[ -n "$SYNC_IMPORT" ] || fail_assert "audiobookshelf.c remote-progress import moved: gate assertion needs re-anchoring"
[ -n "$SYNC_GATE" ] || fail_assert "audiobookshelf.c reconcile_progress() lost its abs_download_sync_blocked() gate (§8.2 step 6)"
[ "$SYNC_GATE" -lt "$SYNC_IMPORT" ] ||
	fail_assert "the reconcile gate must run before the local-progress write (§8.2 step 6)"

printf 'audiobookshelf-queue-test.sh: power busy-gate consult asserted (wifi_in_use, player_is_busy)\n'

# §8.3 lines 558–559 / §8.5 line 589 structural coexistence gate: every
# downloader in the tree registers exactly one abandon, the table capacity in
# streamturn.c pins the known registrant count, and the runtime suite (the
# binary above) already proved filling to the capacity, the loud overflow
# refusal and that abandon-everything reaches everyone. Registrants that moved
# or renamed would need this anchor list updated — deliberately, as a review
# step, in the same commit.
STREAMTURN="$ROOT/sonix-player/src/system/streaming/streamturn.c"
for registrant in \
	"$ROOT/sonix-player/src/system/streaming/qobuzcache.c" \
	"$ROOT/sonix-player/src/system/streaming/tidalcache.c" \
	"$ROOT/sonix-player/src/system/streaming/podcastcache.c" \
	"$ROOT/sonix-player/src/system/streaming/abs_download.c"; do
	grep -q 'streamturn_register_abandon(' "$registrant" ||
		fail_assert "$(basename "$registrant") lost its streamturn abandon registration (§8.3 line 559)"
done
grep -q '_Static_assert(STREAMTURN_ABANDON_MAX >= STREAMTURN_ABANDON_KNOWN' "$STREAMTURN" ||
	fail_assert "streamturn.c lost the build-time abandon-capacity assertion (§8.5 line 589)"
# The queue's per-file turn seam is engine-owned: a non-blocking try with the
# queue mutex released, released after the op (§12.1 line 1154).
grep -q 'streamturn_try_acquire()' "$ROOT/sonix-player/src/system/streaming/abs_download.c" ||
	fail_assert "abs_download.c lost the per-file streamturn try-acquire seam (§8.3 line 558)"
# §12.4 line 1201 funnel: the connectivity notification that wakes the sync
# passes is also the queue's WAITING_NETWORK wake (§12.3 line 1178), including
# the mid-session link regain the wifi poll publishes.
grep -q 'abs_download_network_changed()' "$ROOT/sonix-player/src/system/streaming/audiobookshelf.c" ||
	fail_assert "audiobookshelf_network_changed() lost the download-queue connectivity wake (§8.4 line 569)"
grep -q 'audiobookshelf_network_changed();' "$ROOT/sonix-player/src/system/net/wifi.c" ||
	fail_assert "wifi.c poll_status lost the mid-session online-gain notification (§12.3 line 1178)"

printf 'audiobookshelf-queue-test.sh: streamturn coexistence + connectivity funnel asserted\n'
