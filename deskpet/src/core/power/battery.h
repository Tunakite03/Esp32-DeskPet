#pragma once
#include <stdint.h>

// Battery sense: the 3.7 V Li-Po reaches GPIO 9 through a 1:2 divider made of two
// 200 kOhm resistors (R14 and R15 in the schematic). GPIO 9 is ADC1 channel 8, so
// it keeps working while Wi-Fi is on.
// Limit: with USB connected, the TP4054 charger holds BAT+ near 4.2 V, so the reading
// stays near full with or without a pack, and runs high while the pack is charging.
constexpr uint8_t PIN_BATTERY_ADC = 9;
constexpr uint16_t BATTERY_DIVIDER = 2;
// TP4054 CHRG (pin 1), open drain: low while a pack charges over USB. Wire it to this pin.
// Unwired, the internal pull-up keeps the pin high, which reads as "not charging".
constexpr uint8_t PIN_BATTERY_CHARGING = 21;

struct BatteryReading {
  bool present = false;     // false when no pack is connected, the pin then reads near 0 V
  bool charging = false;    // only ever true while a pack is present
  uint8_t percent = 0;      // 0-100, straight line from an empty cell to a full one
};

// Selects the calibrated ADC attenuation for the pin. Call once at boot.
void initBattery();
// Averages a few samples of the pack voltage. Takes milliseconds, so call it on a timer, not every frame.
BatteryReading readBattery();
