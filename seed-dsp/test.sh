#!/bin/bash
# Builds and runs the seed-dsp host tests (clang, AddressSanitizer/UBSan).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
out="$here/../build/seed-dsp"
mkdir -p "$out"
flags=(-std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer)
clang++ "${flags[@]}" "$here/amp_controls.cpp" "$here/test_amp_controls.cpp" -o "$out/test_amp_controls"
clang++ "${flags[@]}" "$here/effects.cpp" "$here/test_effects.cpp" -o "$out/test_effects"
"$out/test_amp_controls"
"$out/test_effects"
