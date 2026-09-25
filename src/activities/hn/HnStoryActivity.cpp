#include "HnStoryActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HnClient.h"

namespace {
constexpr unsigned long PAGE_HOLD_MS = 500;
constexpr unsigned long CHROME_HOLD_MS = 700;

// Same point size as the reader's built-in font, in the sans family, for
// comment bodies; the article keeps the reader's own family.
int sansAt(const int readerFontId) {
  switch (readerFontId) {
    case NOTOSERIF_12_FONT_ID:
    case NOTOSANS_12_FONT_ID:
      return NOTOSANS_12_FONT_ID;
    case NOTOSERIF_16_FONT_ID:
    case NOTOSANS_16_FONT_ID:
      return NOTOSANS_16_FONT_ID;
    case NOTOSERIF_18_FONT_ID:
    case NOTOSANS_18_FONT_ID:
      return NOTOSANS_18_FONT_ID;
    default:
      return NOTOSANS_14_FONT_ID;
  }
}

const char* toolLabel(const int tool) {
  switch (tool) {
    case HnStoryActivity::TOOL_SAVE:
      return tr(STR_HN_SAVE);
    case HnStoryActivity::TOOL_NEXT_TOP:
      return tr(STR_HN_NEXT_THREAD);
    case HnStoryActivity::TOOL_PREV_PAGE:
      return "<";
    case HnStoryActivity::TOOL_NEXT_PAGE:
      return ">";
    case HnStoryActivity::TOOL_ARTICLE:
      return tr(STR_HN_ARTICLE);
    case HnStoryActivity::TOOL_COMMENTS:
      return tr(STR_HN_COMMENTS);
    default:
      return "";
  }
}
}  // namespace

HnStoryActivity::HnStoryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string baseUrl,
                                 std::string username, std::string password, const uint32_t storyId, std::string title,
                                 std::string meta)
    : Activity("HnStory", renderer, mappedInput),
      baseUrl(std::move(baseUrl)),
      username(std::move(username)),
      password(std::move(password)),
      storyId(storyId),
      title(std::move(title)),
      meta(std::move(meta)) {}

void HnStoryActivity::onEnter() {
  Activity::onEnter();
  hn::heapAllowsFetch("story enter");
  computeGeometry();
  setLoading();
}

void HnStoryActivity::onExit() {
  Activity::onExit();
  // Wi-Fi belongs to the feed underneath; only the page data goes.
  entries.clear();
  articleLines.clear();
  item = hn::Item{};
  LOG_INF("HN", "story exit: %u free", ESP.getFreeHeap());
}

void HnStoryActivity::setLoading() {
  state = State::LOADING;
  loadRequested = false;
  requestUpdate();
}

void HnStoryActivity::fail(const char* message) {
  state = State::ERROR;
  errorMessage = message;
  requestUpdate();
}

// ---------------------------------------------------------------------------
// Geometry and fonts

void HnStoryActivity::computeGeometry() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  bodyLeft = metrics.contentSidePadding;
  bodyWidth = screenW - 2 * metrics.contentSidePadding;

  // Built-in fonts only: an SD-card reader font faults glyphs in per word
  // during layout, which is fine for a cached book but not for text laid out
  // fresh on every page turn.
  const auto& fonts = renderer.getFontMap();
  const auto present = [&fonts, this](const int id) { return fonts.count(id) > 0 && !renderer.isSdCardFont(id); };
  articleFontId = SETTINGS.getReaderFontId();
  if (!present(articleFontId)) articleFontId = present(NOTOSANS_14_FONT_ID) ? NOTOSANS_14_FONT_ID : UI_12_FONT_ID;
  commentFontId = sansAt(articleFontId);
  if (!present(commentFontId)) commentFontId = UI_12_FONT_ID;
  articleLineHeight = std::max(1, renderer.getLineHeight(articleFontId, SETTINGS.getReaderLineCompression()));
  commentLineHeight = std::max(1, renderer.getLineHeight(commentFontId));
  metaLineHeight = std::max(1, renderer.getLineHeight(SMALL_FONT_ID));

  titleLines = renderer.wrappedText(UI_12_FONT_ID, title.c_str(), bodyWidth, 2, EpdFontFamily::BOLD);
  const int titleLineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  headerBottom = metrics.topPadding + static_cast<int>(titleLines.size()) * titleLineHeight + metaLineHeight +
                 metrics.verticalSpacing / 2;
  chromeBodyTop = headerBottom + TOOLBAR_HEIGHT + metrics.verticalSpacing;
  bareBodyTop = metrics.topPadding + metrics.verticalSpacing;
  bodyTop = chromeVisible ? chromeBodyTop : bareBodyTop;
  bodyBottom = screenH - metrics.buttonHintsHeight - metaLineHeight - metrics.verticalSpacing / 2;
}

