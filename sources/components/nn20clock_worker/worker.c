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
 * worker.c - the task worker. See worker.h.
 *
 * Hot path notes, because this runs at 1 Hz per frame today and will run
 * far more often once the pipeline fills out:
 *
 *   - nn20_worker_post() allocates nothing. The MPSC slot already holds
 *     two pointers, so the callback goes in one and its user data in the
 *     other. The original version malloc'd a task item per submission and
 *     freed it on the consumer side.
 *   - The wakeup is a direct-to-task notification on FreeRTOS, which is
 *     the cheapest signal it has.
 *   - The consumer drains the whole queue before sleeping again, so a
 *     burst costs one wakeup rather than one per task.
 */
#include "worker.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "nn20_mpsc_queue.h"
#include "worker_port.h"

/* How long the thread sleeps when idle before looking again. Bounded so a
 * lost signal can only ever cost this much latency, never a hang. */
#define WORKER_IDLE_WAIT_MS 250u

struct nn20_worker_ctx {
    nn20_mpsc_queue queue;
    worker_thread_t thread;
    worker_event_t event;

    /* Written by the worker thread and by any caller of stop(); read by
     * both. Atomic because a plain bool here is a data race. */
    atomic_bool running;
    atomic_bool drain_before_exit;
    atomic_bool stop_requested;
    atomic_int core_id;

    /*
     * Statistics. Producers touch posted/rejected from any thread; the
     * others are written only by the worker thread. They are still atomic
     * because nn20_worker_get_stats() reads them from elsewhere - a plain
     * uint64_t here is a data race, which ThreadSanitizer duly reported.
     * The worker is the sole writer, so relaxed ordering is enough and
     * the increments stay effectively free.
     */
    atomic_ullong posted;
    atomic_ullong rejected;
    atomic_ullong executed;
    atomic_ullong failed;
    atomic_ullong wakeups;
    atomic_uint peak_depth;

    bool joined;
};

/* --------------------------------------------------------------- loop -- */

/*
 * Converting between a data pointer and a function pointer is not allowed
 * by a straight cast in ISO C. A union does it without undefined
 * behaviour and without a diagnostic, and every target here has the same
 * representation for both.
 */
typedef union {
    void *object;
    nn20_worker_callback function;
} callback_cast;

static size_t queue_depth(const nn20_worker_ctx *w)
{
    const size_t tail =
        atomic_load_explicit(&w->queue.tail.value, memory_order_acquire);
    const size_t head =
        atomic_load_explicit(&w->queue.head.value, memory_order_relaxed);
    return tail - head;
}

/*
 * A synchronous post. The request lives on the caller's stack for the
 * duration of the wait, so nothing is allocated here either.
 */
typedef struct {
    nn20_worker_callback callback;
    void *user_data;
    worker_completion_t done;
} sync_request;

static int sync_trampoline(nn20_worker_ctx *w, void *user_data)
{
    sync_request *req = (sync_request *)user_data;
    const int rc = req->callback != NULL ? req->callback(w, req->user_data)
                                         : 0;
    /* Signal last: the caller may reuse its stack the moment this returns. */
    worker_completion_signal(&req->done);
    return rc;
}

/* Drain everything currently queued. Returns how many ran. */
static uint32_t run_pending(nn20_worker_ctx *w)
{
    uint32_t ran = 0;
    for (;;) {
        void *raw_callback = NULL;
        void *raw_user = NULL;
        if (!nn20_mpsc_queue_pop(&w->queue, &raw_callback, &raw_user)) {
            break;
        }

        const callback_cast cast = {.object = raw_callback};
        nn20_worker_callback callback = cast.function;
        if (callback != NULL) {
            const int rc = callback(w, raw_user);
            atomic_fetch_add_explicit(&w->executed, 1ull,
                                      memory_order_relaxed);
            if (rc != 0) {
                atomic_fetch_add_explicit(&w->failed, 1ull,
                                          memory_order_relaxed);
            }
        }
        ran++;

        /* stop_now() while a burst is in flight: abandon the rest. */
        if (!atomic_load_explicit(&w->drain_before_exit, memory_order_acquire)
            && atomic_load_explicit(&w->stop_requested,
                                    memory_order_acquire)) {
            break;
        }
    }
    return ran;
}

