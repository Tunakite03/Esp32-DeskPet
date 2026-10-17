"""Log everything the board prints, reconnecting whenever USB drops, to catch resets and panics."""
from __future__ import annotations

import argparse
import time

import serial
from serial.tools import list_ports


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="COM9")
    parser.add_argument("--seconds", type=float, default=60)
    args = parser.parse_args()
    started = time.monotonic()
    drops = 0
    while time.monotonic() - started < args.seconds:
        if args.port not in [p.device for p in list_ports.comports()]:
            time.sleep(0.2)
            continue
        try:
            with serial.Serial(args.port, 115200, timeout=0.2) as port:
                print(f"{time.monotonic() - started:6.1f}s [connected]", flush=True)
                last_poll = 0.0
                while time.monotonic() - started < args.seconds:
                    if time.monotonic() - last_poll > 5:
                        port.write(b"status\n")
                        last_poll = time.monotonic()
                    line = port.readline()
                    if line:
                        print(f"{time.monotonic() - started:6.1f}s {line.decode('utf-8', errors='replace').rstrip()[:220]}",
                              flush=True)
        except (serial.SerialException, OSError) as error:
            drops += 1
            print(f"{time.monotonic() - started:6.1f}s [usb dropped: {error.__class__.__name__}]", flush=True)
            time.sleep(0.3)
    print(f"usb drops: {drops}")


if __name__ == "__main__":
    main()
