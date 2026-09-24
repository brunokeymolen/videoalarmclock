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
 * Component tests for the day/night brightness schedule.
 *
 * The reason this component exists apart from the display is here: the
 * midnight wrap and the boundary minutes are things you would otherwise
 * find out about at 22:00, once, on a board. Here every minute of the
 * day is a loop.
 */
#include "test_util.h"

#include "nn20clock_brightness.h"

#define HH_MM(h, m) ((uint16_t)((h) * 60 + (m)))

/* The shape somebody actually sets: bright from breakfast, dim from
 * bedtime. */
static NN20ClockBrightnessSchedule ordinary(void)
{
    const NN20ClockBrightnessSchedule schedule = {
        .day_percent = 80u,
        .night_percent = 25u,
        .day_start_minutes = HH_MM(7, 0),
        .night_start_minutes = HH_MM(22, 0),
    };
    return schedule;
}

/* Someone on night shifts: the day half is the one that wraps. */
static NN20ClockBrightnessSchedule inverted(void)
{
    const NN20ClockBrightnessSchedule schedule = {
        .day_percent = 80u,
        .night_percent = 25u,
        .day_start_minutes = HH_MM(22, 0),
        .night_start_minutes = HH_MM(7, 0),
    };
    return schedule;
}

TEST(day_runs_from_its_start_up_to_but_not_including_night)
{
    const NN20ClockBrightnessSchedule schedule = ordinary();

    /* The boundaries themselves, which is where an off-by-one lives. */
    CHECK(nn20clock_brightness_is_day(&schedule, HH_MM(7, 0)));
    CHECK(nn20clock_brightness_is_day(&schedule, HH_MM(21, 59)));
    CHECK(!nn20clock_brightness_is_day(&schedule, HH_MM(22, 0)));
    CHECK(!nn20clock_brightness_is_day(&schedule, HH_MM(6, 59)));

    /* And the middle of each half. */
    CHECK(nn20clock_brightness_is_day(&schedule, HH_MM(12, 0)));
    CHECK(!nn20clock_brightness_is_day(&schedule, HH_MM(3, 0)));
}

TEST(night_wraps_across_midnight)
{
    const NN20ClockBrightnessSchedule schedule = ordinary();

    CHECK(!nn20clock_brightness_is_day(&schedule, HH_MM(23, 59)));
    CHECK(!nn20clock_brightness_is_day(&schedule, HH_MM(0, 0)));
    CHECK(!nn20clock_brightness_is_day(&schedule, HH_MM(0, 1)));
}

TEST(a_schedule_whose_day_wraps_works_the_same_way)
{
    const NN20ClockBrightnessSchedule schedule = inverted();

    CHECK(nn20clock_brightness_is_day(&schedule, HH_MM(22, 0)));
    CHECK(nn20clock_brightness_is_day(&schedule, HH_MM(23, 59)));
    CHECK(nn20clock_brightness_is_day(&schedule, HH_MM(0, 0)));
    CHECK(nn20clock_brightness_is_day(&schedule, HH_MM(6, 59)));
    CHECK(!nn20clock_brightness_is_day(&schedule, HH_MM(7, 0)));
    CHECK(!nn20clock_brightness_is_day(&schedule, HH_MM(12, 0)));
}

TEST(every_minute_of_the_day_belongs_to_exactly_one_half)
{
    const NN20ClockBrightnessSchedule schedule = ordinary();
    unsigned day_minutes = 0;

    for (uint16_t m = 0; m < NN20CLOCK_BRIGHTNESS_DAY_MINUTES; m++) {
        if (nn20clock_brightness_is_day(&schedule, m)) {
            day_minutes++;
        }
    }

    /* 07:00 to 22:00 is fifteen hours, and the rest is night. No minute
     * is counted twice and none is missed - which is what a wrap bug
     * would show up as. */
    CHECK_EQ(15u * 60u, day_minutes);
}

TEST(equal_times_mean_a_schedule_with_no_night_in_it)
{
    NN20ClockBrightnessSchedule schedule = ordinary();
    schedule.night_start_minutes = schedule.day_start_minutes;

    for (uint16_t m = 0; m < NN20CLOCK_BRIGHTNESS_DAY_MINUTES; m += 37u) {
        CHECK(nn20clock_brightness_is_day(&schedule, m));
        CHECK_EQ(80u, nn20clock_brightness_percent(&schedule, m));
    }
}

TEST(the_percentage_follows_the_half_it_is_in)
{
    const NN20ClockBrightnessSchedule schedule = ordinary();

    CHECK_EQ(80u, nn20clock_brightness_percent(&schedule, HH_MM(12, 0)));
    CHECK_EQ(25u, nn20clock_brightness_percent(&schedule, HH_MM(3, 0)));
}

