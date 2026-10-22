#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <time.h>
#include "src/core/audio/audio.h"
#include "src/core/power/battery.h"
#include "src/core/storage/sd_card.h"
#include "src/core/storage/sd_upload.h"
#include "src/core/system/config.h"
#include "src/features/ui/layout.h"
#include "src/core/system/log.h"
#include "src/app/model.h"
#include "src/app/serial_console.h"
#include "src/app/settings_store.h"
#include "src/features/pet/pet_assets.h"
#include "src/features/music/music.h"
#include "src/features/setup/setup_portal.h"
#include "src/app/state.h"
#include "src/features/talk/talk.h"
#include "src/core/system/timekeeping.h"
#include "src/core/input/touch_input.h"
#include "src/features/ui/ui.h"
#include "src/features/weather/weather.h"

namespace {
constexpr uint32_t WEATHER_INTERVAL = 15UL * 60000UL;
constexpr uint32_t WEATHER_RETRY_INTERVAL = 60000UL;
// Battery voltage moves slowly; a sample every 30 s keeps the header current without extra redraws.
constexpr uint32_t BATTERY_INTERVAL = 30000UL;
// DHCP and DNS are often not ready the moment Wi-Fi reports a connection.
constexpr uint32_t NETWORK_SETTLE = 3000;
constexpr uint32_t REACTION_DURATION = 1100;
constexpr uint32_t DIM_AFTER = 60000;
constexpr uint32_t FRAME_INTERVAL = 50;
constexpr uint32_t LONG_PRESS = 650;
constexpr int LONG_PRESS_SLOP = 15, TAP_SLOP = 25;

AppState state;
NetworkCredentials network;

// Bookkeeping that only input handling and the main loop need.
uint32_t alarmStartedAt = 0, lastAlarmCheck = 0;
uint8_t petReactionIndex = 0, rapidPetTaps = 0;
uint32_t lastPetTapAt = 0, lastFrame = 0, lastWeatherAttempt = 0, lastBatteryRead = 0;
uint32_t buttonAt = 0, lastReconnect = 0;
bool buttonHeld = false, buttonWasDown = false, everConnected = false;
bool fatalError = false, setupOffered = false;
uint32_t pendingConnectAt = 0;
// Set by any input so the next frame is drawn immediately instead of on the 50 ms tick.
bool inputPending = false;

// Worst-case timings since the last "perf" command, to find what makes input feel slow.
struct Perf {
  uint32_t loops = 0, renders = 0;
  uint32_t maxLoopUs = 0, maxRenderUs = 0, maxPumpUs = 0, maxSaveUs = 0, maxInputLatencyMs = 0;
  uint64_t totalRenderUs = 0;
};
Perf perf;
// Frames or input handling slower than this print a "slow_frame" event with its breakdown.
constexpr uint32_t SLOW_FRAME_US = 40000;
// Tile rows the display may send in one pass of the main loop.
constexpr uint32_t DISPLAY_SLICE_US = 6000;

bool petPageInteractive() {
  return !state.alarmRinging && !state.setup.open && state.page == PAGE_PET && !state.pet.pickerOpen;
}

void requestWeather(uint32_t now) {
  if (state.weather.busy || WiFi.status() != WL_CONNECTED) return;
  if (requestWeatherFetch(state.weather.place)) {
    state.weather.busy = true;
    lastWeatherAttempt = now;
  }
}

// Applies a form the setup portal has already saved to flash.
void applySetupForm(const SetupForm &form) {
  if (!form.ssid.isEmpty()) {
    network = {form.ssid, form.password};
    pendingConnectAt = millis() + 1000;
  }
  state.pet.name = form.petName;
  state.weather.place = form.place;
  state.device.brightness = form.brightness;
  ++state.uiRevision;
  state.weather.valid = false;
  lastWeatherAttempt = 0;
}

// The portal offers itself once after boot; opening it any other way counts as that offer.
void openSetup() {
  if (openSetupPortal()) setupOffered = true;
}

// ---------------------------------------------------------------------------
// Device settings and alarm

void markDeviceSettingsChanged(uint32_t now) {
  scheduleDeviceSettingsSave(now);
  state.lastInteraction = now;
  ++state.uiRevision;
  setAudioGain(state.device.muted ? 0 : state.device.volume);
  setBacklight(state.device.brightness);
}

void handleSettingsTap(int x, int y, uint32_t now) {
  DeviceSettings &device = state.device;
  if (BRIGHTNESS_SLIDER.row().contains(x, y)) {
    const int target = BRIGHTNESS_SLIDER.target(x, brightnessPercent(device.brightness));
    const uint8_t next = (constrain(target, 1, 100) * 255 + 50) / 100;
    if (next != device.brightness) { device.brightness = next; markDeviceSettingsChanged(now); }
  } else if (VOLUME_SLIDER.row().contains(x, y) && audioIsReady()) {
    const uint8_t next = constrain(VOLUME_SLIDER.target(x, device.volume), 0, 100);
    if (next != device.volume || device.muted) {
      device.volume = next; device.muted = false; markDeviceSettingsChanged(now);
    }
  } else if (hit(MUTE_BUTTON, x, y) && audioIsReady()) {
    device.muted = !device.muted;
    markDeviceSettingsChanged(now);
  } else if (hit(TEST_SOUND_BUTTON, x, y) && audioIsReady()) {
    requestSound();
  } else if (hit(TALK_BUTTON, x, y) && micReady()) {
    device.talk = !device.talk;
    markDeviceSettingsChanged(now);
  }
}

void startAlarm(uint32_t now) {
  stopMusic();
  state.alarmRinging = true;
  alarmStartedAt = now;
  state.lastInteraction = now;
  state.pet.pickerOpen = false;
  startRinging();
  Serial.println("{\"event\":\"alarm_ringing\"}");
}

void stopAlarm(uint32_t now) {
  state.alarmRinging = false;
  state.lastInteraction = now;
  cancelSound();
  Serial.println("{\"event\":\"alarm_stopped\"}");
}

void updateAlarm(uint32_t now) {
  if (state.alarmRinging && !alarmWithinRingWindow(alarmStartedAt, now)) stopAlarm(now);
  if (now - lastAlarmCheck < 250 || !clockReady() || deviceSettingsPending()) return;
  lastAlarmCheck = now;
  const time_t epoch = time(nullptr);
  tm local{};
  localtime_r(&epoch, &local);
  const uint32_t day = (local.tm_year + 1900) * 10000UL + (local.tm_mon + 1) * 100UL + local.tm_mday;
  if (!alarmDue(state.alarm, day, local.tm_hour, local.tm_min)) return;
  state.alarm.lastFiredDay = day;
  if (!saveAlarmFiredDay(day)) {
    logError("alarm", "Unable to persist fired date; a restart may repeat this alarm");
  }
  startAlarm(now);
}

// The microphone is only open while talk mode is on and someone can see the
// companion; it closes for the alarm, the setup portal, the picker and other pages.
void updateTalk(uint32_t now) {
  const DeviceSettings &device = state.device;
  const bool audible = !device.muted && device.volume > 0;
  const bool posed = petAssetsReady() && petAsset(state.pet.mascot).kind == PET_ASSET_POSES;
  setTalkActive(device.talk && audible && posed && petPageInteractive() && !musicActive());
  if (musicStatus().phase == MusicPhase::Playing) state.lastInteraction = now;
  // A conversation counts as interaction: keep the screen bright and the pet awake.
  const TalkPhase phase = talkPhase();
  if (phase == TalkPhase::Hearing || phase == TalkPhase::Talking) state.lastInteraction = now;
}

// ---------------------------------------------------------------------------
// Touch, button and pet interaction

void handlePetHold(int x, int y, uint32_t now) {
  if (state.alarmRinging || state.setup.open || state.page != PAGE_PET || !PET_AREA.contains(x, y)) return;
  if (!petAssetsReady()) return;
  state.pet.pickerPage = state.pet.mascot / PICKER_SLOTS;
  state.pet.pickerOpen = true;
  state.pet.reacting = false;
  state.lastInteraction = now;
}

void selectCompanion(uint8_t choice, uint32_t now) {
  PetState &pet = state.pet;
  if (choice != pet.mascot && !saveCompanion(choice)) {
    logError("settings", "Companion selection could not be saved");
    return;
  }
  pet.mascot = choice;
  pet.pickerOpen = false;
  pet.reacting = petAsset(choice).kind == PET_ASSET_POSES;
  pet.expression = PetExpression::Joy;
  pet.reactedAt = now;
  pet.animationStartedAt = now;
  pet.direction = POSE_FRAME_FRONT;
  rapidPetTaps = 0;
  Serial.printf("{\"event\":\"mascot_selected\",\"mascot\":%u}\n", pet.mascot);
}

void handlePickerTap(int x, int y, uint32_t now) {
  PetState &pet = state.pet;
  const uint8_t pages = (petAssetCount() + PICKER_SLOTS - 1) / PICKER_SLOTS;
  if (PICKER_TITLE_BAR.contains(x, y)) {
    if (PICKER_PREV.contains(x, y)) pet.pickerPage = (pet.pickerPage + pages - 1) % pages;
    else if (PICKER_NEXT.contains(x, y)) pet.pickerPage = (pet.pickerPage + 1) % pages;
    return;
  }
  if (!PICKER_AREA.contains(x, y)) { pet.pickerOpen = false; return; }
  for (uint8_t slot = 0; slot < PICKER_SLOTS; ++slot) {
    const uint8_t choice = pet.pickerPage * PICKER_SLOTS + slot;
    if (choice < petAssetCount() && hit(pickerCard(slot), x, y)) { selectCompanion(choice, now); return; }
  }
}

void handlePetTap(uint32_t now) {
  PetState &pet = state.pet;
  if (petAsset(pet.mascot).kind == PET_ASSET_ANIMATION) {
    pet.animationStartedAt = now;
    pet.reacting = false;
    return;
  }
  constexpr PetExpression reactions[] = {PetExpression::Love, PetExpression::Sparkle, PetExpression::Surprise,
                                         PetExpression::Wink, PetExpression::Bashful, PetExpression::Joy};
  constexpr uint8_t reactionCount = sizeof(reactions) / sizeof(reactions[0]);
  // Four quick taps in a row make the pet dizzy.
  rapidPetTaps = now - lastPetTapAt < 1600 ? rapidPetTaps + 1 : 1;
  lastPetTapAt = now;
  if (rapidPetTaps >= 4) {
    pet.expression = PetExpression::Dizzy;
    rapidPetTaps = 0;
  } else {
    pet.expression = reactions[petReactionIndex];
    petReactionIndex = (petReactionIndex + 1) % reactionCount;
  }
  pet.reacting = true;
  pet.reactedAt = now;
  ++pet.tapCount;
  scheduleTapCountSave();
}

void handleClockTap(int x, int y, uint32_t now) {
  AlarmSchedule &alarm = state.alarm;
  if (hit(ALARM_HOUR_DOWN, x, y)) alarm.hour = (alarm.hour + 23) % 24;
  else if (hit(ALARM_HOUR_UP, x, y)) alarm.hour = (alarm.hour + 1) % 24;
  else if (hit(ALARM_MINUTE_DOWN, x, y)) alarm.minute = (alarm.minute + 59) % 60;
  else if (hit(ALARM_MINUTE_UP, x, y)) alarm.minute = (alarm.minute + 1) % 60;
  else if (hit(ALARM_TOGGLE_BUTTON, x, y)) { alarm.enabled = !alarm.enabled; markDeviceSettingsChanged(now); return; }
  else if (hit(TEST_BELL_BUTTON, x, y)) { startAlarm(now); return; }
  else return;
  // A new time may be later today, so let it fire again.
  alarm.lastFiredDay = 0;
  markDeviceSettingsChanged(now);
}

void handleMusicTap(int x, int y, uint32_t now) {
  const MusicStatus music = musicStatus();
  if (state.musicLibraryOpen) {
    const uint16_t pages = max<uint16_t>(1, (music.count + MUSIC_ROWS_PER_PAGE - 1) / MUSIC_ROWS_PER_PAGE);
    state.musicLibraryPage = min<uint16_t>(state.musicLibraryPage, pages - 1);
    if (hit(MUSIC_BACK, x, y)) state.musicLibraryOpen = false;
    else if (hit(MUSIC_RESCAN, x, y)) { state.musicLibraryPage = 0; scanMusic(); }
    else if (hit(MUSIC_LIST_PREVIOUS, x, y)) state.musicLibraryPage = (state.musicLibraryPage + pages - 1) % pages;
    else if (hit(MUSIC_LIST_NEXT, x, y)) state.musicLibraryPage = (state.musicLibraryPage + 1) % pages;
    else for (uint8_t row = 0; row < MUSIC_ROWS_PER_PAGE; ++row) {
      const uint16_t index = state.musicLibraryPage * MUSIC_ROWS_PER_PAGE + row;
      if (index < music.count && hit(musicTrackRow(row), x, y)) {
        setTalkActive(false);
        selectMusicTrack(index);
        state.musicLibraryOpen = false;
        break;
      }
    }
    return;
  }
  if (hit(MUSIC_LIBRARY, x, y)) { state.musicLibraryOpen = true; state.musicLibraryPage = music.index / MUSIC_ROWS_PER_PAGE; }
  else if (hit(MUSIC_STOP, x, y)) stopMusic();
  else if (hit(MUSIC_PLAY, x, y)) {
    setTalkActive(false);
    if (music.phase == MusicPhase::Playing) pauseMusic(); else playMusic();
  } else if (hit(MUSIC_PREVIOUS, x, y)) { setTalkActive(false); previousMusic(); }
  else if (hit(MUSIC_NEXT, x, y)) { setTalkActive(false); nextMusic(); }
  else if (hit(MUSIC_VOLUME_DOWN, x, y) || hit(MUSIC_VOLUME_UP, x, y)) {
    // Steps land on multiples of 5: up goes to the next one above, down to the next one below.
    const int current = state.device.volume;
    const int volume = hit(MUSIC_VOLUME_UP, x, y) ? (current / 5 + 1) * 5
                     : current % 5 == 0 ? current - 5 : current / 5 * 5;
    state.device.volume = static_cast<uint8_t>(constrain(volume, 0, 100));
    state.device.muted = false;
    markDeviceSettingsChanged(now);
  }
}

void handleTap(int x, int y, uint32_t now) {
  state.lastInteraction = now;
  if (state.alarmRinging) { if (hit(STOP_ALARM_BUTTON, x, y)) stopAlarm(now); return; }
  if (state.setup.open) { if (hit(SETUP_DONE_BUTTON, x, y)) closeSetupPortal(); return; }
  if (NAV_BAR.contains(x, y)) {
    state.page = min(PAGE_COUNT - 1, x / NAV_TAB_WIDTH);
    state.pet.pickerOpen = false;
    return;
  }
  switch (state.page) {
    case PAGE_PET:
      if (!petAssetsReady()) return;
      if (state.pet.pickerOpen) handlePickerTap(x, y, now);
      else handlePetTap(now);
      break;
    case PAGE_CLOCK: handleClockTap(x, y, now); break;
    case PAGE_WEATHER: requestWeather(now); break;
    case PAGE_SETTINGS: handleSettingsTap(x, y, now); break;
    case PAGE_MUSIC: handleMusicTap(x, y, now); break;
  }
}

// Applies one press, move or release from the touch task.
void handleTouchSample(const TouchSample &sample) {
  TouchState &touch = state.touch;
  const uint32_t at = sample.at;
  perf.maxInputLatencyMs = max(perf.maxInputLatencyMs, millis() - at);
  state.lastInteraction = at;
  inputPending = true;
  if (sample.pressed) {
    touch.x = sample.x;
    touch.y = sample.y;
    if (petPageInteractive()) state.pet.direction = petDirectionAt(touch.x, touch.y);
    if (!touch.down) {
      touch.down = true;
      touch.startX = touch.x; touch.startY = touch.y; touch.since = at;
      touch.longPressHandled = false;
      touch.draggingControl = !state.alarmRinging && !state.setup.open && state.page == PAGE_SETTINGS &&
                              (BRIGHTNESS_SLIDER.dragZone().contains(touch.x, touch.y) ||
                               VOLUME_SLIDER.dragZone().contains(touch.x, touch.y));
    }
    if (touch.draggingControl) {
      const Slider &slider =
          BRIGHTNESS_SLIDER.row().contains(touch.startX, touch.startY) ? BRIGHTNESS_SLIDER : VOLUME_SLIDER;
      handleSettingsTap(slider.clampToTrack(touch.x), touch.startY, at);
    }
    return;
  }
  if (!touch.down) return;
  touch.down = false;
  if (touch.longPressHandled) { touch.longPressHandled = false; return; }
  if (touch.draggingControl) { touch.draggingControl = false; return; }
  const int direction = swipeDirection(touch.startX, touch.startY, touch.x, touch.y);
  if (!state.alarmRinging && !state.setup.open && direction != 0) {
    PetState &pet = state.pet;
    if (pet.pickerOpen) {
      const int pages = (petAssetCount() + PICKER_SLOTS - 1) / PICKER_SLOTS;
      pet.pickerPage = (pet.pickerPage + direction + pages) % pages;
    } else {
      state.page = (state.page + direction + PAGE_COUNT) % PAGE_COUNT;
    }
  } else if (abs(touch.x - touch.startX) < TAP_SLOP && abs(touch.y - touch.startY) < TAP_SLOP) {
    handleTap(touch.x, touch.y, at);
  }
}

// A finger held still produces no new samples, so the long press is timed here.
void checkLongPress(uint32_t now) {
  TouchState &touch = state.touch;
  if (!touch.down || touch.longPressHandled || !petPageInteractive()) return;
  const bool still = abs(touch.x - touch.startX) < LONG_PRESS_SLOP && abs(touch.y - touch.startY) < LONG_PRESS_SLOP;
  if (still && PET_AREA.contains(touch.startX, touch.startY) && now - touch.since >= LONG_PRESS) {
    handlePetHold(touch.x, touch.y, now);
    touch.longPressHandled = true;
    inputPending = true;
  }
}

void pollTouch(uint32_t now) {
  TouchSample sample;
  while (nextTouchSample(sample)) handleTouchSample(sample);
  checkLongPress(now);
  state.touch.failures = touchFailures();
}

// BOOT button: short press cycles pages, long press toggles the setup portal,
// any press silences a ringing alarm.
void pollButton(uint32_t now) {
  const bool pressed = digitalRead(PIN_BOOT_BUTTON) == LOW;
  if (pressed && !buttonWasDown) {
    buttonAt = now;
    buttonHeld = false;
    if (state.alarmRinging) { stopAlarm(now); buttonHeld = true; }
  }
  if (pressed && now - buttonAt >= 1800 && !buttonHeld) {
    buttonHeld = true;
    if (state.setup.open) closeSetupPortal(); else openSetup();
  }
  if (!pressed && buttonWasDown && !buttonHeld && now - buttonAt > 30) {
    if (state.setup.open) {
      closeSetupPortal();
    } else {
      state.page = (state.page + 1) % PAGE_COUNT;
      state.pet.pickerOpen = false;
    }
    state.lastInteraction = now;
  }
  if (pressed != buttonWasDown) inputPending = true;
  buttonWasDown = pressed;
}

// ---------------------------------------------------------------------------
// Serial console commands (read and parsed in serial_console.cpp)

void printPerf() {
  Serial.printf("{\"perf\":true,\"loops\":%lu,\"renders\":%lu,\"avg_render_us\":%lu,\"max_render_us\":%lu,"
                "\"max_loop_us\":%lu,\"max_pump_us\":%lu,\"max_save_us\":%lu,\"max_input_latency_ms\":%lu,\"free_heap\":%u}\n",
                static_cast<unsigned long>(perf.loops), static_cast<unsigned long>(perf.renders),
                static_cast<unsigned long>(perf.renders ? perf.totalRenderUs / perf.renders : 0),
                static_cast<unsigned long>(perf.maxRenderUs), static_cast<unsigned long>(perf.maxLoopUs),
                static_cast<unsigned long>(perf.maxPumpUs),
                static_cast<unsigned long>(perf.maxSaveUs), static_cast<unsigned long>(perf.maxInputLatencyMs),
                ESP.getFreeHeap());
  perf = Perf{};
}

void runConsoleCommand(const ConsoleLine &line) {
  if (state.alarmRinging && (line.command == ConsoleCommand::MusicSelect || line.command == ConsoleCommand::MusicResume ||
      line.command == ConsoleCommand::MusicNext || line.command == ConsoleCommand::MusicPrevious)) {
    logError("music", "Stop the ringing alarm before starting music");
    printMusicStatus();
    return;
  }
  switch (line.command) {
    case ConsoleCommand::None: break;
    case ConsoleCommand::Status: printStatus(state); break;
    case ConsoleCommand::UiStats: printUiStats(); break;
    case ConsoleCommand::Perf: printPerf(); break;
    case ConsoleCommand::SettingsStatus: printSettingsStatus(state); break;
    case ConsoleCommand::AssetSelfTest: testPetAssets(); break;
    case ConsoleCommand::AlarmStatus: printAlarmStatus(state); break;
    case ConsoleCommand::PetStatus: printPetStatus(state); break;
    case ConsoleCommand::AssetStatus: printAssetStatus(state); break;
    case ConsoleCommand::SdStatus: printSdStatus(); break;
    case ConsoleCommand::SdSelfTest:
      if (musicActive()) {
        Serial.println("{\"sd_test\":\"busy\",\"bytes\":0,\"cleanup\":false,\"sd_error\":\"Stop music before the SD write test\"}");
        break;
      }
      // A failed SD write can consume the driver's full 5 s timeout. This explicit
      // diagnostic releases the loop watchdog until the bounded I/O has returned.
      disableLoopWDT();
      printSdSelfTest();
      enableLoopWDT();
      break;
    case ConsoleCommand::MusicStatus: printMusicStatus(); break;
    case ConsoleCommand::MusicList: printMusicList(); break;
    case ConsoleCommand::MusicScan: scanMusic(); printMusicStatus(); break;
    case ConsoleCommand::MusicSelect: setTalkActive(false); selectMusicTrack(line.index); printMusicStatus(); break;
    case ConsoleCommand::MusicPause: pauseMusic(); printMusicStatus(); break;
    case ConsoleCommand::MusicResume: resumeMusic(); printMusicStatus(); break;
    case ConsoleCommand::MusicStop: stopMusic(); printMusicStatus(); break;
    case ConsoleCommand::MusicNext: setTalkActive(false); nextMusic(); printMusicStatus(); break;
    case ConsoleCommand::MusicPrevious: setTalkActive(false); previousMusic(); printMusicStatus(); break;
    case ConsoleCommand::SdPutBegin: beginSdUpload(line.bytes, line.text); break;
    case ConsoleCommand::SdPutData: writeSdUploadChunk(line.text); break;
    case ConsoleCommand::SdPutEnd: finishSdUpload(line.text); break;
    case ConsoleCommand::SdPutAbort: abortSdUpload(); break;
    case ConsoleCommand::SoundTest: requestSound(); break;
    case ConsoleCommand::TalkStatus: printTalkStatus(); break;
    case ConsoleCommand::MicTest: runMicTest(); break;
    case ConsoleCommand::TalkTune:
      tuneTalk(line.start, line.end);
      printTalkStatus();
      break;
    case ConsoleCommand::Reboot: ESP.restart(); break;
    case ConsoleCommand::Hold: handlePetHold(line.x, line.y, millis()); break;
    case ConsoleCommand::Tap: handleTap(line.x, line.y, millis()); break;
    case ConsoleCommand::Snapshot:
      // The setup screen is never dumped; the command is answered with the help text instead.
      if (state.setup.open) printConsoleHelp();
      else writeSnapshot();
      break;
    case ConsoleCommand::Page:
      if (state.setup.open) closeSetupPortal();
      state.page = line.page;
      state.pet.pickerOpen = false;
      state.lastInteraction = millis();
      break;
    case ConsoleCommand::SelfTest: printSelfTest(); break;
    case ConsoleCommand::Setup: openSetup(); break;
    case ConsoleCommand::Pet:
      if (state.setup.open) closeSetupPortal();
      state.page = PAGE_PET;
      state.pet.pickerOpen = false;
      break;
    case ConsoleCommand::ApPassword:
      if (!saveApPassword(line.text)) {
        logError("setup", "AP password NVS write failed");
      } else {
        Serial.println("{\"event\":\"ap_password_saved\"}");
        if (state.setup.open) { closeSetupPortal(); openSetup(); }
      }
      break;
  }
}

void pollSerial() {
  ConsoleLine line;
  while (readConsoleLine(line)) {
    runConsoleCommand(line);
    inputPending = true;
  }
}
}  // namespace

