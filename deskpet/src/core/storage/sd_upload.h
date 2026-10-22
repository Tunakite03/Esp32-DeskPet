#pragma once
#include <Arduino.h>

// Receives one music file over the serial console (see tools/upload_music.py).
// The file is written to /music/<name>.part, read back and checked against the
// CRC32 the host sent, and only then renamed, so a broken upload never appears
// in the library. Every call prints one JSON line.
void beginSdUpload(uint32_t bytes, const String &name);
// One chunk of base64 text, at most 384 decoded bytes.
void writeSdUploadChunk(const String &base64);
// Checks the stored file against the 8-digit hex CRC32 and renames it into place.
void finishSdUpload(const String &crc32Hex);
void abortSdUpload();