void HnStoryActivity::toolRect(const int tool, int& x, int& y, int& w, int& h) const {
  const int slot = (bodyWidth - (TOOL_COUNT - 1) * TOOLBAR_GAP) / TOOL_COUNT;
  x = bodyLeft + tool * (slot + TOOLBAR_GAP);
  y = headerBottom;
  w = slot;
  h = TOOLBAR_HEIGHT;
}

// ---------------------------------------------------------------------------
// Data

bool HnStoryActivity::loadStory() {
  if (!hn::heapAllowsFetch("story")) {
    fail(tr(STR_HN_LOW_MEMORY));
    return false;
  }
  if (!hn::fetchItem(baseUrl, username, password, storyId, 1, true, item)) {
    fail(tr(STR_HN_ITEM_FAILED));
    return false;
  }
  loadedApiPage = 1;
  prepareArticle();
  articlePages.clear();
  commentPages.clear();
  articleAtEnd = false;
  commentsAtEnd = false;
  view = articleAvailable ? View::ARTICLE : View::COMMENTS;
  focus = TOOL_COUNT;
  {
    RenderLock lock(*this);
    if (view == View::ARTICLE) {
      articlePages.push_back(ArticleCursor{});
      layoutArticlePage(ArticleCursor{});
    } else {
      commentPages.push_back(CommentCursor{});
      layoutCommentPage(CommentCursor{});
    }
  }
  state = State::READY;
  return true;
}

void HnStoryActivity::prepareArticle() {
  // The article rides only on API page 1; keep it apart from the comment pages
  // that later fetches overwrite. Ask HN self text stands in when there is no
  // linked article.
  article = std::move(item.article);
  item.article = hn::Article{};
  if (!article.ok || article.paragraphs.empty()) {
    if (!item.text.empty()) {
      std::vector<std::string_view> parts;
      hn::splitParagraphs(item.text, parts);
      article.paragraphs.clear();
      article.paragraphs.reserve(parts.size());
      for (const auto& p : parts) article.paragraphs.emplace_back(p);
      article.ok = !article.paragraphs.empty();
    } else {
      article.ok = false;
    }
  }
  item.text.clear();
  item.text.shrink_to_fit();
  articleAvailable = article.ok && !article.paragraphs.empty();
}

bool HnStoryActivity::ensureApiPage(const uint16_t apiPage) {
  if (loadedApiPage == apiPage) return true;
  if (!hn::heapAllowsFetch("comments page")) {
    showToast(tr(STR_HN_LOW_MEMORY));
    return false;
  }
  if (!hn::fetchItem(baseUrl, username, password, storyId, apiPage, false, item)) {
    loadedApiPage = 0;
    showToast(tr(STR_HN_ITEM_FAILED));
    return false;
  }
  loadedApiPage = apiPage;
  return true;
}

const hn::Comment* HnStoryActivity::commentAt(uint32_t& flat) {
  for (int hop = 0; hop < 4; hop++) {
    const uint16_t apiPage = static_cast<uint16_t>(flat / hn::COMMENTS_PER_PAGE + 1);
    const size_t idx = flat % hn::COMMENTS_PER_PAGE;
    if (!ensureApiPage(apiPage)) return nullptr;
    if (idx < item.comments.size()) return &item.comments[idx];
    if (!item.hasMore) return nullptr;
    // Short page with more behind it: continue at the next API page's start.
    flat = static_cast<uint32_t>(apiPage) * hn::COMMENTS_PER_PAGE;
  }
  return nullptr;
}

