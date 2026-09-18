#include "PulpShelf.h"

#include <Logging.h>
#include <StreamingJsonParser.h>

#include <cstdlib>
#include <cstring>

#include "HttpDownloader.h"

namespace {
struct ShelfSink {
  enum class Key : uint8_t { NONE, FILE, URL, SIZE };

  std::vector<PulpShelfItem>& out;
  PulpShelfItem current;
  Key key = Key::NONE;
  int depth = 0;  // 1 = top-level array, 2 = an item object

  explicit ShelfSink(std::vector<PulpShelfItem>& o) : out(o) {}
};

void onKey(void* ctx, const char* key, const size_t len) {
  auto* s = static_cast<ShelfSink*>(ctx);
  if (len == 4 && memcmp(key, "file", 4) == 0) {
    s->key = ShelfSink::Key::FILE;
  } else if (len == 3 && memcmp(key, "url", 3) == 0) {
    s->key = ShelfSink::Key::URL;
  } else if (len == 4 && memcmp(key, "size", 4) == 0) {
    s->key = ShelfSink::Key::SIZE;
  } else {
    s->key = ShelfSink::Key::NONE;
  }
}

void onString(void* ctx, const char* value, const size_t len) {
  auto* s = static_cast<ShelfSink*>(ctx);
  if (s->depth == 2) {
    if (s->key == ShelfSink::Key::FILE) s->current.file.assign(value, len);
    if (s->key == ShelfSink::Key::URL) s->current.url.assign(value, len);
  }
  s->key = ShelfSink::Key::NONE;
}

void onNumber(void* ctx, const char* value, const size_t len) {
  auto* s = static_cast<ShelfSink*>(ctx);
  if (s->depth == 2 && s->key == ShelfSink::Key::SIZE) {
    char buf[24];
    const size_t n = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
    memcpy(buf, value, n);
    buf[n] = '\0';
    s->current.size = static_cast<uint32_t>(strtoul(buf, nullptr, 10));
  }
  s->key = ShelfSink::Key::NONE;
}

void onScalar(void* ctx) { static_cast<ShelfSink*>(ctx)->key = ShelfSink::Key::NONE; }
void onBool(void* ctx, bool) { onScalar(ctx); }

void onObjectStart(void* ctx) {
  auto* s = static_cast<ShelfSink*>(ctx);
  s->depth++;
  if (s->depth == 2) s->current = PulpShelfItem{};
}

void onObjectEnd(void* ctx) {
  auto* s = static_cast<ShelfSink*>(ctx);
  if (s->depth == 2 && !s->current.file.empty() && !s->current.url.empty() && s->out.size() < pulp::MAX_SHELF_ITEMS) {
    s->out.push_back(std::move(s->current));
    s->current = PulpShelfItem{};
  }
  s->depth--;
}

void onArrayStart(void* ctx) { static_cast<ShelfSink*>(ctx)->depth++; }
void onArrayEnd(void* ctx) { static_cast<ShelfSink*>(ctx)->depth--; }
}  // namespace

bool pulp::fetchShelf(const std::string& baseUrl, const std::string& username, const std::string& password,
                      std::vector<PulpShelfItem>& out) {
  out.clear();
  out.reserve(64);  // typical shelf; grows only past that
  ShelfSink sink(out);
  StreamingJsonParser parser(JsonCallbacks{&sink, onKey, onString, onNumber, onBool, onScalar, onObjectStart,
                                           onObjectEnd, onArrayStart, onArrayEnd});

  const std::string url = baseUrl + "/api/shelf";
  const bool ok = HttpDownloader::fetchUrl(
      url,
      [&parser](const uint8_t* data, const size_t len) {
        parser.feed(reinterpret_cast<const char*>(data), len);
        return !parser.hasError();
      },
      username, password);
  if (!ok || parser.hasError()) {
    LOG_ERR("PULP", "shelf fetch failed (%s, parse error=%d)", url.c_str(), parser.hasError() ? 1 : 0);
    out.clear();
    return false;
  }
  LOG_INF("PULP", "shelf: %u items", static_cast<unsigned>(out.size()));
  return true;
}
