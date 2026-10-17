#pragma once
#include <Arduino.h>
#include "src/app/model.h"
#include "src/features/pet/pet_assets.h"
#include "src/features/weather/weather.h"

enum Page : uint8_t { PAGE_PET, PAGE_CLOCK, PAGE_WEATHER, PAGE_SETTINGS, PAGE_COUNT };
enum class PetExpression : uint8_t { Blink, Love, Sparkle, Surprise, Wink, Bashful, Sleepy, Dizzy, Joy };

struct TouchState {
  bool down = false;
  bool draggingControl = false;
  bool longPressHandled = false;
  int startX = 0, startY = 0, x = 0, y = 0;
  uint32_t since = 0;
  uint8_t failures = 0;
};

struct DeviceSettings {
  uint8_t brightness = 170;
  uint8_t volume = 40;
  bool muted = false;
  bool talk = false;  // the companion listens and repeats what it hears
};

struct PetState {
  String name = "Mochi";
  uint8_t mascot = 0;
  PetExpression expression = PetExpression::Joy;
  uint8_t direction = POSE_FRAME_FRONT;
  bool reacting = false;
  uint32_t reactedAt = 0;
  uint32_t animationStartedAt = 0;
  uint32_t tapCount = 0;
  bool pickerOpen = false;
  uint8_t pickerPage = 0;
};

struct WeatherState {
  uint8_t place = 0;
  WeatherResult current;
  bool valid = false;
  bool busy = false;
};

struct SetupPortalState {
  bool open = false;
  String apName, apPassword;
};

// Everything the screen shows. The main loop in app.cpp owns and mutates it;
// ui.cpp only reads it.
struct AppState {
  uint8_t page = PAGE_PET;
  DeviceSettings device;
  PetState pet;
  WeatherState weather;
  AlarmSchedule alarm;
  bool alarmRinging = false;
  SetupPortalState setup;
  TouchState touch;
  uint32_t lastInteraction = 0;
  // Bumped for changes the render stamp cannot see (pet name, place).
  uint32_t uiRevision = 0;
};