bool HnStoryActivity::isCollapsed(const uint32_t id) const {
  return std::find(collapsedIds.begin(), collapsedIds.end(), id) != collapsedIds.end();
}

void HnStoryActivity::toggleCollapsed(const uint32_t id) {
  const auto it = std::find(collapsedIds.begin(), collapsedIds.end(), id);
  if (it != collapsedIds.end()) {
    collapsedIds.erase(it);
    return;
  }
  if (collapsedIds.size() >= MAX_COLLAPSED) collapsedIds.erase(collapsedIds.begin());
  collapsedIds.push_back(id);
}

// ---------------------------------------------------------------------------
// Layout

void HnStoryActivity::layoutArticlePage(const ArticleCursor start) {
  articleLines.clear();
  articleLineY.clear();
  articleAtEnd = false;
  // Ragged right regardless of the reader's justify setting: a 440 px column
  // at 14–18 pt justifies into rivers. Hyphenation closes the worst gaps.
  constexpr bool justify = false;
  const int paragraphGap = articleLineHeight / 2;

  int y = bodyTop;
  uint16_t p = start.paragraph;
  uint16_t skip = start.line;
  while (p < article.paragraphs.size()) {
    hn::Lines lines;
    hn::layoutParagraph(renderer, articleFontId, bodyWidth, article.paragraphs[p], justify, lines,
                        /*hyphenate=*/true);
    size_t i = skip;
    skip = 0;
    if (i >= lines.size()) {
      p++;
      continue;
    }
    if (!articleLines.empty() && i == 0) {
      if (y + paragraphGap + articleLineHeight > bodyBottom) {
        articleNext = ArticleCursor{p, 0};
        return;
      }
      y += paragraphGap;
    }
    for (; i < lines.size(); i++) {
      if (y + articleLineHeight > bodyBottom) {
        articleNext = ArticleCursor{p, static_cast<uint16_t>(i)};
        return;
      }
      articleLineY.push_back(static_cast<int16_t>(y));
      articleLines.push_back(std::move(lines[i]));
      y += articleLineHeight;
    }
    p++;
  }
  articleAtEnd = true;
  articleNext = ArticleCursor{p, 0};
}

