#include "src/core/storage/sd_upload.h"

#include <SD_MMC.h>
#include <mbedtls/base64.h>
#include <cstdlib>
#include <cstring>
#include "src/core/storage/sd_card.h"
#include "src/features/music/music.h"

namespace {
constexpr uint32_t MAX_UPLOAD_BYTES = 96UL * 1024 * 1024;
constexpr size_t MAX_NAME = 120;  // keeps the full path inside MusicTrack::path
constexpr size_t CHUNK_BYTES = 384;  // decoded bytes per chunk, 512 base64 characters
constexpr char MUSIC_DIRECTORY[] = "/music";

struct Upload {
  bool active = false;
  File file;
  String name, partPath, finalPath;
  uint32_t expected = 0, received = 0;
  uint32_t crc = 0xFFFFFFFFUL;  // running CRC32 of the bytes received, not inverted yet
};
Upload upload;

void fail(const char *message) {
  Serial.printf("{\"sd_put\":\"error\",\"error\":\"%s\"}\n", message);
}

// CRC32 (IEEE, reflected) lookup table, built at compile time.
struct CrcTable {
  uint32_t values[256];
  constexpr CrcTable() : values() {
    for (uint32_t index = 0; index < 256; ++index) {
      uint32_t crc = index;
      for (int bit = 0; bit < 8; ++bit) crc = (crc & 1) ? (0xEDB88320UL ^ (crc >> 1)) : (crc >> 1);
      values[index] = crc;
    }
  }
};
constexpr CrcTable CRC_TABLE{};

// CRC32 over data, continuing from a state that is not yet inverted.
uint32_t crc32Update(uint32_t state, const uint8_t *data, size_t length) {
  for (size_t index = 0; index < length; ++index) state = CRC_TABLE.values[(state ^ data[index]) & 0xFF] ^ (state >> 8);
  return state;
}

// Reading back a large track takes longer than the loop watchdog allows.
struct WatchdogPause {
  WatchdogPause() { disableLoopWDT(); }
  ~WatchdogPause() { enableLoopWDT(); }
};

// Names are plain ASCII: no folders, no characters the FAT driver or the JSON replies dislike.
bool validName(const String &name) {
  if (name.length() < 5 || name.length() > MAX_NAME || name[0] == '.') return false;
  for (size_t index = 0; index < name.length(); ++index) {
    const char c = name[index];
    if (c < 32 || c > 126 || strchr("/\\\"*?:<>|", c)) return false;
  }
  String lower = name;
  lower.toLowerCase();
  return lower.endsWith(".mp3") || lower.endsWith(".wav");
}

// Closes the open file and deletes its temporary copy. Safe to call when idle.
void discardUpload() {
  if (upload.file) upload.file.close();
  if (upload.active) SD_MMC.remove(upload.partPath);
  upload = Upload{};
}

// Reads the stored file back and reports its size and CRC32 (inverted, like the host's).
bool readBack(const String &path, uint32_t &bytes, uint32_t &crc) {
  File file = SD_MMC.open(path, FILE_READ);
  if (!file || file.isDirectory()) return false;
  uint8_t block[512];
  uint32_t state = 0xFFFFFFFFUL;
  bytes = 0;
  for (;;) {
    const size_t count = file.read(block, sizeof(block));
    if (count == 0) break;
    state = crc32Update(state, block, count);
    bytes += count;
  }
  file.close();
  crc = state ^ 0xFFFFFFFFUL;
  return true;
}
}  // namespace

void beginSdUpload(uint32_t bytes, const String &name) {
  discardUpload();
  const SdCardStatus sd = sdCardStatus();
  if (!sd.mounted || !sd.writable) { fail("SD card is not mounted and writable"); return; }
  if (musicActive() || musicStatus().phase == MusicPhase::Scanning) { fail("Stop music before uploading"); return; }
  if (bytes == 0 || bytes > MAX_UPLOAD_BYTES) { fail("Size must be between 1 and 100663296 bytes"); return; }
  if (!validName(name)) { fail("Use a .mp3 or .wav name with ASCII characters and no folders"); return; }
  if (!SD_MMC.exists(MUSIC_DIRECTORY) && !SD_MMC.mkdir(MUSIC_DIRECTORY)) { fail("Cannot create /music"); return; }
  upload.finalPath = String(MUSIC_DIRECTORY) + "/" + name;
  upload.partPath = upload.finalPath + ".part";
  if (SD_MMC.exists(upload.partPath)) SD_MMC.remove(upload.partPath);
  upload.file = SD_MMC.open(upload.partPath, FILE_WRITE);
  if (!upload.file || upload.file.isDirectory()) {
    upload.file.close();
    SD_MMC.remove(upload.partPath);
    upload = Upload{};
    fail("Cannot create the temporary file");
    return;
  }
  upload.name = name;
  upload.expected = bytes;
  upload.active = true;
  Serial.printf("{\"sd_put\":\"ready\",\"name\":\"%s\",\"bytes\":%lu}\n", name.c_str(), static_cast<unsigned long>(bytes));
}

