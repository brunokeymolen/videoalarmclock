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
 * Component tests for the Timer's public API (design 6, 7).
 *
 * Track 1 scope: construction, worker lifecycle, subscriber bookkeeping,
 * and the not-implemented-yet contract. There is deliberately nothing
 * here about ticking, time sources, or alarms - the component does not
 * do those yet, and a test that pretended otherwise would be a lie.
 *
 * These build on the host and on the target from the same source; see
 * test/README.md.
 */
#include "test_util.h"

#include "nn20clock_timer.h"

#include <stdlib.h>
#include <time.h>

/* A subscriber that just counts what it is handed. */
typedef struct {
    int calls;
    NN20ClockTimerEventType last_type;
} CountingSink;

static void counting_sink_on_event(const NN20ClockTimerEvent *event,
                                   void *user_data)
{
    CountingSink *sink = user_data;
    sink->calls++;
    sink->last_type = event->type;
}

static NN20ClockTimerSubscriber make_subscriber(CountingSink *sink)
{
    NN20ClockTimerSubscriber subscriber = {
        .on_event = counting_sink_on_event,
        .user_data = sink,
    };
    return subscriber;
}

/* --------------------------------------------------------- lifecycle -- */

/* Storage is not implemented in Track 1, so the ctor must tolerate NULL;
 * every other test here depends on that. */
TEST(ctor_accepts_a_null_storage)
{
    NN20ClockTimer *timer = nn20clock_timer_ctor(NULL);
    REQUIRE(timer != NULL);
    CHECK_EQ(0, nn20clock_timer_subscriber_count(timer));
    CHECK(!nn20clock_timer_is_running(timer));
    nn20clock_timer_dtor(timer);
}

/* The dtor is the one function that gets called on a half-built object,
 * so it has to survive NULL. */
TEST(dtor_tolerates_null)
{
    nn20clock_timer_dtor(NULL);
}

TEST(start_then_stop_round_trips)
{
    NN20ClockTimer *timer = nn20clock_timer_ctor(NULL);
    REQUIRE(timer != NULL);

    CHECK(!nn20clock_timer_is_running(timer));
    CHECK_EQ(ESP_OK, nn20clock_timer_start(timer));
    CHECK(nn20clock_timer_is_running(timer));
    CHECK_EQ(ESP_OK, nn20clock_timer_stop(timer));
    CHECK(!nn20clock_timer_is_running(timer));

    nn20clock_timer_dtor(timer);
}

TEST(start_twice_is_rejected)
{
    NN20ClockTimer *timer = nn20clock_timer_ctor(NULL);
    REQUIRE(timer != NULL);

    CHECK_EQ(ESP_OK, nn20clock_timer_start(timer));
    CHECK_EQ(ESP_ERR_INVALID_STATE, nn20clock_timer_start(timer));
    /* The rejected second start must not have disturbed the first. */
    CHECK(nn20clock_timer_is_running(timer));

    nn20clock_timer_dtor(timer);
}

TEST(stop_without_start_is_rejected)
{
    NN20ClockTimer *timer = nn20clock_timer_ctor(NULL);
    REQUIRE(timer != NULL);

    CHECK_EQ(ESP_ERR_INVALID_STATE, nn20clock_timer_stop(timer));

    nn20clock_timer_dtor(timer);
}

/* The dtor must join the worker thread whether or not _stop() ran. This
 * is the leak/hang that would only ever show up on the target. */
TEST(dtor_while_running_is_safe)
{
    NN20ClockTimer *timer = nn20clock_timer_ctor(NULL);
    REQUIRE(timer != NULL);

    CHECK_EQ(ESP_OK, nn20clock_timer_start(timer));
    nn20clock_timer_dtor(timer);
}

TEST(lifecycle_functions_reject_null)
{
    CHECK_EQ(ESP_ERR_INVALID_ARG, nn20clock_timer_start(NULL));
    CHECK_EQ(ESP_ERR_INVALID_ARG, nn20clock_timer_stop(NULL));
    CHECK(!nn20clock_timer_is_running(NULL));
    CHECK_EQ(0, nn20clock_timer_subscriber_count(NULL));
}

/* ------------------------------------------------------- subscribers -- */

TEST(subscribe_then_unsubscribe_round_trips)
{
    NN20ClockTimer *timer = nn20clock_timer_ctor(NULL);
    REQUIRE(timer != NULL);

    CountingSink sink = {0};
    NN20ClockTimerSubscriber subscriber = make_subscriber(&sink);

    CHECK_EQ(ESP_OK, nn20clock_timer_subscribe(timer, &subscriber));
    CHECK_EQ(1, nn20clock_timer_subscriber_count(timer));
    CHECK_EQ(ESP_OK, nn20clock_timer_unsubscribe(timer, &subscriber));
    CHECK_EQ(0, nn20clock_timer_subscriber_count(timer));

    nn20clock_timer_dtor(timer);
}

TEST(subscribing_twice_is_rejected)
{
    NN20ClockTimer *timer = nn20clock_timer_ctor(NULL);
    REQUIRE(timer != NULL);

    CountingSink sink = {0};
    NN20ClockTimerSubscriber subscriber = make_subscriber(&sink);

    CHECK_EQ(ESP_OK, nn20clock_timer_subscribe(timer, &subscriber));
    CHECK_EQ(ESP_ERR_INVALID_STATE,
             nn20clock_timer_subscribe(timer, &subscriber));
    /* The duplicate must not have been counted. */
    CHECK_EQ(1, nn20clock_timer_subscriber_count(timer));

    nn20clock_timer_dtor(timer);
}

TEST(unsubscribing_a_stranger_is_not_found)
{
    NN20ClockTimer *timer = nn20clock_timer_ctor(NULL);
    REQUIRE(timer != NULL);

    CountingSink sink = {0};
    NN20ClockTimerSubscriber known = make_subscriber(&sink);
    NN20ClockTimerSubscriber stranger = make_subscriber(&sink);

    CHECK_EQ(ESP_OK, nn20clock_timer_subscribe(timer, &known));
    CHECK_EQ(ESP_ERR_NOT_FOUND, nn20clock_timer_unsubscribe(timer, &stranger));
    CHECK_EQ(1, nn20clock_timer_subscriber_count(timer));

    nn20clock_timer_dtor(timer);
}

/* A subscriber with no callback is useless and would be a NULL call at
 * dispatch time, so it must be refused at the door. */
TEST(subscriber_without_a_callback_is_rejected)
{
    NN20ClockTimer *timer = nn20clock_timer_ctor(NULL);
    REQUIRE(timer != NULL);

    NN20ClockTimerSubscriber empty = {0};

    CHECK_EQ(ESP_ERR_INVALID_ARG, nn20clock_timer_subscribe(timer, &empty));
    CHECK_EQ(0, nn20clock_timer_subscriber_count(timer));

    nn20clock_timer_dtor(timer);
}

TEST(subscriber_functions_reject_null)
{
    NN20ClockTimer *timer = nn20clock_timer_ctor(NULL);
    REQUIRE(timer != NULL);

    CountingSink sink = {0};
    NN20ClockTimerSubscriber subscriber = make_subscriber(&sink);

    CHECK_EQ(ESP_ERR_INVALID_ARG, nn20clock_timer_subscribe(NULL, &subscriber));
    CHECK_EQ(ESP_ERR_INVALID_ARG, nn20clock_timer_subscribe(timer, NULL));
    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_timer_unsubscribe(NULL, &subscriber));
    CHECK_EQ(ESP_ERR_INVALID_ARG, nn20clock_timer_unsubscribe(timer, NULL));

    nn20clock_timer_dtor(timer);
}

TEST(the_subscriber_list_fills_up_and_says_so)
{
    NN20ClockTimer *timer = nn20clock_timer_ctor(NULL);
    REQUIRE(timer != NULL);

    CountingSink sink = {0};
    NN20ClockTimerSubscriber subscribers[NN20CLOCK_TIMER_MAX_SUBSCRIBERS + 1];

    for (size_t i = 0; i < NN20CLOCK_TIMER_MAX_SUBSCRIBERS; i++) {
        subscribers[i] = make_subscriber(&sink);
        CHECK_EQ(ESP_OK, nn20clock_timer_subscribe(timer, &subscribers[i]));
    }
    CHECK_EQ(NN20CLOCK_TIMER_MAX_SUBSCRIBERS,
             nn20clock_timer_subscriber_count(timer));

    subscribers[NN20CLOCK_TIMER_MAX_SUBSCRIBERS] = make_subscriber(&sink);
    CHECK_EQ(ESP_ERR_NO_MEM,
             nn20clock_timer_subscribe(
                 timer, &subscribers[NN20CLOCK_TIMER_MAX_SUBSCRIBERS]));

    nn20clock_timer_dtor(timer);
}

