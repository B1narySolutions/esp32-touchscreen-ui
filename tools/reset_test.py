#!/usr/bin/env python3
"""Resets the board repeatedly and checks it boots all the way to the DIAG line each time.

    python tools/reset_test.py --console COM8 --uart COM7 --count 20 [--method usb|en]

--method usb  resets through the native USB port (USB Serial/JTAG), as idf.py/esptool do on it.
--method en   resets through the USB TO UART port's auto-reset circuit (drives the chip's EN pin;
              needs the UART switch SW1 on UART1).
A boot that doesn't print DIAG within the timeout counts as a hang, and the board is recovered
with an EN reset before the next round.
"""

import argparse
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))
from capture_log import capture  # noqa: E402


def esptool_reset(port, command="flash_id"):
    # flash_id runs esptool's stub. A ROM-only command (chip_id) over the native USB port leaves
    # the chip stuck at PSRAM timing tuning after the hard reset: use --rom-only to reproduce it.
    r = subprocess.run([sys.executable, "-m", "esptool", "--chip", "esp32s3", "-p", port, "--after", "hard_reset",
                        command], capture_output=True, text=True)
    return r.returncode == 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--console", default="COM8")
    ap.add_argument("--uart", default="COM7")
    ap.add_argument("--count", type=int, default=20)
    ap.add_argument("--method", choices=["usb", "en"], default="usb")
    ap.add_argument("--timeout", type=float, default=10.0)
    ap.add_argument("--rom-only", action="store_true", help="reset with esptool chip_id instead of flash_id")
    ap.add_argument("--settle", type=float, default=0.0, help="extra seconds to leave the port alone after the reset")
    ap.add_argument("--no-grace", action="store_true",
                    help="open the console port as soon as it exists (reproduces the startup hang)")
    args = ap.parse_args()

    hangs = 0
    for i in range(1, args.count + 1):
        ok_reset = esptool_reset(args.console if args.method == "usb" else args.uart,
                                 "chip_id" if args.rom_only else "flash_id")
        time.sleep(args.settle)
        try:
            lines = capture(args.console, 115200, args.timeout, False, wait=15, reset_grace=not args.no_grace)
        except Exception as e:  # the port may not come back at all
            lines = [f"capture failed: {e}"]
        booted = any(l.startswith("DIAG ") or "DIAG {" in l for l in lines)
        last = next((l for l in reversed(lines) if l.strip()), "(no output)")
        if not booted:
            hangs += 1
            print(f"{i:3d}: HANG (reset {'ok' if ok_reset else 'failed'}); last line: {last[:90]}")
            esptool_reset(args.uart, "chip_id")  # recover through EN (ROM-only is fine on the CH343)
            time.sleep(3)
        else:
            print(f"{i:3d}: booted")
    print(f"{args.count} resets via {args.method}: {hangs} hang(s)")
    return 1 if hangs else 0


if __name__ == "__main__":
    sys.exit(main())
