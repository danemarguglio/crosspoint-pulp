# CrossPoint Reader — Pulp fork

Personal fork of [crosspoint-reader](https://github.com/crosspoint-reader/crosspoint-reader)
for an Xteink X4 Pro that pulls its reading queue from a local **pulp** server
(`pulp`, FastAPI, port 8794: tabs → EPUB). Upstream's ROADMAP
excludes sync engines and auto-download by design, so this lives here.

Branch: `pulp` off upstream `develop` @ `4b17a7bb` (version 1.6.5). Build target: `pio run -e x4pro` → `CROSSPOINT_VERSION` `1.6.5-x4pro`.

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

### 3. Hacker News reader (2026-09-20)
- **Home → "Hacker News"** (right after Pulp, shown under the same condition)
  runs `HnFeedActivity`: headless join of a saved network (`SavedWifiJoiner`,
  the PulpSync recipe), then `GET <pulp>/hn/feed?kind=…&page=…&per=30`. Tabs
  **Top · New · Best · Ask · Show** on the `UiTabListActivity` ring; rows are a
  two-line title plus `123 pts · 45 comments · example.com · 3h`; `« Previous
  page` / `More…` rows at the ends, and the side buttons page when the
  selection runs off either end. Back/Home tears Wi-Fi down with the OPDS
  browser's heap-defrag reboot.
- **Story** (`HnStoryActivity`, pushed on top of the feed so Wi-Fi stays up):
  header (title, meta), a six-button toolbar `Save · Thread › · < · > ·
  Article · Comments`, and the body. **Article** = `article.paragraphs` (or
  Ask HN `text`) laid out with the reader engine (`ParsedText` → `TextBlock`,
  reader font/alignment) and paged; `article.ok=false` shows the reason and
  points at Comments. **Comments** = depth-first order indented 12 px per
  level (visual depth capped at 6, `»` beyond), a thread line per level,
  `author · age` in the small font, bodies wrapped in Noto Sans 14. Screen
  pages fill from a cursor into the flattened list; one API page (40) is
  resident, forward paging fetches the next, back paging re-lays remembered
  starts. Confirm on a comment folds/unfolds its subtree (`[+N]`, `+` when
  the subtree runs past the loaded page); `Thread ›` jumps to the next
  top-level comment; `Save` POSTs `/hn/save/{id}` → "Saved to Pulp".
- **Buttons** (X4 Pro: two side keys + power-click Confirm + touch): Up/Down
  walk the ring (toolbar buttons, then body items); Down past the last body
  item = next page, Up from the first = toolbar; a held side key pages; taps
  hit the same targets; swipe left/right pages; back gesture = feed.
- **Immersive reading** (2026-09-25): the title/meta/toolbar block shows on
  entry and hides on the first page turn, giving the text the whole 800 px
  minus the footer line (thread lines stay in Comments). Back: swipe up
  (anywhere, bottom edge included — the top-edge down swipe is the
  control center), Up from the first body item, or a long Confirm press on
  boards with a front Confirm key. Any page turn hides it again; a downward
  swipe hides it too. Hiding keeps the column width, so line breaks are
  identical and the current page is re-laid from the same first word into the
  taller area (later page boundaries move; the page counter counts page
  starts). Article body is ragged-right with hyphenation regardless of the
  reader's justify setting; comment bodies follow the reader's point size in
  the sans family, the article the reader's own font.
- **Memory**: heap (+PSRAM) logged on entering each screen and after every
  fetch (`HN` tag). Fetches refuse below 28 KB free / 8 KB max block
  (`hn::MIN_FETCH_*` — plain http, so no TLS record buffer; the socket, the
  1 KB read chunk, the parser's 512 B token buffer and one parsed page have
  to fit). Comment text is capped at 4 KB each, a page at 64 comments, an
  article at 32 KB / 256 paragraphs. `StreamingJsonParser` gained an
  optional `onStringPart` callback so values longer than its 512 B token
  buffer stream through instead of being dropped (existing callers unchanged).
- Server contract (pulp `/hn/*`, fixed): `GET /hn/feed?kind&page&per` →
  `{kind,page,per,has_more,items:[{id,title,url,domain,points,by,age,comments,kind}]}`;
  `GET /hn/item/{id}?page&per&article=0|1` → `{id,title,url,domain,points,by,age,kind,
  comments_total,text,article?{ok,title,byline,paragraphs[],truncated,error},
  comments:[{id,by,age,depth,text,dead,kids}],page,per,has_more}` (comments
  already in reading order; `article` only when `article=1`);
  `POST /hn/save/{id}` → `{queued,key,title}`; `GET /hn/health`.
- Home rows: the menu draws at a fixed pitch and seven rows would run into
  the button hints, so the **OPDS Browser row is hidden when the only
  configured OPDS server is the "Pulp" entry** (`pulp::opdsBrowserSuperseded()`
  — the Pulp item covers it). Any other server configured brings it back
  (seven rows, Lyra/Classic portrait overflows ~50 px). `hasOpdsServers` /
  `hasPulp` in `HomeActivity` drive `getMenuItemCount()`, `menuItemToIndex()`
  and `indexToMenuItem()` alike; an initial OPDS_BROWSER selection maps to
  row 0 when the row is hidden.

## Files

New:
- `src/activities/network/PulpSyncActivity.{h,cpp}` — the sync screen / state machine
- `src/network/PulpConfig.{h,cpp}` — URL resolution, auto-sync gate + RTC-memory stamp, `opdsBrowserSuperseded()`
- `src/network/PulpShelf.{h,cpp}` — `/api/shelf` fetch through `StreamingJsonParser`
- `lib/hal/HalFileTime.{h,cpp}` — SdFat timestamp callback
- `src/activities/hn/HnFeedActivity.{h,cpp}` — the tabbed front page
- `src/activities/hn/HnStoryActivity.{h,cpp}` — article + threaded comments
- `src/activities/hn/HnTextLayout.{h,cpp}` — plain text → `TextBlock` lines via `ParsedText`
- `src/network/HnJson.{h,cpp}` — models + streaming sinks (host-testable)
- `src/network/HnClient.{h,cpp}` — the three `/hn` calls, heap floor
- `src/network/SavedWifiJoiner.{h,cpp}` — headless saved-network join
- `test/hn_json/` — host tests for the feed/item shapes, `has_more`, depth, long strings
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
- `src/activities/home/HomeActivity.{h,cpp}` — the Pulp and Hacker News menu items
- `src/activities/ActivityManager.{h,cpp}` — `HomeMenuItem::PULP` / `HACKER_NEWS`, `goToPulpSync()`, `goToHackerNews()`
- `lib/JsonParser/StreamingJsonParser.{h,cpp}` — optional `onStringPart` (long string values); `test/streaming_json_parser/` covers it
- `src/main.cpp` — `HalFileTime::begin()`, boot-route hook for auto-sync
- `lib/I18n/translations/english.yaml` — `STR_PULP*`, `STR_HN*`, `STR_FILE_BROWSER_NEWEST_FIRST`, `STR_FMT_SERVER`
- `platformio.ini` — `-DPULP_DEFAULT_URL` on `[env:x4pro]`
- `test/opds_filename/OpdsFilenameTest.cpp`, `test/fs_helpers/FsHelpersTest.cpp` — host tests for the new helpers

Server side (separate repo, `pulp`): `GET /api/shelf` →
`[{"file","url","size","mtime"}, …]` newest first, same set as `/opds`; the
`/hn/*` proxy described above.

## Rebasing on upstream

```sh
git fetch origin
git rebase origin/develop      # the fork branches from develop, per AGENTS.md
```
Expected conflict surface: `HomeActivity.{h,cpp}` (menu order), `SettingsList.h`
(System block), `CrossPointSettings.h` (field block), `main.cpp` (boot routing),
`english.yaml` (appended keys). Everything else is additive. After a rebase:
`./bin/clang-format-fix -g`, `pio run -e x4pro`, and the host tests
(`cmake -S test -B build && cmake --build build --target OpdsFilenameTest FsHelpersTest HnJsonTest StreamingJsonParserTest`).
The wrapper wants clang-format ≥ 21; `pip install clang-format` into the venv and
prefix `PATH=<venv>/bin:$PATH` if none is on the path.

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
