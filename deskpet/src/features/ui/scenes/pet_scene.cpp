#include "src/features/ui/scenes/scenes.h"

#include <math.h>
#include "src/core/system/timekeeping.h"
#include "src/features/pet/pet_assets.h"
#include "src/features/talk/talk.h"
#include "src/features/ui/ui.h"
#include "src/features/ui/ui_draw.h"

namespace ui {
namespace {
constexpr uint32_t WAVE_STEP = 350;  // idle pulse of the listening waves

constexpr uint32_t expressionFrame(PetExpression expression) {
  return REACTION_FRAME_BASE + static_cast<uint8_t>(expression);
}

// Talk mode only drives posed companions that are not busy with a tap reaction.
bool talkAnimating(const AppState &state) {
  return talkPhase() != TalkPhase::Off && !state.pet.reacting && petAssetsReady() &&
         petAsset(state.pet.mascot).kind == PET_ASSET_POSES;
}

void drawMascotPicker(const AppState &state) {
  header("CHOOSE A COMPANION", state.battery);
  const uint8_t pages = (petAssetCount() + PICKER_SLOTS - 1) / PICKER_SLOTS;
  if (pages > 1) {
    // Page arrows over the PICKER_PREV / PICKER_NEXT touch zones, and the page number.
    constexpr int arrowY = 49;
    for (int side : {-1, 1}) {
      const int tip = side < 0 ? 18 : 302;
      for (int thick = 0; thick < 2; ++thick) {
        canvas.drawLine(tip - side * (6 - thick), arrowY - 6, tip + side * thick, arrowY, COLOR_TEXT);
        canvas.drawLine(tip + side * thick, arrowY, tip - side * (6 - thick), arrowY + 6, COLOR_TEXT);
      }
    }
    uiCentered(41, "Pick your companion  " + String(state.pet.pickerPage + 1) + "/" + String(pages), FONT_BODY,
               COLOR_TEXT);
  } else {
    uiCentered(41, "Pick your companion", FONT_BODY, COLOR_TEXT);
  }
  for (uint8_t slot = 0; slot < PICKER_SLOTS; ++slot) {
    const uint8_t choice = state.pet.pickerPage * PICKER_SLOTS + slot;
    if (choice >= petAssetCount()) continue;
    const Rect card = pickerCard(slot);
    const PetAsset &asset = petAsset(choice);
    const bool selected = choice == state.pet.mascot;
    canvas.drawRoundRect(card.x, card.y, card.w, card.h, 12, selected ? COLOR_SELECTED : COLOR_OUTLINE);
    const uint32_t frame = asset.kind == PET_ASSET_POSES ? POSE_FRAME_FRONT : 0;
    drawPetAsset(canvas.getFramebuffer(), choice, frame, card.x + 5, card.y + 5, card.w - 10, card.w - 10);
    const String label = asset.name;
    uiText(card.x + (card.w - textWidth(label, FONT_SMALL)) / 2, card.y + 95, label, FONT_SMALL, COLOR_TEXT);
    if (selected) uiText(card.x + 23, card.y + 114, "Selected", FONT_SMALL, COLOR_TEXT);
  }
}
}  // namespace

// A slow pulse while waiting, the live level while hearing.
uint8_t listenWaves(uint32_t now) {
  const TalkPhase phase = talkPhase();
  if (phase == TalkPhase::Hearing) return max<uint8_t>(1, talkLevel());
  // While waiting, the waves follow the microphone so you can see it hears you;
  // in a silent room they pulse gently instead.
  if (phase == TalkPhase::Listening) return talkLevel() > 0 ? talkLevel() : 1 + (now / WAVE_STEP) % 3;
  return 0;
}

uint8_t petBounceStep(const AppState &state, uint32_t now) {
  const PetState &pet = state.pet;
  if (!petAssetsReady() || petAsset(pet.mascot).kind != PET_ASSET_POSES || !pet.reacting ||
      now - pet.reactedAt >= 420) {
    return 0;
  }
  return 1 + (now - pet.reactedAt) / 105;
}

void drawPet(const AppState &state, uint32_t now) {
  if (!petAssetsReady()) {
    header("DESK COMPANION", state.battery);
    uiCentered(75, "Pet assets unavailable", FONT_TITLE, COLOR_TEXT);
    uiCentered(105, "Reload the companion asset pack", FONT_SMALL, COLOR_TEXT);
    return;
  }
  const PetState &pet = state.pet;
  if (pet.pickerOpen) { drawMascotPicker(state); return; }
  const PetAsset &asset = petAsset(pet.mascot);
  const bool posed = asset.kind == PET_ASSET_POSES;
  const bool talking = talkAnimating(state);
  const bool sleeping = posed && !talking && now - state.lastInteraction > PET_SLEEP_AFTER && !pet.reacting;
  header("DESK COMPANION", state.battery);
  uiText(15, 43, clockText(), FONT_TIME, COLOR_TEXT);
  uiText(18, 108, dateText("%a, %d %b"), FONT_BODY, COLOR_TEXT);
  canvas.drawFastHLine(18, 133, 124, COLOR_SURFACE);
  String name = posed ? pet.name : String(asset.name);
  while (textWidth(name, FONT_TITLE) > 137 && name.length() > 1) name.remove(name.length() - 1);
  uiText(17, 143, name, FONT_TITLE, COLOR_TEXT);
  canvas.fillCircle(21, 175, 2, pet.reacting ? COLOR_PINK : COLOR_TEXT);
  constexpr const char *reactions[] = {"Hello there", "Feeling loved", "You sparkle", "Oh, hello!", "Just for you",
                                       "A little shy", "Dreaming", "Feeling dizzy", "So happy!"};
  constexpr const char *talkMoods[] = {"", "I'm listening", "I hear you...", "Listen to me!"};
  const bool talkMuted = posed && state.device.talk && (state.device.muted || state.device.volume == 0);
  const char *mood = !posed ? "Tap to replay"
                   : talking ? talkMoods[static_cast<uint8_t>(talkPhase())]
                   : sleeping ? "Dreaming"
                   : pet.reacting ? reactions[static_cast<uint8_t>(pet.expression)]
                   : talkMuted ? "Unmute to talk"
                   : "Here with you";
  uiText(30, 168, mood, FONT_SMALL, COLOR_TEXT);
  uiText(18, 187, "Hold pet to change", FONT_SMALL, COLOR_TEXT);
  // Squash and stretch while reacting or speaking; animations always draw at a fixed size.
  uint8_t step = petBounceStep(state, now);
  if (talking && talkPhase() == TalkPhase::Talking && talkLevel() >= 2) step = (now / 120) % 2 ? 3 : 0;
  constexpr int widths[] = {144, 150, 140, 146, 144};
  constexpr int heights[] = {144, 134, 150, 140, 144};
  const int width = posed ? widths[step] : 128;
  const int height = posed ? heights[step] : 128;
  drawPetAsset(canvas.getFramebuffer(), pet.mascot, petVisualFrame(state, now), PET_CENTER_X - width / 2,
               PET_BASELINE_Y - height, width, height);
  if (talking) {
    // "((( " beside the ear: drawn after the sprite so its black background cannot cover them.
    const uint8_t waves = listenWaves(now);
    for (uint8_t wave = 0; wave < waves; ++wave) {
      const int radius = 10 + wave * 8;
      canvas.fillArc(LISTEN_WAVE_X, LISTEN_WAVE_Y, radius, radius - 3, 130, 230, COLOR_SKY);
    }
  }
}
}  // namespace ui

