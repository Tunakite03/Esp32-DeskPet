#include "src/core/power/battery.h"

#include <Arduino.h>

namespace {
constexpr uint8_t SAMPLES = 8;
// A 3.7 V Li-Po is treated as empty at 3.3 V and full at 4.2 V.
constexpr uint32_t PACK_EMPTY_MV = 3300, PACK_FULL_MV = 4200;
// A disconnected pack lets the divider pull the pin to ground, far below any real cell.
constexpr uint32_t PACK_ABSENT_BELOW_MV = 1000;

BatteryReading readingFromPackMillivolts(uint32_t packMillivolts) {
  if (packMillivolts < PACK_ABSENT_BELOW_MV) return {false, false, 0};
  const uint32_t clamped = min(max(packMillivolts, PACK_EMPTY_MV), PACK_FULL_MV);
  const uint32_t percent = (clamped - PACK_EMPTY_MV) * 100 / (PACK_FULL_MV - PACK_EMPTY_MV);
  return {true, false, static_cast<uint8_t>(percent)};
}
}  // namespace

void initBattery() {
  analogSetPinAttenuation(PIN_BATTERY_ADC, ADC_11db);
  pinMode(PIN_BATTERY_CHARGING, INPUT_PULLUP);
}

BatteryReading readBattery() {
  uint32_t sum = 0;
  for (uint8_t sample = 0; sample < SAMPLES; ++sample) sum += analogReadMilliVolts(PIN_BATTERY_ADC);
  // The calibrated pin voltage is half the pack voltage.
  BatteryReading reading = readingFromPackMillivolts(sum / SAMPLES * BATTERY_DIVIDER);
  reading.charging = reading.present && digitalRead(PIN_BATTERY_CHARGING) == LOW;
  return reading;
}
