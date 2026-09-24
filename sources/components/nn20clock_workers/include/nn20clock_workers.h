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
 * nn20clock_workers.h - the application's thread set (design 4).
 *
 * Design 4 names four workers under "Suggested initial threading". They
 * are created here, once, at startup, and passed into the components
 * that run on them. Nothing below this component creates a thread of its
 * own except the Timer, which design 6 and 7 give its own worker so a
 * one-second tick is not queued behind orchestration or SD card I/O.
 *
 * Creating them in one place is the point: the whole threading model -
 * names, stacks, priorities, and core pinning - is visible in one
 * struct instead of scattered across five constructors.
 *
 *   UiWorker            every UI class, through UiBase. All LVGL calls
 *                       happen here and nowhere else (design 11).
 *   StorageWorker       flash, SD, and filesystem I/O, which block.
 *   ClockManagerWorker  ClockManager state transitions and command
 *                       routing (design 8).
 *   CoreWorker          application-level orchestration: starting and
 *                       supervising the long-lived services.
 *   PlayerWorker        alarm playback (design 17, Milestone 7): the SD
 *                       reads, the JPEG decode, and the blocking I2S
 *                       writes, for as long as an alarm rings.
 *
 * Design 4 names the first four. The player is the fifth and is here
 * for the same reason as the rest: one long-running job that must not
 * queue behind anything, on a thread the whole model can be read from
 * one place. Worth confirming in the design doc.
 *
 * The set is borrowed, never owned, by its consumers: it must outlive
 * every component constructed with one of its workers. NN20ClockApp
 * enforces that by owning the set and tearing it down last.
 */
#ifndef NN20CLOCK_WORKERS_H
#define NN20CLOCK_WORKERS_H

#include <stdbool.h>
#include <stdint.h>

#include "nn20clock_platform.h"
#include "worker.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NN20ClockWorkers NN20ClockWorkers;

/*
 * Which worker a handle refers to. Also the teardown order when read
 * backwards - see nn20clock_workers_dtor().
 */
typedef enum {
    NN20CLOCK_WORKER_CORE,
    NN20CLOCK_WORKER_STORAGE,
    NN20CLOCK_WORKER_CLOCK_MANAGER,
    NN20CLOCK_WORKER_UI,
    NN20CLOCK_WORKER_PLAYER,
    NN20CLOCK_WORKER_READER,
    NN20CLOCK_WORKER_COUNT
} NN20ClockWorkerId;

/*
 * Per-worker knobs. Zero means "use the default for this worker", so a
 * caller can override one field and leave the rest alone. core_id is the
 * exception: 0 is a real core, so NN20CLOCK_WORKER_CORE_DEFAULT marks
 * "unset" instead.
 */
#define NN20CLOCK_WORKER_CORE_DEFAULT (-2)

typedef struct {
    uint32_t queue_capacity;  /* power of two; 0 = default */
    uint32_t stack_bytes;     /* 0 = default */
    int priority;             /* 0 = default */
    int core_id;              /* NN20CLOCK_WORKER_CORE_DEFAULT = default */
} NN20ClockWorkerSpec;

typedef struct {
    uint32_t struct_size;
    NN20ClockWorkerSpec workers[NN20CLOCK_WORKER_COUNT];
} NN20ClockWorkersConfig;

/*
 * Fills out_config with the defaults documented in nn20clock_workers.c.
 * Start from this rather than from a zeroed struct so struct_size and
 * the core_id sentinels are right.
 */
esp_err_t nn20clock_workers_default_config(NN20ClockWorkersConfig *out_config);

/*
 * Creates all four threads. Returns NULL if any of them will not start,
 * having deleted the ones that did - a half-built thread set is worse
 * than none.
 */
NN20ClockWorkers *nn20clock_workers_ctor(void);

/* config may be NULL, which is the same as passing the defaults. */
NN20ClockWorkers *nn20clock_workers_ctor_with(
    const NN20ClockWorkersConfig *config);

/*
 * Stops and deletes every worker. Safe with NULL.
 *
 * Teardown runs CoreWorker first and UiWorker last, the reverse of the
 * dependency direction: orchestration stops posting before the services
 * it drives go away, and the UI thread stays alive long enough to run
 * the teardown work the others post to it.
 *
 * Every component holding one of these workers must already be
 * destroyed. The workers do not know their consumers, so this cannot be
 * checked here.
 */
void nn20clock_workers_dtor(NN20ClockWorkers *pthis);

/*
 * Ask every worker to drain its queue and stop, in the teardown order
 * above, without freeing them. Useful for an orderly shutdown when the
 * caller still wants to read statistics afterwards. Idempotent.
 */
esp_err_t nn20clock_workers_stop(NN20ClockWorkers *pthis);

/* NULL for a NULL set or an out-of-range id. */
nn20_worker_ctx *nn20clock_workers_get(const NN20ClockWorkers *pthis,
                                       NN20ClockWorkerId id);

/* Named accessors; these are what call sites should read like. */
nn20_worker_ctx *nn20clock_workers_ui(const NN20ClockWorkers *pthis);
nn20_worker_ctx *nn20clock_workers_storage(const NN20ClockWorkers *pthis);
nn20_worker_ctx *nn20clock_workers_clock_manager(const NN20ClockWorkers *pthis);
nn20_worker_ctx *nn20clock_workers_core(const NN20ClockWorkers *pthis);
nn20_worker_ctx *nn20clock_workers_player(const NN20ClockWorkers *pthis);

/*
 * The card reader that fills the player's window (design 17, Milestone
 * 11). Its own thread because it spends nearly all of its time blocked
 * in fread(), and the whole point is that the player is doing something
 * else while it does.
 */
nn20_worker_ctx *nn20clock_workers_reader(const NN20ClockWorkers *pthis);

/* "UiWorker", "StorageWorker", ... - for logs and test failures. */
const char *nn20clock_workers_name(NN20ClockWorkerId id);

/* True while every thread in the set is running. */
bool nn20clock_workers_are_running(const NN20ClockWorkers *pthis);

/*
 * One INFO line per worker: core placement, queue depth, and the
 * posted/rejected/executed/failed counters. This is the cheapest way to
 * see the threading model behaving on the board, so app_main calls it
 * from the heartbeat.
 */
void nn20clock_workers_log_stats(const NN20ClockWorkers *pthis);

#ifdef __cplusplus
}
#endif

#endif /* NN20CLOCK_WORKERS_H */
