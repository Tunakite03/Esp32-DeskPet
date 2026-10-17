#include "ui.h"

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <WiFi.h>
#include <math.h>
#include "audio.h"
#include "config.h"
#include "layout.h"
#include "log.h"
#include "pet_assets.h"
#include "talk.h"
#include "timekeeping.h"
#include "ui_assets.h"

namespace {
constexpr uint32_t SLEEP_AFTER = 25000;
constexpr uint32_t WAVE_STEP = 350;  // idle pulse of the listening waves
constexpr uint32_t SPINNER_STEP = 80;  // one 30 degree turn of the loading spinner
constexpr int TILE = 16, TILE_COLUMNS = SCREEN_WIDTH / TILE;
constexpr int TILE_ROWS = SCREEN_HEIGHT / TILE;
constexpr int CONTENT_TILE_ROWS = (NAV_LABEL_Y - 2) / TILE;  // everything above the tab labels
constexpr uint32_t WIPE_STEP = 14;  // ms between bands of the page transition

Arduino_ESP32SPI bus(46, 10, 12, 11, 13);  // DC, CS, SCK, MOSI, MISO
Arduino_ILI9341 display(&bus, -1, 1, true);
Arduino_Canvas canvas(SCREEN_WIDTH, SCREEN_HEIGHT, &display);

uint16_t *previousFrame = nullptr;
bool screenPrimed = false;  // previousFrame matches what the panel shows
uint16_t transferStrip[SCREEN_WIDTH * TILE];
uint32_t uiFrames = 0, uiLastMicros = 0, uiMaxMicros = 0;
uint64_t uiTotalMicros = 0, uiPixelsSent = 0;

constexpr uint32_t expressionFrame(PetExpression expression) {
  return REACTION_FRAME_BASE + static_cast<uint8_t>(expression);
}

// Talk mode only drives posed companions that are not busy with a tap reaction.
bool talkAnimating(const AppState &state) {
  return talkPhase() != TalkPhase::Off && !state.pet.reacting && petAssetsReady() &&
         petAsset(state.pet.mascot).kind == PET_ASSET_POSES;
}

// Number of sound-wave arcs: a slow pulse while waiting, the live level while hearing.
uint8_t listenWaves(uint32_t now) {
  const TalkPhase phase = talkPhase();
  if (phase == TalkPhase::Hearing) return max<uint8_t>(1, talkLevel());
  // While waiting, the waves follow the microphone so you can see it hears you;
  // in a silent room they pulse gently instead.
  if (phase == TalkPhase::Listening) return talkLevel() > 0 ? talkLevel() : 1 + (now / WAVE_STEP) % 3;
  return 0;
}

const UiGlyph *findGlyph(const UiFont &font, char character) {
  // Full fonts hold printable ASCII in order, so the glyph can be indexed directly.
  constexpr uint8_t FIRST = 32, PRINTABLE = 95;
  const uint8_t code = static_cast<uint8_t>(character);
  if (font.count == PRINTABLE) {
    if (code < FIRST || code >= FIRST + PRINTABLE) return nullptr;
    const UiGlyph *glyph = &font.glyphs[code - FIRST];
    if (glyph->code == code) return glyph;
  }
  for (uint8_t i = 0; i < font.count; ++i) {
    if (font.glyphs[i].code == static_cast<uint8_t>(character)) return &font.glyphs[i];
  }
  return nullptr;
}

int textWidth(const String &value, const UiFont &font) {
  int width = 0;
  for (size_t i = 0; i < value.length(); ++i) {
    const UiGlyph *glyph = findGlyph(font, value[i]);
    if (glyph) width += glyph->advance;
  }
  return width;
}

// Draws 4-bit anti-aliased text, blending each glyph against a solid background.
void uiText(int x, int y, const String &value, const UiFont &font, uint16_t color,
            uint16_t background = COLOR_BACKGROUND) {
  uint16_t shades[16];
  for (int alpha = 0; alpha < 16; ++alpha) {
    const int red = (((color >> 11) & 31) * alpha + ((background >> 11) & 31) * (15 - alpha)) / 15;
    const int green = (((color >> 5) & 63) * alpha + ((background >> 5) & 63) * (15 - alpha)) / 15;
    const int blue = ((color & 31) * alpha + (background & 31) * (15 - alpha)) / 15;
    shades[alpha] = (red << 11) | (green << 5) | blue;
  }
  uint16_t *pixels = canvas.getFramebuffer();
  for (size_t i = 0; i < value.length(); ++i) {
    const UiGlyph *glyph = findGlyph(font, value[i]);
    if (!glyph) continue;
    for (int row = 0; row < glyph->height; ++row) {
      if (y + row < 0 || y + row >= SCREEN_HEIGHT) continue;
      for (int column = 0; column < glyph->width; ++column) {
        if (x + column < 0 || x + column >= SCREEN_WIDTH) continue;
        const int index = row * glyph->width + column;
        const uint8_t packed = font.pixels[glyph->offset + index / 2];
        const uint8_t alpha = index % 2 ? packed & 15 : packed >> 4;
        if (alpha) pixels[(y + row) * SCREEN_WIDTH + x + column] = shades[alpha];
      }
    }
    x += glyph->advance;
  }
}

void uiCentered(int y, const String &value, const UiFont &font, uint16_t color) {
  uiText((SCREEN_WIDTH - textWidth(value, font)) / 2, y, value, font, color);
}

void uiRightAligned(int y, const String &value, const UiFont &font) {
  uiText(302 - textWidth(value, font), y, value, font, COLOR_TEXT);
}

void header(const char *title) {
  uiText(16, 11, title, FONT_SMALL, COLOR_TEXT);
  const String label = WiFi.status() == WL_CONNECTED ? "CONNECTED" : "OFFLINE";
  const int width = textWidth(label, FONT_SMALL);
  canvas.fillCircle(294 - width, 17, 2, COLOR_TEXT);
  uiText(302 - width, 11, label, FONT_SMALL, COLOR_TEXT);
}

void navIcon(int x, int y, uint8_t page, uint16_t color) {
  if (page == PAGE_PET) {
    canvas.drawRoundRect(x, y + 2, 13, 11, 4, color);
    canvas.drawLine(x, y + 5, x, y - 1, color);
    canvas.drawLine(x, y - 1, x + 5, y + 3, color);
    canvas.drawLine(x + 8, y + 3, x + 12, y - 1, color);
    canvas.drawLine(x + 12, y - 1, x + 12, y + 5, color);
    canvas.drawPixel(x + 4, y + 7, color); canvas.drawPixel(x + 8, y + 7, color);
  } else if (page == PAGE_SETTINGS) {
    canvas.drawCircle(x + 6, y + 6, 5, color);
    canvas.drawCircle(x + 6, y + 6, 2, color);
    canvas.drawFastHLine(x - 2, y + 6, 3, color); canvas.drawFastHLine(x + 11, y + 6, 3, color);
    canvas.drawFastVLine(x + 6, y - 2, 3, color); canvas.drawFastVLine(x + 6, y + 11, 3, color);
  } else if (page == PAGE_CLOCK) {
    canvas.drawCircle(x + 6, y + 7, 6, color);
    canvas.drawLine(x + 6, y + 7, x + 6, y + 3, color);
    canvas.drawLine(x + 6, y + 7, x + 9, y + 8, color);
    canvas.drawFastHLine(x + 3, y - 2, 7, color);
  } else {
    canvas.drawCircle(x + 6, y + 6, 4, color);
    canvas.drawFastHLine(x - 2, y + 6, 3, color); canvas.drawFastHLine(x + 12, y + 6, 3, color);
    canvas.drawFastVLine(x + 6, y - 2, 3, color); canvas.drawFastVLine(x + 6, y + 12, 3, color);
  }
}

void footer(uint8_t activePage) {
  constexpr const char *labels[] = {"Pet", "Clock", "Weather", "Settings"};
  canvas.drawFastHLine(16, NAV_DIVIDER_Y, 288, COLOR_SURFACE);
  for (uint8_t page = 0; page < PAGE_COUNT; ++page) {
    const int tab = page * NAV_TAB_WIDTH;
    const int width = textWidth(labels[page], FONT_SMALL) + 20;
    const int x = tab + (NAV_TAB_WIDTH - width) / 2;
    navIcon(x, NAV_LABEL_Y, page, COLOR_TEXT);
    uiText(x + 20, NAV_LABEL_Y, labels[page], FONT_SMALL, COLOR_TEXT);
    if (page == activePage) canvas.fillRoundRect(tab + 28, 237, 24, 2, 1, COLOR_TEXT);
  }
}

void pill(const Rect &rect, const String &label, uint16_t color) {
  canvas.fillRoundRect(rect.x, rect.y, rect.w, rect.h, 10, color);
  uiText(rect.x + (rect.w - textWidth(label, FONT_BODY)) / 2, rect.y + 7, label, FONT_BODY, COLOR_TEXT, color);
}

uint8_t petBounceStep(const AppState &state, uint32_t now) {
  const PetState &pet = state.pet;
  if (!petAssetsReady() || petAsset(pet.mascot).kind != PET_ASSET_POSES || !pet.reacting ||
      now - pet.reactedAt >= 420) {
    return 0;
  }
  return 1 + (now - pet.reactedAt) / 105;
}

void drawMascotPicker(const AppState &state) {
  header("CHOOSE A COMPANION");
  uiCentered(41, "Pick your companion", FONT_BODY, COLOR_TEXT);
  for (uint8_t slot = 0; slot < PICKER_SLOTS; ++slot) {
    const uint8_t choice = state.pet.pickerPage * PICKER_SLOTS + slot;
    if (choice >= petAssetCount()) continue;
    const Rect card = pickerCard(slot);
    const PetAsset &asset = petAsset(choice);
    const bool selected = choice == state.pet.mascot;
    canvas.drawRoundRect(card.x, card.y, card.w, card.h, 12, selected ? COLOR_SELECTED : COLOR_OUTLINE);
    const uint32_t frame = asset.kind == PET_ASSET_POSES ? POSE_FRAME_FRONT : 0;
    drawPetAsset(canvas.getFramebuffer(), choice, frame, card.x + 5, card.y + 5, card.w - 10, card.w - 10);
    const String label = asset.name;
    uiText(card.x + (card.w - textWidth(label, FONT_SMALL)) / 2, card.y + 95, label, FONT_SMALL, COLOR_TEXT);
    if (selected) uiText(card.x + 23, card.y + 114, "Selected", FONT_SMALL, COLOR_TEXT);
  }
}

void drawPet(const AppState &state, uint32_t now) {
  if (!petAssetsReady()) {
    header("DESK COMPANION");
    uiCentered(75, "Pet assets unavailable", FONT_TITLE, COLOR_TEXT);
    uiCentered(105, "Reload the companion asset pack", FONT_SMALL, COLOR_TEXT);
    return;
  }
  const PetState &pet = state.pet;
  if (pet.pickerOpen) { drawMascotPicker(state); return; }
  const PetAsset &asset = petAsset(pet.mascot);
  const bool posed = asset.kind == PET_ASSET_POSES;
  const bool talking = talkAnimating(state);
  const bool sleeping = posed && !talking && now - state.lastInteraction > SLEEP_AFTER && !pet.reacting;
  header("DESK COMPANION");
  uiText(15, 43, clockText(), FONT_TIME, COLOR_TEXT);
  uiText(18, 108, dateText("%a, %d %b"), FONT_BODY, COLOR_TEXT);
  canvas.drawFastHLine(18, 133, 124, COLOR_SURFACE);
  String name = posed ? pet.name : String(asset.name);
  while (textWidth(name, FONT_TITLE) > 137 && name.length() > 1) name.remove(name.length() - 1);
  uiText(17, 143, name, FONT_TITLE, COLOR_TEXT);
  canvas.fillCircle(21, 175, 2, pet.reacting ? COLOR_PINK : COLOR_TEXT);
  constexpr const char *reactions[] = {"Hello there", "Feeling loved", "You sparkle", "Oh, hello!", "Just for you",
                                       "A little shy", "Dreaming", "Feeling dizzy", "So happy!"};
  constexpr const char *talkMoods[] = {"", "I'm listening", "I hear you...", "Listen to me!"};
  const bool talkMuted = posed && state.device.talk && (state.device.muted || state.device.volume == 0);
  const char *mood = !posed ? "Tap to replay"
                   : talking ? talkMoods[static_cast<uint8_t>(talkPhase())]
                   : sleeping ? "Dreaming"
                   : pet.reacting ? reactions[static_cast<uint8_t>(pet.expression)]
                   : talkMuted ? "Unmute to talk"
                   : "Here with you";
  uiText(30, 168, mood, FONT_SMALL, COLOR_TEXT);
  uiText(18, 187, "Hold pet to change", FONT_SMALL, COLOR_TEXT);
  // Squash and stretch while reacting or speaking; animations always draw at a fixed size.
  uint8_t step = petBounceStep(state, now);
  if (talking && talkPhase() == TalkPhase::Talking && talkLevel() >= 2) step = (now / 120) % 2 ? 3 : 0;
  constexpr int widths[] = {144, 150, 140, 146, 144};
  constexpr int heights[] = {144, 134, 150, 140, 144};
  const int width = posed ? widths[step] : 128;
  const int height = posed ? heights[step] : 128;
  drawPetAsset(canvas.getFramebuffer(), pet.mascot, petVisualFrame(state, now), PET_CENTER_X - width / 2,
               PET_BASELINE_Y - height, width, height);
  if (talking) {
    // "((( " beside the ear: drawn after the sprite so its black background cannot cover them.
    const uint8_t waves = listenWaves(now);
    for (uint8_t wave = 0; wave < waves; ++wave) {
      const int radius = 10 + wave * 8;
      canvas.fillArc(LISTEN_WAVE_X, LISTEN_WAVE_Y, radius, radius - 3, 130, 230, COLOR_SKY);
    }
  }
}

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

void drawSettings(const AppState &state) {
  const DeviceSettings &device = state.device;
  header("DEVICE SETTINGS");
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

void alarmAdjustButton(const Rect &rect, const char *label) {
  canvas.drawRoundRect(rect.x, rect.y, rect.w, rect.h, 9, COLOR_OUTLINE);
  uiText(rect.x + (rect.w - textWidth(label, FONT_BODY)) / 2, rect.y + 7, label, FONT_BODY, COLOR_TEXT);
}

void drawClock(const AppState &state) {
  const AlarmSchedule &alarm = state.alarm;
  const DeviceSettings &device = state.device;
  header("CLOCK & ALARM");
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
  header("ALARM");
  uiCentered(40, clockText(), FONT_TIME, COLOR_TEXT);
  uiCentered(110, "Time to wake up", FONT_TITLE, COLOR_TEXT);
  const char *detail = device.muted || device.volume == 0 ? "Speaker is muted"
                     : !audioIsReady() ? "Speaker unavailable"
                     : "Daily alarm";
  uiCentered(136, detail, FONT_SMALL, COLOR_TEXT);
  pill(STOP_ALARM_BUTTON, "Stop alarm", COLOR_ACCENT);
}

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

// Rotating arc shown while something slow (an HTTPS request) is in progress.
void spinner(int x, int y, uint32_t now) {
  const int start = (now / SPINNER_STEP) % 12 * 30;
  canvas.fillArc(x, y, 7, 4, 0, 360, COLOR_SURFACE);
  const int end = start + 100;
  if (end <= 360) {
    canvas.fillArc(x, y, 7, 4, start, end, COLOR_SKY);
  } else {  // the arc wraps past 0 degrees
    canvas.fillArc(x, y, 7, 4, start, 359.9F, COLOR_SKY);
    canvas.fillArc(x, y, 7, 4, 0, end - 360, COLOR_SKY);
  }
}

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
  uiText(18, 189, "Open-Meteo  /  Tap to refresh", FONT_SMALL, COLOR_TEXT);
}

void drawConfig(const AppState &state) {
  header("MAKE YOURSELF AT HOME");
  uiText(18, 39, "01  Connect your phone", FONT_BODY, COLOR_TEXT);
  uiText(18, 60, state.setup.apName, FONT_TITLE, COLOR_TEXT);
  uiText(18, 89, "PASSWORD", FONT_SMALL, COLOR_TEXT);
  uiText(18, 105, state.setup.apPassword, FONT_TITLE, COLOR_TEXT);
  uiText(18, 137, "02  Open 192.168.4.1", FONT_BODY, COLOR_TEXT);
  pill(SETUP_DONE_BUTTON, "Done, back to my companion", COLOR_ACCENT);
  uiCentered(220, "Wi-Fi, location, name and brightness", FONT_SMALL, COLOR_TEXT);
}

void flushAll() {
  canvas.flush();
  memcpy(previousFrame, canvas.getFramebuffer(), SCREEN_WIDTH * SCREEN_HEIGHT * sizeof(uint16_t));
  uiPixelsSent += SCREEN_WIDTH * SCREEN_HEIGHT;
  screenPrimed = true;
}

// Compares the new frame with what the panel shows in 16x16 tiles and sends
// only runs of changed tiles inside the given tile rows and columns.
void flushTiles(int firstRow, int lastRow, int firstColumn, int lastColumn) {
  uint16_t *pixels = canvas.getFramebuffer();
  for (int tileRow = firstRow; tileRow < lastRow; ++tileRow) {
    const int tileY = tileRow * TILE;
    bool changed[TILE_COLUMNS]{};
    for (int tileX = firstColumn; tileX < lastColumn; ++tileX) {
      for (int row = 0; row < TILE; ++row) {
        const int offset = (tileY + row) * SCREEN_WIDTH + tileX * TILE;
        if (memcmp(pixels + offset, previousFrame + offset, TILE * sizeof(uint16_t)) != 0) {
          changed[tileX] = true;
          break;
        }
      }
    }
    for (int tileX = firstColumn; tileX < lastColumn;) {
      if (!changed[tileX]) { ++tileX; continue; }
      const int first = tileX;
      while (tileX < lastColumn && changed[tileX]) ++tileX;
      const int width = (tileX - first) * TILE;
      for (int row = 0; row < TILE; ++row) {
        const int offset = (tileY + row) * SCREEN_WIDTH + first * TILE;
        memcpy(transferStrip + row * width, pixels + offset, width * sizeof(uint16_t));
        memcpy(previousFrame + offset, pixels + offset, width * sizeof(uint16_t));
      }
      display.startWrite();
      display.writeAddrWindow(first * TILE, tileY, width, TILE);
      display.writePixels(transferStrip, width * TILE);
      display.endWrite();
      uiPixelsSent += width * TILE;
    }
  }
}

void flushChangedTiles() {
  if (!screenPrimed) flushAll();
  else flushTiles(0, TILE_ROWS, 0, TILE_COLUMNS);
}

// Page change: the tab bar updates first so the tap is acknowledged at once,
// then the new page sweeps in as vertical bands from the side it comes from.
// The SPI transfer is the same as a plain redraw; only its order and pacing change.
void flushWithWipe(bool forward) {
  flushTiles(CONTENT_TILE_ROWS, TILE_ROWS, 0, TILE_COLUMNS);
  constexpr int BANDS = 5, BAND_COLUMNS = TILE_COLUMNS / BANDS;
  for (int step = 0; step < BANDS; ++step) {
    const int band = forward ? BANDS - 1 - step : step;
    flushTiles(0, CONTENT_TILE_ROWS, band * BAND_COLUMNS, (band + 1) * BAND_COLUMNS);
    if (step + 1 < BANDS) vTaskDelay(pdMS_TO_TICKS(WIPE_STEP));
  }
}
}  // namespace

