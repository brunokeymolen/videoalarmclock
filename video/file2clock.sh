#!/usr/bin/env bash
#
# file2clock.sh - turn a video file you already have into something the
# clock plays, and optionally upload it.
#
#   ./file2clock.sh <video-file> <name.avi> [clock-host]
#
#   ./file2clock.sh holiday.mov clip.avi
#   ./file2clock.sh ~/Videos/sunrise.mp4 sunrise.avi 192.168.0.201
#
# The same job as yt2clock.sh without the download: that one is a
# convenience for YouTube, this one is the general case. Anything ffmpeg
# can read works - phone footage, a DVD rip, an old AVI, a GIF.
#
# These files are large: 720x720 MJPEG at 20 fps runs about 65 MB per
# minute, so a four-minute video is roughly 260 MB. Two environment
# variables trim the source before converting, which is usually what
# you want:
#
#   START=00:01:30 DURATION=00:00:45 ./file2clock.sh in.mp4 clip.avi
#
# With a third argument the result is uploaded over FTP. THE CLOCK ONLY
# LISTENS WHILE ITS MEDIA SCREEN IS OPEN: on the device, gear icon ->
# Media. Open it before running this, and leave it open until the
# transfer finishes - closing it stops the server mid-upload.
set -euo pipefail

FFMPEG="${FFMPEG:-ffmpeg}"

# Optional trim, applied to the source. Empty means "all of it".
START="${START:-}"
DURATION="${DURATION:-}"

die() { printf 'file2clock: %s\n' "$*" >&2; exit 1; }

usage() {
    sed -n '3,24p' "$0" | sed 's/^#\ \?//'
    exit 2
}

# ----------------------------------------------------------- arguments --

[ $# -ge 2 ] && [ $# -le 3 ] || usage

SOURCE="$1"
NAME="$2"
CLOCK="${3:-}"

[ -f "$SOURCE" ] || die "'$SOURCE' is not a file"
[ -r "$SOURCE" ] || die "'$SOURCE' cannot be read"

#
# 8.3, and enforced here rather than discovered on the card.
#
# The firmware builds FATFS with long filenames switched off
# (CONFIG_FATFS_LFN_NONE), so a longer name cannot be created at all -
# an upload would fail, or worse, land under a mangled name that no
# longer matches the alarm pointing at it. Eight characters, a dot, and
# "avi".
#
if ! printf '%s' "$NAME" | grep -Eq '^[A-Za-z0-9_-]{1,8}\.[Aa][Vv][Ii]$'; then
    die "'$NAME' is not an 8.3 name: up to 8 of A-Z a-z 0-9 _ - then .avi
     (the clock's filesystem has long filenames disabled)"
fi

# Refusing rather than overwriting: the output name is short and the
# source may well be sitting in the same directory under a similar one.
[ -e "$NAME" ] && die "'$NAME' already exists here; move it or pick another name"

command -v "$FFMPEG" >/dev/null 2>&1 || die "$FFMPEG is not installed - see README.md"
if [ -n "$CLOCK" ]; then
    command -v curl >/dev/null 2>&1 || die "curl is not installed, and it is what uploads"
fi

# ------------------------------------------------------------- convert --

#
# -ss and -t go BEFORE -i, which makes ffmpeg seek in the source rather
# than decode everything and throw most of it away. On a long file that
# is the difference between seconds and minutes.
#
TRIM=()
[ -n "$START" ] && TRIM+=(-ss "$START")
[ -n "$DURATION" ] && TRIM+=(-t "$DURATION")

echo "==> converting to 720x720 MJPEG + PCM"
[ -n "$START" ] && echo "    from $START"
[ -n "$DURATION" ] && echo "    for  $DURATION"

#
# The clock plays exactly one thing: MJPEG video at 720x720 with PCM
# audio in an AVI container. That is not a preference - there is no
# other decoder in the firmware - so this line is fixed rather than
# configurable.
#
# scale ... force_original_aspect_ratio=increase then crop: fill the
# square panel and cut the overflow, rather than letterboxing a 16:9
# video into bars the clock would display as black.
#
# -q:v is the MJPEG quantiser: 2 is best, 31 is worst, and 5 is roughly
# JPEG quality 90. Lower means bigger frames.
#
"$FFMPEG" -hide_banner "${TRIM[@]}" -i "$SOURCE" \
    -vf "scale=720:720:force_original_aspect_ratio=increase,crop=720:720" \
    -c:v mjpeg \
    -q:v 5 \
    -r 20 \
    -c:a pcm_s16le \
    -ar 44100 \
    -ac 2 \
    "$NAME"

#
# A source with no audio track is fine: the player accepts a silent AVI
# and falls back to the frame interval in the file's header for timing.
# It is worth knowing about, though, because an alarm that makes no
# sound is a poor alarm - so it is said, not refused.
#
if ! ffprobe -v error -select_streams a -show_entries stream=codec_name \
        -of csv=p=0 "$NAME" 2>/dev/null | grep -q .; then
    echo "    note: no audio track. The clock will play this, but an"
    echo "          alarm using it will be silent."
fi

SIZE_MB=$(( $(stat -c %s "$NAME") / 1024 / 1024 ))
echo "==> $NAME (${SIZE_MB} MB)"

# -------------------------------------------------------------- upload --

if [ -z "$CLOCK" ]; then
    echo "    copy it to /clock on the card, or re-run with the clock's address"
    exit 0
fi

echo "==> uploading to $CLOCK"
echo "    the clock must have its Media screen open (gear icon -> Media)"
if ! curl --connect-timeout 10 --ftp-method nocwd -T "$NAME" "ftp://$CLOCK/"; then
    die "upload failed - is the Media screen open on the clock?"
fi
echo "==> done"
