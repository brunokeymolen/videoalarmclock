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
 * Component tests for the alarm model (design 10).
 *
 * This is the suite that matters most in the whole project. Everything
 * else being wrong shows up as a visibly broken clock; the firing rules
 * being wrong shows up as an alarm that does not go off, once, at 7am,
 * months from now. So the awkward cases - a skipped second, a clock
 * step, a DST change, a leap day - are all here, and they run in
 * milliseconds because none of it touches a real clock.
 */
#include "test_util.h"

#include "nn20clock_alarm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Every case runs in UTC unless it is explicitly about a timezone, so
 * the expected instants are arithmetic rather than a property of the
 * machine the tests happen to run on. */
static void use_timezone(const char *tz)
{
    setenv("TZ", tz, 1);
    tzset();
}

/* Build a local instant, the same way a person setting an alarm thinks
 * about it: this date, that wall-clock time. */
static time_t local_at(int year, int month, int day, int hour, int minute,
                       int second)
{
    struct tm when = {0};
    when.tm_year = year - 1900;
    when.tm_mon = month - 1;
    when.tm_mday = day;
    when.tm_hour = hour;
    when.tm_min = minute;
    when.tm_sec = second;
    when.tm_isdst = -1;
    return mktime(&when);
}

static NN20ClockAlarmConfig recurrent_at(uint8_t hour, uint8_t minute,
                                         uint8_t weekdays)
{
    NN20ClockAlarmConfig alarm;
    (void)nn20clock_alarm_defaults(&alarm);
    alarm.id = 1u;
    alarm.hour = hour;
    alarm.minute = minute;
    alarm.weekdays_mask = weekdays;
    return alarm;
}

static NN20ClockAlarmConfig one_off_at(int year, int month, int day,
                                       uint8_t hour, uint8_t minute)
{
    NN20ClockAlarmConfig alarm;
    (void)nn20clock_alarm_defaults(&alarm);
    alarm.id = 1u;
    alarm.kind = NN20CLOCK_ALARM_KIND_ONE_OFF;
    alarm.weekdays_mask = 0u;
    alarm.one_off_date.year = (uint16_t)year;
    alarm.one_off_date.month = (uint8_t)month;
    alarm.one_off_date.day = (uint8_t)day;
    alarm.hour = hour;
    alarm.minute = minute;
    return alarm;
}

/* ------------------------------------------------------- validation -- */

TEST(defaults_are_valid_and_sensible)
{
    NN20ClockAlarmConfig alarm;
    REQUIRE(nn20clock_alarm_defaults(&alarm) == ESP_OK);

    CHECK_EQ(ESP_OK, nn20clock_alarm_validate(&alarm));
    CHECK(alarm.enabled);
    CHECK_EQ(NN20CLOCK_ALARM_KIND_RECURRENT, alarm.kind);
    CHECK_EQ(7, alarm.hour);
    CHECK_EQ(NN20CLOCK_ALARM_WEEKDAYS, alarm.weekdays_mask);
    CHECK_EQ(NN20CLOCK_ALARM_ID_NONE, alarm.id);

    CHECK_EQ(ESP_ERR_INVALID_ARG, nn20clock_alarm_defaults(NULL));
}

TEST(validation_rejects_impossible_times)
{
    NN20ClockAlarmConfig alarm = recurrent_at(24u, 0u, NN20CLOCK_ALARM_MONDAY);
    CHECK_EQ(ESP_ERR_INVALID_ARG, nn20clock_alarm_validate(&alarm));

    alarm = recurrent_at(12u, 60u, NN20CLOCK_ALARM_MONDAY);
    CHECK_EQ(ESP_ERR_INVALID_ARG, nn20clock_alarm_validate(&alarm));

    CHECK_EQ(ESP_ERR_INVALID_ARG, nn20clock_alarm_validate(NULL));
}

/* A recurrent alarm with no days never fires. Storing one is a silent
 * way to have no alarm at all. */
TEST(a_recurrent_alarm_needs_at_least_one_weekday)
{
    NN20ClockAlarmConfig alarm = recurrent_at(7u, 0u, 0u);
    CHECK_EQ(ESP_ERR_INVALID_ARG, nn20clock_alarm_validate(&alarm));

    alarm.weekdays_mask = NN20CLOCK_ALARM_SUNDAY;
    CHECK_EQ(ESP_OK, nn20clock_alarm_validate(&alarm));
}

