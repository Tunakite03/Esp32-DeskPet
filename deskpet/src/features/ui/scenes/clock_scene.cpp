#include "src/features/ui/scenes/scenes.h"

#include "src/core/audio/audio.h"
#include "src/core/system/timekeeping.h"
#include "src/features/ui/ui_draw.h"

namespace ui {
namespace {
void alarmAdjustButton(const Rect &rect, const char *label) {
  canvas.drawRoundRect(rect.x, rect.y, rect.w, rect.h, 9, COLOR_OUTLINE);
  uiText(rect.x + (rect.w - textWidth(label, FONT_BODY)) / 2, rect.y + 7, label, FONT_BODY, COLOR_TEXT);
}
}  // namespace

void drawClock(const AppState &state) {
  const AlarmSchedule &alarm = state.alarm;
  const DeviceSettings &device = state.device;
  header("CLOCK & ALARM", state.battery);
  uiCentered(31, clockText(), FONT_VALUE, COLOR_TEXT);
  uiCentered(80, dateText("%a, %d %b  /  UTC+7"), FONT_SMALL, COLOR_TEXT);
  canvas.drawFastHLine(18, 99, 284, COLOR_SURFACE);
  uiText(18, 106, "DAILY ALARM", FONT_SMALL, COLOR_TEXT);
  const char *status = !clockReady() ? "Waiting for time"
                     : !alarm.enabled ? "Off"
                     : !audioIsReady() ? "Speaker unavailable"
                     : device.muted || device.volume == 0 ? "On / muted"
                     : "On";
  uiRightAligned(106, status, FONT_SMALL);
  char hours[3], minutes[3];
  snprintf(hours, sizeof(hours), "%02u", alarm.hour);
  snprintf(minutes, sizeof(minutes), "%02u", alarm.minute);
  uiText(62, 124, hours, FONT_VALUE, COLOR_TEXT);
  uiText(214, 124, minutes, FONT_VALUE, COLOR_TEXT);
  uiText(155, 131, ":", FONT_TITLE, COLOR_TEXT);
  alarmAdjustButton(ALARM_HOUR_DOWN, "-");
  alarmAdjustButton(ALARM_HOUR_UP, "+");
  alarmAdjustButton(ALARM_MINUTE_DOWN, "-");
  alarmAdjustButton(ALARM_MINUTE_UP, "+");
  pill(ALARM_TOGGLE_BUTTON, alarm.enabled ? "Disable alarm" : "Enable alarm",
       alarm.enabled ? COLOR_ACCENT : COLOR_SURFACE);
  pill(TEST_BELL_BUTTON, "Test bell", COLOR_SURFACE);
}

void drawRingingAlarm(const AppState &state) {
  const DeviceSettings &device = state.device;
  header("ALARM", state.battery);
  uiCentered(40, clockText(), FONT_TIME, COLOR_TEXT);
  uiCentered(110, "Time to wake up", FONT_TITLE, COLOR_TEXT);
  const char *detail = device.muted || device.volume == 0 ? "Speaker is muted"
                     : !audioIsReady() ? "Speaker unavailable"
                     : "Daily alarm";
  uiCentered(136, detail, FONT_SMALL, COLOR_TEXT);
  pill(STOP_ALARM_BUTTON, "Stop alarm", COLOR_ACCENT);
}
}  // namespace ui
