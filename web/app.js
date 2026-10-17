// DeskPet Customizer: edits the companions and ringtone of an asset pack and
// flashes it to the board's "assets" partition over Web Serial.
import {
  ANIMATION_SIDE, buildPack, checkAssetPartition, decodeFrame, encodeFrame, firmwareVersion, KIND_ANIMATION,
  KIND_POSES, MAX_ANIMATION_FRAMES, MAX_ANIMATION_MS, MAX_PETS, MAX_RINGTONE_SAMPLES, MIN_PETS, nameProblem,
  PackError, parsePack, parsePartitionTable, PARTITION_OFFSET, PARTITION_SIZE, POSE_FRAME_FRONT, POSE_LABELS,
  POSE_SIDE, readsRingtone, rgb565ToRgba, rgbaToRgb565, SAMPLE_RATE, toRingtonePcm,
} from "./pack.js";

/** @typedef {import("./pack.js").Pack} Pack */
/** @typedef {import("./pack.js").PackPet} PackPet */

const DEFAULT_PACK_URL = "../deskpet/assets/pets.bin";
const ESPTOOL_URL = "https://unpkg.com/esptool-js@0.7.0/bundle.js";
const ESPRESSIF_VID = 0x303a;
const FLASH_BAUD = 921600;
const CONSOLE_BAUD = 115200;
const PARTITION_TABLE_OFFSET = 0x8000;
const PARTITION_TABLE_SIZE = 0xc00;
const ALPHA_THRESHOLD = 24;  // same as build_pet_assets.py
const MAX_SECONDS = MAX_RINGTONE_SAMPLES / SAMPLE_RATE;

/** @type {Pack} */
let pack = { pets: [], ringtone: null, trailer: new Uint8Array(0) };
/** @type {AudioBuffer | null} */
let sourceAudio = null;
/** @type {Uint8Array | null} */
let builtPack = null;
let flashing = false;

/** @param {string} id @returns {any} */
const $ = (id) => {
  const element = document.getElementById(id);
  if (!element) throw new Error(`Missing element #${id}`);
  return element;
};

/**
 * @param {string} id
 * @param {string} message
 * @param {"" | "ok" | "error" | "warn"} tone
 */
function setStatus(id, message, tone) {
  const element = $(id);
  element.textContent = message;
  element.className = `status ${tone}`;
}

/** @param {unknown} error @returns {string} */
function describe(error) {
  return error instanceof Error ? error.message : String(error);
}

/** @param {number} bytes */
function formatBytes(bytes) {
  return bytes >= 1048576 ? `${(bytes / 1048576).toFixed(2)} MB` : `${(bytes / 1024).toFixed(0)} KB`;
}

// ---------------------------------------------------------------------------
// Pack loading

/** @param {Uint8Array} bytes @param {string} source */
function usePack(bytes, source) {
  pack = parsePack(bytes);
  $("pack-source").textContent = `${source}: ${pack.pets.length} companions, ${formatBytes(bytes.length)}` +
    (pack.ringtone ? ", custom ringtone" : "");
  renderPets();
  renderRingtone();
  refreshBuild();
}

async function loadDefaultPack() {
  try {
    const response = await fetch(DEFAULT_PACK_URL, { cache: "no-cache" });
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    usePack(new Uint8Array(await response.arrayBuffer()), "Default pack");
  } catch (error) {
    $("pack-source").textContent = `The default pack could not be loaded (${describe(error)}). ` +
      "Open deskpet/assets/pets.bin with the button above.";
  }
}

// ---------------------------------------------------------------------------
// Companion list

/** @type {Map<HTMLCanvasElement, number>} */
const previewTimers = new Map();

/**
 * @param {HTMLCanvasElement} canvas
 * @param {PackPet} pet
 * @param {number} frame
 */
function drawFrame(canvas, pet, frame) {
  const pixels = decodeFrame(pet.frames[frame].data, pet.side);
  canvas.width = pet.side;
  canvas.height = pet.side;
  const context = /** @type {CanvasRenderingContext2D} */ (canvas.getContext("2d"));
  context.putImageData(new ImageData(rgb565ToRgba(pixels), pet.side, pet.side), 0, 0);
}

