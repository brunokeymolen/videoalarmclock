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
 * nn20clock_brightness.h - the day/night brightness schedule.
 *
 * Dual-mode, and deliberately pure: it is handed a schedule and a time
 * and returns a percentage. It owns no display, reads no clock, and
 * stores nothing. That is what lets the interesting part - the midnight
 * wrap and the boundary minutes - be tested on the host in a
 * millisecond instead of by waiting until 22:00.
 *
 * The time of day decides the backlight for the clock face, a film and
 * a ringing alarm alike. A film may only raise it, never lower it: the
 * schedule carries a playback minimum, and while something is playing
 * the backlight is whichever of the two is brighter - see
 * nn20clock_brightness_playback_percent(). The only other thing that
 * moves it is a finger on the brightness slider, which stands until the
 * screen it is on closes.
 *
 * ------------------------------------------------------------------
 * What it is for
 * ------------------------------------------------------------------
 *
 * A bedside clock at a brightness you can read across a sunlit room is
 * a lamp at three in the morning. So there are two brightnesses and two
 * times: the clock is bright from `day_start` and dim from
 * `night_start`, every day.
 *
 * Times are minutes since local midnight, 0 to 1439. Local, because the
 * only thing anybody cares about is what the clock face says.
 *
 * ------------------------------------------------------------------
 * The wrap is the whole problem
 * ------------------------------------------------------------------
 *
 * Day normally runs 07:00 to 22:00 and does not cross midnight, but
 * night always does. Rather than special-case that, "is it day" is
 * asked as a half-open interval [day_start, night_start) walked
 * forwards from day_start, so both orders fall out of the same
 * arithmetic:
 *
 *   day 07:00, night 22:00   ->  day is 07:00-21:59, night wraps
 *   day 22:00, night 07:00   ->  day wraps instead; legal, and what
 *                                somebody on nights would set
 *
 * Equal times mean a schedule with no night in it: always day. That is
 * the honest reading of "night starts the moment day starts", and it
 * gives the settings screen a way to say "no dimming" without needing a
 * separate switch.
 *
 * ------------------------------------------------------------------
 * The playback minimum
 * ------------------------------------------------------------------
 *
 * A clock dimmed far enough to sleep beside is too dim to watch a film
 * on. So there is a third level, the least a film is shown at. It is a
 * floor and nothing more: it does not replace the schedule, and it
 * never dims anything. At noon with day at 85% and the minimum at 50%,
 * a film plays at 85%; at 23:00 with night at 30%, at 50%. The day and
 * night times still decide which of the two schedule values applies.
 *
 * Zero - or anything at or below the night value - means no minimum,
 * which is what a schedule stored before this existed comes back as.
 */
#ifndef NN20CLOCK_BRIGHTNESS_H
#define NN20CLOCK_BRIGHTNESS_H

#include <stdbool.h>
#include <stdint.h>

#include "nn20clock_platform.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Minutes in a day; every time here is taken modulo this. */
#define NN20CLOCK_BRIGHTNESS_DAY_MINUTES 1440u

/*
 * The granularity the settings screen offers, in minutes.
 *
 * A quarter of an hour: on a 720 px slider a minute per pixel is a
 * control nobody can hit, and the difference between 21:53 and 22:00 is
 * not a thing anyone has an opinion about.
 */
#define NN20CLOCK_BRIGHTNESS_STEP_MINUTES 15u

typedef struct {
    uint8_t day_percent;
    uint8_t night_percent;
    uint16_t day_start_minutes;    /* 0-1439, local */
    uint16_t night_start_minutes;  /* 0-1439, local */
    uint8_t playback_min_percent;  /* the floor while a film plays; 0 = none */
} NN20ClockBrightnessSchedule;

/*
 * Whether `now_minutes` falls in the day half of the schedule.
 *
 * Out-of-range minutes are taken modulo the day rather than rejected:
 * this is called from a timer tick, and a schedule question is not the
 * place to discover a bad clock.
 */
bool nn20clock_brightness_is_day(const NN20ClockBrightnessSchedule *schedule,
                                 uint16_t now_minutes);

/* The percentage to drive the backlight at: the day or the night value,
 * whichever half `now_minutes` falls in. */
uint8_t nn20clock_brightness_percent(
    const NN20ClockBrightnessSchedule *schedule, uint16_t now_minutes);

/* The percentage to drive the backlight at while something is playing:
 * nn20clock_brightness_percent(), raised to the playback minimum if it
 * is below it. Never lower than the schedule. */
uint8_t nn20clock_brightness_playback_percent(
    const NN20ClockBrightnessSchedule *schedule, uint16_t now_minutes);

/* Clamp and normalise a schedule that came out of storage or off a
 * slider: percentages to 0-100, times into the day. */
void nn20clock_brightness_normalise(NN20ClockBrightnessSchedule *schedule);

#ifdef __cplusplus
}
#endif

#endif /* NN20CLOCK_BRIGHTNESS_H */