TEST(a_one_off_needs_a_date_and_no_weekdays)
{
    NN20ClockAlarmConfig alarm = one_off_at(2026, 8, 28, 7u, 0u);
    CHECK_EQ(ESP_OK, nn20clock_alarm_validate(&alarm));

    /* Design 10: a one-off's weekday mask must be zero, so the record
     * has exactly one meaning. */
    alarm.weekdays_mask = NN20CLOCK_ALARM_MONDAY;
    CHECK_EQ(ESP_ERR_INVALID_ARG, nn20clock_alarm_validate(&alarm));

    alarm = one_off_at(2026, 13, 1, 7u, 0u);
    CHECK_EQ(ESP_ERR_INVALID_ARG, nn20clock_alarm_validate(&alarm));

    alarm = one_off_at(2026, 8, 0, 7u, 0u);
    CHECK_EQ(ESP_ERR_INVALID_ARG, nn20clock_alarm_validate(&alarm));
}

TEST(validation_rejects_unterminated_media_strings)
{
    NN20ClockAlarmConfig alarm = recurrent_at(7u, 0u, NN20CLOCK_ALARM_MONDAY);
    memset(alarm.media_path, 'x', sizeof(alarm.media_path));
    CHECK_EQ(ESP_ERR_INVALID_ARG, nn20clock_alarm_validate(&alarm));
}

/*
 * The firing path switches on the media mode to decide whether
 * media_path names the file or is to be ignored, so a value from
 * neither branch would silently take one of them.
 */
TEST(validation_accepts_every_media_mode_and_nothing_else)
{
    NN20ClockAlarmConfig alarm = recurrent_at(7u, 0u, NN20CLOCK_ALARM_MONDAY);

    alarm.media_mode = NN20CLOCK_ALARM_MEDIA_VIDEO;
    CHECK_EQ(ESP_OK, nn20clock_alarm_validate(&alarm));
    alarm.media_mode = NN20CLOCK_ALARM_MEDIA_AUDIO;
    CHECK_EQ(ESP_OK, nn20clock_alarm_validate(&alarm));

    /* A random alarm names no file, and that is not a reason to
     * refuse it: which film is chosen when it goes off, not here. */
    alarm.media_mode = NN20CLOCK_ALARM_MEDIA_RANDOM_SET;
    alarm.media_path[0] = '\0';
    alarm.media_set_id[0] = '\0';
    CHECK_EQ(ESP_OK, nn20clock_alarm_validate(&alarm));

    alarm.media_mode = (NN20ClockAlarmMediaMode)7;
    CHECK_EQ(ESP_ERR_INVALID_ARG, nn20clock_alarm_validate(&alarm));
}

/* ------------------------------------------------- media and folders -- */

/* Set the alarm's media the way the editor does, and validate it. */
static esp_err_t validate_with_file(const char *path)
{
    NN20ClockAlarmConfig alarm = recurrent_at(7u, 0u, NN20CLOCK_ALARM_MONDAY);
    alarm.media_mode = NN20CLOCK_ALARM_MEDIA_AUDIO;
    snprintf(alarm.media_path, sizeof(alarm.media_path), "%s", path);
    alarm.media_set_id[0] = '\0';
    return nn20clock_alarm_validate(&alarm);
}

static esp_err_t validate_with_folder(const char *folder)
{
    NN20ClockAlarmConfig alarm = recurrent_at(7u, 0u, NN20CLOCK_ALARM_MONDAY);
    alarm.media_mode = NN20CLOCK_ALARM_MEDIA_RANDOM_SET;
    alarm.media_path[0] = '\0';
    snprintf(alarm.media_set_id, sizeof(alarm.media_set_id), "%s", folder);
    return nn20clock_alarm_validate(&alarm);
}

TEST(an_alarm_may_name_a_clip_in_the_root_or_in_a_folder)
{
    /* Empty is the built-in tone, which is not a path at all and is
     * always available. */
    CHECK_EQ(ESP_OK, validate_with_file(""));

    CHECK_EQ(ESP_OK, validate_with_file("wake.avi"));
    CHECK_EQ(ESP_OK, validate_with_file("morning/wake.avi"));
    CHECK_EQ(ESP_OK, validate_with_file("weekend/kids/song.avi"));
}