/** @param {HTMLCanvasElement} canvas @param {PackPet} pet */
function startPreview(canvas, pet) {
  let frame = 0;
  let startedAt = performance.now();
  const step = () => {
    if (pet.kind === KIND_ANIMATION) {
      const phase = (performance.now() - startedAt) % pet.duration;
      frame = pet.frames.findIndex((f) => phase < f.end);
    } else {
      frame = (frame + 1) % pet.frames.length;
    }
    drawFrame(canvas, pet, Math.max(0, frame));
  };
  startedAt = performance.now();
  previewTimers.set(canvas, window.setInterval(step, pet.kind === KIND_ANIMATION ? 40 : 220));
}

/** @param {HTMLCanvasElement} canvas @param {PackPet} pet */
function stopPreview(canvas, pet) {
  window.clearInterval(previewTimers.get(canvas));
  previewTimers.delete(canvas);
  drawFrame(canvas, pet, pet.kind === KIND_POSES ? POSE_FRAME_FRONT : 0);
}

/** @param {number} from @param {number} to */
function movePet(from, to) {
  const pets = [...pack.pets];
  const [moved] = pets.splice(from, 1);
  pets.splice(to, 0, moved);
  pack = { ...pack, pets };
  renderPets();
  refreshBuild();
}

function renderPets() {
  for (const timer of previewTimers.values()) window.clearInterval(timer);
  previewTimers.clear();
  const list = $("pets");
  list.replaceChildren();
  pack.pets.forEach((pet, index) => {
    const card = document.createElement("div");
    card.className = "pet";
    card.dataset.test = "pet-card";

    const canvas = document.createElement("canvas");
    drawFrame(canvas, pet, pet.kind === KIND_POSES ? POSE_FRAME_FRONT : 0);
    canvas.addEventListener("mouseenter", () => startPreview(canvas, pet));
    canvas.addEventListener("mouseleave", () => stopPreview(canvas, pet));

    const name = document.createElement("input");
    name.type = "text";
    name.maxLength = 12;
    name.value = pet.name;
    name.setAttribute("aria-label", `Name of companion ${index + 1}`);
    name.addEventListener("input", () => {
      const pets = [...pack.pets];
      pets[index] = { ...pet, name: name.value };
      pack = { ...pack, pets };
      name.setCustomValidity(nameProblem(name.value) ?? "");
      refreshBuild();
    });

    const info = document.createElement("span");
    info.className = "muted";
    info.textContent = pet.kind === KIND_POSES
      ? "Posed · follows touch"
      : `Animation · ${pet.frames.length} frames · ${(pet.duration / 1000).toFixed(1)} s`;

    const actions = document.createElement("div");
    actions.className = "actions";
    /** @param {string} label @param {string} title @param {boolean} enabled @param {() => void} run */
    const action = (label, title, enabled, run) => {
      const button = document.createElement("button");
      button.className = "icon";
      button.textContent = label;
      button.title = title;
      button.setAttribute("aria-label", title);
      button.disabled = !enabled;
      button.addEventListener("click", run);
      actions.append(button);
    };
    action("←", "Move earlier", index > 0, () => movePet(index, index - 1));
    action("→", "Move later", index < pack.pets.length - 1, () => movePet(index, index + 1));
    action("✕", "Remove", pack.pets.length > MIN_PETS, () => {
      pack = { ...pack, pets: pack.pets.filter((_, i) => i !== index) };
      renderPets();
      refreshBuild();
    });

    card.append(canvas, name, info, actions);
    list.append(card);
  });
  $("pet-count").textContent = `${pack.pets.length} of ${MAX_PETS} (at least ${MIN_PETS})`;
  $("add-posed").disabled = pack.pets.length >= MAX_PETS;
  $("add-animated").disabled = pack.pets.length >= MAX_PETS;
}

// ---------------------------------------------------------------------------
// New companions

/**
 * @param {CanvasImageSource} source
 * @param {number} width
 * @param {number} height
 * @returns {ImageData}
 */
function pixelsOf(source, width, height) {
  const canvas = new OffscreenCanvas(width, height);
  const context = /** @type {OffscreenCanvasRenderingContext2D} */ (canvas.getContext("2d"));
  context.drawImage(source, 0, 0);
  return context.getImageData(0, 0, width, height);
}

/**
 * Crops all 18 cells to one square around the artwork (the union of every
 * cell's opaque pixels, plus a margin) and scales them to 144 px, as
 * build_pet_assets.mascot_frames does.
 * @param {string} name
 * @param {File} directionsFile
 * @param {File} reactionsFile
 * @returns {Promise<PackPet>}
 */