/*
 * Drop everything still queued. Synchronous posts must still be released
 * or their callers wait forever on a worker that is never going to run
 * them again.
 */
static void discard_pending(nn20_worker_ctx *w)
{
    const callback_cast sync_marker = {.function = sync_trampoline};

    void *raw_callback = NULL;
    void *raw_user = NULL;
    while (nn20_mpsc_queue_pop(&w->queue, &raw_callback, &raw_user)) {
        if (raw_callback == sync_marker.object && raw_user != NULL) {
            sync_request *req = (sync_request *)raw_user;
            req->callback = NULL;   /* record that it never ran */
            worker_completion_signal(&req->done);
        }
        /* Nothing to free: slots hold borrowed pointers. */
    }
}

static void worker_loop(void *arg)
{
    nn20_worker_ctx *w = (nn20_worker_ctx *)arg;

    /* Register for notifications before the first wait, and record where
     * the scheduler actually put us. */
    worker_event_bind_current(&w->event);
    atomic_store_explicit(&w->core_id, worker_thread_current_core(),
                          memory_order_release);

    while (atomic_load_explicit(&w->running, memory_order_acquire)) {
        if (run_pending(w) > 0) {
            atomic_fetch_add_explicit(&w->wakeups, 1ull, memory_order_relaxed);
            continue;   /* more may have arrived while we worked */
        }

        if (atomic_load_explicit(&w->stop_requested, memory_order_acquire)) {
            break;   /* queue is empty and someone asked us to finish */
        }

        worker_event_wait(&w->event, WORKER_IDLE_WAIT_MS);
    }

    /* A drain-stop runs whatever arrived during the handshake. */
    if (atomic_load_explicit(&w->drain_before_exit, memory_order_acquire)) {
        run_pending(w);
    }
    atomic_store_explicit(&w->running, false, memory_order_release);
}

/* ---------------------------------------------------------- lifecycle -- */

/* Defined below; used by create to confirm the thread is running. */
int nn20_worker_post_sync(nn20_worker_ctx *worker,
                          nn20_worker_callback callback, void *user_data);

/* Used only to confirm the thread is up; see nn20_worker_create_with(). */
static int startup_barrier(nn20_worker_ctx *worker, void *user_data)
{
    (void)worker;
    (void)user_data;
    return 0;
}

static bool config_is_valid(const nn20_worker_config *c)
{
    if (c->queue_capacity != 0 &&
        !nn20_mpsc_queue_is_pow2(c->queue_capacity)) {
        return false;
    }
    if (c->queue_capacity == 1) {
        return false;   /* the ring needs at least two slots */
    }
    if (c->core_id != NN20_WORKER_CORE_ANY &&
        (c->core_id < 0 || c->core_id >= worker_port_core_count())) {
        return false;
    }
    return true;
}

