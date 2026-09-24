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
 * nn20clock_workers.c - creates design 4's threads.
 *
 * The defaults table below is the threading model. Everything else in
 * this file is bookkeeping around it.
 */
#include "nn20clock_workers.h"

#include <stdlib.h>
#include <string.h>

static const char *TAG = "NN20CLOCK_WORKERS";

/*
 * Defaults, per worker.
 *
 * Stacks: the UI worker gets the largest because LVGL rendering and,
 * later, the video path run on it; storage next, because FATFS and the
 * NVS paths are not shallow; the manager and core workers only route
 * messages.
 *
 * Priorities are relative to FreeRTOS's default of 5. The manager and UI
 * sit at 5 so a timer event reaches the screen promptly. Storage and
 * core sit one below: they do the work nobody is watching, and a blocked
 * SD read must not hold the display back.
 *
 * Pinning: the P4 has two cores. The UI worker is pinned to core 1 and
 * kept there - once video decode lands it must not compete with
 * orchestration or a stalled SD read. The rest stay unpinned; there is
 * no measurement yet to justify choosing a core for them, and pinning
 * without one just removes the scheduler's freedom. Revisit at
 * Milestone 7 with numbers.
 */
typedef struct {
    const char *name;         /* design 4's name, used in logs */
    const char *thread_name;  /* what the RTOS/OS shows */
    uint32_t queue_capacity;
    uint32_t stack_bytes;
    int priority;
    int core_id;
} WorkerDefaults;

static const WorkerDefaults DEFAULTS[NN20CLOCK_WORKER_COUNT] = {
    [NN20CLOCK_WORKER_CORE] = {
        .name = "CoreWorker",
        .thread_name = "nn20-core",
        .queue_capacity = 64u,
        .stack_bytes = 4096u,
        .priority = 4,
        .core_id = NN20_WORKER_CORE_ANY,
    },
    [NN20CLOCK_WORKER_STORAGE] = {
        .name = "StorageWorker",
        .thread_name = "nn20-storage",
        .queue_capacity = 64u,
        .stack_bytes = 6144u,
        .priority = 4,
        .core_id = NN20_WORKER_CORE_ANY,
    },
    [NN20CLOCK_WORKER_CLOCK_MANAGER] = {
        .name = "ClockManagerWorker",
        .thread_name = "nn20-manager",
        .queue_capacity = 128u,
        .stack_bytes = 4096u,
        .priority = 5,
        .core_id = NN20_WORKER_CORE_ANY,
    },
    [NN20CLOCK_WORKER_UI] = {
        .name = "UiWorker",
        .thread_name = "nn20-ui",
        .queue_capacity = 256u,
        .stack_bytes = 8192u,
        .priority = 5,
        .core_id = 1,
    },
    [NN20CLOCK_WORKER_PLAYER] = {
        .name = "PlayerWorker",
        .thread_name = "nn20-player",
        /* One job at a time, and it lasts as long as the alarm rings.
         * The queue is for the stop that follows it, not for a backlog. */
        .queue_capacity = 8u,
        /* stdio on FATFS, the AVI walk, and the decode calls, all
         * nested. Not a place to be tight. */
        .stack_bytes = 8192u,
        /* Below the UI worker it feeds frames to, and below the manager
         * that has to be able to stop it. A late frame is a stutter; a
         * late stop is an alarm that will not turn off. */
        .priority = 4,
        /* Core 0, opposite the UI worker: decoding a frame and drawing
         * the previous one then genuinely overlap instead of taking
         * turns. */
        .core_id = 0,
    },
    [NN20CLOCK_WORKER_READER] = {
        .name = "ReaderWorker",
        .thread_name = "nn20-reader",
        /* One long-running fill job per file, like the player's. */
        .queue_capacity = 8u,
        /* stdio on FATFS and nothing else - no decode, no nesting. */
        .stack_bytes = 4096u,
        /* Above the player it feeds. The player can afford to wait for
         * a block; the reader waiting for the player is the stall this
         * whole arrangement exists to remove. */
        .priority = 5,
        /* Core 0 with the player. They are never both runnable for
         * long - the reader blocks on the card and the player on the
         * codec - and pinning them together leaves core 1 to the UI
         * worker, which is the one doing the panel copies. */
        .core_id = 0,
    },
};

