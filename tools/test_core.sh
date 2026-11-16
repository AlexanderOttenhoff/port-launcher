#!/usr/bin/env bash
# Builds and runs the core unit tests on the host. Host builds exist only for these tests;
# Android arm64 is the only shipped target.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build/core-host"
cmake -S "$ROOT/core" -B "$BUILD" -G Ninja -DPLM_CORE_TESTS=ON -DCMAKE_BUILD_TYPE=Debug >/dev/null
cmake --build "$BUILD"
ctest --test-dir "$BUILD" --output-on-failure
