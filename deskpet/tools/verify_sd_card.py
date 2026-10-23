"""Verify automatic SD mounting, real file I/O and remounting after a DeskPet reboot."""
from __future__ import annotations

import argparse
import json
import time

import serial


Scalar = str | int | float | bool | None


def query(port: serial.Serial, command: str, key: str) -> dict[str, Scalar]:
    port.write((command + "\n").encode("ascii"))
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline:
        raw = port.readline().decode("utf-8", errors="replace").strip()
        if not raw.startswith("{"):
            continue
        parsed: object = json.loads(raw)
        if not isinstance(parsed, dict) or key not in parsed:
            continue
        record: dict[str, Scalar] = {
            name: value for name, value in parsed.items()
            if isinstance(name, str) and isinstance(value, (str, int, float, bool, type(None)))
        }
        if key not in record:
            raise ValueError(f"Invalid scalar field {key} in response to {command}")
        print(json.dumps({"command": command, "result": record}), flush=True)
        return record
    raise TimeoutError(f"No {key} response to {command} on {port.port}")


def require_ready(status: dict[str, Scalar]) -> None:
    if status.get("sd_mounted") is not True:
        raise AssertionError("DeskPet must mount the inserted SD card automatically at boot")
    if status.get("sd_writable") is not True:
        raise AssertionError("DeskPet must verify that the mounted SD card accepts and retains writes")


def verify_storage(port: serial.Serial) -> None:
    status = query(port, "sd-status", "sd_mounted")
    require_ready(status)
    capacity = status.get("sd_capacity_bytes")
    if type(capacity) is not int or capacity <= 0:
        raise AssertionError("A mounted SD card must report a positive capacity")
    if status.get("sd_bus_width") not in (1, 4) or status.get("sd_boot_record_valid") is not True:
        raise AssertionError("The SD card must have an active bus and a verified persistent boot record")
    tested = query(port, "sd-selftest", "sd_test")
    if tested.get("sd_test") != "pass" or tested.get("bytes") != 128 or tested.get("cleanup") is not True:
        raise AssertionError("The SD self-test must write, reopen, compare and remove its temporary file")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    args = parser.parse_args()
    port = serial.Serial()
    port.port, port.baudrate, port.timeout, port.write_timeout = args.port, 115200, 0.2, 3
    port.dtr = False
    port.rts = False
    port.open()
    with port:
        port.reset_input_buffer()
        initial = query(port, "status", "firmware")
        require_ready(initial)
        settings = query(port, "settings-status", "brightness")
        assets = query(port, "asset-status", "assets_ready")
        verify_storage(port)
        port.write(b"reboot\n")
        deadline = time.monotonic() + 15
        boot_seen = False
        boot_status: dict[str, Scalar] | None = None
        while time.monotonic() < deadline:
            raw = port.readline().decode("utf-8", errors="replace").strip()
            if not raw.startswith("{"):
                continue
            record: object = json.loads(raw)
            if not isinstance(record, dict):
                continue
            if record.get("event") == "boot":
                boot_seen = True
            if boot_seen and "firmware" in record:
                boot_status = record
                break
        if not boot_seen or boot_status is None:
            raise TimeoutError("DeskPet did not finish booting within 15 seconds after reboot")
        require_ready(boot_status)
        if boot_status.get("firmware") != initial.get("firmware"):
            raise AssertionError("Reboot must keep the running firmware version")
        verify_storage(port)
        after_settings = query(port, "settings-status", "brightness")
        after_assets = query(port, "asset-status", "assets_ready")
        for key in ("brightness", "volume", "muted", "talk"):
            if settings[key] != after_settings[key]:
                raise AssertionError(f"SD initialization changed the saved device setting: {key}")
        for key in ("assets_ready", "asset_count", "asset_bytes", "mascot"):
            if assets[key] != after_assets[key]:
                raise AssertionError(f"SD initialization changed the flash asset state: {key}")
        print("SD integration PASS: automatic mount, verified boot record, file I/O, reboot, settings and assets.")


if __name__ == "__main__":
    main()
