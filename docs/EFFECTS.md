# Effects: delay and reverb

The touchscreen's effects run on the **Seed**, because the guitar signal only exists there; the ESP32
sends the knob settings. The code lives in this repo ([seed-dsp/effects.h](../seed-dsp/effects.h)) so it
can be written and tested here, and it goes into the Seed build as two files plus a few lines.

Why not on the ESP32: it has no audio path and no free pins for one, so the guitar would have to travel
over the 3-wire link and back (about 3 Mbaud, two round trips for pedals before the amp, several ms of
extra latency against the 10 ms requirement), and its delay memory would compete with the display for
PSRAM. The Seed has the processing headroom (about 43 % of each block is free) and 64 MB of SDRAM for
delay memory; its 128 KB of program flash is the tight part (below).

## What's built (2026-10-19)

| Effect | Models | Knobs |
|---|---|---|
| DELAY | Digital Delay, Tape Echo | TIME 40 ms to 1 s, FEEDBACK (each repeat x0 to x0.9), TONE (repeats dark to bright), MIX (echo level, up to as loud as the guitar) |
| REVERB | Hall, Room, Plate, Spring | DECAY (tail time; Hall 1-8 s, Room 0.25-2 s, Plate 0.5-5 s, Spring 0.8-4 s), PRE-DELAY 0-100 ms, TONE (dark to bright tail), MIX (reverb level, up to about the guitar's) |

- **Delay:** one delay line with a low-pass in the feedback loop. Tape adds a low cut, saturation on
  each repeat and slight wow and flutter. Changing TIME crossfades to the new time (30 ms), so it never
  clicks.
- **Reverb:** Hall, Room and Plate are a comb/allpass network (the Schroeder/Freeverb design) sized
  per model. Spring is two loops of stretched allpass filters (the Valimaki/Parker/Abel spring model):
  each pass comes back as a chirp, the "drip" of a spring tank.
- **Trails:** turning an effect off stops new sound going in, and what's already ringing fades out
  naturally. Once silent it costs nothing.
- **Order:** delay and reverb run after the amp and cabinet, in the order the touchscreen's chain has
  them (delay into reverb, or reverb into delay).
- **Ping-Pong** was removed from the touchscreen (2026-10-19): it needs stereo and the carrier's output
  is mono, so it could only ever play as Digital Delay ("no fake controls"). A saved rig that had it
  loads as Digital Delay.

## Tested (Mac, `bash seed-dsp/test.sh`: 63 checks, 0 failures)

- Echoes land at TIME within 2 samples; each repeat is FEEDBACK times the last (within 0.02).
- Reverb decay time, measured at 1 kHz, within 5 % of DECAY on every model (for example Hall 3.40 s
  for 3.48 s, Spring 2.09 s for 2.10 s).
- At full MIX the reverb is within -2 to +4 dB of the guitar's level at any DECAY and TONE.
- MIX 0, or an effect never turned on, leaves the signal bit-for-bit unchanged.
- No clicks when TIME changes; trails ring on after OFF; extreme settings with loud noise stay bounded;
  switching reverb models while ringing is safe.
- Heard in the Mac simulator: `bash tools/amp_sim/run.sh --demo` renders delay and reverb clips, and
  the touchscreen's DELAY and REVERB controls change the sound in `tools/amp_sim/run.sh`.

## On the Seed

- **Cost:** 0.5 to 8 us per block on a Mac (Spring is the most expensive). The Seed is roughly 4 to 5
  times slower than a Mac for this kind of code, so about 40 us worst case, out of about 430 us free.
  Still to be measured on the Seed.
- **Memory:** 296 KB (delay 196 KB, reverb 100 KB), all in the SDRAM (`DSY_SDRAM_BSS`); the internal
  RAM the amp model uses is unchanged.
- **Flash:** 3.8 KB with the code compiled for size (`-Os`, a few microseconds slower). The build with
  the link, SD uploads, delay and reverb is **129,316 of 131,072 bytes (98.7 %)**, 1.7 KB free. No libm.
- `seed3-link-firmware/build.sh` builds it (Rocco's firmware + `docs/seed3-link.patch` + these files).
  **Not run on a Seed yet.**

## Flash for the remaining effects

Gate, compressor, drive and chorus need roughly 3 KB more than is left. Two ways to make room, both
Rocco's call ([SEED_EFFECTS_REQUEST.md](SEED_EFFECTS_REQUEST.md)):

1. **Take the built-in amp weights out of flash (about 30 KB).** The touchscreen already uploads amps
   from its SD card over the link, so it can send the selected amp when it connects. Keep one built-in
   so the Seed still makes sound on its own.
2. **Run from the Daisy bootloader** (`APP_TYPE = BOOT_SRAM`, already in libDaisy's Makefile): the
   program is stored in the Seed's 8 MB QSPI flash and copied into RAM at power-up, so code space goes
   from 128 KB to about 480 KB. Reversible over DFU. Needs a timing check (code placement moves).

## Next

1. Run this build on the Seed with the touchscreen: listen, and read the DSP load and overruns.
2. Show DELAY and REVERB on the touchscreen's "Seed controls only" page once they run on the Seed.
3. Gate, compressor, drive and chorus, the same way, once there's flash for them.
