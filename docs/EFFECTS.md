# Effects

The touchscreen's effects run on the **Seed**, because the guitar signal only exists there; the ESP32
sends the knob settings. The code lives in this repo ([seed-dsp/effects.h](../seed-dsp/effects.h)) so it
can be written and tested here, and it goes into the Seed build as two files plus a few lines.

Why not on the ESP32: it has no audio path and no free pins for one, so the guitar would have to travel
over the 3-wire link and back (about 3 Mbaud, two round trips for pedals before the amp, several ms of
extra latency against the 10 ms requirement), and its delay memory would compete with the display for
PSRAM. The Seed has the processing headroom and 64 MB of SDRAM for delay memory; its 128 KB internal
flash was the tight part, solved with the Daisy bootloader (below).

## What's built

The sponsor's six effects (reverb, delay, distortion/overdrive, compression, chorus, noise gate), with the
touchscreen's knobs and models. `fx::Rig` runs them in the touchscreen's chain order: the ones before AMP
in the chain run before the amp model, the rest after the amp and cabinet. The touchscreen's other
effects (EQ, octave, phaser, tremolo) don't run on the Seed.

| Effect | Models | Knobs |
|---|---|---|
| GATE | | THRESHOLD -80 to -20 dBFS (6 dB hysteresis, 10 ms hold), ATTACK 0.1-20 ms, RELEASE 10 ms-1 s, RANGE 0-90 dB |
| COMP | | THRESHOLD -60 to 0 dBFS, RATIO 1:1-20:1, ATTACK 0.1-100 ms, RELEASE 20 ms-1 s, GAIN -12 to +24 dB (6 dB soft knee) |
| DRIVE | Tube Screamer, Big Muff, Klon-style, Rat Distortion | GAIN (TS 12-41 dB, Muff 30-60 dB over two stages, Klon 0-32 dB, Rat 0-66 dB), TONE dark to bright, LEVEL -30 to +10 dB |
| CHORUS | Classic Chorus, Dimension, Ensemble | RATE (0.1-5 Hz Classic), DEPTH (up to +-3 ms), MIX (equal-power: 50 = both, 100 = vibrato) |
| DELAY | Digital Delay, Tape Echo | TIME 40 ms-1 s, FEEDBACK (each repeat x0 to x0.9), TONE (repeats dark to bright), MIX (echo level) |
| REVERB | Hall, Room, Plate, Spring | DECAY (Hall 1-8 s, Room 0.25-2 s, Plate 0.5-5 s, Spring 0.8-4 s), PRE-DELAY 0-100 ms, TONE, MIX |

- **Drive models** follow each pedal's structure: Tube Screamer clips only the mids and up (720 Hz
  high-pass) and adds the clean signal back (the mid hump); Big Muff is two clipping stages and a tone
  control that blends a low-pass and a high-pass (the mid scoop); Klon-style blends more clean signal the
  lower the gain; Rat's gain bandwidth shrinks as the gain rises, as the LM308's does. The clipping runs
  at 192 kHz through an anti-aliased (ADAA) cubic clipper, so even full fuzz stays clean of off-pitch
  tones; while on, the drive adds 0.57 ms of delay.
- **Chorus:** Classic is one sine-modulated voice; Dimension two voices on opposite triangle LFOs
  (depth without an obvious wobble); Ensemble three voices 120 degrees apart plus a 6.5 Hz shimmer.
  Mono output: the "stereo" wording was taken out of the touchscreen's descriptions.
- **Delay:** Tape adds a low cut, saturation on each repeat and wow and flutter. TIME changes crossfade.
- **Reverb:** Hall, Room and Plate are a Schroeder/Freeverb comb/allpass network sized per model;
  Spring is two loops of stretched allpass filters (Valimaki/Parker/Abel), each pass a chirp.
- **Switching:** gate, comp, drive and chorus crossfade in and out over 5 ms; delay and reverb let what's
  ringing fade out ("trails"). An effect that's off and silent costs nothing.
