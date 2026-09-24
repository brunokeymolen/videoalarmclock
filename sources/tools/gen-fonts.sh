#!/usr/bin/env bash
# Regenerate the clock face font.
#
#   tools/gen-fonts.sh
#
# The generated .c is committed, so this only needs running when the
# size, the glyph set, or the source font changes. It needs node (for
# npx) and the DejaVu fonts package; neither is needed for an ordinary
# build.
#
# Why a generated font at all: LVGL's built-in Montserrat sizes stop at
# 48 px, which is small on a 720x720 panel, and scaling a bitmap font up
# looks like a scaled bitmap font. This renders the glyphs at the size
# they are actually drawn.
#
# Only the glyphs the clock face uses are included - the ten digits, the
# colon, and the hyphen for the "--:--" placeholder. That is what keeps
# a 160 px font down to ~100 KB of flash instead of a few megabytes. Any
# new character on this screen must be added to RANGE or it renders as
# nothing.
set -euo pipefail

SOURCE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_DIR="${SOURCE_ROOT}/components/nn20clock_fonts/fonts"

SOURCE_FONT="${NN20CLOCK_FONT:-/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf}"

if [ ! -f "$SOURCE_FONT" ]; then
    echo "source font not found: $SOURCE_FONT" >&2
    echo "install fonts-dejavu, or set NN20CLOCK_FONT to another TTF" >&2
    exit 1
fi

generate() {
    local name="$1" out_dir="$2" size="$3" range="$4"

    echo "==> ${name}: ${size}px from $(basename "$SOURCE_FONT")" >&2
    npx --yes lv_font_conv@1.5.3 \
        --font "$SOURCE_FONT" \
        --range "$range" \
        --size "$size" \
        --bpp 4 \
        --format lvgl \
        --lv-include lvgl.h \
        --no-compress \
        -o "${out_dir}/${name}.c"
}

# The clock face. Only the glyphs it draws - ten digits, the colon, and
# the hyphen for the "--:--" placeholder. That is what keeps a 220 px
# font to a few hundred KB of source instead of megabytes.
generate "nn20clock_font_clock_220" \
    "${OUT_DIR}" \
    220 '0x30-0x39,0x3A,0x2D'

# The ringing screen shows the time between two large buttons, so it
# gets its own size rather than the clock face's: 220 px would not fit
# in what is left, and scaling one down looks like a scaled bitmap. Same
# glyph set - it is the same kind of thing being said.
generate "nn20clock_font_clock_140" \
    "${OUT_DIR}" \
    140 '0x30-0x39,0x3A,0x2D'

# The settings screens. LVGL's built-in Montserrat has no bold face, and
# thin text on a coloured button is hard to read at arm's length - which
# is how the weekday toggles first came out. Printable ASCII, since this
# is general UI text rather than a fixed set of glyphs.
generate "nn20clock_font_ui_bold_28" \
    "${OUT_DIR}" \
    28 '0x20-0x7E'

echo "==> done; rebuild with: tools/idf.sh build" >&2
