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
 * Component tests for the display formatting (design 11).
 *
 * The clock face itself needs LVGL and a panel; deciding what text to
 * draw does not, which is the whole reason this is a separate
 * component. These are the cases a board would be a slow way to check.
 */
#include "test_util.h"

#include "nn20clock_timefmt.h"

static NN20ClockDateTime at(uint8_t hour, uint8_t minute)
{
    NN20ClockDateTime when = {
        .year = 2026, .month = 8, .day = 28,
        .hour = hour, .minute = minute, .second = 0,
        .weekday = 4,   /* Friday */
    };
    return when;
}

/* Fixed width matters on a display: without zero padding the digits
 * shift sideways every time an hour or minute rolls past 9. */
TEST(hhmm_is_zero_padded_and_24_hour)
{
    char out[NN20CLOCK_HHMM_SIZE];

    NN20ClockDateTime when = at(7, 5);
    CHECK_EQ(ESP_OK, nn20clock_timefmt_hhmm(&when, out, sizeof(out)));
    CHECK_STR_EQ("07:05", out);

    when = at(0, 0);
    CHECK_EQ(ESP_OK, nn20clock_timefmt_hhmm(&when, out, sizeof(out)));
    CHECK_STR_EQ("00:00", out);

    /* 24-hour, so this is 23:59 rather than 11:59. */
    when = at(23, 59);
    CHECK_EQ(ESP_OK, nn20clock_timefmt_hhmm(&when, out, sizeof(out)));
    CHECK_STR_EQ("23:59", out);

    when = at(13, 30);
    CHECK_EQ(ESP_OK, nn20clock_timefmt_hhmm(&when, out, sizeof(out)));
    CHECK_STR_EQ("13:30", out);
}

/* A broken time must look broken. Drawing "0:0" or keeping the last
 * good value would leave someone trusting a wrong clock. */
TEST(an_impossible_time_shows_placeholders)
{
    char out[NN20CLOCK_HHMM_SIZE];

    NN20ClockDateTime when = at(24, 0);
    CHECK_EQ(ESP_ERR_INVALID_SIZE, nn20clock_timefmt_hhmm(&when, out,
                                                          sizeof(out)));
    CHECK_STR_EQ("--:--", out);

    when = at(12, 60);
    CHECK_EQ(ESP_ERR_INVALID_SIZE, nn20clock_timefmt_hhmm(&when, out,
                                                          sizeof(out)));
    CHECK_STR_EQ("--:--", out);
}

TEST(hhmm_rejects_a_short_buffer)
{
    char out[NN20CLOCK_HHMM_SIZE];
    NN20ClockDateTime when = at(12, 0);

    CHECK_EQ(ESP_ERR_INVALID_ARG, nn20clock_timefmt_hhmm(NULL, out,
                                                         sizeof(out)));
    CHECK_EQ(ESP_ERR_INVALID_ARG, nn20clock_timefmt_hhmm(&when, NULL,
                                                         sizeof(out)));
    /* One byte short of "HH:MM\0" is still short. */
    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_timefmt_hhmm(&when, out, NN20CLOCK_HHMM_SIZE - 1));
}

TEST(the_date_is_iso_ordered)
{
    char out[NN20CLOCK_DATE_SIZE];
    NN20ClockDateTime when = at(12, 0);

    CHECK_EQ(ESP_OK, nn20clock_timefmt_date(&when, out, sizeof(out)));
    CHECK_STR_EQ("2026-08-28", out);

    when.month = 13;
    CHECK_EQ(ESP_ERR_INVALID_SIZE, nn20clock_timefmt_date(&when, out,
                                                          sizeof(out)));
    CHECK_STR_EQ("----------", out);

    when.month = 8;
    when.day = 0;
    CHECK_EQ(ESP_ERR_INVALID_SIZE, nn20clock_timefmt_date(&when, out,
                                                          sizeof(out)));

    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_timefmt_date(&when, out, NN20CLOCK_DATE_SIZE - 1));
}

/* Design 10 numbers weekdays from Monday so the value indexes
 * weekdays_mask directly; the names have to follow that, not struct
 * tm's Sunday-first convention. */
TEST(weekday_zero_is_monday)
{
    CHECK_STR_EQ("Mon", nn20clock_timefmt_weekday(0));
    CHECK_STR_EQ("Fri", nn20clock_timefmt_weekday(4));
    CHECK_STR_EQ("Sun", nn20clock_timefmt_weekday(6));
    CHECK_STR_EQ("---", nn20clock_timefmt_weekday(7));
    CHECK_STR_EQ("---", nn20clock_timefmt_weekday(255));
}

TEST_MAIN("nn20clock_timefmt")
{
    RUN(hhmm_is_zero_padded_and_24_hour);
    RUN(an_impossible_time_shows_placeholders);
    RUN(hhmm_rejects_a_short_buffer);
    RUN(the_date_is_iso_ordered);
    RUN(weekday_zero_is_monday);
}
