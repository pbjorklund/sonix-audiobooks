# Changelog

All entries here are specific to Sonix Audiobooks. The project also inherits ongoing work from [Sonix Player](https://github.com/Tartarus6/sonix-player); upstream changes are not repeated line-by-line here.

## Sonix Audiobooks 1.3.1.17 (2026-11-27)

- **Home:** Open TIDAL and Audiobookshelf directly from the start screen. Wireless and local Audiobooks are under **More**. The **Streaming** menu still has all its sources.

## Sonix Audiobooks 1.3.1.16 (2026-11-27)

- **TIDAL playback:** Tapping a track shows **Loading…** immediately and ignores other track taps until the selected track is ready or an error is shown. A failed start leaves the album open so you can retry.

## Sonix Audiobooks 1.3.1.14 (2026-11-27)

- **TIDAL lyrics:** Open **Lyrics** from the Now Playing menu or swipe left across the cover. Timed lyrics highlight and follow the current line. Tap a line to seek. Plain lyrics stay readable, and saved lyrics work offline.
- **Single-ear audio:** Add the button in Control Center settings. Tap to cycle from both ears to left-ear mono, right-ear mono, and back. Both channels are mixed into the selected ear without changing saved mono or balance settings. Bit-perfect and DSD playback are excluded.
- **TIDAL playlist downloads:** Playlist tracks share one copy instead of creating separate one-track album folders. Downloading the album at the same quality can move a playlist copy into the album without downloading the audio again. Existing album files and linked imports are reused.

## Sonix Audiobooks 1.3.1.8 (2026-11-26)

- **Audiobookshelf resume:** Starting a downloaded episode or linked audiobook checks the server for a newer listening position. If the server is unreachable, playback uses the position saved on the R1. Browsing Downloads still works without a network request.
- **Play next on large podcast shows:** The player looks up saved progress in one pass and checks the SD card only for visible rows. Oldest and Newest start playback before adding the selected and following episodes to the download queue.

## Sonix Audiobooks 1.3.1.6 (2026-11-25)

First release on the fork's update channel for the original HiBy R1.

- **Offline Downloads:** Browse Audiobookshelf and TIDAL downloads from the SD card. Downloaded Audiobookshelf podcasts start from the saved local position, and progress sync runs separately. TIDAL's explicit Refresh can still go online.
- **TIDAL download quality:** Downloading again at a different quality asks before replacing existing copies. A failed replacement keeps the old copy playable. Linked imports are left alone.
- **TIDAL library:** Play downloads offline, save albums for later, and link matching files already on the card without taking ownership of them.
- **Audiobookshelf podcasts:** Browse shows and episodes, filter by listening state, and play the oldest or newest unfinished episode. Downloaded episodes keep artwork and playback progress.
- **Sleep timer:** Start it from Control Center or use a daily schedule in Settings.

## Sonix Audiobooks 0.6.2 - 2026-11-23

- **Book detail cover:** The book cover now renders at the top of the detail page (it was previously placed below the fold and could render as a blank card until a later redraw). Fixed the cover decode completing without a repaint.

## Sonix Audiobooks 0.6.1 - 2026-11-23

- **Hat Blue Theme:** New iOS-inspired dark theme with royal-blue `#135EDB` accents on near-black `#08090C` screens and `#191B21` cards. Menu tiles are now flat with 22px rounded corners.
- **Restyled Icons:** 37 menu icons updated to layered blue-gradient artwork, plus 3 page illustrations and 3 playback badges. Brand-blue icons are preserved regardless of selected control accent.
- **Audiobookshelf Icon:** Added a dedicated icon for Audiobookshelf, separate from the local Audiobooks tile on the Home screen.

## Sonix Audiobooks 0.6.0 - 2026-11-23

- **Multiple ABS Servers:** Added support for named URL+token profiles with a profile picker and editor. Includes per-operation auth snapshots and same-origin enforcement.
- **Browse Refinements:** Expanded search to 100 matches with truthful overflow. Added exact sorting and filtering by title, author, date, duration, series, genres, and tags. Implemented virtual scrolling for smoother browsing.
- **Download Queue:** Added a persistent 16-job FIFO queue with durable state. Supports pause, resume, cancel, remove, and retry, with automatic recovery from crashes or power loss and network backoff.
- **Book Detail Page:** New pre-download view showing full metadata (narrator, series position, description, chapters) and authenticated cached covers. Options to play local, link existing, or queue for download.
- **Deferred:** Podcasts via ABS (M5) have been deferred to a future release.

> [!NOTE]
> **Downgrade Note:** A downgrade to 0.5.0 may require re-entering ABS credentials; it must not require redownloading media or resetting local resume.
> **R3PII Parity:** The R3 Pro II English catalogue currently lags the new keys and is source-parity-only; this is not a support claim.

## Sonix Audiobooks 0.5.0 - 2026-11-21

Rebuilt on upstream sonix-player v1.3.1 (commit `5d8bb17`).

> [!IMPORTANT]
> **Manual Upgrade Required:** Users on 0.4.0 must flash this release from an SD card. Because 0.4.0 images report OS 1.3.0, the OTA system cannot jump directly to 0.5.0.

- **Custom Boot Screen:** Replaced all 12 R1 variants with a new custom boot screen (SonixOS lockup + blue DJ gnome).
- **Correctness Fixes:**
  - Effects bypass made non-destructive.
  - EQ write batching and atomic presets implemented.
  - Cover-cache updated to full-SHA-256 with a 256-row bound.
  - Fixed Japanese boundary, password, and lifecycle issues.
- **Vendored Code Note:** The vendored `stb_image_write.h` contains a known benign signed-shift UB (pre-existing upstream; only the low 24 bits are used), surfaced by sanitizers.

## Sonix Audiobooks 0.4.0 - 2026-10-17


First release of the fork's new line, rebuilt on upstream sonix-player v1.3.0.
Firmware build identifier: `171020262008`. The 0.x history (0.1.0-0.3.0)
remains available on `archive/0.x-line`; this line supersedes it.
See `docs/releases/0.4.0.md` for package hashes and the device validation record.

- Base changed to upstream v1.3.0. Everything the 0.x line patched by hand
  (playback/pairing/power hardening, background checkpoints, simulated-card
  databases, USB realpath sizing) is upstream's own now and the local copies
  are dropped. See `docs/REBASE_PLAN.md` for the commit-by-commit disposition.
- The Audiobookshelf stack is re-applied on the new base: server libraries,
  search, authors and series, downloads with sidecars, existing-copy linking,
  offline-first progress sync and the Now Playing status — against upstream's
  audiobook schema v4 (background writer, card epoch, wipe-and-refill scan).
- The 0.x kernel-log 64-read cap and exact-match compilation-flag fixtures are
  retired in favour of upstream's own defences (print-capped kernel flood,
  prefix-matching flags); the regression suite asserts upstream's behaviour.