bool initDisplay(uint8_t backlight) {
  pinMode(PIN_BACKLIGHT, OUTPUT);
  digitalWrite(PIN_BACKLIGHT, LOW);
  if (!canvas.begin(40000000)) { logError("display", "Display or framebuffer allocation failed"); return false; }
  ledcAttach(PIN_BACKLIGHT, 5000, 8);
  setBacklight(backlight);
  canvas.setTextWrap(false);
  previousFrame = static_cast<uint16_t *>(ps_malloc(SCREEN_WIDTH * SCREEN_HEIGHT * sizeof(uint16_t)));
  if (!previousFrame) { logError("display", "Comparison framebuffer allocation failed"); return false; }
  return true;
}

void showBootProgress(const char *step, uint8_t percent) {
  constexpr Rect track{70, 120, 180, 4};
  canvas.fillScreen(COLOR_BACKGROUND);
  uiCentered(80, "DeskPet", FONT_TITLE, COLOR_TEXT);
  canvas.fillRoundRect(track.x, track.y, track.w, track.h, 2, COLOR_SURFACE);
  const int filled = track.w * min<uint8_t>(percent, 100) / 100;
  if (filled > 0) canvas.fillRoundRect(track.x, track.y, filled, track.h, 2, COLOR_SKY);
  uiCentered(134, step, FONT_SMALL, COLOR_TEXT);
  uiCentered(214, VERSION, FONT_SMALL, COLOR_TEXT);
  flushChangedTiles();
}

