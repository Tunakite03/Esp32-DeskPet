# Esp32-DeskPet

DeskPet is a desk companion for a 2.8-inch ESP32-S3 touch display. An animated
pet lives on the screen and reacts to taps. The device also has a clock with an
alarm, a weather page for Vietnamese cities, and a talk mode that repeats what
it hears in a higher voice.

## Features

| Page | What it does |
|---|---|
| Pet | Animated companion (bunny, cat, dino, frog). Tap to make it react, long-press it to pick another one. |
| Clock | Local time (Vietnam, UTC+7, synced over NTP) and an alarm that plays a ringtone through the speaker. |
| Weather | Temperature, humidity, the sky condition and the chance of rain for this hour and the next five, from [Open-Meteo](https://open-meteo.com). It refreshes every 15 minutes, or when you tap the page. |
| Settings | Brightness, volume, mute, a sound test and the talk mode switch. Settings are saved to flash and kept across reboots. |

- **Wi-Fi setup portal**: the device starts its own access point. Join it from a
  phone, open `http://192.168.4.1`, pick your network from the scanned list,
  then choose a weather location.
- **Talk mode** (switched on in Settings): the microphone is only read while
  talk mode is on and the pet page is showing. Recordings stay in RAM, get overwritten by the next phrase,
  and are never stored or sent anywhere.
- **Customizer**: a web page (`web/`) that adds, removes, renames and reorders
  companions, swaps the alarm ringtone, and flashes the result from the browser.
  See [Customize companions and ringtone](#customize-companions-and-ringtone).
- **Smooth UI**: only the 16×16 tiles that changed are sent to the panel, and
  touch input is handled before drawing. On the board the UI renders at
  20–21 frames per second with a worst-case render of 14 ms.

## Hardware

- ES3C28P board: ESP32-S3 with 16 MB flash and 8 MB PSRAM.
- 2.8-inch 320×240 ILI9341 SPI display with FT6336 capacitive touch.
- ES8311 audio codec, speaker and microphone.
- BOOT button and an RGB status LED.

## Repository layout

```
Esp32-DeskPet/
├── deskpet/     Arduino sketch: firmware sources, asset pack, host tools, QA data
│                (see deskpet/README.md for the full source layout)
├── web/         Customizer page: edits pets.bin and flashes it over Web Serial
└── firmware/    A prebuilt full-flash image (merged-binary.bin) in a zip
```

## Quick flash (prebuilt firmware)

No toolchain needed. Download the files from the
[latest release](https://github.com/Tunakite03/Esp32-DeskPet/releases/latest):

| File | Flash at | Use it for |
|---|---|---|
| `deskpet-vX.Y.Z-full.bin` | `0x0` | First install or a clean reset. It contains the bootloader, firmware and pets, and **resets saved settings** (Wi-Fi, pet name, alarm, brightness). |
| `deskpet-vX.Y.Z-app.bin` | `0x10000` | Updating a board that already runs DeskPet. Settings and pets are kept. |

There are two ways to flash them: in the browser (nothing to install) or with
esptool on the command line. After a first install, set up Wi-Fi as described in
[Connect to Wi-Fi](#5-connect-to-wi-fi). The installed version is shown on the
Settings page, next to the title.

### Option A: in the browser (ESP Tool web flasher)

[ESP Tool](https://espressif.github.io/esptool-js/) is Espressif's flasher
that runs in the browser. It needs **Chrome or Edge on a desktop**, because it
talks to the board over Web Serial (Firefox and Safari do not support it).

1. Plug the board in over USB and open
   [espressif.github.io/esptool-js](https://espressif.github.io/esptool-js/).
   Leave **WebUSB (CH340)** unticked: this board uses the ESP32-S3's built-in
   USB, not a CH340 chip.
2. Under **Program**, keep **Baudrate** at `921600` and click **Connect**. In
   the browser's port list, pick the board (usually named
   *USB JTAG/serial debug unit*), then **Connect**. The page then shows
   *Connected to device: ESP32-S3*.
3. In the **Flash Address** column, replace the default `0x1000` with the
   address for your file, then choose the file in the **File** column:

   | File | Flash Address |
   |---|---|
   | `deskpet-vX.Y.Z-full.bin` (first install) | `0x0` |
   | `deskpet-vX.Y.Z-app.bin` (update) | `0x10000` |

   **Do not keep `0x1000`.** That address is for the original ESP32; on the
   ESP32-S3 it leaves the board unable to boot.
4. Leave **Flash Mode** at `keep` and **Flash Size** at `detect`, then click
   **Program** and wait for the progress bar and the log to finish.
5. Click **Disconnect**, then unplug the board and plug it back in to start the
   new firmware.

Do not click **Erase Flash** unless you want a clean board: it also deletes the
pets and settings, so you must then flash the **full** image at `0x0`.

If **Connect** fails or the port is missing, unplug the board, hold **BOOT**
while plugging it back in, release BOOT and click **Connect** again. If
**Program** stops partway, lower the baudrate to `460800` or `115200` and
program again.

### Option B: esptool on the command line

**1. Install esptool** in a virtual environment (Python 3 required):

```bash
python3 -m venv .venv
```

```bash
.venv/bin/pip install esptool
```

On Windows, use `py -m venv .venv` and `.venv\Scripts\pip install esptool`.

**2. Find the port.** Plug the board in over USB. It shows up as
`/dev/cu.usbmodemXXXX` on macOS (`ls /dev/cu.usbmodem*`), `/dev/ttyACMx` on
Linux, or `COMx` on Windows (Device Manager → Ports).

**3. Flash.** Replace the port and the version in the file name.

First install:

```bash
.venv/bin/esptool --chip esp32s3 --port /dev/cu.usbmodem2101 write-flash 0x0 deskpet-v1.8.2-full.bin
```

Update:

```bash
.venv/bin/esptool --chip esp32s3 --port /dev/cu.usbmodem2101 write-flash 0x10000 deskpet-v1.8.2-app.bin
```

If esptool cannot connect, unplug the board, hold BOOT while plugging the USB
cable back in, release BOOT and run the command again. If flashing stops with
*No more data to read from the serial port*, the USB link dropped: run the same
command again, ideally with a short cable plugged straight into the computer.

## Build and flash

Run every command from the `deskpet/` folder. `arduino-cli.yaml` keeps the
libraries in `deskpet/.arduino/`.

### 1. Install the toolchain

Install [arduino-cli](https://arduino.github.io/arduino-cli/latest/installation/)
(on macOS: `brew install arduino-cli`), then the ESP32 core and the two libraries
pinned in `sketch.yaml`:

```bash
arduino-cli core update-index --config-file arduino-cli.yaml --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
```

```bash
arduino-cli core install esp32:esp32@3.3.12 --config-file arduino-cli.yaml --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
```

```bash
arduino-cli lib install --config-file arduino-cli.yaml "GFX Library for Arduino@1.6.9" "ArduinoJson@7.4.3"
```

### 2. Compile

```bash
arduino-cli compile --config-file arduino-cli.yaml --profile es3c28p --build-path build --output-dir out .
```

### 3. Flash the firmware

Replace the port with the board's port: `/dev/cu.usbmodemXXXX` on macOS,
`COMx` on Windows. `arduino-cli board list` shows it.

```bash
arduino-cli upload --config-file arduino-cli.yaml --profile es3c28p -p /dev/cu.usbmodem2101 --input-dir out .
```

This writes the bootloader, partition table and app. It leaves the `assets`
partition alone, so the pets survive firmware updates.

### 4. Flash the asset pack (first time, or after changing the pets)

The pet animations live in `assets/pets.bin`. Flash it to the `assets`
partition at `0x610000` (see `partitions.csv`) with esptool:

```bash
python3 -m venv .venv
```

```bash
.venv/bin/pip install esptool pyserial
```

```bash
.venv/bin/esptool --chip esp32s3 --port /dev/cu.usbmodem2101 write-flash 0x610000 assets/pets.bin
```

If the pack is missing or damaged, the firmware still boots and logs a
`pet_assets` error on the serial port.

### 5. Connect to Wi-Fi

On first boot the device offers the setup portal by itself if it has never
connected to Wi-Fi. You can also open it at any time by holding BOOT for about
2 seconds. The screen shows the access point name (`DeskPet-XXXX`) and its
password, `AAAA8888` by default (change it with the serial command
`ap-password <value>`). Join that network, open `http://192.168.4.1`, then
choose your Wi-Fi and weather location.

## Customize companions and ringtone

`web/index.html` edits the asset pack and writes it to the `assets` partition
(`0x610000`) only, so Wi-Fi, the alarm and other settings stay as they are.
It needs Chrome or Edge on a computer (Web Serial). Serve the repository root
over HTTP so the page can load the default pack, then open the page:

```bash
python3 web/serve.py
```

Open <http://localhost:8000/web/>. Use `web/serve.py` rather than
`python -m http.server`: on Windows the latter often sends `.js` as
`text/plain`, and the browser then refuses to run the page. With GitHub Pages enabled for the
repository, the same page works at `https://<user>.github.io/Esp32-DeskPet/web/`.

- **Companions**: rename, reorder or remove them (3 to 16 in a pack). The
  easiest way to add one is the [page-mascot gallery](https://koboyo.com/page-mascot):
  press *Use* on a character, download its `<name>-directions.webp` and
  `<name>-reactions.webp`, and choose both files in the customizer (the name is
  filled in from the file name). Your own two 3×3 sprite sheets on a
  transparent background work the same way (9 gaze directions, then the 9
  reactions, in the order the page lists; see `deskpet/assets/page-mascot/cat-*.webp`).
  A GIF, animated WebP or APNG also works and loops at 64×64 pixels.
- **Ringtone**: pick an audio file and the part to keep (up to 30 seconds). It
  is converted to 16 kHz mono. Without one, the ringtone compiled into the
  firmware plays.
- **Flash to DeskPet**: asks the running firmware for its version, checks the
  partition table, writes the pack, restarts the board and reads
  `asset-status` back to confirm the companions (and ringtone) loaded.
  **Download pets.bin** saves the file to flash yourself at `0x610000`.

A pack with a custom ringtone is format version 2, which firmware 1.9.0 and
newer reads. Older firmware rejects it and shows no companions, so the page
refuses to flash it to a board that reports an older version. A pack without a
ringtone stays version 1 and works with every release.

Tests for the pack encoder (they rebuild `deskpet/assets/pets.bin` byte for byte):

```bash
node --test web/
```

## Using the device

| Input | Action |
|---|---|
| Tap the tab bar | Switch page |
| Short press BOOT | Next page, or close the setup portal |
| Hold BOOT for about 2 seconds | Open or close the setup portal |
| Tap Stop or press BOOT while the alarm rings | Stop the alarm |
| Tap the weather page | Refresh the weather |
| Long-press the pet | Open the companion picker |

The screen dims after 60 seconds without input.

## Serial console

The board answers one-line commands at 115200 baud. Replies are JSON lines,
which the QA scripts parse.

| Command | Reply |
|---|---|
| `status` | Firmware version, uptime, heap, Wi-Fi, clock and weather state |
| `perf` | Worst-case loop, render and save timings since the last `perf` |
| `ui-stats`, `settings-status`, `alarm-status`, `pet-status`, `asset-status`, `talk-status` | State of each subsystem (`asset-status` includes `ringtone_samples`, 0 for the built-in ringtone) |
| `selftest`, `asset-selftest`, `sound-test`, `mic-test` | On-device checks |
| `page <n>`, `tap <x> <y>`, `hold <x> <y>`, `pet`, `setup` | Drive the UI from a script |
| `snapshot` | Dump the framebuffer (used by `tools/verify_pet_assets.py`) |
| `reboot` | Restart the board |

## Host tools

The scripts in `deskpet/tools/` need Python 3 with `pyserial`, `Pillow` and
`av` (PyAV, for the ringtone converter):

| Script | Purpose |
|---|---|
| `build_pet_assets.py` | Rebuild `assets/pets.bin` from the sprite sheets in `assets/page-mascot/` |
| `build_ringtone.py` | Convert an audio file into the built-in alarm ringtone (`src/core/audio/ringtone_data.h`); the customizer can override it without a rebuild |
| `generate_ui_assets.py` | Regenerate the anti-aliased fonts in `ui_assets.h` (reads Segoe UI from `C:/Windows/Fonts`, so Windows only) |
| `verify_pet_assets.py` | Check the asset pack and UI on a connected board, writing PNG snapshots to `qa/snapshots/` |
| `perf_probe.py`, `jank_monitor.py` | Measure UI responsiveness and catch stalls |
| `boot_logger.py`, `talk_monitor.py` | Log resets and panics, watch talk mode live |

Every serial script takes `--port`, for example
`.venv/bin/python tools/perf_probe.py --port /dev/cu.usbmodem2101`.

## Credits

- Pet sprites: page-mascot by Kamran Ahmed, MIT License
  (`deskpet/assets/page-mascot/LICENSE`).
- Weather data: [Open-Meteo](https://open-meteo.com). No account or API key is
  needed.