struct NN20ClockWorkers {
    nn20_worker_ctx *workers[NN20CLOCK_WORKER_COUNT];
};

/* --------------------------------------------------------- lifecycle -- */

esp_err_t nn20clock_workers_default_config(NN20ClockWorkersConfig *out_config)
{
    if (out_config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out_config, 0, sizeof(*out_config));
    out_config->struct_size = sizeof(*out_config);
    for (int i = 0; i < NN20CLOCK_WORKER_COUNT; i++) {
        out_config->workers[i].queue_capacity = DEFAULTS[i].queue_capacity;
        out_config->workers[i].stack_bytes = DEFAULTS[i].stack_bytes;
        out_config->workers[i].priority = DEFAULTS[i].priority;
        out_config->workers[i].core_id = DEFAULTS[i].core_id;
    }
    return ESP_OK;
}

/* Resolve one spec against the defaults. See NN20ClockWorkerSpec: 0 means
 * "unset" for everything except core_id, where 0 is a real core. */
static nn20_worker_config resolve(int id, const NN20ClockWorkerSpec *spec)
{
    const WorkerDefaults *fallback = &DEFAULTS[id];

    nn20_worker_config config = {
        .struct_size = sizeof(config),
        .name = fallback->thread_name,
        .queue_capacity = fallback->queue_capacity,
        .stack_bytes = fallback->stack_bytes,
        .priority = fallback->priority,
        .core_id = fallback->core_id,
    };

    if (spec != NULL) {
        if (spec->queue_capacity != 0u) {
            config.queue_capacity = spec->queue_capacity;
        }
        if (spec->stack_bytes != 0u) {
            config.stack_bytes = spec->stack_bytes;
        }
        if (spec->priority != 0) {
            config.priority = spec->priority;
        }
        if (spec->core_id != NN20CLOCK_WORKER_CORE_DEFAULT) {
            config.core_id = spec->core_id;
        }
    }

    return config;
}

NN20ClockWorkers *nn20clock_workers_ctor(void)
{
    return nn20clock_workers_ctor_with(NULL);
}

NN20ClockWorkers *nn20clock_workers_ctor_with(
    const NN20ClockWorkersConfig *config)
{
    if (config != NULL && config->struct_size != sizeof(*config)) {
        ESP_LOGE(TAG, "config struct_size %u, expected %u",
                 (unsigned)config->struct_size, (unsigned)sizeof(*config));
        return NULL;
    }

    NN20ClockWorkers *pthis = calloc(1, sizeof(*pthis));
    if (pthis == NULL) {
        ESP_LOGE(TAG, "out of memory");
        return NULL;
    }

    /* Created in id order, which is CoreWorker first and UiWorker last;
     * the dtor unwinds in the opposite direction. */
    for (int i = 0; i < NN20CLOCK_WORKER_COUNT; i++) {
        const NN20ClockWorkerSpec *spec =
            (config != NULL) ? &config->workers[i] : NULL;
        const nn20_worker_config worker_config = resolve(i, spec);

        pthis->workers[i] = nn20_worker_create_with(&worker_config);
        if (pthis->workers[i] == NULL) {
            ESP_LOGE(TAG, "%s would not start", DEFAULTS[i].name);
            /* Partial set: unwind what did start rather than hand back a
             * struct with holes in it. */
            nn20clock_workers_dtor(pthis);
            return NULL;
        }

        ESP_LOGI(TAG, "%s on core %d, %u B stack, priority %d",
                 DEFAULTS[i].name, nn20_worker_core_id(pthis->workers[i]),
                 (unsigned)worker_config.stack_bytes, worker_config.priority);
    }

    return pthis;
}

