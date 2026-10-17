// Reader and writer for the DeskPet asset pack (pets.bin).
//
// The layout and encoding match deskpet/tools/build_pet_assets.py byte for
// byte, and parsePack() repeats every check that
// deskpet/src/features/pet/pet_assets.cpp makes at boot, so a pack this module
// accepts is a pack the board accepts.
//
// Version 1: header (24 bytes) | directory | frame tables | frames | trailer
// Version 2: header (24 bytes) | ringtone offset, sample count (8 bytes) |
//            directory | frame tables | frames | ringtone (4-byte aligned) | trailer
// Firmware 1.9.0 reads both; older firmware only reads version 1, so a pack
// without a ringtone is always written as version 1.

export const PARTITION_OFFSET = 0x610000;
export const PARTITION_SIZE = 0x9e0000;
export const PARTITION_LABEL = "assets";
export const PARTITION_SUBTYPE = 0x40;
export const KIND_POSES = 0;
export const KIND_ANIMATION = 1;
export const POSE_SIDE = 144;
export const POSE_FRAMES = 18;
export const ANIMATION_SIDE = 64;
export const MAX_ANIMATION_FRAMES = 2000;
export const MAX_ANIMATION_MS = 120000;
export const MIN_PETS = 3;
export const MAX_PETS = 16;
export const MAX_NAME_LENGTH = 12;
export const SAMPLE_RATE = 16000;
export const MAX_RINGTONE_SAMPLES = SAMPLE_RATE * 30;
// Gaze directions (3x3 grid, front is index 4) then one frame per PetExpression in state.h.
export const POSE_LABELS = Object.freeze([
  "Up left", "Up", "Up right", "Left", "Front", "Right", "Down left", "Down", "Down right",
  "Blink", "Love", "Sparkle", "Surprise", "Wink", "Bashful", "Sleepy", "Dizzy", "Joy",
]);
export const POSE_FRAME_FRONT = 4;

const MAGIC = "DPETAS01";
const HEADER_BYTES = 24;
const HEADER_V2_BYTES = 32;
const ENTRY_BYTES = 40;
const FRAME_RECORD_BYTES = 12;
const NAME_BYTES = 24;
const MAX_RUN = 32767;
const MAX_LITERAL = 32765;

/** Raised for any pack, frame or partition table the board would reject. */
export class PackError extends Error {
  /** @param {string} message */
  constructor(message) {
    super(message);
    this.name = "PackError";
  }
}

/**
 * @typedef {{ data: Uint8Array, end: number }} PackFrame
 *   One encoded frame; `end` is the frame's end time in ms (0 for posed companions).
 * @typedef {{ name: string, kind: number, side: number, duration: number, frames: PackFrame[] }} PackPet
 * @typedef {{ pets: PackPet[], ringtone: Int16Array | null, trailer: Uint8Array }} Pack
 * @typedef {{ label: string, type: number, subtype: number, offset: number, size: number }} PartitionEntry
 */

const CRC_TABLE = (() => {
  const table = new Uint32Array(256);
  for (let n = 0; n < 256; ++n) {
    let c = n;
    for (let k = 0; k < 8; ++k) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    table[n] = c >>> 0;
  }
  return table;
})();

/**
 * zlib-compatible CRC-32 (the same value as esp_rom_crc32_le(0, ...)).
 * @param {Uint8Array} bytes
 * @returns {number}
 */
export function crc32(bytes) {
  let crc = 0xffffffff;
  for (let i = 0; i < bytes.length; ++i) crc = CRC_TABLE[(crc ^ bytes[i]) & 0xff] ^ (crc >>> 8);
  return (crc ^ 0xffffffff) >>> 0;
}

/**
 * Straight-alpha RGBA8888 composited over black, as RGB565.
 * @param {Uint8ClampedArray | Uint8Array} rgba
 * @returns {Uint16Array}
 */
export function rgbaToRgb565(rgba) {
  const pixels = new Uint16Array(rgba.length / 4);
  for (let p = 0, i = 0; p < pixels.length; ++p, i += 4) {
    const alpha = rgba[i + 3];
    const r = Math.round((rgba[i] * alpha) / 255);
    const g = Math.round((rgba[i + 1] * alpha) / 255);
    const b = Math.round((rgba[i + 2] * alpha) / 255);
    pixels[p] = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
  }
  return pixels;
}

/**
 * @param {Uint16Array} pixels
 * @returns {Uint8ClampedArray} opaque RGBA8888
 */
