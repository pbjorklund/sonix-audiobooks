# Sonix on the original HiBy R1

Use this checkout for Audiobookshelf audiobooks and podcasts, TIDAL, and device verification on the original HiBy R1. Stock HiBy Books integration and ebook reading are out of scope. Track work in the bound `hiby-r1-abs` AmpliFlow project. The sibling `../hiby-r1-abs/` holds historical notes, firmware artifacts, and a separate private archive of the earlier prototypes; do not resume development there.

Read [the development guide](docs/DEVELOPMENT.md) before building firmware, using ADB, staging an update, or cutting a release.

## Source and builds

- `origin` is `pbjorklund/sonix-audiobooks`; `upstream` is `yetisoldier/sonix-audiobooks`. Push only when authorized. Preserve uncommitted work when updating the checkout.
- Edit the player in `sonix-player/` and R1 resources in `sonix-packer/assets/R1/`. Add focused unit tests for new code, run the relevant host checks, and run `make -C sonix-player target` for the MIPS build and glibc 2.22 ABI check.
- Package on the official Sonix R1 runtime to preserve the Bluetooth libraries. Include changed R1 resources, especially language files. Verify both OTA checksum chains and compare the unpacked player and changed resources against the build inputs. Record the firmware SHA-256. After tested device-facing changes, package and install on the connected original R1 by default. Leave ADB enabled for ongoing development. This does not authorize pushing or publishing.
- Keep firmware, build output, device logs, and userdata backups out of Git. Write local test firmware and its `SHA256SUMS` to `build/firmware/`.

## Credentials and device checks

- Keep the private credentials in `../hiby-r1-abs/.env` (mode `0600`). Treat it as data, not a shell script. Never print keys, pass them in arguments, or include them in firmware. Keep `streaming-keys.ini`, `streaming-keys.key`, and `streaming-keys.bin` out of Git and shared artifacts.
- Keep the HTTPS Audiobookshelf hostname from `../hiby-r1-abs/.env`. Home DNS resolves it to the LAN reverse proxy; the R1 is not on the tailnet. Do not substitute an IP or disable TLS verification. Preserve existing profiles in the root-owned mode-`0600` `/usr/data/audiobookshelf_profiles.ini` and restart Sonix after provisioning. TIDAL application keys are provisioned privately in `/usr/data/streaming-keys.ini` (root-owned, mode `0600`).
- Before device access, run `adb devices -l`, select the physical R1 explicitly, and confirm the model and current Sonix build. The USB serial can change after reboot. To enable ADB: Settings → System → About → tap Build number five times, then Settings → Developer options → ADB.
- Before an authorized flash, verify the firmware hash, confirm the SD card, and retain private userdata and firmware recovery copies. Use Settings → System → Update firmware → From SD card; never write raw flash. No reflash is needed for routine verification.
- Verify playback on the player with Wi-Fi off and check resume after a normal reboot for an audiobook and an Audiobookshelf podcast. Check TIDAL Downloads and normal Music scanning on the player. A host test, working catalogue, or successful flash is not proof of physical playback or resume. Record what was observed without exposing credentials or private media.
