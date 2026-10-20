#!/bin/bash
# Builds Jacob's Seed3 firmware: Rocco's realtime-nam-seed3 at a pinned commit plus the touchscreen
# link (docs/seed3-link.patch, the receiver in firmware/main/seed_link_proto/) and the delay and
# reverb (seed-dsp/effects). Rocco's repo is only cloned, never changed. Everything lands in
# build/seed3-link/ (git-ignored): the firmware contains T3K-licensed model weights, so the binary
# is never committed.
#
#   bash seed3-link-firmware/build.sh                  runs under the Daisy bootloader (about 480 KB)
#   bash seed3-link-firmware/build.sh --internal       runs from the Seed's internal flash (128 KB):
#                                                      too small since all six effects (2026-10-20)
#   ARM_GNU_BIN=/path/to/arm-gnu-toolchain/bin bash seed3-link-firmware/build.sh
#
# Output: build/seed3-link/seed3-link-boot.bin (or seed3-link.bin), then flash it with
# seed3-link-firmware/flash.sh (with the same --internal, if used).
#
# The default, APP_TYPE = BOOT_SRAM. The Daisy bootloader sits in the internal flash and copies the
# app from the QSPI flash into the AXI SRAM at power-up, so the app can be about 480 KB instead of
# 128 KB. STM32H750IB_sram_app.lds keeps the data, bss and heap where the internal-flash build has
# them (AXI SRAM), so the amp model runs at the same speed (570 us a block measured, 2026-10-19).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
work="$root/build/seed3-link"
boot=1
[[ "${1:-}" == "--internal" ]] && boot=0

# Rocco's commit the patch is made for, and the toolchain release it was tested with.
seed_commit=1636ef6fc16e1d956022485711790b667f108dc3
arm_release=15.3.rel2
# SHA-256 of the binaries this script produces for that commit, as run on a real Seed (see
# README.md). Empty: not run yet. The bootloader build with all six effects (2026-10-20) hasn't been;
# the last one run on a Seed was 7ac13d5d... (delay and reverb only, 2026-10-19: DSP load measured,
# all amps load). The internal-flash build no longer fits.
tested_sha256=
tested_boot_sha256=

mkdir -p "$work"

# 1. ARM compiler: ARM_GNU_BIN, the PATH, or Arm's own macOS release checked against Arm's checksum.
if [[ -n "${ARM_GNU_BIN:-}" ]]; then
    PATH="$ARM_GNU_BIN:$PATH"
elif ! command -v arm-none-eabi-g++ >/dev/null; then
    case "$(uname -s)-$(uname -m)" in
        Darwin-arm64) host=darwin-arm64 ;;
        Darwin-x86_64) host=darwin-x86_64 ;;
        Linux-x86_64) host=x86_64 ;;
        *) echo "Install the Arm GNU toolchain ($arm_release) and put arm-none-eabi-g++ on PATH." >&2; exit 1 ;;
    esac
    name="arm-gnu-toolchain-$arm_release-$host-arm-none-eabi"
    if [[ ! -x "$work/toolchain/$name/bin/arm-none-eabi-g++" ]]; then
        url="https://gitlab.arm.com/api/v4/projects/tooling%2Fgnu-toolchains-for-arm/packages/generic/gnu-toolchain/$arm_release"
        mkdir -p "$work/toolchain"
        echo "Downloading the Arm GNU toolchain $arm_release (about 150 MB)..."
        curl -fL --retry 2 -o "$work/toolchain/$name.tar.xz" "$url/$name.tar.xz"
        curl -fL -o "$work/toolchain/$name.tar.xz.sha256asc" "$url/$name.tar.xz.sha256asc"
        (cd "$work/toolchain" && shasum -a 256 -c "$name.tar.xz.sha256asc")
        tar -xf "$work/toolchain/$name.tar.xz" -C "$work/toolchain"
    fi
    PATH="$work/toolchain/$name/bin:$PATH"
fi
arm-none-eabi-g++ --version | head -1

# 2. Rocco's firmware at the pinned commit, fresh each build so nothing stale carries over.
if [[ ! -d "$work/realtime-nam-seed3/.git" ]]; then
    git clone -q https://github.com/B1narySolutions/realtime-nam-seed3.git "$work/realtime-nam-seed3"
