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
#include "nn20clock_timezones.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Every abbreviation is plain letters, never the "<+07>" quoted form:
 * the abbreviation is never shown, and plain letters are the form every
 * libc parses, newlib's included.
 *
 * The Brussels entry must stay byte-for-byte the storage default and
 * the Kconfig default, or a clock that has never been configured would
 * open the picker on no zone at all.
 */
static const NN20ClockTimezone ZONES[] = {
    { "Honolulu",                   "HST10",                          -600 },
    { "Anchorage",                  "AKST9AKDT,M3.2.0,M11.1.0",       -540 },
    { "Los Angeles, Vancouver",     "PST8PDT,M3.2.0,M11.1.0",         -480 },
    { "Denver, Edmonton",           "MST7MDT,M3.2.0,M11.1.0",         -420 },
    { "Phoenix",                    "MST7",                           -420 },
    { "Chicago, Winnipeg",          "CST6CDT,M3.2.0,M11.1.0",         -360 },
    { "Mexico City",                "CST6",                           -360 },
    { "New York, Toronto",          "EST5EDT,M3.2.0,M11.1.0",         -300 },
    { "Bogota, Lima",               "COT5",                           -300 },
    { "Halifax",                    "AST4ADT,M3.2.0,M11.1.0",         -240 },
    { "St. John's",                 "NST3:30NDT,M3.2.0,M11.1.0",      -210 },
    { "Sao Paulo, Buenos Aires",    "BRT3",                           -180 },
    { "UTC, Reykjavik",             "UTC0",                              0 },
    { "London, Dublin, Lisbon",     "GMT0BST,M3.5.0/1,M10.5.0",          0 },
    { "Brussels, Paris, Berlin",    "CET-1CEST,M3.5.0,M10.5.0/3",       60 },
    { "Lagos",                      "WAT-1",                            60 },
    { "Athens, Helsinki, Kyiv",     "EET-2EEST,M3.5.0/3,M10.5.0/4",    120 },
    { "Johannesburg",               "SAST-2",                          120 },
    { "Istanbul, Moscow, Riyadh",   "MSK-3",                           180 },
    { "Dubai",                      "GST-4",                           240 },
    { "Karachi",                    "PKT-5",                           300 },
    { "India",                      "IST-5:30",                        330 },
    { "Kathmandu",                  "NPT-5:45",                        345 },
    { "Dhaka",                      "BDT-6",                           360 },
    { "Bangkok, Jakarta",           "ICT-7",                           420 },
    { "Singapore, Beijing, Perth",  "SGT-8",                           480 },
    { "Tokyo, Seoul",               "JST-9",                           540 },
    { "Darwin",                     "ACST-9:30",                       570 },
    { "Adelaide",                   "ACST-9:30ACDT,M10.1.0,M4.1.0/3",  570 },
    { "Brisbane",                   "AEST-10",                         600 },
    { "Sydney, Melbourne",          "AEST-10AEDT,M10.1.0,M4.1.0/3",    600 },
    { "Auckland",                   "NZST-12NZDT,M9.5.0,M4.1.0/3",     720 },
};

#define ZONE_COUNT (sizeof(ZONES) / sizeof(ZONES[0]))

size_t nn20clock_timezones_count(void)
{
    return ZONE_COUNT;
}

const NN20ClockTimezone *nn20clock_timezones_at(size_t index)
{
    return (index < ZONE_COUNT) ? &ZONES[index] : NULL;
}

const NN20ClockTimezone *nn20clock_timezones_find(const char *posix)
{
    if (posix == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < ZONE_COUNT; i++) {
        if (strcmp(ZONES[i].posix, posix) == 0) {
            return &ZONES[i];
        }
    }
    return NULL;
}

esp_err_t nn20clock_timezones_format_offset(int16_t minutes, char *out,
                                            size_t size)
{
    if (out == NULL || size < NN20CLOCK_UTC_OFFSET_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }

    if (minutes == 0) {
        snprintf(out, size, "UTC");
        return ESP_OK;
    }

    const char sign = (minutes < 0) ? '-' : '+';
    const unsigned magnitude = (unsigned)abs(minutes);
    if (magnitude % 60u == 0u) {
        snprintf(out, size, "UTC%c%u", sign, magnitude / 60u);
    } else {
        snprintf(out, size, "UTC%c%u:%02u", sign, magnitude / 60u,
                 magnitude % 60u);
    }
    return ESP_OK;
}