/*
 * Unsubscribe fills the hole with the last entry rather than shifting.
 * Removing from the middle must therefore leave every other subscriber
 * still findable - the obvious way to get this wrong is to lose the tail.
 */
TEST(unsubscribing_from_the_middle_keeps_the_others)
{
    NN20ClockTimer *timer = nn20clock_timer_ctor(NULL);
    REQUIRE(timer != NULL);

    CountingSink sink = {0};
    NN20ClockTimerSubscriber a = make_subscriber(&sink);
    NN20ClockTimerSubscriber b = make_subscriber(&sink);
    NN20ClockTimerSubscriber c = make_subscriber(&sink);

    CHECK_EQ(ESP_OK, nn20clock_timer_subscribe(timer, &a));
    CHECK_EQ(ESP_OK, nn20clock_timer_subscribe(timer, &b));
    CHECK_EQ(ESP_OK, nn20clock_timer_subscribe(timer, &c));

    CHECK_EQ(ESP_OK, nn20clock_timer_unsubscribe(timer, &b));
    CHECK_EQ(2, nn20clock_timer_subscriber_count(timer));

    /* Both survivors are still known, and b is genuinely gone. */
    CHECK_EQ(ESP_ERR_NOT_FOUND, nn20clock_timer_unsubscribe(timer, &b));
    CHECK_EQ(ESP_OK, nn20clock_timer_unsubscribe(timer, &a));
    CHECK_EQ(ESP_OK, nn20clock_timer_unsubscribe(timer, &c));
    CHECK_EQ(0, nn20clock_timer_subscriber_count(timer));

    nn20clock_timer_dtor(timer);
}

/* ---------------------------------------------------- shape of design -- */

/*
 * Design 6 carries the displayed time as a time_t - both date and time -
 * so consumers can do alarm matching and snooze arithmetic on it
 * directly. It was a "HHMMSSyyyymmdd" string in an earlier revision of
 * the design; this pins the value semantics that replaced it.
 */
TEST(the_displayed_time_is_arithmetic)
{
    NN20ClockTimerPayload payload = {0};

    /* 60 seconds after an instant is one minute later, which is the whole
     * point of the change: a string could not do this. */
    payload.displayed_time = 1000;
    const time_t a_minute_later = payload.displayed_time + 60;
    CHECK_EQ(1060, a_minute_later);

    /* Signed, so difftime on two payloads can go either way. */
    CHECK(difftime(a_minute_later, payload.displayed_time) > 0.0);
    CHECK(difftime(payload.displayed_time, a_minute_later) < 0.0);
}

/*
 * time_t must be wide enough to still be counting after 2038. A 32-bit
 * time_t would silently wrap and an alarm set past that date would fire
 * at the wrong moment - the kind of bug that only shows up years later.
 */
TEST(time_t_survives_2038)
{
    CHECK(sizeof(time_t) >= 8);
}

/*
 * Design 6 requires every payload to embed NN20ClockTimerPayload as its
 * first field so a derived payload can be passed as the base. That is
 * only safe if the base sits at offset zero.
 */
TEST(derived_payloads_can_be_upcast)
{
    typedef struct {
        NN20ClockTimerPayload super;
        uint32_t alarm_id;
    } DerivedPayload;

    DerivedPayload derived = {0};
    derived.alarm_id = 42;

    CHECK_EQ(0, offsetof(DerivedPayload, super));

    const NN20ClockTimerPayload *base = (const NN20ClockTimerPayload *)&derived;
    CHECK(base == &derived.super);
}

/* Design 10 numbers weekdays from Monday. struct tm numbers them from
 * Sunday. Recording the choice here so a future conversion helper is
 * written against the right one. */
TEST(weekday_zero_is_monday)
{
    NN20ClockDateTime when = {0};
    when.weekday = 0;   /* Monday */

    const uint8_t monday_only = 1u << 0;
    CHECK_EQ(1, (monday_only >> when.weekday) & 1u);

    when.weekday = 6;   /* Sunday */
    CHECK_EQ(0, (monday_only >> when.weekday) & 1u);
}

/* --------------------------------------------------- the tick (M2) --- */

/*
 * A clock the test moves by hand. Every case below sets an instant,
 * polls, and inspects what came out - no sleeping, no real time, and
 * midnight and new year are as easy to reach as any other second.
 */
typedef struct {
    time_t now;
} FakeClock;

static time_t fake_clock_read(void *user_data)
{
    return ((FakeClock *)user_data)->now;
}

/* Events seen, by type, plus the last payload. */
typedef struct {
    int seconds;
    int minutes;
    int synced;
    int errors;
    time_t last_time;
} EventLog;

static void event_log_on_event(const NN20ClockTimerEvent *event,
                               void *user_data)
{
    EventLog *log = user_data;
    log->last_time = event->payload->displayed_time;

    switch (event->type) {
    case NN20CLOCK_TIMER_EVENT_SECOND:      log->seconds++; break;
    case NN20CLOCK_TIMER_EVENT_MINUTE:      log->minutes++; break;
    case NN20CLOCK_TIMER_EVENT_TIME_SYNCED: log->synced++;  break;
    case NN20CLOCK_TIMER_EVENT_ERROR:       log->errors++;  break;
    case NN20CLOCK_TIMER_EVENT_ALARM:       break;
    }
}

/* 2026-08-28 14:30:00 UTC, a Friday. */
#define BASE_TIME ((time_t)1787927400)

typedef struct {
    NN20ClockTimer *timer;
    FakeClock clock;
    EventLog log;
    NN20ClockTimerSubscriber subscriber;
} TickFixture;

/* Every tick test runs in UTC so the expected wall-clock fields are
 * arithmetic rather than a function of where this is built. */
static void tick_fixture_up(TickFixture *fixture)
{
    memset(fixture, 0, sizeof(*fixture));
    fixture->clock.now = BASE_TIME;

    fixture->timer = nn20clock_timer_ctor(NULL);
    if (fixture->timer == NULL) {
        return;
    }
    (void)nn20clock_timer_set_timezone(fixture->timer, "UTC0");
    (void)nn20clock_timer_set_clock_fn(fixture->timer, fake_clock_read,
                                       &fixture->clock);

    fixture->subscriber.on_event = event_log_on_event;
    fixture->subscriber.user_data = &fixture->log;
    (void)nn20clock_timer_subscribe(fixture->timer, &fixture->subscriber);
}

static void tick_fixture_down(TickFixture *fixture)
{
    if (fixture->timer != NULL) {
        (void)nn20clock_timer_unsubscribe(fixture->timer,
                                          &fixture->subscriber);
        nn20clock_timer_dtor(fixture->timer);
    }
}

/* Poll and wait for the worker to have processed it. */
static void poll_and_settle(TickFixture *fixture)
{
    REQUIRE(nn20clock_timer_poll(fixture->timer) == ESP_OK);
    REQUIRE(nn20clock_timer_flush(fixture->timer) == ESP_OK);
}

/* A fresh subscriber must not wait up to a minute to learn the time. */
TEST(starting_emits_the_current_time_at_once)
{
    TickFixture fixture;
    tick_fixture_up(&fixture);
    REQUIRE(fixture.timer != NULL);

    REQUIRE(nn20clock_timer_start(fixture.timer) == ESP_OK);
    REQUIRE(nn20clock_timer_flush(fixture.timer) == ESP_OK);

    CHECK_EQ(1, fixture.log.seconds);
    CHECK_EQ(1, fixture.log.minutes);
    CHECK_EQ(BASE_TIME, fixture.log.last_time);

    tick_fixture_down(&fixture);
}

/* The tick is a poll, so polling more often than the clock moves must
 * not invent events. */
TEST(polling_without_the_clock_moving_emits_nothing)
{
    TickFixture fixture;
    tick_fixture_up(&fixture);
    REQUIRE(fixture.timer != NULL);
    REQUIRE(nn20clock_timer_start(fixture.timer) == ESP_OK);
    REQUIRE(nn20clock_timer_flush(fixture.timer) == ESP_OK);

    const int seconds_after_start = fixture.log.seconds;
    const int minutes_after_start = fixture.log.minutes;

    for (int i = 0; i < 5; i++) {
        poll_and_settle(&fixture);
    }

    CHECK_EQ(seconds_after_start, fixture.log.seconds);
    CHECK_EQ(minutes_after_start, fixture.log.minutes);

    tick_fixture_down(&fixture);
}

