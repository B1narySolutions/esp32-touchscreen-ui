# seed-dsp

Audio code meant for the Seed, kept in this repo until it's merged into
[realtime-nam-seed3](https://github.com/B1narySolutions/realtime-nam-seed3). Plain C++ (no allocation in the
audio path, no libDaisy), so the Mac simulator ([tools/amp_sim](../tools/amp_sim/README.md)) runs the same code.

| File | What |
|---|---|
| `amp_controls.h/.cpp` | Each amp's real knobs around its NAM capture (drive, tone stack, Bright / Cut / Presence / Master). See [docs/AMP_CONTROLS.md](../docs/AMP_CONTROLS.md). |
| `effects.h/.cpp` | The six effects for the touchscreen's knobs: GATE, COMP, DRIVE (Tube Screamer, Big Muff, Klon-style, Rat), CHORUS (Classic, Dimension, Ensemble), DELAY (Digital, Tape), REVERB (Hall, Room, Plate, Spring), and `Rig`, which runs them in the touchscreen's chain order. See [docs/EFFECTS.md](../docs/EFFECTS.md). |
| `test_amp_controls.cpp`, `test_effects.cpp`, `test.sh` | Host tests: `bash seed-dsp/test.sh` (22 + 134 checks). |
