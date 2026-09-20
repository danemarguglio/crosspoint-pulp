#include "HnFeedActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>

#include <cstdio>

#include "HnStoryActivity.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HnClient.h"
#include "network/PulpConfig.h"

namespace fui = freeink::ui;

namespace {
constexpr int SIDE_PADDING = 20;

const char* kindLabel(const int index) {
  switch (static_cast<hn::FeedKind>(index)) {
    case hn::FeedKind::TOP:
      return tr(STR_HN_TOP);
    case hn::FeedKind::NEW:
      return tr(STR_HN_NEW);
    case hn::FeedKind::BEST:
      return tr(STR_HN_BEST);
    case hn::FeedKind::ASK:
      return tr(STR_HN_ASK);
    case hn::FeedKind::SHOW:
      return tr(STR_HN_SHOW);
    default:
      return "";
  }
}
}  // namespace

HnFeedActivity::HnFeedActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiTabListActivity("HnFeed", renderer, mappedInput) {}

void HnFeedActivity::onEnter() {
  UiTabListActivity::onEnter();
  hn::heapAllowsFetch("feed enter");

  baseUrl = pulp::baseUrl();
  server = pulp::server();
  if (baseUrl.empty()) {
    state = State::ERROR;
    statusMessage = tr(STR_PULP_NOT_CONFIGURED);
    return;
  }
  statusMessage = tr(STR_CONNECTING_SAVED_WIFI);
  if (!joiner.begin(CONNECT_TIMEOUT_MS)) {
    state = State::ERROR;
    statusMessage = tr(STR_CLOCK_SYNC_NO_WIFI);
    return;
  }
  state = State::CONNECTING;
  requestUpdate();
}

void HnFeedActivity::onExit() {
  UiTabListActivity::onExit();
  feed.items.clear();
  metaLines.clear();
  rowItems.clear();
  // Same exit as the OPDS browser: drop the radio, then reboot to defragment
  // the heap after a Wi-Fi/HTTP session.
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    LOG_INF("HN", "free heap on exit: %u", ESP.getFreeHeap());
    silentRestart();
  }
}

void HnFeedActivity::loop() {
  switch (state) {
    case State::CONNECTING: {
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        goHome();
        return;
      }
      const auto result = joiner.poll();
      if (result == SavedWifiJoiner::State::CONNECTED) {
        loadPage(pageOf[activeKind]);
      } else if (result == SavedWifiJoiner::State::FAILED) {
        state = State::ERROR;
        statusMessage = tr(STR_WIFI_CONN_FAILED);
        requestUpdate();
      }
      return;
    }
    case State::LOADING:
      return;
    case State::ERROR: {
      int x = 0;
      int y = 0;
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        goHome();
      } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(x, y)) {
        if (WiFi.status() == WL_CONNECTED) {
          loadPage(pageOf[activeKind]);
        } else if (joiner.begin(CONNECT_TIMEOUT_MS)) {
          state = State::CONNECTING;
          statusMessage = tr(STR_CONNECTING_SAVED_WIFI);
          requestUpdate();
        }
      }
      return;
    }
    case State::BROWSING:
      UiTabListActivity::loop();
      return;
  }
}

void HnFeedActivity::loadPage(const uint16_t page) {
  state = State::LOADING;
  statusMessage = tr(STR_LOADING);
  // The list's interaction table indexes rows that are about to change.
  closeRouting();
  requestUpdateAndWait();

  if (!hn::heapAllowsFetch("feed page")) {
    state = State::ERROR;
    statusMessage = tr(STR_HN_LOW_MEMORY);
    requestUpdate();
    return;
  }
  metaLines.clear();
  rowItems.clear();
  if (!hn::fetchFeed(baseUrl, server.username, server.password, static_cast<hn::FeedKind>(activeKind), page, feed)) {
    state = State::ERROR;
    statusMessage = tr(STR_HN_FEED_FAILED);
    requestUpdate();
    return;
  }
  feed.page = page;
  pageOf[activeKind] = page;
  rebuildRows();
  state = State::BROWSING;
  // Land on the first story of the new page (ring 1), viewport at the top.
  auto& nav = activeNav();
  nav.top = 0;
  nav.requestSelection(rowItems.empty() ? 0 : 1);
  requestUpdate();
}