TEST(a_second_passing_emits_a_second_event_only)
{
    TickFixture fixture;
    tick_fixture_up(&fixture);
    REQUIRE(fixture.timer != NULL);
    REQUIRE(nn20clock_timer_start(fixture.timer) == ESP_OK);
    REQUIRE(nn20clock_timer_flush(fixture.timer) == ESP_OK);

    const int minutes_after_start = fixture.log.minutes;

    fixture.clock.now = BASE_TIME + 1;
    poll_and_settle(&fixture);

    CHECK_EQ(2, fixture.log.seconds);
    /* Still the same minute: design 11's TimeUi redraws on the minute,
     * so this is the event that must not fire. */
    CHECK_EQ(minutes_after_start, fixture.log.minutes);
    CHECK_EQ(BASE_TIME + 1, fixture.log.last_time);

    tick_fixture_down(&fixture);
}

TEST(crossing_a_minute_emits_both)
{
    TickFixture fixture;
    tick_fixture_up(&fixture);
    REQUIRE(fixture.timer != NULL);
    REQUIRE(nn20clock_timer_start(fixture.timer) == ESP_OK);
    REQUIRE(nn20clock_timer_flush(fixture.timer) == ESP_OK);

    fixture.clock.now = BASE_TIME + 60;
    poll_and_settle(&fixture);

    CHECK_EQ(2, fixture.log.seconds);
    CHECK_EQ(2, fixture.log.minutes);

    tick_fixture_down(&fixture);
}

/* Skipping seconds is normal - a slow poll, a busy worker, or an NTP
 * step. One minute event per observed change, not one per second
 * skipped. */
TEST(a_jump_forward_emits_one_event_of_each)
{
    TickFixture fixture;
    tick_fixture_up(&fixture);
    REQUIRE(fixture.timer != NULL);
    REQUIRE(nn20clock_timer_start(fixture.timer) == ESP_OK);
    REQUIRE(nn20clock_timer_flush(fixture.timer) == ESP_OK);

    fixture.clock.now = BASE_TIME + (3600 * 5);   /* five hours on */
    poll_and_settle(&fixture);

    CHECK_EQ(2, fixture.log.seconds);
    CHECK_EQ(2, fixture.log.minutes);

    tick_fixture_down(&fixture);
}

/* An NTP sync can land on the same second-of-minute it started from.
 * Without the forced emit the display would keep the pre-sync time
 * until the next second ticked - and after a large step, show the wrong
 * hour meanwhile. */
TEST(a_time_sync_forces_the_next_poll_to_report)
{
    TickFixture fixture;
    tick_fixture_up(&fixture);
    REQUIRE(fixture.timer != NULL);
    REQUIRE(nn20clock_timer_start(fixture.timer) == ESP_OK);
    REQUIRE(nn20clock_timer_flush(fixture.timer) == ESP_OK);

    CHECK(!nn20clock_timer_is_time_synced(fixture.timer));

    /* Exactly one hour on: same second, same minute, different hour. */
    fixture.clock.now = BASE_TIME + 3600;
    REQUIRE(nn20clock_timer_notify_time_synced(fixture.timer) == ESP_OK);
    REQUIRE(nn20clock_timer_flush(fixture.timer) == ESP_OK);

    CHECK_EQ(1, fixture.log.synced);
    CHECK(nn20clock_timer_is_time_synced(fixture.timer));

    poll_and_settle(&fixture);
    CHECK_EQ(2, fixture.log.seconds);
    CHECK_EQ(2, fixture.log.minutes);
    CHECK_EQ(BASE_TIME + 3600, fixture.log.last_time);

    tick_fixture_down(&fixture);
}

/* Stopping must leave no callback in flight, and starting again must
 * report the time rather than compare against a stale reading. */
TEST(stop_then_start_reports_the_time_again)
{
    TickFixture fixture;
    tick_fixture_up(&fixture);
    REQUIRE(fixture.timer != NULL);
    REQUIRE(nn20clock_timer_start(fixture.timer) == ESP_OK);
    REQUIRE(nn20clock_timer_flush(fixture.timer) == ESP_OK);
    REQUIRE(nn20clock_timer_stop(fixture.timer) == ESP_OK);

    const int seconds_before = fixture.log.seconds;

    /* Polls while stopped do nothing. */
    poll_and_settle(&fixture);
    CHECK_EQ(seconds_before, fixture.log.seconds);

    REQUIRE(nn20clock_timer_start(fixture.timer) == ESP_OK);
    REQUIRE(nn20clock_timer_flush(fixture.timer) == ESP_OK);
    CHECK_EQ(seconds_before + 1, fixture.log.seconds);

    tick_fixture_down(&fixture);
}

TEST(an_unsubscribed_listener_stops_hearing_events)
{
    TickFixture fixture;
    tick_fixture_up(&fixture);
    REQUIRE(fixture.timer != NULL);
    REQUIRE(nn20clock_timer_start(fixture.timer) == ESP_OK);
    REQUIRE(nn20clock_timer_flush(fixture.timer) == ESP_OK);

    REQUIRE(nn20clock_timer_unsubscribe(fixture.timer, &fixture.subscriber)
            == ESP_OK);
    const int seconds_before = fixture.log.seconds;

    fixture.clock.now = BASE_TIME + 120;
    poll_and_settle(&fixture);
    CHECK_EQ(seconds_before, fixture.log.seconds);

    /* Already unsubscribed; the fixture must not do it twice. */
    fixture.subscriber.on_event = NULL;
    nn20clock_timer_dtor(fixture.timer);
    fixture.timer = NULL;
}

TEST(set_clock_fn_and_timezone_reject_bad_arguments)
{
    NN20ClockTimer *timer = nn20clock_timer_ctor(NULL);
    REQUIRE(timer != NULL);

    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_timer_set_clock_fn(NULL, fake_clock_read, NULL));
    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_timer_set_clock_fn(timer, NULL, NULL));
    CHECK_EQ(ESP_ERR_INVALID_ARG, nn20clock_timer_set_timezone(timer, NULL));
    CHECK_EQ(ESP_ERR_INVALID_ARG, nn20clock_timer_set_timezone(NULL, "UTC0"));

    /* The clock source is worker-owned state; changing it under a
     * running Timer would be a write from the wrong thread. */
    REQUIRE(nn20clock_timer_start(timer) == ESP_OK);
    CHECK_EQ(ESP_ERR_INVALID_STATE,
             nn20clock_timer_set_clock_fn(timer, fake_clock_read, NULL));

    nn20clock_timer_dtor(timer);
}

TEST(poll_and_flush_reject_null)
{
    CHECK_EQ(ESP_ERR_INVALID_ARG, nn20clock_timer_poll(NULL));
    CHECK_EQ(ESP_ERR_INVALID_ARG, nn20clock_timer_flush(NULL));
    CHECK_EQ(ESP_ERR_INVALID_ARG, nn20clock_timer_notify_time_synced(NULL));
    CHECK(!nn20clock_timer_is_time_synced(NULL));
}

/* ---------------------------------------------------- now (M2) ------- */

TEST(get_now_returns_the_current_local_time)
{
    TickFixture fixture;
    tick_fixture_up(&fixture);
    REQUIRE(fixture.timer != NULL);

    NN20ClockDateTime now;
    CHECK_EQ(ESP_OK, nn20clock_timer_get_now(fixture.timer, &now));

    /* BASE_TIME is 2026-08-28 14:30:00 UTC, a Friday. */
    CHECK_EQ(2026, now.year);
    CHECK_EQ(8, now.month);
    CHECK_EQ(28, now.day);
    CHECK_EQ(14, now.hour);
    CHECK_EQ(30, now.minute);
    CHECK_EQ(0, now.second);
    CHECK_EQ(4, now.weekday);   /* 0 = Monday, so Friday is 4 */

    CHECK_EQ(ESP_ERR_INVALID_ARG, nn20clock_timer_get_now(NULL, &now));
    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_timer_get_now(fixture.timer, NULL));

    tick_fixture_down(&fixture);
}

/* The timezone has to reach the conversion, or every displayed time is
 * wrong by an offset nobody notices until they travel. */
