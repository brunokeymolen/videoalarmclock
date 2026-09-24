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
 * Component tests for UiBase (design 11).
 *
 * The thing under test is the dispatch rule: whoever calls, the screen's
 * hooks run on the UiWorker and nowhere else. That is what keeps LVGL
 * single-threaded once there is LVGL to keep, so it is worth testing
 * before any screen exists.
 */
#include "test_util.h"

#include "nn20clock_ui.h"

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

/* ------------------------------------------------------ fake screen -- */

/*
 * What a real screen will look like: UiBase first, own state after, so a
 * NN20ClockUiBase* casts to the screen. Counters live in a separate
 * struct because the destroy hook frees the screen itself.
 */
typedef struct {
    int shows;
    int hides;
    int touches;
    int timer_events;
    int destroys;

    NN20ClockTouchEvent last_touch;
    NN20ClockTimerEventType last_timer_type;
    time_t last_displayed_time;
    uint32_t last_alarm_id;

    esp_err_t show_result;   /* let a test make show() fail */
} ScreenLog;

typedef struct {
    NN20ClockUiBase super;
    ScreenLog *log;
} FakeScreen;

static esp_err_t fake_show(NN20ClockUiBase *ui)
{
    FakeScreen *screen = (FakeScreen *)ui;
    screen->log->shows++;
    return screen->log->show_result;
}

static esp_err_t fake_hide(NN20ClockUiBase *ui)
{
    ((FakeScreen *)ui)->log->hides++;
    return ESP_OK;
}

static esp_err_t fake_touch(NN20ClockUiBase *ui,
                            const NN20ClockTouchEvent *event)
{
    FakeScreen *screen = (FakeScreen *)ui;
    screen->log->touches++;
    screen->log->last_touch = *event;
    return ESP_OK;
}

static esp_err_t fake_timer_event(NN20ClockUiBase *ui,
                                  const NN20ClockTimerEvent *event)
{
    FakeScreen *screen = (FakeScreen *)ui;
    screen->log->timer_events++;
    screen->log->last_timer_type = event->type;
    screen->log->last_displayed_time =
        (event->payload != NULL) ? event->payload->displayed_time : 0;

    if (event->type == NN20CLOCK_TIMER_EVENT_ALARM && event->payload != NULL) {
        /* Design 6: downcast, valid because the base sits at offset
         * zero of the derived payload. */
        const NN20ClockTimerAlarmPayload *alarm =
            (const NN20ClockTimerAlarmPayload *)event->payload;
        screen->log->last_alarm_id = alarm->alarm_id;
    }
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
    .hide = fake_hide,
    .handle_touch = fake_touch,
    .handle_timer_event = fake_timer_event,
    .destroy = fake_destroy,
};

/* Every hook NULL: the dispatcher must cope, so a screen with nothing to
 * do on hide does not have to write an empty function. */
static const NN20ClockUiVTable EMPTY_VTABLE = {0};

static FakeScreen *fake_screen_new(nn20_worker_ctx *worker, ScreenLog *log,
                                   const NN20ClockUiVTable *vtable,
                                   NN20ClockManager *manager,
                                   NN20ClockUiCommandFn on_command)
{
    FakeScreen *screen = calloc(1, sizeof(*screen));
    if (screen == NULL) {
        return NULL;
    }

    const NN20ClockUiBaseConfig config = {
        .vtable = vtable,
        .name = "FakeScreen",
        .worker = worker,
        .manager = manager,
        .on_command = on_command,
    };
    if (nn20clock_ui_base_init(&screen->super, &config) != ESP_OK) {
        free(screen);
        return NULL;
    }

    screen->log = log;
    return screen;
}

/* --------------------------------------------------------- lifecycle -- */

