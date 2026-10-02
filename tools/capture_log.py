#!/usr/bin/env python3
"""Capture N seconds of the board's serial output to a file, then summarise the DIAG lines.

idf.py monitor is interactive; this is the non-interactive replacement for reading boot logs
and the once-a-second "DIAG {json}" line (firmware/main/diag.c).

    python tools/capture_log.py --port COM7 --seconds 60 --out logs/baseline_idle.txt
    python tools/capture_log.py --port COM7 --seconds 20 --reset --out logs/boot.txt
    python tools/capture_log.py --summarise logs/baseline_idle.txt   # re-summarise a saved log
"""

import argparse
import json
import sys
import time
from pathlib import Path

# (label, path into the DIAG json)
FIELDS = [
    ("frame_avg ms", ("ui", "frame_avg")),
    ("frame_max ms", ("ui", "frame_max")),
    ("tap ms", ("ui", "tap")),
    ("redraws/s", ("fps",)),
    ("cpu0 %", ("cpu", 0)),
    ("cpu1 %", ("cpu", 1)),
    ("heap KB", ("heap",)),
    ("heap_min KB", ("heap_min",)),
    ("heap_big KB", ("heap_big",)),
    ("psram KB", ("psram",)),
    ("lvgl_stuck", ("lvgl_stuck",)),
    ("rgb_restarts", ("rgb_restarts",)),
    ("exio_lost", ("exio_lost",)),
]
KB_FIELDS = {"heap KB", "heap_min KB", "heap_big KB", "psram KB"}


def dig(obj, path):
    for key in path:
        obj = obj[key]
    return obj


def parse_diag(lines):
    out = []
    for line in lines:
        i = line.find("DIAG {")
        if i < 0:
            continue
        try:
            out.append(json.loads(line[i + 5:]))
        except json.JSONDecodeError:
            pass  # a line torn by a reset or other output
    return out


def summarise(lines, skip_zero_tap=True):
    diags = parse_diag(lines)
    if not diags:
        return "no DIAG lines captured"
    rows = [f"{len(diags)} DIAG lines, uptime {diags[0]['up']} .. {diags[-1]['up']} s, "
            f"fw {diags[-1].get('fw')} #{diags[-1].get('elf')}",
            f"{'field':<14}{'min':>10}{'avg':>10}{'max':>10}{'n':>6}"]
    for label, path in FIELDS:
        vals = []
        for d in diags:
            try:
                v = dig(d, path)
            except (KeyError, IndexError, TypeError):
                continue
            if v is None:
                continue
            # tap is 0 in seconds with no touch, and frame times are 0 when nothing redrew
            if skip_zero_tap and label in ("tap ms", "frame_avg ms", "frame_max ms") and v == 0:
                continue
            vals.append(v / 1024 if label in KB_FIELDS else v)
        if not vals:
            rows.append(f"{label:<14}{'-':>10}{'-':>10}{'-':>10}{0:>6}")
            continue
        rows.append(f"{label:<14}{min(vals):>10.1f}{sum(vals) / len(vals):>10.1f}{max(vals):>10.1f}{len(vals):>6}")
    return "\n".join(rows)


def capture(port, baud, seconds, reset, wait=0.0, out=None):
    import serial  # pyserial; ships in the ESP-IDF python env

    # The native USB console port disappears while the chip resets; --wait keeps retrying.
    deadline = time.monotonic() + wait
    while True:
        try:
            probe = serial.Serial()
            probe.port = port
            probe.dtr = probe.rts = False  # opening with them asserted could reset the board
            probe.open()
            probe.close()
            break
        except serial.SerialException:
            if time.monotonic() >= deadline:
                raise
            time.sleep(0.05)

    # Set DTR/RTS before opening: on Windows, Serial(port) opens with both asserted, and the
    # board's auto-reset circuit turns that into a reset, so every capture would reboot it.
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, baud, 0.2
    s.dtr = False
    s.rts = False
    s.open()
    if reset:
        # Same sequence as esptool's hard reset: EN low via RTS with IO0 left high.
        s.rts = True
        time.sleep(0.1)
        s.rts = False
    buf = bytearray()
    end = time.monotonic() + seconds
    # Written as it arrives, so a capture stopped early (or killed) still leaves its log.
    sink = None
    if out:
        out.parent.mkdir(parents=True, exist_ok=True)
        sink = open(out, "wb")
    try:
        while time.monotonic() < end:
            chunk = s.read(4096)
            if chunk:
                buf += chunk
                if sink:
                    sink.write(chunk)
                    sink.flush()
    finally:
        s.close()
        if sink:
            sink.close()
    return buf.decode("utf-8", errors="replace").splitlines()


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--port", default="COM7")
    p.add_argument("--baud", type=int, default=115200)
    p.add_argument("--seconds", type=float, default=30)
    p.add_argument("--reset", action="store_true", help="reset the board first (captures the boot log)")
    p.add_argument("--wait", type=float, default=0.0, help="seconds to wait for the port to appear")
    p.add_argument("--out", type=Path, help="file to write the raw log to")
    p.add_argument("--summarise", type=Path, help="only summarise an existing log file")
    args = p.parse_args()

    if args.summarise:
        print(summarise(args.summarise.read_text(encoding="utf-8", errors="replace").splitlines()))
        return

    lines = capture(args.port, args.baud, args.seconds, args.reset, args.wait, args.out)
    if args.out:
        print(f"wrote {len(lines)} lines to {args.out}")
    print(summarise(lines))


if __name__ == "__main__":
    sys.exit(main())
