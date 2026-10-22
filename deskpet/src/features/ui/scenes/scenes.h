#pragma once
#include <stdint.h>
#include "src/app/state.h"

struct MusicStatus;

// One draw function per screen. Each paints the whole content area into
// ui::canvas; ui.cpp clears the canvas first and adds the tab bar afterwards.
namespace ui {
constexpr uint32_t PET_SLEEP_AFTER = 25000;  // idle time before the companion dozes off

// Pet page (pet_scene.cpp), including the companion picker.
void drawPet(const AppState &state, uint32_t now);
// Squash-and-stretch step of a tap reaction, 0 when at rest.
uint8_t petBounceStep(const AppState &state, uint32_t now);
// Number of sound-wave arcs beside the companion while talk mode listens.
uint8_t listenWaves(uint32_t now);

// Clock page and the full-screen ringing alarm (clock_scene.cpp).
void drawClock(const AppState &state);
void drawRingingAlarm(const AppState &state);

// Weather page (weather_scene.cpp).
void drawWeather(const AppState &state, uint32_t now);
// Spinner position while a fetch is visible on screen, 0 when hidden.
uint32_t weatherSpinnerStep(const AppState &state, uint32_t now);

// Settings page (settings_scene.cpp).
void drawSettings(const AppState &state);

// Music player and cached track browser (music_scene.cpp).
void drawMusic(const AppState &state, const MusicStatus &music);

// Wi-Fi setup overlay (setup_scene.cpp).
void drawConfig(const AppState &state);
}  // namespace ui