async function makePosedPet(name, directionsFile, reactionsFile) {
  const sheets = await Promise.all([directionsFile, reactionsFile].map((file) => createImageBitmap(file)));
  const [first] = sheets;
  for (const sheet of sheets) {
    if (sheet.width !== sheet.height || sheet.width !== first.width || sheet.width < 3) {
      throw new PackError("Both sheets must be square and the same size");
    }
  }
  const cell = Math.floor(first.width / 3);
  let left = cell, top = cell, right = 0, bottom = 0;
  for (const sheet of sheets) {
    const { data } = pixelsOf(sheet, sheet.width, sheet.height);
    for (let index = 0; index < 9; ++index) {
      const cellX = (index % 3) * cell, cellY = Math.floor(index / 3) * cell;
      for (let y = 0; y < cell; ++y) {
        const row = ((cellY + y) * sheet.width + cellX) * 4;
        for (let x = 0; x < cell; ++x) {
          if (data[row + x * 4 + 3] > ALPHA_THRESHOLD) {
            left = Math.min(left, x); right = Math.max(right, x + 1);
            top = Math.min(top, y); bottom = Math.max(bottom, y + 1);
          }
        }
      }
    }
  }
  if (right === 0) throw new PackError("The sprite sheets have no visible pixels");
  const margin = Math.round((8 * cell) / 360);
  const side = Math.min(cell, Math.max(right - left, bottom - top) + margin);
  const cropX = Math.max(0, Math.min(cell - side, Math.floor((left + right - side) / 2)));
  const cropY = Math.max(0, Math.min(cell - side, Math.floor((top + bottom - side) / 2)));

  const canvas = new OffscreenCanvas(POSE_SIDE, POSE_SIDE);
  const context = /** @type {OffscreenCanvasRenderingContext2D} */ (canvas.getContext("2d"));
  context.imageSmoothingQuality = "high";
  const frames = [];
  for (const sheet of sheets) {
    for (let index = 0; index < 9; ++index) {
      const cellX = (index % 3) * cell, cellY = Math.floor(index / 3) * cell;
      context.clearRect(0, 0, POSE_SIDE, POSE_SIDE);
      context.drawImage(sheet, cellX + cropX, cellY + cropY, side, side, 0, 0, POSE_SIDE, POSE_SIDE);
      const pixels = rgbaToRgb565(context.getImageData(0, 0, POSE_SIDE, POSE_SIDE).data);
      frames.push({ data: encodeFrame(pixels), end: 0 });
    }
  }
  sheets.forEach((sheet) => sheet.close());
  return { name, kind: KIND_POSES, side: POSE_SIDE, duration: 0, frames };
}

/** @param {File} file @returns {string} */
function imageType(file) {
  if (file.type) return file.type;
  const extension = file.name.split(".").pop()?.toLowerCase();
  const types = { gif: "image/gif", webp: "image/webp", png: "image/png", apng: "image/apng", jpg: "image/jpeg",
                  jpeg: "image/jpeg" };
  const type = types[/** @type {keyof typeof types} */ (extension)];
  if (!type) throw new PackError(`Unsupported image file: ${file.name}`);
  return type;
}

/**
 * Decodes every frame (already composited by the browser), fits it into
 * 64 px on black and keeps each frame's delay.
 * @param {string} name
 * @param {File} file
 * @returns {Promise<PackPet>}
 */
