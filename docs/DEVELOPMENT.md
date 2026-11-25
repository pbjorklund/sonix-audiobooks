# Development guide

This guide is for contributors building Sonix Audiobooks, not for people installing a release.

## Checkout and scope

Clone [the working fork](https://github.com/pbjorklund/sonix-audiobooks) as `origin`. Use `yetisoldier/sonix-audiobooks` as `upstream`. Start with `git status --short --branch`; preserve local changes when updating. Run commands below from the repository root unless shown otherwise. Use the bound `hiby-r1-abs` AmpliFlow project for tracked work.

A source push, a firmware build, copying an image to SD, installing it, and publishing a release are separate actions. Do only the actions requested. Do not commit firmware, device logs, userdata backups, or credentials.

## Repository layout

- `sonix-player/`: player source and host/target builds.
- `sonix-packer/`: firmware packer, stock update inputs, and per-device overlays.
- `sonix-packer/assets/R1/`: original-R1 resources installed into the firmware image.
- `tools/`: host smoke tests, package checks, and controlled device-test helpers.

## Host build and audiobook smoke test

On Debian or Ubuntu, install the normal Sonix build dependencies plus `ffmpeg`, `sqlite3`, `xvfb`, `xdotool`, and `scrot`.

```bash
cd sonix-player
make host -j$(nproc)
cd ..
tools/audiobook-host-smoke.sh
```

The smoke test uses the real R1 UI in a headless X display. It creates disposable media and a disposable card under `build/audiobook-host-smoke/`, then validates multipart indexing, bookmarks, persisted direct resume, and automatic part advance.

For Audiobookshelf work, run the dedicated smoke suite:

```bash
tools/audiobookshelf-host-smoke.sh
SONIX_ABS_EXISTING=1 tools/audiobookshelf-host-smoke.sh
SONIX_ABS_EXISTING=1 SONIX_ABS_LOCAL_PROGRESS=1 tools/audiobookshelf-host-smoke.sh
bash tools/audiobookshelf-outbox-test.sh
SONIX_ABS_OFFLINE_RESTART=1 bash tools/audiobookshelf-host-smoke.sh
SONIX_ABS_CONFLICTS=1 bash tools/audiobookshelf-host-smoke.sh
```

## Target build

Since the rebase onto upstream 1.3.0, run the regression suite to verify the
metadata parsers (including the fork's `.tags` series keys and `.description`
sidecars) and the kernel-log follower with mocked system calls:

```bash
bash tools/upstream-regression.sh
```

This compiles the real metadata parsers with AddressSanitizer/UndefinedBehaviorSanitizer, generates disposable tag fixtures, and verifies the kernel-log follower's print-capped flood handling with mocked reads. It needs GCC, Python 3 and FFmpeg in addition to the host build dependencies. It does not read a real kernel log, trigger a kernel fault, or reboot anything.

The 0.x line's historical upstream review lives in [legacy-0.x/UPSTREAM_REVIEW_2026-10-01.md](legacy-0.x/UPSTREAM_REVIEW_2026-10-01.md); since the rebase the fork takes upstream work wholesale instead of backporting selectively.

The first target build downloads and builds the MIPS toolchain and static dependencies. Keep the checkout in the Linux filesystem under WSL rather than a Windows-mounted path; dependency tracking and cross compilation are substantially faster there.

```bash
cd sonix-player
make target -j$(nproc)
```

The target build includes an ABI check that rejects glibc symbols newer than the R1 firmware provides. Do not bypass it: a binary that cannot start can look like a boot loop on the device.

## Feature checks

| Change | Checks |
| --- | --- |
| TIDAL | `tools/tidalkeep-test.sh` (includes saved albums and all R1 language files) |
| Audiobookshelf podcasts | `tools/audiobookshelf-episode-test.sh`, `tools/audiobookshelf-browse-test.sh`, `SONIX_PODCAST_SMOKE_SYNC=1 tools/audiobookshelf-podcast-smoke.sh`; repeat the smoke with `SONIX_PODCAST_SMOKE_FORMAT=m4a` for AAC |
| Covers and details | `tools/audiobookshelf-detail-test.sh` and the Audiobookshelf host smoke above |
| OTA tooling | `python3 tools/pack-r1-test.py` and a package/readback run on the known base |

See the [host CI workflow](../.github/workflows/host-build.yml) for dependencies and the broader regression matrix. The simulator uses disposable media and servers; passing it does not prove R1 playback or reboot resume.

## Package an original R1 test image

Use the **official Sonix Audiobooks runtime for the original R1**, not stock HiBy firmware, an R1 MIDI image, or the R3 Pro II. Keeping the Sonix runtime preserves the Bluetooth stack. The overlay helper replaces the player and the 12 R1 language files and stamps the build ID. If a change needs other resources or runtime files, extend and verify the overlay before building rather than assuming the helper includes them.

The R1 packer sets the internet update channel to the public fork, `pbjorklund/sonix-audiobooks`, and verifies it in the unpacked image. An R1 still pointing at upstream needs one SD update with a fork build before it can check the fork.

Install Python 3, `7z`, `squashfs-tools`, and `xorriso` (which supplies `xorrisofs`). Use the [official Sonix v0.6.2 R1 image](https://github.com/yetisoldier/sonix-audiobooks/releases/tag/v0.6.2) as the base for this branch. Obtain it from the trusted release and check it against a known digest, not a digest calculated only after receiving an untrusted file. GitHub lists the `r1.upt` asset with SHA-256 `40da4e001e03596b747f0b132de1d998718fc55f991900fbef462d9e6a3819fb`.

```bash
# Replace this path with the downloaded official Sonix R1 image.
BASE=/path/to/official/sonix-v0.6.2/r1.upt
make -C sonix-player target -j"$(nproc)"
sha=$(git rev-parse --short HEAD)
out="build/firmware/r1-$sha"
python3 tools/pack-r1.py --base "$BASE" \
  --base-sha256 40da4e001e03596b747f0b132de1d998718fc55f991900fbef462d9e6a3819fb \
  --build-id "r1-$sha" --output "$out"
(cd "$out" && sha256sum -c SHA256SUMS)
```

Pass `--seven-zip /path/to/7z` if it is not on `PATH`. Use a new output directory for each build. Record the full source SHA and whether the tree was dirty; release builds require a clean tree. The helper checks the ABI, both OTA chains, unchanged kernel bytes, and the packaged player and language files against their inputs. It produces a local `r1.upt`, `sonix_player`, and `SHA256SUMS`; it does not stage or install anything. The README's stock-input packer serves a different upstream workflow.

## Deploy and verify on the original R1

1. Enable ADB in **Settings → System → About**: tap **Build number** five times, then open **Settings → Developer options** and enable **ADB**. Connect over USB and run `adb devices -l`. Use the explicit serial every time; it can change after reboot. See [device testing](../DEVICE_TESTING.md) for the local dev-control interface.
2. Confirm the model and installed build before writing. Replace `SERIAL` below with the listed USB serial. Inspect the card mount and free space, preserve any pending update, and keep a private userdata backup and known-good recovery image before installation.

   ```bash
   adb -s SERIAL shell 'grep -E "device-name|build_version" /usr/resource/sonix/components/system-info.json'
   adb -s SERIAL shell 'grep " /mnt/sd_0 " /proc/mounts; df -h /mnt/sd_0'
   ```

3. For authorized staging, verify `SHA256SUMS` on the host. Copy `r1.upt` to the root of the confirmed microSD card, using a temporary filename first if copying over ADB. Verify its SHA-256 on the card before moving it to `r1.upt`. Do not overwrite an unrelated update. Staging does not install it.
4. Charge above 30%. On the R1 use **Settings → System → Update firmware → From SD card**. Never write raw flash. After reboot, reselect the USB serial, confirm the build ID, and compare `adb -s SERIAL shell 'sha256sum /usr/bin/sonix_player'` with the packaged player hash.
5. Record the actual device checks: Bluetooth playback; audiobook and podcast playback with Wi-Fi off and resume after a normal reboot; podcast sync after reconnecting; TIDAL Downloads, Now Playing and normal Music scanning. Follow each task's acceptance criteria. Leave ADB enabled on the development R1. Remove the applied update from the SD root once verified. If rollback is needed, install the known-good original-R1 image through the same SD update flow.

Keep provisioning separate from firmware. Preserve `/usr/data/audiobookshelf_profiles.ini` and use `/usr/data/streaming-keys.ini` for private TIDAL app keys. Both are root-owned, mode `0600`. Restart Sonix after provisioning. Do not disable TLS, print credentials, put them in command arguments, or bundle them in firmware.

## Cut a release

- Confirm the authorized repository, branch, version, and intended audience. Publish fork OTA releases to `pbjorklund/sonix-audiobooks`, not upstream. Use a strictly increasing numeric version: the SD bootstrap uses `1.3.1.1`, so a subsequent release could be `v1.3.1.2`. Set `--build-id` to the same version without `v`; the updater compares up to four numeric components. Do not use a date stamp or a commit hash as an OTA release version.
- The device checks public GitHub Releases without a token. It ignores drafts and prereleases and looks for an uploaded asset named `r1.upt`. Use **Settings → System → Update firmware → Via internet** to find the newer build.
- Review changes since the last release and update the [changelog](../CHANGELOG.md). Build and verify from the exact clean source commit. Run the matching host and device checks. Record the tested commit, build ID, model and hashes in the release notes. Do not promote experimental features as verified or supported where service authorization or hardware checks are still open.
- Verify the release tag points to the intended source commit on the remote. Wait for `host-build.yml` on that tag before publishing the draft.
- With publication authorization, tag the verified commit and create a **draft** on the intended GitHub repository with `r1.upt`, a release-side `SHA256SUMS` for the files actually attached, and the notes. For a release attaching only `r1.upt`, run `sha256sum r1.upt` in the artifact directory to generate that manifest; the build-time manifest also lists the player binary. Verify the tag commit, model, filenames and uploaded asset hashes before publishing. Download the published asset again and confirm its hash and release visibility. Do not attach private fixtures, credentials, userdata, recovery firmware or internal logs.

## Upstream and licensing

Keep fork-specific commits focused and preserve the GPL-3.0 license and upstream copyright notices. Bug fixes that are not specific to Sonix Audiobooks should be suitable for proposing back to [Tartarus6/sonix-player](https://github.com/Tartarus6/sonix-player).
