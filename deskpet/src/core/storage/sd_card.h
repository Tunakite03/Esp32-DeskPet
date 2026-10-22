#pragma once
#include <stdint.h>

enum class SdCardError : uint8_t {
  None, Pins, Mount, Card, Directory, Open, Write, Read, Verify, Remove
};

struct SdCardStatus {
  bool mounted = false;
  bool writable = false;
  bool bootRecordValid = false;
  uint8_t busWidth = 0;
  uint64_t capacityBytes = 0;
  SdCardError error = SdCardError::None;
};

struct SdCardTestResult {
  SdCardError error = SdCardError::None;
  uint16_t bytes = 0;
  bool cleanup = false;
};

// Mounts SD_MMC at /sdcard without formatting and verifies /deskpet/boot.json.
// Call once during setup. An unavailable card never prevents the app from booting.
void initSdCard();
SdCardStatus sdCardStatus();
const char *sdCardErrorName(SdCardError error);
// Writes, reopens, compares and removes a unique 128-byte file under /deskpet.
// The console runs this only while music is stopped. The FAT driver serializes
// filesystem access; music reads run on a separate worker after setup.
SdCardTestResult testSdCard();