async function makeAnimatedPet(name, file) {
  if (!("ImageDecoder" in window)) throw new PackError("This browser cannot decode animations; use Chrome or Edge");
  // @ts-ignore ImageDecoder is not in every DOM typing yet.
  const decoder = new ImageDecoder({ data: await file.arrayBuffer(), type: imageType(file) });
  try {
    await decoder.tracks.ready;
    await decoder.completed;
    const track = decoder.tracks.selectedTrack;
    if (!track) throw new PackError(`${file.name} holds no image`);
    const count = track.frameCount;
    if (count < 1 || count > MAX_ANIMATION_FRAMES) {
      throw new PackError(`The image has ${count} frames; 1 to ${MAX_ANIMATION_FRAMES} are supported`);
    }
    const canvas = new OffscreenCanvas(ANIMATION_SIDE, ANIMATION_SIDE);
    const context = /** @type {OffscreenCanvasRenderingContext2D} */ (canvas.getContext("2d"));
    context.imageSmoothingQuality = "high";
    const frames = [];
    let end = 0;
    for (let index = 0; index < count; ++index) {
      const { image } = await decoder.decode({ frameIndex: index });
      const width = image.displayWidth, height = image.displayHeight;
      const scale = Math.min(ANIMATION_SIDE / width, ANIMATION_SIDE / height);
      const drawWidth = Math.round(width * scale), drawHeight = Math.round(height * scale);
      context.clearRect(0, 0, ANIMATION_SIDE, ANIMATION_SIDE);
      context.drawImage(image, Math.floor((ANIMATION_SIDE - drawWidth) / 2), Math.floor((ANIMATION_SIDE - drawHeight) / 2),
                        drawWidth, drawHeight);
      // Browsers play GIF delays under 20 ms as 100 ms; a still image shows for one second.
      const delay = image.duration ? Math.round(image.duration / 1000) : count === 1 ? 1000 : 100;
      end += delay < 20 ? 100 : delay;
      image.close();
      const pixels = rgbaToRgb565(context.getImageData(0, 0, ANIMATION_SIDE, ANIMATION_SIDE).data);
      frames.push({ data: encodeFrame(pixels), end });
    }
    if (end > MAX_ANIMATION_MS) throw new PackError(`The animation lasts ${end / 1000} s; the limit is 120 s`);
    return { name, kind: KIND_ANIMATION, side: ANIMATION_SIDE, duration: end, frames };
  } finally {
    decoder.close();
  }
}

/** @param {string} nameId @param {() => Promise<PackPet | null>} make */
async function addPet(nameId, make) {
  const name = $(nameId).value.trim();
  const problem = nameProblem(name);
  if (problem) { setStatus("add-status", `Name: ${problem}.`, "error"); return; }
  if (pack.pets.length >= MAX_PETS) { setStatus("add-status", `The pack already holds ${MAX_PETS}.`, "error"); return; }
  setStatus("add-status", "Converting…", "");
  try {
    const pet = await make();
    if (!pet) return;
    pack = { ...pack, pets: [...pack.pets, pet] };
    $(nameId).value = "";
    renderPets();
    refreshBuild();
    setStatus("add-status", `${name} added.`, "ok");
  } catch (error) {
    setStatus("add-status", describe(error), "error");
  }
}

// ---------------------------------------------------------------------------
// Ringtone

function renderRingtone() {
  $("ringtone-state").textContent = pack.ringtone
    ? `Custom ringtone, ${(pack.ringtone.length / SAMPLE_RATE).toFixed(1)} s.`
    : "Built-in ringtone (the one compiled into the firmware).";
  $("ringtone-play").disabled = !pack.ringtone;
  $("ringtone-remove").disabled = !pack.ringtone;
}

/** @param {File} file */
async function openRingtone(file) {
  setStatus("ringtone-status", "Decoding…", "");
  try {
    const context = new OfflineAudioContext(1, 1, SAMPLE_RATE);
    sourceAudio = await context.decodeAudioData(await file.arrayBuffer());
    $("ringtone-start").value = "0";
    $("ringtone-end").value = Math.min(sourceAudio.duration, MAX_SECONDS).toFixed(1);
    for (const id of ["ringtone-start", "ringtone-end", "ringtone-apply"]) $(id).disabled = false;
    $("ringtone-start").max = $("ringtone-end").max = sourceAudio.duration.toFixed(1);
    await applyRingtone();
  } catch (error) {
    sourceAudio = null;
    setStatus("ringtone-status", `The audio could not be decoded: ${describe(error)}`, "error");
  }
}

async function applyRingtone() {
  if (!sourceAudio) return;
  const start = Number($("ringtone-start").value);
  const end = Math.min(Number($("ringtone-end").value), sourceAudio.duration);
  if (!(start >= 0 && end > start)) { setStatus("ringtone-status", "End must be after start.", "error"); return; }
  if (end - start > MAX_SECONDS) {
    setStatus("ringtone-status", `Choose at most ${MAX_SECONDS} seconds.`, "error");
    return;
  }
  const length = Math.round((end - start) * SAMPLE_RATE);
  const context = new OfflineAudioContext(1, length, SAMPLE_RATE);
  const source = context.createBufferSource();
  source.buffer = sourceAudio;
  source.connect(context.destination);
  source.start(0, start, end - start);
  const rendered = await context.startRendering();
  try {
    pack = { ...pack, ringtone: toRingtonePcm(rendered.getChannelData(0)) };
  } catch (error) {
    setStatus("ringtone-status", describe(error), "error");
    return;
  }
  renderRingtone();
  refreshBuild();
  setStatus("ringtone-status", `Using ${start.toFixed(1)}–${end.toFixed(1)} s.`, "ok");
}