TEST(an_alarm_may_choose_at_random_from_any_folder)
{
    /* "" is the root folder - which is also what an alarm stored before
     * folders existed carries, so those keep meaning "the root of the
     * card" rather than becoming invalid. */
    CHECK_EQ(ESP_OK, validate_with_folder(""));

    CHECK_EQ(ESP_OK, validate_with_folder("morning"));
    CHECK_EQ(ESP_OK, validate_with_folder("weekend/kids"));
}

TEST(validation_refuses_media_paths_that_leave_the_card)
{
    /* The same rules the card and the FTP server enforce: an alarm that
     * stored something those two would refuse is an alarm that rings
     * the tone for a reason nobody can see. */
    CHECK_EQ(ESP_ERR_INVALID_ARG, validate_with_file("../escape.avi"));
    CHECK_EQ(ESP_ERR_INVALID_ARG, validate_with_file("/sdcard/wake.avi"));
    CHECK_EQ(ESP_ERR_INVALID_ARG, validate_with_file("morning/../../nvs"));
    CHECK_EQ(ESP_ERR_INVALID_ARG, validate_with_file("morning//wake.avi"));
    CHECK_EQ(ESP_ERR_INVALID_ARG, validate_with_file("morning\\wake.avi"));
    CHECK_EQ(ESP_ERR_INVALID_ARG, validate_with_file("morning/"));

    CHECK_EQ(ESP_ERR_INVALID_ARG, validate_with_folder(".."));
    CHECK_EQ(ESP_ERR_INVALID_ARG, validate_with_folder("/morning"));
    CHECK_EQ(ESP_ERR_INVALID_ARG, validate_with_folder("morning/"));
    CHECK_EQ(ESP_ERR_INVALID_ARG, validate_with_folder("morning/../.."));
}

TEST(the_ignored_media_field_is_left_alone)
{
    /*
     * Which field means anything follows the mode. The editor clears
     * the other one, but a record that carries a stale value in a field
     * the scheduler will not read is still a record the scheduler can
     * evaluate - refusing it would turn a cosmetic leftover into an
     * alarm that cannot be loaded.
     */
    NN20ClockAlarmConfig alarm = recurrent_at(7u, 0u, NN20CLOCK_ALARM_MONDAY);

    alarm.media_mode = NN20CLOCK_ALARM_MEDIA_RANDOM_SET;
    snprintf(alarm.media_set_id, sizeof(alarm.media_set_id), "morning");
    snprintf(alarm.media_path, sizeof(alarm.media_path), "../stale");
    CHECK_EQ(ESP_OK, nn20clock_alarm_validate(&alarm));

    alarm.media_mode = NN20CLOCK_ALARM_MEDIA_AUDIO;
    snprintf(alarm.media_path, sizeof(alarm.media_path), "wake.avi");
    snprintf(alarm.media_set_id, sizeof(alarm.media_set_id), "../stale");
    CHECK_EQ(ESP_OK, nn20clock_alarm_validate(&alarm));
}

/* ---------------------------------------------------- next occurrence -- */

TEST(the_next_occurrence_is_today_when_the_time_is_still_ahead)
{
    use_timezone("UTC0");
    /* 2026-08-28 is a Friday. */
    NN20ClockAlarmConfig alarm = recurrent_at(7u, 0u, NN20CLOCK_ALARM_FRIDAY);

    time_t when = 0;
    REQUIRE(nn20clock_alarm_next_occurrence(
                &alarm, local_at(2026, 8, 28, 6, 0, 0), &when) == ESP_OK);
    CHECK_EQ(local_at(2026, 8, 28, 7, 0, 0), when);
}

TEST(the_next_occurrence_skips_to_next_week_once_today_has_passed)
{
    use_timezone("UTC0");
    NN20ClockAlarmConfig alarm = recurrent_at(7u, 0u, NN20CLOCK_ALARM_FRIDAY);

    time_t when = 0;
    REQUIRE(nn20clock_alarm_next_occurrence(
                &alarm, local_at(2026, 8, 28, 8, 0, 0), &when) == ESP_OK);
    CHECK_EQ(local_at(2026, 9, 4, 7, 0, 0), when);   /* the next Friday */
}