void HnStoryActivity::layoutCommentPage(const CommentCursor start) {
  entries.clear();
  entries.reserve(12);
  commentsAtEnd = false;
  commentNext = start;

  int y = bodyTop;
  uint32_t flat = start.flat;
  uint16_t lineSkip = start.line;
  int skipDepth = -1;
  std::vector<std::string_view> paragraphs;

  while (true) {
    const hn::Comment* c = commentAt(flat);
    if (!c) {
      commentsAtEnd = true;
      commentNext = CommentCursor{flat, 0};
      break;
    }
    if (skipDepth >= 0 && c->depth > skipDepth) {
      flat++;
      continue;
    }
    skipDepth = -1;

    Entry e;
    e.flat = flat;
    e.id = c->id;
    e.depth = c->depth;
    e.collapsed = isCollapsed(c->id);
    const int visualDepth = std::min<int>(c->depth, MAX_VISUAL_DEPTH);
    const int indent = visualDepth * INDENT_PX;
    const int width = bodyWidth - indent;

    // The header alone must fit, plus one body line unless collapsed; otherwise
    // this comment opens the next page.
    const int minNeeded = metaLineHeight + (e.collapsed ? 0 : commentLineHeight);
    if (!entries.empty() && y + minNeeded > bodyBottom) {
      commentNext = CommentCursor{flat, 0};
      break;
    }

    if (e.collapsed) {
      bool openEnded = false;
      const size_t idx = flat % hn::COMMENTS_PER_PAGE;
      const size_t hidden = hn::subtreeSize(item.comments, idx, openEnded);
      e.hidden = static_cast<uint16_t>(std::min<size_t>(hidden, 0xFFFF));
      char marker[24];
      snprintf(marker, sizeof(marker), tr(STR_HN_HIDDEN_FORMAT), static_cast<int>(e.hidden));
      e.header = marker;
      if (openEnded) e.header += '+';
      e.header += ' ';
    }
    if (lineSkip > 0) {
      e.continued = true;
      e.header += "\xE2\x80\xA6 ";  // …
    }
    e.header += c->by.empty() ? "?" : c->by;
    e.header += " \xC2\xB7 ";  // ·
    e.header += c->age;
    if (c->dead) {
      e.header += ' ';
      e.header += tr(STR_HN_DEAD);
    }
    if (c->depth > MAX_VISUAL_DEPTH) e.header += " \xC2\xBB";  // »

    hn::Lines lines;
    if (!e.collapsed) {
      paragraphs.clear();
      hn::splitParagraphs(c->text, paragraphs);
      for (size_t pi = 0; pi < paragraphs.size(); pi++) {
        if (pi > 0) lines.push_back(nullptr);  // paragraph gap
        hn::layoutParagraph(renderer, commentFontId, width, paragraphs[pi], false, lines);
      }
    }

    size_t taken = std::min<size_t>(lineSkip, lines.size());
    lineSkip = 0;
    int yy = y + metaLineHeight;
    while (taken < lines.size()) {
      const int lh = lines[taken] ? commentLineHeight : commentLineHeight / 2;
      if (yy + lh > bodyBottom) break;
      e.lines.push_back(std::move(lines[taken]));
      yy += lh;
      taken++;
    }
    e.cut = taken < lines.size();
    e.y = y;
    e.height = yy - y;
    y = yy + ENTRY_GAP_PX;
    const bool cut = e.cut;
    const bool collapsed = e.collapsed;
    entries.push_back(std::move(e));

    if (cut) {
      commentNext = CommentCursor{flat, static_cast<uint16_t>(taken)};
      break;
    }
    if (collapsed) skipDepth = c->depth;
    flat++;
    if (y + metaLineHeight + commentLineHeight > bodyBottom) {
      commentNext = CommentCursor{flat, 0};
      break;
    }
  }

  const int bodyCount = static_cast<int>(entries.size());
  if (focus >= TOOL_COUNT + bodyCount) focus = bodyCount > 0 ? TOOL_COUNT + bodyCount - 1 : TOOL_COUNT;
}

int HnStoryActivity::bodyItemCount() const {
  if (view == View::ARTICLE) return 1;
  return std::max<int>(1, static_cast<int>(entries.size()));
}

// ---------------------------------------------------------------------------
// Actions

void HnStoryActivity::showToast(const char* text) {
  toastText = text;
  toastUntilMs = millis() + TOAST_MS;
  requestUpdate();
}

void HnStoryActivity::switchView(const View next) {
  view = next;
  focus = TOOL_COUNT;
  {
    RenderLock lock(*this);
    if (view == View::ARTICLE) {
      if (articlePages.empty()) {
        articlePages.push_back(ArticleCursor{});
        layoutArticlePage(ArticleCursor{});
      } else if (articleLines.empty() && !articleAtEnd) {
        layoutArticlePage(articlePages.back());
      }
    } else if (commentPages.empty()) {
      commentPages.push_back(CommentCursor{});
      layoutCommentPage(CommentCursor{});
    }
  }
  fullRefreshPending = true;
  requestUpdate();
}

void HnStoryActivity::hideChromeForPage() {
  if (!chromeVisible) return;
  chromeVisible = false;
  bodyTop = bareBodyTop;
  fullRefreshPending = true;
}

