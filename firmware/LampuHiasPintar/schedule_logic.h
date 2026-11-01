#pragma once

// Pure scheduling logic shared by the ESP32 runtime and native regression tests.
// UTC input is Unix seconds; weekdays follow bit 0 Sunday through bit 6 Saturday.
#include <stdint.h>

namespace luma {

struct ScheduleSettings {
  bool enabled;
  uint16_t startMinute;
  uint16_t endMinute;
  uint8_t days;
  int16_t utcOffsetMinutes;
  uint8_t wakeMinutes;
};

struct ScheduleResult {
  bool open;
  float wakeFactor;
};

inline ScheduleResult evaluateSchedule(uint64_t epoch, bool clockValid,
                                       const ScheduleSettings &settings) {
  if (!settings.enabled) return {true, 1.0f};
  if (!clockValid) return {false, 1.0f};
  // These settings are also validated at the JSON boundary; keep this pure
  // helper fail-closed if invoked independently with malformed parameters.
  if (settings.startMinute >= 1440 || settings.endMinute >= 1440 ||
      settings.startMinute == settings.endMinute || settings.days == 0 ||
      settings.days > 127) return {false, 1.0f};

  const int64_t localSeconds = int64_t(epoch) + int64_t(settings.utcOffsetMinutes) * 60;
  // Floor division makes the weekday calculation correct before Unix epoch,
  // too; production clock inputs are restricted to 2024..2099.
  int64_t dayNumber = localSeconds / 86400;
  int64_t seconds = localSeconds % 86400;
  if (seconds < 0) { seconds += 86400; --dayNumber; }
  int64_t weekday = (dayNumber + 4) % 7; // 1970-01-01 was Thursday.
  if (weekday < 0) weekday += 7;
  const uint16_t minute = uint16_t(seconds / 60);
  uint32_t sinceStartSec;

  if (settings.startMinute < settings.endMinute) {
    if (minute < settings.startMinute || minute >= settings.endMinute)
      return {false, 1.0f};
    sinceStartSec = uint32_t(seconds) - uint32_t(settings.startMinute) * 60;
  } else if (minute >= settings.startMinute) {
    sinceStartSec = uint32_t(seconds) - uint32_t(settings.startMinute) * 60;
  } else if (minute < settings.endMinute) {
    // After midnight the selected weekday belongs to the prior start day.
    weekday = (weekday + 6) % 7;
    sinceStartSec = uint32_t(seconds) + 86400U - uint32_t(settings.startMinute) * 60;
  } else return {false, 1.0f};

  if (!(settings.days & (1U << weekday))) return {false, 1.0f};
  float fade = 1.0f;
  if (settings.wakeMinutes) {
    fade = float(sinceStartSec) / (uint32_t(settings.wakeMinutes) * 60);
    if (fade > 1.0f) fade = 1.0f;
  }
  return {true, fade};
}

} // namespace luma
