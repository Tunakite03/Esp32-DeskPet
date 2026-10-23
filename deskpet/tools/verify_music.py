"""Verify the Music page, SD playback controls, real audio output and alarm priority."""
from __future__ import annotations

import argparse
import json
import time

import serial


Scalar = str | int | float | bool | None
Record = dict[str, Scalar | list[dict[str, Scalar]]]


def query(port: serial.Serial, command: str, key: str) -> Record:
    port.write((command + "\n").encode("ascii"))
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline:
        line = port.readline().decode("utf-8", errors="replace").strip()
        if line.startswith("{"):
            record = json.loads(line)
            if isinstance(record, dict) and key in record:
                print(json.dumps({"command": command, "result": record}), flush=True)
                return record
    raise TimeoutError(f"No response to {command}")


def wait_phase(port: serial.Serial, expected: str) -> Record:
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline:
        status = query(port, "music-status", "music_phase")
        if status["music_phase"] == expected:
            return status
        time.sleep(0.1)
    raise AssertionError(f"Music did not enter {expected}")


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
        status = query(port, "status", "firmware")
        if status.get("music_ready") is not True:
            raise AssertionError("DeskPet must initialize an SD music player and expose its readiness")
        settings = query(port, "settings-status", "brightness")
        assets = query(port, "asset-status", "assets_ready")
        listing = query(port, "music-list", "tracks")
        tracks = listing["tracks"]
        if not isinstance(tracks, list) or len(tracks) < 2:
            raise AssertionError("Playback verification requires two MP3/WAV tracks of at least 8 seconds in /music")
        valid_tracks = [track for track in tracks if 'Z-invalid' not in str(track.get('title', ''))]
        fixtures = [track for track in valid_tracks if str(track.get('title', '')).startswith('DeskPet QA 20261022')]
        selected = fixtures[:2] if len(fixtures) >= 2 else valid_tracks[:2]
        try:
            for track in selected:
                index = track['index']
                query(port, f"music-play {index}", "music_phase")
                wait_phase(port, "playing")
                deadline = time.monotonic() + 4
                while time.monotonic() < deadline:
                    played = query(port, "music-status", "music_phase")
                    if played["music_elapsed_ms"] >= 300 and played["music_peak"] > 0:
                        break
                    time.sleep(0.1)
                else:
                    raise AssertionError("Decoded music must reach the physical I2S output with nonzero PCM")
                query(port, "music-pause", "music_phase")
                wait_phase(port, "paused")
                time.sleep(0.1)
                paused = query(port, "music-status", "music_phase")
                time.sleep(0.35)
                assert query(port, "music-status", "music_phase")["music_elapsed_ms"] == paused["music_elapsed_ms"]
                query(port, "music-resume", "music_phase")
                wait_phase(port, "playing")
            for track in tracks:
                if 'Z-invalid' in str(track.get('title', '')):
                    query(port, f"music-play {track['index']}", "music_phase")
                    wait_phase(port, "error")
                    query(port, 'music-stop', 'music_phase')
                    deadline = time.monotonic() + 3
                    while query(port, 'music-status', 'music_phase')['audio_busy']:
                        if time.monotonic() > deadline:
                            raise AssertionError('Invalid audio left the speaker permanently busy')
                        time.sleep(0.05)
            query(port, f"music-play {selected[0]['index']}", "music_phase")
            wait_phase(port, 'playing')
            port.write(b"page 3\n")
            assert query(port, "status", "firmware")["page"] == 3
            port.write(b"tap 160 138\n")
            wait_phase(port, "paused")
            port.write(b"tap 160 138\n")
            wait_phase(port, "playing")
            port.write(b"tap 264 140\n")
            assert wait_phase(port, "playing")["music_index"] == selected[1]['index']
            port.write(b"tap 56 140\n")
            assert wait_phase(port, "playing")["music_index"] == selected[0]['index']
            port.write(b"tap 264 188\n")
            first_index, second_index = selected[0]['index'], selected[1]['index']
            if type(first_index) is not int or type(second_index) is not int:
                raise ValueError('Library indexes must be integers')
            for _ in range(second_index // 3 - first_index // 3):
                port.write(b'tap 140 189\n')
            port.write(f'tap 160 {57 + second_index % 3 * 42}\n'.encode('ascii'))
            assert wait_phase(port, "playing")["music_index"] == selected[1]['index']
            volume = settings['volume']
            if type(volume) is not int:
                raise ValueError('Speaker volume must be an integer')
            # Music volume buttons snap to multiples of 5: up goes to the next one, down to the previous one.
            up = min(100, (volume // 5 + 1) * 5)
            down = max(0, volume - 5 if volume % 5 == 0 else volume // 5 * 5)
            if volume <= 95:
                first, second, expected_first, expected_second = b'tap 124 188\n', b'tap 30 188\n', up, max(0, up - 5)
            else:
                first, second, expected_first, expected_second = b'tap 30 188\n', b'tap 124 188\n', down, min(100, down + 5)
            port.write(first)
            assert query(port, 'settings-status', 'brightness')['volume'] == expected_first
            port.write(second)
            assert query(port, 'settings-status', 'brightness')['volume'] == expected_second
            if expected_second != volume:
                # The buttons cannot return to an arbitrary volume, so set the original value with the
                # Settings volume slider: its track starts at x=65 and is 190 px wide.
                slider_x = next(65 + k for k in range(191) if k * 100 // 190 == volume)
                port.write(b"page 4\n")
                port.write(f"tap {slider_x} 144\n".encode('ascii'))
                port.write(b"page 3\n")
                assert query(port, 'settings-status', 'brightness')['volume'] == volume
            port.write(b"page 1\ntap 234 189\n")
            alarm = query(port, "alarm-status", "alarm_hour")
            assert alarm["ringing"] is True
            assert wait_phase(port, "stopped")["music_phase"] == "stopped"
            port.write(b"tap 160 174\n")
            assert query(port, "alarm-status", "alarm_hour")["ringing"] is False
            after_settings = query(port, "settings-status", "brightness")
            after_assets = query(port, "asset-status", "assets_ready")
            for key in ("brightness", "volume", "muted", "talk"):
                assert after_settings[key] == settings[key]
            for key in ("asset_count", "asset_bytes", "mascot"):
                assert after_assets[key] == assets[key]
            print("Music integration PASS: MP3/WAV playback, pause/resume, Music navigation, alarm priority, saved settings.")
        finally:
            alarm = query(port, "alarm-status", "alarm_hour")
            if alarm.get("ringing") is True:
                port.write(b"tap 160 174\n")
            port.write(b"music-stop\npage 3\n")


if __name__ == "__main__":
    main()
