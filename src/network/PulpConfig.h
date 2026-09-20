#pragma once
#include <string>

#include "OpdsServerStore.h"

// Pulp fork: where the Pulp server lives and whether an automatic sync is due.
namespace pulp {

// Base URL ("http://host:port", no trailing slash). Resolved from the OPDS
// server entry named "Pulp" (its URL minus a trailing "/opds"), else the
// compile-time PULP_DEFAULT_URL, else "" (the feature stays hidden).
std::string baseUrl();

// The "Pulp" OPDS entry when present (for its credentials), else an empty one.
OpdsServer server();

// True when the only configured OPDS server is the "Pulp" entry: the Home
// "Pulp" item supersedes the OPDS Browser row, so Home hides it. Any other
// server configured brings the row back.
bool opdsBrowserSuperseded();

// True when auto-sync is enabled, a Pulp URL and a saved Wi-Fi network exist,
// and the last attempt is older than pulpAutoSyncMinutes (or unknown, or the
// clock moved backwards since).
bool autoSyncDue();

// Records "now" as the last attempt. Lives in RTC slow memory: it survives deep
// sleep and ESP.restart() but not a power-off, so a cold boot syncs once.
void stampAutoSync();

}  // namespace pulp