/** @type {AudioBufferSourceNode | null} */
let playing = null;

function playRingtone() {
  if (playing) { playing.stop(); return; }
  if (!pack.ringtone) return;
  const context = new AudioContext();
  const buffer = context.createBuffer(1, pack.ringtone.length, SAMPLE_RATE);
  buffer.getChannelData(0).set(Float32Array.from(pack.ringtone, (sample) => sample / 32768));
  const source = context.createBufferSource();
  source.buffer = buffer;
  source.connect(context.destination);
  source.addEventListener("ended", () => { playing = null; $("ringtone-play").textContent = "Play"; context.close(); });
  source.start();
  playing = source;
  $("ringtone-play").textContent = "Stop";
}

// ---------------------------------------------------------------------------
// Build, download and flash

function refreshBuild() {
  try {
    builtPack = buildPack(pack);
    const share = (builtPack.length / PARTITION_SIZE) * 100;
    $("size-text").textContent = `${formatBytes(builtPack.length)} of ${formatBytes(PARTITION_SIZE)} ` +
      `(${share.toFixed(1)}%)`;
    $("size-bar").style.width = `${share}%`;
    $("version-text").textContent = pack.ringtone ? "Pack v2 · needs firmware 1.9.0+" : "Pack v1 · any firmware";
    setStatus("build-status", "", "");
  } catch (error) {
    builtPack = null;
    setStatus("build-status", describe(error), "error");
  }
  $("download").disabled = !builtPack || flashing;
  $("flash").disabled = !builtPack || flashing || !("serial" in navigator);
}

function download() {
  if (!builtPack) return;
  const link = document.createElement("a");
  link.href = URL.createObjectURL(new Blob([builtPack], { type: "application/octet-stream" }));
  link.download = "pets.bin";
  link.click();
  URL.revokeObjectURL(link.href);
}

/** @param {string} line */
function log(line) {
  const element = $("flash-log");
  element.hidden = false;
  element.textContent += `${line}\n`;
  element.scrollTop = element.scrollHeight;
}

/** @param {number} ms */
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

/**
 * Sends a console command to the running firmware (see serial_console.cpp)
 * until a JSON line containing `key` comes back.
 * @param {SerialPort} port
 * @param {string} command
 * @param {string} key
 * @param {number} timeoutMs
 * @returns {Promise<Record<string, unknown> | null>} null when nothing answered in time
 */
async function askFirmware(port, command, key, timeoutMs) {
  await port.open({ baudRate: CONSOLE_BAUD });
  const reader = /** @type {ReadableStreamDefaultReader<Uint8Array>} */ (port.readable?.getReader());
  const writer = /** @type {WritableStreamDefaultWriter<Uint8Array>} */ (port.writable?.getWriter());
  const send = () => writer.write(new TextEncoder().encode(`${command}\n`));
  const resend = window.setInterval(send, 1000);
  const timeout = window.setTimeout(() => reader.cancel(), timeoutMs);
  try {
    await send();
    const text = new TextDecoder();
    let buffer = "";
    for (;;) {
      const { value, done } = await reader.read();
      if (done) return null;
      buffer += text.decode(value, { stream: true });
      const lines = buffer.split("\n");
      buffer = lines.pop() ?? "";
      for (const line of lines.map((l) => l.trim()).filter((l) => l.startsWith("{"))) {
        let parsed;
        try {
          parsed = JSON.parse(line);
        } catch (error) {
          if (error instanceof SyntaxError) continue;  // a log line cut by the boot or a reset
          throw error;
        }
        if (key in parsed) return parsed;
      }
    }
  } finally {
    window.clearInterval(resend);
    window.clearTimeout(timeout);
    reader.releaseLock();
    writer.releaseLock();
    await port.close();
  }
}

