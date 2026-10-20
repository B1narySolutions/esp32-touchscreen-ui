# Seed3 firmware with the touchscreen link

Jacob's build of the Seed firmware: Rocco's [realtime-nam-seed3](https://github.com/B1narySolutions/realtime-nam-seed3)
at `1636ef6` (four amps, volume and tone knobs), plus the touchscreen link, SD amp profile uploads and the six effects
([../docs/seed3-link.patch](../docs/seed3-link.patch), the receiver in [../firmware/main/seed_link_proto/](../firmware/main/seed_link_proto/)
and [../seed-dsp/effects.h](../seed-dsp/effects.h)). It runs under the Daisy bootloader (below). Rocco's repo is only
cloned and never changed; this folder keeps a separate version that can be flashed whenever, until it's merged there.

**No binary is committed:** the firmware has the T3K-licensed amp models built in, so `build.sh` rebuilds it on your
computer instead. It ends up in `build/seed3-link/` (git-ignored).

## Build

```
bash seed3-link-firmware/build.sh
```

Needs git, make, curl, python3. It finds `arm-none-eabi-g++` on the PATH (or `ARM_GNU_BIN=/path/to/bin`); if there
is none, it downloads Arm's GNU toolchain 15.3.rel2 into `build/seed3-link/toolchain/` and checks it against Arm's
published checksum (on 2026-10-13 the Homebrew `gcc-arm-embedded` cask had an outdated checksum and wouldn't install).
It then clones Rocco's repo at the pinned commit, libDaisy at the commit his `install.sh` pins, downloads the three
amp models with his `scripts/download_models.sh`, applies the patch and builds. At the end it says whether the result
is byte-identical to the binary last tested on a Seed.

## Flash (and undo)

```
bash seed3-link-firmware/flash.sh
bash seed3-link-firmware/flash.sh --restore build/seed3-link/backups/seed-flash-YYYYMMDD-HHMMSS.bin
```

Needs `dfu-util`. `flash.sh` asks the Seed to enter its bootloader (the `B` command its firmware understands), or
tells you to hold BOOT, tap RESET and release BOOT. **It always reads a backup of the Seed's flash first** into
`build/seed3-link/backups/`, so the last line it prints is the exact command to put the old firmware back. Use that
before handing the Seed back to the team.

## The Daisy bootloader: room for the effects (tested 2026-10-19)

With all six effects the app no longer fits the Seed's 128 KB internal flash (8.5 KB over), so both scripts now
default to the Daisy bootloader: the internal flash holds libDaisy's Daisy bootloader, and the app lives in the
Seed's 8 MB QSPI flash; the bootloader copies it into the 512 KB AXI SRAM at power-up, so the app can be about
**480 KB** (it uses 199 KB). `--internal` builds and flashes the old way, and says when it doesn't fit.
[STM32H750IB_sram_app.lds](STM32H750IB_sram_app.lds) keeps the app's data, bss and heap in the AXI SRAM as in the
normal build (libDaisy's own BOOT_SRAM script moves them to the DTCM and D2 RAM, where the amp model and reverb
don't both fit).

- The first `flash.sh` on a Seed backs up the internal flash, installs the bootloader, backs up the QSPI region
  the app goes into, and flashes the app. Later runs only update the app (`B` on the console reaches the
  bootloader, which then waits).
- Measured on the borrowed Seed: the amp runs at 570 us a block (573 without the bootloader), the effects cost the
  same, all four amps load, 0 overruns. The app starts 2 s after power-up (the bootloader's DFU window).
- Undo: `flash.sh --restore <internal-flash backup>` (the firmware sends the Seed to the STM32's DFU with `U`). That
  removes the bootloader; the old firmware runs from the internal flash again.

## What it does on the Seed (tested 2026-10-13)

Bare Seed wired to the touchscreen's H3 "UART2" header (D13, D14, DGND; see
[../docs/LAB_DAY_CHECKLIST.md](../docs/LAB_DAY_CHECKLIST.md)), SW1 on UART2:
link up within about 1 s, amp cards switch the Seed's amp, mute silences its output, DSP load and meters are real
(57 % DSP, 0 overruns), ping 1.95 ms, recovers from a Seed reset and a pulled cable. Full results:
[../docs/SEED_LINK_STATUS.md](../docs/SEED_LINK_STATUS.md). SD amp profiles (TONE3000 A2 files on the touchscreen's
card) upload and run on the Seed since 2026-10-15; see [../docs/PDR_DEMO.md](../docs/PDR_DEMO.md).

**The effects** ([../docs/EFFECTS.md](../docs/EFFECTS.md)): delay and reverb ran on the borrowed Seed on 2026-10-19
(0 overruns, worst case 67 % DSP); the build with all six (gate, comp, drive, chorus added 2026-10-20) hasn't run on a
Seed yet. USB console keys toggle them for bench tests without the touchscreen: `G` gate, `K` comp, `O` Tube Screamer,
`M` Big Muff, `H` chorus, `D` delay, `R` hall, `S` spring. Not listened to yet (no audio hardware on that Seed).

## Keeping it current

When Rocco changes `src/main.cpp` or `make/firmware.mk`, the patch may stop applying: `build.sh` stops at
`git apply`. Update the patch against his new commit and change `seed_commit` in `build.sh`. When he merges the
link into his repo, this folder can go.
