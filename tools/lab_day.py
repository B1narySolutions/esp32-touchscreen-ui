#!/usr/bin/env python3
"""Hookup-day monitor: the touchscreen and the real Seed, side by side, as a live checklist.

Reads two consoles and never writes to either:
  * the touchscreen's native "USB" port (its DIAG line and seed_link log lines);
  * optionally the Seed's own USB serial (its 1 Hz status line and ">>> ..." messages). Nothing is
    ever sent to the Seed: its firmware treats some keys as commands ('B' reboots it into DFU).

Every second it prints what is working, what isn't yet, and the next thing to check, in the
order of LAB_DAY_CHECKLIST.md. With the Seed console it also cross-checks the DSP load and model
the touchscreen shows against what the Seed prints, which proves they are real readings.

    python tools/lab_day.py                       # ports found by USB id
    python tools/lab_day.py --screen /dev/cu.usbmodem1101 --seed /dev/cu.usbmodem3301
    python tools/lab_day.py --seconds 120         # stop after two minutes

Both consoles are also written to logs/lab_screen.txt and logs/lab_seed.txt.
"""

import argparse
import json
import re
import sys
import threading
import time
from pathlib import Path

import serial  # pyserial; ships in the ESP-IDF python env
from serial.tools import list_ports

ROOT = Path(__file__).resolve().parent.parent
ESP_VID = 0x303A                  # ESP32-S3 native USB Serial/JTAG
SEED_VID, SEED_PID = 0x0483, 0x5740  # STM32 USB CDC (the Seed's console)

# The Seed's 1 Hz status line (realtime-nam-seed3 src/main.cpp ReportStatus).
SEED_STATUS = re.compile(r"\[(\w+)\s+(.*?)\s*\]\s+cab\s+(\S+)\s+avg\s+(\d+)%.*?peak\s+(\d+)%.*?overruns\s+(\d+)")


def find_port(vid, pid=None):
    for p in list_ports.comports():
        if p.vid == vid and (pid is None or p.pid == pid):
            return p.device
    return None


REOPEN_GRACE_S = 1.5  # a board that reset: wait before opening (the touchscreen can hang if opened too soon)


class Console(threading.Thread):
    """Reads one port line by line into a log file and a list; never writes to it. Reopens the
    port when the board comes back after a reset or a replug."""

    def __init__(self, port, out):
        super().__init__(daemon=True)
        self.port, self.out = port, out
        self.lines = []
        self.lock = threading.Lock()
        self.error = None

    def run(self):
        with open(self.out, "wb") as sink:
            first = True
            while True:
                # Wait for the port, and give a board that just came back a moment to boot.
                if not first:
                    while not any(p.device == self.port for p in list_ports.comports()):
                        time.sleep(0.2)
                    time.sleep(REOPEN_GRACE_S)
                first = False
                try:
                    s = serial.Serial()
                    s.port, s.baudrate, s.timeout = self.port, 115200, 0.2
                    s.dtr = False  # opening with DTR/RTS asserted can reset a board through its auto-reset circuit
                    s.rts = False
                    s.open()
                except serial.SerialException as e:
                    self.error = f"{e} (retrying)"
                    time.sleep(1)
                    continue
                if not self.lines and self.error:
                    self.error = None
                self.read_until_gone(s, sink)

    def read_until_gone(self, s, sink):
        buf = bytearray()
        while True:
            try:
                chunk = s.read(4096)
            except serial.SerialException as e:
                self.error = f"{e} (the board reset or was unplugged; reopening when it's back)"
                try:
                    s.close()
                except serial.SerialException:
                    pass
                return
            if not chunk:
                continue
            sink.write(chunk)
            sink.flush()
            buf += chunk
            *done, rest = buf.split(b"\n")
            buf = bytearray(rest)
            with self.lock:
                self.lines += [d.decode("utf-8", "replace").rstrip("\r") for d in done]

    def take(self):
        with self.lock:
            out, self.lines = self.lines, []
        return out


