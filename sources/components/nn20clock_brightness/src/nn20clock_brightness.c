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
 * nn20clock_brightness.c - see the header.
 *
 * No state, no allocation, no logging. Every function here is a
 * function of its arguments, which is why the tests can sweep all 1440
 * minutes of a day in a loop.
 */
#include "nn20clock_brightness.h"

#define DAY_MINUTES NN20CLOCK_BRIGHTNESS_DAY_MINUTES

static uint16_t wrap(uint32_t minutes)
{
    return (uint16_t)(minutes % DAY_MINUTES);
}

/*
 * How far `minutes` is after `from`, walking forwards and through
 * midnight if need be. The whole wrap problem reduces to this: once
 * both the instant and the boundary are measured from the same origin,
 * the comparison is ordinary.
 */
static uint16_t forward_distance(uint16_t from, uint16_t minutes)
{
    return wrap((uint32_t)minutes + DAY_MINUTES - from);
}

bool nn20clock_brightness_is_day(const NN20ClockBrightnessSchedule *schedule,
                                 uint16_t now_minutes)
{
    if (schedule == NULL) {
        return true;   /* no schedule is not a reason to sit in the dark */
    }

    const uint16_t day_start = wrap(schedule->day_start_minutes);
    const uint16_t night_start = wrap(schedule->night_start_minutes);

    /* Night starting exactly when day does leaves no night at all. See
     * the header: this is how the screen says "never dim". */
    if (day_start == night_start) {
        return true;
    }

    const uint16_t now = wrap(now_minutes);
    return forward_distance(day_start, now) <
           forward_distance(day_start, night_start);
}

uint8_t nn20clock_brightness_percent(
    const NN20ClockBrightnessSchedule *schedule, uint16_t now_minutes)
{
    if (schedule == NULL) {
        return 100u;
    }

    const uint8_t percent = nn20clock_brightness_is_day(schedule, now_minutes)
                                ? schedule->day_percent
                                : schedule->night_percent;
    return (percent > 100u) ? 100u : percent;
}

uint8_t nn20clock_brightness_playback_percent(
    const NN20ClockBrightnessSchedule *schedule, uint16_t now_minutes)
{
    const uint8_t scheduled = nn20clock_brightness_percent(schedule,
                                                           now_minutes);
    if (schedule == NULL) {
        return scheduled;
    }

    const uint8_t floor = (schedule->playback_min_percent > 100u)
                              ? 100u
                              : schedule->playback_min_percent;
    return (scheduled < floor) ? floor : scheduled;
}

void nn20clock_brightness_normalise(NN20ClockBrightnessSchedule *schedule)
{
    if (schedule == NULL) {
        return;
    }
    if (schedule->day_percent > 100u) {
        schedule->day_percent = 100u;
    }
    if (schedule->night_percent > 100u) {
        schedule->night_percent = 100u;
    }
    if (schedule->playback_min_percent > 100u) {
        schedule->playback_min_percent = 100u;
    }
    schedule->day_start_minutes = wrap(schedule->day_start_minutes);
    schedule->night_start_minutes = wrap(schedule->night_start_minutes);
}