TEST(base_init_rejects_missing_pieces)
{
    nn20_worker_ctx *worker = nn20_worker_create();
    REQUIRE(worker != NULL);

    NN20ClockUiBase base;
    NN20ClockUiBaseConfig config = {
        .vtable = &FAKE_VTABLE,
        .name = "X",
        .worker = worker,
    };

    CHECK_EQ(ESP_ERR_INVALID_ARG, nn20clock_ui_base_init(NULL, &config));
    CHECK_EQ(ESP_ERR_INVALID_ARG, nn20clock_ui_base_init(&base, NULL));

    config.vtable = NULL;
    CHECK_EQ(ESP_ERR_INVALID_ARG, nn20clock_ui_base_init(&base, &config));
    config.vtable = &FAKE_VTABLE;

    config.name = NULL;
    CHECK_EQ(ESP_ERR_INVALID_ARG, nn20clock_ui_base_init(&base, &config));
    config.name = "X";

    /* No UiWorker is the interesting one: design 11 requires the shared
     * worker to be passed in, and a screen without one would have to
     * touch LVGL from whatever thread called it. */
    config.worker = NULL;
    CHECK_EQ(ESP_ERR_INVALID_ARG, nn20clock_ui_base_init(&base, &config));

    nn20_worker_delete(worker);
}

TEST(base_init_stores_what_it_was_given)
{
    nn20_worker_ctx *worker = nn20_worker_create();
    REQUIRE(worker != NULL);

    ScreenLog log = {0};
    FakeScreen *screen = fake_screen_new(worker, &log, &FAKE_VTABLE, NULL,
                                         NULL);
    REQUIRE(screen != NULL);

    CHECK_STR_EQ("FakeScreen", nn20clock_ui_name(&screen->super));
    CHECK(nn20clock_ui_worker(&screen->super) == worker);
    CHECK(nn20clock_ui_manager(&screen->super) == NULL);
    CHECK(!nn20clock_ui_is_visible(&screen->super));

    nn20clock_ui_destroy(&screen->super);
    nn20_worker_delete(worker);
}

TEST(accessors_tolerate_null)
{
    CHECK_STR_EQ("(none)", nn20clock_ui_name(NULL));
    CHECK(nn20clock_ui_worker(NULL) == NULL);
    CHECK(nn20clock_ui_manager(NULL) == NULL);
    CHECK(!nn20clock_ui_is_visible(NULL));
    nn20clock_ui_destroy(NULL);
    nn20clock_ui_base_deinit(NULL);
}

/* --------------------------------------------------------- dispatch --- */

TEST(show_and_hide_run_on_the_ui_worker)
{
    nn20_worker_ctx *worker = nn20_worker_create();
    REQUIRE(worker != NULL);

    ScreenLog log = {0};
    FakeScreen *screen = fake_screen_new(worker, &log, &FAKE_VTABLE, NULL,
                                         NULL);
    REQUIRE(screen != NULL);

    CHECK_EQ(ESP_OK, nn20clock_ui_show(&screen->super));
    /* Asynchronous by contract: the hook has probably not run yet. The
     * barrier, not a sleep, is what makes this deterministic. */
    flush(worker);
    CHECK_EQ(1, log.shows);
    CHECK(nn20clock_ui_is_visible(&screen->super));

    CHECK_EQ(ESP_OK, nn20clock_ui_hide(&screen->super));
    flush(worker);
    CHECK_EQ(1, log.hides);
    CHECK(!nn20clock_ui_is_visible(&screen->super));

    nn20clock_ui_destroy(&screen->super);
    nn20_worker_delete(worker);
}

/* A screen that fails to show is not on the display, and must not be
 * left believing it is. */
TEST(a_failed_show_leaves_the_screen_hidden)
{
    nn20_worker_ctx *worker = nn20_worker_create();
    REQUIRE(worker != NULL);

    ScreenLog log = { .show_result = ESP_FAIL };
    FakeScreen *screen = fake_screen_new(worker, &log, &FAKE_VTABLE, NULL,
                                         NULL);
    REQUIRE(screen != NULL);

    CHECK_EQ(ESP_OK, nn20clock_ui_show(&screen->super));
    flush(worker);
    CHECK_EQ(1, log.shows);
    CHECK(!nn20clock_ui_is_visible(&screen->super));

    nn20clock_ui_destroy(&screen->super);
    nn20_worker_delete(worker);
}

