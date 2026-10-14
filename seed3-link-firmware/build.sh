#!/bin/bash
# Builds Jacob's Seed3 firmware: Rocco's realtime-nam-seed3 at a pinned commit plus the touchscreen
# link (docs/seed3-link.patch and the receiver in firmware/main/seed_link_proto/). Rocco's repo is
# only cloned, never changed. Everything lands in build/seed3-link/ (git-ignored): the firmware
# contains T3K-licensed model weights, so the binary is never committed.
#
#   bash seed3-link-firmware/build.sh
#   ARM_GNU_BIN=/path/to/arm-gnu-toolchain/bin bash seed3-link-firmware/build.sh
#
# Output: build/seed3-link/seed3-link.bin, then flash it with seed3-link-firmware/flash.sh.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
work="$root/build/seed3-link"

# Rocco's commit the patch is made for, and the toolchain release it was tested with.
seed_commit=c3abe82872c1f2023f14521141fc0f29e897cb79
arm_release=15.3.rel2
# SHA-256 of the binary this script produced and the Seed ran on 2026-10-13 (see README.md).
tested_sha256=4fe3f8fce9b76ea6301f81176a7b98c11c6dff1e3c7d4a04d3501624ce222c14

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
git -C "$seed" fetch -q origin
git -C "$seed" checkout -q --detach "$seed_commit"
git -C "$seed" reset -q --hard "$seed_commit"
git -C "$seed" clean -q -fdx -e libs/ -e models/local/ -e build/

# 3. libDaisy at the commit Rocco's install script pins, and the three amp models.
daisy_commit=$(sed -n 's/^daisy_commit=//p' "$seed/scripts/install.sh")
if [[ ! -d "$seed/libs/libDaisy/.git" ]]; then
    git clone -q --no-checkout https://github.com/electro-smith/libDaisy.git "$seed/libs/libDaisy"
fi
git -C "$seed/libs/libDaisy" checkout -q --detach "$daisy_commit"
git -C "$seed/libs/libDaisy" submodule update -q --init --recursive
if [[ ! -f "$seed/models/local/marshall-jcm800-g5-a2-lite.nam" ]]; then
    (cd "$seed" && bash scripts/download_models.sh)
fi

# 4. The link: the receiver files from this repo, then the patch.
mkdir -p "$seed/src/link"
cp "$root"/firmware/main/seed_link_proto/{seed_link_proto.h,seed_link_proto.c,slp_receiver.h,slp_receiver.c,slp_params.h} "$seed/src/link/"
git -C "$seed" apply "$root/docs/seed3-link.patch"

# 5. Build (libDaisy at -Os, the firmware at -O3, as Rocco's Makefile does).
make -C "$seed" build JOBS="${JOBS:-8}" > "$work/build.log" 2>&1 || { tail -30 "$work/build.log"; exit 1; }
grep -E '^\s+FLASH:' "$work/build.log"
cp "$seed/build/passthrough.bin" "$work/seed3-link.bin"
cp "$seed/build/passthrough.elf" "$work/seed3-link.elf"

# 6. Say whether this is the exact binary tested on the Seed.
sha=$(shasum -a 256 "$work/seed3-link.bin" | cut -d' ' -f1)
echo "Built $work/seed3-link.bin ($sha)"
if [[ "$sha" == "$tested_sha256" ]]; then
    echo "Identical to the binary tested on the Seed on 2026-10-13."
else
    echo "Note: differs from the binary tested on 2026-10-13 (a different compiler or a changed receiver/patch)."
fi
