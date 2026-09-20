#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "OpdsServerStore.h"
#include "activities/UiTabListActivity.h"
#include "network/HnJson.h"
#include "network/SavedWifiJoiner.h"

// Pulp fork: Hacker News front page through pulp's /hn proxy. Tabs Top · New ·
// Best · Ask · Show on the UiTabListActivity ring (0 = tab band, 1..N = rows),
// 30 stories a page with "« Previous page" / "More…" rows at the ends, and the
// side buttons paging when the selection runs off either end. Joins a saved
// Wi-Fi network headlessly on entry and keeps it up while the story screen is
// pushed on top; leaving tears it down with the OPDS browser's heap-defrag
// reboot.
class HnFeedActivity final : public UiTabListActivity {
 public:
  HnFeedActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  bool preventAutoSleep() override { return state == State::CONNECTING || state == State::LOADING; }

 protected:
  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  int tabCount() const override { return static_cast<int>(hn::FeedKind::COUNT); }
  int activeTab() const override { return activeKind; }
  const char* tabLabel(int index) const override;
  void onTabAction(int index) override;
  void stepTab(int direction) override;
  bool handleButtons() override;
  void navigateButtons() override;
  const char* headerTitle() const override;
  void drawFooter() override;

 private:
  enum class State : uint8_t { CONNECTING, LOADING, BROWSING, ERROR };

  static constexpr unsigned long CONNECT_TIMEOUT_MS = 12000;

  State state = State::CONNECTING;
  SavedWifiJoiner joiner;
  std::string baseUrl;
  OpdsServer server;  // credentials of the "Pulp" OPDS entry, if any
  int activeKind = 0;
  uint16_t pageOf[static_cast<int>(hn::FeedKind::COUNT)] = {1, 1, 1, 1, 1};

  // One feed page in RAM; rows and their meta lines are rebuilt from it.
  hn::Feed feed;
  std::vector<std::string> metaLines;
  std::vector<freeink::ui::ListItem> rowItems;
  const char* statusMessage = nullptr;

  bool hasPrevRow() const { return state == State::BROWSING && feed.page > 1; }
  bool hasMoreRow() const { return state == State::BROWSING && feed.hasMore; }
  // Feed item behind a list row, or -1 for the paging rows.
  int itemForRow(int row) const;
  bool tabsFocused() const { return ringPos() == 0; }

  void loadPage(uint16_t page);
  void rebuildRows();
  void selectTab(int index);
  void openStory(int itemIndex);
  void goHome();
};