void setBacklight(uint8_t level) { ledcWrite(PIN_BACKLIGHT, level); }
uint32_t backlightLevel() { return ledcRead(PIN_BACKLIGHT); }

uint8_t petDirectionAt(int x, int y) {
  const int dx = x - PET_CENTER_X, dy = y - PET_FACE_Y;
  if (dx * dx + dy * dy < 28 * 28) return POSE_FRAME_FRONT;
  // One gaze frame per 45 degree sector around the face, starting to the right.
  constexpr uint8_t directions[] = {5, 8, 7, 6, 3, 0, 1, 2};
  const int sector = (static_cast<int>(roundf(atan2f(dy, dx) / 0.785398163F)) + 8) % 8;
  return directions[sector];
}

uint32_t petVisualFrame(const AppState &state, uint32_t now) {
  const PetState &pet = state.pet;
  if (!petAssetsReady() || pet.mascot >= petAssetCount()) return 0;
  const PetAsset &asset = petAsset(pet.mascot);
  if (asset.kind == PET_ASSET_ANIMATION) return animationFrameAt(asset, now - pet.animationStartedAt);
  if (pet.reacting) {
    // A short blink leads into every reaction except dizziness.
    const bool blink = pet.expression != PetExpression::Dizzy && now - pet.reactedAt < 120;
    return expressionFrame(blink ? PetExpression::Blink : pet.expression);
  }
  if (talkAnimating(state)) {
    switch (talkPhase()) {
      case TalkPhase::Hearing:
        return 3;  // turns towards the sound waves
      case TalkPhase::Talking: {
        // Lip-sync: the louder the syllable, the wider the mouth.
        const uint8_t loudness = talkLevel();
        return loudness >= 2 ? expressionFrame(PetExpression::Joy)
             : loudness == 1 ? expressionFrame(PetExpression::Surprise)
             : POSE_FRAME_FRONT;
      }
      default: {
        if (now % 4800 < 150) return expressionFrame(PetExpression::Blink);
        constexpr uint8_t sway[] = {0, 1, 2, 1};  // a curious head tilt from side to side
        return sway[(now / 700) % 4];
      }
    }
  }
  if (now - state.lastInteraction > SLEEP_AFTER) return expressionFrame(PetExpression::Sleepy);
  if (now % 4800 < 150) return expressionFrame(PetExpression::Blink);
  if (state.touch.down) return pet.direction;
  constexpr uint8_t idleDirections[] = {4, 4, 3, 4, 4, 5};
  return idleDirections[(now / 2800) % 6];
}