- Audiobookshelf strings ship in all twelve upstream languages — 24 files
  across R1 and R3 Pro II. As in 0.x, the non-English blocks are English
  placeholders until translated (0.x never carried translated ABS strings).
- First boot over a card last used by 0.3.0 triggers one audiobook rescan
  (schema 3 -> 4); positions are rebuilt from the scan stash and ABS linkage
  survives.

## Sonix Audiobooks 0.3.0 - 2026-10-01

Public feedback release for the original HiBy R1. Firmware build identifier: `011020261530`.

- Added **Search**, **Authors**, and **Series** directly inside each Streaming > Audiobookshelf library.
- Search uses the server's whole-library search, with an on-screen keyboard and up to 40 matching books. Broad searches prompt you to refine the query.
- Authors and series are alphabetically listed with 40-entry pages. Selecting a series requests books in series sequence; selecting an author lists their books by title.
- Back returns from filtered books to the author/series list. **All books** clears the active search or filter.
- Network requests run in the background rather than blocking the interface.
- Expanded the mock-server smoke test to verify search submission, encoded author/series filters, sequence sorting requests and back navigation, alongside download, multipart resume and progress upload.
- Verified the installed executable on a physical R1, then checked author browsing, series order and keyboard search against a live server. This is not a long-duration playback or battery test.
- Updated the usage guide and added physical-device screenshots of the new browsing views.

## Sonix Audiobooks 0.2.0 - 2026-10-01

Public feedback release for the original HiBy R1. Includes the changes below since 0.1.0. Firmware build identifier: `011020261348`.

### Selective upstream improvements

- Backported upstream compilation-artist display: Now Playing, the screensaver, and the quick panel show the track performer for tagged compilations or albums credited to Various Artists. Ordinary album-artist preference and audiobook catalog author/grouping are unchanged.
- Read compilation tags from ID3v2, MP4, Vorbis comments and APE tags. Extended the backport to UTF-16 TXXX flags and exact boolean-value matching.
- Bounded kernel-log reads per watchdog pass, including repeated interrupted/overrun reads, so a log flood cannot keep that loop draining indefinitely. This does not enable automatic reboot on a kernel oops.
- Added sanitizer-backed metadata fixtures and mocked kernel-log flood tests. See [upstream integration review](docs/UPSTREAM_REVIEW_2026-10-01.md) for scope and deferred changes.

### Audiobookshelf integration

