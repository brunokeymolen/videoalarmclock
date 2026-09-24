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
 * Component tests for the ClockManager (design 5, 8).
 *
 * Two things are tested here. The transition table, which is design 5's
 * diagram in code and is pure enough to test without building anything.
 * And the routing: a timer event arriving on the Timer worker, a UI
 * command arriving on the UiWorker, and both ending up applied on the
 * manager worker and nowhere else.
 */
#include "test_util.h"


/*
 * A namespace of the suite's own.
 *
 * On the board these tests write to real flash, which is the point of
 * running them there - but writing to the product's namespace means a
 * test run reconfigures the actual clock. One did: a test stored a fake
 * network, and the device came up afterwards trying to join it, with no
 * Wi-Fi and no NTP.
 */
#define TEST_NAMESPACE "nn20clock_t"

#include "nn20clock_manager.h"

#include <stdlib.h>

/* Barrier: see the same helper in test_nn20clock_storage.c. */
static int barrier_cb(nn20_worker_ctx *worker, void *user_data)
{
    (void)worker;
    (void)user_data;
    return 0;
}

static void flush(nn20_worker_ctx *worker)
{
    (void)nn20_worker_post_sync(worker, barrier_cb, NULL);
}

typedef struct {
    nn20_worker_ctx *manager_worker;
    nn20_worker_ctx *storage_worker;
    NN20ClockStorage *storage;
    NN20ClockTimer *timer;
    NN20ClockManager *manager;
} Fixture;

static Fixture fixture_up(void)
{
    Fixture fixture = {0};

    fixture.manager_worker = nn20_worker_create();
    fixture.storage_worker = nn20_worker_create();
    if (fixture.manager_worker == NULL || fixture.storage_worker == NULL) {
        return fixture;
    }

    fixture.storage = nn20clock_storage_ctor_named(fixture.storage_worker, TEST_NAMESPACE);
    fixture.timer = nn20clock_timer_ctor(fixture.storage);
    if (fixture.storage == NULL || fixture.timer == NULL) {
        return fixture;
    }

    fixture.manager = nn20clock_manager_ctor(fixture.manager_worker,
                                             fixture.storage, fixture.timer);
    return fixture;
}

static void fixture_down(Fixture *fixture)
{
    nn20clock_manager_dtor(fixture->manager);
    nn20clock_timer_dtor(fixture->timer);
    nn20clock_storage_dtor(fixture->storage);
    nn20_worker_delete(fixture->manager_worker);
    nn20_worker_delete(fixture->storage_worker);
}

/* ------------------------------------------------- transition table -- */

/* The edges design 5's diagram draws. If the design changes, this test
 * is the first thing that should fail. */
TEST(the_table_matches_design_5)
{
    CHECK(nn20clock_manager_transition_allowed(NN20CLOCK_STATE_BOOT,
                                               NN20CLOCK_STATE_TIME));
    CHECK(nn20clock_manager_transition_allowed(NN20CLOCK_STATE_BOOT,
                                               NN20CLOCK_STATE_ERROR));
    CHECK(nn20clock_manager_transition_allowed(NN20CLOCK_STATE_TIME,
                                               NN20CLOCK_STATE_ALARM_SETTINGS));
    CHECK(nn20clock_manager_transition_allowed(
        NN20CLOCK_STATE_TIME, NN20CLOCK_STATE_MEDIA_PLAYBACK));
    /* Media management sits behind the settings screen, not on the
     * clock face - and it goes back where it came from. */
    CHECK(nn20clock_manager_transition_allowed(
        NN20CLOCK_STATE_DEVICE_SETTINGS, NN20CLOCK_STATE_MEDIA_MANAGEMENT));
    CHECK(nn20clock_manager_transition_allowed(
        NN20CLOCK_STATE_MEDIA_MANAGEMENT, NN20CLOCK_STATE_DEVICE_SETTINGS));
    CHECK(nn20clock_manager_transition_allowed(
        NN20CLOCK_STATE_MEDIA_MANAGEMENT, NN20CLOCK_STATE_ERROR));
    CHECK(nn20clock_manager_transition_allowed(
        NN20CLOCK_STATE_MEDIA_PLAYBACK, NN20CLOCK_STATE_TIME));
    CHECK(nn20clock_manager_transition_allowed(
        NN20CLOCK_STATE_MEDIA_PLAYBACK, NN20CLOCK_STATE_ERROR));
    CHECK(nn20clock_manager_transition_allowed(NN20CLOCK_STATE_TIME,
                                               NN20CLOCK_STATE_ALARM_RINGING));
    CHECK(nn20clock_manager_transition_allowed(NN20CLOCK_STATE_ALARM_SETTINGS,
                                               NN20CLOCK_STATE_TIME));
    CHECK(nn20clock_manager_transition_allowed(NN20CLOCK_STATE_ALARM_RINGING,
                                               NN20CLOCK_STATE_TIME));
    CHECK(nn20clock_manager_transition_allowed(NN20CLOCK_STATE_ERROR,
                                               NN20CLOCK_STATE_TIME));
    CHECK(nn20clock_manager_transition_allowed(NN20CLOCK_STATE_ERROR,
                                               NN20CLOCK_STATE_ALARM_SETTINGS));

    /* The alarm can interrupt any normal screen. */
    CHECK(nn20clock_manager_transition_allowed(NN20CLOCK_STATE_ALARM_SETTINGS,
                                               NN20CLOCK_STATE_ALARM_RINGING));
    CHECK(nn20clock_manager_transition_allowed(NN20CLOCK_STATE_MEDIA_MANAGEMENT,
                                               NN20CLOCK_STATE_ALARM_RINGING));
    /* Including one the user is sitting and watching: design 5 has an
     * alarm outrank a film. */
    CHECK(nn20clock_manager_transition_allowed(NN20CLOCK_STATE_MEDIA_PLAYBACK,
                                               NN20CLOCK_STATE_ALARM_RINGING));

    /* Playback fallback: media fails, the alarm restarts in place. */
    CHECK(nn20clock_manager_transition_allowed(NN20CLOCK_STATE_ALARM_RINGING,
                                               NN20CLOCK_STATE_ALARM_RINGING));
}

TEST(the_table_has_no_edges_design_5_does_not_draw)
{
    /* Boot is entered once. Nothing transitions back into it - stopping
     * the manager resets it, which is not a transition. */
    for (int from = 0; from < NN20CLOCK_STATE_COUNT; from++) {
        CHECK(!nn20clock_manager_transition_allowed(
            (NN20ClockManagerState)from, NN20CLOCK_STATE_BOOT));
    }

    /* Boot goes to TIME or ERROR only: no jumping straight into a
     * settings screen or a ringing alarm before init has finished. */
    CHECK(!nn20clock_manager_transition_allowed(NN20CLOCK_STATE_BOOT,
                                                NN20CLOCK_STATE_ALARM_SETTINGS));
    CHECK(!nn20clock_manager_transition_allowed(NN20CLOCK_STATE_BOOT,
                                                NN20CLOCK_STATE_ALARM_RINGING));

    /* ERROR is left deliberately, by recovering or by the user editing
     * the configuration - not by drifting into media management. */
    CHECK(!nn20clock_manager_transition_allowed(
        NN20CLOCK_STATE_ERROR, NN20CLOCK_STATE_MEDIA_MANAGEMENT));

    /* Playback is entered from the clock face and nowhere else: the
     * play icon is on TimeUi, not inside the settings screens. */
    CHECK(!nn20clock_manager_transition_allowed(
        NN20CLOCK_STATE_ALARM_SETTINGS, NN20CLOCK_STATE_MEDIA_PLAYBACK));
    CHECK(!nn20clock_manager_transition_allowed(
        NN20CLOCK_STATE_ALARM_RINGING, NN20CLOCK_STATE_MEDIA_PLAYBACK));
    /* Choosing a file swaps the screen inside the state; it is not a
     * transition, and a self-edge here would rebuild the list over the
     * film. */
    CHECK(!nn20clock_manager_transition_allowed(
        NN20CLOCK_STATE_MEDIA_PLAYBACK, NN20CLOCK_STATE_MEDIA_PLAYBACK));

    /*
     * Media management is reached from the system settings screen and
     * from nowhere else - not from the clock face, and not from the
     * alarm settings screen next door.
     */
    CHECK(!nn20clock_manager_transition_allowed(
        NN20CLOCK_STATE_ALARM_SETTINGS, NN20CLOCK_STATE_MEDIA_MANAGEMENT));
    CHECK(!nn20clock_manager_transition_allowed(
        NN20CLOCK_STATE_TIME, NN20CLOCK_STATE_MEDIA_MANAGEMENT));
    /* And it returns to the settings screen, not two levels out to the
     * clock. */
    CHECK(!nn20clock_manager_transition_allowed(
        NN20CLOCK_STATE_MEDIA_MANAGEMENT, NN20CLOCK_STATE_TIME));

    /* Only TIME is self-reachable-free; ALARM_RINGING is the one
     * exception, tested above. */
    CHECK(!nn20clock_manager_transition_allowed(NN20CLOCK_STATE_TIME,
                                                NN20CLOCK_STATE_TIME));
}

TEST(the_table_rejects_out_of_range_states)
{
    CHECK(!nn20clock_manager_transition_allowed(NN20CLOCK_STATE_COUNT,
                                                NN20CLOCK_STATE_TIME));
    CHECK(!nn20clock_manager_transition_allowed(NN20CLOCK_STATE_TIME,
                                                NN20CLOCK_STATE_COUNT));
    CHECK(!nn20clock_manager_transition_allowed((NN20ClockManagerState)-1,
                                                NN20CLOCK_STATE_TIME));
}

TEST(every_state_has_a_name)
{
    CHECK_STR_EQ("BOOT", nn20clock_manager_state_name(NN20CLOCK_STATE_BOOT));
    CHECK_STR_EQ("TIME", nn20clock_manager_state_name(NN20CLOCK_STATE_TIME));
    CHECK_STR_EQ("ALARM_SETTINGS",
                 nn20clock_manager_state_name(NN20CLOCK_STATE_ALARM_SETTINGS));
    CHECK_STR_EQ("ALARM_RINGING",
                 nn20clock_manager_state_name(NN20CLOCK_STATE_ALARM_RINGING));
    CHECK_STR_EQ("MEDIA_PLAYBACK",
                 nn20clock_manager_state_name(
                     NN20CLOCK_STATE_MEDIA_PLAYBACK));
    CHECK_STR_EQ("MEDIA_MANAGEMENT",
                 nn20clock_manager_state_name(
                     NN20CLOCK_STATE_MEDIA_MANAGEMENT));
    CHECK_STR_EQ("ERROR", nn20clock_manager_state_name(NN20CLOCK_STATE_ERROR));
    CHECK_STR_EQ("UNKNOWN",
                 nn20clock_manager_state_name(NN20CLOCK_STATE_COUNT));
}

