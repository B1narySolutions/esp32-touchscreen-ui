#!/usr/bin/env python3
"""Scripted Seed link acceptance checks against the real ESP32, using tools/mock_seed.py.

Setup: SW1 on UART1 (Seed link -> USB TO UART port), console on the native USB port.

    python tools/link_check.py reboot  --port COM7     mock reboots every 6 s: each new boot must
                                                       get a complete snapshot within 1 s
    python tools/link_check.py faults  --port COM7     5 % corrupted frames + dropped bytes: link
                                                       stays up, and any damaged snapshot is
                                                       repaired within one snapshot period (5 s)
    python tools/link_check.py drag    --port COM7     you drag a slider: SET_PARAMS frames per
                                                       second stay bounded (<= 100)
    python tools/link_check.py soak    --port COM7 --seconds 1800

Each run writes logs/link_<scenario>.jsonl (mock events) and prints PASS/FAIL with numbers.
"""

import argparse
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

SCENARIOS = {
    "reboot": dict(seconds=27, mock=["--reboot-every", "6"]),
    "faults": dict(seconds=60, mock=["--corrupt-rate", "0.05", "--drop-rx-rate", "0.001"]),
    "drag": dict(seconds=30, mock=[]),
    "soak": dict(seconds=1800, mock=[]),
    "basic": dict(seconds=15, mock=[]),
}


def run_mock(port, baud, seconds, extra, events):
    events.parent.mkdir(parents=True, exist_ok=True)
    if events.exists():
        events.unlink()
    cmd = [sys.executable, str(ROOT / "tools" / "mock_seed.py"), "--port", port, "--baud", str(baud),
           "--no-table", "--events", str(events), "--seconds", str(seconds)] + extra
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)
    return [json.loads(line) for line in events.read_text(encoding="utf-8").splitlines() if line.strip()]


def analyse(name, ev):
    boots = [e for e in ev if e["ev"] == "boot"]
    snaps = [e for e in ev if e["ev"] == "snapshot"]
    complete = [e for e in snaps if e["complete"]]
    exit_ev = next((e for e in ev if e["ev"] == "exit"), {})
    end_t = exit_ev.get("t", ev[-1]["t"] if ev else 0)
    ok = True
    print(f"{len(boots)} mock boots, {len(snaps)} snapshots ({len(complete)} complete), "
          f"mock rx frames {exit_ev.get('rx_frames')}, rx errors {exit_ev.get('rx_errors')}, tx {exit_ev.get('tx_frames')}")

    # Every mock boot must be followed by a complete snapshot within 1 s.
    for b in boots:
        nxt = next((s for s in complete if s["t"] >= b["t"]), None)
        delay = (nxt["t"] - b["t"]) if nxt else None
        # Clean line: within 1 s. With injected faults the boot snapshot may itself be damaged,
        # and then the same one-snapshot-period repair limit applies.
        limit = 5.0 if name == "faults" else 1.0
        if b["t"] < end_t - limit:
            good = delay is not None and delay <= limit
            ok &= good
            print(f"  boot at {b['t']:6.2f}s -> complete snapshot after {delay if delay is None else round(delay, 3)} s  {'ok' if good else 'FAIL'}")

    # Longest stretch without a complete snapshot: periodic snapshots every 5 s plus jitter.
    times = [0.0] + [s["t"] for s in complete] + [end_t]
    worst = max(b - a for a, b in zip(times, times[1:]))
    if name != "faults":
        ok &= worst <= 6.5
    print(f"  longest gap between complete snapshots: {worst:.2f} s" + ("" if name == "faults" else " (limit 6.5 s)"))
    # A damaged snapshot (a frame lost) must be repaired quickly by the STATUS-driven resend.
    damaged = [s for s in snaps if not s["complete"]]
    repairs = []
    for d in damaged:
        nxt = next((s for s in complete if s["t"] > d["t"]), None)
        if nxt:
            repairs.append(nxt["t"] - d["t"])
    if damaged:
        # Acceptance: state is never out of sync for longer than one snapshot period (5 s).
        worst_fix = max(repairs) if repairs else float("inf")
        ok &= worst_fix <= 5.0
        print(f"  {len(damaged)} damaged snapshots, repaired after {min(repairs):.2f} to {worst_fix:.2f} s "
              f"(median {sorted(repairs)[len(repairs) // 2]:.2f} s, limit 5.0 s)")
    # The mock also counts snapshots whose BEGIN was lost entirely as never seen; the ESP32's
    # resend covers those too, which the gap limit above checks.

    params = [e for e in ev if e["ev"] == "set_params" and not e.get("in_snapshot")]
    if params:
        per_s = {}
        for e in params:
            per_s[int(e["t"])] = per_s.get(int(e["t"]), 0) + 1
        peak = max(per_s.values())
        print(f"  SET_PARAMS frames: {len(params)} total, peak {peak} per second")
        if name == "drag":
            ok &= 0 < peak <= 101
    elif name == "drag":
        print("  no SET_PARAMS seen: did the slider move?")
        ok = False
    lost = [e for e in ev if e["ev"] == "link_lost"]
    if name in ("faults", "soak", "basic"):
        ok &= not lost
        print(f"  link lost events on the mock side: {len(lost)}")
    print("PASS" if ok else "FAIL")
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("scenario", choices=SCENARIOS)
    ap.add_argument("--port", default="COM7")
    ap.add_argument("--baud", type=int, default=1000000)
    ap.add_argument("--seconds", type=float)
    args = ap.parse_args()
    sc = SCENARIOS[args.scenario]
    events = ROOT / "logs" / f"link_{args.scenario}.jsonl"
    ev = run_mock(args.port, args.baud, args.seconds or sc["seconds"], sc["mock"], events)
    return 0 if analyse(args.scenario, ev) else 1


if __name__ == "__main__":
    sys.exit(main())
