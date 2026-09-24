/*
 * SPDX-FileCopyrightText: 2026 Bruno Keymolen
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of Video Alarm Clock.
 *
 * Video Alarm Clock is free software: you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
/*
 * nn20clock_fonts.h - the generated LVGL fonts (design 11).
 *
 * FIRMWARE ONLY. LVGL's built-in Montserrat stops at 48 px, which is
 * small on a 720x720 panel, and it has no bold face. Both of those are
 * fixed by rendering the glyphs at the size they are actually drawn -
 * see README.md and tools/gen-fonts.sh.
 *
 * They live in a component of their own because more than one screen
 * draws with them: the clock face and the ringing screen both want the
 * big digits, and a second copy compiled into a second component would
 * be hundreds of kilobytes of flash to say the same thing twice.
 *
 * Anything that only needs ordinary UI text should keep using LVGL's
 * built-in faces. These are for the two cases the built-ins cannot do.
 */
#ifndef NN20CLOCK_FONTS_H
#define NN20CLOCK_FONTS_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 220 px, and only the glyphs a clock shows: the ten digits, the colon,
 * and the hyphen for the "--:--" placeholder. Any other character
 * renders as nothing, so a screen that needs letters wants a different
 * font - not another glyph added to this one, which is already the
 * larger half of the firmware.
 */
LV_FONT_DECLARE(nn20clock_font_clock_220);

/*
 * 140 px, same glyphs. For the ringing screen, which shows the time
 * between two large buttons and has less room than the clock face.
 */
LV_FONT_DECLARE(nn20clock_font_clock_140);

/*
 * 28 px bold, printable ASCII. For text on coloured buttons, where
 * LVGL's regular Montserrat is hard to read at arm's length - which is
 * how the weekday toggles first came out.
 */
LV_FONT_DECLARE(nn20clock_font_ui_bold_28);

#ifdef __cplusplus
}
#endif

#endif /* NN20CLOCK_FONTS_H */
