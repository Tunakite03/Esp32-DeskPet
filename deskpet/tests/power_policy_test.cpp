#include <assert.h>
#include <stdio.h>
#include <initializer_list>
#include "src/core/power/power_policy.h"
#include "src/app/model.h"

int main() {
  using namespace power;
  // Playback is deliberately absent from the brightness policy.
  for (unsigned level = 1; level <= 255; ++level) {
    const auto brightness = static_cast<uint8_t>(level);
    const auto dimmed = static_cast<uint8_t>(level / 4 < 3 ? 3 : level / 4);
    for (uint32_t start : {uint32_t(0), uint32_t(1234), uint32_t(UINT32_MAX - 100)}) {
      assert(backlightAfterInactivity(brightness, true, start + DIM_AFTER_MS, start) == brightness);
      assert(backlightAfterInactivity(brightness, true, start + DIM_AFTER_MS + 1, start) == dimmed);
      assert(backlightAfterInactivity(brightness, false, start + DIM_AFTER_MS + 1, start) == brightness);
      assert(backlightAfterInactivity(brightness, true, start + DIM_AFTER_MS + 1, start + DIM_AFTER_MS + 1) == brightness);
    }
  }
  for (uint32_t start : {uint32_t(0), uint32_t(1234), uint32_t(UINT32_MAX - 100)}) {
    // Opening/reusing setup near rollover must not expire immediately even
    // when the other activity source has already been idle for five minutes.
    assert(!setupTimedOut(start, start, start - SETUP_IDLE_MS));
    assert(!setupTimedOut(start, start - SETUP_IDLE_MS, start));
    assert(!setupTimedOut(start + SETUP_IDLE_MS - 1, start, start));
    assert(setupTimedOut(start + SETUP_IDLE_MS, start, start));
    assert(!setupTimedOut(start + SETUP_IDLE_MS, start + 1, start));
    assert(!setupTimedOut(start + SETUP_IDLE_MS, start, start + 1));
    assert(!setupTimedOut(start + SETUP_IDLE_MS, start + SETUP_IDLE_MS, start));
    assert(!setupTimedOut(start + SETUP_IDLE_MS, start, start + SETUP_IDLE_MS));
    assert(loopWaitMs(start + POLL_IDLE_AFTER_MS - 1, start, start, false) == FAST_LOOP_MS);
    assert(loopWaitMs(start + POLL_IDLE_AFTER_MS, start, start, false) == IDLE_LOOP_MS);
    assert(loopWaitMs(start + POLL_IDLE_AFTER_MS, start, start, true) == FAST_LOOP_MS);
    assert(loopWaitMs(start + POLL_IDLE_AFTER_MS, start + POLL_IDLE_AFTER_MS, start, false) == FAST_LOOP_MS);
    assert(loopWaitMs(start + POLL_IDLE_AFTER_MS, start, start + POLL_IDLE_AFTER_MS, false) == FAST_LOOP_MS);
  }
  assert(modelSelfTest());  // original alarm and gesture policy still passes
  puts("PASS: brightness, portal timeout, adaptive waits, rollover, alarm and swipe policy");
}