TEST(the_timezone_shifts_the_local_time)
{
    TickFixture fixture;
    tick_fixture_up(&fixture);
    REQUIRE(fixture.timer != NULL);

    NN20ClockDateTime utc;
    REQUIRE(nn20clock_timer_get_now(fixture.timer, &utc) == ESP_OK);

    /* Europe/Brussels: UTC+2 in August. */
    REQUIRE(nn20clock_timer_set_timezone(fixture.timer,
                                         "CET-1CEST,M3.5.0,M10.5.0/3")
            == ESP_OK);
    NN20ClockDateTime local;
    REQUIRE(nn20clock_timer_get_now(fixture.timer, &local) == ESP_OK);

    CHECK_EQ(14, utc.hour);
    CHECK_EQ(16, local.hour);
    CHECK_EQ(utc.minute, local.minute);

    /* Leave the process timezone as the other cases expect it. */
    (void)nn20clock_timer_set_timezone(fixture.timer, "UTC0");
    tick_fixture_down(&fixture);
}

/*
 * A new zone moves the hour on the face without moving the instant, and
 * a whole-hour zone does not even move the minute stamp - so without
 * being told, the face would keep the old hour until the minute turned.
 * Changing it on a running Timer must redraw at once, and must not
 * count as the clock having been set.
 */
TEST(a_new_zone_redraws_at_once)
{
    TickFixture fixture;
    tick_fixture_up(&fixture);
    REQUIRE(fixture.timer != NULL);
    REQUIRE(nn20clock_timer_start(fixture.timer) == ESP_OK);
    REQUIRE(nn20clock_timer_flush(fixture.timer) == ESP_OK);

    const int minutes_before = fixture.log.minutes;
    poll_and_settle(&fixture);
    CHECK_EQ(minutes_before, fixture.log.minutes);

    REQUIRE(nn20clock_timer_set_timezone(fixture.timer,
                                         "EST5EDT,M3.2.0,M11.1.0")
            == ESP_OK);
    poll_and_settle(&fixture);
    CHECK_EQ(minutes_before + 1, fixture.log.minutes);
    CHECK(!nn20clock_timer_is_time_synced(fixture.timer));

    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_timer_set_timezone(fixture.timer, ""));

    (void)nn20clock_timer_set_timezone(fixture.timer, "UTC0");
    tick_fixture_down(&fixture);
}

TEST(to_local_is_free_standing)
{
    /* No Timer involved: the conversion reads the process timezone, so
     * the test sets it the same way nn20clock_timer_set_timezone() does. */
    setenv("TZ", "UTC0", 1);
    tzset();

    NN20ClockDateTime dt;
    CHECK_EQ(ESP_OK, nn20clock_timer_to_local(0, &dt));
    CHECK_EQ(1970, dt.year);
    CHECK_EQ(1, dt.month);
    CHECK_EQ(1, dt.day);
    CHECK_EQ(0, dt.hour);
    /* 1970-01-01 was a Thursday: 3 with Monday as 0. */
    CHECK_EQ(3, dt.weekday);

    CHECK_EQ(ESP_ERR_INVALID_ARG, nn20clock_timer_to_local(0, NULL));
}

/* ------------------------------------------------- alarms (M3) ------- */

/*
 * A schedule the test hands to the Timer. Counting the loads is how the
 * reload path gets checked: design 8 has the manager reload after every
 * change, and a reload that quietly does nothing would be invisible.
 */
typedef struct {
    NN20ClockAlarmList list;
    int loads;
    esp_err_t result;
} FakeSchedule;

static esp_err_t fake_alarm_source(NN20ClockAlarmList *out, void *user_data)
{
    FakeSchedule *schedule = user_data;
    schedule->loads++;
    if (schedule->result != ESP_OK) {
        return schedule->result;
    }
    *out = schedule->list;
    return ESP_OK;
}

/* Alarm events, and what they carried. */
typedef struct {
    int fires;
    uint32_t last_id;
    time_t last_scheduled;
    uint32_t last_late;
    /* From the base payload of the last event of ANY type, which is
     * what the clock face reads. */
    bool last_snooze_pending;
} AlarmLog;

static void alarm_log_on_event(const NN20ClockTimerEvent *event,
                               void *user_data)
{
    AlarmLog *log = user_data;

    /* Every event carries it, which is the point of it being in the
     * base payload - see the field's comment. */
    log->last_snooze_pending = event->payload->snooze_pending;

    if (event->type != NN20CLOCK_TIMER_EVENT_ALARM) {
        return;
    }

    /* Design 6's derived payload: the event carries the base pointer,
     * and a subscriber that cares about alarms downcasts it. */
    const NN20ClockTimerAlarmPayload *payload =
        (const NN20ClockTimerAlarmPayload *)event->payload;

    log->fires++;
    log->last_id = payload->alarm_id;
    log->last_scheduled = payload->super.displayed_time;
    log->last_late = payload->late_seconds;
}

/*
 * Holds an NN20ClockAlarmList, which puts it over 5 KB - more than the
 * stack of the task this suite runs on when it runs on the board. Every
 * case declares its fixture `static` for that reason; alarm_fixture_up()
 * zeroes it, so reuse between cases is clean.
 */
typedef struct {
    NN20ClockTimer *timer;
    FakeClock clock;
    FakeSchedule schedule;
    AlarmLog log;
    NN20ClockTimerSubscriber subscriber;
} AlarmFixture;

/*
 * One shared alarm fixture, at file scope, rather than a static inside
 * each case.
 *
 * It carries an NN20ClockAlarmList twice over - its own and the fake
 * schedule's - which puts it past 11 KB. It cannot be a local: that is
 * more than the stack of the task this suite runs on when it runs on
 * the board. But one static per case meant twenty copies resident at
 * once, over a hundred kilobytes of .bss that nothing could ever
 * reclaim, and the on-target test app then ran out of internal memory
 * in components that had nothing to do with alarms.
 *
 * Sharing is safe because cases run one after another and
 * alarm_fixture_up() zeroes it first.
 */
static AlarmFixture g_alarms;

static time_t utc_at(int year, int month, int day, int hour, int minute,
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

/* One daily alarm at 07:00, and the clock parked just before it. */
static void alarm_fixture_up(AlarmFixture *fixture)
{
    memset(fixture, 0, sizeof(*fixture));

    fixture->timer = nn20clock_timer_ctor(NULL);
    if (fixture->timer == NULL) {
        return;
    }
    (void)nn20clock_timer_set_timezone(fixture->timer, "UTC0");

    NN20ClockAlarmConfig alarm;
    (void)nn20clock_alarm_defaults(&alarm);
    alarm.id = NN20CLOCK_ALARM_ID_NONE;
    alarm.hour = 7u;
    alarm.minute = 0u;
    alarm.weekdays_mask = NN20CLOCK_ALARM_EVERY_DAY;
    (void)nn20clock_alarm_list_put(&fixture->schedule.list, &alarm, NULL);

    fixture->clock.now = utc_at(2026, 8, 28, 6, 59, 59);
    (void)nn20clock_timer_set_clock_fn(fixture->timer, fake_clock_read,
                                       &fixture->clock);
    (void)nn20clock_timer_set_alarm_source(fixture->timer, fake_alarm_source,
                                           &fixture->schedule);

    fixture->subscriber.on_event = alarm_log_on_event;
    fixture->subscriber.user_data = &fixture->log;
    (void)nn20clock_timer_subscribe(fixture->timer, &fixture->subscriber);
}

static void alarm_fixture_down(AlarmFixture *fixture)
{
    if (fixture->timer != NULL) {
        (void)nn20clock_timer_unsubscribe(fixture->timer,
                                          &fixture->subscriber);
        nn20clock_timer_dtor(fixture->timer);
    }
}

static void alarm_poll(AlarmFixture *fixture)
{
    REQUIRE(nn20clock_timer_poll(fixture->timer) == ESP_OK);
    REQUIRE(nn20clock_timer_flush(fixture->timer) == ESP_OK);
}

TEST(the_schedule_is_loaded_from_the_alarm_source)
{
    alarm_fixture_up(&g_alarms);
    REQUIRE(g_alarms.timer != NULL);

    CHECK_EQ(0, nn20clock_timer_alarm_count(g_alarms.timer));
    CHECK_EQ(ESP_OK, nn20clock_timer_reload_schedule(g_alarms.timer));
    CHECK_EQ(1, g_alarms.schedule.loads);
    CHECK_EQ(1, nn20clock_timer_alarm_count(g_alarms.timer));

    alarm_fixture_down(&g_alarms);
}

/* A failed read must not wipe the live schedule - a clock that forgets
 * its alarms because flash hiccuped is worse than one running a stale
 * list. */
TEST(a_failed_reload_keeps_the_previous_schedule)
{
    alarm_fixture_up(&g_alarms);
    REQUIRE(g_alarms.timer != NULL);
    REQUIRE(nn20clock_timer_reload_schedule(g_alarms.timer) == ESP_OK);

    g_alarms.schedule.result = ESP_FAIL;
    CHECK_EQ(ESP_FAIL, nn20clock_timer_reload_schedule(g_alarms.timer));
    CHECK_EQ(1, nn20clock_timer_alarm_count(g_alarms.timer));

    alarm_fixture_down(&g_alarms);
}

TEST(reload_needs_an_alarm_source)
{
    NN20ClockTimer *timer = nn20clock_timer_ctor(NULL);
    REQUIRE(timer != NULL);

    CHECK_EQ(ESP_ERR_INVALID_STATE, nn20clock_timer_reload_schedule(timer));
    CHECK_EQ(ESP_ERR_INVALID_ARG, nn20clock_timer_reload_schedule(NULL));
    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_timer_set_alarm_source(NULL, fake_alarm_source, NULL));

    nn20clock_timer_dtor(timer);
}