TEST(a_disabled_alarm_never_occurs)
{
    use_timezone("UTC0");
    NN20ClockAlarmConfig alarm = recurrent_at(7u, 0u, NN20CLOCK_ALARM_EVERY_DAY);
    alarm.enabled = false;

    time_t when = 1;
    REQUIRE(nn20clock_alarm_next_occurrence(
                &alarm, local_at(2026, 8, 28, 6, 0, 0), &when) == ESP_OK);
    CHECK_EQ(0, when);
}

TEST(a_one_off_in_the_past_has_no_next_occurrence)
{
    use_timezone("UTC0");
    NN20ClockAlarmConfig alarm = one_off_at(2026, 8, 28, 7u, 0u);

    time_t when = 1;
    REQUIRE(nn20clock_alarm_next_occurrence(
                &alarm, local_at(2026, 8, 28, 8, 0, 0), &when) == ESP_OK);
    CHECK_EQ(0, when);

    REQUIRE(nn20clock_alarm_next_occurrence(
                &alarm, local_at(2026, 8, 28, 6, 0, 0), &when) == ESP_OK);
    CHECK_EQ(local_at(2026, 8, 28, 7, 0, 0), when);
}

/* --------------------------------------------- the interval, and why -- */

/*
 * The case this whole design exists for. The clock is polled, and a
 * poll can be late: the reading goes 01:01:59 then 01:02:01, and
 * 01:02:00 is never observed. Matching an instant would lose the alarm
 * silently; matching the interval finds it.
 *
 * Note the granularity, which is easy to get wrong in both directions:
 * an alarm is set to a minute (design 10 stores hour and minute, so it
 * occurs at second 0), while the scan runs at second resolution
 * (design 7). So the second that gets skipped is the alarm's own
 * :00, not some second within the minute.
 */
TEST(an_alarm_in_a_skipped_second_still_fires)
{
    use_timezone("UTC0");
    NN20ClockAlarmConfig alarm = recurrent_at(1u, 2u, NN20CLOCK_ALARM_EVERY_DAY);

    const time_t before = local_at(2026, 8, 28, 1, 1, 59);
    const time_t after = local_at(2026, 8, 28, 1, 2, 1);

    time_t when = 0;
    CHECK(nn20clock_alarm_occurs_in(&alarm, before, after, &when));
    CHECK_EQ(local_at(2026, 8, 28, 1, 2, 0), when);

    /* And a skip that lands entirely inside the minute, after the
     * alarm's instant, must NOT fire it a second time. */
    CHECK(!nn20clock_alarm_occurs_in(&alarm, local_at(2026, 8, 28, 1, 2, 1),
                                     local_at(2026, 8, 28, 1, 2, 30), &when));
}

/* The interval is half-open at the bottom: `after` was covered by the
 * previous tick. Without that, an alarm exactly on a tick boundary
 * fires twice. */
TEST(an_alarm_exactly_on_the_lower_bound_does_not_fire_again)
{
    use_timezone("UTC0");
    NN20ClockAlarmConfig alarm = recurrent_at(7u, 0u, NN20CLOCK_ALARM_EVERY_DAY);

    const time_t fired_at = local_at(2026, 8, 28, 7, 0, 0);

    /* The tick that fired it: the instant is the upper bound. */
    time_t when = 0;
    CHECK(nn20clock_alarm_occurs_in(&alarm, fired_at - 1, fired_at, &when));
    CHECK_EQ(fired_at, when);

    /* The next tick starts where that one ended. The same instant must
     * not be found again. */
    CHECK(!nn20clock_alarm_occurs_in(&alarm, fired_at, fired_at + 1, &when));
}

/* Consecutive ticks must cover every instant exactly once - no gap
 * between them where an alarm could hide. */
TEST(consecutive_intervals_fire_each_alarm_exactly_once)
{
    use_timezone("UTC0");
    NN20ClockAlarmConfig alarm = recurrent_at(7u, 0u, NN20CLOCK_ALARM_EVERY_DAY);

    const time_t start = local_at(2026, 8, 28, 6, 59, 55);
    int fires = 0;

    /* Walk a second at a time across the alarm, the way the tick does. */
    for (int i = 0; i < 20; i++) {
        time_t when = 0;
        if (nn20clock_alarm_occurs_in(&alarm, start + i, start + i + 1,
                                      &when)) {
            fires++;
            CHECK_EQ(local_at(2026, 8, 28, 7, 0, 0), when);
        }
    }
    CHECK_EQ(1, fires);
}