void HnFeedActivity::rebuildRows() {
  const size_t n = feed.items.size();
  metaLines.clear();
  metaLines.reserve(n);
  rowItems.clear();
  rowItems.reserve(n + 2);

  if (hasPrevRow()) {
    fui::ListItem prev;
    prev.label = tr(STR_PREV_PAGE);
    prev.actionValue = static_cast<int16_t>(rowItems.size());
    rowItems.push_back(prev);
  }
  for (size_t i = 0; i < n; i++) {
    const auto& item = feed.items[i];
    char meta[128];
    if (item.domain.empty()) {
      snprintf(meta, sizeof(meta), tr(STR_HN_META_SELF_FORMAT), static_cast<int>(item.points),
               static_cast<int>(item.comments), item.age.c_str());
    } else {
      snprintf(meta, sizeof(meta), tr(STR_HN_META_FORMAT), static_cast<int>(item.points),
               static_cast<int>(item.comments), item.domain.c_str(), item.age.c_str());
    }
    metaLines.emplace_back(meta);
  }
  for (size_t i = 0; i < n; i++) {
    fui::ListItem row;
    row.label = feed.items[i].title.c_str();
    row.subtitle = metaLines[i].c_str();
    row.actionValue = static_cast<int16_t>(rowItems.size());
    rowItems.push_back(row);
  }
  if (hasMoreRow()) {
    fui::ListItem more;
    more.label = tr(STR_HN_MORE);
    more.value = ">";
    more.actionValue = static_cast<int16_t>(rowItems.size());
    rowItems.push_back(more);
  }
}

int HnFeedActivity::listCount() const { return static_cast<int>(rowItems.size()); }

int HnFeedActivity::itemForRow(const int row) const {
  const int first = hasPrevRow() ? 1 : 0;
  const int idx = row - first;
  if (idx < 0 || idx >= static_cast<int>(feed.items.size())) return -1;
  return idx;
}

const char* HnFeedActivity::tabLabel(const int index) const { return kindLabel(index); }

const char* HnFeedActivity::headerTitle() const { return tr(STR_HN); }

void HnFeedActivity::onTabAction(const int index) { selectTab(index); }

void HnFeedActivity::stepTab(const int direction) {
  const int count = tabCount();
  selectTab((activeKind + (direction > 0 ? 1 : count - 1)) % count);
}

void HnFeedActivity::selectTab(const int index) {
  if (index < 0 || index >= tabCount()) return;
  if (index == activeKind && state == State::BROWSING) {
    requestUpdate();
    return;
  }
  activeKind = index;
  // Keep the tab band focused across the reload so a held button keeps
  // stepping tabs instead of dropping into the rows.
  activeNav().selected = 0;
  activeNav().top = 0;
  if (WiFi.status() == WL_CONNECTED) {
    loadPage(pageOf[activeKind]);
    activeNav().requestSelection(0);
    requestUpdate();
  } else {
    requestUpdate();
  }
}

void HnFeedActivity::activateIndex(const int index) {
  if (state != State::BROWSING || index < 0 || index >= listCount()) return;
  if (hasPrevRow() && index == 0) {
    app.clearTapFlash();
    loadPage(feed.page - 1);
    return;
  }
  if (hasMoreRow() && index == listCount() - 1) {
    app.clearTapFlash();
    loadPage(feed.page + 1);
    return;
  }
  const int item = itemForRow(index);
  if (item < 0) return;
  app.clearTapFlash();
  openStory(item);
}

