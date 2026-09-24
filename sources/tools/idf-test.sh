#!/usr/bin/env bash
# Run the component tests ON THE BOARD.
#
# This is the sparse, deliberate run - it needs hardware attached and takes
# minutes. tools/test.sh is the everyday loop; use this one when something
# is genuinely target-specific (FreeRTOS task behavior, PSRAM, alignment,
# real toolchain codegen) or before calling a milestone done.
#
#   tools/idf-test.sh                       # build, flash, and watch
#   tools/idf-test.sh -p /dev/ttyUSB0
#
# The suites are the same source files test/ builds for the host; see
# test/target/main/nn20clock_target_test_main.c.
set -euo pipefail

SOURCE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

echo "==> building on-target test app (test/target)" >&2
NN20CLOCK_IDF_PROJECT=test/target "$SOURCE_ROOT/tools/idf.sh" build

echo "==> flashing and monitoring; the suite runs at boot" >&2
echo "    results print over serial. Ctrl-] to leave the monitor." >&2
NN20CLOCK_IDF_PROJECT=test/target "$SOURCE_ROOT/tools/idf.sh" "$@" flash monitor
