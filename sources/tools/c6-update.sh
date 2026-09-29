#!/usr/bin/env bash
# Update the board's ESP32-C6 Wi-Fi co-processor firmware. Read
# C6-UPDATE.md at the top of the repository first - this is optional,
# and it is at your own risk.
#
#   tools/c6-update.sh                  # find the port, build, flash, watch
#   tools/c6-update.sh -p /dev/ttyACM0  # say which port
#
# Builds firmware/c6_cp (the C6 image) and firmware/c6_updater (a
# one-off P4 app that carries it), flashes the updater in place of the
# clock and opens the serial monitor. Wait for a RESULT line, leave the
# monitor with Ctrl+], and put the clock back with tools/flash.sh.
set -euo pipefail

SOURCE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IDF="$SOURCE_ROOT/tools/idf.sh"

PORT=""
while [ $# -gt 0 ]; do
    case "$1" in
        -p|--port) PORT="$2"; shift 2 ;;
        -p*)       PORT="${1#-p}"; shift ;;
        -h|--help) sed -n '2,13p' "${BASH_SOURCE[0]}" | sed 's/^# \?//'; exit 0 ;;
        *)         echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

if [ -z "$PORT" ]; then
    PORTS=()
    for p in /dev/ttyACM* /dev/ttyUSB*; do
        [ -e "$p" ] && PORTS+=("$p")
    done
    case "${#PORTS[@]}" in
        0) echo "no serial port found - is the board plugged in?" >&2; exit 1 ;;
        1) PORT="${PORTS[0]}"; echo "==> using $PORT" >&2 ;;
        *) echo "several serial ports present: ${PORTS[*]}" >&2
           echo "pick one:  tools/c6-update.sh -p ${PORTS[0]}" >&2
           exit 1 ;;
    esac
fi

echo "==> building the C6 firmware" >&2
NN20CLOCK_IDF_PROJECT=firmware/c6_cp "$IDF" build

echo "==> building the updater" >&2
NN20CLOCK_IDF_PROJECT=firmware/c6_updater "$IDF" build

echo "==> flashing the updater - the clock is replaced until tools/flash.sh" >&2
NN20CLOCK_IDF_PROJECT=firmware/c6_updater "$IDF" -p "$PORT" flash

echo "==> watching: wait for a RESULT line, then Ctrl+] to leave" >&2
NN20CLOCK_IDF_PROJECT=firmware/c6_updater "$IDF" -p "$PORT" monitor
