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
 * Component tests for the application's worker set (design 4).
 *
 * What is worth testing here is not the threading - worker.c owns that -
 * but the wiring around it: that the workers are distinct, that the
 * accessors hand back the right one, that overrides reach the thread,
 * and that a failure to build the set leaves nothing behind.
 */
#include "test_util.h"

#include "nn20clock_workers.h"

/* --------------------------------------------------------- lifecycle -- */

TEST(ctor_starts_every_worker)
{
    NN20ClockWorkers *workers = nn20clock_workers_ctor();
    REQUIRE(workers != NULL);

    CHECK(nn20clock_workers_are_running(workers));
    for (int i = 0; i < NN20CLOCK_WORKER_COUNT; i++) {
        nn20_worker_ctx *worker = nn20clock_workers_get(workers, i);
        CHECK(worker != NULL);
        CHECK(nn20_worker_is_running(worker));
    }

    nn20clock_workers_dtor(workers);
}

/* Distinct threads, not one handle handed out several times - the
 * whole design 4 separation depends on it. */
TEST(each_worker_is_a_different_thread)
{
    NN20ClockWorkers *workers = nn20clock_workers_ctor();
    REQUIRE(workers != NULL);

    for (int i = 0; i < NN20CLOCK_WORKER_COUNT; i++) {
        for (int j = i + 1; j < NN20CLOCK_WORKER_COUNT; j++) {
            CHECK(nn20clock_workers_get(workers, i) !=
                  nn20clock_workers_get(workers, j));
        }
    }

    nn20clock_workers_dtor(workers);
}

TEST(named_accessors_match_the_ids)
{
    NN20ClockWorkers *workers = nn20clock_workers_ctor();
    REQUIRE(workers != NULL);

    CHECK(nn20clock_workers_ui(workers) ==
          nn20clock_workers_get(workers, NN20CLOCK_WORKER_UI));
    CHECK(nn20clock_workers_storage(workers) ==
          nn20clock_workers_get(workers, NN20CLOCK_WORKER_STORAGE));
    CHECK(nn20clock_workers_clock_manager(workers) ==
          nn20clock_workers_get(workers, NN20CLOCK_WORKER_CLOCK_MANAGER));
    CHECK(nn20clock_workers_core(workers) ==
          nn20clock_workers_get(workers, NN20CLOCK_WORKER_CORE));

    nn20clock_workers_dtor(workers);
}

TEST(accessors_tolerate_null_and_bad_ids)
{
    CHECK(nn20clock_workers_get(NULL, NN20CLOCK_WORKER_UI) == NULL);
    CHECK(nn20clock_workers_ui(NULL) == NULL);
    CHECK(!nn20clock_workers_are_running(NULL));

    NN20ClockWorkers *workers = nn20clock_workers_ctor();
    REQUIRE(workers != NULL);
    CHECK(nn20clock_workers_get(workers, NN20CLOCK_WORKER_COUNT) == NULL);
    CHECK(nn20clock_workers_get(workers, (NN20ClockWorkerId)-1) == NULL);
    nn20clock_workers_dtor(workers);
}

TEST(dtor_and_log_stats_tolerate_null)
{
    nn20clock_workers_dtor(NULL);
    nn20clock_workers_log_stats(NULL);
    CHECK_EQ(ESP_ERR_INVALID_ARG, nn20clock_workers_stop(NULL));
}

/* ------------------------------------------------------------ config -- */

TEST(default_config_is_usable_as_is)
{
    NN20ClockWorkersConfig config;
    REQUIRE(nn20clock_workers_default_config(&config) == ESP_OK);
    CHECK_EQ(sizeof(config), config.struct_size);

    for (int i = 0; i < NN20CLOCK_WORKER_COUNT; i++) {
        /* Every field resolved: nothing left on the "0 means default"
         * sentinel, so this struct describes the real threading. */
        CHECK(config.workers[i].queue_capacity != 0u);
        CHECK(config.workers[i].stack_bytes != 0u);
        CHECK(config.workers[i].priority != 0);
        CHECK(config.workers[i].core_id != NN20CLOCK_WORKER_CORE_DEFAULT);
    }

    NN20ClockWorkers *workers = nn20clock_workers_ctor_with(&config);
    REQUIRE(workers != NULL);
    CHECK(nn20clock_workers_are_running(workers));
    nn20clock_workers_dtor(workers);
}

TEST(default_config_rejects_null)
{
    CHECK_EQ(ESP_ERR_INVALID_ARG, nn20clock_workers_default_config(NULL));
}

/* A stale or mis-sized config is a caller bug, and one that would
 * otherwise be read as garbage stack sizes. */
TEST(ctor_rejects_a_wrong_struct_size)
{
    NN20ClockWorkersConfig config;
    REQUIRE(nn20clock_workers_default_config(&config) == ESP_OK);
    config.struct_size = 4u;

    CHECK(nn20clock_workers_ctor_with(&config) == NULL);
}

