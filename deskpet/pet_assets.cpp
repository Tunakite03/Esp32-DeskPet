#include "pet_assets.h"

#include <Arduino.h>
#include <esp_partition.h>
#include <esp_rom_crc.h>
#include "config.h"
#include "log.h"

namespace {
constexpr uint8_t MAX_PET_ASSETS = 16;
constexpr uint32_t HEADER_BYTES = 24, ENTRY_BYTES = 40, FRAME_RECORD_BYTES = 12;

PetAsset assets[MAX_PET_ASSETS];
uint8_t assetCount = 0;
const uint8_t *assetData = nullptr;
uint32_t assetBytes = 0;
uint16_t *framePixels = nullptr;
bool ready = false;
esp_partition_mmap_handle_t assetMapping = 0;
int cachedAsset = -1;
uint32_t cachedFrame = UINT32_MAX;

uint16_t assetWord(const uint8_t *data) { return data[0] | (static_cast<uint16_t>(data[1]) << 8); }
uint32_t assetDword(const uint8_t *data) {
  return assetWord(data) | (static_cast<uint32_t>(assetWord(data + 2)) << 16);
}

bool assetFailure(const char *reason) {
  logError("pet_assets", reason); ready = false; return false;
}

bool validEntry(const PetAsset &asset) {
  if (asset.name[23] != '\0' || asset.kind > PET_ASSET_ANIMATION || asset.frames == 0 || asset.frames > 2000 ||
      asset.table > assetBytes || asset.frames * FRAME_RECORD_BYTES > assetBytes - asset.table) {
    return assetFailure("Invalid asset directory entry");
  }
  const bool posesValid = asset.side == 144 && asset.frames == 18 && asset.duration == 0;
  const bool animationValid = asset.side == 64 && asset.duration != 0 && asset.duration <= 120000;
  if (asset.kind == PET_ASSET_POSES ? !posesValid : !animationValid) {
    return assetFailure("Unsupported asset dimensions or duration");
  }
  uint32_t previousEnd = 0;
  for (uint32_t frame = 0; frame < asset.frames; ++frame) {
    const uint8_t *record = assetData + asset.table + frame * FRAME_RECORD_BYTES;
    const uint32_t offset = assetDword(record), length = assetDword(record + 4), end = assetDword(record + 8);
    const bool timingValid = asset.kind != PET_ASSET_ANIMATION || (end > previousEnd && end <= asset.duration);
    if (offset >= assetBytes || length < 3 || length > assetBytes - offset || !timingValid) {
      return assetFailure("Invalid animation frame bounds or timing");
    }
    previousEnd = end;
  }
  if (asset.kind == PET_ASSET_ANIMATION && previousEnd != asset.duration) {
    return assetFailure("Animation duration does not match frame table");
  }
  return true;
}

// Frames are run-length encoded, either as raw RGB565 samples (encoding 0) or
// as indices into a per-frame palette of up to 256 colours (encoding 1).
bool decodeFrame(uint8_t index, uint32_t frame) {
  if (!ready || index >= assetCount || frame >= assets[index].frames) return false;
  if (cachedAsset == index && cachedFrame == frame) return true;
  const PetAsset &asset = assets[index];
  const uint8_t *record = assetData + asset.table + frame * FRAME_RECORD_BYTES;
  const uint8_t *cursor = assetData + assetDword(record);
  const uint8_t *end = cursor + assetDword(record + 4);
  const uint8_t encoding = *cursor++;
  uint16_t palette[256]{};
  uint16_t paletteCount = 0;
  if (encoding == 1) {
    if (end - cursor < 2) return assetFailure("Truncated frame palette");
    paletteCount = assetWord(cursor); cursor += 2;
    if (paletteCount == 0 || paletteCount > 256 || end - cursor < paletteCount * 2) {
      return assetFailure("Invalid frame palette size");
    }
    for (uint16_t color = 0; color < paletteCount; ++color) { palette[color] = assetWord(cursor); cursor += 2; }
  } else if (encoding != 0) {
    return assetFailure("Unknown frame encoding");
  }
  const uint8_t sampleSize = encoding == 1 ? 1 : 2;
  uint32_t written = 0;
  const uint32_t expected = asset.side * asset.side;
  while (cursor < end) {
    if (end - cursor < 2) return assetFailure("Truncated frame run");
    const uint16_t tag = assetWord(cursor); cursor += 2;
    const uint16_t count = tag & 0x7FFF;
    const bool repeated = (tag & 0x8000) != 0;
    const uint32_t samples = repeated ? 1 : count;
    if (count == 0 || count > expected - written || static_cast<uint32_t>(end - cursor) < samples * sampleSize) {
      return assetFailure("Frame run exceeds pixel or byte bounds");
    }
    for (uint32_t sample = 0; sample < samples; ++sample) {
      const uint16_t value = encoding == 1 ? *cursor : assetWord(cursor);
      cursor += sampleSize;
      if (encoding == 1 && value >= paletteCount) return assetFailure("Frame palette index out of range");
      const uint16_t color = encoding == 1 ? palette[value] : value;
      if (repeated) {
        for (uint16_t pixel = 0; pixel < count; ++pixel) framePixels[written++] = color;
      } else {
        framePixels[written++] = color;
      }
    }
  }
  if (written != expected) return assetFailure("Decoded frame is incomplete");
  cachedAsset = index; cachedFrame = frame;
  return true;
}
}  // namespace

