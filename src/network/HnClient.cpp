#include "HnClient.h"

#include <Arduino.h>
#include <HalMemory.h>
#include <Logging.h>
#include <StreamingJsonParser.h>
#include <base64.h>

#include <cstdio>

#include "HttpDownloader.h"

#if defined(FREEINK_NET_WOLFSSL)
#include <SecureHttpClient.h>
#else
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#endif

namespace hn {

namespace {
constexpr int HTTP_TIMEOUT_MS = 30000;

bool streamJson(const std::string& url, const std::string& username, const std::string& password,
                StreamingJsonParser& parser) {
  const bool ok = HttpDownloader::fetchUrl(
      url,
      [&parser](const uint8_t* data, const size_t len) {
        parser.feed(reinterpret_cast<const char*>(data), len);
        return !parser.hasError();
      },
      username, password);
  if (!ok || parser.hasError()) {
    LOG_ERR("HN", "fetch failed (%s, parse error=%d)", url.c_str(), parser.hasError() ? 1 : 0);
    return false;
  }
  return true;
}

void logHeap(const char* when) {
  const auto psram = HalMemory::getPsramHeap();
  LOG_INF("HN", "heap %s: %u free, %u max block, psram %u free", when, ESP.getFreeHeap(), ESP.getMaxAllocHeap(),
          static_cast<unsigned>(psram.freeBytes));
}
}  // namespace

bool heapAllowsFetch(const char* what) {
  logHeap(what);
  if (ESP.getFreeHeap() < MIN_FETCH_FREE_HEAP || ESP.getMaxAllocHeap() < MIN_FETCH_MAX_ALLOC) {
    LOG_ERR("HN", "low heap for %s (%u free, %u max block)", what, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    return false;
  }
  return true;
}

bool fetchFeed(const std::string& baseUrl, const std::string& username, const std::string& password,
               const FeedKind kind, const uint16_t page, Feed& out) {
  char query[64];
  snprintf(query, sizeof(query), "/hn/feed?kind=%s&page=%u&per=%u", feedKindQuery(kind), static_cast<unsigned>(page),
           static_cast<unsigned>(FEED_PER_PAGE));
  FeedSink sink(out);
  StreamingJsonParser parser(sink.callbacks());
  if (!streamJson(baseUrl + query, username, password, parser) || !sink.good()) {
    out.items.clear();
    return false;
  }
  LOG_INF("HN", "feed %s p%u: %u items, more=%d", feedKindQuery(kind), static_cast<unsigned>(page),
          static_cast<unsigned>(out.items.size()), out.hasMore ? 1 : 0);
  logHeap("after feed");
  return true;
}

bool fetchItem(const std::string& baseUrl, const std::string& username, const std::string& password, const uint32_t id,
               const uint16_t page, const bool withArticle, Item& out) {
  char query[80];
  snprintf(query, sizeof(query), "/hn/item/%lu?page=%u&per=%u&article=%d", static_cast<unsigned long>(id),
           static_cast<unsigned>(page), static_cast<unsigned>(COMMENTS_PER_PAGE), withArticle ? 1 : 0);
  ItemSink sink(out);
  StreamingJsonParser parser(sink.callbacks());
  if (!streamJson(baseUrl + query, username, password, parser) || !sink.good()) {
    out.comments.clear();
    return false;
  }
  LOG_INF("HN", "item %lu p%u: %u comments, %u paragraphs, more=%d", static_cast<unsigned long>(id),
          static_cast<unsigned>(page), static_cast<unsigned>(out.comments.size()),
          static_cast<unsigned>(out.article.paragraphs.size()), out.hasMore ? 1 : 0);
  logHeap("after item");
  return true;
}

bool saveItem(const std::string& baseUrl, const std::string& username, const std::string& password, const uint32_t id) {
  char path[32];
  snprintf(path, sizeof(path), "/hn/save/%lu", static_cast<unsigned long>(id));
  const std::string url = baseUrl + path;
  LOG_INF("HN", "POST %s", url.c_str());

#if defined(FREEINK_NET_WOLFSSL)
  // Same client setup as HttpDownloader's wolfSSL GET path; plain http URLs
  // ride a WiFiClient inside, so no TLS buffers are touched here.
  freeink::SecureHttpClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setInsecure();
  if (!http.begin(url)) {
    LOG_ERR("HN", "bad URL: %s", url.c_str());
    return false;
  }
  http.setUserAgent("CrossPoint-ESP32-" CROSSPOINT_VERSION);
  if (!username.empty() && !password.empty()) http.setBasicAuth(username, password);
  const int status = http.POST(std::string());
  http.end();
  if (status != 200) {
    LOG_ERR("HN", "save failed: status %d", status);
    return false;
  }
  return true;
#else
  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.method = HTTP_METHOD_POST;
  config.timeout_ms = HTTP_TIMEOUT_MS;
  config.crt_bundle_attach = esp_crt_bundle_attach;
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) {
    LOG_ERR("HN", "client init failed");
    return false;
  }
  esp_http_client_set_header(client, "User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);
  if (!username.empty() && !password.empty()) {
    const std::string credentials = username + ":" + password;
    const String header = "Basic " + base64::encode(credentials.c_str());
    esp_http_client_set_header(client, "Authorization", header.c_str());
  }
  esp_http_client_set_post_field(client, "", 0);
  const esp_err_t err = esp_http_client_perform(client);
  const int status = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);
  if (err != ESP_OK || status != 200) {
    LOG_ERR("HN", "save failed: %s status %d", esp_err_to_name(err), status);
    return false;
  }
  return true;
#endif
}

}  // namespace hn
