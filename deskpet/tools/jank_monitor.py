"""Record what the board is doing while someone uses it, to find stalls the user can feel."""
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
    args = parser.parse_args()
    port = serial.Serial()
    port.port, port.baudrate, port.timeout = args.port, 115200, 0.05
    port.dtr = False  # keep the USB-Serial/JTAG reset lines idle
    port.rts = False
    port.open()
    with port:
        start = time.monotonic()
        next_perf = next_talk = 0.0
        last_page = last_phase = None
        while time.monotonic() - start < args.seconds:
            now = time.monotonic() - start
            if now >= next_perf:
                port.write(b"perf\n")
                next_perf = now + 0.5
            if now >= next_talk:
                port.write(b"talk-status\nstatus\n")
                next_talk = now + 0.25
            line = port.readline().decode("utf-8", errors="replace").strip()
            if not line.startswith("{"):
                if line:
                    print(f"{now:6.1f}s  {line[:160]}", flush=True)
                continue
            record = json.loads(line)
            if "perf" in record:
                slow = record["max_loop_us"] > 120000 or record["max_input_latency_ms"] > 40
                if slow:
                    print(f"{now:6.1f}s  SLOW loop={record['max_loop_us'] / 1000:.0f}ms "
                          f"render={record['max_render_us'] / 1000:.0f}ms save={record['max_save_us'] / 1000:.0f}ms "
                          f"input_latency={record['max_input_latency_ms']}ms", flush=True)
            elif "talk_active" in record:
                if record["phase"] != last_phase:
                    print(f"{now:6.1f}s  talk -> {PHASES[record['phase']]} (rms {record['rms']}, floor {record['noise_floor']})",
                          flush=True)
                    last_phase = record["phase"]
            elif "firmware" in record:
                if record["page"] != last_page:
                    print(f"{now:6.1f}s  page -> {record['page']}", flush=True)
                    last_page = record["page"]
            elif "event" in record or record.get("level"):
                print(f"{now:6.1f}s  {line[:160]}", flush=True)


if __name__ == "__main__":
    main()