/* --------------------------------------------------------- lifecycle -- */

TEST(ctor_requires_a_worker_and_a_timer)
{
    Fixture fixture = fixture_up();
    REQUIRE(fixture.manager != NULL);

    CHECK(nn20clock_manager_ctor(NULL, fixture.storage, fixture.timer)
          == NULL);
    CHECK(nn20clock_manager_ctor(fixture.manager_worker, fixture.storage, NULL)
          == NULL);

    fixture_down(&fixture);
}

/* Storage is not implemented until Milestone 3, so the manager has to be
 * constructible without one. */
TEST(ctor_tolerates_a_null_storage)
{
    nn20_worker_ctx *worker = nn20_worker_create();
    REQUIRE(worker != NULL);
    NN20ClockTimer *timer = nn20clock_timer_ctor(NULL);
    REQUIRE(timer != NULL);

    NN20ClockManager *manager = nn20clock_manager_ctor(worker, NULL, timer);
    REQUIRE(manager != NULL);
    CHECK_EQ(NN20CLOCK_STATE_BOOT, nn20clock_manager_state(manager));

    nn20clock_manager_dtor(manager);
    nn20clock_timer_dtor(timer);
    nn20_worker_delete(worker);
}

TEST(dtor_and_accessors_tolerate_null)
{
    nn20clock_manager_dtor(NULL);
    CHECK_EQ(NN20CLOCK_STATE_BOOT, nn20clock_manager_state(NULL));
    CHECK_EQ(0, nn20clock_manager_rejected_transitions(NULL));
    CHECK_EQ(0, nn20clock_manager_command_count(NULL));
    CHECK_EQ(0, nn20clock_manager_timer_event_count(NULL));
    CHECK(!nn20clock_manager_is_running(NULL));
    CHECK(nn20clock_manager_screen(NULL) == NULL);
    CHECK_EQ(ESP_ERR_INVALID_ARG, nn20clock_manager_start(NULL));
    CHECK_EQ(ESP_ERR_INVALID_ARG, nn20clock_manager_stop(NULL));
}

/* Design 5's "init complete" edge, and the subscription that makes the
 * manager a Timer consumer. */
TEST(start_subscribes_to_the_timer_and_enters_time)
{
    Fixture fixture = fixture_up();
    REQUIRE(fixture.manager != NULL);

    CHECK_EQ(NN20CLOCK_STATE_BOOT, nn20clock_manager_state(fixture.manager));
    CHECK_EQ(0, nn20clock_timer_subscriber_count(fixture.timer));

    CHECK_EQ(ESP_OK, nn20clock_manager_start(fixture.manager));
    CHECK(nn20clock_manager_is_running(fixture.manager));
    CHECK_EQ(NN20CLOCK_STATE_TIME, nn20clock_manager_state(fixture.manager));
    CHECK_EQ(1, nn20clock_timer_subscriber_count(fixture.timer));

    CHECK_EQ(ESP_ERR_INVALID_STATE, nn20clock_manager_start(fixture.manager));

    fixture_down(&fixture);
}

TEST(stop_unsubscribes_and_returns_to_boot)
{
    Fixture fixture = fixture_up();
    REQUIRE(fixture.manager != NULL);
    REQUIRE(nn20clock_manager_start(fixture.manager) == ESP_OK);

    CHECK_EQ(ESP_OK, nn20clock_manager_stop(fixture.manager));
    CHECK(!nn20clock_manager_is_running(fixture.manager));
    CHECK_EQ(NN20CLOCK_STATE_BOOT, nn20clock_manager_state(fixture.manager));
    /* The subscriber lives inside the manager, so leaving it registered
     * would leave the Timer holding a pointer into freed memory. */
    CHECK_EQ(0, nn20clock_timer_subscriber_count(fixture.timer));

    CHECK_EQ(ESP_ERR_INVALID_STATE, nn20clock_manager_stop(fixture.manager));

    /* And it can be started again from BOOT. */
    CHECK_EQ(ESP_OK, nn20clock_manager_start(fixture.manager));
    CHECK_EQ(NN20CLOCK_STATE_TIME, nn20clock_manager_state(fixture.manager));

    fixture_down(&fixture);
}

/* ---------------------------------------------------------- states --- */

typedef struct {
    int calls;
    NN20ClockManagerState last_from;
    NN20ClockManagerState last_to;
} StateLog;

static void record_state(NN20ClockManagerState from, NN20ClockManagerState to,
                         void *user_data)
{
    StateLog *log = user_data;
    log->calls++;
    log->last_from = from;
    log->last_to = to;
}

TEST(a_requested_transition_is_applied_on_the_worker)
{
    Fixture fixture = fixture_up();
    REQUIRE(fixture.manager != NULL);

    StateLog log = {0};
    REQUIRE(nn20clock_manager_set_state_callback(fixture.manager, record_state,
                                                 &log) == ESP_OK);
    REQUIRE(nn20clock_manager_start(fixture.manager) == ESP_OK);
    CHECK_EQ(1, log.calls);   /* BOOT -> TIME */

    CHECK_EQ(ESP_OK, nn20clock_manager_request_state(
                         fixture.manager, NN20CLOCK_STATE_ALARM_SETTINGS));
    flush(fixture.manager_worker);

    CHECK_EQ(2, log.calls);
    CHECK_EQ(NN20CLOCK_STATE_TIME, log.last_from);
    CHECK_EQ(NN20CLOCK_STATE_ALARM_SETTINGS, log.last_to);
    CHECK_EQ(NN20CLOCK_STATE_ALARM_SETTINGS,
             nn20clock_manager_state(fixture.manager));

    fixture_down(&fixture);
}

/* An edge design 5 does not have is refused where the current state is
 * known - on the worker - and counted rather than silently ignored. */
TEST(a_disallowed_transition_is_counted_not_applied)
{
    Fixture fixture = fixture_up();
    REQUIRE(fixture.manager != NULL);
    REQUIRE(nn20clock_manager_start(fixture.manager) == ESP_OK);

    /* TIME -> TIME is not an edge. */
    CHECK_EQ(ESP_OK, nn20clock_manager_request_state(fixture.manager,
                                                     NN20CLOCK_STATE_TIME));
    flush(fixture.manager_worker);

    CHECK_EQ(NN20CLOCK_STATE_TIME, nn20clock_manager_state(fixture.manager));
    CHECK_EQ(1, nn20clock_manager_rejected_transitions(fixture.manager));

    fixture_down(&fixture);
}

TEST(request_state_rejects_out_of_range_values)
{
    Fixture fixture = fixture_up();
    REQUIRE(fixture.manager != NULL);

    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_manager_request_state(fixture.manager,
                                             NN20CLOCK_STATE_COUNT));
    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_manager_request_state(NULL, NN20CLOCK_STATE_TIME));

    fixture_down(&fixture);
}

/* The callback is worker-owned state; setting it while the worker is
 * running would be a write from the wrong thread. */
TEST(the_state_callback_can_only_be_set_before_start)
{
    Fixture fixture = fixture_up();
    REQUIRE(fixture.manager != NULL);
    REQUIRE(nn20clock_manager_start(fixture.manager) == ESP_OK);

    StateLog log = {0};
    CHECK_EQ(ESP_ERR_INVALID_STATE,
             nn20clock_manager_set_state_callback(fixture.manager,
                                                  record_state, &log));

    fixture_down(&fixture);
}

/* ------------------------------------------------------ timer events -- */

TEST(a_timer_event_is_applied_on_the_manager_worker)
{
    Fixture fixture = fixture_up();
    REQUIRE(fixture.manager != NULL);
    REQUIRE(nn20clock_manager_start(fixture.manager) == ESP_OK);

    NN20ClockTimerPayload payload = { .displayed_time = 1735689600 };
    const NN20ClockTimerEvent event = {
        .type = NN20CLOCK_TIMER_EVENT_MINUTE,
        .payload = &payload,
    };

    /* Delivered the way the Timer delivers it: straight into the
     * subscriber callback, which is what runs on the Timer worker. */
    nn20clock_manager_on_timer_event(&event, fixture.manager);
    flush(fixture.manager_worker);

    CHECK_EQ(1, nn20clock_manager_timer_event_count(fixture.manager));
    /* A minute tick is not a state change. */
    CHECK_EQ(NN20CLOCK_STATE_TIME, nn20clock_manager_state(fixture.manager));

    fixture_down(&fixture);
}

/* Design 5: the alarm event is what drives the ringing state. */
TEST(an_alarm_event_enters_alarm_ringing)
{
    Fixture fixture = fixture_up();
    REQUIRE(fixture.manager != NULL);
    REQUIRE(nn20clock_manager_start(fixture.manager) == ESP_OK);

    NN20ClockTimerPayload payload = {0};
    const NN20ClockTimerEvent event = {
        .type = NN20CLOCK_TIMER_EVENT_ALARM,
        .payload = &payload,
    };
    nn20clock_manager_on_timer_event(&event, fixture.manager);
    flush(fixture.manager_worker);

    CHECK_EQ(NN20CLOCK_STATE_ALARM_RINGING,
             nn20clock_manager_state(fixture.manager));

    fixture_down(&fixture);
}

TEST(a_timer_error_event_enters_the_error_state)
{
    Fixture fixture = fixture_up();
    REQUIRE(fixture.manager != NULL);
    REQUIRE(nn20clock_manager_start(fixture.manager) == ESP_OK);

    NN20ClockTimerPayload payload = {0};
    const NN20ClockTimerEvent event = {
        .type = NN20CLOCK_TIMER_EVENT_ERROR,
        .payload = &payload,
    };
    nn20clock_manager_on_timer_event(&event, fixture.manager);
    flush(fixture.manager_worker);

    CHECK_EQ(NN20CLOCK_STATE_ERROR, nn20clock_manager_state(fixture.manager));

    fixture_down(&fixture);
}

TEST(the_timer_subscriber_tolerates_null)
{
    NN20ClockTimerPayload payload = {0};
    const NN20ClockTimerEvent event = {
        .type = NN20CLOCK_TIMER_EVENT_SECOND,
        .payload = &payload,
    };
    nn20clock_manager_on_timer_event(&event, NULL);
    nn20clock_manager_on_timer_event(NULL, NULL);
}

/* -------------------------------------------------------- commands --- */

