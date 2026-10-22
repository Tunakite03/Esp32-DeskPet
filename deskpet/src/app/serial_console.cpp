#include "src/app/serial_console.h"

#include <WiFi.h>
#include <ArduinoJson.h>
#include "src/features/music/music.h"
#include "src/app/model.h"
#include "src/app/settings_store.h"
#include "src/core/audio/audio.h"
#include "src/core/storage/sd_card.h"
#include "src/core/system/config.h"
#include "src/core/system/log.h"
#include "src/core/system/timekeeping.h"
#include "src/features/pet/pet_assets.h"
#include "src/features/ui/ui.h"

namespace {
String serialLine;
// Long enough for an sd-put-data line (512 base64 characters) and an upload name.
constexpr size_t SERIAL_LINE_LIMIT = 600;

void printJsonLine(const JsonDocument &document) {
  if (document.overflowed()) { logError("music", "JSON response allocation failed"); return; }
  String line;
  const size_t expected = measureJson(document);
  if (serializeJson(document, line) != expected) { logError("music", "JSON response serialization failed"); return; }
  line += '\n';
  if (line.length() != expected + 1) { logError("music", "JSON response buffer allocation failed"); return; }
  // One serial write prevents worker events from splitting the JSON response.
  Serial.print(line);
}

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
  if (serialLine == "sd-status") return simple(ConsoleCommand::SdStatus);
  if (serialLine == "sd-selftest") return simple(ConsoleCommand::SdSelfTest);
  if (serialLine == "music-status") return simple(ConsoleCommand::MusicStatus);
  if (serialLine == "music-list") return simple(ConsoleCommand::MusicList);
  if (serialLine == "music-scan") return simple(ConsoleCommand::MusicScan);
  if (serialLine == "music-pause") return simple(ConsoleCommand::MusicPause);
  if (serialLine == "music-resume") return simple(ConsoleCommand::MusicResume);
  if (serialLine == "music-stop") return simple(ConsoleCommand::MusicStop);
  if (serialLine == "music-next") return simple(ConsoleCommand::MusicNext);
  if (serialLine == "music-prev") return simple(ConsoleCommand::MusicPrevious);
  if (serialLine.startsWith("music-play ")) {
    const String value = serialLine.substring(11);
    if (value.length() >= 1 && value.length() <= 2 && value[0] >= '0' && value[0] <= '9' &&
        (value.length() == 1 || (value[1] >= '0' && value[1] <= '9'))) {
      const uint16_t index = value.length() == 1 ? value[0] - '0' : (value[0] - '0') * 10 + value[1] - '0';
      if (index < MUSIC_MAX_TRACKS) {
        line.index = index;
        return simple(ConsoleCommand::MusicSelect);
      }
    }
    logError("music", "Use music-play <index> with an index from 0 to 63");
    return line;
  }
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
  if (serialLine == "sd-put-abort") return simple(ConsoleCommand::SdPutAbort);
  if (serialLine.startsWith("sd-put-begin ")) {
    // "sd-put-begin <bytes> <name>": the name is everything after the byte count, so it may contain spaces.
    const String rest = serialLine.substring(13);
    const int space = rest.indexOf(' ');
    if (space > 0) {
      line.bytes = strtoul(rest.substring(0, space).c_str(), nullptr, 10);
      line.text = rest.substring(space + 1);
      return simple(ConsoleCommand::SdPutBegin);
    }
    logError("sd", "Use sd-put-begin <bytes> <name>");
    return line;
  }
  if (serialLine.startsWith("sd-put-data ")) {
    line.text = serialLine.substring(12);
    return simple(ConsoleCommand::SdPutData);
  }
  if (serialLine.startsWith("sd-put-end ")) {
    line.text = serialLine.substring(11);
    return simple(ConsoleCommand::SdPutEnd);
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
    if (serialLine.length() < SERIAL_LINE_LIMIT && c >= 32 && c <= 126) serialLine += c;
  }
  return false;
}

void printConsoleHelp() {
  Serial.println("Commands: status, selftest, sd-status, sd-selftest, setup, pet, talk-status, mic-test, reboot, ap-password <value>");
  Serial.println("Music: music-status, music-list, music-scan, music-play <index>, music-pause, music-resume, music-stop, music-next, music-prev");
  Serial.println("Upload: sd-put-begin <bytes> <name>, sd-put-data <base64>, sd-put-end <crc32>, sd-put-abort (see tools/upload_music.py)");
}