- **Ping-Pong** was removed from the touchscreen: the output is mono.

## Tested (Mac, `bash seed-dsp/test.sh`: 134 checks, 0 failures)

- Gate: a -74 dBFS hum is turned down by RANGE (63.0 dB at the defaults); a signal above THRESHOLD is
  untouched (within 0.1 dB); a tail between the close and open levels stays open (hysteresis).
- Compressor: the measured static curve within 1 dB of the design at -45 to -10 dBFS in; ATTACK delays
  the gain reduction.
- Drive: harmonics rise with GAIN on every model; audible aliasing (below 16 kHz, a 996 Hz tone) at most
  -66 dB (Big Muff at full gain), -73 dB (Rat at full gain), better than -97 dB otherwise; LEVEL steps
  as designed (within 0.3 dB).
- Chorus: DEPTH 0 is an exact delay of the base time; the level stays within 1 dB at any MIX; RATE changes
  don't click.
- Delay: echoes land at TIME within 2 samples; repeats are FEEDBACK times the last (within 0.02).
- Reverb: decay at 500 Hz within 5 % of DECAY on every model; level within 0 to -3 dB at any DECAY and
  TONE; trails, then idle.
- Rig: an effect runs before or after the amp as the chain says; the link's ids reach the right effect.
- Every effect: MIX 0 or never on leaves the signal bit-for-bit unchanged; extreme settings with loud
  noise stay bounded.
- Heard: `bash tools/amp_sim/run.sh --demo` renders a clip of each (Mac simulator); `tools/amp_sim/run.sh`
  plays them from the touchscreen. The Fender capture compresses by itself at normal levels, so the
  compressor is clearest in front of a clean amp (VOLUME 0: loud-to-quiet range 15.3 dB -> 10.4 dB).

## On the Seed

Measured 2026-10-19 (borrowed Seed3, bare: no guitar or headphones), DSP time per 1 ms block, toggled with
the Seed's console keys:

| On | Average | Peak | Overruns |
|---|---|---|---|
| nothing (amp + cab + link) | 573 us | 584 us | 0 |
| delay | 587 us (+14) | 598 us | 0 |
| hall reverb | 609 us (+36) | 619 us | 0 |
| spring reverb | 641 us (+68) | 652 us | 0 |
| delay + spring | 655 us | 666 us (67 %) | 0 |

Gate, comp, drive and chorus aren't measured on the Seed yet. On the Mac they cost 0.2 (gate), 0.4
(comp), 6.3-7.3 (drive) and 0.7-1.9 (chorus) us a block; the Seed was 8 to 30 times slower than the Mac
for delay and reverb, so roughly 60 us for the drive and 15 us for the Ensemble chorus: every effect on
at once should stay around 75 % of the block.

- **Memory:** delay 196 KB in the SDRAM; reverb 100 KB in the DTCM (from the SDRAM the hall cost 76 us a
  block, from the DTCM 36 us); chorus 8 KB in the AXI SRAM.
- **Flash:** the effects are 9.1 KB compiled for size (no libm). With all six the build no longer fits the
  128 KB internal flash (8.5 KB over), so `seed3-link-firmware/` now builds for the **Daisy bootloader**
  by default: 199 KB of 480 KB used. See [seed3-link-firmware/README.md](../seed3-link-firmware/README.md).
- Seed console keys for bench tests: `G` gate, `K` comp, `O` Tube Screamer, `M` Big Muff, `H` Ensemble
  chorus, `D` delay, `R` hall, `S` spring.

## Next

1. Run this build on the Seed: DSP load with every effect on, then listen with the carrier and the
   touchscreen.
2. Show the effects the Seed runs on the touchscreen's "Seed controls only" page.
3. Decide what happens to EQ, octave, phaser and tremolo (not on the sponsor's list): build them or
   take them off the touchscreen.
