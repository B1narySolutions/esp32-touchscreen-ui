# For Rocco: delay and reverb on the Seed, and room for the other effects

Short version: the touchscreen's DELAY and REVERB now have real DSP behind them, written to run on the
Seed. It's two files, a few lines in `main.cpp`, and it fits in the current flash. The other four effects
(gate, comp, drive, chorus) need about 3 KB more flash than is left, which needs a decision from you.

## 1. Adding delay and reverb (fits today)

Everything below is already done and builds in `docs/seed3-link.patch` (on top of your `1636ef6`);
`seed3-link-firmware/build.sh` builds it. Not run on a Seed yet.

- Copy `seed-dsp/effects.h` and `effects.cpp` into `src/fx/`. Plain C++: no allocation, no libDaisy, no
  libm (same reason as your cab filter's constexpr coefficients).
- `make/firmware.mk`: add `src/fx/effects.cpp` to `CPP_SOURCES`, and compile it for size:
  `$(BUILD_DIR)/effects.o: CPPFLAGS += -Os` (3.6 KB instead of 8 KB at -O3).
- `main.cpp`:
  - Memory in the SDRAM, so your internal RAM is untouched:
    `static float DSY_SDRAM_BSS delay_mem[fx::Delay::kMemFloats];` (196 KB) and the same for the
    reverb (100 KB); `static fx::Delay delay; static fx::Reverb reverb;`
  - After `seed.Init()`: `delay.Init(delay_mem); reverb.Init(reverb_mem);`
  - In `AudioCallback`, right after `audio.Process(...)`:
    `delay.Process(output_channels[0], frame_count); reverb.Process(output_channels[0], frame_count);`
    then copy the left channel to the right.
  - The link's `on_fx`, `on_param` and `on_chain` hooks call `delay.Set(on, model, knobs)` /
    `reverb.Set(...)`. `Set` only stores the values; the audio callback picks them up at the next block.

**Result:** flash 129,316 of 131,072 B (98.7 %), SRAM unchanged, SDRAM 296 KB. Measured on a Mac at 0.5
to 8 us per block (spring reverb is the most expensive); please check the DSP load on the Seed's status
line with both on.

## 2. Flash for gate, comp, drive and chorus (your decision)

About 1.7 KB is left; those four need roughly 3 KB. Options:

1. **Built-in amp weights out of flash (frees about 30 KB).** The touchscreen already uploads amps from
   its SD card over the link (that path is tested bit-identical to the built-ins). It could send the
   selected amp at connect, and the Seed keeps one built-in so it still plays on its own.
2. **Daisy bootloader, `APP_TYPE = BOOT_SRAM`.** The app lives in the 8 MB QSPI and is copied into the
   512 KB AXI SRAM at boot: about 480 KB for code and data together (today about 130 KB of code plus
   55 KB of SRAM data, so it fits with room to spare). One-time `make program-boot`, reversible over DFU.
   Timing needs re-measuring, as your notes say code placement moves it by a few microseconds.

I'd suggest option 1 first (no change to how the Seed boots), and option 2 when more effects come.

## Who does what

The effect code is written and tested in the touchscreen repo (`seed-dsp/`, 63 host checks, plus the
Mac simulator driven by the touchscreen) and stays there until you merge it. Nothing in your repo has
been changed.
