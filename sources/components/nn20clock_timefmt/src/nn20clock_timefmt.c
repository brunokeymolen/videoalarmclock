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
 * nn20clock_timefmt.c - see the header.
 *
 * The placeholders matter more than they look. A clock that has been
 * handed a broken time should say so plainly rather than draw "0:0" or
 * a stale value, because on a bedside display the difference between a
 * wrong time and an obviously-absent one is whether someone trusts it.
 */
#include "nn20clock_timefmt.h"

#include <stdio.h>
#include <string.h>

static const char *WEEKDAYS[7] = {
    "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"
};

esp_err_t nn20clock_timefmt_hhmm(const NN20ClockDateTime *when, char *out,
                                 size_t size)
{
    if (when == NULL || out == NULL || size < NN20CLOCK_HHMM_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }

    if (when->hour > 23u || when->minute > 59u) {
        snprintf(out, size, "--:--");
        return ESP_ERR_INVALID_SIZE;
    }

    /* Zero-padded: "07:05", never "7:5". Fixed width keeps the digits
     * from shifting sideways every time an hour rolls over. */
    snprintf(out, size, "%02u:%02u", (unsigned)when->hour,
             (unsigned)when->minute);
    return ESP_OK;
}

esp_err_t nn20clock_timefmt_date(const NN20ClockDateTime *when, char *out,
                                 size_t size)
{
    if (when == NULL || out == NULL || size < NN20CLOCK_DATE_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }

    if (when->month < 1u || when->month > 12u ||
        when->day < 1u || when->day > 31u) {
        snprintf(out, size, "----------");
        return ESP_ERR_INVALID_SIZE;
    }

    snprintf(out, size, "%04u-%02u-%02u", (unsigned)when->year,
             (unsigned)when->month, (unsigned)when->day);
    return ESP_OK;
}

const char *nn20clock_timefmt_weekday(uint8_t weekday)
{
    if (weekday > 6u) {
        return "---";
    }
    return WEEKDAYS[weekday];
}