/* Same walk, but in ragged steps - which is what a real tick looks like
 * when the worker is busy. Still exactly once. */
TEST(ragged_intervals_still_fire_exactly_once)
{
    use_timezone("UTC0");
    NN20ClockAlarmConfig alarm = recurrent_at(7u, 0u, NN20CLOCK_ALARM_EVERY_DAY);

    const int steps[] = { 3, 1, 4, 1, 5, 2, 6 };   /* seconds per tick */
    time_t cursor = local_at(2026, 8, 28, 6, 59, 50);
    int fires = 0;

    for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
        const time_t next = cursor + steps[i];
        time_t when = 0;
        if (nn20clock_alarm_occurs_in(&alarm, cursor, next, &when)) {
            fires++;
            CHECK_EQ(local_at(2026, 8, 28, 7, 0, 0), when);
        }
        cursor = next;
    }
    CHECK_EQ(1, fires);
}

/* A clock correction that moves time backwards is not elapsed time, and
 * must not fire anything. */
TEST(a_backwards_interval_fires_nothing)
{
    use_timezone("UTC0");
    NN20ClockAlarmConfig alarm = recurrent_at(7u, 0u, NN20CLOCK_ALARM_EVERY_DAY);

    time_t when = 0;
    CHECK(!nn20clock_alarm_occurs_in(&alarm, local_at(2026, 8, 28, 8, 0, 0),
                                     local_at(2026, 8, 28, 6, 0, 0), &when));
    /* An empty interval is not an occurrence either. */
    const time_t same = local_at(2026, 8, 28, 7, 0, 0);
    CHECK(!nn20clock_alarm_occurs_in(&alarm, same, same, &when));
}

/* A long interval can contain more than one occurrence. The earliest is
 * reported, because it is the one whose lateness the caller judges. */
TEST(a_long_interval_reports_the_earliest_occurrence)
{
    use_timezone("UTC0");
    NN20ClockAlarmConfig alarm = recurrent_at(7u, 0u, NN20CLOCK_ALARM_EVERY_DAY);

    time_t when = 0;
    CHECK(nn20clock_alarm_occurs_in(&alarm, local_at(2026, 8, 27, 6, 0, 0),
                                    local_at(2026, 8, 29, 12, 0, 0), &when));
    CHECK_EQ(local_at(2026, 8, 27, 7, 0, 0), when);
}

TEST(occurs_in_tolerates_null)
{
    time_t when = 0;
    NN20ClockAlarmConfig alarm = recurrent_at(7u, 0u, NN20CLOCK_ALARM_EVERY_DAY);
    CHECK(!nn20clock_alarm_occurs_in(NULL, 0, 1, &when));
    CHECK(!nn20clock_alarm_occurs_in(&alarm, 0, 1, NULL));
    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_alarm_next_occurrence(NULL, 0, &when));
    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_alarm_next_occurrence(&alarm, 0, NULL));
}

/* ------------------------------------------------------------- time -- */

/*
 * An alarm is a wall-clock time, not an offset. Across a DST change the
 * day is 23 or 25 hours long, and 07:00 must still mean 07:00 - which
 * is why occurrences are built through local broken-down time rather
 * than by adding 86400.
 */
TEST(an_alarm_keeps_its_wall_clock_time_across_dst)
{
    /* Europe/Brussels: clocks go forward on the last Sunday of March. */
    use_timezone("CET-1CEST,M3.5.0,M10.5.0/3");

    NN20ClockAlarmConfig alarm = recurrent_at(7u, 0u, NN20CLOCK_ALARM_EVERY_DAY);

    /* 2026-03-29 is the spring-forward day. */
    time_t when = 0;
    REQUIRE(nn20clock_alarm_next_occurrence(
                &alarm, local_at(2026, 3, 28, 12, 0, 0), &when) == ESP_OK);
    CHECK_EQ(local_at(2026, 3, 29, 7, 0, 0), when);

    /* And the day is genuinely not 86400 seconds long, which is exactly
     * what would have broken a naive implementation. */
    const time_t before = local_at(2026, 3, 28, 7, 0, 0);
    const time_t across = local_at(2026, 3, 29, 7, 0, 0);
    CHECK_EQ(23 * 60 * 60, (long long)(across - before));

    use_timezone("UTC0");
}

