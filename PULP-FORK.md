# CrossPoint Reader — Pulp fork

Personal fork of [crosspoint-reader](https://github.com/crosspoint-reader/crosspoint-reader)
for an Xteink X4 Pro that pulls its reading queue from a local **pulp** server
(`pulp`, FastAPI, port 8794: tabs → EPUB). Upstream's ROADMAP
excludes sync engines and auto-download by design, so this lives here.

Branch: `pulp` off upstream `develop` @ `4b17a7bb`. Build target: `pio run -e x4pro`.

## What changed

### 1. Pulp sync (one tap + automatic)
- **Home → "Pulp"** menu item (shown when a Pulp URL resolves) runs
  `PulpSyncActivity`: joins a saved Wi-Fi network headlessly (last-connected
  SSID first, then the other saved networks), fetches `GET <pulp>/api/shelf`,
  downloads every EPUB whose filename is not already in the OPDS download
  folder (`SETTINGS.opdsDownloadFolder`, "" = SD root), shows *n of m* +
  filename + progress bar, then drops Wi-Fi and returns Home. Files keep the
  **server filename** (`YYYY-MM-DD-slug.epub`). Failures are counted and
  skipped; Back cancels the run.
- **Auto-sync on boot/wake** (Settings → System → *Pulp Auto-Sync*, default
  on; *Interval* default 30 min): when the boot route is Home and the last
  attempt is older than the interval, the sync runs first with a single small
  status line at the bottom of an otherwise blank screen, then hands off to
  Home. One 8 s connect attempt, so an absent network can never stall boot.
  Waking straight into a book skips it. The last-attempt stamp lives in RTC
  slow memory (`RTC_NOINIT_ATTR`), so it survives deep sleep and the silent
  reboots but a power-off syncs once on the next boot.
- **Pulp URL**: the OPDS server entry named `Pulp` (URL minus `/opds`), else
  the compile-time `PULP_DEFAULT_URL` (set for `x4pro` in `platformio.ini`),
  else the feature is hidden. Credentials of that OPDS entry are reused.
- Manual runs reboot on exit like the OPDS browser does (heap defrag after a
  Wi-Fi session); the boot-time auto-sync does not (fresh heap, and a second
  boot would double the wake time).

### 2. Newest first, everywhere, with real dates
- **File browser**: new default order — folders first, then files by leading
  `YYYY-MM-DD` (desc), then SD mtime (desc), then natural name order.
  Settings → System → *Browse Files: Newest First* (off = upstream order).
- **Library**: already defaults to `RecentDesc` upstream
  (`LibraryListActivity.h`), so no change; it orders by mtime, which the
  next item fixes.
- **Real mtimes on the card**: upstream never installs SdFat's
  `FsDateTime` callback, so every download got the default 2000-01-01 stamp.
  `HalFileTime::begin()` (called from `setup()`) stamps writes from the RTC
  (X4 Pro has a PCF8563 via `halClock`), falling back to the system clock once
  SNTP has set it, and keeps SdFat's default while neither is plausible.
  `PulpSyncActivity` re-syncs the RTC from NTP when it has never been synced
  or reads implausibly; on RTC-less boards it kicks SNTP once per power cycle.
- **OPDS downloads keep the server filename**: new
  `OpdsFilenameFormat::ServerFilename` (basename of the acquisition href,
  percent-decoded, sanitized, `.epub` guaranteed), selectable under OPDS
  Servers → Filename Format, and the default. A settings file written before
  the fork stores the old value explicitly, so `fromJson()` flips it once
  (`pulpForkMigrated`). Books pulled via the OPDS browser and via Pulp sync
  therefore land under the same name and dedupe against each other.

## Files

New:
- `src/activities/network/PulpSyncActivity.{h,cpp}` — the sync screen / state machine
- `src/network/PulpConfig.{h,cpp}` — URL resolution, auto-sync gate + RTC-memory stamp
- `src/network/PulpShelf.{h,cpp}` — `/api/shelf` fetch through `StreamingJsonParser`
- `lib/hal/HalFileTime.{h,cpp}` — SdFat timestamp callback
- `PULP-FORK.md` (this file)

Modified (small, localized):
- `src/CrossPointSettings.{h,cpp}` — `pulpAutoSync`, `pulpAutoSyncMinutes`,
  `fileBrowserNewestFirst`, `pulpForkMigrated`; `opdsFilenameFormat` default 3; one-shot migration
- `src/SettingsList.h` — the three System entries, the migration flag, the 4th filename-format label
- `src/util/OpdsFilename.{h,cpp}` — `ServerFilename` + `opdsServerFilename()`
- `src/activities/browser/OpdsBookBrowserActivity.cpp` — use it when selected
- `src/activities/settings/OpdsServerListActivity.cpp` — label + picker entry
- `lib/FsHelpers/FsHelpers.{h,cpp}` — `leadingDateKey()`, `sortFileListNewestFirst()`
- `src/activities/home/FileBrowserActivity.cpp` — collect mtimes, pick the sort
- `src/activities/home/HomeActivity.{h,cpp}` — the Pulp menu item
- `src/activities/ActivityManager.{h,cpp}` — `HomeMenuItem::PULP`, `goToPulpSync()`
- `src/main.cpp` — `HalFileTime::begin()`, boot-route hook for auto-sync
- `lib/I18n/translations/english.yaml` — `STR_PULP*`, `STR_FILE_BROWSER_NEWEST_FIRST`, `STR_FMT_SERVER`
- `platformio.ini` — `-DPULP_DEFAULT_URL` on `[env:x4pro]`
- `test/opds_filename/OpdsFilenameTest.cpp`, `test/fs_helpers/FsHelpersTest.cpp` — host tests for the new helpers

Server side (separate repo, `pulp`): `GET /api/shelf` →
`[{"file","url","size","mtime"}, …]` newest first, same set as `/opds`.

## Rebasing on upstream

```sh
git fetch origin
git rebase origin/develop      # the fork branches from develop, per AGENTS.md
```
Expected conflict surface: `HomeActivity.{h,cpp}` (menu order), `SettingsList.h`
(System block), `CrossPointSettings.h` (field block), `main.cpp` (boot routing),
`english.yaml` (appended keys). Everything else is additive. After a rebase:
`./bin/clang-format-fix -g`, `pio run -e x4pro`, and the two host tests
(`cmake -S test -B build && cmake --build build --target OpdsFilenameTest FsHelpersTest`).

## OTA from pulp (not built, investigated)

`src/network/OtaUpdater.cpp` fetches GitHub's `releases/latest` JSON, picks
`crosspoint-<tag>-x4pro.bin`, streams it with `HttpDownloader::fetchUrl` into
`esp_ota_begin/write/end` on the inactive slot, and aborts on a
`FirmwareBoardTag` mismatch. To self-host: pulp serves a JSON document shaped
like a GitHub release (`tag_name`, `assets[{name,browser_download_url,size}]`)
plus the `.bin`, and the fork points `latestReleaseUrl` at it (plain `http://`
works — the transport follows the URL scheme). `isUpdateNewer()` compares the
tag against `CROSSPOINT_VERSION`, so the fork's tag must sort above `1.6.0`
(e.g. `1.6.0-pulp.N` → check the comparator, or bump minor). Hardware note:
the device's OTA data partition already has both `app0`/`app1` slots
(`partitions.csv`), so OTA needs no reflash of the table.