void HnFeedActivity::openStory(const int itemIndex) {
  const auto& item = feed.items[static_cast<size_t>(itemIndex)];
  auto story = makeUniqueNoThrow<HnStoryActivity>(renderer, mappedInput, baseUrl, server.username, server.password,
                                                  item.id, item.title, metaLines[static_cast<size_t>(itemIndex)]);
  if (!story) {
    LOG_ERR("HN", "OOM: story activity");
    return;
  }
  // The feed stays on the activity stack with Wi-Fi up; the story pops back
  // here and the base re-renders.
  startActivityForResult(std::move(story), [this](const ActivityResult&) { requestUpdate(); });
}

bool HnFeedActivity::handleButtons() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    goHome();
    return true;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (tabsFocused()) {
      stepTab(1);
    } else {
      activateIndex(ringPos() - 1);
    }
    return true;
  }
  return false;
}

void HnFeedActivity::navigateButtons() {
  const int count = listCount();
  auto& nav = activeNav();
  buttonNavigator.onNextRelease([this, count] {
    const int ring = ringPos();
    if (count > 0 && ring >= count) {
      // Off the end of the page: fetch the next one, else wrap to the tabs.
      if (feed.hasMore) {
        loadPage(feed.page + 1);
      } else {
        moveRingTo(0);
      }
      return;
    }
    moveRingTo(ring + 1);
  });
  buttonNavigator.onPreviousRelease([this] {
    const int ring = ringPos();
    if (ring <= 1) {
      // Off the top of the page: back a page when there is one, else the tabs.
      if (ring == 1 && feed.page > 1) {
        loadPage(feed.page - 1);
      } else {
        moveRingTo(0);
      }
      return;
    }
    moveRingTo(ring - 1);
  });
  buttonNavigator.onNextContinuous([this, count, &nav] {
    if (tabsFocused()) {
      stepTab(1);
    } else if (count > 0) {
      moveRingTo(ButtonNavigator::nextPageIndex(ringPos() - 1, count, nav.pageRows()) + 1);
    }
  });
  buttonNavigator.onPreviousContinuous([this, count, &nav] {
    if (tabsFocused()) {
      stepTab(-1);
    } else if (count > 0) {
      moveRingTo(ButtonNavigator::previousPageIndex(ringPos() - 1, count, nav.pageRows()) + 1);
    }
  });
}

void HnFeedActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int16_t readoutReserved = static_cast<int16_t>(renderer.getLineHeight(SMALL_FONT_ID) + metrics.verticalSpacing);
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight + readoutReserved), 0});
  buildTabBar(screen);

  if (state != State::BROWSING) {
    fui::TextStyle centered = screen.theme().bodyText;
    centered.align = fui::TextAlign::Center;
    screen.centeredText(statusMessage ? statusMessage : "", centered);
    return;
  }
  if (rowItems.empty()) {
    screen.centeredText(tr(STR_NO_ENTRIES));
    return;
  }

  fui::ListProps props;
  props.items = rowItems.data();
  props.count = static_cast<uint16_t>(rowItems.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.labelText = screen.theme().bodyText;
  props.labelText.maxLines = 2;  // titles wrap to two lines, then ellipsize
  props.subtitleText = screen.theme().smallText;
  props.valueInset = 8;
  syncTabListViewport(screen, props);
  screen.list(props);
}

void HnFeedActivity::drawFooter() {
  if (state == State::BROWSING) {
    char buf[32];
    snprintf(buf, sizeof(buf), tr(STR_HN_PAGE_FORMAT), static_cast<int>(feed.page));
    const auto& metrics = UITheme::getInstance().getMetrics();
    const int width = renderer.getTextWidth(SMALL_FONT_ID, buf);
    const int x = renderer.getScreenWidth() - width - SIDE_PADDING;
    const int y = renderer.getScreenHeight() - metrics.buttonHintsHeight - renderer.getLineHeight(SMALL_FONT_ID);
    renderer.drawText(SMALL_FONT_ID, x, y, buf, true);
  }
  const char* confirmLabel = state == State::ERROR ? tr(STR_RETRY) : tabsFocused() ? tr(STR_TOGGLE) : tr(STR_OPEN);
  const auto labels = mappedInput.mapLabels(tr(STR_HOME), confirmLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void HnFeedActivity::goHome() { onGoHome(); }