void HnStoryActivity::setChrome(const bool visible) {
  if (visible == chromeVisible) return;
  chromeVisible = visible;
  bodyTop = visible ? chromeBodyTop : bareBodyTop;
  if (!visible && focus < TOOL_COUNT) focus = TOOL_COUNT;
  if (state == State::READY) {
    // Width is unchanged, so the remembered page start is the same first word;
    // only how much fits below it changes.
    RenderLock lock(*this);
    if (view == View::ARTICLE) {
      if (!articlePages.empty()) layoutArticlePage(articlePages.back());
    } else if (!commentPages.empty()) {
      layoutCommentPage(commentPages.back());
    }
  }
  fullRefreshPending = true;
  requestUpdate();
}

void HnStoryActivity::nextPage() {
  if (view == View::ARTICLE) {
    if (articleAtEnd) {
      showToast(tr(STR_HN_END_OF_ARTICLE));
      return;
    }
    hideChromeForPage();
    RenderLock lock(*this);
    const ArticleCursor next = articleNext;
    layoutArticlePage(next);
    if (articleLines.empty()) {
      articleAtEnd = true;
      layoutArticlePage(articlePages.back());
      showToast(tr(STR_HN_END_OF_ARTICLE));
      return;
    }
    articlePages.push_back(next);
  } else {
    if (commentsAtEnd) {
      showToast(entries.empty() && commentPages.size() <= 1 ? tr(STR_HN_NO_COMMENTS) : tr(STR_HN_END_OF_COMMENTS));
      return;
    }
    const CommentCursor next = commentNext;
    const bool needsFetch = static_cast<uint16_t>(next.flat / hn::COMMENTS_PER_PAGE + 1) != loadedApiPage;
    if (needsFetch) {
      busy = true;
      requestUpdateAndWait();
    }
    hideChromeForPage();
    RenderLock lock(*this);
    layoutCommentPage(next);
    busy = false;
    if (entries.empty()) {
      commentsAtEnd = true;
      layoutCommentPage(commentPages.back());
      showToast(tr(STR_HN_END_OF_COMMENTS));
      return;
    }
    commentPages.push_back(next);
  }
  focus = TOOL_COUNT;
  if (++pageTurns % PAGES_PER_FULL_REFRESH == 0) fullRefreshPending = true;
  requestUpdate();
}

void HnStoryActivity::prevPage() {
  if (view == View::ARTICLE) {
    if (articlePages.size() <= 1) return;
    hideChromeForPage();
    RenderLock lock(*this);
    articlePages.pop_back();
    layoutArticlePage(articlePages.back());
  } else {
    if (commentPages.size() <= 1) return;
    const CommentCursor prev = commentPages[commentPages.size() - 2];
    const bool needsFetch = static_cast<uint16_t>(prev.flat / hn::COMMENTS_PER_PAGE + 1) != loadedApiPage;
    if (needsFetch) {
      busy = true;
      requestUpdateAndWait();
    }
    hideChromeForPage();
    RenderLock lock(*this);
    commentPages.pop_back();
    layoutCommentPage(prev);
    busy = false;
  }
  focus = TOOL_COUNT;
  if (++pageTurns % PAGES_PER_FULL_REFRESH == 0) fullRefreshPending = true;
  requestUpdate();
}

void HnStoryActivity::nextTopLevel() {
  if (view != View::COMMENTS) {
    switchView(View::COMMENTS);
    return;
  }
  uint32_t flat = commentPages.empty() ? 0 : commentPages.back().flat;
  if (focus >= TOOL_COUNT && !entries.empty()) flat = entries[static_cast<size_t>(focus - TOOL_COUNT)].flat;
  flat++;

  busy = true;
  requestUpdateAndWait();
  RenderLock lock(*this);
  bool found = false;
  // Bounded walk: a thread this deep would be thousands of comments anyway.
  for (int guard = 0; guard < 2000; guard++) {
    const hn::Comment* c = commentAt(flat);
    if (!c) break;
    if (c->depth == 0) {
      found = true;
      break;
    }
    flat++;
  }
  if (!found) {
    // The walk may have paged the API forward; put the current page back.
    layoutCommentPage(commentPages.back());
    busy = false;
    showToast(tr(STR_HN_END_OF_COMMENTS));
    return;
  }
  const CommentCursor next{flat, 0};
  hideChromeForPage();
  layoutCommentPage(next);
  busy = false;
  if (!entries.empty()) commentPages.push_back(next);
  focus = TOOL_COUNT;
  if (++pageTurns % PAGES_PER_FULL_REFRESH == 0) fullRefreshPending = true;
  requestUpdate();
}

