#include "src/features/ui/ui.h"

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <WiFi.h>
#include "src/core/audio/audio.h"
#include "src/core/system/config.h"
#include "src/core/system/log.h"
#include "src/features/music/music.h"
#include "src/features/pet/pet_assets.h"
#include "src/features/talk/talk.h"
#include "src/features/ui/layout.h"
#include "src/features/ui/scenes/scenes.h"
#include "src/features/ui/ui_draw.h"

// Frame pipeline: decides when to redraw, asks the scene for the current screen
// to paint the canvas, and sends only what changed to the panel.
namespace {
constexpr int TILE = 16, TILE_COLUMNS = SCREEN_WIDTH / TILE;
constexpr int TILE_ROWS = SCREEN_HEIGHT / TILE;
// The panel is updated a few tile rows at a time, see pumpDisplay().

Arduino_ESP32SPI bus(46, 10, 12, 11, 13);  // DC, CS, SCK, MOSI, MISO
Arduino_ILI9341 display(&bus, -1, 1, true);
}  // namespace

namespace ui {
Arduino_Canvas canvas(SCREEN_WIDTH, SCREEN_HEIGHT, &display);
}  // namespace ui

using namespace ui;

namespace {

uint16_t *previousFrame = nullptr;
bool screenPrimed = false;  // previousFrame matches what the panel shows
uint16_t transferStrip[SCREEN_WIDTH * TILE];
uint32_t uiFrames = 0, uiLastMicros = 0, uiMaxMicros = 0;
uint64_t uiTotalMicros = 0, uiPixelsSent = 0;
RenderTiming lastTiming;
int transferRow = TILE_ROWS;  // tile rows of the last drawn frame that are still to be sent

// The tab highlight slides to a newly chosen tab, easing out over NAV_SLIDE_MS.
constexpr uint32_t NAV_SLIDE_MS = 180;
uint8_t navPage = PAGE_PET;
Rect navFrom{};  // set when a slide starts
uint32_t navChangedAt = 0;

Rect navPillAt(uint32_t now) {
  const Rect to = navPill(navPage);
  const uint32_t elapsed = now - navChangedAt;
  if (elapsed >= NAV_SLIDE_MS) return to;
  const float t = static_cast<float>(elapsed) / NAV_SLIDE_MS;
  const float eased = 1 - (1 - t) * (1 - t) * (1 - t);
  const auto mix = [eased](int from, int target) { return from + static_cast<int>(lroundf((target - from) * eased)); };
  return {mix(navFrom.x, to.x), to.y, mix(navFrom.w, to.w), to.h};
}

// Starts a slide when the page changed; a slide already under way continues from where it is.
void trackNavPage(uint8_t page, uint32_t now) {
  if (page == navPage) return;
  navFrom = navPillAt(now);
  navPage = page;
  navChangedAt = now;
}

// Changes every 10 ms while the highlight slides, so those frames are not skipped.
uint32_t navSlideStep(uint32_t now) {
  const uint32_t elapsed = now - navChangedAt;
  return elapsed < NAV_SLIDE_MS ? 1 + elapsed / 10 : 0;
}

void flushAll() {
  canvas.flush();
  memcpy(previousFrame, canvas.getFramebuffer(), SCREEN_WIDTH * SCREEN_HEIGHT * sizeof(uint16_t));
  uiPixelsSent += SCREEN_WIDTH * SCREEN_HEIGHT;
  screenPrimed = true;
}

// Compares one row of 16-pixel tiles with what the panel shows and sends only the
// runs of tiles that changed.
void flushTileRow(int tileRow) {
  uint16_t *pixels = canvas.getFramebuffer();
  const int tileY = tileRow * TILE;
  bool changed[TILE_COLUMNS]{};
  for (int tileX = 0; tileX < TILE_COLUMNS; ++tileX) {
    for (int row = 0; row < TILE; ++row) {
      const int offset = (tileY + row) * SCREEN_WIDTH + tileX * TILE;
      if (memcmp(pixels + offset, previousFrame + offset, TILE * sizeof(uint16_t)) != 0) {
        changed[tileX] = true;
        break;
      }
    }
  }
  for (int tileX = 0; tileX < TILE_COLUMNS;) {
    if (!changed[tileX]) { ++tileX; continue; }
    const int first = tileX;
    while (tileX < TILE_COLUMNS && changed[tileX]) ++tileX;
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

// Sends the whole screen in one go; only used by the boot splash.
void flushChangedTiles() {
  if (!screenPrimed) {
    flushAll();
    return;
  }
  for (int tileRow = 0; tileRow < TILE_ROWS; ++tileRow) flushTileRow(tileRow);
}
}  // namespace

// Sends the next tile rows of the last drawn frame, for about budgetUs at most.
// The main loop calls this every pass, so a page change never holds up input.
void pumpDisplay(uint32_t budgetUs) {
  if (transferRow >= TILE_ROWS) return;
  if (!screenPrimed) {
    flushAll();
    transferRow = TILE_ROWS;
    return;
  }
  const uint32_t started = micros();
  while (transferRow < TILE_ROWS && micros() - started < budgetUs) flushTileRow(transferRow++);
}

bool displayTransferPending() { return transferRow < TILE_ROWS; }

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

void renderFrame(const AppState &state, uint32_t now) {
  const PetState &pet = state.pet;
  const WeatherState &weather = state.weather;
  const DeviceSettings &device = state.device;
  const bool sleeping = now - state.lastInteraction > PET_SLEEP_AFTER && !pet.reacting;
  const bool petVisible = !state.alarmRinging && !state.setup.open && state.page == PAGE_PET && !pet.pickerOpen;
  const bool musicVisible = !state.alarmRinging && !state.setup.open && state.page == PAGE_MUSIC;
  const MusicStatus music = musicVisible ? musicStatus() : MusicStatus{};
  const uint32_t animation = petVisible ? petVisualFrame(state, now) * 5 + petBounceStep(state, now) : 0;
  trackNavPage(state.page, now);
  // Everything that can change what is on screen. Identical stamps skip the frame.
  const uint32_t stamp[] = {
    state.page, state.setup.open, static_cast<uint32_t>(time(nullptr) / 60), static_cast<uint32_t>(WiFi.status()),
    state.alarm.hour, state.alarm.minute, state.alarm.enabled, state.alarmRinging,
    weather.current.fetchedAt, static_cast<uint32_t>(weather.current.error), weather.busy, weather.valid,
    (now - weather.current.fetchedAt) / 60000,
    pet.tapCount, pet.reacting, sleeping, animation, state.uiRevision, state.battery.present, state.battery.percent, state.battery.charging,
    device.brightness, device.volume, device.muted, audioIsReady(),
    pet.mascot, pet.pickerOpen, pet.pickerPage, petAssetsReady(), static_cast<uint8_t>(pet.expression),
    device.talk, micReady(), static_cast<uint8_t>(talkPhase()), talkLevel(), petVisible ? listenWaves(now) : 0u,
    weatherSpinnerStep(state, now), navSlideStep(now), state.musicLibraryOpen, state.musicLibraryPage,
    music.ready, static_cast<uint8_t>(music.phase), static_cast<uint8_t>(music.error), music.revision,
    music.count, music.index, music.elapsedMs / 1000, music.progress
  };
  static uint32_t previousStamp[sizeof(stamp) / sizeof(stamp[0])]{};
  lastTiming = RenderTiming{};
  if (uiFrames != 0 && memcmp(stamp, previousStamp, sizeof(stamp)) == 0) return;
  memcpy(previousStamp, stamp, sizeof(stamp));
  const uint32_t started = micros();
  takePetDecodeMicros();  // count only the decodes this frame causes
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
      case PAGE_MUSIC: drawMusic(state, music); break;
      default: drawSettings(state); break;
    }
    footer(navPillAt(now));
  }
  // The changed tiles are sent by pumpDisplay() over the next loops, not in this call.
  transferRow = 0;
  const uint32_t drawn = micros();
  lastTiming = {true, drawn - started, takePetDecodeMicros()};
  uiLastMicros = drawn - started;
  uiMaxMicros = max(uiMaxMicros, uiLastMicros);
  uiTotalMicros += uiLastMicros;
  ++uiFrames;
}

RenderTiming lastRenderTiming() { return lastTiming; }

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
