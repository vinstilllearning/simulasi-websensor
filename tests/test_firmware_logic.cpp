// Regression tests of the same schedule_logic.h compiled into the firmware.
// Run: c++ -std=c++17 -Wall -Wextra -pedantic tests/test_firmware_logic.cpp -o /tmp/luma-schedule-test && /tmp/luma-schedule-test
#include "../firmware/LampuHiasPintar/schedule_logic.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

static unsigned checks = 0;

static void expect(uint64_t epoch, bool clock, const luma::ScheduleSettings &settings,
                   bool open, float fade = -1.0f) {
  const auto result = luma::evaluateSchedule(epoch, clock, settings);
  assert(result.open == open);
  if (fade >= 0) assert(fabsf(result.wakeFactor - fade) < 0.00001f);
  ++checks;
}

int main() {
  // 2026-11-01 00:00 UTC: Sunday, verified fixed Unix epoch.
  constexpr uint64_t sunday = 1793491200ULL;
  luma::ScheduleSettings schedule{false, 1080, 1380, 127, 0, 10};
  expect(0, false, schedule, true, 1.0f); // Disabled never needs a clock.
  schedule.enabled = true;
  expect(sunday + 19 * 3600, false, schedule, false);
  expect(sunday + 18 * 3600 - 1, true, schedule, false);
  expect(sunday + 18 * 3600, true, schedule, true, 0.0f);
  expect(sunday + 18 * 3600 + 300, true, schedule, true, 0.5f);
  expect(sunday + 18 * 3600 + 600, true, schedule, true, 1.0f);
  expect(sunday + 23 * 3600 - 1, true, schedule, true, 1.0f);
  expect(sunday + 23 * 3600, true, schedule, false); // End exclusive.

  schedule.days = 1; // Sunday only.
  schedule.wakeMinutes = 0;
  expect(sunday + 18 * 3600, true, schedule, true, 1.0f);
  expect(sunday + 86400 + 18 * 3600, true, schedule, false);
  schedule.startMinute = 22 * 60;
  schedule.endMinute = 2 * 60;
  expect(sunday + 22 * 3600 - 1, true, schedule, false);
  expect(sunday + 22 * 3600, true, schedule, true, 1.0f);
  expect(sunday + 86400 + 3600, true, schedule, true, 1.0f);
  expect(sunday + 86400 + 2 * 3600, true, schedule, false);
  expect(sunday + 86400 + 23 * 3600, true, schedule, false);
  // Sunday's early morning belongs to Saturday, which isn't selected.
  expect(sunday + 3600, true, schedule, false);
  schedule.days = 64;
  expect(sunday + 3600, true, schedule, true, 1.0f);

  // Local UTC+7 Sunday 22:00 is Sunday 15:00 UTC.
  schedule.days = 1;
  schedule.utcOffsetMinutes = 420;
  expect(sunday + 15 * 3600, true, schedule, true, 1.0f);
  expect(sunday + 18 * 3600, true, schedule, true, 1.0f); // Monday01:00 local.
  expect(sunday + 19 * 3600, true, schedule, false); // Monday02:00 local.

  // UTC-12 Sunday 22:00 is Monday10:00 UTC; +14 crosses backwards.
  schedule.utcOffsetMinutes = -720;
  expect(sunday + 86400 + 10 * 3600, true, schedule, true, 1.0f);
  schedule.utcOffsetMinutes = 840;
  expect(sunday + 8 * 3600, true, schedule, true, 1.0f);
  // Wake fade across midnight, with selected Sunday start-day ownership.
  schedule.utcOffsetMinutes = 0;
  schedule.startMinute = 23 * 60 + 45;
  schedule.wakeMinutes = 60;
  expect(sunday + 86400 + 15 * 60, true, schedule, true, 0.5f);
  expect(sunday + 86400 + 45 * 60, true, schedule, true, 1.0f);

  // Every selected weekday is matched to its correct bit.
  schedule.startMinute = 18 * 60;
  schedule.endMinute = 23 * 60;
  schedule.wakeMinutes = 0;
  for (unsigned selected = 0; selected < 7; ++selected) {
    schedule.days = 1U << selected;
    for (unsigned actual = 0; actual < 7; ++actual)
      expect(sunday + actual * 86400ULL + 19 * 3600, true, schedule, actual == selected);
  }
  schedule.days = 127;
  schedule.startMinute = schedule.endMinute;
  expect(sunday, true, schedule, false);
  schedule.startMinute = 1440;
  expect(sunday, true, schedule, false);
  schedule.startMinute = 1;
  schedule.days = 0;
  expect(sunday, true, schedule, false);
  printf("PASS: %u native firmware scheduler assertions\n", checks);
}