TEST(an_alarm_fires_when_its_moment_arrives)
{
    alarm_fixture_up(&g_alarms);
    REQUIRE(g_alarms.timer != NULL);
    REQUIRE(nn20clock_timer_reload_schedule(g_alarms.timer) == ESP_OK);
    REQUIRE(nn20clock_timer_start(g_alarms.timer) == ESP_OK);
    REQUIRE(nn20clock_timer_flush(g_alarms.timer) == ESP_OK);

    /* The first poll established the baseline at 06:59:59 and fired
     * nothing. */
    CHECK_EQ(0, g_alarms.log.fires);

    g_alarms.clock.now = utc_at(2026, 8, 28, 7, 0, 0);
    alarm_poll(&g_alarms);

    CHECK_EQ(1, g_alarms.log.fires);
    CHECK_EQ(1, g_alarms.log.last_id);
    CHECK_EQ(utc_at(2026, 8, 28, 7, 0, 0), g_alarms.log.last_scheduled);
    CHECK_EQ(0, g_alarms.log.last_late);

    alarm_fixture_down(&g_alarms);
}

/*
 * The question this design answers: the clock goes 06:59:59 ->
 * 07:00:01 and never reads 07:00:00. Matching an instant would lose the
 * alarm; matching the interval fires it, one second late.
 */
TEST(an_alarm_whose_second_was_skipped_still_fires)
{
    alarm_fixture_up(&g_alarms);
    REQUIRE(g_alarms.timer != NULL);
    REQUIRE(nn20clock_timer_reload_schedule(g_alarms.timer) == ESP_OK);
    REQUIRE(nn20clock_timer_start(g_alarms.timer) == ESP_OK);
    REQUIRE(nn20clock_timer_flush(g_alarms.timer) == ESP_OK);

    g_alarms.clock.now = utc_at(2026, 8, 28, 7, 0, 1);
    alarm_poll(&g_alarms);

    CHECK_EQ(1, g_alarms.log.fires);
    /* Reported at the time it was scheduled for, with its lateness
     * alongside - not at the moment it happened to be noticed. */
    CHECK_EQ(utc_at(2026, 8, 28, 7, 0, 0), g_alarms.log.last_scheduled);
    CHECK_EQ(1, g_alarms.log.last_late);

    alarm_fixture_down(&g_alarms);
}

/* The other half of the question: having fired, it must not fire again
 * on any later tick. */
TEST(an_alarm_does_not_fire_twice)
{
    alarm_fixture_up(&g_alarms);
    REQUIRE(g_alarms.timer != NULL);
    REQUIRE(nn20clock_timer_reload_schedule(g_alarms.timer) == ESP_OK);
    REQUIRE(nn20clock_timer_start(g_alarms.timer) == ESP_OK);
    REQUIRE(nn20clock_timer_flush(g_alarms.timer) == ESP_OK);

    g_alarms.clock.now = utc_at(2026, 8, 28, 7, 0, 0);
    alarm_poll(&g_alarms);
    CHECK_EQ(1, g_alarms.log.fires);

    /* Tick through the rest of the minute a second at a time. */
    for (int i = 1; i <= 30; i++) {
        g_alarms.clock.now = utc_at(2026, 8, 28, 7, 0, i);
        alarm_poll(&g_alarms);
    }
    CHECK_EQ(1, g_alarms.log.fires);

    /* And the next day it fires once more - it is a daily alarm. */
    g_alarms.clock.now = utc_at(2026, 8, 29, 6, 59, 59);
    alarm_poll(&g_alarms);
    g_alarms.clock.now = utc_at(2026, 8, 29, 7, 0, 0);
    alarm_poll(&g_alarms);
    CHECK_EQ(2, g_alarms.log.fires);

    alarm_fixture_down(&g_alarms);
}

/* A reload must not resurrect an alarm that already went off - the
 * interval alone would not stop this, which is why the Timer also
 * remembers the occurrence it fired. */
TEST(a_reload_does_not_re_fire_the_current_occurrence)
{
    alarm_fixture_up(&g_alarms);
    REQUIRE(g_alarms.timer != NULL);
    REQUIRE(nn20clock_timer_reload_schedule(g_alarms.timer) == ESP_OK);
    REQUIRE(nn20clock_timer_start(g_alarms.timer) == ESP_OK);
    REQUIRE(nn20clock_timer_flush(g_alarms.timer) == ESP_OK);

    g_alarms.clock.now = utc_at(2026, 8, 28, 7, 0, 0);
    alarm_poll(&g_alarms);
    CHECK_EQ(1, g_alarms.log.fires);

    REQUIRE(nn20clock_timer_reload_schedule(g_alarms.timer) == ESP_OK);
    g_alarms.clock.now = utc_at(2026, 8, 28, 7, 0, 5);
    alarm_poll(&g_alarms);
    CHECK_EQ(1, g_alarms.log.fires);

    alarm_fixture_down(&g_alarms);
}

/* Late but inside the grace window: still rings. */
TEST(a_slightly_late_alarm_still_rings)
{
    alarm_fixture_up(&g_alarms);
    REQUIRE(g_alarms.timer != NULL);
    REQUIRE(nn20clock_timer_reload_schedule(g_alarms.timer) == ESP_OK);
    REQUIRE(nn20clock_timer_start(g_alarms.timer) == ESP_OK);
    REQUIRE(nn20clock_timer_flush(g_alarms.timer) == ESP_OK);

    /* Half a minute of stall - a long time for this device, well within
     * the window. */
    g_alarms.clock.now = utc_at(2026, 8, 28, 7, 0, 30);
    alarm_poll(&g_alarms);

    CHECK_EQ(1, g_alarms.log.fires);
    CHECK_EQ(30, g_alarms.log.last_late);
    CHECK_EQ(0, nn20clock_timer_missed_alarm_count(g_alarms.timer));

    alarm_fixture_down(&g_alarms);
}

/*
 * Beyond the grace window it is a missed alarm, not a late one. Ringing
 * a 07:00 alarm hours after the fact would be worse than staying quiet,
 * so it is counted and skipped.
 */
TEST(an_alarm_beyond_the_grace_window_is_missed_not_fired)
{
    alarm_fixture_up(&g_alarms);
    REQUIRE(g_alarms.timer != NULL);
    REQUIRE(nn20clock_timer_reload_schedule(g_alarms.timer) == ESP_OK);
    REQUIRE(nn20clock_timer_start(g_alarms.timer) == ESP_OK);
    REQUIRE(nn20clock_timer_flush(g_alarms.timer) == ESP_OK);

    /* The device was busy - or asleep - for two hours. */
    g_alarms.clock.now = utc_at(2026, 8, 28, 9, 0, 0);
    alarm_poll(&g_alarms);

    CHECK_EQ(0, g_alarms.log.fires);
    CHECK_EQ(1, nn20clock_timer_missed_alarm_count(g_alarms.timer));

    /* And it is not reconsidered on the following tick. */
    g_alarms.clock.now = utc_at(2026, 8, 28, 9, 0, 1);
    alarm_poll(&g_alarms);
    CHECK_EQ(0, g_alarms.log.fires);
    CHECK_EQ(1, nn20clock_timer_missed_alarm_count(g_alarms.timer));

    alarm_fixture_down(&g_alarms);
}

