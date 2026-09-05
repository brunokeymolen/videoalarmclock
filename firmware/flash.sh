#!/usr/bin/env bash
#
# flash.sh - put the Video Alarm Clock firmware on a board.
#
#   ./flash.sh                       # newest release, autodetected port
#   ./flash.sh --port /dev/ttyACM0
#   ./flash.sh --version v0.2.0
#   ./flash.sh --file ./nn20clock-v0.2.0-full.bin
#
# This is the FIRST install, over a USB cable. After it, the clock
# updates itself: gear icon -> About -> Check for updates. You should
# not need this script twice on the same board.
#
# It downloads the release's `-full.bin`, which is the bootloader, the
# partition table, the OTA selector and the application already merged
# into one file that goes at offset 0. That is deliberate: the four
# pieces have four different offsets, one of them changed once already,
# and a board flashed with the app but not the OTA selector boots the
# wrong half of its own flash.
#
# The only thing needed on this machine is esptool:
#
#   pip install --user esptool
#
set -euo pipefail

OWNER="${VIDEOALARM_OWNER:-brunokeymolen}"
REPO="${VIDEOALARM_REPO:-videoalarmclock}"

PORT=""
VERSION=""
LOCAL_FILE=""
ERASE=0

die() { printf 'flash: %s\n' "$*" >&2; exit 1; }

usage() {
    sed -n '3,20p' "$0" | sed 's/^#\ \?//'
    exit 2
}

while [ $# -gt 0 ]; do
    case "$1" in
        -p|--port)    PORT="${2:-}";       shift 2 ;;
        -v|--version) VERSION="${2:-}";    shift 2 ;;
        -f|--file)    LOCAL_FILE="${2:-}"; shift 2 ;;
        --erase)      ERASE=1;             shift ;;
        -h|--help)    usage ;;
        *)            die "unknown option '$1' (try --help)" ;;
    esac
done

# ------------------------------------------------------------- esptool --

# Both spellings exist in the wild: a standalone `esptool.py` from pip,
# and `python -m esptool` inside a virtualenv or an ESP-IDF export.
if command -v esptool.py >/dev/null 2>&1; then
    ESPTOOL=(esptool.py)
elif python3 -c 'import esptool' >/dev/null 2>&1; then
    ESPTOOL=(python3 -m esptool)
else
    die "esptool is not installed.

     pip install --user esptool

   If pip refuses because the system Python is managed:

     pipx install esptool
     # or: python3 -m venv ~/.venvs/esptool &&
     #     ~/.venvs/esptool/bin/pip install esptool"
fi

# ---------------------------------------------------------------- port --

if [ -z "$PORT" ]; then
    # The board enumerates as a USB CDC device. One candidate is an
    # answer; several is a question only the person holding the cable
    # can settle, so it is asked rather than guessed.
    mapfile -t PORTS < <(ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null || true)
    case "${#PORTS[@]}" in
        0) die "no serial port found. Is the board plugged into its UART
     port and powered? Look for /dev/ttyACM* or /dev/ttyUSB*." ;;
        1) PORT="${PORTS[0]}" ;;
        *) die "several serial ports found: ${PORTS[*]}
     Say which one with --port" ;;
    esac
fi

[ -e "$PORT" ] || die "'$PORT' does not exist"
if [ ! -r "$PORT" ] || [ ! -w "$PORT" ]; then
    die "cannot open $PORT.

   On Linux the port belongs to the 'dialout' group:

     sudo usermod -aG dialout \"\$USER\"

   That takes effect at your next login, not immediately."
fi

# Nothing else may hold the port while esptool drives it - a serial
# monitor left open in another terminal is the usual culprit, and the
# failure it produces is a timeout that looks like broken hardware.
if command -v fuser >/dev/null 2>&1 && fuser "$PORT" >/dev/null 2>&1; then
    echo "flash: something else is holding $PORT:" >&2
    fuser -v "$PORT" >&2 || true
    die "close it and try again"
fi

# ---------------------------------------------------------------- image --

WORK=""
cleanup() { [ -n "$WORK" ] && rm -rf "$WORK"; }
trap cleanup EXIT

if [ -n "$LOCAL_FILE" ]; then
    [ -f "$LOCAL_FILE" ] || die "'$LOCAL_FILE' is not a file"
    IMAGE="$LOCAL_FILE"
    echo "==> using $IMAGE"
else
    command -v curl >/dev/null 2>&1 || die "curl is not installed, and it is what downloads"
    WORK="$(mktemp -d)"

    if [ -z "$VERSION" ]; then
        echo "==> asking GitHub for the newest release"
        VERSION="$(curl -fsSL \
            "https://api.github.com/repos/$OWNER/$REPO/releases/latest" \
            | sed -n 's/.*"tag_name": *"\([^"]*\)".*/\1/p' | head -1)"
        [ -n "$VERSION" ] || die "could not work out the newest release.
     Give one with --version, or check https://github.com/$OWNER/$REPO/releases"
    fi

    ASSET="nn20clock-$VERSION-full.bin"
    BASE="https://github.com/$OWNER/$REPO/releases/download/$VERSION"

    echo "==> downloading $VERSION"
    curl -fL --progress-bar -o "$WORK/$ASSET" "$BASE/$ASSET" \
        || die "could not download $ASSET.
     Check https://github.com/$OWNER/$REPO/releases/tag/$VERSION"
    IMAGE="$WORK/$ASSET"

    # SHA256SUMS ships beside the assets. A release without one is not a
    # reason to refuse - older ones may predate it - but a mismatch is.
    if curl -fsSL -o "$WORK/SHA256SUMS" "$BASE/SHA256SUMS" 2>/dev/null; then
        echo "==> checking the download"
        ( cd "$WORK" && grep " $ASSET\$" SHA256SUMS | sha256sum -c - ) \
            || die "the download does not match its published checksum.
     Do not flash it. Try again, and if it happens twice say so."
    else
        echo "    (no SHA256SUMS published for $VERSION; skipping the check)"
    fi
fi

# --------------------------------------------------------------- flash --

echo
echo "    board  $PORT"
echo "    image  $(basename "$IMAGE") ($(stat -c '%s' "$IMAGE") bytes)"
echo

if [ "$ERASE" -eq 1 ]; then
    # Wipes NVS too: every alarm, the Wi-Fi network, brightness and
    # volume. Only worth it when the board is misbehaving in a way that
    # smells like stale settings.
    echo "==> erasing the whole flash (this also clears alarms and Wi-Fi)"
    "${ESPTOOL[@]}" --chip esp32p4 -p "$PORT" erase_flash
fi

echo "==> flashing"
"${ESPTOOL[@]}" --chip esp32p4 -p "$PORT" -b 921600 \
    --before default_reset --after hard_reset \
    write_flash --flash_mode dio --flash_size 32MB --flash_freq 80m \
    0x0 "$IMAGE"

echo
echo "==> done. The board resets itself and should show the clock face."
echo
echo "    Next: set the Wi-Fi network on the device - gear icon -> Wi-Fi."
echo "    The time sets itself from the network a few seconds later."
echo "    Then put video on it: see ../video/README.md"
