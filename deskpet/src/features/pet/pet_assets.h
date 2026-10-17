#pragma once
#include <stdint.h>

// Sprite artwork: page-mascot, MIT, Copyright (c) 2026 Kamran Ahmed.
// The complete license is included in assets/page-mascot/LICENSE and pets.bin.

// A posed companion: 9 gaze directions (3x3 grid) followed by one frame per PetExpression.
constexpr uint16_t PET_ASSET_POSES = 0;
// A looping animation with per-frame end times.
constexpr uint16_t PET_ASSET_ANIMATION = 1;

constexpr uint32_t POSE_FRAME_FRONT = 4;     // looking straight at the viewer
constexpr uint32_t REACTION_FRAME_BASE = 9;  // first expression frame

struct PetAsset {
  char name[24]{};
  uint16_t kind = PET_ASSET_POSES, side = 0;
  uint32_t frames = 0, table = 0, duration = 0;
};

// Alarm ringtone carried by a version 2 pack: mono 16 kHz PCM, memory-mapped
// from flash. `count` is 0 when the pack has none.
struct AssetRingtone {
  const int16_t *samples = nullptr;
  uint32_t count = 0;
};
constexpr uint32_t MAX_ASSET_RINGTONE_SAMPLES = 16000UL * 30;

// Maps and validates the asset partition. Safe to call once at boot.
bool initPetAssets();
AssetRingtone assetRingtone();
bool petAssetsReady();
uint8_t petAssetCount();
uint32_t petAssetBytes();
const PetAsset &petAsset(uint8_t index);
// Decodes every frame and prints a CRC over the pixels (serial "asset-selftest").
void testPetAssets();
uint32_t animationFrameAt(const PetAsset &asset, uint32_t elapsed);
// Microseconds spent decoding companion frames since the last call.
uint32_t takePetDecodeMicros();
// Draws a frame scaled (nearest neighbour) into a SCREEN_WIDTH x SCREEN_HEIGHT framebuffer.
void drawPetAsset(uint16_t *framebuffer, uint8_t index, uint32_t frame, int x, int y, int width, int height);
