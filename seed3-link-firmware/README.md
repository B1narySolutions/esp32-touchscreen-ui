# Seed3 firmware with the touchscreen link

Jacob's build of the Seed firmware: Rocco's [realtime-nam-seed3](https://github.com/B1narySolutions/realtime-nam-seed3)
at `c3abe82` (with his volume and tone knobs), plus the touchscreen link ([../docs/seed3-link.patch](../docs/seed3-link.patch)
and the receiver in [../firmware/main/seed_link_proto/](../firmware/main/seed_link_proto/)). Rocco's repo is only cloned
and never changed; this folder keeps a separate version that can be flashed whenever, until the link is merged there.

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
is byte-identical to the binary tested on the Seed (SHA-256 `4fe3f8fc...`).

## Flash (and undo)

```
bash seed3-link-firmware/flash.sh
bash seed3-link-firmware/flash.sh --restore build/seed3-link/backups/seed-flash-YYYYMMDD-HHMMSS.bin
```

Needs `dfu-util`. `flash.sh` asks the Seed to enter its bootloader (the `B` command its firmware understands), or
tells you to hold BOOT, tap RESET and release BOOT. **It always reads a backup of the Seed's flash first** into
`build/seed3-link/backups/`, so the last line it prints is the exact command to put the old firmware back. Use that
before handing the Seed back to the team.

## What it does on the Seed (tested 2026-10-13)

Bare Seed wired to the touchscreen's H3 "UART2" header (D13, D14, DGND; see
[../docs/LAB_DAY_CHECKLIST.md](../docs/LAB_DAY_CHECKLIST.md)), SW1 on UART2:
link up within about 1 s, amp cards switch the Seed's amp, mute silences its output, DSP load and meters are real
(57 % DSP, 0 overruns), ping 1.95 ms, recovers from a Seed reset and a pulled cable. Full results:
[../docs/SEED_LINK_STATUS.md](../docs/SEED_LINK_STATUS.md). Not done yet on the Seed side: SD profile uploads.

## Keeping it current

When Rocco changes `src/main.cpp` or `make/firmware.mk`, the patch may stop applying: `build.sh` stops at
`git apply`. Update the patch against his new commit and change `seed_commit` in `build.sh`. When he merges the
link into his repo, this folder can go.
