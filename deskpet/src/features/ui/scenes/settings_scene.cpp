#include "src/features/ui/scenes/scenes.h"

#include "src/core/audio/audio.h"
#include "src/features/ui/ui_draw.h"

namespace ui {
namespace {
void settingSlider(const Slider &slider, int percent, uint16_t accent) {
  const Rect minus = slider.minus(), plus = slider.plus(), track = slider.track();
  const int y = slider.centerY;
  canvas.drawRoundRect(minus.x, minus.y, minus.w, minus.h, 9, COLOR_OUTLINE);
  canvas.drawFastHLine(minus.x + 10, y, 12, COLOR_TEXT);
  canvas.drawRoundRect(plus.x, plus.y, plus.w, plus.h, 9, COLOR_OUTLINE);
  canvas.drawFastHLine(plus.x + 10, y, 12, COLOR_TEXT);
  canvas.drawFastVLine(plus.x + 16, y - 6, 13, COLOR_TEXT);
  canvas.fillRoundRect(track.x, track.y, track.w, track.h, 2, COLOR_OUTLINE);
  const int extent = percent * track.w / 100;
  if (extent > 0) canvas.fillRoundRect(track.x, track.y, extent, track.h, 2, accent);
  canvas.fillCircle(track.x + extent, y, 6, COLOR_TEXT);
}
}  // namespace

void drawSettings(const AppState &state) {
  const DeviceSettings &device = state.device;
  const String title = "DEVICE SETTINGS";
  header(title.c_str());
  // Firmware version as an outlined badge in the free space of the header, after the title.
  const String version = String("v") + VERSION_NUMBER;
  const int versionX = 16 + textWidth(title, FONT_SMALL) + 14;
  canvas.drawRoundRect(versionX - 6, 8, textWidth(version, FONT_SMALL) + 12, 20, 6, COLOR_OUTLINE);
  uiText(versionX, 11, version, FONT_SMALL, COLOR_TEXT);
  uiText(18, 38, "Brightness", FONT_TITLE, COLOR_TEXT);
  uiRightAligned(41, String(brightnessPercent(device.brightness)) + "%", FONT_BODY);
  settingSlider(BRIGHTNESS_SLIDER, brightnessPercent(device.brightness), COLOR_PEACH);
  uiText(18, 106, "Speaker", FONT_TITLE, COLOR_TEXT);
  const String sound = !audioIsReady() ? "Unavailable" : device.muted ? "Muted" : String(device.volume) + "%";
  uiRightAligned(109, sound, FONT_BODY);
  settingSlider(VOLUME_SLIDER, device.muted ? 0 : device.volume, COLOR_SKY);
  pill(MUTE_BUTTON, device.muted ? "Unmute" : "Mute", COLOR_SURFACE);
  pill(TEST_SOUND_BUTTON, "Test", COLOR_SURFACE);
  const char *talkLabel = !micReady() ? "No mic" : device.talk ? "Talk on" : "Talk off";
  pill(TALK_BUTTON, talkLabel, device.talk && micReady() ? COLOR_ACCENT : COLOR_SURFACE);
}
}  // namespace ui
