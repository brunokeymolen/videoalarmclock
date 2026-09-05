#!/usr/bin/env bash
#
# yt2clock.sh - fetch a video and turn it into something NN20Clock plays.
#
#   ./yt2clock.sh <youtube-url> <name.avi> [clock-host]
#
#   ./yt2clock.sh "https://www.youtube.com/watch?v=PIb6AZdTr-A" clg308.avi
#   ./yt2clock.sh "https://youtu.be/PIb6AZdTr-A" clg308.avi 192.168.0.201
#
# These files are large: 720x720 MJPEG at 20 fps runs about 65 MB per
# minute, so trimming a long source is usually what you want. The same
# two variables file2clock.sh takes, with the same meaning:
#
#   DURATION=00:00:45 ./yt2clock.sh "https://youtu.be/..." clip.avi
#
# They are applied when converting, not when downloading: yt-dlp still
# fetches the whole video, and what this saves is the encode and the
# size of the result.
#
# The clock plays exactly one thing: MJPEG video at 720x720 with PCM
# audio in an AVI container. That is not a preference - there is no
# other decoder in the firmware - so the ffmpeg line below is fixed
# rather than configurable.
#
# With a third argument the result is uploaded over FTP. THE CLOCK ONLY
# LISTENS WHILE ITS MEDIA SCREEN IS OPEN: on the device, gear icon ->
# Media. Open it before running this, and leave it open until the
# transfer finishes - closing it stops the server mid-upload.
#
# See README.md next to this script for installing ffmpeg and yt-dlp.
set -euo pipefail

YTDLP="${YTDLP:-yt-dlp}"
FFMPEG="${FFMPEG:-ffmpeg}"

# Optional trim, applied to the downloaded source. Empty means "all of
# it". videoclock passes these through into the container.
START="${START:-}"
DURATION="${DURATION:-}"

die() { printf 'yt2clock: %s\n' "$*" >&2; exit 1; }

usage() {
    sed -n '3,30p' "$0" | sed 's/^#\ \?//'
    exit 2
}

# ----------------------------------------------------------- arguments --

[ $# -ge 2 ] && [ $# -le 3 ] || usage

URL="$1"
NAME="$2"
CLOCK="${3:-}"

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

[ -e "$NAME" ] && die "'$NAME' already exists here; move it or pick another name"

for tool in "$YTDLP" "$FFMPEG"; do
    command -v "$tool" >/dev/null 2>&1 || die "$tool is not installed - see README.md"
done
if [ -n "$CLOCK" ]; then
    command -v curl >/dev/null 2>&1 || die "curl is not installed, and it is what uploads"
fi

# ------------------------------------------------------------ download --

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

#
# YouTube needs a JavaScript runtime to work out the download URLs, and
# yt-dlp enables only Deno by default. With Node installed but not Deno,
# extraction fails with a warning about runtimes followed by a flat
# "This video is not available" - which reads like the video is gone
# rather than like a missing dependency, and cost an afternoon once.
#
# So: use Deno if it is there, otherwise tell yt-dlp about Node.
#
JS_ARGS=()
if command -v deno >/dev/null 2>&1; then
    :
elif command -v node >/dev/null 2>&1; then
    JS_ARGS=(--js-runtimes node)
elif command -v bun >/dev/null 2>&1; then
    JS_ARGS=(--js-runtimes bun)
else
    echo "yt2clock: no JavaScript runtime (deno, node or bun) - YouTube" >&2
    echo "          downloads will probably fail. See README.md." >&2
fi

echo "==> downloading"
#
# --no-playlist because a YouTube URL copied from a playlist carries the
# whole list with it, and "&list=..." would otherwise fetch all of it.
# --no-part so the finished file is the only thing in the directory,
# which is how the next step finds it without guessing the extension.
#
"$YTDLP" --no-playlist --no-part "${JS_ARGS[@]}" \
    -o "$WORK/source.%(ext)s" "$URL"

# Exactly one file, or something unexpected happened - a merge left two
# streams behind, say. Better to stop than to convert the wrong one.
mapfile -t FOUND < <(find "$WORK" -maxdepth 1 -type f)
[ "${#FOUND[@]}" -eq 1 ] || die "expected one downloaded file, found ${#FOUND[@]}"
SOURCE="${FOUND[0]}"

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
# scale ... force_original_aspect_ratio=increase then crop: fill the
# square panel and cut the overflow, rather than letterboxing a 16:9
# video into bars the clock would display as black.
#
# -q:v is the MJPEG quantiser: 2 is best, 31 is worst, and 5 is roughly
# JPEG quality 90. Lower means bigger frames.
#
# SD reads are no longer the constraint - about 7.1 MB/s, against
# roughly 1.1 MB/s at these settings. The binding limit is the player
# thread, where decoding and the audio write are serialised: at 15 fps
# that came to 88% of the frame budget, so there is room to raise the
# rate but not unlimited room. Raise -q:v or -r and watch for stutter
# rather than assuming.
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
