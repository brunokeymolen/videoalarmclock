#!/usr/bin/env bash
# Run an idf.py command for firmware/nn20clock inside the ESP-IDF
# devcontainer.
#
#   tools/idf.sh set-target esp32p4        # once, and after fullclean
#   tools/idf.sh build
#   tools/idf.sh -p /dev/ttyACM0 flash monitor
#
# Set NN20CLOCK_IDF_PROJECT to point the wrapper at another IDF project in
# this tree, which is how tools/idf-test.sh reuses it:
#
#   NN20CLOCK_IDF_PROJECT=test/target tools/idf.sh build
#
# NN20CLOCK_IDF_RAW=1 runs the arguments as a command inside the
# container instead of handing them to idf.py, so the same USB and user
# passthrough serves the few things idf.py has no subcommand for. It is
# how tools/flash.sh writes a merged release image:
#
#   NN20CLOCK_IDF_RAW=1 tools/idf.sh python -m esptool --chip esp32p4 version
#
# The container runs as the invoking user so build/ and sdkconfig are not
# left root-owned. HOME is redirected because ESP-IDF's export.sh writes
# there and the host home is not mounted.
#
# What is mounted is the repository, not just sources/ - see
# tools/lib/paths.sh for why that matters to the version in the image.
set -euo pipefail

SOURCE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=lib/paths.sh
. "$SOURCE_ROOT/tools/lib/paths.sh"
MOUNT_ROOT="$(nn20clock_mount_root "$SOURCE_ROOT")"
CONTAINER_SOURCE="$(nn20clock_container_path "$MOUNT_ROOT" "$SOURCE_ROOT")"
IMAGE="${NN20CLOCK_IDF_IMAGE:-nn20clock-esp32-idf}"
PROJECT="${NN20CLOCK_IDF_PROJECT:-firmware/nn20clock}"

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    echo "building $IMAGE from .devcontainer/esp32 ..." >&2
    docker build -t "$IMAGE" "$SOURCE_ROOT/.devcontainer/esp32"
fi

# Raw mode runs what it is given; otherwise everything is an idf.py
# subcommand, which is all but one caller.
COMMAND="idf.py"
[ "${NN20CLOCK_IDF_RAW:-0}" = "1" ] && COMMAND=""

# -it only when attached to a terminal, so CI and scripted use still work.
TTY_FLAGS=()
[ -t 0 ] && TTY_FLAGS=(-it)

DEVICE_FLAGS=()
[ -d /dev/bus/usb ] && DEVICE_FLAGS+=(--volume=/dev/bus/usb:/dev/bus/usb)

# Pass the board's serial port through. --volume is not enough for a
# character device: the container needs --device to get a usable node, or
# flashing fails with "No such file or directory".
for port in /dev/ttyACM* /dev/ttyUSB*; do
    [ -e "$port" ] && DEVICE_FLAGS+=(--device "$port")
done

# The esp user inside the image is not in dialout, so run as this user's
# groups too, otherwise opening the port is denied.
GROUP_FLAGS=()
for port in /dev/ttyACM0 /dev/ttyUSB0; do
    if [ -e "$port" ]; then
        GROUP_FLAGS+=(--group-add "$(stat -c '%g' "$port")")
        break
    fi
done

docker run --rm "${TTY_FLAGS[@]}" \
    --user "$(id -u):$(id -g)" \
    "${GROUP_FLAGS[@]}" \
    -e HOME=/tmp \
    -v "$MOUNT_ROOT:/workspace" \
    -w "${CONTAINER_SOURCE}/${PROJECT}" \
    --device-cgroup-rule='c 166:* rmw' \
    --device-cgroup-rule='c 188:* rmw' \
    "${DEVICE_FLAGS[@]}" \
    "$IMAGE" \
    bash -lc ". \$IDF_PATH/export.sh >/dev/null 2>&1 && $COMMAND $*"
