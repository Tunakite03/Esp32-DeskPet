// Run with: node --test web/
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { test } from "node:test";
import {
  APP_OFFSET, buildPack, checkAppPartition, checkAssetPartition, crc32, decodeFrame, encodeFrame, firmwareVersion,
  inspectFirmware, KIND_ANIMATION, PackError, parsePack, parsePartitionTable, PARTITION_OFFSET, readsRingtone,
  toRingtonePcm,
} from "./pack.js";

const SHIPPED = new Uint8Array(readFileSync(new URL("../deskpet/assets/pets.bin", import.meta.url)));
const MANIFEST = JSON.parse(readFileSync(new URL("../deskpet/assets/manifest.json", import.meta.url), "utf8"));

test("crc32 matches zlib", () => {
  assert.equal(crc32(new TextEncoder().encode("123456789")), 0xcbf43926);
  assert.equal(crc32(SHIPPED.subarray(24)), MANIFEST.crc32);
});

test("the shipped pack parses and decodes to the manifest's pixel CRC", () => {
  const pack = parsePack(SHIPPED);
  assert.deepEqual(pack.pets.map((pet) => pet.name), MANIFEST.assets.map((asset) => asset.name));
  assert.equal(pack.ringtone, null);
  // Same running CRC as decoded_crc32 in build_pet_assets.py.
  let crc = 0xffffffff;
  const table = new Uint32Array(256).map((_, n) => {
    let c = n;
    for (let k = 0; k < 8; ++k) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    return c >>> 0;
  });
  for (const pet of pack.pets) {
    for (const frame of pet.frames) {
      const pixels = decodeFrame(frame.data, pet.side);
      const bytes = new Uint8Array(pixels.buffer);
      for (const byte of bytes) crc = table[(crc ^ byte) & 0xff] ^ (crc >>> 8);
    }
  }
  assert.equal((crc ^ 0xffffffff) >>> 0, MANIFEST.decoded_crc32);
});

test("re-encoding every frame reproduces build_pet_assets.py byte for byte", () => {
  for (const pet of parsePack(SHIPPED).pets) {
    for (const frame of pet.frames) {
      assert.deepEqual(encodeFrame(decodeFrame(frame.data, pet.side)), frame.data);
    }
  }
});

test("rebuilding the shipped pack is byte-identical (version 1)", () => {
  assert.deepEqual(buildPack(parsePack(SHIPPED)), SHIPPED);
});

test("a ringtone makes a version 2 pack that round-trips", () => {
  const pack = parsePack(SHIPPED);
  const ringtone = toRingtonePcm(Float32Array.from({ length: 16001 }, (_, i) => Math.sin(i / 10)));
  const built = buildPack({ ...pack, ringtone });
  const view = new DataView(built.buffer);
  assert.equal(view.getUint32(8, true), 2);
  assert.equal(view.getUint32(24, true) % 4, 0);
  const reread = parsePack(built);
  assert.deepEqual(reread.ringtone, ringtone);
  assert.deepEqual(reread.trailer, pack.trailer);
  assert.deepEqual(reread.pets.map((pet) => pet.name), pack.pets.map((pet) => pet.name));
});

test("an animated companion round-trips", () => {
  const pack = parsePack(SHIPPED);
  const frames = [0x001f, 0xf800, 0x07e0].map((color, i) => ({
    data: encodeFrame(new Uint16Array(64 * 64).fill(color)),
    end: (i + 1) * 120,
  }));
  const pet = { name: "Blob", kind: KIND_ANIMATION, side: 64, duration: 360, frames };
  const reread = parsePack(buildPack({ ...pack, pets: [...pack.pets, pet] }));
  assert.equal(reread.pets.length, 5);
  assert.deepEqual(reread.pets[4], pet);
});

test("packs the board would reject are refused", () => {
  const pack = parsePack(SHIPPED);
  assert.throws(() => buildPack({ ...pack, pets: pack.pets.slice(0, 2) }), PackError);
  assert.throws(() => buildPack({ ...pack, pets: [{ ...pack.pets[0], name: "Mèo" }, ...pack.pets.slice(1)] }),
                PackError);
  assert.throws(() => buildPack({ ...pack, pets: [{ ...pack.pets[0], name: "A very long name" }, ...pack.pets] }),
                PackError);
  const corrupt = SHIPPED.slice();
  corrupt[5000] ^= 0xff;
  assert.throws(() => parsePack(corrupt), /CRC/);
  assert.throws(() => decodeFrame(new Uint8Array([0, 0x05, 0x80, 0, 0]), 2), PackError);
});