void HnStoryActivity::save() {
  if (!hn::heapAllowsFetch("save")) {
    showToast(tr(STR_HN_LOW_MEMORY));
    return;
  }
  busy = true;
  requestUpdateAndWait();
  const bool ok = hn::saveItem(baseUrl, username, password, storyId);
  busy = false;
  showToast(ok ? tr(STR_HN_SAVED) : tr(STR_HN_SAVE_FAILED));
}

void HnStoryActivity::activateFocus() {
  if (focus < TOOL_COUNT) {
    switch (focus) {
      case TOOL_ARTICLE:
        switchView(View::ARTICLE);
        break;
      case TOOL_COMMENTS:
        switchView(View::COMMENTS);
        break;
      case TOOL_SAVE:
        save();
        break;
      case TOOL_NEXT_TOP:
        nextTopLevel();
        break;
      case TOOL_PREV_PAGE:
        prevPage();
        break;
      case TOOL_NEXT_PAGE:
        nextPage();
        break;
      default:
        break;
    }
    return;
  }
  if (view == View::ARTICLE) {
    nextPage();
    return;
  }
  const size_t i = static_cast<size_t>(focus - TOOL_COUNT);
  if (i >= entries.size()) return;
  const uint32_t id = entries[i].id;
  RenderLock lock(*this);
  toggleCollapsed(id);
  // Re-lay the same page: the toggled subtree changes what fits below it.
  layoutCommentPage(commentPages.back());
  requestUpdate();
}

void HnStoryActivity::moveFocus(const int delta) {
  const int last = TOOL_COUNT + bodyItemCount() - 1;
  if (delta > 0) {
    if (focus >= last) {
      nextPage();
      return;
    }
    focus++;
  } else {
    if (focus == TOOL_COUNT && !chromeVisible) {
      // Chrome hidden: the ring's toolbar half is off-screen; bring it back
      // and land on its last button instead of walking into nothing.
      setChrome(true);
      focus = TOOL_COUNT - 1;
      requestUpdate();
      return;
    }
    if (focus <= 0) return;
    focus--;
  }
  requestUpdate();
}

bool HnStoryActivity::handleTap(const int x, const int y) {
  for (int tool = 0; chromeVisible && tool < TOOL_COUNT; tool++) {
    int tx = 0;
    int ty = 0;
    int tw = 0;
    int th = 0;
    toolRect(tool, tx, ty, tw, th);
    if (x >= tx && x < tx + tw && y >= ty - 4 && y < ty + th + 4) {
      focus = tool;
      activateFocus();
      return true;
    }
  }
  if (y < bodyTop || y >= bodyBottom) return false;
  if (view == View::ARTICLE) {
    x < renderer.getScreenWidth() / 2 ? prevPage() : nextPage();
    return true;
  }
  for (size_t i = 0; i < entries.size(); i++) {
    const Entry& e = entries[i];
    if (y >= e.y && y < e.y + e.height + ENTRY_GAP_PX) {
      focus = TOOL_COUNT + static_cast<int>(i);
      activateFocus();
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// Loop

void HnStoryActivity::loop() {
  if (state == State::LOADING) {
    if (!loadRequested) {
      loadRequested = true;
      requestUpdateAndWait();  // paint "Loading" before the blocking fetch
      if (loadStory()) requestUpdate();
    }
    return;
  }

  if (toastText && static_cast<long>(millis() - toastUntilMs) >= 0) {
    toastText = nullptr;
    requestUpdate();
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (state == State::ERROR) {
    int x = 0;
    int y = 0;
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(x, y)) {
      setLoading();
    }
    return;
  }

  // Button-only chrome toggle (boards with a front Confirm key; on the X4 Pro
  // the power click has no hold, so Up from the first body item does it).
  if (mappedInput.wasLongPressed(MappedInputManager::Button::Confirm, CHROME_HOLD_MS)) {
    setChrome(!chromeVisible);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateFocus();
    return;
  }
  // Held side key turns the page; a tap steps the focus ring.
  if (mappedInput.wasLongPressed(MappedInputManager::Button::NavNext, PAGE_HOLD_MS)) {
    nextPage();
    return;
  }
  if (mappedInput.wasLongPressed(MappedInputManager::Button::NavPrevious, PAGE_HOLD_MS)) {
    prevPage();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::NavNext)) {
    moveFocus(1);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::NavPrevious)) {
    moveFocus(-1);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    nextPage();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    prevPage();
    return;
  }

  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Left) {
    nextPage();
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Right) {
    prevPage();
    return;
  }
  // Upward swipe (the bottom-edge one included) brings the chrome back; a
  // downward swipe hides it. The top-edge downward swipe never reaches here —
  // ActivityManager opens the control center on it first.
  if (swipe == MappedInputManager::SwipeDir::Up) {
    setChrome(true);
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Down) {
    setChrome(false);
    return;
  }
  int x = 0;
  int y = 0;
  if (mappedInput.wasScreenTapped(x, y)) handleTap(x, y);
}