void appSetup() {
  Serial.begin(115200);
  pinMode(PIN_BOOT_BUTTON, INPUT_PULLUP);
  pinMode(PIN_SPEAKER_MUTE, OUTPUT);
  digitalWrite(PIN_SPEAKER_MUTE, HIGH);
  rgbLedWrite(PIN_STATUS_LED, 0, 0, 0);
  const bool settingsOpen = openSettingsStore();
  if (settingsOpen) loadSettings(state, network);
  // The backlight comes up at the stored level, so a dim room is not flashed at boot.
  if (!initDisplay(state.device.brightness)) { fatalError = true; return; }
  if (!settingsOpen) {
    logError("settings", "NVS open failed");
    showBootProgress("Settings storage failed", 0);
    fatalError = true;
    return;
  }
  showBootProgress("Checking SD card", 10);
  initSdCard();
  printSdStatus();
  showBootProgress("Loading companions", 15);
  initPetAssets();
  // The stored companion may have been removed from a newer asset pack.
  if (petAssetsReady() && state.pet.mascot >= petAssetCount()) {
    state.pet.mascot = 0;
    if (!saveCompanion(0)) logError("settings", "Replacement companion could not be saved");
    Serial.println("{\"event\":\"removed_companion_replaced\",\"mascot\":0}");
  }
  state.pet.animationStartedAt = millis();
  setAudioGain(state.device.muted ? 0 : state.device.volume);
  showBootProgress("Starting touch and sound", 45);
  // Pulse the touch controller reset before starting the shared I2C bus.
  pinMode(PIN_TOUCH_RESET, OUTPUT);
  digitalWrite(PIN_TOUCH_RESET, LOW); delay(10);
  digitalWrite(PIN_TOUCH_RESET, HIGH); delay(180);
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 400000);
  Wire.setTimeOut(20);
  if (!initAudio()) logError("audio", "Speaker disabled because initialization failed");
  else initTalk();
  initMusic();
  // A ringtone in the asset pack (written by the web customizer) replaces the built-in one.
  const AssetRingtone ringtone = assetRingtone();
  if (ringtone.count != 0) {
    setRingtone(ringtone.samples, ringtone.count);
    Serial.printf("{\"event\":\"custom_ringtone\",\"samples\":%lu}\n", static_cast<unsigned long>(ringtone.count));
  }
  // Touch sampling starts after the codec setup, the only other user of the I2C bus.
  if (!startTouchInput()) { logError("touch", "Touch task allocation failed"); fatalError = true; return; }
  initBattery();
  state.battery = readBattery();
  lastBatteryRead = millis();
  showBootProgress("Connecting", 75);
  if (!startWeatherWorker()) { logError("weather", "Worker allocation failed"); fatalError = true; return; }
  initSetupPortal(state, network, applySetupForm);
  setenv("TZ", TIMEZONE, 1);
  tzset();
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  if (network.ssid.isEmpty()) WiFi.begin(); else WiFi.begin(network.ssid.c_str(), network.password.c_str());
  configTzTime(TIMEZONE, "pool.ntp.org", "time.cloudflare.com", "time.google.com");
  showBootProgress("Ready", 100);
  // If the UI loop is ever blocked for 5 s, reboot (and keep a core dump) instead of freezing.
  const esp_task_wdt_config_t watchdog{5000, 0, true};
  if (esp_task_wdt_reconfigure(&watchdog) != ESP_OK) logError("system", "Watchdog configuration failed");
  enableLoopWDT();
  Serial.printf("{\"event\":\"boot\",\"setup_ms\":%lu,\"reset_reason\":%d}\n",
                static_cast<unsigned long>(millis()), static_cast<int>(esp_reset_reason()));
  state.lastInteraction = millis();
  lastReconnect = millis();
  printSelfTest();
  printStatus(state);
}