TEST(a_ui_command_routes_to_a_state_change)
{
    Fixture fixture = fixture_up();
    REQUIRE(fixture.manager != NULL);
    REQUIRE(nn20clock_manager_start(fixture.manager) == ESP_OK);

    const NN20ClockUiCommand open = {
        .type = NN20CLOCK_UI_COMMAND_OPEN_SETTINGS,
    };
    CHECK_EQ(ESP_OK, nn20clock_manager_handle_ui_command(fixture.manager,
                                                         &open));
    flush(fixture.manager_worker);
    CHECK_EQ(NN20CLOCK_STATE_ALARM_SETTINGS,
             nn20clock_manager_state(fixture.manager));

    const NN20ClockUiCommand close = {
        .type = NN20CLOCK_UI_COMMAND_CLOSE_SETTINGS,
    };
    CHECK_EQ(ESP_OK, nn20clock_manager_handle_ui_command(fixture.manager,
                                                         &close));
    flush(fixture.manager_worker);
    CHECK_EQ(NN20CLOCK_STATE_TIME, nn20clock_manager_state(fixture.manager));

    CHECK_EQ(2, nn20clock_manager_command_count(fixture.manager));

    fixture_down(&fixture);
}

/*
 * Design 5, Milestone 10: media management is opened from the system
 * settings screen and closes back into it, rather than out to the clock
 * face the way CLOSE_SETTINGS does. The two commands exist to keep
 * those apart, so this is the test that they actually differ.
 */
TEST(media_management_opens_and_closes_inside_the_settings_screen)
{
    Fixture fixture = fixture_up();
    REQUIRE(fixture.manager != NULL);
    REQUIRE(nn20clock_manager_start(fixture.manager) == ESP_OK);
    REQUIRE(nn20clock_manager_request_state(fixture.manager,
                                            NN20CLOCK_STATE_DEVICE_SETTINGS)
            == ESP_OK);
    flush(fixture.manager_worker);

    const NN20ClockUiCommand open = {
        .type = NN20CLOCK_UI_COMMAND_OPEN_MEDIA_MANAGEMENT,
    };
    CHECK_EQ(ESP_OK, nn20clock_manager_handle_ui_command(fixture.manager,
                                                         &open));
    flush(fixture.manager_worker);
    CHECK_EQ(NN20CLOCK_STATE_MEDIA_MANAGEMENT,
             nn20clock_manager_state(fixture.manager));

    const NN20ClockUiCommand close = {
        .type = NN20CLOCK_UI_COMMAND_CLOSE_MEDIA_MANAGEMENT,
    };
    CHECK_EQ(ESP_OK, nn20clock_manager_handle_ui_command(fixture.manager,
                                                         &close));
    flush(fixture.manager_worker);
    CHECK_EQ(NN20CLOCK_STATE_DEVICE_SETTINGS,
             nn20clock_manager_state(fixture.manager));

    fixture_down(&fixture);
}

/*
 * The same command from the clock face is refused, because design 5
 * draws no such edge. It is the counterpart to the test above: the
 * routing is by command, but the table is what decides.
 */
TEST(media_management_cannot_be_opened_from_the_clock_face)
{
    Fixture fixture = fixture_up();
    REQUIRE(fixture.manager != NULL);
    REQUIRE(nn20clock_manager_start(fixture.manager) == ESP_OK);
    CHECK_EQ(NN20CLOCK_STATE_TIME, nn20clock_manager_state(fixture.manager));

    const NN20ClockUiCommand open = {
        .type = NN20CLOCK_UI_COMMAND_OPEN_MEDIA_MANAGEMENT,
    };
    CHECK_EQ(ESP_OK, nn20clock_manager_handle_ui_command(fixture.manager,
                                                         &open));
    flush(fixture.manager_worker);
    CHECK_EQ(NN20CLOCK_STATE_TIME, nn20clock_manager_state(fixture.manager));

    fixture_down(&fixture);
}

/* Design 8: dismissing a ringing alarm returns to the clock. */
TEST(stopping_an_alarm_returns_to_time)
{
    Fixture fixture = fixture_up();
    REQUIRE(fixture.manager != NULL);
    REQUIRE(nn20clock_manager_start(fixture.manager) == ESP_OK);
    REQUIRE(nn20clock_manager_request_state(fixture.manager,
                                            NN20CLOCK_STATE_ALARM_RINGING)
            == ESP_OK);
    flush(fixture.manager_worker);

    const NN20ClockUiCommand stop = { .type = NN20CLOCK_UI_COMMAND_STOP_ALARM };
    CHECK_EQ(ESP_OK, nn20clock_manager_handle_ui_command(fixture.manager,
                                                         &stop));
    flush(fixture.manager_worker);
    CHECK_EQ(NN20CLOCK_STATE_TIME, nn20clock_manager_state(fixture.manager));

    fixture_down(&fixture);
}

/* The audio and brightness services do not exist yet, so these commands
 * are accepted and go nowhere - but they must not change state. */
TEST(commands_with_no_service_behind_them_are_inert)
{
    Fixture fixture = fixture_up();
    REQUIRE(fixture.manager != NULL);
    REQUIRE(nn20clock_manager_start(fixture.manager) == ESP_OK);

    const NN20ClockUiCommandType inert[] = {
        NN20CLOCK_UI_COMMAND_MUTE,
        NN20CLOCK_UI_COMMAND_UNMUTE,
        NN20CLOCK_UI_COMMAND_VOLUME_UP,
        NN20CLOCK_UI_COMMAND_VOLUME_DOWN,
        NN20CLOCK_UI_COMMAND_BRIGHTNESS_UP,
        NN20CLOCK_UI_COMMAND_BRIGHTNESS_DOWN,
    };

    for (size_t i = 0; i < sizeof(inert) / sizeof(inert[0]); i++) {
        const NN20ClockUiCommand command = { .type = inert[i] };
        CHECK_EQ(ESP_OK, nn20clock_manager_handle_ui_command(fixture.manager,
                                                             &command));
    }
    flush(fixture.manager_worker);

    CHECK_EQ(NN20CLOCK_STATE_TIME, nn20clock_manager_state(fixture.manager));
    CHECK_EQ(0, nn20clock_manager_rejected_transitions(fixture.manager));
    CHECK_EQ(6, nn20clock_manager_command_count(fixture.manager));

    fixture_down(&fixture);
}

TEST(handle_ui_command_rejects_null)
{
    Fixture fixture = fixture_up();
    REQUIRE(fixture.manager != NULL);

    const NN20ClockUiCommand command = { .type = NN20CLOCK_UI_COMMAND_MUTE };
    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_manager_handle_ui_command(NULL, &command));
    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_manager_handle_ui_command(fixture.manager, NULL));

    fixture_down(&fixture);
}

/* --------------------------------------------------------- screens --- */

typedef struct {
    int shows;
    int destroys;
} ScreenLog;

typedef struct {
    NN20ClockUiBase super;
    ScreenLog *log;
} FakeScreen;

static esp_err_t fake_show(NN20ClockUiBase *ui)
{
    ((FakeScreen *)ui)->log->shows++;
    return ESP_OK;
}

static void fake_destroy(NN20ClockUiBase *ui)
{
    FakeScreen *screen = (FakeScreen *)ui;
    screen->log->destroys++;
    nn20clock_ui_base_deinit(ui);
    free(screen);
}

static const NN20ClockUiVTable FAKE_VTABLE = {
    .show = fake_show,
    .destroy = fake_destroy,
};

static FakeScreen *fake_screen_new(nn20_worker_ctx *worker, ScreenLog *log,
                                   NN20ClockManager *manager)
{
    FakeScreen *screen = calloc(1, sizeof(*screen));
    if (screen == NULL) {
        return NULL;
    }

    const NN20ClockUiBaseConfig config = {
        .vtable = &FAKE_VTABLE,
        .name = "FakeScreen",
        .worker = worker,
        .manager = manager,
        .on_command = nn20clock_manager_handle_ui_command,
    };
    if (nn20clock_ui_base_init(&screen->super, &config) != ESP_OK) {
        free(screen);
        return NULL;
    }
    screen->log = log;
    return screen;
}

/* Design 8: the outgoing screen is destroyed before the next one is
 * built, so two screens never hold the display at once. */
TEST(switching_screens_destroys_the_old_one_first)
{
    Fixture fixture = fixture_up();
    REQUIRE(fixture.manager != NULL);
    REQUIRE(nn20clock_manager_start(fixture.manager) == ESP_OK);

    nn20_worker_ctx *ui_worker = nn20_worker_create();
    REQUIRE(ui_worker != NULL);

    ScreenLog first_log = {0};
    ScreenLog second_log = {0};
    FakeScreen *first = fake_screen_new(ui_worker, &first_log,
                                        fixture.manager);
    FakeScreen *second = fake_screen_new(ui_worker, &second_log,
                                         fixture.manager);
    REQUIRE(first != NULL);
    REQUIRE(second != NULL);

    CHECK_EQ(ESP_OK, nn20clock_manager_set_screen(fixture.manager,
                                                  &first->super));
    flush(fixture.manager_worker);
    flush(ui_worker);
    CHECK_EQ(1, first_log.shows);
    CHECK(nn20clock_manager_screen(fixture.manager) == &first->super);

    CHECK_EQ(ESP_OK, nn20clock_manager_set_screen(fixture.manager,
                                                  &second->super));
    flush(fixture.manager_worker);
    flush(ui_worker);
    CHECK_EQ(1, first_log.destroys);
    CHECK_EQ(1, second_log.shows);
    CHECK_EQ(0, second_log.destroys);

    /* Stopping the manager drops the screen it still owns. */
    REQUIRE(nn20clock_manager_stop(fixture.manager) == ESP_OK);
    CHECK_EQ(1, second_log.destroys);
    CHECK(nn20clock_manager_screen(fixture.manager) == NULL);

    nn20_worker_delete(ui_worker);
    fixture_down(&fixture);
}

/* The whole point of the extra hop: an event that arrives on the Timer
 * worker reaches the screen on the UiWorker, never in between. */
TEST(a_timer_event_reaches_the_active_screen)
{
    Fixture fixture = fixture_up();
    REQUIRE(fixture.manager != NULL);
    REQUIRE(nn20clock_manager_start(fixture.manager) == ESP_OK);

    nn20_worker_ctx *ui_worker = nn20_worker_create();
    REQUIRE(ui_worker != NULL);

    ScreenLog log = {0};
    FakeScreen *screen = fake_screen_new(ui_worker, &log, fixture.manager);
    REQUIRE(screen != NULL);

    REQUIRE(nn20clock_manager_set_screen(fixture.manager, &screen->super)
            == ESP_OK);
    flush(fixture.manager_worker);
    flush(ui_worker);

    NN20ClockTimerPayload payload = { .displayed_time = 42 };
    const NN20ClockTimerEvent event = {
        .type = NN20CLOCK_TIMER_EVENT_SECOND,
        .payload = &payload,
    };
    nn20clock_manager_on_timer_event(&event, fixture.manager);

    flush(fixture.manager_worker);
    flush(ui_worker);
    CHECK_EQ(1, nn20clock_manager_timer_event_count(fixture.manager));

    REQUIRE(nn20clock_manager_stop(fixture.manager) == ESP_OK);
    nn20_worker_delete(ui_worker);
    fixture_down(&fixture);
}

/* ---------------------------------------------------- screen factory -- */

