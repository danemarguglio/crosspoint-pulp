#include "PulpSyncActivity.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_mac.h>

#include <cstdio>
#include <cstdlib>
#include <ctime>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "WifiCredentialStore.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "network/PulpConfig.h"
#include "util/BookCacheUtils.h"
#include "util/OpdsFilename.h"

PulpSyncActivity::PulpSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const bool silent,
                                   const bool cleanHomeRefresh)
    : Activity(silent ? "PulpAutoSync" : "PulpSync", renderer, mappedInput),
      silent(silent),
      cleanHomeRefresh(cleanHomeRefresh) {}

void PulpSyncActivity::onEnter() {
  Activity::onEnter();

  baseUrl = pulp::baseUrl();
  server = pulp::server();
  headline = tr(STR_PULP_SYNC);
  if (baseUrl.empty()) {
    complete(tr(STR_PULP_NOT_CONFIGURED));
    return;
  }

  WIFI_STORE.loadFromFile();
  const size_t count = WIFI_STORE.getCredentialCount();
  attemptSsids.clear();
  attemptSsids.reserve(count);
  const std::string last = WIFI_STORE.getLastConnectedSsid();
  if (!last.empty() && WIFI_STORE.hasSavedCredential(last)) attemptSsids.push_back(last);
  for (size_t i = 0; i < count; i++) {
    const auto ssid = WIFI_STORE.getSsidAt(i);
    if (ssid && *ssid != last) attemptSsids.push_back(*ssid);
  }
  // A single attempt at boot: the point of the bound is a boot that never waits
  // on a network that isn't there.
  if (silent && attemptSsids.size() > 1) attemptSsids.resize(1);

  // Even a failed attempt counts, so a dead network can't retrigger on every wake.
  pulp::stampAutoSync();

  if (WiFi.status() == WL_CONNECTED) {
    state = State::SYNCING;
    headline = tr(STR_PULP_FETCHING_SHELF);
    requestUpdate();
    return;
  }
  if (!startNextAttempt()) {
    complete(tr(STR_CLOCK_SYNC_NO_WIFI));
    return;
  }
  requestUpdate();
}

void PulpSyncActivity::onExit() {
  Activity::onExit();
  shelf.clear();

  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(30);
    LOG_INF("PULP", "free heap after sync: %u", ESP.getFreeHeap());
    // Upstream's OPDS browser reboots here to defragment the heap after a
    // Wi-Fi/HTTP session — a C3 (no PSRAM, ~380 KB) habit. On the X4 Pro (S3,
    // 8 MB PSRAM) it cost a visible ~10 s reboot after every manual sync that
    // read as a crash (2026-09-20). Only do it when the heap actually
    // looks fragmented: the largest free block below what a reader session
    // needs (~48 KB framebuffer-class allocation).
    if (!silent && ESP.getMaxAllocHeap() < 60000) {
      LOG_INF("PULP", "fragmented heap (max block %u) — restarting", ESP.getMaxAllocHeap());
      silentRestart();
    }
  }
}

bool PulpSyncActivity::startNextAttempt() {
  while (attemptIndex < attemptSsids.size()) {
    const auto cred = WIFI_STORE.findCredential(attemptSsids[attemptIndex++]);
    if (!cred) continue;

    LOG_INF("PULP", "Wi-Fi attempt: %s", cred->ssid.c_str());
    currentSsid = cred->ssid;
    headline = tr(STR_CONNECTING_SAVED_WIFI);
    detail = cred->ssid;

    if (!wifiStarted) {
      // Same bring-up as WifiSelectionActivity::attemptConnection().
      wifiStarted = true;
      WiFi.persistent(false);
      WiFi.mode(WIFI_STA);
      WiFi.disconnect(true, true);
      delay(100);
      WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
      WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
      uint8_t mac[6] = {};
      if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        char hostname[sizeof("CrossPoint-Reader-") + 12];
        snprintf(hostname, sizeof(hostname), "CrossPoint-Reader-%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2],
                 mac[3], mac[4], mac[5]);
        WiFi.setHostname(hostname);
      }
    } else {
      WiFi.disconnect();
    }

    if (cred->password.empty()) {
      WiFi.begin(cred->ssid.c_str());
    } else {
      WiFi.begin(cred->ssid.c_str(), cred->password.c_str());
    }
    attemptStartedMs = millis();
    return true;
  }
  return false;
}

