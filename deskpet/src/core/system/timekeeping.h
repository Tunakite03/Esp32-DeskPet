#pragma once
#include <Arduino.h>
#include <time.h>

// 2024-01-01 UTC. Anything earlier means the clock was never synchronised.
constexpr time_t CLOCK_VALID_AFTER = 1704067200;

inline bool clockReady() { return time(nullptr) > CLOCK_VALID_AFTER; }

inline String dateText(const char *format) {
  if (!clockReady()) return "Set time in setup";
  const time_t epoch = time(nullptr);
  tm local{};
  localtime_r(&epoch, &local);
  char value[32];
  strftime(value, sizeof(value), format, &local);
  return String(value);
}

inline String clockText() { return clockReady() ? dateText("%H:%M") : String("--:--"); }