typedef struct {
    nn20_worker_ctx *ui_worker;
    NN20ClockManager *manager;
    ScreenLog logs[NN20CLOCK_STATE_COUNT];
    int calls;
    NN20ClockManagerState last_state;
} FactoryFixture;

/* Builds a screen for TIME and ALARM_SETTINGS only, so the NULL case -
 * a state with no screen yet, which is most of them at Milestone 2 - is
 * covered too. */
static NN20ClockUiBase *fake_factory(NN20ClockManagerState state,
                                     void *user_data)
{
    FactoryFixture *factory = user_data;
    factory->calls++;
    factory->last_state = state;

    if (state != NN20CLOCK_STATE_TIME &&
        state != NN20CLOCK_STATE_ALARM_SETTINGS) {
        return NULL;
    }

    FakeScreen *screen = fake_screen_new(factory->ui_worker,
                                         &factory->logs[state],
                                         factory->manager);
    return (screen != NULL) ? &screen->super : NULL;
}

/* Design 8: the manager selects the active UI. Entering TIME must build
 * and show the clock screen without anyone asking it to. */
TEST(entering_a_state_builds_its_screen)
{
    Fixture fixture = fixture_up();
    REQUIRE(fixture.manager != NULL);

    FactoryFixture factory = {0};
    factory.ui_worker = nn20_worker_create();
    factory.manager = fixture.manager;
    REQUIRE(factory.ui_worker != NULL);

    REQUIRE(nn20clock_manager_set_screen_factory(fixture.manager, fake_factory,
                                                 &factory) == ESP_OK);
    REQUIRE(nn20clock_manager_start(fixture.manager) == ESP_OK);
    flush(fixture.manager_worker);
    flush(factory.ui_worker);

    /* BOOT -> TIME asked the factory for a TIME screen and showed it. */
    CHECK_EQ(1, factory.calls);
    CHECK_EQ(NN20CLOCK_STATE_TIME, factory.last_state);
    CHECK_EQ(1, factory.logs[NN20CLOCK_STATE_TIME].shows);
    CHECK(nn20clock_manager_screen(fixture.manager) != NULL);

    REQUIRE(nn20clock_manager_stop(fixture.manager) == ESP_OK);
    nn20_worker_delete(factory.ui_worker);
    fixture_down(&fixture);
}

/* Switching state switches screen, old one destroyed first (design 8). */
TEST(changing_state_swaps_the_screen)
{
    Fixture fixture = fixture_up();
    REQUIRE(fixture.manager != NULL);

    FactoryFixture factory = {0};
    factory.ui_worker = nn20_worker_create();
    factory.manager = fixture.manager;
    REQUIRE(factory.ui_worker != NULL);

    REQUIRE(nn20clock_manager_set_screen_factory(fixture.manager, fake_factory,
                                                 &factory) == ESP_OK);
    REQUIRE(nn20clock_manager_start(fixture.manager) == ESP_OK);
    flush(fixture.manager_worker);
    flush(factory.ui_worker);

    REQUIRE(nn20clock_manager_request_state(fixture.manager,
                                            NN20CLOCK_STATE_ALARM_SETTINGS)
            == ESP_OK);
    flush(fixture.manager_worker);
    flush(factory.ui_worker);

    CHECK_EQ(1, factory.logs[NN20CLOCK_STATE_TIME].destroys);
    CHECK_EQ(1, factory.logs[NN20CLOCK_STATE_ALARM_SETTINGS].shows);
    CHECK_EQ(0, factory.logs[NN20CLOCK_STATE_ALARM_SETTINGS].destroys);

    REQUIRE(nn20clock_manager_stop(fixture.manager) == ESP_OK);
    nn20_worker_delete(factory.ui_worker);
    fixture_down(&fixture);
}

/* Most states have no screen at Milestone 2. That must leave the
 * display empty, not keep the previous screen up behind a state it no
 * longer belongs to. */
TEST(a_state_with_no_screen_clears_the_display)
{
    Fixture fixture = fixture_up();
    REQUIRE(fixture.manager != NULL);

    FactoryFixture factory = {0};
    factory.ui_worker = nn20_worker_create();
    factory.manager = fixture.manager;
    REQUIRE(factory.ui_worker != NULL);

    REQUIRE(nn20clock_manager_set_screen_factory(fixture.manager, fake_factory,
                                                 &factory) == ESP_OK);
    REQUIRE(nn20clock_manager_start(fixture.manager) == ESP_OK);
    flush(fixture.manager_worker);
    flush(factory.ui_worker);

    REQUIRE(nn20clock_manager_request_state(fixture.manager,
                                            NN20CLOCK_STATE_ALARM_RINGING)
            == ESP_OK);
    flush(fixture.manager_worker);
    flush(factory.ui_worker);

    CHECK_EQ(1, factory.logs[NN20CLOCK_STATE_TIME].destroys);
    CHECK(nn20clock_manager_screen(fixture.manager) == NULL);

    REQUIRE(nn20clock_manager_stop(fixture.manager) == ESP_OK);
    nn20_worker_delete(factory.ui_worker);
    fixture_down(&fixture);
}

/* Without a factory the manager still runs every transition - that is
 * the headless configuration the host tests and Milestone 1 used. */
TEST(no_factory_means_no_screens_and_no_complaints)
{
    Fixture fixture = fixture_up();
    REQUIRE(fixture.manager != NULL);
    REQUIRE(nn20clock_manager_start(fixture.manager) == ESP_OK);

    CHECK_EQ(NN20CLOCK_STATE_TIME, nn20clock_manager_state(fixture.manager));
    CHECK(nn20clock_manager_screen(fixture.manager) == NULL);

    fixture_down(&fixture);
}

TEST(the_screen_factory_can_only_be_set_before_start)
{
    Fixture fixture = fixture_up();
    REQUIRE(fixture.manager != NULL);

    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_manager_set_screen_factory(NULL, fake_factory, NULL));

    REQUIRE(nn20clock_manager_start(fixture.manager) == ESP_OK);
    CHECK_EQ(ESP_ERR_INVALID_STATE,
             nn20clock_manager_set_screen_factory(fixture.manager,
                                                  fake_factory, NULL));

    fixture_down(&fixture);
}

/* ------------------------------------------------------ alarms (M3) -- */

/*
 * Start storage and empty it.
 *
 * On the host every fixture starts blank; on the board the alarms are
 * in flash and outlive the instance that wrote them, so a test
 * asserting "one alarm is stored" has to know it started from none.
 * That difference is persistence working, not a quirk.
 */
static void storage_up_empty(Fixture *fixture)
{
    REQUIRE(nn20clock_storage_start(fixture->storage) == ESP_OK);

    static NN20ClockAlarmList list;
    while (nn20clock_storage_list_alarms(fixture->storage, &list) == ESP_OK &&
           list.count > 0u) {
        /* Deleting rearranges the list, so re-read rather than iterate. */
        (void)nn20clock_storage_delete_alarm(fixture->storage,
                                             list.alarms[0].id);
    }
}

/* The moment every alarm in these tests is scheduled for. A real epoch
 * second, because the ringing bound measures from it. */
#define ALARM_TIME ((time_t)1787927400)

/* Deliver an alarm event the way the Timer does: into the subscriber
 * callback, which is what runs on the Timer's worker. `when` is the
 * occurrence's own time, which is what the payload carries. */
static void deliver_alarm_at(Fixture *fixture, uint32_t alarm_id, time_t when)
{
    NN20ClockTimerAlarmPayload payload = {
        .super = { .displayed_time = when },
        .alarm_id = alarm_id,
        .late_seconds = 0u,
    };
    const NN20ClockTimerEvent event = {
        .type = NN20CLOCK_TIMER_EVENT_ALARM,
        .payload = &payload.super,
    };
    nn20clock_manager_on_timer_event(&event, fixture->manager);
    flush(fixture->manager_worker);
}

static void deliver_alarm(Fixture *fixture, uint32_t alarm_id)
{
    deliver_alarm_at(fixture, alarm_id, ALARM_TIME);
}

/* One second of the clock going by, which is what the ringing bound is
 * checked against. */
static void deliver_tick(Fixture *fixture, time_t when)
{
    NN20ClockTimerPayload payload = { .displayed_time = when };
    const NN20ClockTimerEvent event = {
        .type = NN20CLOCK_TIMER_EVENT_SECOND,
        .payload = &payload,
    };
    nn20clock_manager_on_timer_event(&event, fixture->manager);
    flush(fixture->manager_worker);
}

static uint32_t store_alarm(Fixture *fixture, NN20ClockAlarmKind kind)
{
    NN20ClockAlarmConfig alarm;
    (void)nn20clock_alarm_defaults(&alarm);
    alarm.id = NN20CLOCK_ALARM_ID_NONE;
    alarm.kind = kind;
    if (kind == NN20CLOCK_ALARM_KIND_ONE_OFF) {
        alarm.weekdays_mask = 0u;
        alarm.one_off_date.year = 2026u;
        alarm.one_off_date.month = 8u;
        alarm.one_off_date.day = 28u;
    }

    uint32_t id = NN20CLOCK_ALARM_ID_NONE;
    (void)nn20clock_storage_save_alarm(fixture->storage, &alarm, &id);
    return id;
}

TEST(an_alarm_event_records_which_alarm_is_ringing)
{
    Fixture fixture = fixture_up();
    REQUIRE(fixture.manager != NULL);
    storage_up_empty(&fixture);
    REQUIRE(nn20clock_manager_start(fixture.manager) == ESP_OK);

    CHECK_EQ(NN20CLOCK_ALARM_ID_NONE,
             nn20clock_manager_ringing_alarm(fixture.manager));

    deliver_alarm(&fixture, 3u);

    CHECK_EQ(NN20CLOCK_STATE_ALARM_RINGING,
             nn20clock_manager_state(fixture.manager));
    CHECK_EQ(3, nn20clock_manager_ringing_alarm(fixture.manager));
    CHECK_EQ(1, nn20clock_manager_alarm_count(fixture.manager));

    fixture_down(&fixture);
}

/* Design 10: a fired one-off is removed from storage once dismissed. */
TEST(dismissing_a_one_off_alarm_deletes_it)
{
    Fixture fixture = fixture_up();
    REQUIRE(fixture.manager != NULL);
    storage_up_empty(&fixture);
    REQUIRE(nn20clock_manager_start(fixture.manager) == ESP_OK);

    const uint32_t id = store_alarm(&fixture, NN20CLOCK_ALARM_KIND_ONE_OFF);
    REQUIRE(id != NN20CLOCK_ALARM_ID_NONE);

    deliver_alarm(&fixture, id);
    REQUIRE(nn20clock_manager_state(fixture.manager)
            == NN20CLOCK_STATE_ALARM_RINGING);

    const NN20ClockUiCommand stop = { .type = NN20CLOCK_UI_COMMAND_STOP_ALARM };
    REQUIRE(nn20clock_manager_handle_ui_command(fixture.manager, &stop)
            == ESP_OK);
    flush(fixture.manager_worker);

    CHECK_EQ(NN20CLOCK_STATE_TIME, nn20clock_manager_state(fixture.manager));
    CHECK_EQ(NN20CLOCK_ALARM_ID_NONE,
             nn20clock_manager_ringing_alarm(fixture.manager));

    /* Static: over 5 KB, and the board's task stack is smaller. */
    static NN20ClockAlarmList list;
    REQUIRE(nn20clock_storage_list_alarms(fixture.storage, &list) == ESP_OK);
    CHECK_EQ(0, list.count);

    fixture_down(&fixture);
}

