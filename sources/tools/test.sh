#!/usr/bin/env bash
# Host build + component tests. This is the fast inner loop: seconds, and
# no board attached. The ESP32-P4 build (tools/idf.sh build) is the slow
# confirmation, and tools/idf-test.sh is the rare on-target run.
#
#   tools/test.sh                 # everything
#   tools/test.sh -R timer        # just the suites matching "timer"
set -euo pipefail

SOURCE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${SOURCE_ROOT}/build"

cmake -S "$SOURCE_ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Debug >/dev/null
cmake --build "$BUILD_DIR" -j "$(nproc)"
ctest --test-dir "$BUILD_DIR" --output-on-failure "$@"