TEST(the_grace_boundary_is_where_it_says_it_is)
{
    alarm_fixture_up(&g_alarms);
    REQUIRE(g_alarms.timer != NULL);
    REQUIRE(nn20clock_timer_reload_schedule(g_alarms.timer) == ESP_OK);
    REQUIRE(nn20clock_timer_start(g_alarms.timer) == ESP_OK);
    REQUIRE(nn20clock_timer_flush(g_alarms.timer) == ESP_OK);

    /* Exactly at the limit still rings. */
    g_alarms.clock.now = utc_at(2026, 8, 28, 7, 0, 0) +
                        NN20CLOCK_TIMER_ALARM_GRACE_SECONDS;
    alarm_poll(&g_alarms);
    CHECK_EQ(1, g_alarms.log.fires);
    CHECK_EQ(NN20CLOCK_TIMER_ALARM_GRACE_SECONDS, g_alarms.log.last_late);

    alarm_fixture_down(&g_alarms);
}

/*
 * The dangerous case. SNTP corrects the clock forward across an alarm;
 * that span is not elapsed time and contains no real occurrence, so
 * nothing may fire. Without the re-baseline this would ring the moment
 * the network came up.
 */
TEST(a_clock_correction_does_not_fire_the_alarms_it_jumped_over)
{
    alarm_fixture_up(&g_alarms);
    REQUIRE(g_alarms.timer != NULL);
    REQUIRE(nn20clock_timer_reload_schedule(g_alarms.timer) == ESP_OK);

    /* Boot with the RTC hours behind, as it would be before a sync. */
    g_alarms.clock.now = utc_at(2026, 8, 28, 3, 0, 0);
    REQUIRE(nn20clock_timer_start(g_alarms.timer) == ESP_OK);
    REQUIRE(nn20clock_timer_flush(g_alarms.timer) == ESP_OK);

    /* SNTP lands and the clock jumps past 07:00. */
    g_alarms.clock.now = utc_at(2026, 8, 28, 9, 0, 0);
    REQUIRE(nn20clock_timer_notify_time_synced(g_alarms.timer) == ESP_OK);
    REQUIRE(nn20clock_timer_flush(g_alarms.timer) == ESP_OK);

    alarm_poll(&g_alarms);
    CHECK_EQ(0, g_alarms.log.fires);
    /* Not even counted as missed: no time actually passed over it. */
    CHECK_EQ(0, nn20clock_timer_missed_alarm_count(g_alarms.timer));

    /* The clock is now trusted, and the next real alarm still works. */
    g_alarms.clock.now = utc_at(2026, 8, 29, 6, 59, 59);
    alarm_poll(&g_alarms);
    g_alarms.clock.now = utc_at(2026, 8, 29, 7, 0, 0);
    alarm_poll(&g_alarms);
    CHECK_EQ(1, g_alarms.log.fires);

    alarm_fixture_down(&g_alarms);
}

/*
 * The same correction, made by hand instead of by SNTP.
 *
 * A device with no internet is set from the settings screen, and that
 * jump is no more elapsed time than a sync is. Before the re-baseline
 * moved into the hand-set path this scanned the whole span, found the
 * earliest occurrence in it, and burned the tick marking that occurrence
 * missed - so the alarm the user set a minute later never rang.
 */
TEST(a_hand_set_clock_does_not_fire_or_consume_the_alarms_it_jumped_over)
{
    alarm_fixture_up(&g_alarms);
    REQUIRE(g_alarms.timer != NULL);
    REQUIRE(nn20clock_timer_reload_schedule(g_alarms.timer) == ESP_OK);

    /* Yesterday's restored last-known-time, which is what a clock with
     * no sync comes back to after a power cut. */
    g_alarms.clock.now = utc_at(2026, 8, 27, 6, 59, 0);
    REQUIRE(nn20clock_timer_start(g_alarms.timer) == ESP_OK);
    REQUIRE(nn20clock_timer_flush(g_alarms.timer) == ESP_OK);

    /* The user sets it to the real time: a day and a bit forward, over
     * yesterday's 07:00 occurrence. */
    g_alarms.clock.now = utc_at(2026, 8, 28, 6, 59, 0);
    REQUIRE(nn20clock_timer_notify_time_set(g_alarms.timer) == ESP_OK);
    REQUIRE(nn20clock_timer_flush(g_alarms.timer) == ESP_OK);

    alarm_poll(&g_alarms);
    CHECK_EQ(0, g_alarms.log.fires);
    CHECK_EQ(0, nn20clock_timer_missed_alarm_count(g_alarms.timer));

    /* And the 07:00 a minute later - the alarm the user just set - still
     * rings. This is the assertion the bug broke. */
    g_alarms.clock.now = utc_at(2026, 8, 28, 6, 59, 59);
    alarm_poll(&g_alarms);
    g_alarms.clock.now = utc_at(2026, 8, 28, 7, 0, 0);
    alarm_poll(&g_alarms);
    CHECK_EQ(1, g_alarms.log.fires);

    alarm_fixture_down(&g_alarms);
}

/* Setting the clock by hand is an authoritative act: the face must stop
 * saying "clock not set" straight afterwards, on a device where SNTP
 * will never come along to say it instead. */
TEST(a_hand_set_clock_counts_as_synced)
{
    alarm_fixture_up(&g_alarms);
    REQUIRE(g_alarms.timer != NULL);
    REQUIRE(nn20clock_timer_start(g_alarms.timer) == ESP_OK);
    REQUIRE(nn20clock_timer_flush(g_alarms.timer) == ESP_OK);

    CHECK(!nn20clock_timer_is_time_synced(g_alarms.timer));

    REQUIRE(nn20clock_timer_notify_time_set(g_alarms.timer) == ESP_OK);
    REQUIRE(nn20clock_timer_flush(g_alarms.timer) == ESP_OK);

    CHECK(nn20clock_timer_is_time_synced(g_alarms.timer));

    alarm_fixture_down(&g_alarms);
}

TEST(a_disabled_alarm_never_fires)
{
    alarm_fixture_up(&g_alarms);
    REQUIRE(g_alarms.timer != NULL);

    g_alarms.schedule.list.alarms[0].enabled = false;
    REQUIRE(nn20clock_timer_reload_schedule(g_alarms.timer) == ESP_OK);
    REQUIRE(nn20clock_timer_start(g_alarms.timer) == ESP_OK);
    REQUIRE(nn20clock_timer_flush(g_alarms.timer) == ESP_OK);

    g_alarms.clock.now = utc_at(2026, 8, 28, 7, 0, 0);
    alarm_poll(&g_alarms);
    CHECK_EQ(0, g_alarms.log.fires);

    alarm_fixture_down(&g_alarms);
}

/* A one-off fires once and, unlike a recurrent alarm, has no next
 * occurrence. Design 10 has the ClockManager delete it after that. */
TEST(a_one_off_alarm_fires_once_and_has_no_next_time)
{
    alarm_fixture_up(&g_alarms);
    REQUIRE(g_alarms.timer != NULL);

    NN20ClockAlarmConfig one_off;
    (void)nn20clock_alarm_defaults(&one_off);
    one_off.id = NN20CLOCK_ALARM_ID_NONE;
    one_off.kind = NN20CLOCK_ALARM_KIND_ONE_OFF;
    one_off.weekdays_mask = 0u;
    one_off.one_off_date.year = 2026u;
    one_off.one_off_date.month = 8u;
    one_off.one_off_date.day = 28u;
    one_off.hour = 7u;
    one_off.minute = 0u;

    memset(&g_alarms.schedule.list, 0, sizeof(g_alarms.schedule.list));
    REQUIRE(nn20clock_alarm_list_put(&g_alarms.schedule.list, &one_off, NULL)
            == ESP_OK);
    REQUIRE(nn20clock_timer_reload_schedule(g_alarms.timer) == ESP_OK);
    REQUIRE(nn20clock_timer_start(g_alarms.timer) == ESP_OK);
    REQUIRE(nn20clock_timer_flush(g_alarms.timer) == ESP_OK);

    g_alarms.clock.now = utc_at(2026, 8, 28, 7, 0, 0);
    alarm_poll(&g_alarms);
    CHECK_EQ(1, g_alarms.log.fires);

    /* The next day brings nothing: it was a one-off. */
    g_alarms.clock.now = utc_at(2026, 8, 29, 6, 59, 59);
    alarm_poll(&g_alarms);
    g_alarms.clock.now = utc_at(2026, 8, 29, 7, 0, 0);
    alarm_poll(&g_alarms);
    CHECK_EQ(1, g_alarms.log.fires);

    alarm_fixture_down(&g_alarms);
}

