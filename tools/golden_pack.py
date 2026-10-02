#!/usr/bin/env python3
"""Golden reference for the ESP32's .nam packer.

Downloads (once) the three models listed in realtime-nam-seed3/models/amps.json into the
git-ignored models/download/, checks their SHA-256, then runs the Seed repo's own
scripts/convert_a2.py (imported read-only, the authority on validation and packing):
extract the A2-Lite submodel, validate, round to float32, pack into the engine's layout.

Writes models/golden/<amp>.a2l (packed little-endian float32, 7484 bytes) and prints each
model's CRC32, which the device logs when it loads the same file. The C packer's host test
(tests/host/test_nam.c) must produce these bytes exactly.

The weights are T3K-licensed: never commit anything under models/.

    python tools/golden_pack.py
"""

import binascii
import hashlib
import json
import struct
import sys
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SEED = ROOT.parent / "reference" / "realtime-nam-seed3"
DOWNLOAD = ROOT / "models" / "download"
GOLDEN = ROOT / "models" / "golden"

sys.path.insert(0, str(SEED / "scripts"))
import convert_a2  # noqa: E402  (read-only import from the reference repo)


def main():
    amps = json.loads((SEED / "models" / "amps.json").read_text())
    DOWNLOAD.mkdir(parents=True, exist_ok=True)
    GOLDEN.mkdir(parents=True, exist_ok=True)
    manifest = []
    for amp in amps:
        src = DOWNLOAD / f"{amp['amp_id']}-a2-container.nam"
        if not src.exists():
            urllib.request.urlretrieve(amp["url"], src)
        raw = src.read_bytes()
        digest = hashlib.sha256(raw).hexdigest()
        if digest != amp["download_sha256"]:
            print(f"{src.name}: SHA-256 mismatch ({digest})")
            return 1

        lite = convert_a2.extract(json.loads(raw), src)
        packed = convert_a2.pack(convert_a2.checked_weights(lite, src))
        blob = struct.pack(f"<{len(packed)}f", *packed)
        crc = binascii.crc32(blob) & 0xFFFFFFFF
        (GOLDEN / f"{amp['amp_id']}.a2l").write_bytes(blob)
        manifest.append({"amp_id": amp["amp_id"], "name": amp["name"], "source": src.name,
                         "source_sha256": digest, "weights": len(packed), "bytes": len(blob),
                         "crc32": f"{crc:08x}"})
        print(f"{amp['amp_id']:<9} {len(packed)} weights, {len(blob)} bytes, CRC32 {crc:08x}  ({amp['name']})")
    (GOLDEN / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