// ---------------------------------------------------------------------------
// Drawing

void HnStoryActivity::drawHeader() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  int y = metrics.topPadding;
  const int titleLineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  for (const auto& line : titleLines) {
    renderer.drawText(UI_12_FONT_ID, bodyLeft, y, line.c_str(), true, EpdFontFamily::BOLD);
    y += titleLineHeight;
  }
  const std::string fitted = renderer.truncatedText(SMALL_FONT_ID, meta.c_str(), bodyWidth);
  renderer.drawText(SMALL_FONT_ID, bodyLeft, y, fitted.c_str());
}

void HnStoryActivity::drawToolbar() const {
  for (int tool = 0; tool < TOOL_COUNT; tool++) {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    toolRect(tool, x, y, w, h);
    const bool focused = focus == tool;
    const bool active =
        (tool == TOOL_ARTICLE && view == View::ARTICLE) || (tool == TOOL_COMMENTS && view == View::COMMENTS);
    if (focused) {
      renderer.fillRoundedRect(x, y, w, h, 4, Color::Black);
    } else {
      renderer.drawRoundedRect(x, y, w, h, active ? 2 : 1, 4, true);
    }
    const std::string label = renderer.truncatedText(UI_10_FONT_ID, toolLabel(tool), w - 8);
    const int tw = renderer.getTextWidth(UI_10_FONT_ID, label.c_str());
    const int ty = y + (h - renderer.getLineHeight(UI_10_FONT_ID)) / 2;
    renderer.drawText(UI_10_FONT_ID, x + (w - tw) / 2, ty, label.c_str(), !focused);
  }
}

void HnStoryActivity::drawArticle() const {
  if (!articleAvailable) {
    const int mid = (bodyTop + bodyBottom) / 2;
    renderer.drawCenteredText(UI_12_FONT_ID, mid - 30, tr(STR_HN_NO_ARTICLE), true, EpdFontFamily::BOLD);
    if (!article.error.empty()) {
      const std::string reason = renderer.truncatedText(UI_10_FONT_ID, article.error.c_str(), bodyWidth);
      renderer.drawCenteredText(UI_10_FONT_ID, mid, reason.c_str());
    }
    renderer.drawCenteredText(UI_10_FONT_ID, mid + 30, tr(STR_HN_TRY_COMMENTS));
    return;
  }
  for (size_t i = 0; i < articleLines.size(); i++) {
    if (articleLines[i]) articleLines[i]->render(renderer, articleFontId, bodyLeft, articleLineY[i]);
  }
}