class State:
    def __init__(self):
        self.diag = None          # last touchscreen DIAG dict
        self.first_diag = None
        self.hello = None         # last "Seed HELLO" log line from the touchscreen
        self.mock = False
        self.seed_status = None   # last parsed Seed status line
        self.seed_status_at = 0.0  # when it arrived (time.monotonic)
        self.seed_link_up = None  # from the Seed's ">>> touchscreen link up/lost"
        self.seed_events = []     # other ">>> ..." lines from the Seed
        self.err_start = None
        self.started = time.monotonic()

    def screen_line(self, line):
        i = line.find("DIAG {")
        if i >= 0:
            try:
                d = json.loads(line[i + 5:])
            except json.JSONDecodeError:
                return
            self.diag = d
            if self.first_diag is None:
                self.first_diag = d
                self.err_start = d["seed"]["err"]
            return
        if "seed_link:" in line and "HELLO:" in line:
            self.hello = line.split("seed_link: ", 1)[-1]
            self.mock = "MOCK" in line
        elif "seed_link:" in line and ("link lost" in line or "link back" in line):
            self.seed_events.append("touchscreen: " + line.split("seed_link: ", 1)[-1])

    def seed_line(self, line):
        m = SEED_STATUS.search(line)
        if m:
            self.seed_status = dict(mode=m[1], model=m[2], cab=m[3], avg=int(m[4]), peak=int(m[5]), overruns=int(m[6]))
            self.seed_status_at = time.monotonic()
        elif ">>> touchscreen link" in line:
            self.seed_link_up = line.rstrip().endswith("up")
        elif ">>>" in line:
            self.seed_events.append("Seed: " + line[line.find(">>>"):].strip())


