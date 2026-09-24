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
#include "worker_port.h"

#include <string.h>

#if defined(ESP_PLATFORM)

/* --------------------------------------------------------- ESP-IDF -- */

typedef struct {
    void (*entry)(void *);
    void *arg;
    SemaphoreHandle_t finished;
} task_trampoline_t;

/*
 * FreeRTOS tasks must not return. This one signals that it is done and
 * then suspends itself, leaving worker_thread_join() to delete it.
 *
 * The obvious ending - vTaskDelete(NULL) - is what this used to do, and
 * it leaks. A task that deletes ITSELF cannot free its own stack while
 * it is still standing on it, so FreeRTOS defers the reclamation to the
 * idle task. join() returns as soon as the semaphore is given, long
 * before idle has run, so a caller that creates and destroys workers
 * without ever blocking sees the heap drain: 68 KB across eleven cycles
 * in the on-target test app, which then failed for want of memory in
 * places that had nothing to do with workers.
 *
 * Deleting a task from ANOTHER task frees its stack and control block
 * immediately, so the memory is back before join() returns. That is
 * what makes teardown deterministic rather than eventual.
 */
static void task_entry(void *param)
{
    task_trampoline_t *t = (task_trampoline_t *)param;
    void (*entry)(void *) = t->entry;
    void *arg = t->arg;
    SemaphoreHandle_t finished = t->finished;
    vPortFree(t);

    entry(arg);

    xSemaphoreGive(finished);

    /*
     * Wait to be deleted. The join may well have deleted this task
     * already - it can run the moment the semaphore above is given -
     * in which case this line is never reached, which is fine. The
     * loop is there because a suspended task can in principle be
     * resumed by something else; it must not fall out of this function.
     */
    for (;;) {
        vTaskSuspend(NULL);
    }
}

bool worker_thread_start(worker_thread_t *thread,
                         const worker_thread_config_t *config,
                         void (*entry)(void *), void *arg)
{
    if (thread == NULL || config == NULL || entry == NULL) {
        return false;
    }
    memset(thread, 0, sizeof(*thread));

    thread->finished = xSemaphoreCreateBinary();
    if (thread->finished == NULL) {
        return false;
    }

    task_trampoline_t *t = pvPortMalloc(sizeof(*t));
    if (t == NULL) {
        vSemaphoreDelete(thread->finished);
        thread->finished = NULL;
        return false;
    }
    t->entry = entry;
    t->arg = arg;
    t->finished = thread->finished;

    /* ESP-IDF takes the stack in bytes, not words. */
    const char *name = config->name ? config->name : "nn20-worker";
    BaseType_t rc;
    if (config->core_id >= 0 && config->core_id < portNUM_PROCESSORS) {
        rc = xTaskCreatePinnedToCore(task_entry, name, config->stack_bytes, t,
                                     (UBaseType_t)config->priority,
                                     &thread->handle,
                                     (BaseType_t)config->core_id);
    } else {
        rc = xTaskCreate(task_entry, name, config->stack_bytes, t,
                         (UBaseType_t)config->priority, &thread->handle);
    }

    if (rc != pdPASS) {
        vPortFree(t);
        vSemaphoreDelete(thread->finished);
        thread->finished = NULL;
        return false;
    }
    thread->started = true;
    return true;
}

void worker_thread_join(worker_thread_t *thread)
{
    if (thread == NULL || !thread->started) {
        return;
    }
    xSemaphoreTake(thread->finished, portMAX_DELAY);

    /*
     * From here, not from inside the task: see task_entry(). Deleting
     * another task frees its stack and control block on the spot, so
     * the memory is available again by the time this returns.
     */
    if (thread->handle != NULL) {
        vTaskDelete(thread->handle);
    }

    vSemaphoreDelete(thread->finished);
    thread->finished = NULL;
    thread->handle = NULL;
    thread->started = false;
}

int worker_thread_current_core(void)
{
    return (int)xPortGetCoreID();
}

int worker_port_core_count(void)
{
    return (int)portNUM_PROCESSORS;
}