/** After a reset the board re-enumerates, so its port object may be a new one. */
async function findBoardPort() {
  for (let attempt = 0; attempt < 20; ++attempt) {
    const ports = await navigator.serial.getPorts();
    const board = ports.find((port) => port.getInfo().usbVendorId === ESPRESSIF_VID);
    if (board) return board;
    await sleep(500);
  }
  return null;
}

async function flash() {
  if (!builtPack) return;
  const image = builtPack;
  const hasRingtone = pack.ringtone !== null;
  flashing = true;
  refreshBuild();
  $("flash-log").textContent = "";
  $("flash-bar").style.width = "0%";
  /** @type {any} */
  let transport = null;
  try {
    const port = await navigator.serial.requestPort({ filters: [{ usbVendorId: ESPRESSIF_VID }] });

    setStatus("flash-status", "Asking the board for its firmware version…", "");
    const status = await askFirmware(port, "status", "firmware", 6000);
    const version = typeof status?.firmware === "string" ? firmwareVersion(status.firmware) : null;
    log(version ? `Board runs ${status?.firmware}` : "The board did not report a firmware version");
    if (hasRingtone && version && !readsRingtone(version)) {
      throw new PackError(`${status?.firmware} cannot read a custom ringtone and would lose its companions. ` +
                          "Update the firmware to 1.9.0 or newer first, or use the built-in ringtone.");
    }
    if (hasRingtone && !version &&
        !window.confirm("The firmware version is unknown. Firmware older than 1.9.0 shows no companions with a " +
                        "custom ringtone. Flash anyway?")) {
      throw new PackError("Flashing cancelled");
    }

    setStatus("flash-status", "Connecting to the bootloader…", "");
    const { ESPLoader, Transport } = await import(ESPTOOL_URL);
    transport = new Transport(port, false);
    const terminal = { clean() {}, writeLine: log, write: log };
    const loader = new ESPLoader({ transport, baudrate: FLASH_BAUD, terminal });
    const chip = await loader.main();
    if (!/ESP32-S3/.test(chip)) throw new PackError(`Expected an ESP32-S3, found ${chip}`);

    const table = await loader.readFlash(PARTITION_TABLE_OFFSET, PARTITION_TABLE_SIZE);
    checkAssetPartition(parsePartitionTable(table), image.length);
    log(`Assets partition found at 0x${PARTITION_OFFSET.toString(16)}`);

    setStatus("flash-status", "Writing the asset pack…", "");
    await loader.writeFlash({
      fileArray: [{ data: image, address: PARTITION_OFFSET }],
      flashMode: "keep", flashFreq: "keep", flashSize: "keep", eraseAll: false, compress: true,
      reportProgress: (/** @type {number} */ _file, /** @type {number} */ written, /** @type {number} */ total) => {
        $("flash-bar").style.width = `${(written / total) * 100}%`;
      },
    });
    await loader.after("hard_reset");
    await transport.disconnect();
    transport = null;

    setStatus("flash-status", "Restarting the board and checking the pack…", "");
    await sleep(2000);
    const board = await findBoardPort();
    const assets = board ? await askFirmware(board, "asset-status", "assets_ready", 12000) : null;
    if (!assets) {
      setStatus("flash-status", "Flashed. The board did not answer afterwards; check that the companions show up.",
                "warn");
    } else if (assets.assets_ready !== true || assets.asset_count !== pack.pets.length) {
      setStatus("flash-status", "Flashed, but the board rejected the pack. See the serial log for the reason.",
                "error");
      log(JSON.stringify(assets));
    } else {
      const ringtoneNote = hasRingtone
        ? (assets.ringtone_samples === pack.ringtone?.length ? " with the custom ringtone" : " (ringtone not read)")
        : "";
      setStatus("flash-status", `Done: the board loaded ${assets.asset_count} companions${ringtoneNote}.`, "ok");
    }
  } catch (error) {
    if (error instanceof DOMException && error.name === "NotFoundError") {
      setStatus("flash-status", "No board was chosen.", "warn");
    } else {
      setStatus("flash-status", describe(error), "error");
      log(`Error: ${describe(error)}`);
    }
    if (transport) await transport.disconnect();
  } finally {
    flashing = false;
    refreshBuild();
  }
}

// ---------------------------------------------------------------------------
// Wiring

/**
 * Sorts sheets picked in either box by their page-mascot file names
 * ("bear-directions.webp", "bear-reactions.webp") and, when the name box is
 * empty, fills it from the character's name.
 * @param {HTMLInputElement} input
 */
