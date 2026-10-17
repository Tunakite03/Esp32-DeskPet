"""Watch talk mode live: microphone level, phase changes and repeated phrases."""
from __future__ import annotations

import argparse
import json
import time

import serial

PHASES = ["off", "listening", "hearing", "talking"]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="COM9")
    parser.add_argument("--seconds", type=float, default=120)
    parser.add_argument("--tune", nargs=2, type=int, metavar=("START", "END"),
                        help="minimum start/end RMS to apply before watching")
    args = parser.parse_args()
    with serial.Serial(args.port, 115200, timeout=0.1) as port:
        time.sleep(1.5)
        port.reset_input_buffer()
        if args.tune:
            port.write(f"talk-tune {args.tune[0]} {args.tune[1]}\n".encode())
        port.write(b"pet\n")
        start = time.monotonic()
        next_poll = 0.0
        second_peak, last_phase, last_report = 0, None, 0.0
        while time.monotonic() - start < args.seconds:
            now = time.monotonic() - start
            if now >= next_poll:
                port.write(b"talk-status\n")
                next_poll += 0.25
            line = port.readline().decode("utf-8", errors="replace").strip()
            if not line:
                continue
            if "talk_phrase" in line or "recording_complete" in line or "Backtrace" in line or '"level":"error"' in line:
                print(f"{now:6.1f}s  {line}", flush=True)
                continue
            if "talk_active" not in line:
                continue
            status = json.loads(line)
            second_peak = max(second_peak, status["peak_rms"])
            if status["phase"] != last_phase:
                print(f"{now:6.1f}s  -> {PHASES[status['phase']]:9} floor={status['noise_floor']}", flush=True)
                last_phase = status["phase"]
            if now - last_report >= 1:
                start_threshold = max(status["noise_floor"] * 3, status["min_start"])
                bar = "#" * min(60, second_peak // 10)
                print(f"{now:6.1f}s  peak={second_peak:5} start>{start_threshold:4} {bar}", flush=True)
                second_peak, last_report = 0, now


if __name__ == "__main__":
    main()
