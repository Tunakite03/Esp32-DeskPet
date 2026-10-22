#pragma once
#include <stdint.h>

// Bump only the number; the boot splash, settings page and serial status read it from here.
#define DESKPET_VERSION_NUMBER "1.12.0"
inline constexpr char VERSION_NUMBER[] = DESKPET_VERSION_NUMBER;
inline constexpr char VERSION[] = "DeskPet " DESKPET_VERSION_NUMBER;
inline constexpr char TIMEZONE[] = "ICT-7";
// Setup access point password unless one was stored with `ap-password <value>`.
inline constexpr char DEFAULT_AP_PASSWORD[] = "AAAA8888";

constexpr int SCREEN_WIDTH = 320, SCREEN_HEIGHT = 240;

// Board-level pins shared by several modules. Display, codec and touch bus
// pins live next to the driver that owns them.
constexpr uint8_t PIN_BOOT_BUTTON = 0;
constexpr uint8_t PIN_SPEAKER_MUTE = 1;  // HIGH switches the amplifier off
constexpr uint8_t PIN_I2C_SCL = 15;
constexpr uint8_t PIN_I2C_SDA = 16;
constexpr uint8_t PIN_TOUCH_RESET = 18;
constexpr uint8_t PIN_STATUS_LED = 42;
constexpr uint8_t PIN_BACKLIGHT = 45;

// RGB565 palette. Text and icons are white for contrast on the small panel
// (only inactive tabs are muted); colour is reserved for accents.
constexpr uint16_t COLOR_BACKGROUND = 0x0000;
constexpr uint16_t COLOR_TEXT = 0xFFFF;
constexpr uint16_t COLOR_MUTED = 0x9CF3;     // labels of the tabs that are not shown
constexpr uint16_t COLOR_SURFACE = 0x18C3;   // dividers and neutral buttons
constexpr uint16_t COLOR_OUTLINE = 0x4208;   // control outlines and slider tracks
constexpr uint16_t COLOR_ACCENT = 0x228B;    // primary buttons and enabled alarm
constexpr uint16_t COLOR_SELECTED = 0x3DD5;  // chosen companion
constexpr uint16_t COLOR_PEACH = 0xF60F;     // sun and brightness slider
constexpr uint16_t COLOR_SKY = 0x459F;       // rain, snow and volume slider
constexpr uint16_t COLOR_CLOUD = 0xCEFF;
constexpr uint16_t COLOR_PINK = 0xF493;      // reaction indicator

struct Place { const char *name; float latitude; float longitude; };
inline constexpr Place PLACES[] = {
  {"Ho Chi Minh City", 10.8231F, 106.6297F}, {"Ha Noi", 21.0285F, 105.8542F},
  {"Da Nang", 16.0544F, 108.2022F}, {"Can Tho", 10.0452F, 105.7469F},
  {"Da Lat", 11.9404F, 108.4583F}, {"Hai Phong", 20.8449F, 106.6881F}
};
constexpr uint8_t PLACE_COUNT = sizeof(PLACES) / sizeof(PLACES[0]);

// Raw companion asset pack written by build_pet_assets.py (see partitions.csv).
constexpr uint8_t ASSET_PARTITION_SUBTYPE = 0x40;
inline constexpr char ASSET_PARTITION_LABEL[] = "assets";
