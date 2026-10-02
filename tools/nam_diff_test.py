#!/usr/bin/env python3
"""Differential test: the firmware's .nam loader (C) versus convert_a2.py (Python reference).

Mutates a real TONE3000 A2 container in ways that probe Python's semantics (True == 1,
48000 == 48000.0, duplicate keys, NaN, float32 overflow, rounding ties, metadata types, ...),
runs every case through both, and requires the same accept/reject decision and, on accept, the
same packed bytes (CRC32). Needs build/host/nam_check.exe (tools/run_host_tests.ps1) and the
downloads from tools/golden_pack.py.

    python tools/nam_diff_test.py
"""

import binascii
import copy
import json
import random
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT.parent / "reference" / "realtime-nam-seed3" / "scripts"))
import convert_a2  # noqa: E402

NAM_CHECK = ROOT / "build" / "host" / "nam_check.exe"
SOURCE = ROOT / "models" / "download" / "Fender-a2-container.nam"


def python_reference(raw: bytes):
    """What the ESP32 pipeline does, in the reference code: extract() for a container, else
    checked_weights(), then pack(). Any exception is a reject."""
    try:
        doc = json.loads(raw)
        if isinstance(doc, dict) and doc.get("architecture") == "SlimmableContainer":
            model = convert_a2.extract(doc, "f")
        else:
            model = doc
        packed = convert_a2.pack(convert_a2.checked_weights(model, "f"))
        return "OK", binascii.crc32(struct.pack(f"<{len(packed)}f", *packed)) & 0xFFFFFFFF
    except Exception as e:  # noqa: BLE001  (Python rejects by raising, whatever the type)
        return "ERR", type(e).__name__


def c_loader(raw: bytes):
    with tempfile.NamedTemporaryFile(suffix=".nam", delete=False) as f:
        f.write(raw)
        path = f.name
    try:
        out = subprocess.run([str(NAM_CHECK), path], capture_output=True, text=True, check=True).stdout.strip()
    finally:
        Path(path).unlink()
    parts = out.split(" ", 2)
    return ("OK", int(parts[1], 16)) if parts[0] == "OK" else ("ERR", out)