function sortSheets(input) {
  const files = [...(input.files ?? [])];
  const directions = files.find((file) => /-directions\.[a-z]+$/i.test(file.name));
  const reactions = files.find((file) => /-reactions\.[a-z]+$/i.test(file.name));
  /** @param {string} id @param {File | undefined} file */
  const place = (id, file) => {
    if (!file) return;
    const transfer = new DataTransfer();
    transfer.items.add(file);
    $(id).files = transfer.files;
  };
  if (files.length > 1) {
    if (!directions || !reactions) {
      setStatus("add-status", "With two files at once, name them <name>-directions and <name>-reactions.", "error");
      input.value = "";
      return;
    }
    place("directions-file", directions);
    place("reactions-file", reactions);
  } else if (input.id === "directions-file" && reactions) {
    place("reactions-file", reactions);
    input.value = "";
  } else if (input.id === "reactions-file" && directions) {
    place("directions-file", directions);
    input.value = "";
  }
  const named = directions ?? reactions;
  const nameBox = $("posed-name");
  if (named && !nameBox.value.trim()) {
    const base = named.name.replace(/-(directions|reactions)\.[a-z]+$/i, "").replace(/[-_]+/g, " ").trim();
    const title = base.replace(/\b[a-z]/g, (letter) => letter.toUpperCase()).slice(0, 12).trim();
    if (!nameProblem(title)) nameBox.value = title;
  }
  setStatus("add-status", "", "");
}

function wireTabs() {
  const tabs = /** @type {HTMLButtonElement[]} */ ([...document.querySelectorAll("[role=tab]")]);
  for (const tab of tabs) {
    tab.addEventListener("click", () => {
      for (const other of tabs) {
        const selected = other === tab;
        other.setAttribute("aria-selected", String(selected));
        $(/** @type {string} */ (other.dataset.panel)).hidden = !selected;
      }
    });
  }
}

function wire() {
  $("pose-labels").replaceChildren(...POSE_LABELS.map((label) => {
    const item = document.createElement("li");
    item.textContent = label;
    return item;
  }));
  wireTabs();
  $("open-pack").addEventListener("change", async (/** @type {Event} */ event) => {
    const file = /** @type {HTMLInputElement} */ (event.target).files?.[0];
    if (!file) return;
    try {
      usePack(new Uint8Array(await file.arrayBuffer()), file.name);
    } catch (error) {
      $("pack-source").textContent = `${file.name} is not a valid pack: ${describe(error)}`;
    }
  });
  $("reset-pack").addEventListener("click", loadDefaultPack);
  for (const id of ["directions-file", "reactions-file"]) {
    $(id).addEventListener("change", (/** @type {Event} */ event) => {
      sortSheets(/** @type {HTMLInputElement} */ (event.target));
    });
  }
  $("add-posed").addEventListener("click", () => addPet("posed-name", async () => {
    const directions = $("directions-file").files?.[0];
    const reactions = $("reactions-file").files?.[0];
    if (!directions || !reactions) {
      setStatus("add-status", "Choose both sprite sheets.", "error");
      return null;
    }
    return makePosedPet($("posed-name").value.trim(), directions, reactions);
  }));
  $("add-animated").addEventListener("click", () => addPet("animated-name", async () => {
    const file = $("animated-file").files?.[0];
    if (!file) {
      setStatus("add-status", "Choose an image.", "error");
      return null;
    }
    return makeAnimatedPet($("animated-name").value.trim(), file);
  }));
  $("ringtone-file").addEventListener("change", (/** @type {Event} */ event) => {
    const file = /** @type {HTMLInputElement} */ (event.target).files?.[0];
    if (file) openRingtone(file);
  });
  $("ringtone-apply").addEventListener("click", applyRingtone);
  $("ringtone-play").addEventListener("click", playRingtone);
  $("ringtone-remove").addEventListener("click", () => {
    pack = { ...pack, ringtone: null };
    renderRingtone();
    refreshBuild();
    setStatus("ringtone-status", "The built-in ringtone will be used.", "ok");
  });
  $("download").addEventListener("click", download);
  $("flash").addEventListener("click", flash);
  if (!("serial" in navigator)) {
    setStatus("flash-status", "This browser cannot reach USB devices. Use Chrome or Edge, or download the file.",
              "warn");
  }
  renderRingtone();
}

wire();
loadDefaultPack();