TEST(an_alarm_survives_a_leap_day_and_a_year_boundary)
{
    use_timezone("UTC0");

    /* 2028 is a leap year; 2028-02-29 is a Tuesday. */
    NN20ClockAlarmConfig alarm = recurrent_at(7u, 0u, NN20CLOCK_ALARM_TUESDAY);
    time_t when = 0;
    REQUIRE(nn20clock_alarm_next_occurrence(
                &alarm, local_at(2028, 2, 28, 12, 0, 0), &when) == ESP_OK);
    CHECK_EQ(local_at(2028, 2, 29, 7, 0, 0), when);

    /* New year's eve into new year's day. */
    NN20ClockAlarmConfig daily = recurrent_at(0u, 30u,
                                              NN20CLOCK_ALARM_EVERY_DAY);
    REQUIRE(nn20clock_alarm_next_occurrence(
                &daily, local_at(2026, 12, 31, 23, 0, 0), &when) == ESP_OK);
    CHECK_EQ(local_at(2027, 1, 1, 0, 30, 0), when);
}

TEST(the_weekday_mask_counts_from_monday)
{
    use_timezone("UTC0");

    /* 2026-08-31 is a Monday. An alarm set for Mondays only must fire
     * on it and not on the Sunday before. */
    NN20ClockAlarmConfig alarm = recurrent_at(7u, 0u, NN20CLOCK_ALARM_MONDAY);

    time_t when = 0;
    REQUIRE(nn20clock_alarm_next_occurrence(
                &alarm, local_at(2026, 8, 30, 0, 0, 0), &when) == ESP_OK);
    CHECK_EQ(local_at(2026, 8, 31, 7, 0, 0), when);

    NN20ClockAlarmConfig sunday = recurrent_at(7u, 0u, NN20CLOCK_ALARM_SUNDAY);
    REQUIRE(nn20clock_alarm_next_occurrence(
                &sunday, local_at(2026, 8, 30, 0, 0, 0), &when) == ESP_OK);
    CHECK_EQ(local_at(2026, 8, 30, 7, 0, 0), when);
}

TEST(weekend_and_weekday_masks_select_the_right_days)
{
    use_timezone("UTC0");

    /* Friday 2026-08-28: a Mon-Fri alarm fires today, a weekend one
     * waits for Saturday. */
    NN20ClockAlarmConfig weekdays = recurrent_at(7u, 0u,
                                                 NN20CLOCK_ALARM_WEEKDAYS);
    NN20ClockAlarmConfig weekend =
        recurrent_at(7u, 0u, NN20CLOCK_ALARM_SATURDAY | NN20CLOCK_ALARM_SUNDAY);

    time_t when = 0;
    REQUIRE(nn20clock_alarm_next_occurrence(
                &weekdays, local_at(2026, 8, 28, 6, 0, 0), &when) == ESP_OK);
    CHECK_EQ(local_at(2026, 8, 28, 7, 0, 0), when);

    REQUIRE(nn20clock_alarm_next_occurrence(
                &weekend, local_at(2026, 8, 28, 6, 0, 0), &when) == ESP_OK);
    CHECK_EQ(local_at(2026, 8, 29, 7, 0, 0), when);
}

/* ------------------------------------------------------------- list -- */

/*
 * A note on `static` below. An NN20ClockAlarmList is over 5 KB, which is
 * more than the stack of the task these run on when the suite runs on
 * the board - a plain local reset it with a stack protection fault.
 * Static gives the storage a fixed home instead. Tests run one at a
 * time, so sharing is safe, but each case that reuses one must clear it.
 */


