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
 * nn20clock_timefmt.h - turning a time into the text on the screen.
 *
 * Small and dual-mode on purpose. The screen that shows this is
 * firmware-only because it needs LVGL, but deciding what characters to
 * draw is pure logic, and pure logic belongs where it can be tested in
 * seconds without a board.
 */
#ifndef NN20CLOCK_TIMEFMT_H
#define NN20CLOCK_TIMEFMT_H

#include <stddef.h>

#include "nn20clock_platform.h"
#include "nn20clock_timer.h"

#ifdef __cplusplus
extern "C" {
#endif

/* "HH:MM" plus the terminator. */
#define NN20CLOCK_HHMM_SIZE 6

/* "YYYY-MM-DD" plus the terminator. */
#define NN20CLOCK_DATE_SIZE 11

/*
 * Write "HH:MM" - 24-hour, zero-padded, so the text never changes width
 * and the digits never shift on screen.
 *
 * ESP_ERR_INVALID_ARG on NULL or a buffer smaller than
 * NN20CLOCK_HHMM_SIZE. ESP_ERR_INVALID_SIZE if the time is out of range
 * (hour > 23, minute > 59), with the buffer left as "--:--" rather than
 * showing a nonsense time as though it were real.
 */
esp_err_t nn20clock_timefmt_hhmm(const NN20ClockDateTime *when, char *out,
                                 size_t size);

/* "YYYY-MM-DD". Same contract; the placeholder is "----------". */
esp_err_t nn20clock_timefmt_date(const NN20ClockDateTime *when, char *out,
                                 size_t size);

/* "Mon".."Sun", using design 10's numbering where 0 is Monday. Returns
 * "---" for anything out of range. */
const char *nn20clock_timefmt_weekday(uint8_t weekday);

#ifdef __cplusplus
}
#endif

#endif /* NN20CLOCK_TIMEFMT_H */
