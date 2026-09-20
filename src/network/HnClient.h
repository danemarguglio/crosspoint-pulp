#pragma once
#include <cstdint>
#include <string>

#include "HnJson.h"

// Pulp fork: HTTP access to pulp's Hacker News proxy. Every body streams
// through the HnJson sinks; nothing buffers a whole response.
namespace hn {

// Pre-flight heap floor for one fetch. Plain http to the local server needs
// no TLS record buffer (~17 KB, the reason for HttpDownloader's 40 KB floor);
// what still has to fit is the lwIP socket (~6 KB), the 1 KB read chunk, the
// parser's 512 B token buffer and one parsed page (~15 KB typical for 40
// comments, capped around 40 KB), plus the page layout that follows. 28 KB
// free with an 8 KB contiguous block covers a typical page with margin; a
// pathological page still fails cleanly inside the caps instead of abort()ing.
constexpr uint32_t MIN_FETCH_FREE_HEAP = 28000;
constexpr uint32_t MIN_FETCH_MAX_ALLOC = 8000;

// Logs the heap and returns false when a fetch should not be attempted.
bool heapAllowsFetch(const char* what);

// GET <base>/hn/feed?kind=&page=&per=30
bool fetchFeed(const std::string& baseUrl, const std::string& username, const std::string& password, FeedKind kind,
               uint16_t page, Feed& out);

// GET <base>/hn/item/{id}?page=&per=40&article=0|1
bool fetchItem(const std::string& baseUrl, const std::string& username, const std::string& password, uint32_t id,
               uint16_t page, bool withArticle, Item& out);

// POST <base>/hn/save/{id}; true when the server queued the story for the shelf.
bool saveItem(const std::string& baseUrl, const std::string& username, const std::string& password, uint32_t id);

constexpr uint16_t FEED_PER_PAGE = 30;
constexpr uint16_t COMMENTS_PER_PAGE = 40;

}  // namespace hn