void writeSdUploadChunk(const String &base64) {
  if (!upload.active) { fail("No upload is open"); return; }
  uint8_t bytes[CHUNK_BYTES];
  size_t decoded = 0;
  const bool valid = base64.length() > 0 && base64.length() <= CHUNK_BYTES / 3 * 4 && base64.length() % 4 == 0 &&
                     mbedtls_base64_decode(bytes, sizeof(bytes), &decoded,
                                           reinterpret_cast<const uint8_t *>(base64.c_str()), base64.length()) == 0;
  if (!valid) { discardUpload(); fail("Invalid base64 chunk; upload cancelled"); return; }
  if (decoded > upload.expected - upload.received) { discardUpload(); fail("More data than announced; upload cancelled"); return; }
  if (upload.file.write(bytes, decoded) != decoded) { discardUpload(); fail("SD write failed; upload cancelled"); return; }
  upload.crc = crc32Update(upload.crc, bytes, decoded);
  upload.received += decoded;
  Serial.printf("{\"sd_put\":\"ok\",\"received\":%lu}\n", static_cast<unsigned long>(upload.received));
}

void finishSdUpload(const String &crc32Hex) {
  if (!upload.active) { fail("No upload is open"); return; }
  WatchdogPause pause;
  char *end = nullptr;
  const unsigned long expectedCrc = strtoul(crc32Hex.c_str(), &end, 16);
  if (crc32Hex.length() != 8 || *end != '\0') { discardUpload(); fail("CRC32 must be 8 hex digits; upload cancelled"); return; }
  upload.file.flush();
  upload.file.close();
  const uint32_t sentCrc = upload.crc ^ 0xFFFFFFFFUL;
  if (upload.received != upload.expected || sentCrc != expectedCrc) {
    discardUpload();
    fail("Received data does not match the announced size or CRC32; upload cancelled");
    return;
  }
  // The checksum must match what is on the card now, not only what arrived over serial.
  uint32_t storedBytes = 0, storedCrc = 0;
  if (!readBack(upload.partPath, storedBytes, storedCrc) || storedBytes != upload.expected || storedCrc != sentCrc) {
    discardUpload();
    fail("Stored file does not match the upload; upload cancelled");
    return;
  }
  if (SD_MMC.exists(upload.finalPath) && !SD_MMC.remove(upload.finalPath)) {
    discardUpload();
    fail("Cannot replace the existing track");
    return;
  }
  if (!SD_MMC.rename(upload.partPath, upload.finalPath)) {
    discardUpload();
    fail("Cannot rename the uploaded file");
    return;
  }
  Serial.printf("{\"sd_put\":\"done\",\"name\":\"%s\",\"bytes\":%lu,\"crc32\":\"%08lx\"}\n", upload.name.c_str(),
                static_cast<unsigned long>(upload.expected), static_cast<unsigned long>(sentCrc));
  upload = Upload{};
  if (musicStatus().ready) scanMusic();  // the new track appears in the library straight away
}

void abortSdUpload() {
  const bool wasActive = upload.active;
  discardUpload();
  Serial.printf("{\"sd_put\":\"aborted\",\"was_active\":%s}\n", wasActive ? "true" : "false");
}

void deleteSdMusic(const String &name) {
  discardUpload();
  if (musicActive()) stopMusic();
  const String path = name.startsWith("/") ? name : String(MUSIC_DIRECTORY) + "/" + name;
  if (!SD_MMC.exists(path)) {
    Serial.printf("{\"sd_delete\":\"error\",\"error\":\"not_found\",\"path\":\"%s\"}\n", path.c_str());
    return;
  }
  const bool ok = SD_MMC.remove(path);
  Serial.printf("{\"sd_delete\":\"%s\",\"path\":\"%s\"}\n", ok ? "ok" : "error", path.c_str());
  if (ok && musicStatus().ready) scanMusic();
}