TEST(putting_an_alarm_assigns_the_lowest_free_id)
{
    static NN20ClockAlarmList list;
    memset(&list, 0, sizeof(list));
    NN20ClockAlarmConfig alarm = recurrent_at(7u, 0u, NN20CLOCK_ALARM_MONDAY);
    alarm.id = NN20CLOCK_ALARM_ID_NONE;

    uint32_t id = 0;
    CHECK_EQ(ESP_OK, nn20clock_alarm_list_put(&list, &alarm, &id));
    CHECK_EQ(1, id);
    CHECK_EQ(1, list.count);

    CHECK_EQ(ESP_OK, nn20clock_alarm_list_put(&list, &alarm, &id));
    CHECK_EQ(2, id);
    CHECK_EQ(2, list.count);

    /* Removing the first frees its id for reuse, so ids stay small. */
    CHECK_EQ(ESP_OK, nn20clock_alarm_list_remove(&list, 1u));
    CHECK_EQ(ESP_OK, nn20clock_alarm_list_put(&list, &alarm, &id));
    CHECK_EQ(1, id);
}

TEST(putting_an_existing_id_replaces_rather_than_duplicates)
{
    static NN20ClockAlarmList list;
    memset(&list, 0, sizeof(list));
    NN20ClockAlarmConfig alarm = recurrent_at(7u, 0u, NN20CLOCK_ALARM_MONDAY);
    alarm.id = NN20CLOCK_ALARM_ID_NONE;

    uint32_t id = 0;
    REQUIRE(nn20clock_alarm_list_put(&list, &alarm, &id) == ESP_OK);

    alarm.id = id;
    alarm.hour = 9u;
    CHECK_EQ(ESP_OK, nn20clock_alarm_list_put(&list, &alarm, NULL));
    CHECK_EQ(1, list.count);

    size_t index = 0;
    REQUIRE(nn20clock_alarm_list_find(&list, id, &index) == ESP_OK);
    CHECK_EQ(9, list.alarms[index].hour);
}

TEST(an_invalid_alarm_is_refused_by_the_list)
{
    static NN20ClockAlarmList list;
    memset(&list, 0, sizeof(list));
    NN20ClockAlarmConfig alarm = recurrent_at(25u, 0u, NN20CLOCK_ALARM_MONDAY);

    CHECK_EQ(ESP_ERR_INVALID_ARG, nn20clock_alarm_list_put(&list, &alarm,
                                                           NULL));
    CHECK_EQ(0, list.count);
}

TEST(the_list_fills_up_and_says_so)
{
    static NN20ClockAlarmList list;
    memset(&list, 0, sizeof(list));
    NN20ClockAlarmConfig alarm = recurrent_at(7u, 0u, NN20CLOCK_ALARM_MONDAY);

    for (int i = 0; i < NN20CLOCK_ALARM_MAX; i++) {
        alarm.id = NN20CLOCK_ALARM_ID_NONE;
        REQUIRE(nn20clock_alarm_list_put(&list, &alarm, NULL) == ESP_OK);
    }
    alarm.id = NN20CLOCK_ALARM_ID_NONE;
    CHECK_EQ(ESP_ERR_NO_MEM, nn20clock_alarm_list_put(&list, &alarm, NULL));
}

TEST(removing_and_finding_report_missing_ids)
{
    static NN20ClockAlarmList list;
    memset(&list, 0, sizeof(list));
    CHECK_EQ(ESP_ERR_NOT_FOUND, nn20clock_alarm_list_remove(&list, 7u));
    CHECK_EQ(ESP_ERR_NOT_FOUND, nn20clock_alarm_list_find(&list, 7u, NULL));
    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_alarm_list_find(&list, NN20CLOCK_ALARM_ID_NONE, NULL));
}

TEST(the_list_reports_the_earliest_alarm_in_the_interval)
{
    use_timezone("UTC0");
    static NN20ClockAlarmList list;
    memset(&list, 0, sizeof(list));

    NN20ClockAlarmConfig seven = recurrent_at(7u, 0u,
                                              NN20CLOCK_ALARM_EVERY_DAY);
    seven.id = NN20CLOCK_ALARM_ID_NONE;
    NN20ClockAlarmConfig six = recurrent_at(6u, 30u, NN20CLOCK_ALARM_EVERY_DAY);
    six.id = NN20CLOCK_ALARM_ID_NONE;

    uint32_t seven_id = 0;
    uint32_t six_id = 0;
    REQUIRE(nn20clock_alarm_list_put(&list, &seven, &seven_id) == ESP_OK);
    REQUIRE(nn20clock_alarm_list_put(&list, &six, &six_id) == ESP_OK);

    uint32_t fired = 0;
    time_t when = 0;
    CHECK(nn20clock_alarm_list_first_in(&list, local_at(2026, 8, 28, 6, 0, 0),
                                        local_at(2026, 8, 28, 8, 0, 0),
                                        &fired, &when));
    /* 06:30 comes before 07:00, whatever order they were added in. */
    CHECK_EQ(six_id, fired);
    CHECK_EQ(local_at(2026, 8, 28, 6, 30, 0), when);
}

