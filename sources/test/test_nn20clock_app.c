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
 * Component tests for the composition root (design 4's CoreWorker).
 *
 * This suite is about wiring, not behavior: that everything gets built,
 * that it starts in dependency order, that it stops in the reverse, and
 * that a construct/destroy cycle leaves nothing running. The individual
 * components are tested in their own suites.
 */
#include "test_util.h"

#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#endif

#include "nn20clock_app.h"

/* Barrier: see the same helper in test_nn20clock_storage.c. */
static int barrier_cb(nn20_worker_ctx *worker, void *user_data)
{
    (void)worker;
    (void)user_data;
    return 0;
}

/* --------------------------------------------------------- lifecycle -- */

TEST(ctor_builds_every_service)
{
    NN20ClockApp *app = nn20clock_app_ctor();
    REQUIRE(app != NULL);

    CHECK(nn20clock_app_workers(app) != NULL);
    CHECK(nn20clock_app_storage(app) != NULL);
    CHECK(nn20clock_app_timer(app) != NULL);
    CHECK(nn20clock_app_manager(app) != NULL);

    /* Built, not started: design 16 wants a skeleton that comes up in
     * two deliberate steps, so a failure is attributable to one of
     * them. */
    CHECK(!nn20clock_app_is_running(app));
    CHECK(!nn20clock_timer_is_running(nn20clock_app_timer(app)));
    CHECK(!nn20clock_manager_is_running(nn20clock_app_manager(app)));
    CHECK_EQ(NN20CLOCK_STATE_BOOT,
             nn20clock_manager_state(nn20clock_app_manager(app)));

    nn20clock_app_dtor(app);
}

TEST(the_workers_are_up_and_wired_to_their_owners)
{
    NN20ClockApp *app = nn20clock_app_ctor();
    REQUIRE(app != NULL);

    NN20ClockWorkers *workers = nn20clock_app_workers(app);
    REQUIRE(workers != NULL);
    CHECK(nn20clock_workers_are_running(workers));

    /* Design 4's four, plus the player added at Milestone 7 and the
     * card reader at Milestone 11, each a thread of its own. The Timer
     * is deliberately not among them: it brings its own worker so a
     * one-second tick does not queue behind storage or the display. */
    CHECK_EQ(6, NN20CLOCK_WORKER_COUNT);
    CHECK(nn20clock_workers_ui(workers) != NULL);
    CHECK(nn20clock_workers_storage(workers) != NULL);
    CHECK(nn20clock_workers_clock_manager(workers) != NULL);
    CHECK(nn20clock_workers_core(workers) != NULL);
    CHECK(nn20clock_workers_player(workers) != NULL);
    CHECK(nn20clock_workers_reader(workers) != NULL);

    nn20clock_app_dtor(app);
}

TEST(dtor_and_accessors_tolerate_null)
{
    nn20clock_app_dtor(NULL);
    CHECK(!nn20clock_app_is_running(NULL));
    CHECK(nn20clock_app_workers(NULL) == NULL);
    CHECK(nn20clock_app_storage(NULL) == NULL);
    CHECK(nn20clock_app_timer(NULL) == NULL);
    CHECK(nn20clock_app_manager(NULL) == NULL);
    CHECK_EQ(ESP_ERR_INVALID_ARG, nn20clock_app_start(NULL));
    CHECK_EQ(ESP_ERR_INVALID_ARG, nn20clock_app_stop(NULL));
    CHECK_EQ(ESP_ERR_INVALID_ARG, nn20clock_app_supervise(NULL));
    nn20clock_app_log_status(NULL);
}

TEST(start_brings_up_storage_timer_and_manager)
{
    NN20ClockApp *app = nn20clock_app_ctor();
    REQUIRE(app != NULL);

    CHECK_EQ(ESP_OK, nn20clock_app_start(app));
    CHECK(nn20clock_app_is_running(app));

    CHECK(nn20clock_storage_is_ready(nn20clock_app_storage(app)));
    CHECK(nn20clock_timer_is_running(nn20clock_app_timer(app)));
    CHECK(nn20clock_manager_is_running(nn20clock_app_manager(app)));

    /* Design 5: boot complete lands in TIME. */
    CHECK_EQ(NN20CLOCK_STATE_TIME,
             nn20clock_manager_state(nn20clock_app_manager(app)));

    /*
     * Who subscribed to the Timer on the way up.
     *
     * The manager always does. The firmware build adds a second, the
     * minute tick that runs the brightness schedule - it lives behind
     * ESP_PLATFORM because it moves a real backlight, so a headless
     * build has one subscriber and a board has two.
     *
     * Spelled out per build rather than relaxed to "at least one".
     * This assertion is here to catch a subscription that was added and
     * never unsubscribed, and a bound that accepts any number catches
     * nothing - which is what the previous hardcoded 1 did in reverse,
     * passing on the host and failing on target from the day the
     * brightness schedule landed. stop_takes_it_all_back_down() checks
     * the other end of both.
     */
#if defined(ESP_PLATFORM)
    CHECK_EQ(2, nn20clock_timer_subscriber_count(nn20clock_app_timer(app)));
#else
    CHECK_EQ(1, nn20clock_timer_subscriber_count(nn20clock_app_timer(app)));
#endif

    CHECK_EQ(ESP_ERR_INVALID_STATE, nn20clock_app_start(app));

    nn20clock_app_dtor(app);
}

TEST(stop_takes_it_all_back_down)
{
    NN20ClockApp *app = nn20clock_app_ctor();
    REQUIRE(app != NULL);
    REQUIRE(nn20clock_app_start(app) == ESP_OK);

    CHECK_EQ(ESP_OK, nn20clock_app_stop(app));
    CHECK(!nn20clock_app_is_running(app));
    CHECK(!nn20clock_timer_is_running(nn20clock_app_timer(app)));
    CHECK(!nn20clock_manager_is_running(nn20clock_app_manager(app)));
    CHECK_EQ(0, nn20clock_timer_subscriber_count(nn20clock_app_timer(app)));

    CHECK_EQ(ESP_ERR_INVALID_STATE, nn20clock_app_stop(app));

    nn20clock_app_dtor(app);
}

/* The threads outlive a stop, so the app can be restarted rather than
 * rebuilt - which is what a recovery path will need at Milestone 12. */
TEST(it_can_be_started_again_after_a_stop)
{
    NN20ClockApp *app = nn20clock_app_ctor();
    REQUIRE(app != NULL);

    REQUIRE(nn20clock_app_start(app) == ESP_OK);
    REQUIRE(nn20clock_app_stop(app) == ESP_OK);

    CHECK_EQ(ESP_OK, nn20clock_app_start(app));
    CHECK_EQ(NN20CLOCK_STATE_TIME,
             nn20clock_manager_state(nn20clock_app_manager(app)));

    nn20clock_app_dtor(app);
}

/* The dtor has to stop what is running: on the target, app_main may go
 * away without stopping anything first. */
TEST(dtor_stops_a_running_app)
{
    NN20ClockApp *app = nn20clock_app_ctor();
    REQUIRE(app != NULL);
    REQUIRE(nn20clock_app_start(app) == ESP_OK);

    nn20clock_app_dtor(app);   /* must not hang or fault */
}

/* Build and tear down repeatedly: a worker or subscriber left behind
 * would show up here rather than three milestones later. */
/*
 * Building and tearing the whole application down, three times over.
 *
 * On the board this is also the leak check, and it is the first test in
 * the suite so that it runs on a heap nothing else has touched. It
 * earns that place: a cycle that does not give its memory back shows up
 * here as a number, and everywhere else as an unrelated component
 * failing to allocate several tests later.
 */
TEST(construct_and_destroy_cycles_cleanly)
{
#if defined(ESP_PLATFORM)
    size_t after_first = 0;
#endif

    for (int i = 0; i < 3; i++) {
        NN20ClockApp *app = nn20clock_app_ctor();
        REQUIRE(app != NULL);
        REQUIRE(nn20clock_app_start(app) == ESP_OK);
        REQUIRE(nn20clock_app_stop(app) == ESP_OK);
        nn20clock_app_dtor(app);

#if defined(ESP_PLATFORM)
        const size_t free_now = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        const size_t largest =
            heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        printf("        cycle %d: internal heap %u KB, largest block %u KB\n",
               i, (unsigned)(free_now / 1024U), (unsigned)(largest / 1024U));

        /*
         * The first cycle is allowed to keep things: LVGL initialises
         * once and stays initialised by design, and ESP-IDF's drivers
         * hold onto some of what they touch. What matters is that the
         * second and third cycles cost nothing more - a per-cycle leak
         * is what eventually stops the display starting.
         */
        if (i == 0) {
            after_first = free_now;
        } else {
            const size_t slack = 4096U;
            CHECK(free_now + slack >= after_first);
        }
#endif
    }
}

/* ------------------------------------------------------- supervision -- */

TEST(supervise_passes_on_a_healthy_app)
{
    NN20ClockApp *app = nn20clock_app_ctor();
    REQUIRE(app != NULL);

    /* Nothing to supervise before it starts. */
    CHECK_EQ(ESP_ERR_INVALID_STATE, nn20clock_app_supervise(app));

    REQUIRE(nn20clock_app_start(app) == ESP_OK);
    CHECK_EQ(ESP_OK, nn20clock_app_supervise(app));
    CHECK_EQ(ESP_OK, nn20clock_app_supervise(app));   /* repeatable */

    nn20clock_app_dtor(app);
}

/* Supervision that only ever returns OK is decoration. Put the manager
 * in ERROR and it has to say so. */
TEST(supervise_reports_a_manager_in_error)
{
    NN20ClockApp *app = nn20clock_app_ctor();
    REQUIRE(app != NULL);
    REQUIRE(nn20clock_app_start(app) == ESP_OK);

    NN20ClockManager *manager = nn20clock_app_manager(app);
    REQUIRE(nn20clock_manager_request_state(manager, NN20CLOCK_STATE_ERROR)
            == ESP_OK);

    /* supervise() runs on the CoreWorker, but the transition is applied
     * on the manager worker. Wait for it with a barrier rather than a
     * spin: post_sync returns only after everything queued ahead of it
     * has run, and it does not depend on this task ever being scheduled
     * against a busy worker. */
    (void)nn20_worker_post_sync(
        nn20clock_workers_clock_manager(nn20clock_app_workers(app)),
        barrier_cb, NULL);
    CHECK_EQ(NN20CLOCK_STATE_ERROR, nn20clock_manager_state(manager));

    CHECK_EQ(ESP_FAIL, nn20clock_app_supervise(app));

    nn20clock_app_dtor(app);
}

TEST(log_status_runs_without_a_board)
{
    NN20ClockApp *app = nn20clock_app_ctor();
    REQUIRE(app != NULL);
    REQUIRE(nn20clock_app_start(app) == ESP_OK);

    /* It is what app_main prints from its heartbeat; if it can fault on
     * a half-built app, the board loses its only status output. */
    nn20clock_app_log_status(app);

    nn20clock_app_dtor(app);
}

TEST_MAIN("nn20clock_app")
{
    /* First, so the leak check sees a clean heap - see its comment. */
    RUN(construct_and_destroy_cycles_cleanly);
    RUN(ctor_builds_every_service);
    RUN(the_workers_are_up_and_wired_to_their_owners);
    RUN(dtor_and_accessors_tolerate_null);
    RUN(start_brings_up_storage_timer_and_manager);
    RUN(stop_takes_it_all_back_down);
    RUN(it_can_be_started_again_after_a_stop);
    RUN(dtor_stops_a_running_app);
    RUN(supervise_passes_on_a_healthy_app);
    RUN(supervise_reports_a_manager_in_error);
    RUN(log_status_runs_without_a_board);
}
