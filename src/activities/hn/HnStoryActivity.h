#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "HnTextLayout.h"
#include "activities/Activity.h"
#include "network/HnJson.h"

// Pulp fork: one Hacker News story — the extracted article (or Ask HN text)
// and the threaded comments, both paged to the e-ink screen.
//
// Navigation is a single ring the side buttons walk: the toolbar buttons
// (Article · Comments · Save · Next thread · ‹ page · page ›) followed by the
// body items of the current page (one per comment in Comments, one for the
// whole page in Article). Down past the last body item turns the page; Up from
// the first body item climbs into the toolbar. Confirm activates the focused
// toolbar button or collapses/expands the focused comment's subtree. Touch
// taps the same targets; a left/right swipe pages; the back gesture returns to
// the feed. Everything is therefore reachable on the X4 Pro's two side keys
// plus the power-click Confirm.
//
// Memory: one API page of comments (40) is resident at a time; a screen page
// is laid out from a cursor into the flattened depth-first order, and turning
// pages forward past the loaded API page fetches the next one. Page starts are
// remembered so the previous page is a re-layout, not a re-fetch, unless it
// lives on an earlier API page.
class HnStoryActivity final : public Activity {
 public:
  HnStoryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string baseUrl, std::string username,
                  std::string password, uint32_t storyId, std::string title, std::string meta);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == State::LOADING || busy; }

  // Toolbar order puts the view tabs last, one Up-press away from the body.
  enum Tool : uint8_t {
    TOOL_SAVE,
    TOOL_NEXT_TOP,
    TOOL_PREV_PAGE,
    TOOL_NEXT_PAGE,
    TOOL_ARTICLE,
    TOOL_COMMENTS,
    TOOL_COUNT
  };

 private:
  enum class State : uint8_t { LOADING, READY, ERROR };
  enum class View : uint8_t { ARTICLE, COMMENTS };

  // Indent per thread level; the visual depth stops growing at MAX_VISUAL_DEPTH
  // and deeper comments carry a "»" marker instead.
  static constexpr int INDENT_PX = 12;
  static constexpr int MAX_VISUAL_DEPTH = 6;
  static constexpr int ENTRY_GAP_PX = 8;
  static constexpr int TOOLBAR_HEIGHT = 34;
  static constexpr int TOOLBAR_GAP = 6;
  static constexpr int PAGES_PER_FULL_REFRESH = 6;
  static constexpr unsigned long TOAST_MS = 1800;
  static constexpr size_t MAX_COLLAPSED = 64;

  // Where a screen page of comments starts: a flat index into the depth-first
  // order (across API pages) and a line offset for a comment that spilled
  // over from the previous page.
  struct CommentCursor {
    uint32_t flat = 0;
    uint16_t line = 0;
    bool operator==(const CommentCursor& o) const { return flat == o.flat && line == o.line; }
  };
  struct ArticleCursor {
    uint16_t paragraph = 0;
    uint16_t line = 0;
  };
  // One comment as laid out on the current page.
  struct Entry {
    uint32_t flat = 0;
    uint32_t id = 0;
    uint8_t depth = 0;
    bool collapsed = false;
    bool continued = false;  // body continues from the previous page
    bool cut = false;        // body continues on the next page
    uint16_t hidden = 0;     // subtree size when collapsed
    int y = 0;               // screen rows, set by layout
    int height = 0;
    std::string header;  // "author · age" plus markers
    hn::Lines lines;
  };

  const std::string baseUrl;
  const std::string username;
  const std::string password;
  const uint32_t storyId;
  const std::string title;
  const std::string meta;

  State state = State::LOADING;
  View view = View::COMMENTS;
  const char* errorMessage = nullptr;
  bool loadRequested = false;
  int focus = TOOL_COUNT;  // ring position: < TOOL_COUNT toolbar, else body item
  int pageTurns = 0;
  bool fullRefreshPending = false;
  bool busy = false;  // blocking fetch in progress: render shows the Loading popup
  const char* toastText = nullptr;
  unsigned long toastUntilMs = 0;

  // Fonts and geometry, resolved in onEnter.
  int articleFontId = 0;
  int commentFontId = 0;
  int articleLineHeight = 0;
  int commentLineHeight = 0;
  int metaLineHeight = 0;
  int bodyTop = 0;
  int bodyBottom = 0;
  int bodyLeft = 0;
  int bodyWidth = 0;
  int headerBottom = 0;
  std::vector<std::string> titleLines;

  // Item data: the resident API page of comments, and the article (or Ask HN
  // text) lifted out of page 1 so later comment fetches leave it alone.
  hn::Item item;
  hn::Article article;
  uint16_t loadedApiPage = 0;  // 0 = nothing loaded
  bool articleAvailable = false;
  std::vector<uint32_t> collapsedIds;

  // Article paging: page i starts at articlePages[i]; the current page's lines
  // and their screen rows (paragraph gaps show up as larger steps).
  std::vector<ArticleCursor> articlePages;
  ArticleCursor articleNext;
  bool articleAtEnd = false;
  hn::Lines articleLines;
  std::vector<int16_t> articleLineY;

  // Comment paging.
  std::vector<CommentCursor> commentPages;
  CommentCursor commentNext;
  bool commentsAtEnd = false;
  std::vector<Entry> entries;

  // --- data ------------------------------------------------------------------
  bool loadStory();
  bool ensureApiPage(uint16_t apiPage);
  // Comment at a flat index, loading its API page on demand; null past the end.
  const hn::Comment* commentAt(uint32_t& flat);
  bool isCollapsed(uint32_t id) const;
  void toggleCollapsed(uint32_t id);
  void prepareArticle();

  // --- layout (runs on the loop task; render only draws) --------------------
  void layoutArticlePage(ArticleCursor start);
  void layoutCommentPage(CommentCursor start);
  int bodyItemCount() const;

  // --- actions ---------------------------------------------------------------
  void switchView(View next);
  void nextPage();
  void prevPage();
  void nextTopLevel();
  void save();
  void activateFocus();
  void moveFocus(int delta);
  void showToast(const char* text);
  void fail(const char* message);
  void setLoading();

  // --- drawing ---------------------------------------------------------------
  void computeGeometry();
  void toolRect(int tool, int& x, int& y, int& w, int& h) const;
  void drawHeader() const;
  void drawToolbar() const;
  void drawArticle() const;
  void drawComments() const;
  void drawFooter() const;
  bool handleTap(int x, int y);
};