- Added an optional Audiobookshelf client in Streaming.
- Added server and token configuration, library browsing, and book detail views.
- Added downloads to `/Audiobooks/Audiobookshelf/<author>/<title>/` with artwork, title, author, series, and description sidecars that the local audiobook scanner understands.
- Added safe handling for interrupted downloads: incomplete files keep a `.part` suffix and are not indexed as playable books.
- Added local-book linking for existing SD-card copies when the server book has matching parts and duration, avoiding a redundant download.
- Added server-position import when downloading/linking a book and background progress uploads for downloaded and linked books.
- Made uploads offline-first: the existing local checkpoint remains pending until the server acknowledges that exact position. Pending progress survives Wi-Fi being off, server errors, book changes and reboot; turning Wi-Fi on resumes attempts without reopening the book.
- Added bidirectional conflict resolution for linked books: newest listening timestamp wins, including intentional rewinds. Missing, implausible or tied timestamps fall back to the farther whole-book position (completed counts as the end).
- Added server-clock plausibility checks, millisecond local event timestamps preserved through scans, verified uploads, and transactional imports guarded against concurrent local edits. Incoming progress waits while the book is playing; paused books use the imported file/position on their next Play.
- Added reconnect and bounded periodic checks for newer server progress on already-synced books. The supported GET/PATCH progress API is used instead of the removed legacy sync route; simultaneous server writes are not atomic across that API.
- Added a Now Playing status indicator for local-only, linked, pending, waiting for Wi-Fi, syncing, synced, synced-offline and retry-pending states.
- Centered the sync status above the artwork and sized its shaded background to the wording, with compact padding and wrapping for longer text.
- Kept Wi-Fi under user control: sync never enables it, makes no upload attempts while it is off, and has no periodic retry wake-up while it stays off. UI reads a cached status rather than querying the network or SD card.
- Added server binding to new link manifests and card-generation checks on acknowledgements. An old response cannot mark a newer backward seek or a replacement card as synced.
- Added direct mapping between Audiobookshelf whole-book progress and the correct local multipart file and offset.
- Added an Audiobookshelf host smoke suite covering download, resume import, existing-copy linking, local-progress upload, and remote progress upload.
- Added an outage/restart smoke test and sanitizer-backed durable-progress tests covering multiple books, backward seeks, completion, stale acknowledgements and card/server isolation.
- Added simulator conflict tests for local and remote rewinds, paused multipart resume through the physical Play handler, missing/future timestamps and timestamp ties.

### Stability and correctness

- Fixed non-terminating pathname copies in the audio playback path.
- Fixed an empty folder browser path that could call `qsort` with a null entry buffer.
- Increased the saved setting value capacity so ordinary Audiobookshelf API tokens are not truncated.
- Added bounded parsing for server-supplied titles and descriptions.
- Kept `Authorization` headers on same-origin redirects only; credentials are not forwarded to a redirected media host.

### Validation

- Added sanitizer, static-analysis, target ABI, package, local audiobook, and Audiobookshelf smoke coverage.
- Validated the installed candidate on an R1: normal boot, empty-folder handling, direct local resume, playback/pause, and 15 repeated Audiobooks enter/exit cycles with stable process memory, thread count, and file-descriptor count.

## 0.1.0 - 2026-09-30

First public Sonix Audiobooks release, based on Sonix Player 1.1.0.

### Audiobook experience

- Added a dedicated Audiobooks home with Library, Series, Authors, Continue, Bookmarks, and Folders views.
- Added custom audiobook navigation icons and a distinct home-screen icon.
- Added indexed folder-hierarchy browsing without synchronous SD-card directory walks.
- Added manual per-book bookmarks with a global list, direct resume, and deletion.
- Added summary/description indexing and display for common M4B/MP4 and MP3 metadata.
- Hardened single-file and multipart resume with background checkpoints, saves at important transitions, direct saved-part return, completion reset, and card-swap isolation.
- Added Audiobook Now Playing actions for chapters, adding and opening bookmarks, and book descriptions.
- Improved multipart auto-advance by sending decoder completion to the controller immediately instead of waiting for the UI progress timer.

### R1 reliability

- Fixed R1 touch-loader line endings that could leave the screen unresponsive after firmware update.
- Replaced fixed Linux input-event assumptions with capability and name discovery for touch, physical keys, headset controls, and touch-wake monitoring.
- Added a packaging audit that rejects malformed module-loader scripts.
- Reconciled successfully saved Bluetooth bonds when BlueZ reports a late pairing timeout.
- Restored audio routing and DAC state after every suspend attempt, including a rejected kernel suspend.
- Bounded real-time CPU behavior so a faulty worker cannot indefinitely starve the single-core interface and kernel tasks.
- Enabled the existing 500 mA R1 charge-current cap by default without changing battery-percentage reporting.

### Development and validation

- Added a root-only local ADB UI-control channel for tap, swipe, wake, status, and framebuffer screenshots.
- Added a Windows helper and adversarial input probe-order test.
- Expanded the headless host smoke test to cover multipart indexing, direct resume, bookmarks, restart persistence, and automatic part advance.
- Added ABI, package, installed-hash, database-integrity, boot, memory, and input-routing release checks.