bool initPetAssets() {
  const esp_partition_t *partition = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, static_cast<esp_partition_subtype_t>(ASSET_PARTITION_SUBTYPE), ASSET_PARTITION_LABEL);
  uint8_t header[HEADER_BYTES]{};
  if (!partition || esp_partition_read(partition, 0, header, sizeof(header)) != ESP_OK) {
    return assetFailure("Asset partition could not be read");
  }
  assetBytes = assetDword(header + 12);
  const uint32_t count = assetDword(header + 20);
  if (memcmp(header, "DPETAS01", 8) != 0 || assetDword(header + 8) != 1 || assetBytes < HEADER_BYTES ||
      assetBytes > partition->size || count < 3 || count > MAX_PET_ASSETS ||
      HEADER_BYTES + count * ENTRY_BYTES > assetBytes) {
    return assetFailure("Invalid asset pack header or capacity");
  }
  const void *mapped = nullptr;
  if (esp_partition_mmap(partition, 0, assetBytes, ESP_PARTITION_MMAP_DATA, &mapped, &assetMapping) != ESP_OK) {
    return assetFailure("Unable to map asset partition");
  }
  assetData = static_cast<const uint8_t *>(mapped);
  if (esp_rom_crc32_le(0, assetData + HEADER_BYTES, assetBytes - HEADER_BYTES) != assetDword(header + 16)) {
    return assetFailure("Asset pack CRC mismatch");
  }
  assetCount = count;
  for (uint8_t index = 0; index < assetCount; ++index) {
    const uint8_t *entry = assetData + HEADER_BYTES + index * ENTRY_BYTES;
    PetAsset &asset = assets[index];
    memcpy(asset.name, entry, 24);
    asset.kind = assetWord(entry + 24);
    asset.side = assetWord(entry + 26);
    asset.frames = assetDword(entry + 28);
    asset.table = assetDword(entry + 32);
    asset.duration = assetDword(entry + 36);
    if (!validEntry(asset)) return false;
  }
  framePixels = static_cast<uint16_t *>(ps_malloc(144 * 144 * sizeof(uint16_t)));
  if (!framePixels) return assetFailure("Animation frame allocation failed");
  ready = true;
  Serial.printf("{\"event\":\"pet_assets_ready\",\"count\":%u,\"bytes\":%lu}\n",
                assetCount, static_cast<unsigned long>(assetBytes));
  return true;
}

bool petAssetsReady() { return ready; }
uint8_t petAssetCount() { return assetCount; }
uint32_t petAssetBytes() { return assetBytes; }
const PetAsset &petAsset(uint8_t index) { return assets[index < MAX_PET_ASSETS ? index : 0]; }

void testPetAssets() {
  if (!ready) { logError("pet_assets", "Cannot test unavailable assets"); return; }
  uint32_t crc = 0, frames = 0;
  for (uint8_t index = 0; index < assetCount; ++index) {
    for (uint32_t frame = 0; frame < assets[index].frames; ++frame) {
      if (!decodeFrame(index, frame)) return;
      const uint32_t side = assets[index].side;
      crc = esp_rom_crc32_le(crc, reinterpret_cast<const uint8_t *>(framePixels), side * side * 2);
      ++frames;
      delay(1);
    }
  }
  Serial.printf("{\"asset_test\":\"pass\",\"frames\":%lu,\"decoded_crc32\":%lu}\n",
                static_cast<unsigned long>(frames), static_cast<unsigned long>(crc));
}

uint32_t animationFrameAt(const PetAsset &asset, uint32_t elapsed) {
  const uint32_t phase = elapsed % asset.duration;
  uint32_t low = 0, high = asset.frames;
  while (low < high) {
    const uint32_t middle = low + (high - low) / 2;
    const uint32_t end = assetDword(assetData + asset.table + middle * FRAME_RECORD_BYTES + 8);
    if (phase < end) high = middle; else low = middle + 1;
  }
  return min(low, asset.frames - 1);
}

void drawPetAsset(uint16_t *framebuffer, uint8_t index, uint32_t frame, int x, int y, int width, int height) {
  if (!decodeFrame(index, frame)) return;
  const int side = assets[index].side;
  for (int row = 0; row < height; ++row) {
    if (y + row < 0 || y + row >= SCREEN_HEIGHT) continue;
    const uint16_t *source = framePixels + row * side / height * side;
    for (int column = 0; column < width; ++column) {
      if (x + column >= 0 && x + column < SCREEN_WIDTH) {
        framebuffer[(y + row) * SCREEN_WIDTH + x + column] = source[column * side / width];
      }
    }
  }
}
