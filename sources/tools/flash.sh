#!/usr/bin/env bash
# Build the firmware and install it on the board over USB.
#
#   tools/flash.sh                      # find the port, build, flash, watch
#   tools/flash.sh -p /dev/ttyACM0      # say which port
#   tools/flash.sh --no-monitor         # flash and exit
#   tools/flash.sh --erase              # wipe the flash first, then install
#   tools/flash.sh --image dist/v1.2.0/assets/nn20clock-v1.2.0-full.bin
#                                       # write a staged release instead
#
# Everything happens inside the ESP-IDF container that tools/idf.sh
# builds; the host needs Docker and the board plugged in, nothing else.
#
# --erase wipes NVS along with the application, so the alarms and the
# Wi-Fi credentials stored on the device go with it. It is the answer to
# a board that will not boot, not part of a normal update.
#
# --image writes a merged full-flash image (bootloader, partition table,
# OTA selector and application, at offset 0) rather than building. It is
# how a release staged by release-scripts/prepare-release.sh gets onto a
# board before it is published - which is what the gap between the two
# release scripts is for. The path is relative to sources/, or absolute
# anywhere inside the repository - that is what the container can see.
set -euo pipefail

SOURCE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROJECT_DIR="${SOURCE_ROOT}/firmware/nn20clock"
# shellcheck source=lib/paths.sh
. "$SOURCE_ROOT/tools/lib/paths.sh"

PORT=""
MONITOR=1
ERASE=0
IMAGE=""

while [ $# -gt 0 ]; do
    case "$1" in
        -p|--port)     PORT="$2"; shift 2 ;;
        --image)       IMAGE="$2"; shift 2 ;;
        -p*)           PORT="${1#-p}"; shift ;;
        --no-monitor)  MONITOR=0; shift ;;
        --erase)       ERASE=1; shift ;;
        -h|--help)     sed -n '2,23p' "${BASH_SOURCE[0]}" | sed 's/^# \?//'; exit 0 ;;
        *)             echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

# Find the board if no port was given. Refuse to guess between two: the
# wrong one is somebody's 3D printer.
if [ -z "$PORT" ]; then
    PORTS=()
    for p in /dev/ttyACM* /dev/ttyUSB*; do
        [ -e "$p" ] && PORTS+=("$p")
    done
    case "${#PORTS[@]}" in
        0) echo "no serial port found - is the board plugged in?" >&2
           echo "expected something like /dev/ttyACM0" >&2
           exit 1 ;;
        1) PORT="${PORTS[0]}"
           echo "==> using $PORT" >&2 ;;
        *) echo "several serial ports present: ${PORTS[*]}" >&2
           echo "pick one:  tools/flash.sh -p ${PORTS[0]}" >&2
           exit 1 ;;
    esac
fi

if [ ! -r "$PORT" ] || [ ! -w "$PORT" ]; then
    echo "warning: $PORT is not readable and writable by $(id -un)." >&2
    echo "         On most distributions that means:  sudo usermod -aG dialout $(id -un)" >&2
    echo "         then log out and back in. Carrying on anyway." >&2
fi

if [ -z "$IMAGE" ]; then
    # The target is set once per build directory. sdkconfig is generated,
    # so its absence is exactly the "first build here" signal.
    if [ ! -f "${PROJECT_DIR}/sdkconfig" ]; then
        echo "==> first build in this tree: setting the target to esp32p4" >&2
        "${SOURCE_ROOT}/tools/idf.sh" set-target esp32p4
    fi

    echo "==> building" >&2
    "${SOURCE_ROOT}/tools/idf.sh" build
else
    # A relative path is relative to sources/, which is where dist/ is.
    case "$IMAGE" in
        /*) IMAGE_HOST="$IMAGE" ;;
        *)  IMAGE_HOST="${SOURCE_ROOT}/${IMAGE}" ;;
    esac
    [ -f "$IMAGE_HOST" ] || { echo "no such image: $IMAGE" >&2; exit 1; }
    # esptool runs in the container, so the path has to be one the
    # container can see - which is anything inside the repository.
    MOUNT_ROOT="$(nn20clock_mount_root "$SOURCE_ROOT")"
    IMAGE_IN_CONTAINER="$(nn20clock_container_path "$MOUNT_ROOT" "$IMAGE_HOST")" || {
        echo "that image is outside $MOUNT_ROOT, which is all the container" >&2
        echo "can see. Copy it in, or pass a path inside the repository." >&2
        exit 1
    }
    echo "==> writing $IMAGE (merged image, offset 0)" >&2
fi

if [ "$ERASE" = 1 ]; then
    echo "==> erasing the whole flash (alarms and Wi-Fi settings included)" >&2
    "${SOURCE_ROOT}/tools/idf.sh" -p "$PORT" erase-flash
fi

echo "==> installing on $PORT" >&2
if [ -z "$IMAGE" ]; then
    "${SOURCE_ROOT}/tools/idf.sh" -p "$PORT" flash
else
    # idf.py has no subcommand for "write this file", so esptool is
    # called directly - through the same wrapper, so the container, the
    # user and the USB passthrough are the ones that already work.
    NN20CLOCK_IDF_RAW=1 "${SOURCE_ROOT}/tools/idf.sh" \
        python -m esptool --chip esp32p4 -p "$PORT" \
        write_flash 0x0 "$IMAGE_IN_CONTAINER"
fi

# idf.py monitor needs a real terminal; from a script or CI it fails with
# "Monitor requires standard input to be attached to TTY".
if [ "$MONITOR" = 1 ] && [ -t 0 ]; then
    echo "==> the clock is running. Ctrl-] leaves the monitor." >&2
    "${SOURCE_ROOT}/tools/idf.sh" -p "$PORT" monitor
else
    echo "==> done. Watch it with:  tools/idf.sh -p $PORT monitor" >&2
fi
