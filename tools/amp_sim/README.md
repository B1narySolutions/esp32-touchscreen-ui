# amp_sim: the Seed's amp on the Mac, controlled by the touchscreen

Runs Rocco's [realtime-nam-seed3](https://github.com/B1narySolutions/realtime-nam-seed3) audio code (the
three NAM amp captures, cabinet EQ, volume/tone code) plus [seed-dsp/amp_controls](../../seed-dsp/amp_controls.h)
on a simulated guitar, plays it on the Mac's speakers, and takes its orders from the real touchscreen over
the link, exactly as the Seed build with the link does (`slp_receiver.c`, same master/mute ramp and model
switching). Every change the touchscreen sends is printed with whether it changes the sound, how (in dB),
and how long it took to reach the audio. Rocco's repo is cloned at the commit the Seed build uses into
`build/amp-sim/` and never changed. macOS only (AudioToolbox).

## Run it

1. Touchscreen **SW1 on UART1**, its **"USB TO UART"** port plugged into the Mac.
2. `bash tools/amp_sim/run.sh` (builds on first run; finds the port by USB id). Start with the Mac's volume
   low, and keep the touchscreen's MASTER around 70: at 11 % the output is about -54 dBFS, close to silent.
3. On the touchscreen: Settings, "Seed controls only" off; AMP panel; pick an amp and turn its knobs.
4. Ctrl-C (or tap MUTE on the touchscreen to silence it). The session is saved to
   `build/amp-sim/amp_sim_output.wav`.

Options: `--input guitar.wav` (48 kHz mono or stereo, 16/24-bit or float) instead of the simulated guitar,
`--seconds N`, `--no-speakers`.

The touchscreen labels the simulator **MOCK**: the sound is the Seed's code, but the DSP load it reports is
the Mac's. The simulator refuses SD profile uploads, as the Seed build with the link does today.

## Hear every knob without the touchscreen

```
bash tools/amp_sim/run.sh --demo
afplay build/amp-sim/amp_demo.wav
```

Renders 3-second before/after clips of every amp control (same strum each time) into one 68-second file
and prints a table of the measured level in three bands per clip.

## What it prints

```
  24.509  UI  TREBLE 60 -> 50  -> changes the sound: +0.1 dB at 100 Hz, -1.0 at 500 Hz, -1.4 at 2.5 kHz, -1.4 at 5 kHz
  26.799  UI  AMP -> Marshall JCM800 G5  (its knobs: PREAMP BASS MIDDLE TREBLE PRESENCE)  -> changes the sound; in the audio 4.2 ms after arriving (includes loading the model)
  70.416  UI  MASTER 37 -> 30 (gain 0.09, -20.9 dB)  -> changes the sound; in the audio 3.9 ms after arriving
          UI  DELAY on, model 0  -> no effect on the sound (the Seed has no DELAY yet)
   75.000  [Fender Twin65 VOLUME 25 BASS 55 MIDDLE 50 TREBLE 60 BRIGHT 60 | master 63 | cab on]  guitar -27.2 dBFS rms -> out -26.4 dBFS rms   link up
```

The "in the audio after" times include the Mac's 10 ms audio buffers; on the Seed a change reaches the audio
within its 1 ms block.
