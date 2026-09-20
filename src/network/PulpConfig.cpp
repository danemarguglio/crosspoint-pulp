#include "PulpConfig.h"

#include <esp_attr.h>

#include <cctype>
#include <ctime>

#include "CrossPointSettings.h"
#include "WifiCredentialStore.h"

namespace {
constexpr uint32_t STAMP_MAGIC = 0x50554C50;  // "PULP"
RTC_NOINIT_ATTR uint32_t pulpStampMagic;
RTC_NOINIT_ATTR uint32_t pulpStampEpoch;

bool nameIsPulp(const std::string& name) {
  static constexpr char WANT[] = "pulp";
  if (name.size() != sizeof(WANT) - 1) return false;
  for (size_t i = 0; i < name.size(); i++) {
    if (std::tolower(static_cast<unsigned char>(name[i])) != WANT[i]) return false;
  }
  return true;
}

std::string stripOpdsSuffix(std::string url) {
  while (!url.empty() && url.back() == '/') url.pop_back();
  static constexpr char SUFFIX[] = "/opds";
  constexpr size_t n = sizeof(SUFFIX) - 1;
  if (url.size() > n && url.compare(url.size() - n, n, SUFFIX) == 0) url.erase(url.size() - n);
  while (!url.empty() && url.back() == '/') url.pop_back();
  return url;
}

const OpdsServer* pulpEntry() {
  for (const auto& s : OPDS_STORE.getServers()) {
    if (nameIsPulp(s.name)) return &s;
  }
  return nullptr;
}
}  // namespace

std::string pulp::baseUrl() {
  if (const auto* entry = pulpEntry()) {
    const std::string base = stripOpdsSuffix(entry->url);
    if (!base.empty()) return base;
  }
#ifdef PULP_DEFAULT_URL
  return stripOpdsSuffix(PULP_DEFAULT_URL);
#else
  return "";
#endif
}

OpdsServer pulp::server() {
  if (const auto* entry = pulpEntry()) return *entry;
  return OpdsServer{};
}

bool pulp::opdsBrowserSuperseded() {
  const auto& servers = OPDS_STORE.getServers();
  return servers.size() == 1 && nameIsPulp(servers[0].name);
}

bool pulp::autoSyncDue() {
  if (!SETTINGS.pulpAutoSync) return false;
  if (baseUrl().empty()) return false;
  // Nothing else loads the credential store this early in boot.
  if (WIFI_STORE.getCredentialCount() == 0) WIFI_STORE.loadFromFile();
  if (WIFI_STORE.getCredentialCount() == 0) return false;
  if (pulpStampMagic != STAMP_MAGIC) return true;
  const auto now = static_cast<uint32_t>(time(nullptr));
  if (now < pulpStampEpoch) return true;
  return now - pulpStampEpoch >= static_cast<uint32_t>(SETTINGS.pulpAutoSyncMinutes) * 60u;
}

void pulp::stampAutoSync() {
  pulpStampEpoch = static_cast<uint32_t>(time(nullptr));
  pulpStampMagic = STAMP_MAGIC;
}