void nn20clock_workers_dtor(NN20ClockWorkers *pthis)
{
    if (pthis == NULL) {
        return;
    }

    /* Reverse creation order: the UI worker is the last thread standing,
     * so teardown work posted to it by the others still runs. */
    for (int i = NN20CLOCK_WORKER_COUNT - 1; i >= 0; i--) {
        nn20_worker_delete(pthis->workers[i]);
        pthis->workers[i] = NULL;
    }

    free(pthis);
}

esp_err_t nn20clock_workers_stop(NN20ClockWorkers *pthis)
{
    if (pthis == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t result = ESP_OK;
    for (int i = NN20CLOCK_WORKER_COUNT - 1; i >= 0; i--) {
        /* Drains first, so queued work is not silently dropped. Already
         * stopped is not an error: nn20_worker_stop() is idempotent. */
        if (nn20_worker_stop(pthis->workers[i]) != 0) {
            ESP_LOGW(TAG, "%s did not stop cleanly", DEFAULTS[i].name);
            result = ESP_FAIL;
        }
    }
    return result;
}

/* --------------------------------------------------------- accessors -- */

nn20_worker_ctx *nn20clock_workers_get(const NN20ClockWorkers *pthis,
                                       NN20ClockWorkerId id)
{
    if (pthis == NULL || id < 0 || id >= NN20CLOCK_WORKER_COUNT) {
        return NULL;
    }
    return pthis->workers[id];
}

nn20_worker_ctx *nn20clock_workers_ui(const NN20ClockWorkers *pthis)
{
    return nn20clock_workers_get(pthis, NN20CLOCK_WORKER_UI);
}

nn20_worker_ctx *nn20clock_workers_storage(const NN20ClockWorkers *pthis)
{
    return nn20clock_workers_get(pthis, NN20CLOCK_WORKER_STORAGE);
}

nn20_worker_ctx *nn20clock_workers_clock_manager(const NN20ClockWorkers *pthis)
{
    return nn20clock_workers_get(pthis, NN20CLOCK_WORKER_CLOCK_MANAGER);
}

nn20_worker_ctx *nn20clock_workers_core(const NN20ClockWorkers *pthis)
{
    return nn20clock_workers_get(pthis, NN20CLOCK_WORKER_CORE);
}

nn20_worker_ctx *nn20clock_workers_player(const NN20ClockWorkers *pthis)
{
    return nn20clock_workers_get(pthis, NN20CLOCK_WORKER_PLAYER);
}

nn20_worker_ctx *nn20clock_workers_reader(const NN20ClockWorkers *pthis)
{
    return nn20clock_workers_get(pthis, NN20CLOCK_WORKER_READER);
}

const char *nn20clock_workers_name(NN20ClockWorkerId id)
{
    if (id < 0 || id >= NN20CLOCK_WORKER_COUNT) {
        return "UnknownWorker";
    }
    return DEFAULTS[id].name;
}

bool nn20clock_workers_are_running(const NN20ClockWorkers *pthis)
{
    if (pthis == NULL) {
        return false;
    }
    for (int i = 0; i < NN20CLOCK_WORKER_COUNT; i++) {
        if (!nn20_worker_is_running(pthis->workers[i])) {
            return false;
        }
    }
    return true;
}

void nn20clock_workers_log_stats(const NN20ClockWorkers *pthis)
{
    if (pthis == NULL) {
        return;
    }

    for (int i = 0; i < NN20CLOCK_WORKER_COUNT; i++) {
        nn20_worker_stats stats = {0};
        nn20_worker_get_stats(pthis->workers[i], &stats);

        ESP_LOGI(TAG,
                 "%-18s core %d  pending %u  peak %u  posted %llu  "
                 "executed %llu  rejected %llu  failed %llu",
                 DEFAULTS[i].name, nn20_worker_core_id(pthis->workers[i]),
                 (unsigned)nn20_worker_pending(pthis->workers[i]),
                 (unsigned)stats.peak_depth,
                 (unsigned long long)stats.posted,
                 (unsigned long long)stats.executed,
                 (unsigned long long)stats.rejected,
                 (unsigned long long)stats.failed);
    }
}
