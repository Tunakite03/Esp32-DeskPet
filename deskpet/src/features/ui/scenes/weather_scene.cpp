#include "src/features/ui/scenes/scenes.h"

#include <WiFi.h>
#include "src/features/ui/ui_draw.h"

namespace ui {
namespace {
bool isSnow(int code) { return (code >= 71 && code <= 77) || code == 85 || code == 86; }

const char *weatherLabel(int code) {
  if (code == 0) return "Clear skies";
  if (code <= 3) return "Cloudy skies";
  if (code == 45 || code == 48) return "Foggy";
  if (code >= 95) return "Thunderstorms";
  if (isSnow(code)) return "Snow";
  return "Rain / drizzle";
}

void weatherIcon(int x, int y, int code) {
  canvas.fillCircle(x, y, 17, COLOR_PEACH);
  for (int side : {-1, 1}) {
    canvas.fillRoundRect(x + side * 25 - 2, y - 1, 5, 3, 1, COLOR_PEACH);
    canvas.fillRoundRect(x - 1, y + side * 25 - 2, 3, 5, 1, COLOR_PEACH);
  }
  if (code != 0) {
    canvas.fillCircle(x + 4, y + 15, 15, COLOR_CLOUD);
    canvas.fillCircle(x + 22, y + 18, 11, COLOR_CLOUD);
    canvas.fillRoundRect(x - 12, y + 16, 46, 14, 7, COLOR_CLOUD);
  }
  if (isSnow(code)) {
    for (int flake = 0; flake < 3; ++flake) {
      const int center = x - 3 + flake * 12;
      canvas.drawFastHLine(center - 3, y + 37, 7, COLOR_SKY);
      canvas.drawFastVLine(center, y + 34, 7, COLOR_TEXT);
    }
  } else if (code >= 51 && code <= 99) {
    for (int drop = 0; drop < 3; ++drop) canvas.fillRoundRect(x - 5 + drop * 12, y + 34, 3, 7, 1, COLOR_SKY);
    if (code >= 95) canvas.fillTriangle(x + 10, y + 26, x + 19, y + 26, x + 9, y + 42, COLOR_PEACH);
  }
}
}  // namespace

uint32_t weatherSpinnerStep(const AppState &state, uint32_t now) {
  const bool visible = state.weather.busy && state.page == PAGE_WEATHER && !state.alarmRinging && !state.setup.open;
  return visible ? 1 + (now / SPINNER_STEP) % 12 : 0;
}

void drawWeather(const AppState &state, uint32_t now) {
  const WeatherState &weather = state.weather;
  const WeatherResult &current = weather.current;
  const bool online = WiFi.status() == WL_CONNECTED;
  header("A LOOK OUTSIDE");
  uiText(18, 39, PLACES[weather.place].name, FONT_TITLE, COLOR_TEXT);
  if (weather.valid) {
    const String temperature = String(current.temperature, 1);
    uiText(17, 76, temperature, FONT_VALUE, COLOR_TEXT);
    const int right = 20 + textWidth(temperature, FONT_VALUE);
    canvas.drawCircle(right + 3, 83, 2, COLOR_TEXT);
    uiText(right + 10, 80, "C", FONT_TITLE, COLOR_TEXT);
    weatherIcon(260, 93, current.code);
    uiText(19, 130, weatherLabel(current.code), FONT_BODY, COLOR_TEXT);
    canvas.drawFastHLine(18, 155, 284, COLOR_SURFACE);
    uiText(18, 168, "HUMIDITY", FONT_SMALL, COLOR_TEXT);
    uiText(85, 166, String(current.humidity) + "%", FONT_BODY, COLOR_TEXT);
    const String age = weather.busy ? "Updating..."
                     : current.error != WeatherError::None ? "Retry pending"
                     : String((now - current.fetchedAt) / 60000) + " min ago";
    uiRightAligned(168, age, FONT_SMALL);
    if (weather.busy) spinner(302 - textWidth(age, FONT_SMALL) - 13, 174, now);
  } else {
    weatherIcon(270, 100, 3);
    const char *message = weather.busy ? "Checking the sky" : online ? "Weather pending" : "Waiting for Wi-Fi";
    uiText(18, 87, message, FONT_TITLE, COLOR_TEXT);
    if (weather.busy) spinner(18 + textWidth(message, FONT_TITLE) + 14, 99, now);
    uiText(18, 116, online ? "Tap to try again" : "Hold BOOT to connect", FONT_BODY, COLOR_TEXT);
    if (current.error == WeatherError::Clock) uiText(18, 145, "Waiting for clock sync", FONT_SMALL, COLOR_TEXT);
  }
  uiText(18, 189, "Tap to refresh", FONT_SMALL, COLOR_TEXT);
}
}  // namespace ui