void PulpSyncActivity::loop() {
  if (leaving) return;

  switch (state) {
    case State::CONNECTING: {
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        leave();
        return;
      }
      const wl_status_t status = WiFi.status();
      if (status == WL_CONNECTED) {
        WIFI_STORE.setLastConnectedSsid(currentSsid);
        state = State::SYNCING;
        headline = tr(STR_PULP_FETCHING_SHELF);
        detail.clear();
        requestUpdate();
        return;
      }
      const unsigned long timeout = silent ? SILENT_CONNECT_TIMEOUT_MS : MANUAL_CONNECT_TIMEOUT_MS;
      const bool failed = status == WL_CONNECT_FAILED || status == WL_NO_SSID_AVAIL;
      if (failed || millis() - attemptStartedMs >= timeout) {
        if (startNextAttempt()) {
          requestUpdate();
        } else {
          complete(tr(STR_WIFI_CONN_FAILED));
        }
      }
      return;
    }
    case State::SYNCING:
      if (!syncStarted) {
        syncStarted = true;
        // Paint the "fetching" screen before the blocking transfer starts.
        requestUpdateAndWait();
        runSync();
      }
      return;
    case State::DONE: {
      int x = 0;
      int y = 0;
      const bool dismissed = mappedInput.wasReleased(MappedInputManager::Button::Back) ||
                             mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
                             mappedInput.wasScreenTapped(x, y);
      if (silent || dismissed || millis() - doneAtMs >= RESULT_HOLD_MS) leave();
      return;
    }
  }
}

void PulpSyncActivity::syncClock() {
  // Downloads get their mtime from the RTC (HalFileTime). Upstream syncs the
  // RTC from NTP once per device; also re-sync when the RTC reads implausibly
  // (battery lost, never set) so files never land with a 2000-01-01 stamp.
  if (halClock.isAvailable()) {
    struct tm now = {};
    const bool plausible = halClock.localTime(now) && now.tm_year + 1900 >= 2024;
    if (SETTINGS.clockHasBeenSynced && plausible) return;
    headline = tr(STR_SYNCING_TIME);
    requestUpdate(true);
    if (halClock.syncFromNTP()) {
      SETTINGS.clockHasBeenSynced = 1;
      SETTINGS.saveToFile();
    }
    return;
  }
  // No RTC: set the system clock once per power cycle, asynchronously. The
  // timestamp callback picks it up as soon as SNTP lands, typically before the
  // first download finishes.
  static bool kicked = false;
  if (kicked) return;
  kicked = true;
  const char* tz = getenv("TZ");
  configTzTime(tz && tz[0] != '\0' ? tz : "UTC0", "pool.ntp.org", "time.nist.gov");
}

std::string PulpSyncActivity::destinationPath(const std::string& file) const {
  // Same sanitizer as the OPDS browser's "Server filename" format, so books
  // pulled either way collide on the same on-card name and are skipped here.
  const std::string leaf = opdsServerFilename(file);
  if (leaf.empty()) return "";
  std::string path;
  path.reserve(folderPrefix.size() + 1 + leaf.size());
  path += folderPrefix;
  path += '/';
  path += leaf;
  return path;
}

void PulpSyncActivity::runSync() {
  syncClock();

  headline = tr(STR_PULP_FETCHING_SHELF);
  detail.clear();
  requestUpdate(true);
  if (!pulp::fetchShelf(baseUrl, server.username, server.password, shelf)) {
    complete(tr(STR_PULP_SHELF_FAILED));
    return;
  }

  // Download folder handling mirrors OpdsBookBrowserActivity::downloadBook.
  folderPrefix = SETTINGS.opdsDownloadFolder;
  if (!folderPrefix.empty() && !Storage.exists(folderPrefix.c_str()) && !Storage.mkdir(folderPrefix.c_str())) {
    LOG_ERR("PULP", "mkdir failed for %s, using SD root", folderPrefix.c_str());
    folderPrefix.clear();
  }

  std::vector<size_t> todo;
  todo.reserve(shelf.size());
  for (size_t i = 0; i < shelf.size(); i++) {
    const std::string dest = destinationPath(shelf[i].file);
    if (!dest.empty() && !Storage.exists(dest.c_str())) todo.push_back(i);
  }
  itemCount = todo.size();
  LOG_INF("PULP", "shelf %u, missing %u", static_cast<unsigned>(shelf.size()), static_cast<unsigned>(todo.size()));
  if (todo.empty()) {
    complete(tr(STR_PULP_UP_TO_DATE));
    return;
  }

  // Shelf order is newest first, so an interrupted run still lands the latest
  // articles.
  headline = tr(STR_DOWNLOADING);
  for (size_t n = 0; n < todo.size() && !cancelRequested; n++) {
    const PulpShelfItem& item = shelf[todo[n]];
    itemIndex = n + 1;
    detail = item.file;
    bytesDone = 0;
    bytesTotal = item.size;
    requestUpdate(true);

    const std::string dest = destinationPath(item.file);
    LOG_DBG("PULP", "%s -> %s (heap %u)", item.url.c_str(), dest.c_str(), ESP.getFreeHeap());

    int lastPercent = -1;
    unsigned long lastUpdateMs = 0;
    const auto result = HttpDownloader::downloadToFile(
        item.url, dest,
        [this, &lastPercent, &lastUpdateMs](const size_t downloaded, const size_t total) {
          bytesDone = downloaded;
          bytesTotal = total;
          // The activity loop is blocked for the whole transfer; pump input so
          // Back can abort the run.
          mappedInput.update(true);
          if (mappedInput.wasReleased(MappedInputManager::Button::Back)) cancelRequested = true;
          const int percent = total > 0 ? static_cast<int>(static_cast<uint64_t>(downloaded) * 100 / total) : 0;
          const unsigned long now = millis();
          if (lastPercent < 0 || percent >= lastPercent + PROGRESS_STEP_PERCENT ||
              now - lastUpdateMs >= PROGRESS_MIN_UPDATE_MS) {
            lastPercent = percent;
            lastUpdateMs = now;
            requestUpdate(true);
          }
        },
        &cancelRequested, server.username, server.password);

    if (result == HttpDownloader::OK) {
      newCount++;
      clearBookCache(dest);
    } else if (result == HttpDownloader::ABORTED) {
      LOG_INF("PULP", "cancelled after %d files", newCount);
      break;
    } else {
      failCount++;
      LOG_ERR("PULP", "download failed (%d): %s", static_cast<int>(result), item.url.c_str());
    }
  }

  char msg[48];
  snprintf(msg, sizeof(msg), tr(STR_PULP_RESULT_FORMAT), newCount, failCount);
  complete(msg);
}

