#pragma once
#include <stdint.h>

// Pure timing policy shared by the firmware and host regression tests.
namespace power {
constexpr uint32_t DIM_AFTER_MS = 60000;
constexpr uint32_t SETUP_IDLE_MS = 5 * 60000;
constexpr uint32_t POLL_IDLE_AFTER_MS = 1000;
constexpr uint32_t FAST_LOOP_MS = 2;
constexpr uint32_t IDLE_LOOP_MS = 10;

constexpr uint8_t backlightAfterInactivity(uint8_t brightness, bool mayDim,
                                          uint32_t now, uint32_t lastInteraction) {
  if (!mayDim || uint32_t(now - lastInteraction) <= DIM_AFTER_MS) return brightness;
  return brightness / 4 < 3 ? 3 : brightness / 4;
}

constexpr bool setupTimedOut(uint32_t now, uint32_t lastWebActivity, uint32_t lastInteraction) {
  // Subtraction stays correct across millis() rollover. Both sources must be idle.
  return uint32_t(now - lastWebActivity) >= SETUP_IDLE_MS &&
         uint32_t(now - lastInteraction) >= SETUP_IDLE_MS;
}

constexpr uint32_t loopWaitMs(uint32_t now, uint32_t lastInteraction,
                              uint32_t lastSerialActivity, bool busy) {
  return busy || uint32_t(now - lastInteraction) < POLL_IDLE_AFTER_MS ||
         uint32_t(now - lastSerialActivity) < POLL_IDLE_AFTER_MS ? FAST_LOOP_MS : IDLE_LOOP_MS;
}
}  // namespace power
