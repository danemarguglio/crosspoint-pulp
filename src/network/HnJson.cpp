#include "HnJson.h"

#include <cstdlib>
#include <cstring>

namespace hn {

namespace {
bool keyIs(const char* key, const size_t len, const char* want) {
  const size_t n = strlen(want);
  return len == n && memcmp(key, want, n) == 0;
}

uint32_t parseU32(const char* value, const size_t len) {
  char buf[24];
  const size_t n = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
  memcpy(buf, value, n);
  buf[n] = '\0';
  const long long v = strtoll(buf, nullptr, 10);
  if (v < 0) return 0;
  return v > 0xFFFFFFFFLL ? 0xFFFFFFFFu : static_cast<uint32_t>(v);
}

uint16_t clampU16(const uint32_t v) { return v > 0xFFFFu ? 0xFFFFu : static_cast<uint16_t>(v); }

void appendCappedTo(std::string& target, const size_t cap, const char* value, const size_t len) {
  if (target.size() >= cap) return;
  const size_t room = cap - target.size();
  target.append(value, len < room ? len : room);
}
}  // namespace

const char* feedKindQuery(const FeedKind kind) {
  switch (kind) {
    case FeedKind::TOP:
      return "top";
    case FeedKind::NEW:
      return "new";
    case FeedKind::BEST:
      return "best";
    case FeedKind::ASK:
      return "ask";
    case FeedKind::SHOW:
      return "show";
    default:
      return "top";
  }
}

// ---------------------------------------------------------------------------
// FeedSink

FeedSink::FeedSink(Feed& o) : out(o) {
  out.items.clear();
  out.items.reserve(32);  // one server page (30) plus slack
  out.hasMore = false;
}

JsonCallbacks FeedSink::callbacks() {
  JsonCallbacks cb{this,   onKey,         nullptr,     onNumber,     onBool,
                   onNull, onObjectStart, onObjectEnd, onArrayStart, onArrayEnd};
  cb.onStringPart = onStringPart;
  return cb;
}

void FeedSink::push(const Section s) {
  if (depth < StreamingJsonParser::MAX_NESTING) depth++;
  stack[depth] = s;
}

void FeedSink::pop() {
  if (depth > 0) depth--;
  key = Key::NONE;
}

std::string* FeedSink::stringTarget() {
  if (section() != Section::ITEM) return nullptr;
  switch (key) {
    case Key::TITLE:
      return &current.title;
    case Key::SITE:
      return &current.domain;
    case Key::BY:
      return &current.by;
    case Key::AGE:
      return &current.age;
    default:
      return nullptr;
  }
}

void FeedSink::onKey(void* ctx, const char* key, const size_t len) {
  auto* s = static_cast<FeedSink*>(ctx);
  if (keyIs(key, len, "id")) {
    s->key = Key::ID;
  } else if (keyIs(key, len, "title")) {
    s->key = Key::TITLE;
  } else if (keyIs(key, len, "domain")) {
    s->key = Key::SITE;
  } else if (keyIs(key, len, "by")) {
    s->key = Key::BY;
  } else if (keyIs(key, len, "age")) {
    s->key = Key::AGE;
  } else if (keyIs(key, len, "points")) {
    s->key = Key::POINTS;
  } else if (keyIs(key, len, "comments")) {
    s->key = Key::COMMENTS;
  } else if (keyIs(key, len, "page")) {
    s->key = Key::PAGE;
  } else if (keyIs(key, len, "has_more")) {
    s->key = Key::HAS_MORE;
  } else if (keyIs(key, len, "items")) {
    s->key = Key::ITEMS;
  } else {
    s->key = Key::OTHER;
  }
}

void FeedSink::onStringPart(void* ctx, const char* value, const size_t len, const bool last) {
  auto* s = static_cast<FeedSink*>(ctx);
  if (std::string* target = s->stringTarget()) appendCappedTo(*target, MAX_SHORT_TEXT, value, len);
  if (last) s->key = Key::NONE;
}

void FeedSink::onNumber(void* ctx, const char* value, const size_t len) {
  auto* s = static_cast<FeedSink*>(ctx);
  const uint32_t v = parseU32(value, len);
  if (s->section() == Section::ITEM) {
    if (s->key == Key::ID) s->current.id = v;
    if (s->key == Key::POINTS) s->current.points = clampU16(v);
    if (s->key == Key::COMMENTS) s->current.comments = clampU16(v);
  } else if (s->section() == Section::TOP) {
    if (s->key == Key::PAGE) s->out.page = clampU16(v);
  }
  s->key = Key::NONE;
}

void FeedSink::onBool(void* ctx, const bool value) {
  auto* s = static_cast<FeedSink*>(ctx);
  if (s->section() == Section::TOP && s->key == Key::HAS_MORE) s->out.hasMore = value;
  s->key = Key::NONE;
}

void FeedSink::onNull(void* ctx) { static_cast<FeedSink*>(ctx)->key = Key::NONE; }

void FeedSink::onObjectStart(void* ctx) {
  auto* s = static_cast<FeedSink*>(ctx);
  if (s->depth == 0) {
    s->push(Section::TOP);
  } else if (s->section() == Section::ITEMS) {
    s->current = FeedItem{};
    s->push(Section::ITEM);
  } else {
    s->push(Section::OTHER);
  }
  s->key = Key::NONE;
}

void FeedSink::onObjectEnd(void* ctx) {
  auto* s = static_cast<FeedSink*>(ctx);
  if (s->section() == Section::ITEM && s->current.id != 0 && s->out.items.size() < MAX_FEED_ITEMS) {
    s->out.items.push_back(std::move(s->current));
    s->current = FeedItem{};
  }
  s->pop();
}

void FeedSink::onArrayStart(void* ctx) {
  auto* s = static_cast<FeedSink*>(ctx);
  if (s->section() == Section::TOP && s->key == Key::ITEMS) {
    s->sawItems = true;
    s->push(Section::ITEMS);
  } else {
    s->push(Section::OTHER);
  }
  s->key = Key::NONE;
}

void FeedSink::onArrayEnd(void* ctx) { static_cast<FeedSink*>(ctx)->pop(); }

// ---------------------------------------------------------------------------
// ItemSink

ItemSink::ItemSink(Item& o) : out(o) {
  out.comments.clear();
  out.comments.reserve(40);  // one server page
  out.article = Article{};
  out.hasMore = false;
}

JsonCallbacks ItemSink::callbacks() {
  JsonCallbacks cb{this,   onKey,         nullptr,     onNumber,     onBool,
                   onNull, onObjectStart, onObjectEnd, onArrayStart, onArrayEnd};
  cb.onStringPart = onStringPart;
  return cb;
}

void ItemSink::push(const Section s) {
  if (depth < StreamingJsonParser::MAX_NESTING) depth++;
  stack[depth] = s;
}

void ItemSink::pop() {
  if (depth > 0) depth--;
  key = Key::NONE;
}

std::string* ItemSink::stringTarget(size_t& cap) {
  cap = MAX_SHORT_TEXT;
  switch (section()) {
    case Section::TOP:
      switch (key) {
        case Key::TITLE:
          return &out.title;
        case Key::URL:
          return &out.url;
        case Key::SITE:
          return &out.domain;
        case Key::BY:
          return &out.by;
        case Key::AGE:
          return &out.age;
        case Key::TEXT:
          cap = MAX_ARTICLE_TEXT;
          return &out.text;
        default:
          return nullptr;
      }
    case Section::ARTICLE:
      switch (key) {
        case Key::TITLE:
          return &out.article.title;
        case Key::BYLINE:
          return &out.article.byline;
        case Key::ERROR_TEXT:
          return &out.article.error;
        default:
          return nullptr;
      }
    case Section::PARAGRAPHS:
      cap = MAX_PARAGRAPH_TEXT;
      return &paragraph;
    case Section::COMMENT:
      switch (key) {
        case Key::BY:
          return &current.by;
        case Key::AGE:
          return &current.age;
        case Key::TEXT:
          cap = MAX_COMMENT_TEXT;
          return &current.text;
        default:
          return nullptr;
      }
    default:
      return nullptr;
  }
}

void ItemSink::appendCapped(std::string& target, const size_t cap, const char* value, const size_t len) {
  const size_t before = target.size();
  appendCappedTo(target, cap, value, len);
  if (target.size() - before < len && (&target == &paragraph || &target == &out.text)) out.article.truncated = true;
}

void ItemSink::onKey(void* ctx, const char* key, const size_t len) {
  auto* s = static_cast<ItemSink*>(ctx);
  struct Entry {
    const char* name;
    Key key;
  };
  static constexpr Entry TABLE[] = {
      {"id", Key::ID},
      {"title", Key::TITLE},
      {"url", Key::URL},
      {"domain", Key::SITE},
      {"by", Key::BY},
      {"age", Key::AGE},
      {"points", Key::POINTS},
      {"comments_total", Key::COMMENTS_TOTAL},
      {"text", Key::TEXT},
      {"page", Key::PAGE},
      {"has_more", Key::HAS_MORE},
      {"article", Key::ARTICLE},
      {"comments", Key::COMMENTS},
      {"ok", Key::OK},
      {"byline", Key::BYLINE},
      {"paragraphs", Key::PARAGRAPHS},
      {"truncated", Key::TRUNCATED},
      {"error", Key::ERROR_TEXT},
      {"depth", Key::DEPTH},
      {"dead", Key::DEAD},
      {"kids", Key::KIDS},
  };
  s->key = Key::OTHER;
  for (const auto& e : TABLE) {
    if (keyIs(key, len, e.name)) {
      s->key = e.key;
      return;
    }
  }
}

void ItemSink::onStringPart(void* ctx, const char* value, const size_t len, const bool last) {
  auto* s = static_cast<ItemSink*>(ctx);
  size_t cap = 0;
  if (std::string* target = s->stringTarget(cap)) s->appendCapped(*target, cap, value, len);
  if (!last) return;
  if (s->section() == Section::PARAGRAPHS) {
    // One paragraph complete. The whole-article byte cap keeps a runaway page
    // from filling the heap; the server also truncates on its side.
    if (s->out.article.paragraphs.size() < MAX_PARAGRAPHS &&
        s->articleBytes + s->paragraph.size() <= MAX_ARTICLE_TEXT) {
      s->articleBytes += s->paragraph.size();
      s->out.article.paragraphs.push_back(std::move(s->paragraph));
    } else {
      s->out.article.truncated = true;
    }
    s->paragraph.clear();
  }
  s->key = Key::NONE;
}

void ItemSink::onNumber(void* ctx, const char* value, const size_t len) {
  auto* s = static_cast<ItemSink*>(ctx);
  const uint32_t v = parseU32(value, len);
  switch (s->section()) {
    case Section::TOP:
      switch (s->key) {
        case Key::ID:
          s->out.id = v;
          s->sawId = v != 0;
          break;
        case Key::POINTS:
          s->out.points = clampU16(v);
          break;
        case Key::COMMENTS_TOTAL:
          s->out.commentsTotal = clampU16(v);
          break;
        case Key::PAGE:
          s->out.page = clampU16(v);
          break;
        default:
          break;
      }
      break;
    case Section::COMMENT:
      switch (s->key) {
        case Key::ID:
          s->current.id = v;
          break;
        case Key::DEPTH:
          s->current.depth = v > 255 ? 255 : static_cast<uint8_t>(v);
          break;
        case Key::KIDS:
          s->current.kids = clampU16(v);
          break;
        default:
          break;
      }
      break;
    default:
      break;
  }
  s->key = Key::NONE;
}

void ItemSink::onBool(void* ctx, const bool value) {
  auto* s = static_cast<ItemSink*>(ctx);
  switch (s->section()) {
    case Section::TOP:
      if (s->key == Key::HAS_MORE) s->out.hasMore = value;
      break;
    case Section::ARTICLE:
      if (s->key == Key::OK) s->out.article.ok = value;
      if (s->key == Key::TRUNCATED && value) s->out.article.truncated = true;
      break;
    case Section::COMMENT:
      if (s->key == Key::DEAD) s->current.dead = value;
      break;
    default:
      break;
  }
  s->key = Key::NONE;
}

void ItemSink::onNull(void* ctx) { static_cast<ItemSink*>(ctx)->key = Key::NONE; }

void ItemSink::onObjectStart(void* ctx) {
  auto* s = static_cast<ItemSink*>(ctx);
  if (s->depth == 0) {
    s->push(Section::TOP);
  } else if (s->section() == Section::TOP && s->key == Key::ARTICLE) {
    s->out.article.present = true;
    s->push(Section::ARTICLE);
  } else if (s->section() == Section::COMMENTS) {
    s->current = Comment{};
    s->push(Section::COMMENT);
  } else {
    s->push(Section::OTHER);
  }
  s->key = Key::NONE;
}

void ItemSink::onObjectEnd(void* ctx) {
  auto* s = static_cast<ItemSink*>(ctx);
  if (s->section() == Section::COMMENT && s->current.id != 0 && s->out.comments.size() < MAX_COMMENTS_PER_PAGE) {
    s->out.comments.push_back(std::move(s->current));
    s->current = Comment{};
  }
  s->pop();
}

void ItemSink::onArrayStart(void* ctx) {
  auto* s = static_cast<ItemSink*>(ctx);
  if (s->section() == Section::TOP && s->key == Key::COMMENTS) {
    s->push(Section::COMMENTS);
  } else if (s->section() == Section::ARTICLE && s->key == Key::PARAGRAPHS) {
    s->paragraph.clear();
    s->push(Section::PARAGRAPHS);
  } else {
    s->push(Section::OTHER);
  }
  s->key = Key::NONE;
}

void ItemSink::onArrayEnd(void* ctx) { static_cast<ItemSink*>(ctx)->pop(); }

// ---------------------------------------------------------------------------

bool parseFeed(const char* json, const size_t len, Feed& out) {
  FeedSink sink(out);
  StreamingJsonParser parser(sink.callbacks());
  parser.feed(json, len);
  return !parser.hasError() && sink.good();
}

bool parseItem(const char* json, const size_t len, Item& out) {
  ItemSink sink(out);
  StreamingJsonParser parser(sink.callbacks());
  parser.feed(json, len);
  return !parser.hasError() && sink.good();
}

size_t subtreeSize(const std::vector<Comment>& comments, const size_t index, bool& openEnded) {
  openEnded = false;
  if (index >= comments.size()) return 0;
  const uint8_t depth = comments[index].depth;
  size_t n = 0;
  for (size_t i = index + 1; i < comments.size(); i++) {
    if (comments[i].depth <= depth) return n;
    n++;
  }
  openEnded = true;  // ran off the loaded page without meeting a sibling
  return n;
}

}  // namespace hn
