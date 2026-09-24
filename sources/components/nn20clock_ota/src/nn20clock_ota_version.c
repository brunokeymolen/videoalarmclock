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
/* nn20clock_ota_version.c - see nn20clock_ota_version.h. */
#include "nn20clock_ota_version.h"

#include <stddef.h>

/*
 * A version number this large is a malformed manifest rather than a
 * very new release, and refusing it here is what keeps the arithmetic
 * below from overflowing.
 */
#define COMPONENT_MAX 99999u

bool nn20clock_ota_parse_version(const char *text, unsigned out[3],
                                 bool *out_extra)
{
    if (text == NULL || out == NULL) {
        return false;
    }
    if (*text == 'v' || *text == 'V') {
        text++;
    }

    unsigned parsed[3] = {0u, 0u, 0u};
    for (int i = 0; i < 3; i++) {
        if (*text < '0' || *text > '9') {
            return false;
        }
        unsigned value = 0u;
        while (*text >= '0' && *text <= '9') {
            if (value > COMPONENT_MAX) {
                return false;
            }
            value = (value * 10u) + (unsigned)(*text - '0');
            text++;
        }
        parsed[i] = value;

        if (i < 2) {
            if (*text != '.') {
                return false;
            }
            text++;
        }
    }

    /*
     * Written only once all three parsed, so a caller that ignores the
     * false return is not left with half a version.
     */
    for (int i = 0; i < 3; i++) {
        out[i] = parsed[i];
    }
    if (out_extra != NULL) {
        *out_extra = (*text != '\0');
    }
    return true;
}

NN20ClockOtaVersionVerdict nn20clock_ota_compare_versions(const char *offered,
                                                          const char *running)
{
    unsigned theirs[3] = {0u, 0u, 0u};
    unsigned ours[3] = {0u, 0u, 0u};

    if (!nn20clock_ota_parse_version(offered, theirs, NULL)) {
        return NN20CLOCK_OTA_VERSION_OFFER_UNREADABLE;
    }
    if (!nn20clock_ota_parse_version(running, ours, NULL)) {
        return NN20CLOCK_OTA_VERSION_RUNNING_UNREADABLE;
    }

    for (int i = 0; i < 3; i++) {
        if (theirs[i] != ours[i]) {
            return (theirs[i] > ours[i]) ? NN20CLOCK_OTA_VERSION_NEWER
                                         : NN20CLOCK_OTA_VERSION_NOT_NEWER;
        }
    }

    /*
     * Equal tags. Extra commits on the running side only make it newer,
     * never older, so there is nothing to offer either way.
     */
    return NN20CLOCK_OTA_VERSION_NOT_NEWER;
}