/* ------------------------------------------------------ snooze (M7) -- */

/* A snooze is a one-shot occurrence the Timer holds in RAM. It fires
 * once, at the right time, carrying the original alarm's id. */
TEST(a_snooze_fires_the_same_alarm_again)
{
    alarm_fixture_up(&g_alarms);
    REQUIRE(g_alarms.timer != NULL);
    REQUIRE(nn20clock_timer_reload_schedule(g_alarms.timer) == ESP_OK);
    REQUIRE(nn20clock_timer_start(g_alarms.timer) == ESP_OK);
    REQUIRE(nn20clock_timer_flush(g_alarms.timer) == ESP_OK);

    g_alarms.clock.now = utc_at(2026, 8, 28, 7, 0, 0);
    alarm_poll(&g_alarms);
    REQUIRE(g_alarms.log.fires == 1);

    /* Snoozed at 07:00 for nine minutes. */
    REQUIRE(nn20clock_timer_snooze(g_alarms.timer, 1u, 9u) == ESP_OK);
    CHECK_EQ(1, nn20clock_timer_snoozed_alarm(g_alarms.timer));
    CHECK_EQ(utc_at(2026, 8, 28, 7, 9, 0),
             nn20clock_timer_snooze_time(g_alarms.timer));

    /* Nothing at eight minutes. */
    g_alarms.clock.now = utc_at(2026, 8, 28, 7, 8, 0);
    alarm_poll(&g_alarms);
    CHECK_EQ(1, g_alarms.log.fires);

    /* And it comes back at nine, as the same alarm. */
    g_alarms.clock.now = utc_at(2026, 8, 28, 7, 9, 0);
    alarm_poll(&g_alarms);
    CHECK_EQ(2, g_alarms.log.fires);
    CHECK_EQ(1, g_alarms.log.last_id);

    /* Once only: a snooze is consumed when it fires. */
    g_alarms.clock.now = utc_at(2026, 8, 28, 7, 10, 0);
    alarm_poll(&g_alarms);
    CHECK_EQ(2, g_alarms.log.fires);
    CHECK_EQ(NN20CLOCK_ALARM_ID_NONE,
             nn20clock_timer_snoozed_alarm(g_alarms.timer));

    alarm_fixture_down(&g_alarms);
}

/*
 * A pending snooze rides on every event, not just the alarm one.
 *
 * The clock face shows a snoozed alarm in the corner, and a screen
 * built after the snooze was set - opening the settings and coming back
 * does exactly that - has no other way to learn about it. Same reason
 * time_synced is in the base payload, and the same bug if it is not.
 */
TEST(a_pending_snooze_is_carried_on_every_event)
{
    alarm_fixture_up(&g_alarms);
    REQUIRE(g_alarms.timer != NULL);
    REQUIRE(nn20clock_timer_reload_schedule(g_alarms.timer) == ESP_OK);
    REQUIRE(nn20clock_timer_start(g_alarms.timer) == ESP_OK);
    REQUIRE(nn20clock_timer_flush(g_alarms.timer) == ESP_OK);

    g_alarms.clock.now = utc_at(2026, 8, 28, 7, 0, 0);
    alarm_poll(&g_alarms);
    REQUIRE(g_alarms.log.fires == 1);
    CHECK(!g_alarms.log.last_snooze_pending);

    REQUIRE(nn20clock_timer_snooze(g_alarms.timer, 1u, 9u) == ESP_OK);

    /* An ordinary tick, no alarm involved, and it says so. */
    g_alarms.clock.now = utc_at(2026, 8, 28, 7, 0, 1);
    alarm_poll(&g_alarms);
    CHECK(g_alarms.log.last_snooze_pending);

    /* Cancelled, and the next tick says that too. */
    REQUIRE(nn20clock_timer_cancel_snooze(g_alarms.timer) == ESP_OK);
    g_alarms.clock.now = utc_at(2026, 8, 28, 7, 0, 2);
    alarm_poll(&g_alarms);
    CHECK(!g_alarms.log.last_snooze_pending);

    alarm_fixture_down(&g_alarms);
}

/*
 * And it is false again once the snooze has been consumed by firing -
 * otherwise the clock face would show a reminder for an alarm that is
 * already ringing.
 */
TEST(a_fired_snooze_is_no_longer_pending)
{
    alarm_fixture_up(&g_alarms);
    REQUIRE(g_alarms.timer != NULL);
    REQUIRE(nn20clock_timer_reload_schedule(g_alarms.timer) == ESP_OK);
    REQUIRE(nn20clock_timer_start(g_alarms.timer) == ESP_OK);
    REQUIRE(nn20clock_timer_flush(g_alarms.timer) == ESP_OK);

    g_alarms.clock.now = utc_at(2026, 8, 28, 7, 0, 0);
    alarm_poll(&g_alarms);
    REQUIRE(nn20clock_timer_snooze(g_alarms.timer, 1u, 9u) == ESP_OK);

    g_alarms.clock.now = utc_at(2026, 8, 28, 7, 9, 0);
    alarm_poll(&g_alarms);
    CHECK_EQ(2, g_alarms.log.fires);
    CHECK(!g_alarms.log.last_snooze_pending);

    alarm_fixture_down(&g_alarms);
}

/* The whole point of cancelling: an alarm snoozed and then dismissed
 * must not come back. */
TEST(a_cancelled_snooze_never_fires)
{
    alarm_fixture_up(&g_alarms);
    REQUIRE(g_alarms.timer != NULL);
    REQUIRE(nn20clock_timer_reload_schedule(g_alarms.timer) == ESP_OK);
    REQUIRE(nn20clock_timer_start(g_alarms.timer) == ESP_OK);
    REQUIRE(nn20clock_timer_flush(g_alarms.timer) == ESP_OK);

    g_alarms.clock.now = utc_at(2026, 8, 28, 7, 0, 0);
    alarm_poll(&g_alarms);
    REQUIRE(nn20clock_timer_snooze(g_alarms.timer, 1u, 9u) == ESP_OK);

    CHECK_EQ(ESP_OK, nn20clock_timer_cancel_snooze(g_alarms.timer));
    CHECK_EQ(NN20CLOCK_ALARM_ID_NONE,
             nn20clock_timer_snoozed_alarm(g_alarms.timer));

    g_alarms.clock.now = utc_at(2026, 8, 28, 7, 9, 0);
    alarm_poll(&g_alarms);
    CHECK_EQ(1, g_alarms.log.fires);   /* the original firing only */

    /* Cancelling when nothing is pending is not an error - the caller
     * should not have to know. */
    CHECK_EQ(ESP_OK, nn20clock_timer_cancel_snooze(g_alarms.timer));

    alarm_fixture_down(&g_alarms);
}

/* A late tick must not lose a snooze, exactly as it must not lose an
 * alarm - it is matched over the same half-open interval. */
TEST(a_snooze_in_a_skipped_second_still_fires)
{
    alarm_fixture_up(&g_alarms);
    REQUIRE(g_alarms.timer != NULL);
    REQUIRE(nn20clock_timer_reload_schedule(g_alarms.timer) == ESP_OK);
    REQUIRE(nn20clock_timer_start(g_alarms.timer) == ESP_OK);
    REQUIRE(nn20clock_timer_flush(g_alarms.timer) == ESP_OK);

    g_alarms.clock.now = utc_at(2026, 8, 28, 7, 0, 0);
    alarm_poll(&g_alarms);
    REQUIRE(nn20clock_timer_snooze(g_alarms.timer, 1u, 9u) == ESP_OK);

    /* The clock skips straight over 07:09:00. */
    g_alarms.clock.now = utc_at(2026, 8, 28, 7, 8, 59);
    alarm_poll(&g_alarms);
    g_alarms.clock.now = utc_at(2026, 8, 28, 7, 9, 1);
    alarm_poll(&g_alarms);

    CHECK_EQ(2, g_alarms.log.fires);
    CHECK_EQ(1, g_alarms.log.last_late);

    alarm_fixture_down(&g_alarms);
}

