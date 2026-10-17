#pragma once
#include "config.h"

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
constexpr int NAV_TAB_WIDTH = 80;
constexpr int NAV_DIVIDER_Y = 205;
constexpr int NAV_LABEL_Y = 218;

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
  // Step buttons nudge by 10 points; touching the track jumps to that position.
  constexpr int target(int x, int current) const {
    return x < track().x - 5 ? current - 10
         : x > track().right() + 5 ? current + 10
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
