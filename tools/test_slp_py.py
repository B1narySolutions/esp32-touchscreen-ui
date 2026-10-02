#!/usr/bin/env python3
"""Cross-check tools/slp.py against the C protocol library.

Builds nothing itself: run tools/run_host_tests.ps1 first (it builds build/host/slp_examples.exe).
For every example frame the C encoder printed, the Python decoder must accept it, and Python's
encoder fed the same header and payload must produce the identical wire bytes. Also runs the
Python framing through the same COBS vectors and a random round trip.

    python tools/test_slp_py.py
"""

import random
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import slp  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
EXE = ROOT / "build" / "host" / "slp_examples.exe"


def main():
    fails = 0

    def check(cond, what):
        nonlocal fails
        if not cond:
            fails += 1
            print("FAIL", what)

    # COBS vectors and CRC check values.
    check(slp.cobs_encode(b"\x00") == b"\x01\x01", "cobs 00")
    check(slp.cobs_encode(b"\x11\x22\x00\x33") == b"\x03\x11\x22\x02\x33", "cobs 11 22 00 33")
    check(slp.cobs_encode(bytes(range(1, 255))) == b"\xff" + bytes(range(1, 255)), "cobs 254 run")
    check(slp.crc16(b"123456789") == 0x29B1, "crc16 check value")
    check(slp.crc32(b"123456789") == 0xCBF43926, "crc32 check value")
    rnd = random.Random(7)
    for _ in range(2000):
        data = bytes(rnd.choice([0, rnd.randrange(256)]) for _ in range(rnd.randrange(600)))
        check(slp.cobs_decode(slp.cobs_encode(data)) == data, "cobs round trip")

    # Every C example decodes in Python, unpacks, and re-encodes identically.
    if not EXE.exists():
        print(f"missing {EXE}: run tools/run_host_tests.ps1 first")
        return 1
    out = subprocess.run([str(EXE)], capture_output=True, text=True, check=True).stdout
    examples = re.findall(r"### (.*?)\n```\npayload\s*(.*?)\nwire\s*(.*?)\n```", out)
    check(len(examples) >= 15, f"found {len(examples)} examples")
    for title, payload_hex, wire_hex in examples:
        wire = bytes.fromhex(wire_hex)
        frames = list(slp.Decoder().feed(wire))
        check(len(frames) == 1, f"decode: {title}")
        if not frames:
            continue
        f = frames[0]
        check(f.payload == bytes.fromhex(payload_hex), f"payload: {title}")
        try:
            slp.UNPACKERS[f.type](f.payload)
        except ValueError as e:
            check(False, f"unpack {title}: {e}")
        check(slp.frame_encode(f.type, f.seq, f.payload, f.flags) == wire, f"re-encode: {title}")

    # Python-built messages decode with the Python unpackers.
    hello = slp.pack_hello(slp.ROLE_MOCK_SEED, 99, "mock", 48000, 48, [(1, "Fender Twin65")])
    check(slp.unpack_hello(hello)["builtins"] == [(1, "Fender Twin65")], "hello round trip")
    check(slp.unpack_set_params(slp.pack_set_params([(0x500, 58), (0x101, -1)])) == [(0x500, 58), (0x101, -1)], "params")

    print(f"{len(examples)} C examples cross-checked, {fails} failures")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