export function rgb565ToRgba(pixels) {
  const rgba = new Uint8ClampedArray(pixels.length * 4);
  for (let p = 0, i = 0; p < pixels.length; ++p, i += 4) {
    const r = (pixels[p] >> 11) & 0x1f;
    const g = (pixels[p] >> 5) & 0x3f;
    const b = pixels[p] & 0x1f;
    rgba[i] = (r << 3) | (r >> 2);
    rgba[i + 1] = (g << 2) | (g >> 4);
    rgba[i + 2] = (b << 3) | (b >> 2);
    rgba[i + 3] = 255;
  }
  return rgba;
}

/** Little-endian byte sink. */
class ByteWriter {
  constructor() {
    /** @type {number[]} */
    this.bytes = [];
  }
  /** @param {number} value */
  u8(value) {
    this.bytes.push(value & 0xff);
  }
  /** @param {number} value */
  u16(value) {
    this.bytes.push(value & 0xff, (value >>> 8) & 0xff);
  }
  /** @param {number} value @param {number} size */
  sample(value, size) {
    if (size === 1) this.u8(value);
    else this.u16(value);
  }
  /** @returns {Uint8Array} */
  finish() {
    return Uint8Array.from(this.bytes);
  }
}

/**
 * Run-length encoding from build_pet_assets.encode_runs: a run of 3+ equal
 * samples is (0x8000 | count, sample); anything else is (count, samples...).
 * @param {ArrayLike<number>} values
 * @param {number} sampleSize 1 or 2 bytes
 * @param {ByteWriter} out
 */
function encodeRuns(values, sampleSize, out) {
  const n = values.length;
  let cursor = 0;
  while (cursor < n) {
    let end = cursor + 1;
    while (end < n && values[end] === values[cursor] && end - cursor < MAX_RUN) ++end;
    if (end - cursor >= 3) {
      out.u16(0x8000 | (end - cursor));
      out.sample(values[cursor], sampleSize);
      cursor = end;
      continue;
    }
    const start = cursor;
    cursor = end;
    while (cursor < n && cursor - start < MAX_LITERAL) {
      if (cursor + 2 < n && values[cursor] === values[cursor + 1] && values[cursor] === values[cursor + 2]) break;
      ++cursor;
    }
    out.u16(cursor - start);
    for (let i = start; i < cursor; ++i) out.sample(values[i], sampleSize);
  }
}

/**
 * Encodes RGB565 pixels as raw runs (encoding 0) or palette runs (encoding 1),
 * whichever is smaller, exactly like build_pet_assets.encode.
 * @param {Uint16Array} pixels
 * @returns {Uint8Array}
 */
export function encodeFrame(pixels) {
  const direct = new ByteWriter();
  direct.u8(0);
  encodeRuns(pixels, 2, direct);
  /** @type {Map<number, number>} */
  const indices = new Map();
  for (const color of pixels) {
    if (!indices.has(color)) indices.set(color, indices.size);
    if (indices.size > 256) return direct.finish();
  }
  const indexed = new ByteWriter();
  indexed.u8(1);
  indexed.u16(indices.size);
  for (const color of indices.keys()) indexed.u16(color);
  const mapped = new Uint8Array(pixels.length);
  for (let i = 0; i < pixels.length; ++i) mapped[i] = /** @type {number} */ (indices.get(pixels[i]));
  encodeRuns(mapped, 1, indexed);
  return indexed.bytes.length < direct.bytes.length ? indexed.finish() : direct.finish();
}

/**
 * Decodes one frame with the same bounds checks as decodeFrame() in pet_assets.cpp.
 * @param {Uint8Array} data
 * @param {number} side
 * @returns {Uint16Array}
 */
