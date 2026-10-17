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
└── firmware/    A prebuilt full-flash image (merged-binary.bin) in a zip
```

## Quick flash (prebuilt firmware)

No toolchain needed. Download the files from the
[latest release](https://github.com/Tunakite03/Esp32-DeskPet/releases/latest):

| File | Flash at | Use it for |
|---|---|---|
| `deskpet-vX.Y.Z-full.bin` | `0x0` | First install or a clean reset. It contains the bootloader, firmware and pets, and **resets saved settings** (Wi-Fi, pet name, alarm, brightness). |
| `deskpet-vX.Y.Z-app.bin` | `0x10000` | Updating a board that already runs DeskPet. Settings and pets are kept. |

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
.venv/bin/esptool --chip esp32s3 --port /dev/cu.usbmodem2101 write-flash 0x0 deskpet-v1.8.0-full.bin
```

Update:

```bash
.venv/bin/esptool --chip esp32s3 --port /dev/cu.usbmodem2101 write-flash 0x10000 deskpet-v1.8.0-app.bin
```

The board restarts by itself when flashing is done. If esptool cannot connect,
unplug the board, hold BOOT while plugging the USB cable back in, release BOOT
and run the command again.
After a first install, set up Wi-Fi as described in
[Connect to Wi-Fi](#5-connect-to-wi-fi). The installed version is shown on the
Settings page, next to the title.

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
password. Join that network, open `http://192.168.4.1`, then choose your Wi-Fi
and weather location.

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
| `ui-stats`, `settings-status`, `alarm-status`, `pet-status`, `asset-status`, `talk-status` | State of each subsystem |
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
| `build_ringtone.py` | Convert an audio file into the alarm ringtone (`src/core/audio/ringtone_data.h`) |
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
