"""Measure UI responsiveness on the board: idle pages, tab switching and settings edits."""
from __future__ import annotations

import argparse
import json
import time

import serial


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="COM9")
    args = parser.parse_args()
    # DTR/RTS stay low so opening the port does not reset the board (USB-Serial/JTAG reset lines).
    port = serial.Serial()
    port.port, port.baudrate, port.timeout = args.port, 115200, 0.1
    port.dtr = False
    port.rts = False
    port.open()
    with port:
        time.sleep(2)
        port.reset_input_buffer()

        def send(command: str) -> None:
            port.write((command + "\n").encode("ascii"))

        def query(command: str, key: str, attempts: int = 1) -> dict:
            for _ in range(attempts):
                send(command)
                deadline = time.monotonic() + 4
                while time.monotonic() < deadline:
                    line = port.readline().decode("utf-8", errors="replace").strip()
                    if line.startswith("{") and key in line:
                        return json.loads(line)
            raise TimeoutError(command)

        # Opening the port can reset the board; wait until it answers.
        query("status", "firmware", attempts=8)
        time.sleep(3)

        def scenario(name: str, actions, seconds: float) -> None:
            query("perf", "perf")  # reset counters
            started = time.monotonic()
            step = 0
            while time.monotonic() - started < seconds:
                if actions:
                    command, pause = actions[step % len(actions)]
                    send(command)
                    step += 1
                    time.sleep(pause)
                else:
                    time.sleep(0.2)
            result = query("perf", "perf")
            fps = result["renders"] / seconds
            print(f"{name:28} renders/s={fps:5.1f} avg_render={result['avg_render_us'] / 1000:5.1f}ms "
                  f"max_render={result['max_render_us'] / 1000:5.1f}ms max_loop={result['max_loop_us'] / 1000:5.1f}ms "
                  f"max_save={result['max_save_us'] / 1000:5.1f}ms input_latency={result['max_input_latency_ms']}ms",
                  flush=True)

        settings = query("settings-status", "talk")
        alarm = query("alarm-status", "alarm_hour")
        send("pet"); time.sleep(1)
        scenario(f"pet idle (talk {'on' if settings['talk'] else 'off'})", [], 8)
        scenario("clock idle", [("page 1", 8)], 8)
        scenario("tab switching every 0.3s", [("page 0", 0.3), ("page 1", 0.3), ("page 2", 0.3), ("page 3", 0.3)], 8)
        scenario("settings: brightness +/-", [("page 3", 0.2), ("tap 290 80", 0.7), ("tap 30 80", 0.7)], 8)
        scenario("clock: alarm hour +/-", [("page 1", 0.2), ("tap 134 150", 0.7), ("tap 34 150", 0.7)], 8)

        # Put the user's brightness and alarm hour back exactly as they were.
        send("page 3"); time.sleep(0.2)
        for _ in range(12):
            now = query("settings-status", "brightness")["brightness"]
            if now == settings["brightness"]:
                break
            send("tap 30 80" if now > settings["brightness"] else "tap 290 80"); time.sleep(0.3)
        send("page 1"); time.sleep(0.2)
        for _ in range(24):
            hour = query("alarm-status", "alarm_hour")["alarm_hour"]
            if hour == alarm["alarm_hour"]:
                break
            send("tap 134 150"); time.sleep(0.2)
        time.sleep(1)
        after = query("settings-status", "brightness")
        after_alarm = query("alarm-status", "alarm_hour")
        print("restored:", after["brightness"] == settings["brightness"], after_alarm["alarm_hour"] == alarm["alarm_hour"],
              "pending:", after["settings_pending"])
        send("pet")


if __name__ == "__main__":
    main()
