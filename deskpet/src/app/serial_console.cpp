#include "src/app/serial_console.h"

#include <WiFi.h>
#include "src/app/model.h"
#include "src/app/settings_store.h"
#include "src/core/audio/audio.h"
#include "src/core/system/config.h"
#include "src/core/system/log.h"
#include "src/core/system/timekeeping.h"
#include "src/features/pet/pet_assets.h"
#include "src/features/ui/ui.h"

namespace {
String serialLine;

// Parses "<command> x y" and rejects trailing input or off-screen points.
bool parsePoint(const char *format, int &x, int &y) {
  char extra = 0;
  return sscanf(serialLine.c_str(), format, &x, &y, &extra) == 2 && x >= 0 && x < SCREEN_WIDTH && y >= 0 &&
         y < SCREEN_HEIGHT;
}

ConsoleLine parseLine() {
  ConsoleLine line;
  const auto simple = [&line](ConsoleCommand command) { line.command = command; return line; };
  if (serialLine == "status") return simple(ConsoleCommand::Status);
  if (serialLine == "ui-stats") return simple(ConsoleCommand::UiStats);
  if (serialLine == "perf") return simple(ConsoleCommand::Perf);
  if (serialLine == "settings-status") return simple(ConsoleCommand::SettingsStatus);
  if (serialLine == "asset-selftest") return simple(ConsoleCommand::AssetSelfTest);
  if (serialLine == "alarm-status") return simple(ConsoleCommand::AlarmStatus);
  if (serialLine == "pet-status") return simple(ConsoleCommand::PetStatus);
  if (serialLine == "asset-status") return simple(ConsoleCommand::AssetStatus);
  if (serialLine == "sound-test") return simple(ConsoleCommand::SoundTest);
  if (serialLine == "talk-status") return simple(ConsoleCommand::TalkStatus);
  if (serialLine == "mic-test") return simple(ConsoleCommand::MicTest);
  if (serialLine.startsWith("talk-tune ")) {
    unsigned start = 0, end = 0;
    char extra = 0;
    if (sscanf(serialLine.c_str(), "talk-tune %u %u %c", &start, &end, &extra) == 2 && end > 0 && end <= start &&
        start <= 5000) {
      line.start = start;
      line.end = end;
      return simple(ConsoleCommand::TalkTune);
    }
    logError("input", "Use talk-tune <start> <end> with 0 < end <= start <= 5000");
    return line;
  }
  if (serialLine == "reboot") return simple(ConsoleCommand::Reboot);
  if (serialLine.startsWith("hold ")) {
    if (parsePoint("hold %d %d %c", line.x, line.y)) return simple(ConsoleCommand::Hold);
    logError("input", "Invalid hold coordinates");
    return line;
  }
  if (serialLine.startsWith("tap ")) {
    if (parsePoint("tap %d %d %c", line.x, line.y)) return simple(ConsoleCommand::Tap);
    logError("input", "Invalid tap coordinates");
    return line;
  }
  if (serialLine == "snapshot") return simple(ConsoleCommand::Snapshot);
  if (serialLine.length() == 6 && serialLine.startsWith("page ") && serialLine[5] >= '0' &&
      serialLine[5] < '0' + PAGE_COUNT) {
    line.page = serialLine[5] - '0';
    return simple(ConsoleCommand::Page);
  }
  if (serialLine == "selftest") return simple(ConsoleCommand::SelfTest);
  if (serialLine == "setup") return simple(ConsoleCommand::Setup);
  if (serialLine == "pet") return simple(ConsoleCommand::Pet);
  if (serialLine.startsWith("ap-password ")) {
    line.text = serialLine.substring(12);
    if (line.text.length() >= 8 && line.text.length() <= 63) return simple(ConsoleCommand::ApPassword);
    logError("setup", "AP password must contain 8 to 63 characters");
    return line;
  }
  if (!serialLine.isEmpty()) printConsoleHelp();
  return line;
}
}  // namespace

