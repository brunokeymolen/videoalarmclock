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
 * nn20clock_timezones.h - the time zones the settings screen offers
 * (design 14).
 *
 * A list of places, not a text field. What the clock stores and applies
 * is a POSIX TZ string, and nobody standing at an alarm clock should be
 * asked to type "EST5EDT,M3.2.0,M11.1.0" on a touch keyboard - one
 * wrong character and the clock is silently in UTC. So the screen picks
 * from here, and every string here is checked on the host by the
 * timefmt suite against the offsets it claims.
 *
 * The device has no zone database, so each entry carries its own
 * daylight-saving rule. Those rules are the ones in force in 2026; a
 * country that changes its rule needs this table changed and a
 * firmware update, the same as any other clock without tzdata.
 *
 * Pure data and lookups, no LVGL: dual-mode like the rest of timefmt.
 */
#ifndef NN20CLOCK_TIMEZONES_H
#define NN20CLOCK_TIMEZONES_H

#include <stddef.h>
#include <stdint.h>

#include "nn20clock_platform.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* Cities, ASCII only: the UI fonts carry nothing else. */
    const char *name;
    /* The POSIX TZ string stored and applied. Fits
     * NN20CLOCK_TIMEZONE_MAX. */
    const char *posix;
    /* Standard (winter) time, in minutes east of UTC. */
    int16_t utc_offset_minutes;
} NN20ClockTimezone;

/* Longest "UTC+5:45" plus the terminator. */
#define NN20CLOCK_UTC_OFFSET_SIZE 10

size_t nn20clock_timezones_count(void);

/* NULL past the end. Ordered west to east, as a list of zones on any
 * other device is. */
const NN20ClockTimezone *nn20clock_timezones_at(size_t index);

/*
 * The entry whose POSIX string is exactly `posix`, or NULL.
 *
 * NULL is a real answer and not an error: a zone set some other way -
 * the Kconfig fallback, say - may not be on the list, and the screen
 * then shows the raw string rather than pretending it is a city.
 */
const NN20ClockTimezone *nn20clock_timezones_find(const char *posix);

/*
 * "UTC-5", "UTC+5:30", "UTC" - for the standard offset. The hours are
 * not padded: this is read beside a city name, not lined up in a column.
 *
 * ESP_ERR_INVALID_ARG on NULL or a buffer smaller than
 * NN20CLOCK_UTC_OFFSET_SIZE.
 */
esp_err_t nn20clock_timezones_format_offset(int16_t minutes, char *out,
                                            size_t size);

#ifdef __cplusplus
}
#endif

#endif /* NN20CLOCK_TIMEZONES_H */
