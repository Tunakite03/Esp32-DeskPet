"""Verify the asset pack and companion UI on a connected DeskPet board."""
from __future__ import annotations

import argparse
import json
import re
import struct
import time
from pathlib import Path

import serial
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]  # sketch root; this script lives in tools/
SNAPSHOTS = ROOT / "qa" / "snapshots"
Scalar = str | int | float | bool | None
VERSION = re.search(r'VERSION\[\] = "([^"]+)"', (ROOT / "config.h").read_text()).group(1)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    args = parser.parse_args()
    SNAPSHOTS.mkdir(parents=True, exist_ok=True)
    manifest = json.loads((ROOT / "assets/manifest.json").read_text())
    with serial.Serial(args.port, 115200, timeout=0.3, write_timeout=3) as port:
        time.sleep(2)
        port.reset_input_buffer()

        def send(command: str) -> None:
            port.write((command + "\n").encode("ascii"))
            port.flush()

        def query(command: str, key: str, timeout: float = 8) -> dict[str, Scalar]:
            send(command)
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                line = port.readline().decode("utf-8", errors="replace").strip()
                if not line:
                    continue
                if line.startswith("{"):
                    record = json.loads(line)
                    if record.get("error") or record.get("level") == "error":
                        raise RuntimeError(f"Device error: {record}")
                    if key in record:
                        print(json.dumps({"command": command, "result": record}), flush=True)
                        return record
            raise TimeoutError(f"No response for {command}")

        def snapshot(name: str) -> None:
            time.sleep(0.65)
            send("snapshot")
            deadline = time.monotonic() + 8
            while port.readline().strip() != b"FRAME 320 240":
                if time.monotonic() > deadline:
                    raise TimeoutError("Snapshot header missing")
            raw = bytearray()
            while len(raw) < 153600 and time.monotonic() < deadline:
                raw.extend(port.read(153600 - len(raw)))
            if len(raw) != 153600 or port.readline().strip() != b"FRAME END":
                raise RuntimeError("Snapshot payload incomplete")
            rgb = bytearray()
            for (pixel,) in struct.iter_unpack("<H", raw):
                rgb.extend((((pixel >> 11) & 31) * 255 // 31,
                            ((pixel >> 5) & 63) * 255 // 63,
                            (pixel & 31) * 255 // 31))
            Image.frombytes("RGB", (320, 240), bytes(rgb)).save(SNAPSHOTS / f"preview-1.5-{name}.png")

        def select(index: int) -> None:
            send("page 0")
            send("hold 234 120")
            state = query("asset-status", "assets_ready")
            assert state["picker_page"] == 0
            send(f"tap {60 + index % 3 * 100} 130")
            time.sleep(0.65)
            state = query("asset-status", "assets_ready")
            assert state["mascot"] == index and state["assets_ready"] is True

        status = query("status", "firmware")
        assert status["firmware"] == VERSION
        initial = query("asset-status", "assets_ready")
        assert initial["assets_ready"] is True and initial["asset_count"] == 3
        assert initial["asset_bytes"] == manifest["bytes"]
        settings = query("settings-status", "brightness")
        alarm = query("alarm-status", "alarm_hour")
        tested = query("asset-selftest", "asset_test", 40)
        assert tested["asset_test"] == "pass"
        assert tested["decoded_crc32"] == manifest["decoded_crc32"]
        assert tested["frames"] == sum(asset["frames"] for asset in manifest["assets"])
        send("selftest")
        deadline = time.monotonic() + 5
        while True:
            line = port.readline().decode("utf-8", errors="replace").strip()
            if line.startswith("SELFTEST"):
                assert line.startswith("SELFTEST PASS"), line
                print(line, flush=True)
                break
            if time.monotonic() > deadline:
                raise TimeoutError("Model self-test response missing")
        for index in range(3):
            sound_before = query("settings-status", "brightness")
            select(index)
            send("tap 234 120")
            time.sleep(0.7)
            sound_after = query("settings-status", "brightness")
            assert sound_after["audio_completed"] == sound_before["audio_completed"]
            time.sleep(0.2)
            snapshot(f"companion-{index:02}")
        select(0)
        time.sleep(1.7)
        for _ in range(4):
            send("tap 234 120")
            time.sleep(0.09)
        reaction = query("pet-status", "picker_open")
        assert reaction["expression"] == 7 and reaction["reacting"] is True
        snapshot("dizzy")
        assert query("settings-status", "brightness")["audio_completed"] == settings["audio_completed"]
        send("page 0")
        send("hold 234 120")
        assert query("pet-status", "picker_open")["picker_open"] is True
        assert query("asset-status", "picker_page")["picker_page"] == 0
        snapshot("picker-0")
        for page, name in ((1, "clock"), (2, "weather"), (3, "settings")):
            send(f"page {page}")
            snapshot(name)
        after_settings = query("settings-status", "brightness")
        after_alarm = query("alarm-status", "alarm_hour")
        for key in ("brightness", "volume", "muted"):
            assert after_settings[key] == settings[key]
        for key in ("alarm_hour", "alarm_minute", "enabled"):
            assert after_alarm[key] == alarm[key]
        select(int(initial["mascot"]))
        time.sleep(1.2)
        snapshot("pet")
        query("ui-stats", "ui_frames")
        assert query("status", "firmware")["touch_errors"] == 0
        print("PASS: 54 poses, 3 companions, silent pet selection and taps, settings retention", flush=True)


if __name__ == "__main__":
    main()
