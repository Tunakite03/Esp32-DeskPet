#include "src/core/storage/sd_card.h"

#include <Arduino.h>
#include <SD_MMC.h>
#include <esp_random.h>
#include <esp_system.h>
#include <cstring>
#include "src/core/system/config.h"

namespace {
// ES3C28P SDIO socket: CLK, CMD, DATA0..DATA3 (independent of the LCD SPI bus).
constexpr int PIN_SD_CLK = 38, PIN_SD_CMD = 40;
constexpr int PIN_SD_D0 = 39, PIN_SD_D1 = 41, PIN_SD_D2 = 48, PIN_SD_D3 = 47;
constexpr char DATA_DIRECTORY[] = "/deskpet";
constexpr char BOOT_RECORD[] = "/deskpet/boot.json";
constexpr size_t TEST_BYTES = 128;
SdCardStatus status;

void recordError(SdCardError error, const char *operation, const char *path) {
  status.error = error;
  status.writable = false;
  Serial.printf("{\"level\":\"error\",\"component\":\"sd\",\"operation\":\"%s\","
                "\"error\":\"%s\",\"path\":\"%s\"}\n", operation, sdCardErrorName(error), path);
}

SdCardError verifyFile(const char *path, const uint8_t *expected, size_t length) {
  File file = SD_MMC.open(path, FILE_READ);
  if (!file || file.isDirectory()) { file.close(); return SdCardError::Open; }
  if (file.size() != length) { file.close(); return SdCardError::Verify; }
  uint8_t buffer[64];
  for (size_t offset = 0; offset < length;) {
    const size_t count = min(sizeof(buffer), length - offset);
    if (file.read(buffer, count) != count) { file.close(); return SdCardError::Read; }
    if (memcmp(buffer, expected + offset, count) != 0) { file.close(); return SdCardError::Verify; }
    offset += count;
  }
  file.close();
  return SdCardError::None;
}

SdCardError writeAndVerifyFile(const char *path, const uint8_t *data, size_t length) {
  File file = SD_MMC.open(path, FILE_WRITE);
  if (!file || file.isDirectory()) { file.close(); return SdCardError::Open; }
  const size_t written = file.write(data, length);
  file.flush();
  file.close();
  if (written != length) return SdCardError::Write;
  // Reopening the file also checks that its length and contents survived close.
  return verifyFile(path, data, length);
}

bool mountCard() {
  // Two 4-bit attempts, then a 1-bit attempt for cards with unstable data lines.
  constexpr uint8_t WIDTHS[] = {4, 4, 1};
  for (uint8_t attempt = 0; attempt < sizeof(WIDTHS); ++attempt) {
    const uint8_t width = WIDTHS[attempt];
    if (SD_MMC.begin("/sdcard", width == 1, false, SDMMC_FREQ_DEFAULT, 3)) {
      status.mounted = true;
      status.busWidth = width;
      return true;
    }
    SD_MMC.end();
    Serial.printf("{\"level\":\"warning\",\"component\":\"sd\",\"operation\":\"mount\","
                  "\"attempt\":%u,\"bus_width\":%u,\"reason\":\"Card did not mount; check insertion and FAT filesystem\"}\n",
                  attempt + 1, width);
    if (attempt + 1 < sizeof(WIDTHS)) delay(100);
  }
  recordError(SdCardError::Mount, "mount", "/sdcard");
  return false;
}

bool prepareDirectory() {
  if (!SD_MMC.exists(DATA_DIRECTORY) && !SD_MMC.mkdir(DATA_DIRECTORY)) return false;
  File directory = SD_MMC.open(DATA_DIRECTORY, FILE_READ);
  const bool ready = directory && directory.isDirectory();
  directory.close();
  return ready;
}

void writeBootRecord() {
  char record[192];
  const int length = snprintf(record, sizeof(record),
                              "{\"firmware\":\"%s\",\"reset_reason\":%d,\"sd_bus_width\":%u,"
                              "\"sd_capacity_bytes\":%llu}\n", VERSION, static_cast<int>(esp_reset_reason()),
                              status.busWidth, static_cast<unsigned long long>(status.capacityBytes));
  if (length <= 0 || static_cast<size_t>(length) >= sizeof(record)) {
    recordError(SdCardError::Write, "format_boot_record", BOOT_RECORD);
    return;
  }
  const SdCardError error = writeAndVerifyFile(BOOT_RECORD, reinterpret_cast<const uint8_t *>(record),
                                               static_cast<size_t>(length));
  if (error != SdCardError::None) {
    recordError(error, "write_boot_record", BOOT_RECORD);
    return;
  }
  status.bootRecordValid = true;
  status.writable = true;
}
}  // namespace

const char *sdCardErrorName(SdCardError error) {
  switch (error) {
    case SdCardError::None: return "none";
    case SdCardError::Pins: return "SDIO pin configuration failed";
    case SdCardError::Mount: return "Card missing, unreadable, or unsupported filesystem; formatting disabled";
    case SdCardError::Card: return "Mounted card has no valid type or capacity";
    case SdCardError::Directory: return "Cannot create or open the application directory";
    case SdCardError::Open: return "Cannot open the requested file";
    case SdCardError::Write: return "File write was incomplete or could not be prepared";
    case SdCardError::Read: return "File read was incomplete";
    case SdCardError::Verify: return "Stored file size or contents do not match the written data";
    case SdCardError::Remove: return "Cannot remove the temporary test file";
  }
  return "Invalid SD error code";
}

void initSdCard() {
  if (status.mounted) return;
  status = SdCardStatus{};
  if (!SD_MMC.setPins(PIN_SD_CLK, PIN_SD_CMD, PIN_SD_D0, PIN_SD_D1, PIN_SD_D2, PIN_SD_D3)) {
    recordError(SdCardError::Pins, "configure_pins", "/sdcard");
    return;
  }
  if (!mountCard()) return;
  status.capacityBytes = SD_MMC.cardSize();
  if (SD_MMC.cardType() == CARD_NONE || status.capacityBytes == 0) {
    SD_MMC.end();
    status.mounted = false;
    status.busWidth = 0;
    recordError(SdCardError::Card, "identify_card", "/sdcard");
    return;
  }
  if (!prepareDirectory()) {
    recordError(SdCardError::Directory, "prepare_directory", DATA_DIRECTORY);
    return;
  }
  writeBootRecord();
}

SdCardStatus sdCardStatus() { return status; }

SdCardTestResult testSdCard() {
  if (!status.mounted) return {status.error == SdCardError::None ? SdCardError::Mount : status.error, 0, false};
  char path[64];
  snprintf(path, sizeof(path), "/deskpet/.check-%08lx.tmp", static_cast<unsigned long>(esp_random()));
  if (SD_MMC.exists(path)) {
    recordError(SdCardError::Open, "test_path_already_exists", path);
    return {SdCardError::Open, 0, false};
  }
  uint8_t payload[TEST_BYTES];
  for (size_t index = 0; index < sizeof(payload); ++index) payload[index] = (index * 37U) ^ 0xa5U;
  SdCardError error = writeAndVerifyFile(path, payload, sizeof(payload));
  const bool cleanup = SD_MMC.remove(path);
  if (!cleanup && error == SdCardError::None) error = SdCardError::Remove;
  if (error != SdCardError::None) recordError(error, "file_selftest", path);
  else { status.error = SdCardError::None; status.writable = true; }
  return {error, static_cast<uint16_t>(error == SdCardError::None ? TEST_BYTES : 0), cleanup};
}