/* A recurrent alarm is meant to come back tomorrow, so dismissing it
 * must not delete it. */
TEST(dismissing_a_recurrent_alarm_keeps_it)
{
    Fixture fixture = fixture_up();
    REQUIRE(fixture.manager != NULL);
    storage_up_empty(&fixture);
    REQUIRE(nn20clock_manager_start(fixture.manager) == ESP_OK);

    const uint32_t id = store_alarm(&fixture, NN20CLOCK_ALARM_KIND_RECURRENT);
    REQUIRE(id != NN20CLOCK_ALARM_ID_NONE);

    deliver_alarm(&fixture, id);
    const NN20ClockUiCommand stop = { .type = NN20CLOCK_UI_COMMAND_STOP_ALARM };
    REQUIRE(nn20clock_manager_handle_ui_command(fixture.manager, &stop)
            == ESP_OK);
    flush(fixture.manager_worker);

    CHECK_EQ(NN20CLOCK_STATE_TIME, nn20clock_manager_state(fixture.manager));

    /* Static: over 5 KB, and the board's task stack is smaller. */
    static NN20ClockAlarmList list;
    REQUIRE(nn20clock_storage_list_alarms(fixture.storage, &list) == ESP_OK);
    CHECK_EQ(1, list.count);

    fixture_down(&fixture);
}

/*
 * Snoozing and dismissing are different outcomes, not two ways of
 * stopping the noise. Snoozing schedules the same alarm to come back
 * and leaves the record alone.
 */
TEST(snoozing_schedules_the_alarm_to_return)
{
    Fixture fixture = fixture_up();
    REQUIRE(fixture.manager != NULL);
    storage_up_empty(&fixture);
    REQUIRE(nn20clock_manager_start(fixture.manager) == ESP_OK);

    const uint32_t id = store_alarm(&fixture, NN20CLOCK_ALARM_KIND_RECURRENT);
    deliver_alarm(&fixture, id);

    const NN20ClockUiCommand snooze = {
        .type = NN20CLOCK_UI_COMMAND_SNOOZE_ALARM,
    };
    REQUIRE(nn20clock_manager_handle_ui_command(fixture.manager, &snooze)
            == ESP_OK);
    flush(fixture.manager_worker);

    CHECK_EQ(NN20CLOCK_STATE_TIME, nn20clock_manager_state(fixture.manager));
    CHECK_EQ(NN20CLOCK_ALARM_ID_NONE,
             nn20clock_manager_ringing_alarm(fixture.manager));

    /* The Timer is holding it, for this alarm. */
    CHECK_EQ(id, nn20clock_timer_snoozed_alarm(fixture.timer));
    CHECK(nn20clock_timer_snooze_time(fixture.timer) != 0);

    fixture_down(&fixture);
}

/* A one-off that is snoozed has not finished happening, so it must NOT
 * be deleted the way a dismissed one is. */
TEST(snoozing_a_one_off_does_not_delete_it)
{
    Fixture fixture = fixture_up();
    REQUIRE(fixture.manager != NULL);
    storage_up_empty(&fixture);
    REQUIRE(nn20clock_manager_start(fixture.manager) == ESP_OK);

    const uint32_t id = store_alarm(&fixture, NN20CLOCK_ALARM_KIND_ONE_OFF);
    deliver_alarm(&fixture, id);

    const NN20ClockUiCommand snooze = {
        .type = NN20CLOCK_UI_COMMAND_SNOOZE_ALARM,
    };
    REQUIRE(nn20clock_manager_handle_ui_command(fixture.manager, &snooze)
            == ESP_OK);
    flush(fixture.manager_worker);

    static NN20ClockAlarmList list;
    REQUIRE(nn20clock_storage_list_alarms(fixture.storage, &list) == ESP_OK);
    CHECK_EQ(1, list.count);
    CHECK_EQ(id, nn20clock_timer_snoozed_alarm(fixture.timer));

    fixture_down(&fixture);
}

/* Dismissing after snoozing must cancel it: an alarm turned off should
 * not come back nine minutes later. */
TEST(dismissing_cancels_a_pending_snooze)
{
    Fixture fixture = fixture_up();
    REQUIRE(fixture.manager != NULL);
    storage_up_empty(&fixture);
    REQUIRE(nn20clock_manager_start(fixture.manager) == ESP_OK);

    const uint32_t id = store_alarm(&fixture, NN20CLOCK_ALARM_KIND_RECURRENT);
    deliver_alarm(&fixture, id);

    const NN20ClockUiCommand snooze = {
        .type = NN20CLOCK_UI_COMMAND_SNOOZE_ALARM,
    };
    REQUIRE(nn20clock_manager_handle_ui_command(fixture.manager, &snooze)
            == ESP_OK);
    flush(fixture.manager_worker);
    REQUIRE(nn20clock_timer_snoozed_alarm(fixture.timer) == id);

    /* It rings again, and this time it is dismissed. */
    deliver_alarm(&fixture, id);
    const NN20ClockUiCommand stop = { .type = NN20CLOCK_UI_COMMAND_STOP_ALARM };
    REQUIRE(nn20clock_manager_handle_ui_command(fixture.manager, &stop)
            == ESP_OK);
    flush(fixture.manager_worker);

    CHECK_EQ(NN20CLOCK_ALARM_ID_NONE,
             nn20clock_timer_snoozed_alarm(fixture.timer));

    fixture_down(&fixture);
}

/*
 * A snooze can be given up on from the clock face, without waiting for
 * it to ring again - which is what the reminder in the corner of TimeUi
 * is for. It changes no state: the clock face is already showing.
 */
TEST(a_pending_snooze_can_be_cancelled_from_the_clock_face)
{
    Fixture fixture = fixture_up();
    REQUIRE(fixture.manager != NULL);
    storage_up_empty(&fixture);
    REQUIRE(nn20clock_manager_start(fixture.manager) == ESP_OK);

    const uint32_t id = store_alarm(&fixture, NN20CLOCK_ALARM_KIND_RECURRENT);
    deliver_alarm(&fixture, id);

    const NN20ClockUiCommand snooze = {
        .type = NN20CLOCK_UI_COMMAND_SNOOZE_ALARM,
    };
    REQUIRE(nn20clock_manager_handle_ui_command(fixture.manager, &snooze)
            == ESP_OK);
    flush(fixture.manager_worker);
    REQUIRE(nn20clock_timer_snoozed_alarm(fixture.timer) == id);
    REQUIRE(nn20clock_manager_state(fixture.manager) == NN20CLOCK_STATE_TIME);

    const NN20ClockUiCommand cancel = {
        .type = NN20CLOCK_UI_COMMAND_CANCEL_SNOOZE,
    };
    REQUIRE(nn20clock_manager_handle_ui_command(fixture.manager, &cancel)
            == ESP_OK);
    flush(fixture.manager_worker);

    CHECK_EQ(NN20CLOCK_ALARM_ID_NONE,
             nn20clock_timer_snoozed_alarm(fixture.timer));
    /* Still the clock face: cancelling a snooze is not a transition. */
    CHECK_EQ(NN20CLOCK_STATE_TIME, nn20clock_manager_state(fixture.manager));

    fixture_down(&fixture);
}

/* Cancelling when nothing is snoozed is a no-op, not a fault: the
 * command can arrive from a screen built a moment before the snooze
 * fired. */
TEST(cancelling_a_snooze_that_is_not_pending_is_harmless)
{
    Fixture fixture = fixture_up();
    REQUIRE(fixture.manager != NULL);
    storage_up_empty(&fixture);
    REQUIRE(nn20clock_manager_start(fixture.manager) == ESP_OK);

    const NN20ClockUiCommand cancel = {
        .type = NN20CLOCK_UI_COMMAND_CANCEL_SNOOZE,
    };
    CHECK_EQ(ESP_OK,
             nn20clock_manager_handle_ui_command(fixture.manager, &cancel));
    flush(fixture.manager_worker);

    CHECK_EQ(NN20CLOCK_ALARM_ID_NONE,
             nn20clock_timer_snoozed_alarm(fixture.timer));
    CHECK_EQ(NN20CLOCK_STATE_TIME, nn20clock_manager_state(fixture.manager));

    fixture_down(&fixture);
}

/* An alarm deleted while it was ringing must not break the dismissal. */
TEST(dismissing_an_alarm_that_vanished_is_harmless)
{
    Fixture fixture = fixture_up();
    REQUIRE(fixture.manager != NULL);
    storage_up_empty(&fixture);
    REQUIRE(nn20clock_manager_start(fixture.manager) == ESP_OK);

    deliver_alarm(&fixture, 99u);   /* an id storage has never heard of */

    const NN20ClockUiCommand stop = { .type = NN20CLOCK_UI_COMMAND_STOP_ALARM };
    REQUIRE(nn20clock_manager_handle_ui_command(fixture.manager, &stop)
            == ESP_OK);
    flush(fixture.manager_worker);

    CHECK_EQ(NN20CLOCK_STATE_TIME, nn20clock_manager_state(fixture.manager));

    fixture_down(&fixture);
}

TEST(stopping_the_manager_clears_a_ringing_alarm)
{
    Fixture fixture = fixture_up();
    REQUIRE(fixture.manager != NULL);
    storage_up_empty(&fixture);
    REQUIRE(nn20clock_manager_start(fixture.manager) == ESP_OK);

    deliver_alarm(&fixture, 2u);
    REQUIRE(nn20clock_manager_ringing_alarm(fixture.manager) == 2u);

    REQUIRE(nn20clock_manager_stop(fixture.manager) == ESP_OK);
    CHECK_EQ(NN20CLOCK_ALARM_ID_NONE,
             nn20clock_manager_ringing_alarm(fixture.manager));

    fixture_down(&fixture);
}

TEST(alarm_accessors_tolerate_null)
{
    CHECK_EQ(NN20CLOCK_ALARM_ID_NONE, nn20clock_manager_ringing_alarm(NULL));
    CHECK_EQ(0, nn20clock_manager_alarm_count(NULL));
    CHECK_EQ(0, nn20clock_manager_ring_deadline(NULL));
    CHECK_EQ(ESP_ERR_INVALID_ARG, nn20clock_manager_reload_alarms(NULL));
}