nn20_worker_ctx *nn20_worker_create_with(const nn20_worker_config *config)
{
    nn20_worker_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.struct_size = (uint32_t)sizeof(cfg);
    cfg.core_id = NN20_WORKER_CORE_ANY;
    cfg.priority = NN20_WORKER_DEFAULT_PRIORITY;

    if (config != NULL) {
        if (config->struct_size < sizeof(nn20_worker_config)) {
            return NULL;
        }
        cfg = *config;
    }
    if (!config_is_valid(&cfg)) {
        return NULL;
    }

    nn20_worker_ctx *w = calloc(1, sizeof(*w));
    if (w == NULL) {
        return NULL;
    }

    const size_t capacity = cfg.queue_capacity != 0
                                ? cfg.queue_capacity
                                : NN20_WORKER_DEFAULT_QUEUE;
    if (!nn20_mpsc_queue_init(&w->queue, capacity)) {
        free(w);
        return NULL;
    }
    if (!worker_event_init(&w->event)) {
        nn20_mpsc_queue_deinit(&w->queue);
        free(w);
        return NULL;
    }

    atomic_init(&w->running, true);   /* true *before* the thread starts, so
                                       * delete() always joins */
    atomic_init(&w->drain_before_exit, false);
    atomic_init(&w->stop_requested, false);
    atomic_init(&w->core_id, NN20_WORKER_CORE_ANY);
    atomic_init(&w->posted, 0ull);
    atomic_init(&w->rejected, 0ull);
    atomic_init(&w->executed, 0ull);
    atomic_init(&w->failed, 0ull);
    atomic_init(&w->wakeups, 0ull);
    atomic_init(&w->peak_depth, 0u);

    const worker_thread_config_t tcfg = {
        .name = cfg.name != NULL ? cfg.name : "nn20-worker",
        .stack_bytes = cfg.stack_bytes != 0 ? cfg.stack_bytes
                                            : NN20_WORKER_DEFAULT_STACK,
        .priority = cfg.priority,
        .core_id = cfg.core_id,
    };
    if (!worker_thread_start(&w->thread, &tcfg, worker_loop, w)) {
        atomic_store(&w->running, false);
        worker_event_deinit(&w->event);
        nn20_mpsc_queue_deinit(&w->queue);
        free(w);
        return NULL;
    }

    /*
     * Wait for the thread to reach its loop before returning.
     *
     * The core it landed on is published by the thread itself, so without
     * this nn20_worker_core_id() reports "unknown" for the first moments
     * of the worker's life - a race the caller has no way to see coming.
     * A synchronous no-op costs microseconds once, and doubles as proof
     * that the worker really is able to run work.
     */
    (void)nn20_worker_post_sync(w, startup_barrier, NULL);

    /* The barrier is bookkeeping, not the caller's work. Clear the
     * counters so the statistics only ever describe posted tasks. */
    atomic_store_explicit(&w->posted, 0ull, memory_order_relaxed);
    atomic_store_explicit(&w->executed, 0ull, memory_order_relaxed);
    atomic_store_explicit(&w->wakeups, 0ull, memory_order_relaxed);
    atomic_store_explicit(&w->peak_depth, 0u, memory_order_relaxed);
    return w;
}

nn20_worker_ctx *nn20_worker_create(void)
{
    return nn20_worker_create_with(NULL);
}

/* Ask the thread to finish and wait for it. `drain` decides whether tasks
 * already queued still run. Idempotent. */
static void shutdown_worker(nn20_worker_ctx *w, bool drain)
{
    if (w == NULL || w->joined) {
        return;
    }
    atomic_store_explicit(&w->drain_before_exit, drain, memory_order_release);
    atomic_store_explicit(&w->stop_requested, true, memory_order_release);
    if (!drain) {
        atomic_store_explicit(&w->running, false, memory_order_release);
    }

    /*
     * Wake it directly rather than posting a stop task. The original
     * design queued a callback that cleared the flag, which deadlocked
     * whenever the queue happened to be full: the post failed, nothing
     * ever cleared the flag, and the join blocked forever.
     */
    worker_event_signal(&w->event);

    worker_thread_join(&w->thread);
    w->joined = true;
    atomic_store_explicit(&w->running, false, memory_order_release);
}

int nn20_worker_stop(nn20_worker_ctx *worker)
{
    if (worker == NULL) {
        return -1;
    }
    shutdown_worker(worker, true);
    return 0;
}

void nn20_worker_stop_now(nn20_worker_ctx *worker)
{
    if (worker == NULL) {
        return;
    }
    shutdown_worker(worker, false);
    discard_pending(worker);
}

void nn20_worker_delete(nn20_worker_ctx *worker)
{
    if (worker == NULL) {
        return;
    }
    shutdown_worker(worker, false);
    discard_pending(worker);
    worker_event_deinit(&worker->event);
    nn20_mpsc_queue_deinit(&worker->queue);
    free(worker);
}

/* ---------------------------------------------------------- submission -- */

