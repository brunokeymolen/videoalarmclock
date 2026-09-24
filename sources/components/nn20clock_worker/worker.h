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
 * worker.h - a single-consumer task worker on its own thread.
 *
 * Producers hand it callbacks from any thread; it runs them in order on
 * one thread of its own, which may be pinned to a specific core. On the
 * ESP32-P4 that is a FreeRTOS task created with
 * xTaskCreatePinnedToCore(); on a desktop it is a pthread with CPU
 * affinity.
 *
 * Pinning is the point on the P4: the receive path and the embedding
 * pipeline should sit on different cores rather than preempting each
 * other.
 *
 * Submission is lock-free and allocation-free. The queue is a bounded
 * MPSC ring; the callback and its user pointer are stored directly in a
 * slot, so nn20_worker_post() never calls malloc and never blocks.
 */
#ifndef NN20_WORKER_H
#define NN20_WORKER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nn20_worker_ctx nn20_worker_ctx;

/*
 * Runs on the worker thread. Returning non-zero is reported in the
 * statistics but does not stop the worker.
 */
typedef int (*nn20_worker_callback)(nn20_worker_ctx *worker, void *user_data);

/* Pass as core_id to leave the thread unpinned. */
#define NN20_WORKER_CORE_ANY (-1)

/* Small by design: this is a task queue, not a data buffer. 256 slots is
 * 6 KB, which internal RAM on the P4 can afford; the original 8192 would
 * have been 196 KB. Must be a power of two. */
#define NN20_WORKER_DEFAULT_QUEUE 256u
#define NN20_WORKER_DEFAULT_STACK 4096u
#define NN20_WORKER_DEFAULT_PRIORITY 5

typedef struct {
    uint32_t struct_size;

    const char *name;         /* task/thread name; NULL for a default */
    uint32_t queue_capacity;  /* power of two; 0 = default */
    uint32_t stack_bytes;     /* 0 = default */
    int priority;             /* FreeRTOS priority; ignored on POSIX */
    int core_id;              /* NN20_WORKER_CORE_ANY, or 0..cores-1 */
} nn20_worker_config;

typedef struct {
    uint64_t posted;      /* accepted by nn20_worker_post() */
    uint64_t rejected;    /* refused because the queue was full */
    uint64_t executed;    /* callbacks actually run */
    uint64_t failed;      /* callbacks that returned non-zero */
    uint64_t wakeups;     /* times the thread woke with work waiting */
    uint32_t peak_depth;  /* deepest the queue has been */
} nn20_worker_stats;

/* Defaults: unpinned, 256 slots, 4 KB stack. */
nn20_worker_ctx *nn20_worker_create(void);

/* Returns NULL if the thread cannot start or the config is invalid
 * (capacity not a power of two, core_id out of range). */
nn20_worker_ctx *nn20_worker_create_with(const nn20_worker_config *config);

/*
 * Stop the thread and release everything. Waits for the task in progress
 * to finish; queued-but-unstarted tasks are discarded. Safe with NULL,
 * and safe after nn20_worker_stop().
 */
void nn20_worker_delete(nn20_worker_ctx *worker);

/*
 * Queue a callback. Returns 0 on success, -1 on bad arguments, and 1 if
 * the queue is full - a full queue is backpressure, not an error, so the
 * caller decides whether to drop or retry.
 *
 * Safe from any thread, including from inside a callback.
 */
int nn20_worker_post(nn20_worker_ctx *worker, nn20_worker_callback callback,
                     void *user_data);

/*
 * Queue a callback and block until it has finished running on the worker.
 *
 * Returns 0 when the callback ran to completion, -1 on bad arguments or
 * once the worker is stopping, and 1 if the queue was full - in which case
 * nothing ran and the caller was not blocked.
 *
 * The callback's own return value is not reported here; a non-zero one is
 * counted in nn20_worker_stats::failed. Say so if you need it back and the
 * signature can grow an out-parameter.
 *
 * Called from inside a worker callback, it runs the callback inline and
 * returns, rather than waiting for a thread that is already busy waiting -
 * that would deadlock. Ordering still holds for everything else.
 *
 * If the worker is stopped while a caller is waiting, the wait is released
 * and the callback does not run.
 */
int nn20_worker_post_sync(nn20_worker_ctx *worker,
                          nn20_worker_callback callback, void *user_data);

/*
 * Ask the worker to finish the queue and stop, then wait for it. Tasks
 * already queued ahead of the request still run. Idempotent.
 */
int nn20_worker_stop(nn20_worker_ctx *worker);

/*
 * Stop without draining: the current callback completes, anything still
 * queued is dropped. Idempotent.
 */
void nn20_worker_stop_now(nn20_worker_ctx *worker);

bool nn20_worker_is_running(const nn20_worker_ctx *worker);

/* Tasks waiting to run. */
size_t nn20_worker_pending(const nn20_worker_ctx *worker);

/*
 * Core the worker thread actually ended up on, sampled once the thread
 * starts. NN20_WORKER_CORE_ANY when unpinned or unknown.
 */
int nn20_worker_core_id(const nn20_worker_ctx *worker);

void nn20_worker_get_stats(const nn20_worker_ctx *worker,
                           nn20_worker_stats *stats);

/* Cores available on this platform: 2 on the ESP32-P4. */
int nn20_worker_core_count(void);

#ifdef __cplusplus
}
#endif

#endif /* NN20_WORKER_H */