test("the partition table check accepts DeskPet's layout only", () => {
  const entry = (/** @type {string} */ label, type, subtype, offset, size) => {
    const bytes = new Uint8Array(32);
    const view = new DataView(bytes.buffer);
    bytes.set([0xaa, 0x50, type, subtype]);
    view.setUint32(4, offset, true);
    view.setUint32(8, size, true);
    bytes.set(new TextEncoder().encode(label), 12);
    return bytes;
  };
  const table = new Uint8Array(32 * 3).fill(0xff);
  table.set(entry("app0", 0, 0x10, 0x10000, 0x300000), 0);
  table.set(entry("assets", 1, 0x40, PARTITION_OFFSET, 0x9e0000), 32);
  const entries = parsePartitionTable(table);
  assert.equal(entries.length, 2);
  checkAssetPartition(entries, SHIPPED.length);
  assert.throws(() => checkAssetPartition(entries.slice(0, 1), 10), PackError);
  assert.throws(() => checkAssetPartition([{ ...entries[1], offset: 0x400000 }], 10), PackError);
});

test("firmware versions gate the ringtone", () => {
  assert.deepEqual(firmwareVersion("DeskPet 1.9.0"), [1, 9, 0]);
  assert.equal(firmwareVersion("Other 1.0.0"), null);
  assert.equal(readsRingtone([1, 8, 2]), false);
  assert.equal(readsRingtone([1, 9, 0]), true);
  assert.equal(readsRingtone([2, 0, 0]), true);
});

test("firmware images are told apart and checked", () => {
  /** An ESP32-S3 image header with a version string further in. */
  const image = (bytes, at, version) => {
    bytes[at] = 0xe9;
    bytes[at + 12] = 9;
    bytes[at + 13] = 0;
    if (version) bytes.set(new TextEncoder().encode(`${version}\0`), at + 0x100);
    return bytes;
  };
  const entry = (label, type, subtype, offset, size) => {
    const bytes = new Uint8Array(32);
    const view = new DataView(bytes.buffer);
    bytes[0] = 0xaa; bytes[1] = 0x50; bytes[2] = type; bytes[3] = subtype;
    view.setUint32(4, offset, true);
    view.setUint32(8, size, true);
    bytes.set(new TextEncoder().encode(label), 12);
    return bytes;
  };
  const app = image(new Uint8Array(0x20000), 0, "DeskPet 1.9.0");
  assert.deepEqual(inspectFirmware(app), { kind: "app", address: APP_OFFSET, version: "DeskPet 1.9.0" });
  assert.equal(inspectFirmware(image(new Uint8Array(0x1000), 0, null)).version, null);

  const full = image(new Uint8Array(PARTITION_OFFSET + 16).fill(0xff), 0, null);
  full.set(entry("app0", 0, 0x10, APP_OFFSET, 0x300000), 0x8000);
  full.set(entry("assets", 1, 0x40, PARTITION_OFFSET, 0x9e0000), 0x8020);
  image(full, APP_OFFSET, "DeskPet 2.0.1");
  assert.deepEqual(inspectFirmware(full), { kind: "full", address: 0, version: "DeskPet 2.0.1" });

  assert.throws(() => inspectFirmware(new Uint8Array(0x1000)), PackError);
  const esp32 = image(new Uint8Array(0x1000), 0, null);
  esp32[12] = 0;
  assert.throws(() => inspectFirmware(esp32), PackError);
  const noApp = full.slice();
  noApp[APP_OFFSET] = 0xff;
  assert.throws(() => inspectFirmware(noApp), PackError);

  const table = parsePartitionTable(full.subarray(0x8000, 0x8000 + 0xc00));
  checkAppPartition(table, 0x200000);
  assert.throws(() => checkAppPartition(table, 0x300001), PackError);
  assert.throws(() => checkAppPartition(table.slice(1), 10), PackError);
});

test("ringtone PCM fades both ends", () => {
  const pcm = toRingtonePcm(new Float32Array(1000).fill(1));
  assert.equal(pcm[0], 0);
  assert.equal(pcm[pcm.length - 1], 0);
  assert.equal(pcm[500], 32767);
  assert.throws(() => toRingtonePcm(new Float32Array(0)), PackError);
});
