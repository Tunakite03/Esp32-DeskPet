#pragma once
#include "src/core/system/config.h"

// Screen geometry shared by drawing (ui.cpp) and touch handling (app.cpp), so
// a control and its touch target can never drift apart.
struct Rect {
  int x, y, w, h;
  constexpr int right() const { return x + w; }
  constexpr int bottom() const { return y + h; }
  constexpr bool contains(int px, int py, int pad = 0) const {
    return px >= x - pad && px < x + w + pad && py >= y - pad && py < y + h + pad;
  }
};

// Extra margin around buttons so taps that land just outside the edge still count.
constexpr int TOUCH_PAD = 4;
constexpr bool hit(const Rect &rect, int x, int y) { return rect.contains(x, y, TOUCH_PAD); }

// Navigation bar: one tab per page across the bottom of the screen.
constexpr Rect NAV_BAR{0, 207, SCREEN_WIDTH, SCREEN_HEIGHT - 207};
constexpr int NAV_TAB_WIDTH = 64;
constexpr int NAV_DIVIDER_Y = 205;
constexpr int NAV_ICON_Y = 210;
constexpr int NAV_LABEL_Y = 226;

// Pet page. The companion stands on a baseline to the right of the clock.
constexpr Rect PET_AREA{160, 35, 160, 170};
constexpr int PET_SPRITE_SIZE = 144;
constexpr int PET_CENTER_X = 234;
constexpr int PET_BASELINE_Y = 194;
constexpr int PET_FACE_Y = PET_BASELINE_Y - PET_SPRITE_SIZE / 2;
// Sound waves drawn beside the companion's ear while talk mode listens.
constexpr int LISTEN_WAVE_X = PET_CENTER_X - 64;
constexpr int LISTEN_WAVE_Y = PET_FACE_Y - 22;

// Companion picker: a title bar with page arrows above a row of cards.
constexpr uint8_t PICKER_SLOTS = 3;
constexpr Rect PICKER_TITLE_BAR{0, 33, SCREEN_WIDTH, 31};
constexpr Rect PICKER_PREV{0, 33, 71, 31};
constexpr Rect PICKER_NEXT{250, 33, 70, 31};
constexpr Rect PICKER_AREA{0, 69, SCREEN_WIDTH, 133};
constexpr Rect pickerCard(uint8_t slot) { return {14 + slot * 100, 69, 92, 132}; }

// Settings sliders: a minus button, a track and a plus button on one row.
struct Slider {
  int centerY;
  constexpr Rect row() const { return {0, centerY - 18, SCREEN_WIDTH, 37}; }
  constexpr Rect minus() const { return {18, centerY - 15, 32, 30}; }
  constexpr Rect plus() const { return {270, centerY - 15, 32, 30}; }
  constexpr Rect track() const { return {65, centerY - 2, 190, 4}; }
  // Dragging works on the track plus a small gap towards each step button.
  constexpr Rect dragZone() const { return {track().x - 5, row().y, track().w + 11, row().h}; }
  // Step buttons snap to the next/prev multiple of 5; touching the track jumps to that position.
  constexpr int target(int x, int current) const {
    return x < track().x - 5 ? (current <= 0 ? 0 : (current - 1) / 5 * 5)
         : x > track().right() + 5 ? (current >= 100 ? 100 : (current / 5 + 1) * 5)
         : (x - track().x) * 100 / track().w;
  }
  constexpr int clampToTrack(int x) const {
    return x < track().x ? track().x : x > track().right() ? track().right() : x;
  }
};
constexpr Slider BRIGHTNESS_SLIDER{80};
constexpr Slider VOLUME_SLIDER{144};
constexpr Rect MUTE_BUTTON{18, 174, 89, 30};
constexpr Rect TEST_SOUND_BUTTON{115, 174, 89, 30};
constexpr Rect TALK_BUTTON{213, 174, 89, 30};

// Music player and the three-row library share their bounds with touch handling.
constexpr Rect MUSIC_PREVIOUS{24, 116, 64, 48};
constexpr Rect MUSIC_PLAY{112, 112, 96, 52};
constexpr Rect MUSIC_NEXT{232, 116, 64, 48};
constexpr Rect MUSIC_VOLUME_DOWN{14, 174, 32, 30};
constexpr Rect MUSIC_VOLUME_UP{108, 174, 32, 30};
constexpr Rect MUSIC_STOP{148, 174, 68, 30};
constexpr Rect MUSIC_LIBRARY{224, 174, 82, 30};
constexpr uint8_t MUSIC_ROWS_PER_PAGE = 3;
constexpr Rect musicTrackRow(uint8_t slot) { return {14, 38 + slot * 42, 292, 38}; }
constexpr Rect MUSIC_LIST_PREVIOUS{14, 174, 44, 30};
constexpr Rect MUSIC_LIST_NEXT{118, 174, 44, 30};
constexpr Rect MUSIC_RESCAN{170, 174, 70, 30};
constexpr Rect MUSIC_BACK{248, 174, 58, 30};

// Clock page: hour and minute steppers above two buttons.
constexpr Rect ALARM_HOUR_DOWN{18, 136, 32, 30};
constexpr Rect ALARM_HOUR_UP{118, 136, 32, 30};
constexpr Rect ALARM_MINUTE_DOWN{170, 136, 32, 30};
constexpr Rect ALARM_MINUTE_UP{270, 136, 32, 30};
constexpr Rect ALARM_TOGGLE_BUTTON{14, 174, 142, 30};
constexpr Rect TEST_BELL_BUTTON{164, 174, 142, 30};

// Full-screen overlays.
constexpr Rect STOP_ALARM_BUTTON{18, 159, 284, 30};
constexpr Rect SETUP_DONE_BUTTON{18, 174, 284, 30};