bool readConsoleLine(ConsoleLine &line) {
  while (Serial.available()) {
    const char c = Serial.read();
    if (c == '\n') {
      serialLine.trim();
      line = parseLine();
      serialLine = "";
      return true;
    }
    if (serialLine.length() < 80 && c >= 32 && c <= 126) serialLine += c;
  }
  return false;
}

void printConsoleHelp() {
  Serial.println("Commands: status, selftest, setup, pet, talk-status, mic-test, reboot, ap-password <value>");
}

void printStatus(const AppState &state) {
  Serial.printf("{\"firmware\":\"%s\",\"board\":\"ES3C28P\",\"uptime_ms\":%lu,\"heap\":%u,\"psram\":%u,"
                "\"page\":%u,\"wifi\":%s,\"clock_synced\":%s,\"weather_valid\":%s,\"weather_error\":%u,"
                "\"alarm_ringing\":%s,\"touch_errors\":%u}\n",
                VERSION, static_cast<unsigned long>(millis()), ESP.getFreeHeap(), ESP.getPsramSize(), state.page,
                WiFi.status() == WL_CONNECTED ? "true" : "false", clockReady() ? "true" : "false",
                state.weather.valid ? "true" : "false", static_cast<unsigned>(state.weather.current.error),
                state.alarmRinging ? "true" : "false", state.touch.failures);
}

void printPetStatus(const AppState &state) {
  const PetState &pet = state.pet;
  Serial.printf("{\"mascot\":%u,\"expression\":%u,\"reacting\":%s,\"picker_open\":%s}\n", pet.mascot,
                static_cast<uint8_t>(pet.expression), pet.reacting ? "true" : "false",
                pet.pickerOpen ? "true" : "false");
}

void printAssetStatus(const AppState &state) {
  Serial.printf("{\"assets_ready\":%s,\"asset_count\":%u,\"asset_bytes\":%lu,\"mascot\":%u,\"frame\":%lu,"
                "\"picker_page\":%u}\n",
                petAssetsReady() ? "true" : "false", petAssetCount(), static_cast<unsigned long>(petAssetBytes()),
                state.pet.mascot, static_cast<unsigned long>(petVisualFrame(state, millis())), state.pet.pickerPage);
}

void printSettingsStatus(const AppState &state) {
  const DeviceSettings &device = state.device;
  const AudioStats audio = audioStats();
  Serial.printf("{\"brightness\":%u,\"brightness_percent\":%d,\"backlight_pwm\":%lu,\"volume\":%u,"
                "\"muted\":%s,\"audio_ready\":%s,\"audio_completed\":%lu,\"audio_peak\":%lu,"
                "\"settings_pending\":%s,\"talk\":%s,\"mic_ready\":%s}\n",
                device.brightness, brightnessPercent(device.brightness),
                static_cast<unsigned long>(backlightLevel()), device.volume, device.muted ? "true" : "false",
                audioIsReady() ? "true" : "false", static_cast<unsigned long>(audio.completed),
                static_cast<unsigned long>(audio.peak), deviceSettingsPending() ? "true" : "false",
                device.talk ? "true" : "false", micReady() ? "true" : "false");
}

void printAlarmStatus(const AppState &state) {
  const AlarmSchedule &alarm = state.alarm;
  Serial.printf("{\"alarm_hour\":%u,\"alarm_minute\":%u,\"enabled\":%s,\"ringing\":%s,\"last_fired_day\":%lu,"
                "\"clock_synced\":%s,\"local_time\":\"%s\"}\n",
                alarm.hour, alarm.minute, alarm.enabled ? "true" : "false", state.alarmRinging ? "true" : "false",
                static_cast<unsigned long>(alarm.lastFiredDay), clockReady() ? "true" : "false",
                dateText("%Y-%m-%d %H:%M:%S").c_str());
}

void printSelfTest() {
  Serial.println(modelSelfTest() ? "SELFTEST PASS: alarm schedule, midnight, daily deduplication, timeout, swipe"
                                 : "SELFTEST FAIL");
}
