#include "src/app/settings_store.h"

#include <Preferences.h>
#include "src/core/system/config.h"
#include "src/core/system/log.h"

namespace {
Preferences preferences;
bool devicePending = false;
uint32_t deviceChangedAt = 0;
bool tapCountPending = false;
uint32_t lastTapCountSave = 0;
}  // namespace

bool openSettingsStore() { return preferences.begin("deskpet", false); }

void loadSettings(AppState &state, NetworkCredentials &network) {
  PetState &pet = state.pet;
  DeviceSettings &device = state.device;
  AlarmSchedule &alarm = state.alarm;
  pet.name = preferences.getString("pet", "Mochi");
  pet.mascot = preferences.getUChar("mascot", 0);
  state.weather.place = preferences.getUChar("place", 0);
  if (state.weather.place >= PLACE_COUNT) state.weather.place = 0;
  device.brightness = max(static_cast<uint8_t>(3), preferences.getUChar("bright", 170));
  device.volume = min(static_cast<uint8_t>(100), preferences.getUChar("volume", 40));
  device.muted = preferences.getBool("muted", false);
  device.talk = preferences.getBool("talk", false);
  alarm.hour = preferences.getUChar("alarm_hour", 7);
  alarm.minute = preferences.getUChar("alarm_min", 0);
  alarm.enabled = preferences.getBool("alarm_on", false);
  alarm.lastFiredDay = preferences.getUInt("alarm_day", 0);
  if (alarm.hour > 23 || alarm.minute > 59) {
    logError("settings", "Invalid alarm schedule; alarm disabled");
    alarm = AlarmSchedule{};
  }
  pet.tapCount = preferences.getUInt("pets", 0);
  network.ssid = preferences.getString("ssid", "");
  network.password = preferences.getString("password", "");
}

bool saveCompanion(uint8_t mascot) { return preferences.putUChar("mascot", mascot) != 0; }

bool saveAlarmFiredDay(uint32_t day) { return preferences.putUInt("alarm_day", day) != 0; }

String loadApPassword() { return preferences.getString("ap_password", ""); }

bool saveApPassword(const String &password) { return preferences.putString("ap_password", password) != 0; }

bool saveSetupForm(const String &petName, uint8_t place, uint8_t brightness) {
  return preferences.putString("pet", petName) != 0 && preferences.putUChar("place", place) != 0 &&
         preferences.putUChar("bright", brightness) != 0;
}

bool saveNetwork(const NetworkCredentials &network) {
  if (preferences.putString("ssid", network.ssid) == 0 ||
      (!network.password.isEmpty() && preferences.putString("password", network.password) == 0)) {
    return false;
  }
  if (network.password.isEmpty()) preferences.remove("password");
  return true;
}

void scheduleDeviceSettingsSave(uint32_t now) {
  devicePending = true;
  deviceChangedAt = now;
}

bool deviceSettingsPending() { return devicePending; }

void saveDeviceSettings(const AppState &state, uint32_t now) {
  if (!devicePending || now - deviceChangedAt < 600) return;
  const DeviceSettings &device = state.device;
  const AlarmSchedule &alarm = state.alarm;
  const bool saved = preferences.putUChar("bright", device.brightness) != 0 &&
                     preferences.putUChar("volume", device.volume) != 0 &&
                     preferences.putBool("muted", device.muted) != 0 &&
                     preferences.putBool("talk", device.talk) != 0 &&
                     preferences.putUChar("alarm_hour", alarm.hour) != 0 &&
                     preferences.putUChar("alarm_min", alarm.minute) != 0 &&
                     preferences.putBool("alarm_on", alarm.enabled) != 0 &&
                     preferences.putUInt("alarm_day", alarm.lastFiredDay) != 0;
  if (!saved) {
    logError("settings", "Device or alarm settings could not be saved");
    deviceChangedAt = now;
    return;
  }
  devicePending = false;
  Serial.println("{\"event\":\"device_settings_saved\"}");
}

void scheduleTapCountSave() { tapCountPending = true; }

void saveTapCount(uint32_t count, uint32_t now) {
  if (!tapCountPending || now - lastTapCountSave <= 15000) return;
  if (preferences.putUInt("pets", count) == 0) logError("settings", "Pet counter write failed");
  else tapCountPending = false;
  lastTapCountSave = now;
}