void appLoop() {
  if (fatalError) { delay(500); return; }
  const uint32_t loopStarted = micros();
  const uint32_t inputAt = millis();
  pollTouch(inputAt);
  pollButton(inputAt);
  pollSerial();
  const uint32_t now = millis();
  if (state.setup.open) pollSetupPortal();
  if (pendingConnectAt != 0 && static_cast<int32_t>(now - pendingConnectAt) >= 0) {
    pendingConnectAt = 0;
    WiFi.disconnect(false, false);
    WiFi.begin(network.ssid.c_str(), network.password.c_str());
    lastReconnect = now;
  }

  const bool connected = WiFi.status() == WL_CONNECTED;
  if (connected && !everConnected) { everConnected = true; Serial.println("{\"event\":\"wifi_connected\"}"); }
  static bool wasConnected = false;
  static uint32_t connectedAt = 0;
  if (connected && !wasConnected) connectedAt = now;  // also after every reconnect
  wasConnected = connected;
  const bool networkSettled = connected && now - connectedAt >= NETWORK_SETTLE;
  // Offer the setup portal once if the device has never reached Wi-Fi and nobody is using it.
  if (!connected && !everConnected && !setupOffered && now > 18000 && !state.setup.open &&
      now - state.lastInteraction > 15000) {
    openSetup();
  }
  // The setup portal pauses reconnects so it can scan; closing it reconnects.
  if (!connected && !state.setup.open && now - lastReconnect > 60000) { WiFi.reconnect(); lastReconnect = now; }

  WeatherState &weather = state.weather;
  const uint32_t weatherInterval = weather.valid ? WEATHER_INTERVAL : WEATHER_RETRY_INTERVAL;
  if (networkSettled && clockReady() && !weather.busy &&
      (lastWeatherAttempt == 0 || now - lastWeatherAttempt >= weatherInterval)) {
    requestWeather(now);
  }
  WeatherResult result;
  if (takeWeatherResult(result)) {
    weather.busy = false;
    if (result.place != weather.place) {
      lastWeatherAttempt = 0;  // the place changed mid-request; fetch the new one right away
    } else if (result.error == WeatherError::None) {
      weather.current = result;
      weather.valid = true;
      Serial.println("{\"event\":\"weather_updated\"}");
    } else {
      weather.current.error = result.error;
      Serial.printf("{\"level\":\"error\",\"component\":\"weather\","
                    "\"reason\":\"Request failed after bounded retries\",\"error_code\":%u}\n",
                    static_cast<unsigned>(result.error));
    }
  }

  if (now - lastBatteryRead >= BATTERY_INTERVAL) {
    state.battery = readBattery();
    lastBatteryRead = now;
  }

  if (state.pet.reacting && now - state.pet.reactedAt > REACTION_DURATION) state.pet.reacting = false;
  updateAlarm(now);
  updateTalk(now);
  static bool alarmLed = false;
  const bool nextLed = state.alarmRinging && ((now / 500) % 2 == 0);
  if (nextLed != alarmLed) {
    rgbLedWrite(PIN_STATUS_LED, nextLed ? 32 : 0, 0, nextLed ? 10 : 0);
    alarmLed = nextLed;
  }
  saveTapCount(state.pet.tapCount, now);
  const uint32_t saveStarted = micros();
  saveDeviceSettings(state, now);
  perf.maxSaveUs = max(perf.maxSaveUs, micros() - saveStarted);
  const uint8_t brightness = state.device.brightness;
  const bool idle = !state.alarmRinging && !state.setup.open && state.page != PAGE_SETTINGS &&
                    now - state.lastInteraction > DIM_AFTER;
  const uint8_t backlight = idle ? max(3, brightness / 4) : brightness;
  static int lastBacklight = -1;
  if (backlight != lastBacklight) { setBacklight(backlight); lastBacklight = backlight; }
  // Input is answered on the very next loop; animation runs on the regular frame tick.
  if (inputPending || now - lastFrame >= FRAME_INTERVAL) {
    inputPending = false;
    const uint32_t renderStarted = micros();
    renderFrame(state, now);
    const uint32_t renderUs = micros() - renderStarted;
    perf.maxRenderUs = max(perf.maxRenderUs, renderUs);
    perf.totalRenderUs += renderUs;
    ++perf.renders;
    lastFrame = now;
    const uint32_t beforeRender = renderStarted - loopStarted;
    if (renderUs > SLOW_FRAME_US || beforeRender > SLOW_FRAME_US) {
      const RenderTiming timing = lastRenderTiming();
      Serial.printf("{\"event\":\"slow_frame\",\"before_render_us\":%lu,\"render_us\":%lu,\"drawn\":%s,"
                    "\"draw_us\":%lu,\"decode_us\":%lu,"
                    "\"page\":%u,\"picker\":%s,\"ringing\":%s}\n",
                    static_cast<unsigned long>(beforeRender), static_cast<unsigned long>(renderUs),
                    timing.drawn ? "true" : "false", static_cast<unsigned long>(timing.drawUs),
                    static_cast<unsigned long>(timing.decodeUs),
                    state.page, state.pet.pickerOpen ? "true" : "false", state.alarmRinging ? "true" : "false");
    }
  }
  const uint32_t pumpStarted = micros();
  pumpDisplay(DISPLAY_SLICE_US);
  perf.maxPumpUs = max(perf.maxPumpUs, micros() - pumpStarted);
  ++perf.loops;
  perf.maxLoopUs = max(perf.maxLoopUs, micros() - loopStarted);
  delay(2);
}