bool worker_event_init(worker_event_t *event)
{
    if (event == NULL) {
        return false;
    }
    /* Nothing to allocate: the notification lives in the task control
     * block. The handle is filled in by worker_event_bind_current(). */
    event->waiter = NULL;
    return true;
}

void worker_event_deinit(worker_event_t *event)
{
    if (event != NULL) {
        event->waiter = NULL;
    }
}

void worker_event_bind_current(worker_event_t *event)
{
    if (event != NULL) {
        event->waiter = xTaskGetCurrentTaskHandle();
    }
}

void worker_event_signal(worker_event_t *event)
{
    if (event == NULL) {
        return;
    }
    TaskHandle_t waiter = event->waiter;
    if (waiter != NULL) {
        /* The notification counter latches, so a signal that races the
         * wait is remembered rather than lost. */
        xTaskNotifyGive(waiter);
    }
}

void worker_event_wait(worker_event_t *event, uint32_t timeout_ms)
{
    (void)event;
    /* pdTRUE: clear on exit, so each wait consumes all pending signals. */
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(timeout_ms));
}

bool worker_thread_is_current(const worker_thread_t *thread)
{
    return thread != NULL && thread->started &&
           thread->handle == xTaskGetCurrentTaskHandle();
}

bool worker_completion_init(worker_completion_t *completion)
{
    if (completion == NULL) {
        return false;
    }
    completion->handle = xSemaphoreCreateBinaryStatic(&completion->storage);
    return completion->handle != NULL;
}

void worker_completion_deinit(worker_completion_t *completion)
{
    if (completion != NULL && completion->handle != NULL) {
        vSemaphoreDelete(completion->handle);
        completion->handle = NULL;
    }
}

void worker_completion_signal(worker_completion_t *completion)
{
    if (completion != NULL && completion->handle != NULL) {
        xSemaphoreGive(completion->handle);
    }
}

void worker_completion_wait(worker_completion_t *completion)
{
    if (completion != NULL && completion->handle != NULL) {
        xSemaphoreTake(completion->handle, portMAX_DELAY);
    }
}

#else

/* ---------------------------------------------------------- POSIX -- */

#include <errno.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    void (*entry)(void *);
    void *arg;
} thread_trampoline_t;

static void *thread_entry(void *param)
{
    thread_trampoline_t *t = (thread_trampoline_t *)param;
    void (*entry)(void *) = t->entry;
    void *arg = t->arg;
    free(t);
    entry(arg);
    return NULL;
}

bool worker_thread_start(worker_thread_t *thread,
                         const worker_thread_config_t *config,
                         void (*entry)(void *), void *arg)
{
    if (thread == NULL || config == NULL || entry == NULL) {
        return false;
    }
    memset(thread, 0, sizeof(*thread));

    thread_trampoline_t *t = malloc(sizeof(*t));
    if (t == NULL) {
        return false;
    }
    t->entry = entry;
    t->arg = arg;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    if (config->stack_bytes > 0) {
        size_t stack = config->stack_bytes;
        if (stack < (size_t)PTHREAD_STACK_MIN) {
            stack = (size_t)PTHREAD_STACK_MIN;
        }
        pthread_attr_setstacksize(&attr, stack);
    }

#if defined(__linux__)
    /*
     * Affinity goes on the attributes, not on the running thread. Setting
     * it after pthread_create() leaves a window in which the thread is
     * already executing unpinned - long enough, in practice, for it to
     * record the wrong core and to do its first work on it.
     * xTaskCreatePinnedToCore() has no such window, so this keeps the two
     * platforms behaving the same.
     */
    cpu_set_t set;
    if (config->core_id >= 0 && config->core_id < worker_port_core_count()) {
        CPU_ZERO(&set);
        CPU_SET((size_t)config->core_id, &set);
        /* Best effort: a container may forbid it, and running unpinned is
         * better than not running. */
        (void)pthread_attr_setaffinity_np(&attr, sizeof(set), &set);
    }
#endif

    const int rc = pthread_create(&thread->handle, &attr, thread_entry, t);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        free(t);
        return false;
    }
    thread->started = true;
    thread->joined = false;