export function decodeFrame(data, side) {
  const view = new DataView(data.buffer, data.byteOffset, data.byteLength);
  const expected = side * side;
  const pixels = new Uint16Array(expected);
  const end = data.length;
  if (end < 3) throw new PackError("Frame is shorter than 3 bytes");
  let cursor = 1;
  const encoding = data[0];
  /** @type {Uint16Array | null} */
  let palette = null;
  if (encoding === 1) {
    const count = view.getUint16(cursor, true);
    cursor += 2;
    if (count === 0 || count > 256 || end - cursor < count * 2) throw new PackError("Invalid frame palette size");
    palette = new Uint16Array(count);
    for (let i = 0; i < count; ++i, cursor += 2) palette[i] = view.getUint16(cursor, true);
  } else if (encoding !== 0) {
    throw new PackError(`Unknown frame encoding ${encoding}`);
  }
  const sampleSize = palette ? 1 : 2;
  let written = 0;
  while (cursor < end) {
    if (end - cursor < 2) throw new PackError("Truncated frame run");
    const tag = view.getUint16(cursor, true);
    cursor += 2;
    const count = tag & 0x7fff;
    const repeated = (tag & 0x8000) !== 0;
    const samples = repeated ? 1 : count;
    if (count === 0 || count > expected - written || end - cursor < samples * sampleSize) {
      throw new PackError("Frame run exceeds pixel or byte bounds");
    }
    for (let s = 0; s < samples; ++s) {
      const value = palette ? data[cursor] : view.getUint16(cursor, true);
      cursor += sampleSize;
      if (palette && value >= palette.length) throw new PackError("Frame palette index out of range");
      const color = palette ? palette[value] : value;
      if (repeated) {
        pixels.fill(color, written, written + count);
        written += count;
      } else {
        pixels[written++] = color;
      }
    }
  }
  if (written !== expected) throw new PackError("Decoded frame is incomplete");
  return pixels;
}

/**
 * Companion names are shown with the board's ASCII fonts.
 * @param {string} name
 * @returns {string | null} why the name is rejected, or null
 */
export function nameProblem(name) {
  if (name.length < 1 || name.length > MAX_NAME_LENGTH) return `Use 1 to ${MAX_NAME_LENGTH} characters`;
  if (!/^[\x20-\x7e]+$/.test(name)) return "Use English letters, numbers and symbols only";
  return null;
}

/**
 * @param {Uint8Array} bytes
 * @returns {Pack}
 */
export function parsePack(bytes) {
  if (bytes.length < HEADER_BYTES) throw new PackError("File is too short to be an asset pack");
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const u32 = (/** @type {number} */ at) => view.getUint32(at, true);
  if (String.fromCharCode(...bytes.subarray(0, 8)) !== MAGIC) throw new PackError("Not a DeskPet asset pack");
  const version = u32(8);
  const total = u32(12);
  const count = u32(20);
  const directory = version === 2 ? HEADER_V2_BYTES : HEADER_BYTES;
  if (version !== 1 && version !== 2) throw new PackError(`Unsupported pack version ${version}`);
  if (total < directory || total > bytes.length || total > PARTITION_SIZE) {
    throw new PackError(`Pack size ${total} does not fit the file or the partition`);
  }
  if (count < MIN_PETS || count > MAX_PETS || directory + count * ENTRY_BYTES > total) {
    throw new PackError(`Pack holds ${count} companions; ${MIN_PETS} to ${MAX_PETS} are supported`);
  }
  if (crc32(bytes.subarray(HEADER_BYTES, total)) !== u32(16)) throw new PackError("Pack CRC mismatch");

  let dataEnd = directory + count * ENTRY_BYTES;
  /** @type {Int16Array | null} */
  let ringtone = null;
  if (version === 2) {
    const offset = u32(HEADER_BYTES);
    const samples = u32(HEADER_BYTES + 4);
    if (offset !== 0 || samples !== 0) {
      if (offset % 2 !== 0 || offset < directory + count * ENTRY_BYTES || offset > total || samples === 0 ||
          samples > MAX_RINGTONE_SAMPLES || samples * 2 > total - offset) {
        throw new PackError("Invalid ringtone bounds");
      }
      ringtone = new Int16Array(samples);
      for (let i = 0; i < samples; ++i) ringtone[i] = view.getInt16(offset + i * 2, true);
      dataEnd = Math.max(dataEnd, offset + samples * 2);
    }
  }

  /** @type {PackPet[]} */
  const pets = [];
  for (let index = 0; index < count; ++index) {
    const entry = directory + index * ENTRY_BYTES;
    const nameBytes = bytes.subarray(entry, entry + NAME_BYTES);
    if (nameBytes[NAME_BYTES - 1] !== 0) throw new PackError(`Companion ${index + 1} has an unterminated name`);
    const nameEnd = nameBytes.indexOf(0);
    const name = String.fromCharCode(...nameBytes.subarray(0, nameEnd));
    const kind = view.getUint16(entry + 24, true);
    const side = view.getUint16(entry + 26, true);
    const frameCount = u32(entry + 28);
    const table = u32(entry + 32);
    const duration = u32(entry + 36);
    if (kind > KIND_ANIMATION || frameCount === 0 || frameCount > MAX_ANIMATION_FRAMES || table > total ||
        frameCount * FRAME_RECORD_BYTES > total - table) {
      throw new PackError(`Invalid directory entry for ${name || `companion ${index + 1}`}`);
    }
    const posesValid = side === POSE_SIDE && frameCount === POSE_FRAMES && duration === 0;
    const animationValid = side === ANIMATION_SIDE && duration !== 0 && duration <= MAX_ANIMATION_MS;
    if (kind === KIND_POSES ? !posesValid : !animationValid) {
      throw new PackError(`Unsupported dimensions or duration for ${name}`);
    }
    /** @type {PackFrame[]} */
    const frames = [];
    let previousEnd = 0;
    for (let frame = 0; frame < frameCount; ++frame) {
      const record = table + frame * FRAME_RECORD_BYTES;
      const offset = u32(record);
      const length = u32(record + 4);
      const end = u32(record + 8);
      const timingValid = kind !== KIND_ANIMATION || (end > previousEnd && end <= duration);
      if (offset >= total || length < 3 || length > total - offset || !timingValid) {
        throw new PackError(`Invalid frame bounds or timing in ${name}`);
      }
      previousEnd = end;
      frames.push({ data: bytes.slice(offset, offset + length), end });
      dataEnd = Math.max(dataEnd, offset + length, record + FRAME_RECORD_BYTES);
    }
    if (kind === KIND_ANIMATION && previousEnd !== duration) {
      throw new PackError(`Animation duration of ${name} does not match its frames`);
    }
    pets.push({ name, kind, side, duration, frames });
  }
  return { pets, ringtone, trailer: bytes.slice(dataEnd, total) };
}

