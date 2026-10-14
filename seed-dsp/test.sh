#!/bin/bash
# Builds and runs the amp_controls host tests (clang, AddressSanitizer/UBSan).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
out="$here/../build/seed-dsp"
mkdir -p "$out"
clang++ -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer \
    "$here/amp_controls.cpp" "$here/test_amp_controls.cpp" -o "$out/test_amp_controls"
"$out/test_amp_controls"