TEST(minutes_outside_the_day_are_wrapped_rather_than_rejected)
{
    const NN20ClockBrightnessSchedule schedule = ordinary();

    /* A tick from a clock that has not been set yet must not decide the
     * panel goes dark; it just lands somewhere sane. */
    CHECK_EQ(nn20clock_brightness_is_day(&schedule, HH_MM(12, 0)),
             nn20clock_brightness_is_day(
                 &schedule,
                 (uint16_t)(HH_MM(12, 0) + NN20CLOCK_BRIGHTNESS_DAY_MINUTES)));
}

TEST(normalise_clamps_percentages_and_folds_times_into_the_day)
{
    NN20ClockBrightnessSchedule schedule = {
        .day_percent = 200u,
        .night_percent = 101u,
        .day_start_minutes = 1440u + 60u,
        .night_start_minutes = 5000u,
    };
    nn20clock_brightness_normalise(&schedule);

    CHECK_EQ(100u, schedule.day_percent);
    CHECK_EQ(100u, schedule.night_percent);
    CHECK_EQ(60u, schedule.day_start_minutes);
    CHECK_EQ(5000u % 1440u, schedule.night_start_minutes);
}

TEST(playback_is_raised_to_the_minimum_at_night_only)
{
    NN20ClockBrightnessSchedule schedule = ordinary();
    schedule.playback_min_percent = 50u;

    /* Night is 25: a film is lifted to the floor. */
    CHECK_EQ(50u, nn20clock_brightness_playback_percent(&schedule,
                                                        HH_MM(23, 0)));
    /* Day is 80: the floor is below it and changes nothing. */
    CHECK_EQ(80u, nn20clock_brightness_playback_percent(&schedule,
                                                        HH_MM(12, 0)));
    /* And the boundary is still the schedule's, not the floor's. */
    CHECK_EQ(80u, nn20clock_brightness_playback_percent(&schedule,
                                                        HH_MM(21, 59)));
    CHECK_EQ(50u, nn20clock_brightness_playback_percent(&schedule,
                                                        HH_MM(22, 0)));
}

TEST(a_playback_minimum_never_dims)
{
    NN20ClockBrightnessSchedule schedule = ordinary();

    /* Zero is "no minimum": exactly the schedule, every minute. */
    schedule.playback_min_percent = 0u;
    for (uint16_t m = 0; m < NN20CLOCK_BRIGHTNESS_DAY_MINUTES; m++) {
        CHECK_EQ(nn20clock_brightness_percent(&schedule, m),
                 nn20clock_brightness_playback_percent(&schedule, m));
    }

    /* Above both halves it wins both; out of range it is clamped. */
    schedule.playback_min_percent = 90u;
    CHECK_EQ(90u, nn20clock_brightness_playback_percent(&schedule,
                                                        HH_MM(12, 0)));
    schedule.playback_min_percent = 150u;
    CHECK_EQ(100u, nn20clock_brightness_playback_percent(&schedule,
                                                         HH_MM(3, 0)));
    nn20clock_brightness_normalise(&schedule);
    CHECK_EQ(100u, schedule.playback_min_percent);
}

TEST(a_null_schedule_is_day_at_full_brightness)
{
    /* Never the dark answer: a missing schedule must not be a way to
     * end up with a panel nobody can read. */
    CHECK(nn20clock_brightness_is_day(NULL, 0u));
    CHECK_EQ(100u, nn20clock_brightness_percent(NULL, 0u));
    CHECK_EQ(100u, nn20clock_brightness_playback_percent(NULL, 0u));
    nn20clock_brightness_normalise(NULL);
}

TEST_MAIN("nn20clock_brightness")
{
    RUN(day_runs_from_its_start_up_to_but_not_including_night);
    RUN(night_wraps_across_midnight);
    RUN(a_schedule_whose_day_wraps_works_the_same_way);
    RUN(every_minute_of_the_day_belongs_to_exactly_one_half);
    RUN(equal_times_mean_a_schedule_with_no_night_in_it);

    RUN(the_percentage_follows_the_half_it_is_in);
    RUN(playback_is_raised_to_the_minimum_at_night_only);
    RUN(a_playback_minimum_never_dims);

    RUN(minutes_outside_the_day_are_wrapped_rather_than_rejected);
    RUN(normalise_clamps_percentages_and_folds_times_into_the_day);
    RUN(a_null_schedule_is_day_at_full_brightness);
}