/**
 * Checks one companion the way the board will, including a full decode of every frame.
 * @param {PackPet} pet
 */
export function validatePet(pet) {
  const problem = nameProblem(pet.name);
  if (problem) throw new PackError(`Name "${pet.name}": ${problem}`);
  if (pet.kind === KIND_POSES) {
    if (pet.side !== POSE_SIDE || pet.frames.length !== POSE_FRAMES || pet.duration !== 0) {
      throw new PackError(`${pet.name}: a posed companion needs ${POSE_FRAMES} frames of ${POSE_SIDE} px`);
    }
  } else if (pet.kind === KIND_ANIMATION) {
    if (pet.side !== ANIMATION_SIDE || pet.frames.length < 1 || pet.frames.length > MAX_ANIMATION_FRAMES ||
        pet.duration < 1 || pet.duration > MAX_ANIMATION_MS) {
      throw new PackError(`${pet.name}: an animation needs 1 to ${MAX_ANIMATION_FRAMES} frames of ` +
                          `${ANIMATION_SIDE} px lasting at most ${MAX_ANIMATION_MS / 1000} s`);
    }
    let previous = 0;
    for (const frame of pet.frames) {
      if (frame.end <= previous) throw new PackError(`${pet.name}: frame end times must increase`);
      previous = frame.end;
    }
    if (previous !== pet.duration) throw new PackError(`${pet.name}: duration does not match the last frame`);
  } else {
    throw new PackError(`${pet.name}: unknown companion kind ${pet.kind}`);
  }
  for (const frame of pet.frames) decodeFrame(frame.data, pet.side);
}

/**
 * Lays out a pack in the order build_pet_assets.py uses and re-reads it with
 * parsePack() before returning it.
 * @param {Pack} pack
 * @returns {Uint8Array}
 */
