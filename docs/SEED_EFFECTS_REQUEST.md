# For Rocco: the effects on the Seed, and the Daisy bootloader

Short version: the touchscreen's six effects (gate, comp, drive, chorus, delay, reverb: the sponsor's list)
now have real DSP behind them, written to run on the Seed. It's two files and a few lines in `main.cpp`.
All six don't fit the 128 KB internal flash (8.5 KB over), but they fit easily under the Daisy bootloader,
which I tested on a Seed. Switching the team firmware to it is your decision.

Everything below is done in `docs/seed3-link.patch` (on top of your `1636ef6`) and builds with
`seed3-link-firmware/build.sh`. Delay and reverb ran on a Seed on 2026-10-19 (0 overruns, worst case 67 %
DSP); the build with all six hasn't run on a Seed yet. Nothing has been listened to on the Seed yet.

## 1. The code

- Copy `seed-dsp/effects.h` and `effects.cpp` into `src/fx/`. Plain C++: no allocation, no libDaisy, no
  libm (same reason as your cab filter's constexpr coefficients). 9.1 KB compiled for size.
- `make/firmware.mk`: add `src/fx/effects.cpp` to `CPP_SOURCES`, and `$(BUILD_DIR)/effects.o: CPPFLAGS += -Os`.
- `main.cpp`:
  - Memory: `static float DSY_SDRAM_BSS delay_mem[fx::Delay::kMemFloats];` (196 KB),
    `static float DTCM_MEM_SECTION reverb_mem[fx::Reverb::kMemFloats];` (100 KB; from the SDRAM the hall
    reverb cost 76 us a block, from the DTCM 36 us; the stack keeps 28 KB of the DTCM),
    `static float chorus_mem[fx::Chorus::kMemFloats];` (8 KB) and `static fx::Rig rig;` (all-zero
    members, so it costs no flash). After `seed.Init()`: `rig.Init(delay_mem, reverb_mem, chorus_mem);`
  - `AudioCallback`: copy the input block, `rig.ProcessPre(pre, n)` (gate, comp, drive: before the amp),
    `audio.Process(pre, ...)`, then `rig.ProcessPost(output_channels[0], n)` (chorus, delay, reverb) and
    copy the left channel to the right. `Rig` follows the touchscreen's chain order.
  - The link's hooks: `on_fx` -> `rig.SetFx`, `on_param` -> `rig.SetParam`, `on_chain` -> `rig.SetChain`.
    They only store values; the audio callback picks them up at the next block.
  - Console keys for bench tests without the touchscreen: `G` gate, `K` comp, `O` Tube Screamer, `M` Big
    Muff, `H` Ensemble chorus, `D` delay, `R` hall, `S` spring.

Measured on the Seed so far: +14 us a block for the delay, +36 for the hall reverb, +68 for the spring.
Estimated for the rest (from the Mac): about 60 us for the drive (it clips at 192 kHz against aliasing),
15 for the chorus, a few for gate and comp.

## 2. The Daisy bootloader (tested on a Seed, 2026-10-19)

`APP_TYPE = BOOT_SRAM`: the Daisy bootloader sits in the internal flash; the app lives in the 8 MB QSPI and
is copied into the 512 KB AXI SRAM at power-up, so code and data together can be about 480 KB (the build
with all six effects uses 199 KB).

- libDaisy's own `STM32H750IB_sram.lds` moves `.data`/`.bss` to the DTCM and the heap to the D2 RAM; then
  the A2-Lite buffers (about 180 KB of heap) and the reverb don't fit. `seed3-link-firmware/
  STM32H750IB_sram_app.lds` keeps `.data`, `.bss` and the heap in the AXI SRAM after the code, exactly as
  in your flash build, so nothing about the model's memory changes.
- Measured: 570 us a block (573 from the internal flash), all four amps load, effects cost the same, 0
  overruns. The app starts 2 s after power-up (the bootloader's DFU window).
- Install once with `make program-boot` (STM32 DFU); apps then go to `0x90040000` through the bootloader.
  `EnterBootloaderIfRequested` uses `ResetToBootloader(DAISY_INFINITE_TIMEOUT)` under `BOOT_APP`, so `B`
  still works for updates; `U` goes to the STM32 DFU. Undo: flash any internal-flash build over it.
- One thing to know: in this libDaisy, 64-bit atomics need libatomic, which the build doesn't link;
  `Rig` uses 32-bit ones.

The other way to make room, taking the built-in amp weights out of flash (about 30 KB, the touchscreen
uploading the selected amp at connect), would just about fit these six but leaves no room after them.

## Who does what

The effect code is written and tested in the touchscreen repo (`seed-dsp/`: 134 host checks, plus the Mac
simulator driven by the touchscreen) and stays there until you merge it. Nothing in your repo has been
changed.