void renderFrame(const AppState &state, uint32_t now) {
  const PetState &pet = state.pet;
  const WeatherState &weather = state.weather;
  const DeviceSettings &device = state.device;
  const bool sleeping = now - state.lastInteraction > SLEEP_AFTER && !pet.reacting;
  const bool petVisible = !state.alarmRinging && !state.setup.open && state.page == PAGE_PET && !pet.pickerOpen;
  const uint32_t animation = petVisible ? petVisualFrame(state, now) * 5 + petBounceStep(state, now) : 0;
  // Everything that can change what is on screen. Identical stamps skip the frame.
  const uint32_t stamp[] = {
    state.page, state.setup.open, static_cast<uint32_t>(time(nullptr) / 60), static_cast<uint32_t>(WiFi.status()),
    state.alarm.hour, state.alarm.minute, state.alarm.enabled, state.alarmRinging,
    weather.current.fetchedAt, static_cast<uint32_t>(weather.current.error), weather.busy, weather.valid,
    (now - weather.current.fetchedAt) / 60000,
    pet.tapCount, pet.reacting, sleeping, animation, state.uiRevision,
    device.brightness, device.volume, device.muted, audioIsReady(),
    pet.mascot, pet.pickerOpen, pet.pickerPage, petAssetsReady(), static_cast<uint8_t>(pet.expression),
    device.talk, micReady(), static_cast<uint8_t>(talkPhase()), talkLevel(), petVisible ? listenWaves(now) : 0u,
    weatherSpinnerStep(state, now)
  };
  static uint32_t previousStamp[sizeof(stamp) / sizeof(stamp[0])]{};
  if (uiFrames != 0 && memcmp(stamp, previousStamp, sizeof(stamp)) == 0) return;
  memcpy(previousStamp, stamp, sizeof(stamp));
  const uint32_t started = micros();
  canvas.fillScreen(COLOR_BACKGROUND);
  if (state.alarmRinging) {
    drawRingingAlarm(state);
  } else if (state.setup.open) {
    drawConfig(state);
  } else {
    switch (state.page) {
      case PAGE_PET: drawPet(state, now); break;
      case PAGE_CLOCK: drawClock(state); break;
      case PAGE_WEATHER: drawWeather(state, now); break;
      default: drawSettings(state); break;
    }
    footer(state.page);
  }
  // Pages are 0-3; overlays get their own ids so opening or closing them is not a page change.
  const uint8_t screen = state.alarmRinging ? 0xA0 : state.setup.open ? 0xA1 : state.page;
  static uint8_t shownScreen = 0xFF;
  if (screenPrimed && shownScreen < PAGE_COUNT && screen < PAGE_COUNT && screen != shownScreen) {
    flushWithWipe((screen - shownScreen + PAGE_COUNT) % PAGE_COUNT <= PAGE_COUNT / 2);
  } else {
    flushChangedTiles();
  }
  shownScreen = screen;
  uiLastMicros = micros() - started;
  uiMaxMicros = max(uiMaxMicros, uiLastMicros);
  uiTotalMicros += uiLastMicros;
  ++uiFrames;
}

void printUiStats() {
  Serial.printf("{\"ui_frames\":%lu,\"render_last_us\":%lu,\"render_avg_us\":%llu,\"render_max_us\":%lu,"
                "\"pixels_sent\":%llu,\"full_frame_pixels\":%llu}\n",
                static_cast<unsigned long>(uiFrames), static_cast<unsigned long>(uiLastMicros),
                uiFrames ? uiTotalMicros / uiFrames : 0ULL, static_cast<unsigned long>(uiMaxMicros), uiPixelsSent,
                static_cast<uint64_t>(uiFrames) * SCREEN_WIDTH * SCREEN_HEIGHT);
}

void writeSnapshot() {
  Serial.println("FRAME 320 240");
  Serial.write(reinterpret_cast<const uint8_t *>(canvas.getFramebuffer()),
               SCREEN_WIDTH * SCREEN_HEIGHT * sizeof(uint16_t));
  Serial.println("FRAME END");
}