/* ------------------------------------------------- the ringing bound -- */

/*
 * An alarm nobody answers stops on its own. This is the bug the bound
 * exists for: before it lived here it was an lv_timer on the ringing
 * screen, and LVGL's timers do not run while the player owns the panel,
 * so a video alarm rang until somebody came home.
 */
TEST(an_unanswered_alarm_stops_after_an_hour)
{
    Fixture fixture = fixture_up();
    REQUIRE(fixture.manager != NULL);
    storage_up_empty(&fixture);
    REQUIRE(nn20clock_manager_start(fixture.manager) == ESP_OK);

    const uint32_t id = store_alarm(&fixture, NN20CLOCK_ALARM_KIND_RECURRENT);
    deliver_alarm(&fixture, id);
    REQUIRE(nn20clock_manager_state(fixture.manager)
            == NN20CLOCK_STATE_ALARM_RINGING);

    /* The last second of the hour still rings: the bound is one hour of
     * ringing, not fifty-nine minutes of it. */
    deliver_tick(&fixture,
                 ALARM_TIME + NN20CLOCK_MANAGER_MAX_RING_SECONDS - 1);
    CHECK_EQ(NN20CLOCK_STATE_ALARM_RINGING,
             nn20clock_manager_state(fixture.manager));
    CHECK_EQ(id, nn20clock_manager_ringing_alarm(fixture.manager));

    deliver_tick(&fixture, ALARM_TIME + NN20CLOCK_MANAGER_MAX_RING_SECONDS);
    CHECK_EQ(NN20CLOCK_STATE_TIME, nn20clock_manager_state(fixture.manager));
    CHECK_EQ(NN20CLOCK_ALARM_ID_NONE,
             nn20clock_manager_ringing_alarm(fixture.manager));
    CHECK_EQ(0, nn20clock_manager_ring_deadline(fixture.manager));

    fixture_down(&fixture);
}

/* The hour is measured from the alarm's own time, so a tick that
 * arrives while nothing is ringing changes nothing. */
TEST(the_hour_runs_from_the_alarms_own_time)
{
    Fixture fixture = fixture_up();
    REQUIRE(fixture.manager != NULL);
    storage_up_empty(&fixture);
    REQUIRE(nn20clock_manager_start(fixture.manager) == ESP_OK);

    deliver_tick(&fixture, ALARM_TIME - 60);
    CHECK_EQ(0, nn20clock_manager_ring_deadline(fixture.manager));
    CHECK_EQ(NN20CLOCK_STATE_TIME, nn20clock_manager_state(fixture.manager));

    const uint32_t id = store_alarm(&fixture, NN20CLOCK_ALARM_KIND_RECURRENT);
    deliver_alarm(&fixture, id);

    CHECK_EQ(ALARM_TIME + NN20CLOCK_MANAGER_MAX_RING_SECONDS,
             nn20clock_manager_ring_deadline(fixture.manager));

    fixture_down(&fixture);
}

/*
 * A snooze is the same alarm continuing, so it does not buy another
 * hour. Nine minutes of quiet is not nine minutes of credit.
 */
TEST(a_snooze_does_not_extend_the_ringing_hour)
{
    Fixture fixture = fixture_up();
    REQUIRE(fixture.manager != NULL);
    storage_up_empty(&fixture);
    REQUIRE(nn20clock_manager_start(fixture.manager) == ESP_OK);

    const uint32_t id = store_alarm(&fixture, NN20CLOCK_ALARM_KIND_RECURRENT);
    deliver_alarm(&fixture, id);

    const NN20ClockUiCommand snooze = {
        .type = NN20CLOCK_UI_COMMAND_SNOOZE_ALARM,
    };
    REQUIRE(nn20clock_manager_handle_ui_command(fixture.manager, &snooze)
            == ESP_OK);
    flush(fixture.manager_worker);

    /* Still standing while it rests: the occurrence is not over. */
    CHECK_EQ(ALARM_TIME + NN20CLOCK_MANAGER_MAX_RING_SECONDS,
             nn20clock_manager_ring_deadline(fixture.manager));

    /* Back nine minutes later, with the same deadline. */
    deliver_alarm_at(&fixture, id, ALARM_TIME + 540);
    CHECK_EQ(NN20CLOCK_STATE_ALARM_RINGING,
             nn20clock_manager_state(fixture.manager));
    CHECK_EQ(ALARM_TIME + NN20CLOCK_MANAGER_MAX_RING_SECONDS,
             nn20clock_manager_ring_deadline(fixture.manager));

    deliver_tick(&fixture, ALARM_TIME + NN20CLOCK_MANAGER_MAX_RING_SECONDS);
    CHECK_EQ(NN20CLOCK_STATE_TIME, nn20clock_manager_state(fixture.manager));

    fixture_down(&fixture);
}

/* A snooze that lands after the hour is up is not a new occurrence and
 * does not get to ring. */
TEST(an_alarm_returning_past_its_hour_does_not_ring)
{
    Fixture fixture = fixture_up();
    REQUIRE(fixture.manager != NULL);
    storage_up_empty(&fixture);
    REQUIRE(nn20clock_manager_start(fixture.manager) == ESP_OK);

    const uint32_t id = store_alarm(&fixture, NN20CLOCK_ALARM_KIND_RECURRENT);
    deliver_alarm(&fixture, id);

    const NN20ClockUiCommand snooze = {
        .type = NN20CLOCK_UI_COMMAND_SNOOZE_ALARM,
    };
    REQUIRE(nn20clock_manager_handle_ui_command(fixture.manager, &snooze)
            == ESP_OK);
    flush(fixture.manager_worker);

    deliver_alarm_at(&fixture, id,
                     ALARM_TIME + NN20CLOCK_MANAGER_MAX_RING_SECONDS + 60);

    CHECK_EQ(NN20CLOCK_STATE_TIME, nn20clock_manager_state(fixture.manager));
    CHECK_EQ(NN20CLOCK_ALARM_ID_NONE,
             nn20clock_manager_ringing_alarm(fixture.manager));
    CHECK_EQ(0, nn20clock_manager_ring_deadline(fixture.manager));

    fixture_down(&fixture);
}

/* Tomorrow's alarm gets its own hour, not the remains of today's. */
TEST(the_next_alarm_gets_a_fresh_hour)
{
    Fixture fixture = fixture_up();
    REQUIRE(fixture.manager != NULL);
    storage_up_empty(&fixture);
    REQUIRE(nn20clock_manager_start(fixture.manager) == ESP_OK);

    const uint32_t id = store_alarm(&fixture, NN20CLOCK_ALARM_KIND_RECURRENT);
    deliver_alarm(&fixture, id);

    const NN20ClockUiCommand stop = { .type = NN20CLOCK_UI_COMMAND_STOP_ALARM };
    REQUIRE(nn20clock_manager_handle_ui_command(fixture.manager, &stop)
            == ESP_OK);
    flush(fixture.manager_worker);
    CHECK_EQ(0, nn20clock_manager_ring_deadline(fixture.manager));

    const time_t tomorrow = ALARM_TIME + 86400;
    deliver_alarm_at(&fixture, id, tomorrow);

    CHECK_EQ(NN20CLOCK_STATE_ALARM_RINGING,
             nn20clock_manager_state(fixture.manager));
    CHECK_EQ(tomorrow + NN20CLOCK_MANAGER_MAX_RING_SECONDS,
             nn20clock_manager_ring_deadline(fixture.manager));

    fixture_down(&fixture);
}

/*
 * Cancelling a snooze from the clock face ends the occurrence, so its
 * deadline goes with it - otherwise the next alarm would inherit one
 * that has already passed and be silenced before it rang.
 */
TEST(cancelling_a_snooze_clears_the_ringing_deadline)
{
    Fixture fixture = fixture_up();
    REQUIRE(fixture.manager != NULL);
    storage_up_empty(&fixture);
    REQUIRE(nn20clock_manager_start(fixture.manager) == ESP_OK);

    const uint32_t id = store_alarm(&fixture, NN20CLOCK_ALARM_KIND_RECURRENT);
    deliver_alarm(&fixture, id);

    const NN20ClockUiCommand snooze = {
        .type = NN20CLOCK_UI_COMMAND_SNOOZE_ALARM,
    };
    REQUIRE(nn20clock_manager_handle_ui_command(fixture.manager, &snooze)
            == ESP_OK);
    flush(fixture.manager_worker);
    REQUIRE(nn20clock_manager_ring_deadline(fixture.manager) != 0);

    const NN20ClockUiCommand cancel = {
        .type = NN20CLOCK_UI_COMMAND_CANCEL_SNOOZE,
    };
    REQUIRE(nn20clock_manager_handle_ui_command(fixture.manager, &cancel)
            == ESP_OK);
    flush(fixture.manager_worker);

    CHECK_EQ(0, nn20clock_manager_ring_deadline(fixture.manager));

    fixture_down(&fixture);
}

/*
 * A device whose clock is not set has no time to measure an hour from.
 * Bounding against a zero epoch would stop the alarm in its first
 * second, which is worse than not bounding it at all.
 */
TEST(an_alarm_with_no_clock_behind_it_is_not_bounded)
{
    Fixture fixture = fixture_up();
    REQUIRE(fixture.manager != NULL);
    storage_up_empty(&fixture);
    REQUIRE(nn20clock_manager_start(fixture.manager) == ESP_OK);

    const uint32_t id = store_alarm(&fixture, NN20CLOCK_ALARM_KIND_RECURRENT);
    deliver_alarm_at(&fixture, id, (time_t)0);

    CHECK_EQ(NN20CLOCK_STATE_ALARM_RINGING,
             nn20clock_manager_state(fixture.manager));
    CHECK_EQ(0, nn20clock_manager_ring_deadline(fixture.manager));

    deliver_tick(&fixture, (time_t)0);
    CHECK_EQ(NN20CLOCK_STATE_ALARM_RINGING,
             nn20clock_manager_state(fixture.manager));

    fixture_down(&fixture);
}

/* ---------------------------------------------- device settings (M5) -- */

/* Design 5 gained DEVICE_SETTINGS: it mirrors ALARM_SETTINGS, reachable
 * from TIME, leavable to TIME, interruptible by an alarm. */
/*
 * ------------------------------------------------------------------
 * The end of an occurrence
 * ------------------------------------------------------------------
 *
 * Four different things end one and only two of them are state changes,
 * which is why there is a callback for it rather than a rule written
 * against the ALARM_RINGING edge. A snooze must not report: whatever the
 * subscriber keeps for the occurrence is what the snooze is coming back
 * for.
 */

typedef struct {
    unsigned calls;
    uint32_t last_alarm_id;
} EndedLog;

static void record_ended(uint32_t alarm_id, void *user_data)
{
    EndedLog *log = user_data;
    log->calls++;
    log->last_alarm_id = alarm_id;
}

