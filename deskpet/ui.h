#pragma once
#include <stdint.h>
#include "state.h"

// Starts the ILI9341 panel, backlight PWM and the comparison framebuffer.
bool initDisplay(uint8_t backlight);
void setBacklight(uint8_t level);
// Boot splash with a progress bar, shown while setup is still running.
void showBootProgress(const char *step, uint8_t percent);
uint32_t backlightLevel();
// Redraws only when something visible changed, then sends just the changed tiles.
void renderFrame(const AppState &state, uint32_t now);
void printUiStats();
// Dumps the framebuffer over serial for the QA scripts.
void writeSnapshot();

// Which gaze frame faces the touch point at (x, y).
uint8_t petDirectionAt(int x, int y);
uint32_t petVisualFrame(const AppState &state, uint32_t now);