uint8_t petDirectionAt(int x, int y) {
  const int dx = x - PET_CENTER_X, dy = y - PET_FACE_Y;
  if (dx * dx + dy * dy < 28 * 28) return POSE_FRAME_FRONT;
  // One gaze frame per 45 degree sector around the face, starting to the right.
  constexpr uint8_t directions[] = {5, 8, 7, 6, 3, 0, 1, 2};
  const int sector = (static_cast<int>(roundf(atan2f(dy, dx) / 0.785398163F)) + 8) % 8;
  return directions[sector];
}

uint32_t petVisualFrame(const AppState &state, uint32_t now) {
  using ui::expressionFrame;
  const PetState &pet = state.pet;
  if (!petAssetsReady() || pet.mascot >= petAssetCount()) return 0;
  const PetAsset &asset = petAsset(pet.mascot);
  if (asset.kind == PET_ASSET_ANIMATION) return animationFrameAt(asset, now - pet.animationStartedAt);
  if (pet.reacting) {
    // A short blink leads into every reaction except dizziness.
    const bool blink = pet.expression != PetExpression::Dizzy && now - pet.reactedAt < 120;
    return expressionFrame(blink ? PetExpression::Blink : pet.expression);
  }
  if (ui::talkAnimating(state)) {
    switch (talkPhase()) {
      case TalkPhase::Hearing:
        return 3;  // turns towards the sound waves
      case TalkPhase::Talking: {
        // Lip-sync: the louder the syllable, the wider the mouth.
        const uint8_t loudness = talkLevel();
        return loudness >= 2 ? expressionFrame(PetExpression::Joy)
             : loudness == 1 ? expressionFrame(PetExpression::Surprise)
             : POSE_FRAME_FRONT;
      }
      default: {
        if (now % 4800 < 150) return expressionFrame(PetExpression::Blink);
        constexpr uint8_t sway[] = {0, 1, 2, 1};  // a curious head tilt from side to side
        return sway[(now / 700) % 4];
      }
    }
  }
  if (now - state.lastInteraction > ui::PET_SLEEP_AFTER) return expressionFrame(PetExpression::Sleepy);
  if (now % 4800 < 150) return expressionFrame(PetExpression::Blink);
  if (state.touch.down) return pet.direction;
  constexpr uint8_t idleDirections[] = {4, 4, 3, 4, 4, 5};
  return idleDirections[(now / 2800) % 6];
}