def checklist(st, have_seed_console):
    """Returns (rows, next_step). Rows are (mark, text); marks are OK, .. (waiting) and !!."""
    rows = []
    if not st.diag:
        return [("..", "touchscreen console: waiting for the first DIAG line")], \
            "If nothing appears in ~10 s, press the touchscreen's RESET button (startup hang)."
    s = st.diag["seed"]
    age = time.monotonic() - st.started
    rows.append(("OK", f"touchscreen running: fw {st.diag.get('fw')}, up {st.diag['up']} s"))
    # The Seed's own console works without the link, so it is shown from the start.
    seed_fresh = st.seed_status is not None and time.monotonic() - st.seed_status_at < 3
    if have_seed_console:
        if st.seed_status and not seed_fresh:
            rows.append(("..", f"Seed console: no status line for {time.monotonic() - st.seed_status_at:.0f} s "
                               "(Seed resetting, or its USB unplugged)"))
        elif st.seed_status:
            ss = st.seed_status
            rows.append(("OK", f"Seed console: [{ss['mode']} {ss['model']}] cab {ss['cab']}, DSP avg {ss['avg']} %, "
                               f"peak {ss['peak']} %, overruns {ss['overruns']}"))
        else:
            rows.append(("..", "Seed console: waiting for its 1 Hz status line"))

    # 1. Bytes from the Seed at all.
    if s["rx"] == 0:
        rows.append(("!!" if age > 10 else "..", "bytes from the Seed: none yet (rx 0)"))
        return rows, ("No bytes arrive. Power off and check: Seed D13 -> H3 pin 3 and D14 -> H3 pin 4 "
                      "(swap them if unsure), GND -> H3 pin 2, SW1 on UART2, and that the Seed runs the "
                      "build with the link.")
    errs = s["err"] - (st.err_start or 0)
    if errs > 5 and s["conn"] is False:
        rows.append(("!!", f"bytes arrive but don't decode (rx {s['rx']}, errors +{errs})"))
        return rows, "Wrong baud or a noisy line: both sides must be 1,000,000 baud; check the GND wire."
    rows.append(("OK", f"bytes from the Seed: rx {s['rx']} frames, tx {s['tx']}"))

    # 2. Handshake.
    if not s["conn"]:
        rows.append(("..", "Seed HELLO: not yet"))
        return rows, "Frames arrive but no HELLO yet; wait a second, or check the Seed's firmware version."
    rows.append(("!!" if st.mock or s.get("mock") else "OK",
                 f"Seed HELLO: fw \"{s['fw']}\", {s['sr']} Hz / {s['blk']}"
                 + ("  <- a SIMULATOR (MOCK), not the real Seed" if st.mock or s.get("mock") else "")))
    if have_seed_console:
        if st.seed_link_up is None:
            rows.append(("..", "Seed console: no '>>> touchscreen link up' yet"))
        else:
            rows.append(("OK" if st.seed_link_up else "!!",
                         "Seed console: touchscreen link " + ("up" if st.seed_link_up else "LOST")))

    # 3. Link health.
    rows.append(("OK" if errs <= 2 else "!!", f"link errors since start: {errs} (a couple at connect is normal)"))
    rows.append(("OK" if s["ovr"] <= 1 else "!!", f"UART overruns on the touchscreen: {s['ovr']} (one at connect is normal)"))
    rows.append(("OK" if s["snap_ok"] and s["snap_ok"] >= s["snaps"] - 1 else "..",
                 f"snapshots confirmed by the Seed: {s['snap_ok']} of {s['snaps']}"))
    rows.append(("OK" if s["rtt"] > 0 else "..",
                 f"ping round trip: {s['rtt']:.2f} ms" if s["rtt"] > 0 else "ping: press Test Mode > Ping SEED"))

    # 4. Telemetry, cross-checked against the Seed's own console.
    if not s.get("stv"):
        rows.append(("..", "Seed STATUS (DSP load, model): none yet"))
    else:
        text = f"Seed STATUS: DSP {s['cpu']:.1f} %, model \"{s['nam']}\", Seed overruns {s['dovr']}"
        mark = "OK"
        if seed_fresh:
            seed_avg = st.seed_status["avg"]
            same_cpu = abs(s["cpu"] - seed_avg) <= 3
            text += f"  | Seed console: avg {seed_avg} %, [{st.seed_status['mode']} {st.seed_status['model']}]"
            text += "  (match)" if same_cpu else "  (DSP load DIFFERS)"
            mark = "OK" if same_cpu else "!!"
        rows.append((mark, text))
    rows.append(("OK" if s["metv"] else "..",
                 f"Seed METERS: in {s['inpk']:.1f} dBFS, out {s['outpk']:.1f} dBFS, clips {s['clips']}"
                 if s["metv"] else "Seed METERS: none in the last second"))

    if st.mock or s.get("mock"):
        return rows, "This peer is a PC simulator. For the real Seed: its link build, its USB cable, SW1 on UART2."
    if any(m == "!!" for m, _ in rows):
        return rows, "Look at the [!!] rows above; LAB_DAY_CHECKLIST.md section 5 lists the usual causes."
    if not s.get("stv") or not s["metv"]:
        return rows, "Link is up; STATUS/METERS should follow within a second (the Seed sends them)."
    return rows, ("All link checks pass. Now LAB_DAY_CHECKLIST.md section 6: mute, master, amp cards, "
                  "meters while playing, SD profile, Seed reboot, cable pull.")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--screen", help="touchscreen native USB port (default: found by USB id)")
    ap.add_argument("--seed", help="Seed USB console port (default: found by USB id; optional)")
    ap.add_argument("--seconds", type=float, default=0, help="stop after this long (0 = until Ctrl-C)")
    args = ap.parse_args()

    screen = args.screen or find_port(ESP_VID)
    if not screen:
        sys.exit("touchscreen not found: plug in its \"USB\" port (not \"USB TO UART\")")
    seed = args.seed or find_port(SEED_VID, SEED_PID)
    logs = ROOT / "logs"
    logs.mkdir(exist_ok=True)
    print(f"touchscreen: {screen}\nSeed console: {seed or 'not connected (optional; cross-checks need it)'}")
    print("Reading only; nothing is sent to either board. Ctrl-C to stop.\n")

    st = State()
    con_screen = Console(screen, logs / "lab_screen.txt")
    con_screen.start()
    con_seed = None
    if seed:
        con_seed = Console(seed, logs / "lab_seed.txt")
        con_seed.start()

    last = None
    shown_events = 0
    end = time.monotonic() + args.seconds if args.seconds else None
    try:
        while end is None or time.monotonic() < end:
            time.sleep(1)
            for line in con_screen.take():
                st.screen_line(line)
            if con_seed:
                for line in con_seed.take():
                    st.seed_line(line)
            for c in (con_screen, con_seed):
                if c and c.error:
                    print(f"!! {c.port}: {c.error}")
                    c.error = None
            for e in st.seed_events[shown_events:]:
                print(f"   event: {e}")
            shown_events = len(st.seed_events)
            rows, nxt = checklist(st, con_seed is not None)
            block = "\n".join(f"  [{m}] {t}" for m, t in rows) + f"\n  next: {nxt}"
            # Reprint only when something other than the counters changed, to keep the log readable.
            key = re.sub(r"\d+(\.\d+)?", "#", block)
            if key != last:
                print(time.strftime("%H:%M:%S") + "\n" + block + "\n", flush=True)
                last = key
    except KeyboardInterrupt:
        pass
    rows, nxt = checklist(st, con_seed is not None)
    print("final:\n" + "\n".join(f"  [{m}] {t}" for m, t in rows) + f"\n  next: {nxt}")


if __name__ == "__main__":
    main()
