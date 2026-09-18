#pragma once
#include <string>
#include <vector>

#include "OpdsServerStore.h"
#include "activities/Activity.h"
#include "network/PulpShelf.h"

// Pulp fork. Joins a saved Wi-Fi network, fetches the Pulp shelf, downloads
// every EPUB missing from the OPDS download folder under the server's own
// filename, drops Wi-Fi and returns Home.
//
// `silent` is the boot/wake auto-sync: one bounded connect attempt, a single
// status line instead of the progress screen, no result screen to dismiss and
// no heap-defrag reboot on the way out.
class PulpSyncActivity final : public Activity {
 public:
  PulpSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool silent, bool cleanHomeRefresh = false);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool skipLoopDelay() override { return state == State::SYNCING; }
  bool preventAutoSleep() override { return state != State::DONE; }

 private:
  enum class State : uint8_t { CONNECTING, SYNCING, DONE };

  // Boot must never hang on an absent network; the manual run can afford the
  // full association time of a slow AP.
  static constexpr unsigned long SILENT_CONNECT_TIMEOUT_MS = 8000;
  static constexpr unsigned long MANUAL_CONNECT_TIMEOUT_MS = 12000;
  static constexpr unsigned long RESULT_HOLD_MS = 2500;
  static constexpr int PROGRESS_STEP_PERCENT = 5;
  static constexpr unsigned long PROGRESS_MIN_UPDATE_MS = 3000;

  const bool silent;
  const bool cleanHomeRefresh;
  State state = State::CONNECTING;
  bool syncStarted = false;
  bool wifiStarted = false;
  bool cancelRequested = false;
  bool leaving = false;

  std::string baseUrl;
  OpdsServer server;  // credentials of the "Pulp" OPDS entry, if any
  std::string folderPrefix;
  std::vector<PulpShelfItem> shelf;

  // Saved networks in attempt order (last connected first).
  std::vector<std::string> attemptSsids;
  size_t attemptIndex = 0;
  std::string currentSsid;
  unsigned long attemptStartedMs = 0;
  unsigned long doneAtMs = 0;

  // Screen state: what we're doing, and the current filename / final result.
  std::string headline;
  std::string detail;
  size_t itemIndex = 0;
  size_t itemCount = 0;
  size_t bytesDone = 0;
  size_t bytesTotal = 0;
  int newCount = 0;
  int failCount = 0;

  bool startNextAttempt();
  void runSync();
  void syncClock();
  void complete(const char* message);
  void leave();
  std::string destinationPath(const std::string& file) const;
};