#if defined(__linux__)
    if (config->name != NULL) {
        (void)pthread_setname_np(thread->handle, config->name);
    }
#endif
    return true;
}

void worker_thread_join(worker_thread_t *thread)
{
    if (thread == NULL || !thread->started || thread->joined) {
        return;
    }
    pthread_join(thread->handle, NULL);
    thread->joined = true;
    thread->started = false;
}

int worker_thread_current_core(void)
{
#if defined(__linux__)
    const int cpu = sched_getcpu();
    return cpu < 0 ? NN20_WORKER_CORE_ANY : cpu;
#else
    return NN20_WORKER_CORE_ANY;
#endif
}

int worker_port_core_count(void)
{
    const long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
}

bool worker_event_init(worker_event_t *event)
{
    if (event == NULL) {
        return false;
    }
    memset(event, 0, sizeof(*event));
    if (pthread_mutex_init(&event->mutex, NULL) != 0) {
        return false;
    }
    if (pthread_cond_init(&event->cond, NULL) != 0) {
        pthread_mutex_destroy(&event->mutex);
        return false;
    }
    event->valid = true;
    return true;
}

void worker_event_deinit(worker_event_t *event)
{
    if (event == NULL || !event->valid) {
        return;
    }
    pthread_cond_destroy(&event->cond);
    pthread_mutex_destroy(&event->mutex);
    event->valid = false;
}

void worker_event_bind_current(worker_event_t *event)
{
    (void)event;   /* the condvar needs no per-thread registration */
}

void worker_event_signal(worker_event_t *event)
{
    if (event == NULL || !event->valid) {
        return;
    }
    pthread_mutex_lock(&event->mutex);
    /* Sticky, matching the FreeRTOS binary semaphore: a signal that
     * arrives just before the wait must not be lost. */
    event->flag = true;
    pthread_cond_signal(&event->cond);
    pthread_mutex_unlock(&event->mutex);
}

void worker_event_wait(worker_event_t *event, uint32_t timeout_ms)
{
    if (event == NULL || !event->valid) {
        return;
    }
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_nsec += (long)(timeout_ms % 1000u) * 1000000L;
    deadline.tv_sec += (time_t)(timeout_ms / 1000u);
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec += 1;
        deadline.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock(&event->mutex);
    while (!event->flag) {
        if (pthread_cond_timedwait(&event->cond, &event->mutex,
                                   &deadline) == ETIMEDOUT) {
            break;
        }
    }
    event->flag = false;
    pthread_mutex_unlock(&event->mutex);
}


bool worker_thread_is_current(const worker_thread_t *thread)
{
    return thread != NULL && thread->started &&
           pthread_equal(pthread_self(), thread->handle) != 0;
}

bool worker_completion_init(worker_completion_t *completion)
{
    if (completion == NULL) {
        return false;
    }
    memset(completion, 0, sizeof(*completion));
    if (pthread_mutex_init(&completion->mutex, NULL) != 0) {
        return false;
    }
    if (pthread_cond_init(&completion->cond, NULL) != 0) {
        pthread_mutex_destroy(&completion->mutex);
        return false;
    }
    completion->valid = true;
    return true;
}

void worker_completion_deinit(worker_completion_t *completion)
{
    if (completion == NULL || !completion->valid) {
        return;
    }
    pthread_cond_destroy(&completion->cond);
    pthread_mutex_destroy(&completion->mutex);
    completion->valid = false;
}

void worker_completion_signal(worker_completion_t *completion)
{
    if (completion == NULL || !completion->valid) {
        return;
    }
    pthread_mutex_lock(&completion->mutex);
    completion->done = true;
    pthread_cond_signal(&completion->cond);
    pthread_mutex_unlock(&completion->mutex);
}

void worker_completion_wait(worker_completion_t *completion)
{
    if (completion == NULL || !completion->valid) {
        return;
    }
    pthread_mutex_lock(&completion->mutex);
    while (!completion->done) {
        pthread_cond_wait(&completion->cond, &completion->mutex);
    }
    pthread_mutex_unlock(&completion->mutex);
}

#endif