void PulpSyncActivity::complete(const char* message) {
  detail = message;
  state = State::DONE;
  doneAtMs = millis();
  // Re-stamp after the run: the first NTP sync of a power cycle moves the
  // clock forward, and the pre-run stamp would otherwise read as ancient.
  if (syncStarted) pulp::stampAutoSync();
  if (silent) {
    // Nothing happened worth a frame (no server, no Wi-Fi): straight to Home.
    if (!syncStarted) {
      leave();
      return;
    }
    requestUpdateAndWait();  // let the result line land before Home replaces it
    return;
  }
  requestUpdate();
}

void PulpSyncActivity::leave() {
  if (leaving) return;
  leaving = true;
  activityManager.goHome(HomeMenuItem::NONE, cleanHomeRefresh);
}

void PulpSyncActivity::render(RenderLock&&) {
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int percent = bytesTotal > 0 ? static_cast<int>(static_cast<uint64_t>(bytesDone) * 100 / bytesTotal) : 0;

  renderer.clearScreen();

  if (silent) {
    // Boot/wake: one small status line at the bottom, the rest stays blank.
    char line[192];
    if (state == State::SYNCING && itemCount > 0) {
      snprintf(line, sizeof(line), "%s: %u/%u  %s  %d%%", tr(STR_PULP), static_cast<unsigned>(itemIndex),
               static_cast<unsigned>(itemCount), detail.c_str(), percent);
    } else {
      snprintf(line, sizeof(line), "%s: %s%s%s", tr(STR_PULP), headline.c_str(), detail.empty() ? "" : "  ",
               detail.c_str());
    }
    const std::string fitted = renderer.truncatedText(SMALL_FONT_ID, line, pageWidth - 40);
    renderer.drawCenteredText(SMALL_FONT_ID, pageHeight - 30, fitted.c_str());
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    return;
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_PULP_SYNC));

  const int midY = pageHeight / 2;
  renderer.drawCenteredText(UI_12_FONT_ID, midY - 60, headline.c_str(), true, EpdFontFamily::BOLD);
  if (!detail.empty()) {
    const std::string fitted =
        renderer.truncatedText(UI_10_FONT_ID, detail.c_str(), pageWidth - 2 * metrics.contentSidePadding);
    renderer.drawCenteredText(UI_10_FONT_ID, midY - 25, fitted.c_str());
  }
  if (state == State::SYNCING && itemCount > 0) {
    char counter[32];
    snprintf(counter, sizeof(counter), tr(STR_PULP_PROGRESS_FORMAT), static_cast<int>(itemIndex),
             static_cast<int>(itemCount));
    renderer.drawCenteredText(UI_10_FONT_ID, midY + 10, counter);
    const int barX = 50;
    const int barW = pageWidth - 100;
    const int barY = midY + 30;
    constexpr int barH = 16;
    renderer.drawRect(barX, barY, barW, barH);
    if (bytesTotal > 0) renderer.fillRect(barX, barY, barW * percent / 100, barH);
  }

  const auto labels = mappedInput.mapLabels(state == State::DONE ? tr(STR_BACK) : tr(STR_CANCEL), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