TEST(touch_events_reach_the_screen_by_value)
{
    nn20_worker_ctx *worker = nn20_worker_create();
    REQUIRE(worker != NULL);

    ScreenLog log = {0};
    FakeScreen *screen = fake_screen_new(worker, &log, &FAKE_VTABLE, NULL,
                                         NULL);
    REQUIRE(screen != NULL);

    NN20ClockTouchEvent touch = {
        .type = NN20CLOCK_TOUCH_PRESS,
        .x = 360u,
        .y = 719u,   /* bottom edge of the 720x720 panel */
    };
    CHECK_EQ(ESP_OK, nn20clock_ui_handle_touch(&screen->super, &touch));

    /* The caller's event is gone by the time the hook runs; the copy is
     * what must arrive. */
    touch.x = 0u;
    touch.y = 0u;

    flush(worker);
    CHECK_EQ(1, log.touches);
    CHECK_EQ(NN20CLOCK_TOUCH_PRESS, log.last_touch.type);
    CHECK_EQ(360u, log.last_touch.x);
    CHECK_EQ(719u, log.last_touch.y);

    nn20clock_ui_destroy(&screen->super);
    nn20_worker_delete(worker);
}

/* A timer event's payload is valid only inside the Timer's callback, so
 * the dispatcher has to copy it - otherwise the screen reads a dangling
 * pointer one thread hop later. */
TEST(timer_events_arrive_with_a_copied_payload)
{
    nn20_worker_ctx *worker = nn20_worker_create();
    REQUIRE(worker != NULL);

    ScreenLog log = {0};
    FakeScreen *screen = fake_screen_new(worker, &log, &FAKE_VTABLE, NULL,
                                         NULL);
    REQUIRE(screen != NULL);

    NN20ClockTimerPayload payload = { .displayed_time = 1735689600 };
    const NN20ClockTimerEvent event = {
        .type = NN20CLOCK_TIMER_EVENT_MINUTE,
        .payload = &payload,
    };
    CHECK_EQ(ESP_OK, nn20clock_ui_handle_timer_event(&screen->super, &event));

    payload.displayed_time = 0;   /* the Timer's payload is transient */

    flush(worker);
    CHECK_EQ(1, log.timer_events);
    CHECK_EQ(NN20CLOCK_TIMER_EVENT_MINUTE, log.last_timer_type);
    CHECK_EQ(1735689600, log.last_displayed_time);

    nn20clock_ui_destroy(&screen->super);
    nn20_worker_delete(worker);
}

/*
 * An alarm event's payload derives from the base and carries more than
 * it does. Copying only the base would drop the alarm id silently, and
 * a screen would show the wrong alarm rather than fail.
 */
TEST(a_derived_payload_survives_the_dispatch)
{
    nn20_worker_ctx *worker = nn20_worker_create();
    REQUIRE(worker != NULL);

    ScreenLog log = {0};
    FakeScreen *screen = fake_screen_new(worker, &log, &FAKE_VTABLE, NULL,
                                         NULL);
    REQUIRE(screen != NULL);

    NN20ClockTimerAlarmPayload payload = {
        .super = { .displayed_time = 1787927400 },
        .alarm_id = 7u,
        .late_seconds = 3u,
    };
    const NN20ClockTimerEvent event = {
        .type = NN20CLOCK_TIMER_EVENT_ALARM,
        .payload = &payload.super,
    };
    CHECK_EQ(ESP_OK, nn20clock_ui_handle_timer_event(&screen->super, &event));

    /* The Timer's payload is transient; the copy is what must arrive. */
    payload.alarm_id = 0u;
    payload.super.displayed_time = 0;

    flush(worker);
    CHECK_EQ(1, log.timer_events);
    CHECK_EQ(NN20CLOCK_TIMER_EVENT_ALARM, log.last_timer_type);
    CHECK_EQ(1787927400, log.last_displayed_time);
    CHECK_EQ(7, log.last_alarm_id);

    nn20clock_ui_destroy(&screen->super);
    nn20_worker_delete(worker);
}

