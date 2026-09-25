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

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "nn20clock_timefmt.h"
#include "nn20clock_timezones.h"
#include "nn20clock_storage.h"

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

/* ------------------------------------------------------ time zones -- */

/* 2026-01-15 and 2026-07-15, both 12:00 UTC: one in each half of the
 * year, whichever hemisphere a zone is in. */
#define MID_JANUARY 1768478400
#define MID_JULY    1784116800

/* Minutes east of UTC at `when`, in whatever zone TZ says. Worked out
 * from the two broken-down times rather than tm_gmtoff, which is not
 * standard C. */
static int offset_at(time_t when)
{
    struct tm utc;
    struct tm local;
    if (gmtime_r(&when, &utc) == NULL || localtime_r(&when, &local) == NULL) {
        return 99999;
    }
    int days = local.tm_yday - utc.tm_yday;
    if (local.tm_year != utc.tm_year) {
        days = (local.tm_year > utc.tm_year) ? 1 : -1;
    }
    return days * 1440 + (local.tm_hour - utc.tm_hour) * 60 +
           (local.tm_min - utc.tm_min);
}

static void use_tz(const char *tz)
{
    setenv("TZ", tz, 1);
    tzset();
}

/*
 * Every string on the list is what it says it is.
 *
 * A mistyped TZ string does not fail: libc quietly treats it as UTC, so
 * the only way to catch one is to ask it for its offsets. Standard time
 * is the smaller of the two, whichever half of the year it falls in,
 * and a zone has a second, larger offset exactly when it has a rule.
 */
TEST(every_zone_has_the_offset_it_claims)
{
    const size_t count = nn20clock_timezones_count();
    REQUIRE(count > 0u);

    for (size_t i = 0; i < count; i++) {
        const NN20ClockTimezone *zone = nn20clock_timezones_at(i);
        REQUIRE(zone != NULL);
        CHECK(strlen(zone->posix) < NN20CLOCK_TIMEZONE_MAX);

        use_tz(zone->posix);
        const int january = offset_at(MID_JANUARY);
        const int july = offset_at(MID_JULY);
        const int standard = (january < july) ? january : july;
        const bool has_rule = (strchr(zone->posix, ',') != NULL);

        if (standard != zone->utc_offset_minutes) {
            printf("  %s (%s): standard %d, claims %d\n", zone->name,
                   zone->posix, standard, (int)zone->utc_offset_minutes);
        }
        CHECK_EQ(zone->utc_offset_minutes, standard);
        CHECK_EQ(has_rule, january != july);

        /* West to east, so the screen's list reads like any other. */
        if (i > 0u) {
            CHECK(nn20clock_timezones_at(i - 1u)->utc_offset_minutes <=
                  zone->utc_offset_minutes);
        }
    }
    CHECK(nn20clock_timezones_at(count) == NULL);

    use_tz("UTC0");
}

/* The zone a US build was first asked about: the change is on the
 * second Sunday of March at 02:00 local, which is 07:00 UTC. */
TEST(new_york_changes_on_the_right_morning)
{
    const NN20ClockTimezone *zone =
        nn20clock_timezones_find("EST5EDT,M3.2.0,M11.1.0");
    REQUIRE(zone != NULL);

    use_tz(zone->posix);
    CHECK_EQ(-300, offset_at((time_t)1772953200 - 1));
    CHECK_EQ(-240, offset_at((time_t)1772953200));
    use_tz("UTC0");
}

/*
 * A clock that has never been configured stores the storage default,
 * and the picker has to find it - or the one screen meant to show the
 * zone would open on none at all.
 */
TEST(the_storage_default_is_on_the_list)
{
    NN20ClockConfig defaults;
    REQUIRE(nn20clock_storage_default_config(&defaults) == ESP_OK);

    const NN20ClockTimezone *zone = nn20clock_timezones_find(defaults.timezone);
    REQUIRE(zone != NULL);
    CHECK_EQ(60, zone->utc_offset_minutes);

    CHECK(nn20clock_timezones_find("Mars/Olympus_Mons") == NULL);
    CHECK(nn20clock_timezones_find(NULL) == NULL);
}

TEST(offsets_read_as_people_write_them)
{
    char out[NN20CLOCK_UTC_OFFSET_SIZE];

    CHECK_EQ(ESP_OK, nn20clock_timezones_format_offset(0, out, sizeof(out)));
    CHECK_STR_EQ("UTC", out);
    CHECK_EQ(ESP_OK, nn20clock_timezones_format_offset(-300, out,
                                                       sizeof(out)));
    CHECK_STR_EQ("UTC-5", out);
    CHECK_EQ(ESP_OK, nn20clock_timezones_format_offset(330, out, sizeof(out)));
    CHECK_STR_EQ("UTC+5:30", out);
    CHECK_EQ(ESP_OK, nn20clock_timezones_format_offset(-210, out,
                                                       sizeof(out)));
    CHECK_STR_EQ("UTC-3:30", out);

    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_timezones_format_offset(0, out, sizeof(out) - 1u));
    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_timezones_format_offset(0, NULL, sizeof(out)));
}

TEST_MAIN("nn20clock_timefmt")
{
    RUN(hhmm_is_zero_padded_and_24_hour);
    RUN(an_impossible_time_shows_placeholders);
    RUN(hhmm_rejects_a_short_buffer);
    RUN(the_date_is_iso_ordered);
    RUN(weekday_zero_is_monday);
    RUN(every_zone_has_the_offset_it_claims);
    RUN(new_york_changes_on_the_right_morning);
    RUN(the_storage_default_is_on_the_list);
    RUN(offsets_read_as_people_write_them);
}