export function buildPack(pack) {
  const { pets, ringtone, trailer } = pack;
  if (pets.length < MIN_PETS || pets.length > MAX_PETS) {
    throw new PackError(`Keep ${MIN_PETS} to ${MAX_PETS} companions (now ${pets.length})`);
  }
  pets.forEach(validatePet);
  if (ringtone && (ringtone.length === 0 || ringtone.length > MAX_RINGTONE_SAMPLES)) {
    throw new PackError(`The ringtone must be 1 sample to ${MAX_RINGTONE_SAMPLES / SAMPLE_RATE} s long`);
  }
  const version = ringtone ? 2 : 1;
  const directory = version === 2 ? HEADER_V2_BYTES : HEADER_BYTES;
  const frameTotal = pets.reduce((sum, pet) => sum + pet.frames.length, 0);
  const indexSize = directory + pets.length * ENTRY_BYTES + frameTotal * FRAME_RECORD_BYTES;
  const payloadSize = pets.reduce((sum, pet) => sum + pet.frames.reduce((s, f) => s + f.data.length, 0), 0);
  const ringtoneOffset = ringtone ? Math.ceil((indexSize + payloadSize) / 4) * 4 : 0;
  const ringtoneEnd = ringtone ? ringtoneOffset + ringtone.length * 2 : indexSize + payloadSize;
  const total = ringtoneEnd + trailer.length;
  if (total > PARTITION_SIZE) {
    throw new PackError(`Pack is ${total} bytes; the assets partition holds ${PARTITION_SIZE}`);
  }

  const bytes = new Uint8Array(total);
  const view = new DataView(bytes.buffer);
  let table = directory + pets.length * ENTRY_BYTES;
  let payload = indexSize;
  pets.forEach((pet, index) => {
    const entry = directory + index * ENTRY_BYTES;
    for (let i = 0; i < pet.name.length; ++i) bytes[entry + i] = pet.name.charCodeAt(i);
    view.setUint16(entry + 24, pet.kind, true);
    view.setUint16(entry + 26, pet.side, true);
    view.setUint32(entry + 28, pet.frames.length, true);
    view.setUint32(entry + 32, table, true);
    view.setUint32(entry + 36, pet.duration, true);
    for (const frame of pet.frames) {
      view.setUint32(table, payload, true);
      view.setUint32(table + 4, frame.data.length, true);
      view.setUint32(table + 8, frame.end, true);
      bytes.set(frame.data, payload);
      table += FRAME_RECORD_BYTES;
      payload += frame.data.length;
    }
  });
  if (ringtone) {
    view.setUint32(HEADER_BYTES, ringtoneOffset, true);
    view.setUint32(HEADER_BYTES + 4, ringtone.length, true);
    ringtone.forEach((sample, i) => view.setInt16(ringtoneOffset + i * 2, sample, true));
  }
  bytes.set(trailer, ringtoneEnd);
  for (let i = 0; i < MAGIC.length; ++i) bytes[i] = MAGIC.charCodeAt(i);
  view.setUint32(8, version, true);
  view.setUint32(12, total, true);
  view.setUint32(16, crc32(bytes.subarray(HEADER_BYTES)), true);
  view.setUint32(20, pets.length, true);
  parsePack(bytes);
  return bytes;
}

/**
 * Mono float samples at SAMPLE_RATE to 16-bit PCM with the 8 ms fade at both
 * ends that build_ringtone.py applies, so the loop does not click.
 * @param {Float32Array} samples
 * @returns {Int16Array}
 */
export function toRingtonePcm(samples) {
  if (samples.length === 0) throw new PackError("The ringtone is empty");
  if (samples.length > MAX_RINGTONE_SAMPLES) {
    throw new PackError(`The ringtone is longer than ${MAX_RINGTONE_SAMPLES / SAMPLE_RATE} s`);
  }
  const pcm = new Int16Array(samples.length);
  for (let i = 0; i < samples.length; ++i) pcm[i] = Math.round(Math.max(-1, Math.min(1, samples[i])) * 32767);
  const fade = Math.min(Math.floor(pcm.length / 2), (SAMPLE_RATE * 8) / 1000);
  for (let i = 0; i < fade; ++i) {
    const gain = i / fade;
    pcm[i] = Math.trunc(pcm[i] * gain);
    pcm[pcm.length - 1 - i] = Math.trunc(pcm[pcm.length - 1 - i] * gain);
  }
  return pcm;
}

/**
 * Reads an ESP-IDF partition table (flash 0x8000).
 * @param {Uint8Array} bytes
 * @returns {PartitionEntry[]}
 */
export function parsePartitionTable(bytes) {
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  /** @type {PartitionEntry[]} */
  const entries = [];
  for (let at = 0; at + 32 <= bytes.length; at += 32) {
    if (bytes[at] !== 0xaa || bytes[at + 1] !== 0x50) break;
    const labelBytes = bytes.subarray(at + 12, at + 28);
    const labelEnd = labelBytes.indexOf(0);
    entries.push({
      type: bytes[at + 2],
      subtype: bytes[at + 3],
      offset: view.getUint32(at + 4, true),
      size: view.getUint32(at + 8, true),
      label: String.fromCharCode(...labelBytes.subarray(0, labelEnd < 0 ? 16 : labelEnd)),
    });
  }
  return entries;
}

/**
 * Confirms the board has DeskPet's assets partition where this tool writes.
 * @param {PartitionEntry[]} entries
 * @param {number} packBytes
 */
