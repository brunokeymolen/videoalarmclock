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
 * worker_port.h - private thread + event glue for the worker.
 *
 * FreeRTOS tasks pinned to a core on the ESP32-P4, pthreads with CPU
 * affinity on a desktop. Nothing above this header knows which it is.
 *
 * The P4 is dual core: pinning matters because the receive path and the
 * embedding pipeline should not fight over the same core.
 */
#ifndef NN20_WORKER_PORT_H
#define NN20_WORKER_PORT_H

#include <stdbool.h>
#include <stdint.h>

#if defined(ESP_PLATFORM)
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

typedef struct {
    TaskHandle_t handle;
    /* FreeRTOS has no join; the task gives this before it deletes itself. */
    SemaphoreHandle_t finished;
    bool started;
} worker_thread_t;

/*
 * Direct-to-task notification: the cheapest wakeup FreeRTOS offers, ~45%
 * faster than a binary semaphore and with no extra object to allocate.
 * The waiting task records its own handle here at startup.
 */
typedef struct {
    TaskHandle_t waiter;
} worker_event_t;

/* One-shot completion for a synchronous post. Static storage, so the
 * caller can put it on its stack and no allocation happens per call. */
typedef struct {
    StaticSemaphore_t storage;
    SemaphoreHandle_t handle;
} worker_completion_t;

#else
#include <pthread.h>

typedef struct {
    pthread_t handle;
    bool started;
    bool joined;
} worker_thread_t;

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    bool flag;
    bool valid;
} worker_event_t;

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    bool done;
    bool valid;
} worker_completion_t;
#endif

/* Bind to no particular core. */
#define NN20_WORKER_CORE_ANY (-1)

typedef struct {
    const char *name;
    uint32_t stack_bytes;
    int priority;
    int core_id;   /* NN20_WORKER_CORE_ANY for no affinity */
} worker_thread_config_t;

/*
 * Start `entry` on a new thread. Returns false and leaves the handle
 * unstarted on failure. Requesting a core that does not exist is not an
 * error: the thread runs unpinned and worker_thread_core_id() reports
 * that, because refusing to run at all would be worse.
 */
bool worker_thread_start(worker_thread_t *thread,
                         const worker_thread_config_t *config,
                         void (*entry)(void *), void *arg);

/* Wait for the thread to finish. Safe to call more than once. */
void worker_thread_join(worker_thread_t *thread);

/* Core the calling thread is running on, or NN20_WORKER_CORE_ANY. */
int worker_thread_current_core(void);

/* How many cores this platform reports. */
int worker_port_core_count(void);

bool worker_event_init(worker_event_t *event);
void worker_event_deinit(worker_event_t *event);

/*
 * Called by the waiting thread once, before its first wait. On FreeRTOS
 * this records the task handle that notifications are sent to; elsewhere
 * it does nothing.
 */
void worker_event_bind_current(worker_event_t *event);

/* Wake a waiter. Signalling with nothing waiting is remembered, so a
 * signal that races with the wait is not lost. */
void worker_event_signal(worker_event_t *event);

/* Wait for a signal or the timeout, whichever comes first. */
void worker_event_wait(worker_event_t *event, uint32_t timeout_ms);

/* True when the caller is the thread this handle refers to. Used to spot
 * a synchronous post issued from inside the worker, which would otherwise
 * wait for itself forever. */
bool worker_thread_is_current(const worker_thread_t *thread);

/* One-shot completion, signalled exactly once. */
bool worker_completion_init(worker_completion_t *completion);
void worker_completion_deinit(worker_completion_t *completion);
void worker_completion_signal(worker_completion_t *completion);
void worker_completion_wait(worker_completion_t *completion);

#endif /* NN20_WORKER_PORT_H */
