#
# agc.sh - loudness levelling, shared by file2clock.sh and yt2clock.sh.
#
# Not a script to run. Both converters source it, because the logic is
# the same for a file on disk and a file just downloaded, and it is
# subtle enough that two copies would drift.
#
#   . "$(dirname "$0")/agc.sh"
#   agc_configure                 # early: validate the flag, check tools
#   agc_measure "$SOURCE"         # later: measure, fill in AUDIO_FILTER
#   "$FFMPEG" ... "${AUDIO_FILTER[@]}" ... out.avi
#
# The caller provides die(), FFMPEG, FFPROBE and TRIM. The split into
# two calls is so that a mistyped AGC= fails before yt2clock.sh spends
# five minutes downloading.
#
# ---------------------------------------------------------------------
#
# Why this exists: videos arrive at wildly different levels. A music
# video mastered for loudness and a clip off a phone can sit 20 dB
# apart, so the volume that is right for one is either inaudible or a
# heart attack for the other - and you find that out at 7am.
#
# Why it is two passes rather than one: ffmpeg's loudnorm has two modes.
# Handed a target and nothing else it works dynamically, riding the gain
# as the file plays - pushing quiet passages up and pulling loud ones
# down. That is the wrong thing here. The content is music and
# television, where the swell from a quiet intro to the chorus is the
# part that actually wakes you, and a gain rider flattens exactly that,
# audibly, as pumping.
#
# Handed the measurements from a first pass it instead works out one
# constant gain for the whole file and applies it from the first sample.
# Nothing is compressed, nothing pumps, and what changes is where this
# video sits relative to every other video on the card.
#
# The cost is one extra read of the source. It is audio only - -vn keeps
# the video decoder out of it - so it is far cheaper than the convert
# that follows, but it is not free, which is why this is opt-in.
#

#
# The targets, in EBU R128 terms. The defaults suit a bedside speaker
# rather than a broadcast chain: -16 LUFS is a few dB hotter than the
# -23 broadcast standard and about what streaming services use, -1.5
# dBTP keeps enough headroom that the loudest moment survives the
# resampler, and a loudness range of 11 is ffmpeg's own default, left
# alone so it does not fight the measurement.
#
AGC="${AGC:-}"
AGC_I="${AGC_I:--16}"
AGC_TP="${AGC_TP:--1.5}"
AGC_LRA="${AGC_LRA:-11}"

# Empty unless agc_measure finds something to do. Expands to nothing in
# the ffmpeg line when AGC is off.
AUDIO_FILTER=()

#
# Normalise the flag and check what it needs. Call this while arguments
# are still being checked, before any expensive work.
#
# AGC is a flag, and a flag people write in several ways. Anything
# obviously meaning "on" turns it on, anything obviously meaning "off"
# turns it off, and a typo is an error rather than a silent no - the
# whole point of the option is that you cannot hear whether it ran.
#
agc_configure() {
    case "$(printf '%s' "$AGC" | tr '[:upper:]' '[:lower:]')" in
        ""|0|no|off|false)  AGC="" ;;
        1|yes|on|true)      AGC=1 ;;
        *) die "AGC='$AGC' is neither on nor off: use AGC=1 or leave it unset" ;;
    esac

    [ -n "$AGC" ] || return 0

    command -v "$FFPROBE" >/dev/null 2>&1 \
        || die "$FFPROBE is not installed, and AGC needs it to find the audio track
     (it ships with ffmpeg - see README.md)"
}

# One field out of loudnorm's JSON. Not jq: the container does not have
# it, and each field is on its own line, which sed can manage.
agc_field() {
    printf '%s\n' "$1" \
        | sed -n "s/.*\"$2\"[^\"]*\"\([^\"]*\)\".*/\1/p" | head -1
}

#
# Measure $1 and leave the second-pass filter in AUDIO_FILTER. Does
# nothing if AGC is off, and turns AGC off rather than failing if the
# source cannot be measured - a silent alarm is a worse outcome than an
# unlevelled one.
#
agc_measure() {
    local source="$1" json ok=1 field
    local in_i in_tp in_lra in_thresh offset

    [ -n "$AGC" ] || return 0

    #
    # A source with no audio at all would make loudnorm print nothing to
    # parse, and the failure would surface later as an ffmpeg error
    # about a filter that was never instantiated. Check here, where the
    # message can say what is actually wrong.
    #
    # Captured rather than piped into grep -q: -o pipefail is on, and
    # grep -q exits on its first line, which can leave ffprobe killed by
    # SIGPIPE and the pipeline looking like a failure on a source that
    # does have audio.
    #
    local streams
    streams=$("$FFPROBE" -v error -select_streams a \
        -show_entries stream=codec_name -of csv=p=0 "$source" 2>/dev/null || true)
    if [ -z "$streams" ]; then
        echo "==> AGC: no audio track in the source, nothing to even out"
        AGC=""
        return 0
    fi

    echo "==> measuring loudness (AGC)"

    #
    # TRIM goes on this pass too. Measuring the whole of a four minute
    # song and then encoding forty seconds of it would level the clip
    # against audio that is not in it.
    #
    json=$("$FFMPEG" -hide_banner -nostats -v info \
        ${TRIM[@]+"${TRIM[@]}"} -i "$source" \
        -vn \
        -af "loudnorm=I=$AGC_I:TP=$AGC_TP:LRA=$AGC_LRA:print_format=json" \
        -f null - 2>&1 | sed -n '/^{/,/^}/p')

    in_i=$(agc_field "$json" input_i)
    in_tp=$(agc_field "$json" input_tp)
    in_lra=$(agc_field "$json" input_lra)
    in_thresh=$(agc_field "$json" input_thresh)
    offset=$(agc_field "$json" target_offset)

    #
    # Silence measures as -inf, and a missing field means the parse or
    # the pass itself went wrong. Either way the second pass would be
    # handed nonsense, so fall back to converting without AGC rather
    # than producing a file that is quietly wrong.
    #
    for field in "$in_i" "$in_tp" "$in_lra" "$in_thresh" "$offset"; do
        case "$field" in
            -inf|inf|nan|"") ok="" ;;
            *[!0-9.+-]*)     ok="" ;;
        esac
    done

    if [ -z "$ok" ]; then
        echo "    could not measure this source - converting without AGC"
        AGC=""
        return 0
    fi

    echo "    measured ${in_i} LUFS, peak ${in_tp} dBTP -> target ${AGC_I} LUFS"

    #
    # linear=true is the whole point: one gain for the file. It is not a
    # promise - loudnorm drops back to its dynamic mode on its own if
    # the constant gain would push the true peak past TP - but that only
    # happens where the alternative is clipping, which is the right
    # trade.
    #
    # dual_mono=true corrects the measurement of a mono source. R128
    # reads mono about 3 dB quieter than the same material coming out of
    # two speakers, and -ac 2 means it will be. It does nothing to a
    # source that is already stereo.
    #
    AUDIO_FILTER=(-af "loudnorm=I=$AGC_I:TP=$AGC_TP:LRA=$AGC_LRA\
:measured_I=$in_i:measured_TP=$in_tp:measured_LRA=$in_lra\
:measured_thresh=$in_thresh:offset=$offset\
:linear=true:dual_mono=true")
}