TEST(dispatch_rejects_null_arguments)
{
    nn20_worker_ctx *worker = nn20_worker_create();
    REQUIRE(worker != NULL);

    ScreenLog log = {0};
    FakeScreen *screen = fake_screen_new(worker, &log, &FAKE_VTABLE, NULL,
                                         NULL);
    REQUIRE(screen != NULL);

    NN20ClockTouchEvent touch = {0};
    CHECK_EQ(ESP_ERR_INVALID_ARG, nn20clock_ui_show(NULL));
    CHECK_EQ(ESP_ERR_INVALID_ARG, nn20clock_ui_hide(NULL));
    CHECK_EQ(ESP_ERR_INVALID_ARG, nn20clock_ui_handle_touch(NULL, &touch));
    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_ui_handle_touch(&screen->super, NULL));
    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_ui_handle_timer_event(&screen->super, NULL));

    nn20clock_ui_destroy(&screen->super);
    nn20_worker_delete(worker);
}

TEST(a_vtable_of_nulls_is_a_no_op_not_a_crash)
{
    nn20_worker_ctx *worker = nn20_worker_create();
    REQUIRE(worker != NULL);

    ScreenLog log = {0};
    FakeScreen *screen = fake_screen_new(worker, &log, &EMPTY_VTABLE, NULL,
                                         NULL);
    REQUIRE(screen != NULL);

    NN20ClockTouchEvent touch = { .type = NN20CLOCK_TOUCH_RELEASE };
    NN20ClockTimerPayload payload = {0};
    const NN20ClockTimerEvent event = {
        .type = NN20CLOCK_TIMER_EVENT_SECOND,
        .payload = &payload,
    };

    CHECK_EQ(ESP_OK, nn20clock_ui_show(&screen->super));
    CHECK_EQ(ESP_OK, nn20clock_ui_handle_touch(&screen->super, &touch));
    CHECK_EQ(ESP_OK, nn20clock_ui_handle_timer_event(&screen->super, &event));
    CHECK_EQ(ESP_OK, nn20clock_ui_hide(&screen->super));
    flush(worker);

    /* No hooks ran, so nothing was counted - but the screen is still
     * marked visible: with no show() to fail, there is nothing to
     * report, and the manager needs the state to be true. */
    CHECK_EQ(0, log.shows);
    CHECK_EQ(0, log.timer_events);

    /* No destroy hook either, so the base has to be cleaned up here. */
    nn20clock_ui_destroy(&screen->super);
    nn20clock_ui_base_deinit(&screen->super);
    free(screen);
    nn20_worker_delete(worker);
}

/* -------------------------------------------------------- commands --- */

typedef struct {
    int calls;
    NN20ClockUiCommandType last_type;
    int last_value;
    NN20ClockManager *last_manager;
} CommandLog;

static CommandLog g_command_log;

static esp_err_t record_command(NN20ClockManager *manager,
                                const NN20ClockUiCommand *command)
{
    g_command_log.calls++;
    g_command_log.last_type = command->type;
    g_command_log.last_value = command->value;
    g_command_log.last_manager = manager;
    return ESP_OK;
}

