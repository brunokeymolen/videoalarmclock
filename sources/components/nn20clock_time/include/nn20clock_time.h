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
 * nn20clock_time.h - the project's time types.
 *
 * These live in their own header for a structural reason. The Timer
 * needs the alarm model (to know what fires in a tick) and the alarm
 * model needs a calendar date (design 10's one-off alarms). If the date
 * type lived in the Timer's header the two would include each other,
 * and a cycle in headers is a cycle in the design.
 *
 * Header-only, no dependencies beyond the platform seam.
 */
#ifndef NN20CLOCK_TIME_H
#define NN20CLOCK_TIME_H

#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A calendar date with no time of day. Design 10's one-off alarms carry
 * one of these. */
typedef struct {
    uint16_t year;    /* full year, e.g. 2026 */
    uint8_t  month;   /* 1-12 */
    uint8_t  day;     /* 1-31 */
} NN20ClockDate;

/*
 * Broken-down local time. Always local: design 6 is explicit that NTP
 * keeps the displayed local time correct and that consumers must not
 * apply their own timezone conversion.
 */
typedef struct {
    uint16_t year;    /* full year, e.g. 2026 */
    uint8_t  month;   /* 1-12 */
    uint8_t  day;     /* 1-31 */
    uint8_t  hour;    /* 0-23 */
    uint8_t  minute;  /* 0-59 */
    uint8_t  second;  /* 0-59 */
    /* 0 = Monday .. 6 = Sunday, so a weekday indexes design 10's
     * weekdays_mask directly: (mask >> weekday) & 1. Note this is NOT
     * struct tm's tm_wday, which is 0 = Sunday. */
    uint8_t  weekday;
} NN20ClockDateTime;

#ifdef __cplusplus
}
#endif

#endif /* NN20CLOCK_TIME_H */
