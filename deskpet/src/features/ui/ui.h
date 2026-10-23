#pragma once
#include <stdint.h>
#include "src/app/state.h"

// Starts the ILI9341 panel, backlight PWM and the comparison framebuffer.
bool initDisplay(uint8_t backlight);
void setBacklight(uint8_t level);
// Boot splash with a progress bar, shown while setup is still running.
void showBootProgress(const char *step, uint8_t percent);
uint32_t backlightLevel();
// Redraws only when something visible changed, then sends just the changed tiles.
void renderFrame(const AppState &state, uint32_t now);
// Draw time of the last frame that was drawn; `drawn` is false after a skipped frame.
struct RenderTiming {
  bool drawn = false;
  uint32_t drawUs = 0, decodeUs = 0;
};
RenderTiming lastRenderTiming();
// Sends the next few tile rows of the last drawn frame, for about budgetUs at most.
// Call it every loop after renderFrame().
void pumpDisplay(uint32_t budgetUs);
// Keep the main loop fast until the current frame has reached the panel.
bool displayTransferPending();
void printUiStats();
// Dumps the framebuffer over serial for the QA scripts.
void writeSnapshot();

// Which gaze frame faces the touch point at (x, y).
uint8_t petDirectionAt(int x, int y);
uint32_t petVisualFrame(const AppState &state, uint32_t now);