TEST(commands_reach_the_manager_callback)
{
    nn20_worker_ctx *worker = nn20_worker_create();
    REQUIRE(worker != NULL);

    memset(&g_command_log, 0, sizeof(g_command_log));

    /* No real manager needed: UiBase only carries the pointer through,
     * which is exactly the decoupling design 11 asks for. */
    NN20ClockManager *const fake_manager = (NN20ClockManager *)(uintptr_t)0x1;

    ScreenLog log = {0};
    FakeScreen *screen = fake_screen_new(worker, &log, &FAKE_VTABLE,
                                         fake_manager, record_command);
    REQUIRE(screen != NULL);

    const NN20ClockUiCommand command = {
        .type = NN20CLOCK_UI_COMMAND_SNOOZE_ALARM,
        .value = 9,
    };
    CHECK_EQ(ESP_OK, nn20clock_ui_send_command(&screen->super, &command));

    CHECK_EQ(1, g_command_log.calls);
    CHECK_EQ(NN20CLOCK_UI_COMMAND_SNOOZE_ALARM, g_command_log.last_type);
    CHECK_EQ(9, g_command_log.last_value);
    CHECK(g_command_log.last_manager == fake_manager);
    CHECK(nn20clock_ui_manager(&screen->super) == fake_manager);

    nn20clock_ui_destroy(&screen->super);
    nn20_worker_delete(worker);
}

TEST(a_screen_with_no_manager_reports_rather_than_crashes)
{
    nn20_worker_ctx *worker = nn20_worker_create();
    REQUIRE(worker != NULL);

    ScreenLog log = {0};
    FakeScreen *screen = fake_screen_new(worker, &log, &FAKE_VTABLE, NULL,
                                         NULL);
    REQUIRE(screen != NULL);

    const NN20ClockUiCommand command = { .type = NN20CLOCK_UI_COMMAND_MUTE };
    CHECK_EQ(ESP_ERR_INVALID_STATE,
             nn20clock_ui_send_command(&screen->super, &command));
    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_ui_send_command(&screen->super, NULL));
    CHECK_EQ(ESP_ERR_INVALID_ARG, nn20clock_ui_send_command(NULL, &command));

    nn20clock_ui_destroy(&screen->super);
    nn20_worker_delete(worker);
}

TEST(every_command_has_a_name)
{
    /* Names end up in logs when a command misbehaves; a missing one
     * would read as "UNKNOWN" exactly when it matters. */
    CHECK_STR_EQ("MUTE",
                 nn20clock_ui_command_name(NN20CLOCK_UI_COMMAND_MUTE));
    CHECK_STR_EQ("STOP_ALARM",
                 nn20clock_ui_command_name(NN20CLOCK_UI_COMMAND_STOP_ALARM));
    CHECK_STR_EQ("BRIGHTNESS_DOWN",
                 nn20clock_ui_command_name(
                     NN20CLOCK_UI_COMMAND_BRIGHTNESS_DOWN));
    CHECK_STR_EQ("OPEN_SETTINGS",
                 nn20clock_ui_command_name(
                     NN20CLOCK_UI_COMMAND_OPEN_SETTINGS));
    CHECK_STR_EQ("CANCEL_SNOOZE",
                 nn20clock_ui_command_name(
                     NN20CLOCK_UI_COMMAND_CANCEL_SNOOZE));
    CHECK_STR_EQ("OPEN_MEDIA_PLAYBACK",
                 nn20clock_ui_command_name(
                     NN20CLOCK_UI_COMMAND_OPEN_MEDIA_PLAYBACK));
    CHECK_STR_EQ("CLOSE_MEDIA_PLAYBACK",
                 nn20clock_ui_command_name(
                     NN20CLOCK_UI_COMMAND_CLOSE_MEDIA_PLAYBACK));
    CHECK_STR_EQ("OPEN_MEDIA_MANAGEMENT",
                 nn20clock_ui_command_name(
                     NN20CLOCK_UI_COMMAND_OPEN_MEDIA_MANAGEMENT));
    CHECK_STR_EQ("CLOSE_MEDIA_MANAGEMENT",
                 nn20clock_ui_command_name(
                     NN20CLOCK_UI_COMMAND_CLOSE_MEDIA_MANAGEMENT));
    CHECK_STR_EQ("UNKNOWN", nn20clock_ui_command_name(
                                (NN20ClockUiCommandType)999));

    /*
     * Every one of them, so a command added without a name is a failing
     * test rather than an "UNKNOWN" in a log six months later.
     */
    for (int i = 0; i <= NN20CLOCK_UI_COMMAND_CLOSE_MEDIA_MANAGEMENT; i++) {
        CHECK(strcmp("UNKNOWN", nn20clock_ui_command_name(
                                    (NN20ClockUiCommandType)i)) != 0);
    }
}