TEST(a_disabled_alarm_is_skipped_by_the_list)
{
    use_timezone("UTC0");
    static NN20ClockAlarmList list;
    memset(&list, 0, sizeof(list));

    NN20ClockAlarmConfig early = recurrent_at(6u, 30u,
                                              NN20CLOCK_ALARM_EVERY_DAY);
    early.id = NN20CLOCK_ALARM_ID_NONE;
    early.enabled = false;
    NN20ClockAlarmConfig late = recurrent_at(7u, 0u, NN20CLOCK_ALARM_EVERY_DAY);
    late.id = NN20CLOCK_ALARM_ID_NONE;

    REQUIRE(nn20clock_alarm_list_put(&list, &early, NULL) == ESP_OK);
    uint32_t late_id = 0;
    REQUIRE(nn20clock_alarm_list_put(&list, &late, &late_id) == ESP_OK);

    uint32_t fired = 0;
    time_t when = 0;
    CHECK(nn20clock_alarm_list_first_in(&list, local_at(2026, 8, 28, 6, 0, 0),
                                        local_at(2026, 8, 28, 8, 0, 0),
                                        &fired, &when));
    CHECK_EQ(late_id, fired);
}

TEST(an_empty_list_fires_nothing)
{
    static NN20ClockAlarmList list;
    memset(&list, 0, sizeof(list));
    uint32_t fired = 0;
    time_t when = 0;
    CHECK(!nn20clock_alarm_list_first_in(&list, 0, 100000, &fired, &when));
    CHECK(!nn20clock_alarm_list_first_in(NULL, 0, 1, &fired, &when));
}

TEST_MAIN("nn20clock_alarm")
{
    RUN(defaults_are_valid_and_sensible);
    RUN(validation_rejects_impossible_times);
    RUN(a_recurrent_alarm_needs_at_least_one_weekday);
    RUN(a_one_off_needs_a_date_and_no_weekdays);
    RUN(validation_rejects_unterminated_media_strings);
    RUN(validation_accepts_every_media_mode_and_nothing_else);
    RUN(an_alarm_may_name_a_clip_in_the_root_or_in_a_folder);
    RUN(an_alarm_may_choose_at_random_from_any_folder);
    RUN(validation_refuses_media_paths_that_leave_the_card);
    RUN(the_ignored_media_field_is_left_alone);

    RUN(the_next_occurrence_is_today_when_the_time_is_still_ahead);
    RUN(the_next_occurrence_skips_to_next_week_once_today_has_passed);
    RUN(a_disabled_alarm_never_occurs);
    RUN(a_one_off_in_the_past_has_no_next_occurrence);

    RUN(an_alarm_in_a_skipped_second_still_fires);
    RUN(an_alarm_exactly_on_the_lower_bound_does_not_fire_again);
    RUN(consecutive_intervals_fire_each_alarm_exactly_once);
    RUN(ragged_intervals_still_fire_exactly_once);
    RUN(a_backwards_interval_fires_nothing);
    RUN(a_long_interval_reports_the_earliest_occurrence);
    RUN(occurs_in_tolerates_null);

    RUN(an_alarm_keeps_its_wall_clock_time_across_dst);
    RUN(an_alarm_survives_a_leap_day_and_a_year_boundary);
    RUN(the_weekday_mask_counts_from_monday);
    RUN(weekend_and_weekday_masks_select_the_right_days);

    RUN(putting_an_alarm_assigns_the_lowest_free_id);
    RUN(putting_an_existing_id_replaces_rather_than_duplicates);
    RUN(an_invalid_alarm_is_refused_by_the_list);
    RUN(the_list_fills_up_and_says_so);
    RUN(removing_and_finding_report_missing_ids);
    RUN(the_list_reports_the_earliest_alarm_in_the_interval);
    RUN(a_disabled_alarm_is_skipped_by_the_list);
    RUN(an_empty_list_fires_nothing);
}
