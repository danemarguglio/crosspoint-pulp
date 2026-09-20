#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Pulp fork: headless join of a saved Wi-Fi network, last-connected SSID
// first, then the other saved networks in store order. The same bring-up as
// PulpSyncActivity, packaged so other network screens can poll it from loop().
class SavedWifiJoiner {
 public:
  enum class State : uint8_t { IDLE, CONNECTING, CONNECTED, FAILED };

  // Loads the credential store and starts the first attempt. Returns false
  // (state FAILED) when no network is saved. An already-connected radio goes
  // straight to CONNECTED without touching it.
  bool begin(unsigned long attemptTimeoutMs);

  // Advance the state machine; call once per loop pass while CONNECTING.
  State poll();

  State state() const { return current; }
  const std::string& currentSsid() const { return ssid; }
  // True when this object brought the radio up (so the caller owns teardown).
  bool startedRadio() const { return wifiStarted; }

  // Disconnect and power the radio down if this object started it.
  void shutdown();

 private:
  State current = State::IDLE;
  bool wifiStarted = false;
  unsigned long timeoutMs = 0;
  unsigned long attemptStartedMs = 0;
  std::vector<std::string> attemptSsids;
  size_t attemptIndex = 0;
  std::string ssid;

  bool startNextAttempt();
};
