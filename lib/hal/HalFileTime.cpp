#include "HalFileTime.h"

#include <FS.h>  // must precede SdFat.h (File class compatibility)
#include <SdFat.h>

#include <ctime>

#include "HalClock.h"

namespace {
constexpr int MIN_PLAUSIBLE_YEAR = 2024;

bool currentLocalTime(struct tm& out) {
  if (halClock.localTime(out) && out.tm_year + 1900 >= MIN_PLAUSIBLE_YEAR) return true;
  const time_t now = time(nullptr);
  localtime_r(&now, &out);
  return out.tm_year + 1900 >= MIN_PLAUSIBLE_YEAR;
}

// FsDateTime callback: FAT timestamps are local time by convention.
void fatDateTime(uint16_t* date, uint16_t* time) {
  struct tm t = {};
  if (!currentLocalTime(t)) {
    *date = FS_DATE(2000, 1, 1);
    *time = FS_TIME(0, 0, 0);
    return;
  }
  *date = FS_DATE(static_cast<uint16_t>(t.tm_year + 1900), static_cast<uint8_t>(t.tm_mon + 1),
                  static_cast<uint8_t>(t.tm_mday));
  *time = FS_TIME(static_cast<uint8_t>(t.tm_hour), static_cast<uint8_t>(t.tm_min), static_cast<uint8_t>(t.tm_sec));
}
}  // namespace

void HalFileTime::begin() { FsDateTime::setCallback(fatDateTime); }
