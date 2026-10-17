#pragma once
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include "src/core/system/config.h"
#include "src/features/ui/layout.h"
#include "src/features/ui/ui_assets.h"

// Drawing helpers shared by the scenes (scenes/) and the frame pipeline (ui.cpp).
namespace ui {
// The frame being drawn. ui.cpp owns it and sends only the changed tiles to the panel.
extern Arduino_Canvas canvas;

constexpr uint32_t SPINNER_STEP = 80;  // one 30 degree turn of the loading spinner
constexpr int NAV_INDICATOR_Y = 237, NAV_INDICATOR_INSET = 28;  // underline of the active tab

int textWidth(const String &value, const UiFont &font);
// Draws 4-bit anti-aliased text, blending each glyph against a solid background.
void uiText(int x, int y, const String &value, const UiFont &font, uint16_t color,
            uint16_t background = COLOR_BACKGROUND);
void uiCentered(int y, const String &value, const UiFont &font, uint16_t color);
void uiRightAligned(int y, const String &value, const UiFont &font);

// Page title on the left, Wi-Fi status on the right.
void header(const char *title);
// Tab bar along the bottom with the underline under `activePage`.
void footer(uint8_t activePage);
void navIndicator(int x);
void pill(const Rect &rect, const String &label, uint16_t color);
// Rotating arc shown while something slow (an HTTPS request) is in progress.
void spinner(int x, int y, uint32_t now);
}  // namespace ui
