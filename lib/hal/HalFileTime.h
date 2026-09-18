#pragma once

// Pulp fork. Stamps files SdFat creates or modifies with the wall-clock time.
// Upstream never installs FsDateTime's callback, so every download lands with
// SdFat's default 2000-01-01 date and the Library's "Recent" order and any
// mtime-based sort put it below everything else.
namespace HalFileTime {

// Install once at boot, before the first SD write. Time comes from the RTC when
// the board has one (halClock), else from the system clock once SNTP has set
// it; until either is plausible SdFat's default date is kept so bogus 1970
// stamps never reach the card.
void begin();

}  // namespace HalFileTime