TEST(null_config_means_defaults)
{
    NN20ClockWorkers *workers = nn20clock_workers_ctor_with(NULL);
    REQUIRE(workers != NULL);
    CHECK(nn20clock_workers_are_running(workers));
    nn20clock_workers_dtor(workers);
}

/*
 * An override has to reach the thread, or the config struct is
 * decoration. Queue capacity is the one that can be observed from
 * outside: fill the queue and see where it stops accepting work.
 *
 * The filling is done from inside a callback running on that same
 * worker. While a callback runs, the worker is not draining, so the
 * queue fills behind it - and posting from inside a callback is
 * explicitly allowed and does not block.
 *
 * The obvious alternative, parking the worker in a spin while the test
 * thread posts, deadlocks on the target: the worker task sits at
 * priority 4 and the task running the tests at 1, so a spinning worker
 * never yields the core and the flag releasing it is never set. It
 * passes happily on a desktop, where the two are just pthreads on
 * different cores. Ordering work through the worker itself has no such
 * assumption, on either platform.
 */
typedef struct {
    nn20_worker_ctx *worker;
    int accepted;
} FillProbe;

static int do_nothing(nn20_worker_ctx *worker, void *user_data)
{
    (void)worker;
    (void)user_data;
    return 0;
}

static int fill_the_queue(nn20_worker_ctx *worker, void *user_data)
{
    FillProbe *probe = user_data;

    /* `worker` is the handle the callback is running on - the same one
     * being filled. */
    while (probe->accepted < 64) {
        if (nn20_worker_post(worker, do_nothing, NULL) != 0) {
            break;   /* full: backpressure, not an error */
        }
        probe->accepted++;
    }
    return 0;
}

TEST(queue_capacity_override_reaches_the_worker)
{
    NN20ClockWorkersConfig config;
    REQUIRE(nn20clock_workers_default_config(&config) == ESP_OK);
    config.workers[NN20CLOCK_WORKER_CORE].queue_capacity = 4u;

    NN20ClockWorkers *workers = nn20clock_workers_ctor_with(&config);
    REQUIRE(workers != NULL);

    nn20_worker_ctx *core = nn20clock_workers_core(workers);
    FillProbe probe = { .worker = core, .accepted = 0 };

    /* Synchronous, so the filling has finished by the time this
     * returns. */
    REQUIRE(nn20_worker_post_sync(core, fill_the_queue, &probe) == 0);

    /* A 4-slot queue cannot take 64 posts. The exact number depends on
     * how the ring accounts for the slot the running callback came from,
     * so assert the bound rather than a magic number. */
    CHECK(probe.accepted > 0);
    CHECK(probe.accepted <= 4);

    nn20clock_workers_dtor(workers);
}

/* ----------------------------------------------------------- naming -- */

TEST(names_are_the_design_4_names)
{
    CHECK_STR_EQ("CoreWorker", nn20clock_workers_name(NN20CLOCK_WORKER_CORE));
    CHECK_STR_EQ("StorageWorker",
                 nn20clock_workers_name(NN20CLOCK_WORKER_STORAGE));
    CHECK_STR_EQ("ClockManagerWorker",
                 nn20clock_workers_name(NN20CLOCK_WORKER_CLOCK_MANAGER));
    CHECK_STR_EQ("UiWorker", nn20clock_workers_name(NN20CLOCK_WORKER_UI));
    CHECK_STR_EQ("UnknownWorker",
                 nn20clock_workers_name(NN20CLOCK_WORKER_COUNT));
}

/* --------------------------------------------------------- shutdown -- */

TEST(stop_drains_and_leaves_the_set_readable)
{
    NN20ClockWorkers *workers = nn20clock_workers_ctor();
    REQUIRE(workers != NULL);

    CHECK_EQ(ESP_OK, nn20clock_workers_stop(workers));
    CHECK(!nn20clock_workers_are_running(workers));

    /* Statistics still readable after the threads are gone; that is the
     * reason _stop() exists separately from the dtor. */
    nn20clock_workers_log_stats(workers);

    CHECK_EQ(ESP_OK, nn20clock_workers_stop(workers));   /* idempotent */
    nn20clock_workers_dtor(workers);
}

TEST_MAIN("nn20clock_workers")
{
    RUN(ctor_starts_every_worker);
    RUN(each_worker_is_a_different_thread);
    RUN(named_accessors_match_the_ids);
    RUN(accessors_tolerate_null_and_bad_ids);
    RUN(dtor_and_log_stats_tolerate_null);
    RUN(default_config_is_usable_as_is);
    RUN(default_config_rejects_null);
    RUN(ctor_rejects_a_wrong_struct_size);
    RUN(null_config_means_defaults);
    RUN(queue_capacity_override_reaches_the_worker);
    RUN(names_are_the_design_4_names);
    RUN(stop_drains_and_leaves_the_set_readable);
}
