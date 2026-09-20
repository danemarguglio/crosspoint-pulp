#include "SavedWifiJoiner.h"

#include <Arduino.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_mac.h>

#include <cstdio>

#include "WifiCredentialStore.h"

bool SavedWifiJoiner::begin(const unsigned long attemptTimeoutMs) {
  timeoutMs = attemptTimeoutMs;
  attemptIndex = 0;
  attemptSsids.clear();

  if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
    current = State::CONNECTED;
    return true;
  }

  WIFI_STORE.loadFromFile();
  const size_t count = WIFI_STORE.getCredentialCount();
  attemptSsids.reserve(count);
  const std::string last = WIFI_STORE.getLastConnectedSsid();
  if (!last.empty() && WIFI_STORE.hasSavedCredential(last)) attemptSsids.push_back(last);
  for (size_t i = 0; i < count; i++) {
    const auto saved = WIFI_STORE.getSsidAt(i);
    if (saved && *saved != last) attemptSsids.push_back(*saved);
  }

  if (!startNextAttempt()) {
    current = State::FAILED;
    return false;
  }
  current = State::CONNECTING;
  return true;
}

bool SavedWifiJoiner::startNextAttempt() {
  while (attemptIndex < attemptSsids.size()) {
    const auto cred = WIFI_STORE.findCredential(attemptSsids[attemptIndex++]);
    if (!cred) continue;

    LOG_INF("WIFI", "saved-network attempt: %s", cred->ssid.c_str());
    ssid = cred->ssid;

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

SavedWifiJoiner::State SavedWifiJoiner::poll() {
  if (current != State::CONNECTING) return current;
  const wl_status_t status = WiFi.status();
  if (status == WL_CONNECTED) {
    WIFI_STORE.setLastConnectedSsid(ssid);
    current = State::CONNECTED;
    return current;
  }
  const bool failed = status == WL_CONNECT_FAILED || status == WL_NO_SSID_AVAIL;
  if (failed || millis() - attemptStartedMs >= timeoutMs) {
    if (!startNextAttempt()) current = State::FAILED;
  }
  return current;
}

void SavedWifiJoiner::shutdown() {
  if (!wifiStarted) return;
  wifiStarted = false;
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(30);
  current = State::IDLE;
}
