#pragma once
#include <StreamingJsonParser.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Pulp fork: models and streaming JSON sinks for pulp's Hacker News proxy
// (GET /hn/feed, GET /hn/item/{id}). Host-testable: no Arduino dependencies.
namespace hn {

enum class FeedKind : uint8_t { TOP, NEW, BEST, ASK, SHOW, COUNT };

// Query value for ?kind= ("top", "new", ...).
const char* feedKindQuery(FeedKind kind);

struct FeedItem {
  uint32_t id = 0;
  std::string title;
  std::string domain;  // "" for self posts
  std::string by;
  std::string age;  // server-formatted, e.g. "3h"
  uint16_t points = 0;
  uint16_t comments = 0;
};

struct Feed {
  std::vector<FeedItem> items;
  uint16_t page = 1;
  bool hasMore = false;
};

struct Comment {
  uint32_t id = 0;
  std::string by;
  std::string age;
  std::string text;  // plain text, "\n\n" between paragraphs
  uint16_t kids = 0;
  uint8_t depth = 0;
  bool dead = false;
};

struct Article {
  bool present = false;  // the response carried an "article" object (page 1 only)
  bool ok = false;
  bool truncated = false;
  std::string title;
  std::string byline;
  std::string error;
  std::vector<std::string> paragraphs;
};

struct Item {
  uint32_t id = 0;
  std::string title;
  std::string url;
  std::string domain;
  std::string by;
  std::string age;
  std::string text;  // Ask HN self text, "" otherwise
  uint16_t points = 0;
  uint16_t commentsTotal = 0;
  uint16_t page = 1;
  bool hasMore = false;
  Article article;
  std::vector<Comment> comments;  // depth-first reading order, one API page
};

// Working-set caps. The server pages the feed at 30 and comments at 40; these
// bound a misbehaving response, they are not targets. Text caps keep one page
// of comments around 40 KB worst case on the internal heap.
constexpr size_t MAX_FEED_ITEMS = 60;
constexpr size_t MAX_COMMENTS_PER_PAGE = 64;
constexpr size_t MAX_PARAGRAPHS = 256;
constexpr size_t MAX_COMMENT_TEXT = 4096;
constexpr size_t MAX_PARAGRAPH_TEXT = 4096;
constexpr size_t MAX_ARTICLE_TEXT = 32768;
constexpr size_t MAX_SHORT_TEXT = 256;  // titles, names, ages, domains

// Sink for GET /hn/feed. Feed the parser built from callbacks(); `out` is
// filled as items arrive. good() is false when the body was not a feed object.
class FeedSink {
 public:
  explicit FeedSink(Feed& out);
  JsonCallbacks callbacks();
  bool good() const { return sawItems; }

 private:
  enum class Section : uint8_t { NONE, TOP, ITEMS, ITEM, OTHER };
  enum class Key : uint8_t { NONE, ID, TITLE, SITE, BY, AGE, POINTS, COMMENTS, PAGE, HAS_MORE, ITEMS, OTHER };

  Feed& out;
  FeedItem current;
  Section stack[StreamingJsonParser::MAX_NESTING + 1] = {};
  uint8_t depth = 0;
  Key key = Key::NONE;
  bool sawItems = false;

  Section section() const { return stack[depth]; }
  void push(Section s);
  void pop();
  std::string* stringTarget();

  static void onKey(void* ctx, const char* key, size_t len);
  static void onStringPart(void* ctx, const char* value, size_t len, bool last);
  static void onNumber(void* ctx, const char* value, size_t len);
  static void onBool(void* ctx, bool value);
  static void onNull(void* ctx);
  static void onObjectStart(void* ctx);
  static void onObjectEnd(void* ctx);
  static void onArrayStart(void* ctx);
  static void onArrayEnd(void* ctx);
};

// Sink for GET /hn/item/{id}. Comments and article paragraphs land in `out`
// as they stream; long strings arrive in pieces and are appended up to the
// caps above (excess bytes are dropped and the article marked truncated).
class ItemSink {
 public:
  explicit ItemSink(Item& out);
  JsonCallbacks callbacks();
  bool good() const { return sawId; }

 private:
  enum class Section : uint8_t { NONE, TOP, ARTICLE, PARAGRAPHS, COMMENTS, COMMENT, OTHER };
  enum class Key : uint8_t {
    NONE,
    ID,
    TITLE,
    URL,
    SITE,
    BY,
    AGE,
    POINTS,
    COMMENTS_TOTAL,
    TEXT,
    PAGE,
    HAS_MORE,
    ARTICLE,
    COMMENTS,
    OK,
    BYLINE,
    PARAGRAPHS,
    TRUNCATED,
    ERROR_TEXT,
    DEPTH,
    DEAD,
    KIDS,
    OTHER
  };

  Item& out;
  Comment current;
  std::string paragraph;
  size_t articleBytes = 0;
  Section stack[StreamingJsonParser::MAX_NESTING + 1] = {};
  uint8_t depth = 0;
  Key key = Key::NONE;
  bool sawId = false;

  Section section() const { return stack[depth]; }
  void push(Section s);
  void pop();
  // Where the current string value accumulates and its cap; null = skip.
  std::string* stringTarget(size_t& cap);
  void appendCapped(std::string& target, size_t cap, const char* value, size_t len);

  static void onKey(void* ctx, const char* key, size_t len);
  static void onStringPart(void* ctx, const char* value, size_t len, bool last);
  static void onNumber(void* ctx, const char* value, size_t len);
  static void onBool(void* ctx, bool value);
  static void onNull(void* ctx);
  static void onObjectStart(void* ctx);
  static void onObjectEnd(void* ctx);
  static void onArrayStart(void* ctx);
  static void onArrayEnd(void* ctx);
};

// Whole-buffer helpers (tests, stubs). The device streams through the sinks.
bool parseFeed(const char* json, size_t len, Feed& out);
bool parseItem(const char* json, size_t len, Item& out);

// Number of comments hidden under `index` when it is collapsed: entries after
// it whose depth is greater, up to the end of `comments`. The subtree may
// continue on the next API page; `openEnded` reports that case.
size_t subtreeSize(const std::vector<Comment>& comments, size_t index, bool& openEnded);

}  // namespace hn
