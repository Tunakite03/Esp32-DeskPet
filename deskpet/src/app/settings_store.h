#pragma once
#include <Arduino.h>
#include "src/app/state.h"

// Settings kept in the "deskpet" NVS namespace. Writes return false when the
// flash could not be written; the caller decides how to report it.

struct NetworkCredentials { String ssid, password; };

// Opens the namespace; false when NVS is unavailable.
bool openSettingsStore();
// Fills the persisted parts of the state and the saved Wi-Fi network.
void loadSettings(AppState &state, NetworkCredentials &network);

bool saveCompanion(uint8_t mascot);
bool saveAlarmFiredDay(uint32_t day);
String loadApPassword();
bool saveApPassword(const String &password);
// Pet name, weather place and brightness from the setup form.
bool saveSetupForm(const String &petName, uint8_t place, uint8_t brightness);
// An empty password removes the stored one (open network).
bool saveNetwork(const NetworkCredentials &network);

// Device and alarm settings are written 600 ms after the last change, so
// dragging a slider does not wear the flash.
void scheduleDeviceSettingsSave(uint32_t now);
bool deviceSettingsPending();
void saveDeviceSettings(const AppState &state, uint32_t now);

// The pet counter is written at most every 15 s.
void scheduleTapCountSave();
void saveTapCount(uint32_t count, uint32_t now);
