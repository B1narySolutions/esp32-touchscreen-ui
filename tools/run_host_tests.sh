#!/bin/bash
# Builds and runs the host unit tests (tests/host) with clang + AddressSanitizer/UBSan on macOS
# or Linux; the counterpart of run_host_tests.ps1. Needs cmake and ninja (brew install cmake ninja).
#   bash tools/run_host_tests.sh [--examples]
# Also builds build/host/seed_sim, the Seed's receiver on a serial port:
#   build/host/seed_sim /dev/cu.usbmodemXXXX 60
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
build="$root/build/host"

cmake -S "$root/tests/host" -B "$build" -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_BUILD_TYPE=Debug >/dev/null
cmake --build "$build"
if [[ "${1:-}" == "--examples" ]]; then
    exec "$build/slp_examples"
fi
exec "$build/host_tests"