def main():
    if not NAM_CHECK.exists() or not SOURCE.exists():
        print("needs build/host/nam_check.exe (tools/run_host_tests.ps1) and tools/golden_pack.py downloads")
        return 1
    base_raw = SOURCE.read_bytes()
    base = json.loads(base_raw)
    lite_index = next(i for i, s in enumerate(base["config"]["submodels"]) if s["max_value"] == 0.5)

    def lite(doc):
        return doc["config"]["submodels"][lite_index]["model"]

    cases = []  # (name, bytes)

    def obj_case(name, fn):
        d = copy.deepcopy(base)
        fn(d)
        cases.append((name, json.dumps(d, allow_nan=True).encode()))

    def text_case(name, fn):
        cases.append((name, fn(base_raw.decode())))

    cases.append(("original container", base_raw))
    bare = copy.deepcopy(lite(base))
    bare.setdefault("sample_rate", base["sample_rate"])
    cases.append(("bare Lite WaveNet", json.dumps(bare).encode()))
    obj_case("sample_rate int 48000", lambda d: lite(d).__setitem__("sample_rate", 48000))
    obj_case("sample_rate 44100", lambda d: lite(d).__setitem__("sample_rate", 44100))
    obj_case("sample_rate true", lambda d: lite(d).__setitem__("sample_rate", True))
    obj_case("lite sample_rate removed (inherits)", lambda d: lite(d).pop("sample_rate", None))
    obj_case("lite sample_rate null (inherits)", lambda d: lite(d).__setitem__("sample_rate", None))
    obj_case("both sample_rates removed", lambda d: (lite(d).pop("sample_rate", None), d.pop("sample_rate")))
    obj_case("weight as int 0", lambda d: lite(d)["weights"].__setitem__(5, 0))
    obj_case("weight true", lambda d: lite(d)["weights"].__setitem__(5, True))
    obj_case("weight string", lambda d: lite(d)["weights"].__setitem__(5, "1.0"))
    obj_case("weight NaN", lambda d: lite(d)["weights"].__setitem__(5, float("nan")))
    obj_case("weight Infinity", lambda d: lite(d)["weights"].__setitem__(5, float("inf")))
    obj_case("weight 1e39 (float32 overflow)", lambda d: lite(d)["weights"].__setitem__(5, 1e39))
    obj_case("weight 1e-50 (underflow to 0)", lambda d: lite(d)["weights"].__setitem__(5, 1e-50))
    obj_case("weight -0.0", lambda d: lite(d)["weights"].__setitem__(5, -0.0))
    obj_case("weight rounding tie 1+2^-24", lambda d: lite(d)["weights"].__setitem__(5, 1.0000000596046448))
    obj_case("1870 weights", lambda d: lite(d)["weights"].pop())
    obj_case("head_scale changed", lambda d: lite(d)["config"].__setitem__("head_scale", lite(d)["config"]["head_scale"] + 1))
    obj_case("head_scale true", lambda d: lite(d)["config"].__setitem__("head_scale", True))
    obj_case("config extra key", lambda d: lite(d)["config"].__setitem__("extra", 1))
    obj_case("config keys reordered", lambda d: lite(d).__setitem__("config", dict(reversed(list(lite(d)["config"].items())))))
    obj_case("layer keys reordered", lambda d: lite(d)["config"]["layers"].__setitem__(0, dict(reversed(list(lite(d)["config"]["layers"][0].items())))))
    obj_case("bias 1 instead of true", lambda d: lite(d)["config"]["layers"][0]["head"].__setitem__("bias", 1))
    obj_case("bias 1.0 instead of true", lambda d: lite(d)["config"]["layers"][0]["head"].__setitem__("bias", 1.0))
    obj_case("bias \"true\"", lambda d: lite(d)["config"]["layers"][0]["head"].__setitem__("bias", "true"))
    obj_case("negative_slope NaN", lambda d: lite(d)["config"]["layers"][0]["activation"][0].__setitem__("negative_slope", float("nan")))
    obj_case("version 0.7.0 with space", lambda d: lite(d).__setitem__("version", "0.7.0 "))
    obj_case("two Lite submodels", lambda d: d["config"]["submodels"].append(copy.deepcopy(d["config"]["submodels"][lite_index])))
    obj_case("container metadata [1]", lambda d: d.__setitem__("metadata", [1]))
    obj_case("container metadata []", lambda d: d.__setitem__("metadata", []))
    obj_case("container metadata \"x\"", lambda d: d.__setitem__("metadata", "x"))
    obj_case("lite metadata 0", lambda d: lite(d).__setitem__("metadata", 0))
    obj_case("head null -> false", lambda d: lite(d)["config"].__setitem__("head", False))
    text_case("max_value written 0.50", lambda t: t.replace('"max_value":0.5', '"max_value":0.50', 1).encode())
    text_case("negative_slope written 1e-2", lambda t: t.replace('"negative_slope":0.01', '"negative_slope":1e-2', 1).encode())
    text_case("escaped key \\u0076ersion", lambda t: t.replace('"model":{"version"', '"model":{"\\u0076ersion"', 1).encode())
    text_case("duplicate head key, last null wins", lambda t: t.replace('"head":null', '"head":5,"head":null', 1).encode())
    text_case("duplicate head key, last 5 wins", lambda t: t.replace('"head":null', '"head":null,"head":5', 1).encode())
    text_case("UTF-8 BOM", lambda t: b"\xef\xbb\xbf" + t.encode())
    text_case("trailing garbage", lambda t: (t + " x").encode())
    text_case("truncated", lambda t: t[: len(t) // 2].encode())
    # Random re-spellings of the Lite weights (exponents, trailing zeros): same floats.
    rnd = random.Random(1)
    for k in range(5):
        d = copy.deepcopy(base)
        w = lite(d)["weights"]
        idx = rnd.sample(range(len(w) - 1), 50)
        text = json.dumps(d)
        for i in idx:
            w[i] = round(w[i], rnd.randint(3, 12))  # changed values, still valid
        cases.append((f"random weight values #{k}", json.dumps(d).encode()))
        del text

    fails = 0
    for name, raw in cases:
        py, c = python_reference(raw), c_loader(raw)
        same = py[0] == c[0] and (py[0] == "ERR" or py[1] == c[1])
        fails += not same
        detail = f"crc {py[1]:08x}" if py[0] == "OK" else f"py {py[1]} / c {c[1]}"
        print(f"{'ok  ' if same else 'DIFF'} {py[0]:<3} {name:<40} {detail if same else f'py={py} c={c}'}")
    print(f"{len(cases)} cases, {fails} differences")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