fi
seed="$work/realtime-nam-seed3"
# This clone is build.sh's own scratch copy (build/ is git-ignored): drop the last build's patch
# before switching commits.
git -C "$seed" fetch -q origin
git -C "$seed" reset -q --hard
git -C "$seed" clean -q -fdx -e libs/ -e models/local/ -e build/ -e build-boot/
git -C "$seed" checkout -q --detach "$seed_commit"

# 3. libDaisy at the commit Rocco's install script pins, and the three amp models.
daisy_commit=$(sed -n 's/^daisy_commit=//p' "$seed/scripts/install.sh")
if [[ ! -d "$seed/libs/libDaisy/.git" ]]; then
    git clone -q --no-checkout https://github.com/electro-smith/libDaisy.git "$seed/libs/libDaisy"
fi
git -C "$seed/libs/libDaisy" checkout -q --detach "$daisy_commit"
git -C "$seed/libs/libDaisy" submodule update -q --init --recursive
missing=0
for name in $(cd "$seed" && python3 -B scripts/convert_a2.py names); do
    [[ -f "$seed/models/local/$name.nam" ]] || missing=1
done
if [[ $missing -eq 1 ]]; then
    (cd "$seed" && bash scripts/download_models.sh)
fi

# 4. The link: the receiver files and the effects (delay, reverb) from this repo, then the patch.
mkdir -p "$seed/src/link" "$seed/src/fx"
cp "$root"/firmware/main/seed_link_proto/{seed_link_proto.h,seed_link_proto.c,slp_receiver.h,slp_receiver.c,slp_params.h} "$seed/src/link/"
cp "$root"/seed-dsp/{effects.h,effects.cpp} "$seed/src/fx/"
git -C "$seed" apply "$root/docs/seed3-link.patch"

# 5. Build (libDaisy at -Os, the firmware at -O3, as Rocco's Makefile does).
if [[ $boot -eq 0 ]]; then
    make -C "$seed" build JOBS="${JOBS:-8}" > "$work/build.log" 2>&1 || {
        grep -E "overflowed|error:" "$work/build.log" | tail -5
        echo "The internal flash (128 KB) is too small for this build; use the default (the bootloader)." >&2
        exit 1
    }
    grep -E '^\s+FLASH:' "$work/build.log"
    out="$work/seed3-link"
    cp "$seed/build/passthrough.bin" "$out.bin"
    cp "$seed/build/passthrough.elf" "$out.elf"
else
    cp "$here/STM32H750IB_sram_app.lds" "$seed/make/"
    make -C "$seed" build JOBS="${JOBS:-8}" APP_TYPE=BOOT_SRAM LDSCRIPT=make/STM32H750IB_sram_app.lds BUILD_DIR=build-boot \
        > "$work/build-boot.log" 2>&1 || { tail -30 "$work/build-boot.log"; exit 1; }
    grep -E '^\s+(SRAM|DTCMRAM):' "$work/build-boot.log"
    out="$work/seed3-link-boot"
    cp "$seed/build-boot/passthrough.bin" "$out.bin"
    cp "$seed/build-boot/passthrough.elf" "$out.elf"
    # The bootloader flash.sh installs: libDaisy's, at the pinned commit (waits 2 s for DFU at power-up).
    cp "$seed/libs/libDaisy/core/dsy_bootloader_v6_4-intdfu-2000ms.bin" "$work/daisy-bootloader.bin"
    tested_sha256=$tested_boot_sha256
fi

# 6. Say whether this is the exact binary tested on the Seed.
sha=$(shasum -a 256 "$out.bin" | cut -d' ' -f1)
echo "Built $out.bin ($sha)"
if [[ -z "$tested_sha256" ]]; then
    echo "Note: this version of the patch hasn't been run on a Seed yet."
elif [[ "$sha" == "$tested_sha256" ]]; then
    echo "Identical to the binary tested on a Seed."
else
    echo "Note: differs from the binary tested on a Seed (a different compiler or a changed receiver/patch)."
fi