export function checkAssetPartition(entries, packBytes) {
  const assets = entries.find((entry) => entry.label === PARTITION_LABEL);
  if (!assets) throw new PackError("The board has no \"assets\" partition. Flash the full DeskPet image first.");
  if (assets.type !== 1 || assets.subtype !== PARTITION_SUBTYPE || assets.offset !== PARTITION_OFFSET) {
    throw new PackError(`The "assets" partition is at 0x${assets.offset.toString(16)}, ` +
                        `not 0x${PARTITION_OFFSET.toString(16)}. Flash the full DeskPet image first.`);
  }
  if (packBytes > assets.size) throw new PackError(`The pack (${packBytes} bytes) is larger than the partition`);
}

/**
 * "DeskPet 1.9.0" → [1, 9, 0]
 * @param {string} firmware
 * @returns {number[] | null}
 */
export function firmwareVersion(firmware) {
  const match = /^DeskPet (\d+)\.(\d+)\.(\d+)$/.exec(firmware);
  return match ? match.slice(1).map(Number) : null;
}

/**
 * @param {number[]} version
 * @returns {boolean} whether this firmware reads a version 2 pack (custom ringtone)
 */
export function readsRingtone(version) {
  const [major, minor] = version;
  return major > 1 || (major === 1 && minor >= 9);
}

// ---------------------------------------------------------------------------
// Firmware images

export const APP_OFFSET = 0x10000;
export const FULL_IMAGE_OFFSET = 0x0;
const IMAGE_MAGIC = 0xe9;
const CHIP_ID_ESP32S3 = 9;
const FULL_TABLE_OFFSET = 0x8000;
const FULL_TABLE_SIZE = 0xc00;

/**
 * @typedef {{ kind: "app" | "full", address: number, version: string | null }} FirmwareImage
 *   `version` is the "DeskPet x.y.z" string compiled into the firmware, or null
 *   for a valid ESP32-S3 image that is not DeskPet.
 */

/**
 * Tells a release's app image (flashed at 0x10000) from its full image
 * (bootloader at 0x0, partition table at 0x8000, app at 0x10000, pets at
 * 0x610000), so a file is never written at the wrong address.
 * @param {Uint8Array} bytes
 * @returns {FirmwareImage}
 */
export function inspectFirmware(bytes) {
  /** @param {number} at */
  const isS3Image = (at) => bytes.length >= at + 24 && bytes[at] === IMAGE_MAGIC &&
    (bytes[at + 12] | (bytes[at + 13] << 8)) === CHIP_ID_ESP32S3;
  if (!isS3Image(0)) throw new PackError("This is not an ESP32-S3 firmware image.");
  const full = bytes.length > FULL_TABLE_OFFSET + 32 &&
    bytes[FULL_TABLE_OFFSET] === 0xaa && bytes[FULL_TABLE_OFFSET + 1] === 0x50;
  if (full) {
    const table = parsePartitionTable(bytes.subarray(FULL_TABLE_OFFSET, FULL_TABLE_OFFSET + FULL_TABLE_SIZE));
    const slot = table.find((entry) => entry.type === 0 && entry.offset === APP_OFFSET);
    if (!slot || !isS3Image(APP_OFFSET)) throw new PackError("This full image has no firmware at 0x10000.");
    checkAssetPartition(table, 0);
  }
  // The version is the null-terminated VERSION string from config.h.
  const app = full ? bytes.subarray(APP_OFFSET, PARTITION_OFFSET) : bytes;
  const match = /DeskPet \d+\.\d+\.\d+(?=\0)/.exec(new TextDecoder("latin1").decode(app));
  return { kind: full ? "full" : "app", address: full ? FULL_IMAGE_OFFSET : APP_OFFSET,
           version: match ? match[0] : null };
}

/**
 * Confirms an app image fits the board's first app slot, which is where the
 * bootloader starts DeskPet. A board without DeskPet's layout needs the full image.
 * @param {PartitionEntry[]} entries
 * @param {number} appBytes
 */
export function checkAppPartition(entries, appBytes) {
  const app = entries.find((entry) => entry.type === 0 && entry.offset === APP_OFFSET);
  if (!app) throw new PackError("The board has no firmware slot at 0x10000. Flash the full DeskPet image first.");
  checkAssetPartition(entries, 0);
  if (appBytes > app.size) throw new PackError(`The firmware (${appBytes} bytes) is larger than its slot`);
}