void HnStoryActivity::drawComments() const {
  if (entries.empty()) {
    renderer.drawCenteredText(UI_12_FONT_ID, (bodyTop + bodyBottom) / 2, tr(STR_HN_NO_COMMENTS));
    return;
  }
  for (size_t i = 0; i < entries.size(); i++) {
    const Entry& e = entries[i];
    const int visualDepth = std::min<int>(e.depth, MAX_VISUAL_DEPTH);
    const int indent = visualDepth * INDENT_PX;
    const int x = bodyLeft + indent;
    // One thin thread line per level, hugging the left of each indent step.
    for (int level = 1; level <= visualDepth; level++) {
      const int lx = bodyLeft + level * INDENT_PX - INDENT_PX / 2 - 1;
      renderer.drawLine(lx, e.y, lx, e.y + e.height, true);
    }
    const bool focused = focus == TOOL_COUNT + static_cast<int>(i);
    const std::string header = renderer.truncatedText(SMALL_FONT_ID, e.header.c_str(), bodyWidth - indent - 8);
    if (focused) {
      const int hw = renderer.getTextWidth(SMALL_FONT_ID, header.c_str());
      renderer.fillRoundedRect(x - 3, e.y, hw + 8, metaLineHeight, 3, Color::Black);
      renderer.drawText(SMALL_FONT_ID, x + 1, e.y, header.c_str(), false);
    } else {
      renderer.drawText(SMALL_FONT_ID, x + 1, e.y, header.c_str(), true);
    }
    int y = e.y + metaLineHeight;
    for (const auto& line : e.lines) {
      if (line) {
        line->render(renderer, commentFontId, x, y);
        y += commentLineHeight;
      } else {
        y += commentLineHeight / 2;
      }
    }
  }
}

void HnStoryActivity::drawFooter() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  char buf[40];
  const int page =
      view == View::ARTICLE ? static_cast<int>(articlePages.size()) : static_cast<int>(commentPages.size());
  const bool atEnd = view == View::ARTICLE ? articleAtEnd : commentsAtEnd;
  snprintf(buf, sizeof(buf), atEnd ? tr(STR_HN_PAGE_END_FORMAT) : tr(STR_HN_PAGE_FORMAT), std::max(1, page));
  const int width = renderer.getTextWidth(SMALL_FONT_ID, buf);
  const int y = renderer.getScreenHeight() - metrics.buttonHintsHeight - metaLineHeight;
  renderer.drawText(SMALL_FONT_ID, renderer.getScreenWidth() - width - bodyLeft, y, buf);

  const char* confirmLabel = tr(STR_SELECT);
  if (state == State::ERROR) {
    confirmLabel = tr(STR_RETRY);
  } else if (focus >= TOOL_COUNT) {
    confirmLabel = view == View::COMMENTS ? tr(STR_HN_FOLD) : tr(STR_HN_NEXT_PAGE_SHORT);
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void HnStoryActivity::render(RenderLock&&) {
  renderer.clearScreen();
  if (chromeVisible || state != State::READY) drawHeader();

  if (state == State::LOADING || state == State::ERROR) {
    const int mid = (bodyTop + bodyBottom) / 2;
    if (state == State::LOADING) {
      renderer.drawCenteredText(UI_12_FONT_ID, mid, tr(STR_LOADING), true, EpdFontFamily::BOLD);
    } else {
      renderer.drawCenteredText(UI_12_FONT_ID, mid - 15, tr(STR_ERROR_MSG), true, EpdFontFamily::BOLD);
      renderer.drawCenteredText(UI_10_FONT_ID, mid + 15, errorMessage ? errorMessage : "");
    }
    drawFooter();
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    return;
  }

  if (chromeVisible) drawToolbar();
  if (view == View::ARTICLE) {
    drawArticle();
  } else {
    drawComments();
  }
  drawFooter();
  if (busy) {
    GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  } else if (toastText) {
    GUI.drawPopup(renderer, toastText);
  }
  // Page turns ghost on a fast LUT; every few of them take the slower clean
  // waveform. Selection moves and toasts stay fast.
  const auto mode = fullRefreshPending ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH;
  fullRefreshPending = false;
  renderer.displayBuffer(mode);
}
