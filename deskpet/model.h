#pragma once
#include <stdint.h>

struct AlarmSchedule {
  uint8_t hour = 7;
  uint8_t minute = 0;
  bool enabled = false;
  uint32_t lastFiredDay = 0;
};

inline bool alarmDue(const AlarmSchedule &alarm, uint32_t day, int hour, int minute) {
  return alarm.enabled && day != 0 && day != alarm.lastFiredDay &&
         hour == alarm.hour && minute == alarm.minute;
}

inline bool alarmWithinRingWindow(uint32_t startedAt, uint32_t now) {
  return now - startedAt < 120000UL;
}

inline int brightnessPercent(uint8_t level) { return (level * 100 + 127) / 255; }

inline int swipeDirection(int startX, int startY, int endX, int endY) {
  const int dx = endX - startX;
  const int dy = endY - startY;
  const int vertical = dy < 0 ? -dy : dy;
  if (dx > 55 && dx > vertical * 2) return -1;
  if (dx < -55 && -dx > vertical * 2) return 1;
  return 0;
}

inline bool modelSelfTest() {
  AlarmSchedule alarm{7, 30, true, 0};
  if (!alarmDue(alarm, 20261016, 7, 30)) return false;
  if (alarmDue(alarm, 20261016, 7, 29) || alarmDue(alarm, 20261016, 8, 30)) return false;
  alarm.lastFiredDay = 20261016;
  if (alarmDue(alarm, 20261016, 7, 30)) return false;
  if (!alarmDue(alarm, 20261017, 7, 30)) return false;
  alarm.enabled = false;
  if (alarmDue(alarm, 20261017, 7, 30)) return false;
  alarm = {0, 0, true, 20261231};
  if (!alarmDue(alarm, 20270101, 0, 0) || alarmDue(alarm, 0, 0, 0)) return false;
  if (!alarmWithinRingWindow(1000, 120999) || alarmWithinRingWindow(1000, 121000)) return false;
  if (!alarmWithinRingWindow(UINT32_MAX - 99, 100)) return false;
  if (swipeDirection(250, 100, 100, 110) != 1) return false;
  if (swipeDirection(100, 100, 250, 110) != -1) return false;
  return swipeDirection(100, 100, 110, 220) == 0;
}
