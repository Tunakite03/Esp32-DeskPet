#pragma once
#include <stdint.h>

// One change seen on the FT6336 touch controller, in screen (landscape) coordinates.
struct TouchSample {
  uint32_t at;  // millis() when it was read
  int16_t x, y;
  bool pressed;
};

// Samples the controller every 10 ms on its own task, so a tap is never lost
// while the main loop is busy drawing. Wire must already be running.
bool startTouchInput();
// Next queued change (press, move or release); false when none is waiting.
bool nextTouchSample(TouchSample &sample);
// Consecutive failed reads; non-zero means the controller is not answering.
uint8_t touchFailures();