/* Up, with storage, an alarm stored, and the callback watching. */
static uint32_t ringing_with_ended_log(Fixture *fixture, EndedLog *log)
{
    storage_up_empty(fixture);
    REQUIRE(nn20clock_manager_set_ringing_ended_callback(
                fixture->manager, record_ended, log) == ESP_OK);
    REQUIRE(nn20clock_manager_start(fixture->manager) == ESP_OK);

    const uint32_t id = store_alarm(fixture, NN20CLOCK_ALARM_KIND_RECURRENT);
    deliver_alarm(fixture, id);
    CHECK_EQ(0u, log->calls);   /* ringing is not ending */
    return id;
}

TEST(dismissing_an_alarm_reports_the_occurrence_as_over)
{
    Fixture fixture = fixture_up();
    REQUIRE(fixture.manager != NULL);
    EndedLog log = { 0 };
    const uint32_t id = ringing_with_ended_log(&fixture, &log);

    const NN20ClockUiCommand stop = { .type = NN20CLOCK_UI_COMMAND_STOP_ALARM };
    REQUIRE(nn20clock_manager_handle_ui_command(fixture.manager, &stop)
            == ESP_OK);
    flush(fixture.manager_worker);

    CHECK_EQ(1u, log.calls);
    CHECK_EQ(id, log.last_alarm_id);

    fixture_down(&fixture);
}

TEST(snoozing_an_alarm_reports_nothing)
{
    Fixture fixture = fixture_up();
    REQUIRE(fixture.manager != NULL);
    EndedLog log = { 0 };
    (void)ringing_with_ended_log(&fixture, &log);

    const NN20ClockUiCommand snooze = {
        .type = NN20CLOCK_UI_COMMAND_SNOOZE_ALARM,
    };
    REQUIRE(nn20clock_manager_handle_ui_command(fixture.manager, &snooze)
            == ESP_OK);
    flush(fixture.manager_worker);

    /* The occurrence is paused, not over: the film it rolled and the
     * position it reached are what it comes back to. */
    CHECK_EQ(0u, log.calls);

    fixture_down(&fixture);
}

TEST(an_alarm_that_rings_its_hour_out_reports_the_occurrence_as_over)
{
    Fixture fixture = fixture_up();
    REQUIRE(fixture.manager != NULL);
    EndedLog log = { 0 };
    const uint32_t id = ringing_with_ended_log(&fixture, &log);

    deliver_tick(&fixture, ALARM_TIME + NN20CLOCK_MANAGER_MAX_RING_SECONDS);

    CHECK_EQ(NN20CLOCK_STATE_TIME, nn20clock_manager_state(fixture.manager));
    CHECK_EQ(1u, log.calls);
    CHECK_EQ(id, log.last_alarm_id);

    fixture_down(&fixture);
}

/*
 * The path no state change can be read for: the snooze fires after the
 * hour is up, so it is ended without ever ringing and the clock face was
 * showing throughout.
 */
TEST(a_snooze_returning_past_its_hour_reports_the_occurrence_as_over)
{
    Fixture fixture = fixture_up();
    REQUIRE(fixture.manager != NULL);
    EndedLog log = { 0 };
    const uint32_t id = ringing_with_ended_log(&fixture, &log);

    const NN20ClockUiCommand snooze = {
        .type = NN20CLOCK_UI_COMMAND_SNOOZE_ALARM,
    };
    REQUIRE(nn20clock_manager_handle_ui_command(fixture.manager, &snooze)
            == ESP_OK);
    flush(fixture.manager_worker);
    CHECK_EQ(0u, log.calls);

    deliver_alarm_at(&fixture, id,
                     ALARM_TIME + NN20CLOCK_MANAGER_MAX_RING_SECONDS + 60);

    CHECK_EQ(NN20CLOCK_STATE_TIME, nn20clock_manager_state(fixture.manager));
    CHECK_EQ(1u, log.calls);
    CHECK_EQ(id, log.last_alarm_id);

    fixture_down(&fixture);
}

/* The other one: the reminder on the clock face, cancelled by hand. */
TEST(cancelling_a_snooze_reports_the_occurrence_as_over)
{
    Fixture fixture = fixture_up();
    REQUIRE(fixture.manager != NULL);
    EndedLog log = { 0 };
    const uint32_t id = ringing_with_ended_log(&fixture, &log);

    const NN20ClockUiCommand snooze = {
        .type = NN20CLOCK_UI_COMMAND_SNOOZE_ALARM,
    };
    REQUIRE(nn20clock_manager_handle_ui_command(fixture.manager, &snooze)
            == ESP_OK);
    flush(fixture.manager_worker);

    const NN20ClockUiCommand cancel = {
        .type = NN20CLOCK_UI_COMMAND_CANCEL_SNOOZE,
    };
    REQUIRE(nn20clock_manager_handle_ui_command(fixture.manager, &cancel)
            == ESP_OK);
    flush(fixture.manager_worker);

    CHECK_EQ(1u, log.calls);
    CHECK_EQ(id, log.last_alarm_id);

    fixture_down(&fixture);
}

TEST(the_ringing_ended_callback_can_only_be_set_before_start)
{
    Fixture fixture = fixture_up();
    REQUIRE(fixture.manager != NULL);
    EndedLog log = { 0 };

    REQUIRE(nn20clock_manager_start(fixture.manager) == ESP_OK);
    CHECK_EQ(ESP_ERR_INVALID_STATE,
             nn20clock_manager_set_ringing_ended_callback(fixture.manager,
                                                          record_ended, &log));
    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_manager_set_ringing_ended_callback(NULL, record_ended,
                                                          &log));
    fixture_down(&fixture);
}

TEST(the_device_settings_state_mirrors_alarm_settings)
{
    CHECK(nn20clock_manager_transition_allowed(
        NN20CLOCK_STATE_TIME, NN20CLOCK_STATE_DEVICE_SETTINGS));
    CHECK(nn20clock_manager_transition_allowed(
        NN20CLOCK_STATE_DEVICE_SETTINGS, NN20CLOCK_STATE_TIME));
    CHECK(nn20clock_manager_transition_allowed(
        NN20CLOCK_STATE_DEVICE_SETTINGS, NN20CLOCK_STATE_ALARM_RINGING));
    CHECK(nn20clock_manager_transition_allowed(
        NN20CLOCK_STATE_DEVICE_SETTINGS, NN20CLOCK_STATE_ERROR));

    /* The two settings screens are separate entries from TIME, not
     * doors into each other. */
    CHECK(!nn20clock_manager_transition_allowed(
        NN20CLOCK_STATE_DEVICE_SETTINGS, NN20CLOCK_STATE_ALARM_SETTINGS));
    CHECK(!nn20clock_manager_transition_allowed(
        NN20CLOCK_STATE_ALARM_SETTINGS, NN20CLOCK_STATE_DEVICE_SETTINGS));

    CHECK_STR_EQ("DEVICE_SETTINGS", nn20clock_manager_state_name(
                                        NN20CLOCK_STATE_DEVICE_SETTINGS));
}

TEST(the_gear_command_opens_device_settings)
{
    Fixture fixture = fixture_up();
    REQUIRE(fixture.manager != NULL);
    REQUIRE(nn20clock_manager_start(fixture.manager) == ESP_OK);

    const NN20ClockUiCommand open = {
        .type = NN20CLOCK_UI_COMMAND_OPEN_DEVICE_SETTINGS,
    };
    REQUIRE(nn20clock_manager_handle_ui_command(fixture.manager, &open)
            == ESP_OK);
    flush(fixture.manager_worker);
    CHECK_EQ(NN20CLOCK_STATE_DEVICE_SETTINGS,
             nn20clock_manager_state(fixture.manager));

    const NN20ClockUiCommand close = {
        .type = NN20CLOCK_UI_COMMAND_CLOSE_SETTINGS,
    };
    REQUIRE(nn20clock_manager_handle_ui_command(fixture.manager, &close)
            == ESP_OK);
    flush(fixture.manager_worker);
    CHECK_EQ(NN20CLOCK_STATE_TIME, nn20clock_manager_state(fixture.manager));

    fixture_down(&fixture);
}

typedef struct {
    int brightness_calls;
    uint8_t last_brightness;
    int current_brightness_calls;
    int connect_calls;
    char last_ssid[64];
} HookLog;

static HookLog g_hook_log;

static esp_err_t fake_brightness(void *ctx, uint8_t percent)
{
    (void)ctx;
    g_hook_log.brightness_calls++;
    g_hook_log.last_brightness = percent;
    return ESP_OK;
}

static uint8_t fake_current_brightness(void *ctx)
{
    (void)ctx;
    g_hook_log.current_brightness_calls++;
    return g_hook_log.last_brightness;
}

static esp_err_t fake_connect(void *ctx, const char *ssid,
                              const char *password)
{
    (void)ctx;
    (void)password;   /* never recorded, never logged */
    g_hook_log.connect_calls++;
    snprintf(g_hook_log.last_ssid, sizeof(g_hook_log.last_ssid), "%s", ssid);
    return ESP_OK;
}

/* The service is what the settings screen is handed; config goes to
 * storage and the hardware calls go through the installed hooks. */
TEST(the_device_service_reaches_storage_and_the_hooks)
{
    Fixture fixture = fixture_up();
    REQUIRE(fixture.manager != NULL);
    REQUIRE(nn20clock_storage_start(fixture.storage) == ESP_OK);

    memset(&g_hook_log, 0, sizeof(g_hook_log));
    const NN20ClockDeviceHooks hooks = {
        .apply_brightness = fake_brightness,
        .current_brightness = fake_current_brightness,
        .connect_wifi = fake_connect,
        .ctx = NULL,
    };
    REQUIRE(nn20clock_manager_set_device_hooks(fixture.manager, hooks)
            == ESP_OK);

    const NN20ClockDeviceService service =
        nn20clock_manager_device_service(fixture.manager);

    NN20ClockConfig config;
    CHECK_EQ(ESP_OK, service.load_config(service.ctx, &config));

    snprintf(config.wifi_ssid, sizeof(config.wifi_ssid), "somewhere");
    snprintf(config.wifi_password, sizeof(config.wifi_password), "secret");
    config.brightness_day = 60u;
    CHECK_EQ(ESP_OK, service.save_config(service.ctx, &config));

    /* Round-trips through storage, credentials included. */
    NN20ClockConfig reloaded;
    REQUIRE(service.load_config(service.ctx, &reloaded) == ESP_OK);
    CHECK_STR_EQ("somewhere", reloaded.wifi_ssid);
    CHECK_STR_EQ("secret", reloaded.wifi_password);
    CHECK_EQ(60, reloaded.brightness_day);

    CHECK_EQ(ESP_OK, service.apply_brightness(service.ctx, 42u));
    CHECK_EQ(1, g_hook_log.brightness_calls);
    CHECK_EQ(42, g_hook_log.last_brightness);

    /* What the panel is at, which with a schedule is not the same
     * question as what is stored - the playback screen's slider starts
     * from this rather than from config. */
    CHECK_EQ(42, service.current_brightness(service.ctx));
    CHECK_EQ(1, g_hook_log.current_brightness_calls);

    CHECK_EQ(ESP_OK, service.connect_wifi(service.ctx, "somewhere", "secret"));
    CHECK_EQ(1, g_hook_log.connect_calls);
    CHECK_STR_EQ("somewhere", g_hook_log.last_ssid);

    fixture_down(&fixture);
}