/* ---------------------------------------------------------- destroy -- */

/* Design 8: the outgoing screen must be gone before the next is built,
 * so destroy is synchronous and hides on the way out. */
TEST(destroy_hides_first_and_waits)
{
    nn20_worker_ctx *worker = nn20_worker_create();
    REQUIRE(worker != NULL);

    ScreenLog log = {0};
    FakeScreen *screen = fake_screen_new(worker, &log, &FAKE_VTABLE, NULL,
                                         NULL);
    REQUIRE(screen != NULL);

    REQUIRE(nn20clock_ui_show(&screen->super) == ESP_OK);
    flush(worker);

    nn20clock_ui_destroy(&screen->super);
    /* No barrier here on purpose: if destroy were asynchronous these
     * counts would still be zero. */
    CHECK_EQ(1, log.hides);
    CHECK_EQ(1, log.destroys);

    nn20_worker_delete(worker);
}

/* A screen that was never shown must not get a hide it did not ask for. */
TEST(destroying_a_hidden_screen_does_not_hide_it_again)
{
    nn20_worker_ctx *worker = nn20_worker_create();
    REQUIRE(worker != NULL);

    ScreenLog log = {0};
    FakeScreen *screen = fake_screen_new(worker, &log, &FAKE_VTABLE, NULL,
                                         NULL);
    REQUIRE(screen != NULL);

    nn20clock_ui_destroy(&screen->super);
    CHECK_EQ(0, log.hides);
    CHECK_EQ(1, log.destroys);

    nn20_worker_delete(worker);
}

/* The UiWorker is gone, so there is nobody to run the hook - it must
 * still run, or the screen leaks on every shutdown path. */
TEST(destroy_runs_inline_when_the_worker_is_gone)
{
    nn20_worker_ctx *worker = nn20_worker_create();
    REQUIRE(worker != NULL);

    ScreenLog log = {0};
    FakeScreen *screen = fake_screen_new(worker, &log, &FAKE_VTABLE, NULL,
                                         NULL);
    REQUIRE(screen != NULL);

    REQUIRE(nn20_worker_stop(worker) == 0);

    nn20clock_ui_destroy(&screen->super);
    CHECK_EQ(1, log.destroys);

    nn20_worker_delete(worker);
}

TEST_MAIN("nn20clock_ui")
{
    RUN(base_init_rejects_missing_pieces);
    RUN(base_init_stores_what_it_was_given);
    RUN(accessors_tolerate_null);
    RUN(show_and_hide_run_on_the_ui_worker);
    RUN(a_failed_show_leaves_the_screen_hidden);
    RUN(touch_events_reach_the_screen_by_value);
    RUN(timer_events_arrive_with_a_copied_payload);
    RUN(a_derived_payload_survives_the_dispatch);
    RUN(dispatch_rejects_null_arguments);
    RUN(a_vtable_of_nulls_is_a_no_op_not_a_crash);
    RUN(commands_reach_the_manager_callback);
    RUN(a_screen_with_no_manager_reports_rather_than_crashes);
    RUN(every_command_has_a_name);
    RUN(destroy_hides_first_and_waits);
    RUN(destroying_a_hidden_screen_does_not_hide_it_again);
    RUN(destroy_runs_inline_when_the_worker_is_gone);
}
