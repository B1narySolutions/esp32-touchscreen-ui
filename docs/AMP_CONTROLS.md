# Amp controls

Each amp's real knobs around its NAM capture, so the touchscreen's AMP knobs change the sound.
Code: [seed-dsp/amp_controls.h](../seed-dsp/amp_controls.h) (C++, no allocation, no libDaisy), shared by the
Mac simulator ([tools/amp_sim](../tools/amp_sim/README.md)) and, next, the Seed firmware.

## The knobs

A NAM capture is the amp at one fixed setting, so the knobs move the sound relative to the capture:
**every knob at 50 (Fender Bright off) is exactly the capture.** The touchscreen's five AMP knob slots
carry each amp's own controls; their labels change with the selected amp.

| Slot (wire id) | Fender Twin '65 | Vox AC30 Top Boost | Marshall JCM800 2203 | SD profile (amp unknown) |
|---|---|---|---|---|
| 1 `amp.gain` | VOLUME | VOLUME | PREAMP | GAIN |
| 2 `amp.bass` | BASS | BASS | BASS | BASS |
| 3 `amp.mid` | MIDDLE | CUT | MIDDLE | MIDDLE |
| 4 `amp.treble` | TREBLE | TREBLE | TREBLE | TREBLE |
| 5 `amp.level` | BRIGHT (switch) | MASTER | PRESENCE | LEVEL |

No new wire ids: the five existing amp parameters carry them (only their descriptions changed in
`tools/param_ids.json`). Reverb and tremolo, which the real Twin and AC30 also have, are separate effects in
the UI.

## How each one works

| Control | Model | Range |
|---|---|---|
| VOLUME / PREAMP / GAIN | Gain into the capture: driving it harder gives more breakup, as on the amp | -20 dB (0) .. capture (50) .. +14 dB (100) |
| BASS / MIDDLE / TREBLE | The amp's tone-stack circuit (Yeh & Smith's analytic Fender/Marshall/Vox stack with each amp's component values, 3rd order, bilinear), times a fixed 5-band correction that divides out the capture's own stack at noon | as the circuit; the bass pot is audio taper |
| Fender BRIGHT | The bright cap on the volume pot: a treble shelf (1.2 kHz) before the capture, strongest at low volume | up to +9 dB at volume 0, none at volume 100 |
| Vox CUT | The power amp's top cut: a first-order low-pass sweeping 20 kHz (0) to 1 kHz (100), relative to noon | |
| Marshall PRESENCE | The power amp's presence: a high shelf at 3.5 kHz | -6 dB (0) .. +6 dB (100) |
| Vox MASTER, SD LEVEL | Output level | -12 dB .. +12 dB |

The tone controls sit after the capture (on a real amp the stack is inside the preamp), so they're a close
imitation of each amp's knobs, not a recapture at other settings.

## Tested (2026-10-14, Mac)

`bash seed-dsp/test.sh` (clang, sanitizers): 22 checks, 0 failures.

- All knobs at noon: within 1.8 dB of the capture (Fender), 1.4 dB (Vox), 1.1 dB (Marshall), 50 Hz to 15 kHz.
  Since the stack itself is exact, this is also the error against the circuit at any knob setting.
- Every knob moves the sound the right way; the real-time path matches the designed response to 0.000 dB.
- Extreme settings with loud noise stay finite.
- Cost on the Mac: 1.3 us per 48-sample block (Seed: still to measure; estimated a few percent of the budget).

Measured on rendered audio (simulated guitar through the capture, `tools/amp_sim/run.sh --demo`):
Fender TREBLE 0 to 100 +21 dB in the 2-8 kHz band; Marshall PREAMP 0 to 100 +11 dB overall; Vox CUT 100 to 0
+10.7 dB in 2-8 kHz; Marshall PRESENCE 0 to 100 +5.2 dB in 2-8 kHz; Fender MIDDLE and BASS about +3 dB and
+1 dB in their bands (the real Fender stack is weak there at these settings; laptop speakers hide bass).

## Not done yet

- **On the Seed:** add `seed-dsp/amp_controls.cpp` to the Seed build, call `ProcessPre` before the model and
  `ProcessPost` between the model and the cabinet EQ (inside `NamAudio::Process`), and `Configure` from the
  link's `on_param` / `on_select_model` hooks. Then measure its cost on the Seed. Flash is the tight part:
  the Seed build with the link is at 89.4 % of 128 KB.
- **Seed controls only** page: add the amp knobs once the Seed build runs them.
- TONE3000 capture settings: if a capture's page lists the knob positions it was recorded at, those could
  become the "50" of each knob.
