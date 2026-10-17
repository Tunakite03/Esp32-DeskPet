#pragma once
#include <Arduino.h>
#include "src/app/state.h"

// Serial console at 115200 baud, used by the QA scripts in tools/. Commands are
// single lines; replies are JSON lines.

enum class ConsoleCommand : uint8_t {
  None,  // empty line, or a line already answered (help text or an argument error)
  Status, UiStats, Perf, SettingsStatus, AssetSelfTest, AlarmStatus, PetStatus, AssetStatus,
  SoundTest, TalkStatus, MicTest, TalkTune, Reboot, Hold, Tap, Snapshot, Page, SelfTest,
  Setup, Pet, ApPassword
};

struct ConsoleLine {
  ConsoleCommand command = ConsoleCommand::None;
  int x = 0, y = 0;             // Hold, Tap: an on-screen point
  uint16_t start = 0, end = 0;  // TalkTune: 0 < end <= start <= 5000
  uint8_t page = 0;             // Page: below PAGE_COUNT
  String text;                  // ApPassword: 8 to 63 characters
};

// Reads the available serial input. Returns true each time a full line has
// arrived; call it again for the next line.
bool readConsoleLine(ConsoleLine &line);
void printConsoleHelp();

void printStatus(const AppState &state);
void printSettingsStatus(const AppState &state);
void printAlarmStatus(const AppState &state);
void printPetStatus(const AppState &state);
void printAssetStatus(const AppState &state);
void printSelfTest();