int nn20_worker_post(nn20_worker_ctx *worker, nn20_worker_callback callback,
                     void *user_data)
{
    if (worker == NULL || callback == NULL) {
        return -1;
    }
    if (atomic_load_explicit(&worker->stop_requested, memory_order_acquire)) {
        return -1;   /* shutting down: refuse rather than queue forever */
    }

    /* No allocation: the slot carries the callback and its user pointer. */
    const callback_cast cast = {.function = callback};
    if (!nn20_mpsc_queue_push(&worker->queue, cast.object, user_data)) {
        atomic_fetch_add_explicit(&worker->rejected, 1ull,
                                  memory_order_relaxed);
        return 1;   /* full - backpressure, not a failure */
    }

    atomic_fetch_add_explicit(&worker->posted, 1ull, memory_order_relaxed);

    const uint32_t depth = (uint32_t)queue_depth(worker);
    uint32_t peak = atomic_load_explicit(&worker->peak_depth,
                                         memory_order_relaxed);
    while (depth > peak &&
           !atomic_compare_exchange_weak_explicit(&worker->peak_depth, &peak,
                                                  depth, memory_order_relaxed,
                                                  memory_order_relaxed)) {
        /* peak was reloaded by the exchange; retry */
    }

    worker_event_signal(&worker->event);
    return 0;
}

int nn20_worker_post_sync(nn20_worker_ctx *worker,
                          nn20_worker_callback callback, void *user_data)
{
    if (worker == NULL || callback == NULL) {
        return -1;
    }

    /*
     * Posted from the worker itself - almost always from inside a
     * callback. Waiting would mean waiting on this very thread to come
     * round again, which never happens. Run it inline instead.
     */
    if (worker_thread_is_current(&worker->thread)) {
        const int rc = callback(worker, user_data);
        atomic_fetch_add_explicit(&worker->executed, 1ull,
                                  memory_order_relaxed);
        if (rc != 0) {
            atomic_fetch_add_explicit(&worker->failed, 1ull,
                                      memory_order_relaxed);
        }
        return 0;
    }

    if (atomic_load_explicit(&worker->stop_requested, memory_order_acquire)) {
        return -1;
    }

    sync_request req;
    req.callback = callback;
    req.user_data = user_data;
    if (!worker_completion_init(&req.done)) {
        return -1;
    }

    const int posted = nn20_worker_post(worker, sync_trampoline, &req);
    if (posted != 0) {
        worker_completion_deinit(&req.done);
        return posted;   /* full, or refused: nothing ran, nothing waited */
    }

    worker_completion_wait(&req.done);
    /* req.callback is cleared by discard_pending() if the worker was
     * stopped before it got to us. */
    const bool ran = req.callback != NULL;
    worker_completion_deinit(&req.done);

    return ran ? 0 : -1;
}

/* ---------------------------------------------------------- accessors -- */

bool nn20_worker_is_running(const nn20_worker_ctx *worker)
{
    return worker != NULL &&
           atomic_load_explicit(&((nn20_worker_ctx *)worker)->running,
                                memory_order_acquire);
}

size_t nn20_worker_pending(const nn20_worker_ctx *worker)
{
    return worker == NULL ? 0u : queue_depth(worker);
}

int nn20_worker_core_id(const nn20_worker_ctx *worker)
{
    if (worker == NULL) {
        return NN20_WORKER_CORE_ANY;
    }
    return atomic_load_explicit(&((nn20_worker_ctx *)worker)->core_id,
                                memory_order_acquire);
}

void nn20_worker_get_stats(const nn20_worker_ctx *worker,
                           nn20_worker_stats *stats)
{
    if (worker == NULL || stats == NULL) {
        return;
    }
    nn20_worker_ctx *w = (nn20_worker_ctx *)worker;
    memset(stats, 0, sizeof(*stats));
    stats->posted = atomic_load_explicit(&w->posted, memory_order_relaxed);
    stats->rejected = atomic_load_explicit(&w->rejected, memory_order_relaxed);
    stats->executed = atomic_load_explicit(&w->executed,
                                           memory_order_relaxed);
    stats->failed = atomic_load_explicit(&w->failed, memory_order_relaxed);
    stats->wakeups = atomic_load_explicit(&w->wakeups, memory_order_relaxed);
    stats->peak_depth = atomic_load_explicit(&w->peak_depth,
                                             memory_order_relaxed);
}

int nn20_worker_core_count(void)
{
    return worker_port_core_count();
}
