"""Copy MP3 or WAV tracks onto the DeskPet SD card over the USB serial cable.

Each track goes to /music/<name>.part first. DeskPet reads it back, checks the
CRC32 and only then renames it, so an interrupted upload never shows up in the
library. Stop playback before uploading (Stop on the Music page or music-stop).

    python tools/upload_music.py --port COM9 "E:\\Music\\track.mp3" "E:\\Music\\other.wav"

Names must be plain ASCII. Each chunk waits for DeskPet's reply, so the serial
buffer never overflows; expect a few minutes for a few megabytes.
"""
from __future__ import annotations

import argparse
import base64
import contextlib
import json
import time
import zlib
from pathlib import Path

import serial

# Keep each sd-put-data line (about 205 bytes) below the 256-byte USB serial receive
# buffer: a longer burst loses bytes, and the device then rejects the line.
CHUNK_BYTES = 144
MAX_NAME = 120
PROGRESS_EVERY = 1024 * 1024


def read_json(port: serial.Serial, key: str, timeout_s: float = 15.0) -> dict:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        raw = port.readline().decode("utf-8", errors="replace").strip()
        if not raw.startswith("{"):
            continue
        record = json.loads(raw)
        if isinstance(record, dict) and key in record:
            return record
    raise TimeoutError(f"No {key} reply from DeskPet")


def send(port: serial.Serial, command: str, key: str = "sd_put", timeout_s: float = 15.0) -> dict:
    port.write((command + "\n").encode("ascii"))
    return read_json(port, key, timeout_s)


def check_name(path: Path) -> str:
    name = path.name
    if not name.isascii() or len(name) > MAX_NAME:
        raise ValueError(f"{name}: use an ASCII name of at most {MAX_NAME} characters")
    if not name.lower().endswith((".mp3", ".wav")):
        raise ValueError(f"{name}: only .mp3 and .wav files can be copied")
    return name


def upload(port: serial.Serial, path: Path) -> dict:
    name = check_name(path)
    data = path.read_bytes()
    if not data:
        raise ValueError(f"{name} is empty")
    # DeskPet rescans the library after each upload; wait for that scan to finish
    # rather than failing, but stop at once for any other refusal.
    deadline = time.monotonic() + 15
    while True:
        reply = send(port, f"sd-put-begin {len(data)} {name}")
        if reply["sd_put"] == "ready":
            break
        busy = reply.get("error") == "Stop music before uploading"
        if not busy or time.monotonic() > deadline:
            raise RuntimeError(f"{name}: {reply.get('error', reply)}")
        time.sleep(0.5)
    try:
        sent = 0
        for offset in range(0, len(data), CHUNK_BYTES):
            chunk = base64.b64encode(data[offset:offset + CHUNK_BYTES]).decode("ascii")
            reply = send(port, f"sd-put-data {chunk}")
            if reply["sd_put"] != "ok":
                raise RuntimeError(f"{name}: {reply.get('error', reply)}")
            previous, sent = sent, offset + CHUNK_BYTES
            if previous // PROGRESS_EVERY != sent // PROGRESS_EVERY:
                print(f"  {name}: {min(sent, len(data))} of {len(data)} bytes", flush=True)
        # DeskPet reads the whole file back before it answers, which takes longer for big tracks.
        reply = send(port, f"sd-put-end {zlib.crc32(data):08x}", timeout_s=180)
        if reply["sd_put"] != "done":
            raise RuntimeError(f"{name}: {reply.get('error', reply)}")
        return reply
    except BaseException:
        with contextlib.suppress(Exception):
            send(port, "sd-put-abort")
        raise


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", required=True)
    parser.add_argument("files", nargs="+", type=Path)
    args = parser.parse_args()
    for path in args.files:
        check_name(path)
    port = serial.Serial()
    port.port, port.baudrate, port.timeout, port.write_timeout = args.port, 115200, 0.2, 3
    port.dtr = False
    port.rts = False
    port.open()
    with port:
        port.reset_input_buffer()
        for path in args.files:
            print(json.dumps(upload(port, path)), flush=True)
        # The rescan after the last upload empties the list while it runs, so wait for it.
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            status = send(port, "music-status", key="music_phase")
            if status["music_phase"] != "scanning":
                break
            time.sleep(0.5)
        port.write(b"music-list\n")
        print(json.dumps(read_json(port, "music_tracks", timeout_s=8)), flush=True)


if __name__ == "__main__":
    main()