/* Without hooks - the headless build, and the host tests - the hardware
 * half reports not-supported rather than pretending. */
TEST(the_device_service_without_hooks_says_so)
{
    Fixture fixture = fixture_up();
    REQUIRE(fixture.manager != NULL);
    REQUIRE(nn20clock_storage_start(fixture.storage) == ESP_OK);

    const NN20ClockDeviceService service =
        nn20clock_manager_device_service(fixture.manager);

    CHECK_EQ(ESP_ERR_NOT_SUPPORTED, service.apply_brightness(service.ctx, 50u));
    CHECK_EQ(ESP_ERR_NOT_SUPPORTED,
             service.connect_wifi(service.ctx, "x", "y"));

    /* Storage still works: it is not hardware. */
    NN20ClockConfig config;
    CHECK_EQ(ESP_OK, service.load_config(service.ctx, &config));

    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_manager_set_device_hooks(NULL,
                                                (NN20ClockDeviceHooks){0}));

    fixture_down(&fixture);
}

/* --------------------------------------------------- media playback -- */

/* What the composition root's play_media() hook does, minus the player
 * and the screen: record that it was asked, and for what. */
typedef struct {
    unsigned calls;
    char name[64];
    uint32_t sleep_ms;
    esp_err_t result;
} MediaSpy;

static esp_err_t media_spy_play(void *ctx, const char *name,
                                uint32_t sleep_ms)
{
    MediaSpy *spy = ctx;

    spy->calls++;
    snprintf(spy->name, sizeof(spy->name), "%s", name);
    spy->sleep_ms = sleep_ms;
    return spy->result;
}

TEST(the_play_icon_opens_media_playback_and_stopping_returns)
{
    Fixture fixture = fixture_up();
    REQUIRE(fixture.manager != NULL);
    REQUIRE(nn20clock_manager_start(fixture.manager) == ESP_OK);

    const NN20ClockUiCommand open = {
        .type = NN20CLOCK_UI_COMMAND_OPEN_MEDIA_PLAYBACK,
    };
    REQUIRE(nn20clock_manager_handle_ui_command(fixture.manager, &open)
            == ESP_OK);
    flush(fixture.manager_worker);
    CHECK_EQ(NN20CLOCK_STATE_MEDIA_PLAYBACK,
             nn20clock_manager_state(fixture.manager));

    /*
     * A separate command from CLOSE_SETTINGS even though both land on
     * TIME today: closing a settings screen and stopping a film are
     * different events, and design 17's Milestone 9 gives the second
     * one a sleep timer that can end it too.
     */
    const NN20ClockUiCommand close = {
        .type = NN20CLOCK_UI_COMMAND_CLOSE_MEDIA_PLAYBACK,
    };
    REQUIRE(nn20clock_manager_handle_ui_command(fixture.manager, &close)
            == ESP_OK);
    flush(fixture.manager_worker);
    CHECK_EQ(NN20CLOCK_STATE_TIME, nn20clock_manager_state(fixture.manager));

    fixture_down(&fixture);
}

TEST(the_media_service_reaches_the_hook)
{
    Fixture fixture = fixture_up();
    REQUIRE(fixture.manager != NULL);

    MediaSpy spy = { .result = ESP_OK };
    const NN20ClockMediaHooks hooks = {
        .play_media = media_spy_play,
        .ctx = &spy,
    };
    REQUIRE(nn20clock_manager_set_media_hooks(fixture.manager, hooks)
            == ESP_OK);
    REQUIRE(nn20clock_manager_start(fixture.manager) == ESP_OK);

    const NN20ClockMediaService service =
        nn20clock_manager_media_service(fixture.manager);

    CHECK_EQ(ESP_OK, service.play(service.ctx, "DUCK720.AVI", 0u));
    CHECK_EQ(1u, spy.calls);
    CHECK_STR_EQ("DUCK720.AVI", spy.name);
    /* Zero is design 11's `all`: play the media once and stop. */
    CHECK_EQ(0u, spy.sleep_ms);

    /* The sleep timer travels with the request rather than being
     * stored: it belongs to this playback, not to the device. */
    CHECK_EQ(ESP_OK, service.play(service.ctx, "DUCK720.AVI", 1800000u));
    CHECK_EQ(1800000u, spy.sleep_ms);

    /* Whatever the hook says comes back, so a screen can leave its list
     * up when nothing started. */
    spy.result = ESP_ERR_NOT_FOUND;
    CHECK_EQ(ESP_ERR_NOT_FOUND, service.play(service.ctx, "GONE.AVI", 0u));

    /* A name is required: there is no "play nothing". */
    CHECK_EQ(ESP_ERR_INVALID_ARG, service.play(service.ctx, NULL, 0u));
    CHECK_EQ(ESP_ERR_INVALID_ARG, service.play(service.ctx, "", 0u));
    CHECK_EQ(3u, spy.calls);

    fixture_down(&fixture);
}

TEST(the_media_service_without_hooks_says_so)
{
    Fixture fixture = fixture_up();
    REQUIRE(fixture.manager != NULL);

    /* The host build has no player and no screens. Saying
     * NOT_SUPPORTED is the truth here, not a failure. */
    const NN20ClockMediaService service =
        nn20clock_manager_media_service(fixture.manager);
    CHECK_EQ(ESP_ERR_NOT_SUPPORTED, service.play(service.ctx, "X.AVI", 0u));

    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_manager_set_media_hooks(NULL,
                                               (NN20ClockMediaHooks){0}));

    fixture_down(&fixture);
}

/* The hooks are read from the manager's own worker once it is running,
 * so they may only be installed before that - the same rule as the
 * device hooks and the screen factory. */
TEST(the_media_hooks_can_only_be_set_before_start)
{
    Fixture fixture = fixture_up();
    REQUIRE(fixture.manager != NULL);
    REQUIRE(nn20clock_manager_start(fixture.manager) == ESP_OK);

    MediaSpy spy = { .result = ESP_OK };
    const NN20ClockMediaHooks hooks = {
        .play_media = media_spy_play,
        .ctx = &spy,
    };
    CHECK_EQ(ESP_ERR_INVALID_STATE,
             nn20clock_manager_set_media_hooks(fixture.manager, hooks));

    fixture_down(&fixture);
}

TEST_MAIN("nn20clock_manager")
{
    RUN(the_table_matches_design_5);
    RUN(the_table_has_no_edges_design_5_does_not_draw);
    RUN(the_table_rejects_out_of_range_states);
    RUN(every_state_has_a_name);
    RUN(ctor_requires_a_worker_and_a_timer);
    RUN(ctor_tolerates_a_null_storage);
    RUN(dtor_and_accessors_tolerate_null);
    RUN(start_subscribes_to_the_timer_and_enters_time);
    RUN(stop_unsubscribes_and_returns_to_boot);
    RUN(a_requested_transition_is_applied_on_the_worker);
    RUN(a_disallowed_transition_is_counted_not_applied);
    RUN(request_state_rejects_out_of_range_values);
    RUN(the_state_callback_can_only_be_set_before_start);
    RUN(a_timer_event_is_applied_on_the_manager_worker);
    RUN(an_alarm_event_enters_alarm_ringing);
    RUN(a_timer_error_event_enters_the_error_state);
    RUN(the_timer_subscriber_tolerates_null);
    RUN(a_ui_command_routes_to_a_state_change);
    RUN(media_management_opens_and_closes_inside_the_settings_screen);
    RUN(media_management_cannot_be_opened_from_the_clock_face);
    RUN(stopping_an_alarm_returns_to_time);
    RUN(commands_with_no_service_behind_them_are_inert);
    RUN(handle_ui_command_rejects_null);
    RUN(switching_screens_destroys_the_old_one_first);
    RUN(a_timer_event_reaches_the_active_screen);

    RUN(entering_a_state_builds_its_screen);
    RUN(changing_state_swaps_the_screen);
    RUN(a_state_with_no_screen_clears_the_display);
    RUN(no_factory_means_no_screens_and_no_complaints);
    RUN(the_screen_factory_can_only_be_set_before_start);

    RUN(an_alarm_event_records_which_alarm_is_ringing);
    RUN(dismissing_a_one_off_alarm_deletes_it);
    RUN(dismissing_a_recurrent_alarm_keeps_it);
    RUN(snoozing_schedules_the_alarm_to_return);
    RUN(snoozing_a_one_off_does_not_delete_it);
    RUN(dismissing_cancels_a_pending_snooze);
    RUN(a_pending_snooze_can_be_cancelled_from_the_clock_face);
    RUN(cancelling_a_snooze_that_is_not_pending_is_harmless);
    RUN(dismissing_an_alarm_that_vanished_is_harmless);
    RUN(stopping_the_manager_clears_a_ringing_alarm);
    RUN(alarm_accessors_tolerate_null);

    RUN(an_unanswered_alarm_stops_after_an_hour);
    RUN(the_hour_runs_from_the_alarms_own_time);
    RUN(a_snooze_does_not_extend_the_ringing_hour);
    RUN(an_alarm_returning_past_its_hour_does_not_ring);
    RUN(the_next_alarm_gets_a_fresh_hour);
    RUN(cancelling_a_snooze_clears_the_ringing_deadline);
    RUN(an_alarm_with_no_clock_behind_it_is_not_bounded);

    RUN(dismissing_an_alarm_reports_the_occurrence_as_over);
    RUN(snoozing_an_alarm_reports_nothing);
    RUN(an_alarm_that_rings_its_hour_out_reports_the_occurrence_as_over);
    RUN(a_snooze_returning_past_its_hour_reports_the_occurrence_as_over);
    RUN(cancelling_a_snooze_reports_the_occurrence_as_over);
    RUN(the_ringing_ended_callback_can_only_be_set_before_start);

    RUN(the_device_settings_state_mirrors_alarm_settings);
    RUN(the_gear_command_opens_device_settings);
    RUN(the_device_service_reaches_storage_and_the_hooks);
    RUN(the_device_service_without_hooks_says_so);

    RUN(the_play_icon_opens_media_playback_and_stopping_returns);
    RUN(the_media_service_reaches_the_hook);
    RUN(the_media_service_without_hooks_says_so);
    RUN(the_media_hooks_can_only_be_set_before_start);
}