void printStatus(const AppState &state) {
  const SdCardStatus sd = sdCardStatus();
  const MusicStatus music = musicStatus();
  Serial.printf("{\"firmware\":\"%s\",\"board\":\"ES3C28P\",\"uptime_ms\":%lu,\"heap\":%u,\"psram\":%u,"
                "\"page\":%u,\"wifi\":%s,\"clock_synced\":%s,\"weather_valid\":%s,\"weather_error\":%u,"
                "\"alarm_ringing\":%s,\"touch_errors\":%u,\"sd_mounted\":%s,\"sd_writable\":%s,\"music_ready\":%s}\n",
                VERSION, static_cast<unsigned long>(millis()), ESP.getFreeHeap(), ESP.getPsramSize(), state.page,
                WiFi.status() == WL_CONNECTED ? "true" : "false", clockReady() ? "true" : "false",
                state.weather.valid ? "true" : "false", static_cast<unsigned>(state.weather.current.error),
                state.alarmRinging ? "true" : "false", state.touch.failures,
                sd.mounted ? "true" : "false", sd.writable ? "true" : "false", music.ready ? "true" : "false");
}

void printMusicStatus() {
  const MusicStatus music = musicStatus();
  MusicTrack track;
  getMusicTrack(music.index, track);
  JsonDocument document;
  document["music_ready"] = music.ready;
  document["music_phase"] = musicPhaseName(music.phase);
  document["music_tracks"] = music.count;
  document["music_index"] = music.index;
  document["music_title"] = track.title;
  document["music_elapsed_ms"] = music.elapsedMs;
  document["music_progress"] = music.progress;
  document["music_peak"] = music.peak;
  document["audio_busy"] = audioBusy();
  document["music_error"] = musicErrorName(music.error);
  printJsonLine(document);
}

void printMusicList() {
  const MusicStatus music = musicStatus();
  JsonDocument document;
  document["music_tracks"] = music.count;
  JsonArray list = document["tracks"].to<JsonArray>();
  for (uint16_t index = 0; index < music.count; ++index) {
    MusicTrack track;
    if (!getMusicTrack(index, track)) break;
    JsonObject item = list.add<JsonObject>();
    item["index"] = index;
    item["title"] = track.title;
    item["path"] = track.path;
  }
  printJsonLine(document);
}

void printSdStatus() {
  const SdCardStatus sd = sdCardStatus();
  Serial.printf("{\"sd_mounted\":%s,\"sd_writable\":%s,\"sd_capacity_bytes\":%llu,\"sd_bus_width\":%u,"
                "\"sd_boot_record_valid\":%s,\"sd_mountpoint\":\"/sdcard\",\"sd_directory\":\"/deskpet\","
                "\"sd_error\":\"%s\"}\n", sd.mounted ? "true" : "false", sd.writable ? "true" : "false",
                static_cast<unsigned long long>(sd.capacityBytes), sd.busWidth,
                sd.bootRecordValid ? "true" : "false", sdCardErrorName(sd.error));
}

void printSdSelfTest() {
  const SdCardTestResult result = testSdCard();
  Serial.printf("{\"sd_test\":\"%s\",\"bytes\":%u,\"cleanup\":%s,\"sd_error\":\"%s\"}\n",
                result.error == SdCardError::None ? "pass" : "fail", result.bytes,
                result.cleanup ? "true" : "false", sdCardErrorName(result.error));
}

void printPetStatus(const AppState &state) {
  const PetState &pet = state.pet;
  Serial.printf("{\"mascot\":%u,\"expression\":%u,\"reacting\":%s,\"picker_open\":%s}\n", pet.mascot,
                static_cast<uint8_t>(pet.expression), pet.reacting ? "true" : "false",
                pet.pickerOpen ? "true" : "false");
}

void printAssetStatus(const AppState &state) {
  Serial.printf("{\"assets_ready\":%s,\"asset_count\":%u,\"asset_bytes\":%lu,\"mascot\":%u,\"frame\":%lu,"
                "\"picker_page\":%u,\"ringtone_samples\":%lu}\n",
                petAssetsReady() ? "true" : "false", petAssetCount(), static_cast<unsigned long>(petAssetBytes()),
                state.pet.mascot, static_cast<unsigned long>(petVisualFrame(state, millis())), state.pet.pickerPage,
                static_cast<unsigned long>(assetRingtone().count));
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