/* Beyond the grace window it is missed, like any other occurrence - and
 * consumed, so it does not ambush anyone later. */
TEST(a_snooze_beyond_the_grace_window_is_missed)
{
    alarm_fixture_up(&g_alarms);
    REQUIRE(g_alarms.timer != NULL);
    REQUIRE(nn20clock_timer_reload_schedule(g_alarms.timer) == ESP_OK);
    REQUIRE(nn20clock_timer_start(g_alarms.timer) == ESP_OK);
    REQUIRE(nn20clock_timer_flush(g_alarms.timer) == ESP_OK);

    g_alarms.clock.now = utc_at(2026, 8, 28, 7, 0, 0);
    alarm_poll(&g_alarms);
    REQUIRE(nn20clock_timer_snooze(g_alarms.timer, 1u, 9u) == ESP_OK);

    /* An hour late. */
    g_alarms.clock.now = utc_at(2026, 8, 28, 8, 9, 0);
    alarm_poll(&g_alarms);

    CHECK_EQ(1, g_alarms.log.fires);
    CHECK_EQ(1, nn20clock_timer_missed_alarm_count(g_alarms.timer));
    CHECK_EQ(NN20CLOCK_ALARM_ID_NONE,
             nn20clock_timer_snoozed_alarm(g_alarms.timer));

    alarm_fixture_down(&g_alarms);
}

/* Design 10: a pending snooze is RAM-only. Stopping the Timer is the
 * closest a test gets to a reboot, and it must forget. */
TEST(stopping_the_timer_forgets_a_snooze)
{
    alarm_fixture_up(&g_alarms);
    REQUIRE(g_alarms.timer != NULL);
    REQUIRE(nn20clock_timer_reload_schedule(g_alarms.timer) == ESP_OK);
    REQUIRE(nn20clock_timer_start(g_alarms.timer) == ESP_OK);
    REQUIRE(nn20clock_timer_flush(g_alarms.timer) == ESP_OK);

    g_alarms.clock.now = utc_at(2026, 8, 28, 7, 0, 0);
    alarm_poll(&g_alarms);
    REQUIRE(nn20clock_timer_snooze(g_alarms.timer, 1u, 9u) == ESP_OK);

    REQUIRE(nn20clock_timer_stop(g_alarms.timer) == ESP_OK);
    CHECK_EQ(NN20CLOCK_ALARM_ID_NONE,
             nn20clock_timer_snoozed_alarm(g_alarms.timer));

    REQUIRE(nn20clock_timer_start(g_alarms.timer) == ESP_OK);
    REQUIRE(nn20clock_timer_flush(g_alarms.timer) == ESP_OK);
    const int fires_after_restart = g_alarms.log.fires;

    g_alarms.clock.now = utc_at(2026, 8, 28, 7, 9, 0);
    alarm_poll(&g_alarms);
    CHECK_EQ(fires_after_restart, g_alarms.log.fires);

    alarm_fixture_down(&g_alarms);
}

/* Only one at a time: there is one speaker. */
TEST(a_second_snooze_replaces_the_first)
{
    alarm_fixture_up(&g_alarms);
    REQUIRE(g_alarms.timer != NULL);
    REQUIRE(nn20clock_timer_start(g_alarms.timer) == ESP_OK);
    REQUIRE(nn20clock_timer_flush(g_alarms.timer) == ESP_OK);

    g_alarms.clock.now = utc_at(2026, 8, 28, 7, 0, 0);
    REQUIRE(nn20clock_timer_snooze(g_alarms.timer, 1u, 9u) == ESP_OK);
    REQUIRE(nn20clock_timer_snooze(g_alarms.timer, 1u, 5u) == ESP_OK);

    CHECK_EQ(utc_at(2026, 8, 28, 7, 5, 0),
             nn20clock_timer_snooze_time(g_alarms.timer));

    alarm_fixture_down(&g_alarms);
}

TEST(snooze_rejects_bad_arguments)
{
    NN20ClockTimer *timer = nn20clock_timer_ctor(NULL);
    REQUIRE(timer != NULL);

    CHECK_EQ(ESP_ERR_INVALID_ARG, nn20clock_timer_snooze(NULL, 1u, 9u));
    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_timer_snooze(timer, NN20CLOCK_ALARM_ID_NONE, 9u));
    CHECK_EQ(ESP_ERR_INVALID_ARG, nn20clock_timer_snooze(timer, 1u, 0u));
    CHECK_EQ(ESP_ERR_INVALID_ARG, nn20clock_timer_cancel_snooze(NULL));
    CHECK_EQ(NN20CLOCK_ALARM_ID_NONE, nn20clock_timer_snoozed_alarm(NULL));
    CHECK_EQ(0, nn20clock_timer_snooze_time(NULL));

    nn20clock_timer_dtor(timer);
}

TEST(alarm_accessors_tolerate_null)
{
    CHECK_EQ(0, nn20clock_timer_alarm_count(NULL));
    CHECK_EQ(0, nn20clock_timer_missed_alarm_count(NULL));
}

TEST_MAIN("nn20clock_timer")
{
    RUN(ctor_accepts_a_null_storage);
    RUN(dtor_tolerates_null);
    RUN(start_then_stop_round_trips);
    RUN(start_twice_is_rejected);
    RUN(stop_without_start_is_rejected);
    RUN(dtor_while_running_is_safe);
    RUN(lifecycle_functions_reject_null);

    RUN(subscribe_then_unsubscribe_round_trips);
    RUN(subscribing_twice_is_rejected);
    RUN(unsubscribing_a_stranger_is_not_found);
    RUN(subscriber_without_a_callback_is_rejected);
    RUN(subscriber_functions_reject_null);
    RUN(the_subscriber_list_fills_up_and_says_so);
    RUN(unsubscribing_from_the_middle_keeps_the_others);

    RUN(the_displayed_time_is_arithmetic);
    RUN(time_t_survives_2038);
    RUN(derived_payloads_can_be_upcast);
    RUN(weekday_zero_is_monday);

    RUN(starting_emits_the_current_time_at_once);
    RUN(polling_without_the_clock_moving_emits_nothing);
    RUN(a_second_passing_emits_a_second_event_only);
    RUN(crossing_a_minute_emits_both);
    RUN(a_jump_forward_emits_one_event_of_each);
    RUN(a_time_sync_forces_the_next_poll_to_report);
    RUN(stop_then_start_reports_the_time_again);
    RUN(an_unsubscribed_listener_stops_hearing_events);
    RUN(set_clock_fn_and_timezone_reject_bad_arguments);
    RUN(poll_and_flush_reject_null);

    RUN(get_now_returns_the_current_local_time);
    RUN(the_timezone_shifts_the_local_time);
    RUN(a_new_zone_redraws_at_once);
    RUN(to_local_is_free_standing);

    RUN(the_schedule_is_loaded_from_the_alarm_source);
    RUN(a_failed_reload_keeps_the_previous_schedule);
    RUN(reload_needs_an_alarm_source);
    RUN(an_alarm_fires_when_its_moment_arrives);
    RUN(an_alarm_whose_second_was_skipped_still_fires);
    RUN(an_alarm_does_not_fire_twice);
    RUN(a_reload_does_not_re_fire_the_current_occurrence);
    RUN(a_slightly_late_alarm_still_rings);
    RUN(an_alarm_beyond_the_grace_window_is_missed_not_fired);
    RUN(the_grace_boundary_is_where_it_says_it_is);
    RUN(a_clock_correction_does_not_fire_the_alarms_it_jumped_over);
    RUN(a_hand_set_clock_does_not_fire_or_consume_the_alarms_it_jumped_over);
    RUN(a_hand_set_clock_counts_as_synced);
    RUN(a_disabled_alarm_never_fires);
    RUN(a_one_off_alarm_fires_once_and_has_no_next_time);
    RUN(a_snooze_fires_the_same_alarm_again);
    RUN(a_pending_snooze_is_carried_on_every_event);
    RUN(a_fired_snooze_is_no_longer_pending);
    RUN(a_cancelled_snooze_never_fires);
    RUN(a_snooze_in_a_skipped_second_still_fires);
    RUN(a_snooze_beyond_the_grace_window_is_missed);
    RUN(stopping_the_timer_forgets_a_snooze);
    RUN(a_second_snooze_replaces_the_first);
    RUN(snooze_rejects_bad_arguments);
    RUN(alarm_accessors_tolerate_null);
}
