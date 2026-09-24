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
 * nn20clock_storage.c - Milestone 1 Storage facade (design 9).
 *
 * The config record lives in `owned` below. Only callbacks running on
 * the StorageWorker touch it; every public function either posts one of
 * those callbacks or reads a value that no worker callback writes. That
 * is the whole synchronization story - there is no mutex in this file,
 * and adding one would mean the ownership rule had been broken
 * somewhere.
 *
 * The flash side is NVS, on the target only. Both records - the config
 * and the alarm list - are stored as single blobs rather than as
 * per-field keys: one write per change is atomic from the reader's
 * point of view, and easier on a flash part than a dozen small ones.
 *
 * Writing raw structs to flash is a decision with a cost. It is fast
 * and simple, and it means a struct layout change invalidates the
 * stored data - which is what the schema version guards: on any
 * mismatch, or any blob whose size is not what this build expects, the
 * stored record is discarded and the defaults are used. That is the
 * honest behaviour for a clock (it forgets your alarms once, after a
 * firmware change) and design 17 puts real migration in Milestone 12.
 *
 * The SD side (design 13, media) is still absent - Milestone 6.
 */
#include "nn20clock_storage.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nn20clock_brightness.h"
#include "nn20clock_reqpool.h"

#if defined(ESP_PLATFORM)
#include "nvs.h"
#include "nvs_flash.h"
#endif

static const char *TAG = "NN20CLOCK_STORAGE";

/* Defaults for a device that has never been configured. Europe/Brussels
 * matches where this clock is being built; design 14 makes the timezone
 * a stored setting, so this is only the starting value. */
#define DEFAULT_TIMEZONE   "CET-1CEST,M3.5.0,M10.5.0/3"
/*
 * The starting schedule: bright from breakfast, dim from the evening.
 *
 * Night is well above the display's floor
 * (NN20CLOCK_DISPLAY_MIN_ON_BRIGHTNESS, below which the backlight goes
 * dark rather than dim) - a clock is meant to be readable across a dark
 * room, not merely lit. 07:00 to 20:00 is a guess at a household, and
 * all four are meant to be changed on the screen.
 *
 * These apply to a device that has never stored a config. A device that
 * has one keeps it: changing them here does nothing to a clock already
 * in use, which is the point of storing settings at all.
 */
#define DEFAULT_BRIGHTNESS_DAY   85u
#define DEFAULT_BRIGHTNESS_NIGHT 30u
#define DEFAULT_DAY_START_MIN    (7u * 60u)
#define DEFAULT_NIGHT_START_MIN  (20u * 60u)
/* No playback minimum: a film follows the schedule until somebody asks
 * for more. Also what a record from before the minimum existed gets. */
#define DEFAULT_BRIGHTNESS_PLAYBACK_MIN 0u
#define DEFAULT_VOLUME     60u

/* Saves in flight at once. Config saves come from settings screens and
 * the odd time-sync write, so four is generous. */
#define STORAGE_MAX_PENDING 4

/*
 * One namespace, two blobs. The names are part of the on-flash format:
 * changing them orphans whatever is already stored.
 *
 * Defined on both platforms even though only the target has NVS - the
 * host's no-op nvs_store() takes the same key, so the call sites stay
 * identical rather than sprouting an #if each.
 *
 * The namespace is per-instance rather than a constant: the on-target
 * tests use their own, so a test run cannot reconfigure the device it
 * is running on.
 */
#define NVS_KEY_CONFIG  "config"
/*
 * The alarm blob's key carries its layout, and MUST be changed whenever
 * NN20ClockAlarmConfig changes.
 *
 * Unlike the config record, the alarm list has no version field inside
 * it - the only check is that the stored blob is the size this build
 * expects. That check is weaker than it looks: dropping the per-alarm
 * volume (a uint16_t) changed the struct by nothing at all, because the
 * padding absorbed it, while moving snooze_minutes two bytes down. An
 * old blob would have passed the size check and every alarm would have
 * come back with a 70-minute snooze.
 *
 * A new key cannot be misread: the old blob is simply not found, and
 * the alarms start empty. Design 17 makes real migration a Milestone 12
 * concern; until then, forgetting once is the documented behaviour.
 *
 * The superseded key must also be ERASED - see LEGACY_KEYS. The list is
 * over five kilobytes, and leaving the old one behind fills the NVS
 * partition, at which point nothing can be saved at all.
 */
#define NVS_KEY_ALARMS  "alarms_v2"

/*
 * Keys this build no longer writes, erased once at startup.
 *
 * Not housekeeping: the first target run after the rename failed every
 * save with ESP_ERR_NVS_NOT_ENOUGH_SPACE, because the partition was
 * holding two 5.5 KB alarm lists and had room for neither. Anything
 * added to NVS_KEY_ALARMS's history belongs here.
 */
#if defined(ESP_PLATFORM)
static const char *const LEGACY_KEYS[] = { "alarms" };
#endif

/*
 * The layout that key describes, pinned.
 *
 * Changing NN20ClockAlarmConfig without changing the key above is the
 * failure this guards: the size check cannot see it, and the alarms
 * come back quietly wrong. If one of these fails, that is the point -
 * take the new key, do not adjust the number.
 */
_Static_assert(sizeof(NN20ClockAlarmConfig) == 348u,
               "alarm layout changed: take a new NVS_KEY_ALARMS");
_Static_assert(offsetof(NN20ClockAlarmConfig, snooze_minutes) == 344u,
               "alarm layout changed: take a new NVS_KEY_ALARMS");

/*
 * The config record one version back, which load() still accepts.
 *
 * Version 4 is version 5 without brightness_playback_min, and that only
 * holds while the field comes straight after what used to be the last
 * one. Moving it breaks the migration without breaking the build -
 * unless this does.
 */
#define CONFIG_SCHEMA_PREVIOUS 4u
#define CONFIG_PREVIOUS_SIZE offsetof(NN20ClockConfig, brightness_playback_min)
_Static_assert(offsetof(NN20ClockConfig, wifi_password) +
                       NN20CLOCK_WIFI_PASSWORD_MAX ==
                   CONFIG_PREVIOUS_SIZE,
               "brightness_playback_min must follow the version-4 layout");

typedef struct {
    NN20ClockStorage *storage;
    NN20ClockConfig config;              /* copied, not borrowed */
    NN20ClockStorageDoneFn on_done;
    void *user_data;
    size_t slot;
} SaveRequest;

struct NN20ClockStorage {
    nn20_worker_ctx *worker;   /* borrowed; owned by NN20ClockWorkers */
    const char *nvs_namespace; /* borrowed; not copied */

    /* ---- worker-owned. Read and written only on the StorageWorker. -- */
    NN20ClockConfig config;

    NN20ClockAlarmList alarms;

    /* Written on the worker, read from any thread, so atomic - a
     * defined snapshot rather than a data race. Not a lock. */
    atomic_bool ready;

#if defined(ESP_PLATFORM)
    nvs_handle_t nvs;
    bool nvs_open;
#endif

    /* ---- claimed from any thread, read on the worker. --------------- */
    SaveRequest requests[STORAGE_MAX_PENDING];
    atomic_bool request_in_use[STORAGE_MAX_PENDING];
    NN20ClockReqPool request_pool;
};

/* ------------------------------------------------------ persistence -- */

/*
 * All four run on the StorageWorker. On the host they are no-ops that
 * report success: the records live in RAM, which is exactly what
 * nn20clock_storage_is_persistent() tells callers.
 */

#if defined(ESP_PLATFORM)

static esp_err_t nvs_load(NN20ClockStorage *pthis)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* The partition is unusable as it stands - a firmware update
         * changed the format, or it was never initialized. Erasing
         * costs the stored settings, which is better than refusing to
         * boot. */
        ESP_LOGW(TAG, "NVS partition needs erasing");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_open(pthis->nvs_namespace, NVS_READWRITE, &pthis->nvs);
    if (err != ESP_OK) {
        return err;
    }
    pthis->nvs_open = true;

    /*
     * Before anything is read or written: the space the old records
     * occupy is space this build needs. Absent keys are the normal
     * case - every boot after the first - so NOT_FOUND is not news.
     */
    for (size_t i = 0; i < sizeof(LEGACY_KEYS) / sizeof(LEGACY_KEYS[0]); i++) {
        const esp_err_t erased = nvs_erase_key(pthis->nvs, LEGACY_KEYS[i]);
        if (erased == ESP_OK) {
            ESP_LOGI(TAG, "erased superseded record '%s'", LEGACY_KEYS[i]);
            (void)nvs_commit(pthis->nvs);
        } else if (erased != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "could not erase '%s' (0x%x)", LEGACY_KEYS[i],
                     (unsigned)erased);
        }
    }

    /* A blob of the wrong size is from a different build of this
     * struct. Discarding it is the schema check: see the file comment. */
    /* NN20ClockConfig is small enough for the stack; the alarm list
     * below is emphatically not. */
    size_t size = sizeof(pthis->config);
    NN20ClockConfig stored = {0};
    err = nvs_get_blob(pthis->nvs, NVS_KEY_CONFIG, &stored, &size);
    if (err == ESP_OK && size == sizeof(stored) &&
        stored.schema_version == NN20CLOCK_STORAGE_SCHEMA_VERSION) {
        pthis->config = stored;
        ESP_LOGI(TAG, "config loaded from flash");
    } else if (err == ESP_OK &&
               stored.schema_version == CONFIG_SCHEMA_PREVIOUS &&
               size >= CONFIG_PREVIOUS_SIZE && size <= sizeof(stored)) {
        /*
         * The previous layout, which is this one short of its last
         * field. The blob may still be sizeof(stored) - the new byte sat
         * in what was tail padding - so whatever was read into it is
         * not trusted either way. Not written back here: the next save
         * stores the current version, and until then this is reread and
         * migrated again at no cost.
         */
        stored.brightness_playback_min = DEFAULT_BRIGHTNESS_PLAYBACK_MIN;
        stored.schema_version = NN20CLOCK_STORAGE_SCHEMA_VERSION;
        pthis->config = stored;
        ESP_LOGI(TAG, "config loaded from flash (version %u, migrated)",
                 (unsigned)CONFIG_SCHEMA_PREVIOUS);
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "no stored config; using defaults");
    } else {
        ESP_LOGW(TAG, "stored config unusable (0x%x, %u B); using defaults",
                 (unsigned)err, (unsigned)size);
    }

    /* Read straight into the owned list rather than via a 5.5 KB stack
     * copy. A partial or wrong-sized read is discarded below, so
     * reading in place costs nothing. */
    size = sizeof(pthis->alarms);
    NN20ClockAlarmList *alarms = &pthis->alarms;
    err = nvs_get_blob(pthis->nvs, NVS_KEY_ALARMS, alarms, &size);
    if (err == ESP_OK && size == sizeof(*alarms) &&
        alarms->count <= NN20CLOCK_ALARM_MAX) {
        ESP_LOGI(TAG, "%u alarm(s) loaded from flash",
                 (unsigned)pthis->alarms.count);
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        memset(&pthis->alarms, 0, sizeof(pthis->alarms));
        ESP_LOGI(TAG, "no stored alarms");
    } else {
        memset(&pthis->alarms, 0, sizeof(pthis->alarms));
        ESP_LOGW(TAG, "stored alarms unusable (0x%x, %u B); starting empty",
                 (unsigned)err, (unsigned)size);
    }

    return ESP_OK;
}

static esp_err_t nvs_store(NN20ClockStorage *pthis, const char *key,
                           const void *data, size_t size)
{
    if (!pthis->nvs_open) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = nvs_set_blob(pthis->nvs, key, data, size);
    if (err == ESP_OK) {
        /* Without the commit the write may still be in NVS's cache when
         * the power goes, which is precisely when it matters. */
        err = nvs_commit(pthis->nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "writing '%s' failed (0x%x)", key, (unsigned)err);
    }
    return err;
}

static void nvs_close_if_open(NN20ClockStorage *pthis)
{
    if (pthis->nvs_open) {
        nvs_close(pthis->nvs);
        pthis->nvs_open = false;
    }
}

#else /* ------------------------------------------------- host build -- */

static esp_err_t nvs_load(NN20ClockStorage *pthis)
{
    (void)pthis;
    return ESP_OK;   /* nothing stored, nothing to load */
}

static esp_err_t nvs_store(NN20ClockStorage *pthis, const char *key,
                           const void *data, size_t size)
{
    (void)pthis;
    (void)key;
    (void)data;
    (void)size;
    return ESP_OK;   /* RAM only; the caller is told so by is_persistent() */
}

static void nvs_close_if_open(NN20ClockStorage *pthis)
{
    (void)pthis;
}

#endif /* ESP_PLATFORM */

/* ------------------------------------------------------- validation -- */

static esp_err_t validate_config(const NN20ClockConfig *config)
{
    if (config->brightness_day > 100u || config->brightness_night > 100u ||
        config->day_start_minutes >= NN20CLOCK_BRIGHTNESS_DAY_MINUTES ||
        config->night_start_minutes >= NN20CLOCK_BRIGHTNESS_DAY_MINUTES ||
        config->brightness_playback_min > 100u || config->volume > 100u) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Must be NUL-terminated inside the field, or every later strcpy of
     * it is a buffer overrun waiting to happen. */
    if (memchr(config->timezone, '\0', sizeof(config->timezone)) == NULL ||
        memchr(config->wifi_ssid, '\0', sizeof(config->wifi_ssid)) == NULL ||
        memchr(config->wifi_password, '\0',
               sizeof(config->wifi_password)) == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

/* Translate a worker post result into an esp_err_t. 1 is a full queue,
 * which is backpressure; -1 means the worker is going away. */
static esp_err_t post_result(int rc)
{
    if (rc == 0) {
        return ESP_OK;
    }
    return (rc == 1) ? ESP_ERR_TIMEOUT : ESP_ERR_INVALID_STATE;
}

/* --------------------------------------------------------- lifecycle -- */

NN20ClockStorage *nn20clock_storage_ctor(nn20_worker_ctx *worker)
{
    return nn20clock_storage_ctor_named(worker,
                                        NN20CLOCK_STORAGE_NAMESPACE);
}

NN20ClockStorage *nn20clock_storage_ctor_named(nn20_worker_ctx *worker,
                                               const char *name)
{
    if (name == NULL) {
        return NULL;
    }
    if (worker == NULL) {
        ESP_LOGE(TAG, "no StorageWorker (design 4)");
        return NULL;
    }

    NN20ClockStorage *pthis = calloc(1, sizeof(*pthis));
    if (pthis == NULL) {
        ESP_LOGE(TAG, "out of memory");
        return NULL;
    }

    pthis->worker = worker;
    pthis->nvs_namespace = name;
    atomic_init(&pthis->ready, false);
    nn20clock_reqpool_init(&pthis->request_pool, pthis->request_in_use,
                           STORAGE_MAX_PENDING);
    (void)nn20clock_storage_default_config(&pthis->config);
    return pthis;
}

/* Does nothing; it exists to be waited on. See the dtor. */
static int drain_private(nn20_worker_ctx *worker, void *user_data)
{
    (void)worker;
    (void)user_data;
    return 0;
}

void nn20clock_storage_dtor(NN20ClockStorage *pthis)
{
    if (pthis == NULL) {
        return;
    }

    /* An async save that is queued but has not run yet would dereference
     * this object after the free. Bounce an empty task through the queue
     * and wait for it: the worker is FIFO, so when that returns,
     * everything posted before it has already run.
     *
     * A save posted from another thread *during* the dtor still races.
     * That is not solvable here and does not need to be - the rule is
     * that a component is destroyed only once its callers are done with
     * it, which for the whole set is what NN20ClockApp's teardown order
     * guarantees. */
    const size_t in_flight = nn20clock_reqpool_in_flight(&pthis->request_pool);
    if (in_flight != 0) {
        ESP_LOGD(TAG, "draining %u save(s) still in flight",
                 (unsigned)in_flight);
        (void)nn20_worker_post_sync(pthis->worker, drain_private, NULL);
    }

    nvs_close_if_open(pthis);
    free(pthis);
}

static int start_private(nn20_worker_ctx *worker, void *user_data)
{
    (void)worker;
    NN20ClockStorage *pthis = user_data;

    const esp_err_t err = nvs_load(pthis);
    if (err != ESP_OK) {
        /* A clock that cannot reach its stored settings should still be
         * a clock: carry on with the defaults and say so. */
        ESP_LOGE(TAG, "storage unavailable (0x%x); running on defaults",
                 (unsigned)err);
    }

    atomic_store_explicit(&pthis->ready, true, memory_order_release);
    ESP_LOGI(TAG, "ready ('%s', schema v%u, %s, %u alarm(s))",
             pthis->nvs_namespace,
             (unsigned)pthis->config.schema_version,
             nn20clock_storage_is_persistent(pthis) ? "flash-backed"
                                                    : "in RAM only",
             (unsigned)pthis->alarms.count);
    return 0;
}

esp_err_t nn20clock_storage_start(NN20ClockStorage *pthis)
{
    if (pthis == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (nn20clock_storage_is_ready(pthis)) {
        return ESP_ERR_INVALID_STATE;
    }
    return post_result(
        nn20_worker_post_sync(pthis->worker, start_private, pthis));
}

bool nn20clock_storage_is_ready(const NN20ClockStorage *pthis)
{
    return pthis != NULL &&
           atomic_load_explicit(&pthis->ready, memory_order_acquire);
}

bool nn20clock_storage_is_persistent(const NN20ClockStorage *pthis)
{
#if defined(ESP_PLATFORM)
    return pthis != NULL && pthis->nvs_open;
#else
    /* No NVS off the target: the records are real, but they last only
     * as long as the process. */
    (void)pthis;
    return false;
#endif
}

/* ------------------------------------------------------------ config -- */

esp_err_t nn20clock_storage_default_config(NN20ClockConfig *out_config)
{
    if (out_config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out_config, 0, sizeof(*out_config));
    out_config->schema_version = NN20CLOCK_STORAGE_SCHEMA_VERSION;
    /* Fits by construction; the field is sized for a TZ string with DST
     * rules and DEFAULT_TIMEZONE is one. */
    snprintf(out_config->timezone, sizeof(out_config->timezone), "%s",
             DEFAULT_TIMEZONE);
    out_config->brightness_day = DEFAULT_BRIGHTNESS_DAY;
    out_config->brightness_night = DEFAULT_BRIGHTNESS_NIGHT;
    out_config->day_start_minutes = DEFAULT_DAY_START_MIN;
    out_config->night_start_minutes = DEFAULT_NIGHT_START_MIN;
    out_config->brightness_playback_min = DEFAULT_BRIGHTNESS_PLAYBACK_MIN;
    out_config->volume = DEFAULT_VOLUME;
    out_config->ntp_enabled = true;   /* the clock sets itself by default */
    out_config->last_known_time = 0;
    return ESP_OK;
}

typedef struct {
    NN20ClockStorage *storage;
    NN20ClockConfig *out_config;
} LoadRequest;

static int load_config_private(nn20_worker_ctx *worker, void *user_data)
{
    (void)worker;
    LoadRequest *request = user_data;
    /* Milestone 3 re-reads NVS here if the cache is dirty. */
    *request->out_config = request->storage->config;
    return 0;
}

esp_err_t nn20clock_storage_load_config(NN20ClockStorage *pthis,
                                        NN20ClockConfig *out_config)
{
    if (pthis == NULL || out_config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* On the stack, and the caller blocks until the callback is done, so
     * this needs no slot from the pool. */
    LoadRequest request = { .storage = pthis, .out_config = out_config };
    return post_result(
        nn20_worker_post_sync(pthis->worker, load_config_private, &request));
}

typedef struct {
    NN20ClockStorage *storage;
    const NN20ClockConfig *config;
    esp_err_t result;
} SyncSaveRequest;

static esp_err_t apply_config(NN20ClockStorage *pthis,
                              const NN20ClockConfig *config)
{
    NN20ClockConfig updated = *config;
    /* The caller does not get to pick the schema version: it describes
     * the layout this build writes, not anything the caller knows. */
    updated.schema_version = NN20CLOCK_STORAGE_SCHEMA_VERSION;

    const esp_err_t err = nvs_store(pthis, NVS_KEY_CONFIG, &updated,
                                    sizeof(updated));
    if (err != ESP_OK) {
        /* Leave the in-memory copy alone, so what callers read still
         * matches what is actually stored. */
        return err;
    }

    pthis->config = updated;
    return ESP_OK;
}

static int save_config_private(nn20_worker_ctx *worker, void *user_data)
{
    (void)worker;
    SyncSaveRequest *request = user_data;
    request->result = apply_config(request->storage, request->config);
    return (request->result == ESP_OK) ? 0 : -1;
}

esp_err_t nn20clock_storage_save_config(NN20ClockStorage *pthis,
                                        const NN20ClockConfig *config)
{
    if (pthis == NULL || config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const esp_err_t valid = validate_config(config);
    if (valid != ESP_OK) {
        ESP_LOGE(TAG, "rejected config: brightness %u/%u/%u, %u-%u min, "
                      "volume %u",
                 (unsigned)config->brightness_day,
                 (unsigned)config->brightness_night,
                 (unsigned)config->brightness_playback_min,
                 (unsigned)config->day_start_minutes,
                 (unsigned)config->night_start_minutes,
                 (unsigned)config->volume);
        return valid;
    }

    SyncSaveRequest request = {
        .storage = pthis,
        .config = config,
        .result = ESP_FAIL,
    };
    const esp_err_t posted = post_result(
        nn20_worker_post_sync(pthis->worker, save_config_private, &request));
    return (posted != ESP_OK) ? posted : request.result;
}

static int save_config_async_private(nn20_worker_ctx *worker, void *user_data)
{
    (void)worker;
    SaveRequest *request = user_data;
    NN20ClockStorage *pthis = request->storage;

    const esp_err_t result = apply_config(pthis, &request->config);

    /* Copy out what the continuation needs before the slot is released:
     * once it is free another thread may overwrite the request. */
    const NN20ClockStorageDoneFn on_done = request->on_done;
    void *const done_user = request->user_data;
    nn20clock_reqpool_release(&pthis->request_pool, request->slot);

    if (on_done != NULL) {
        on_done(result, done_user);
    }
    return (result == ESP_OK) ? 0 : -1;
}

esp_err_t nn20clock_storage_save_config_async(NN20ClockStorage *pthis,
                                              const NN20ClockConfig *config,
                                              NN20ClockStorageDoneFn on_done,
                                              void *user_data)
{
    if (pthis == NULL || config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const esp_err_t valid = validate_config(config);
    if (valid != ESP_OK) {
        return valid;
    }

    const size_t slot = nn20clock_reqpool_claim(&pthis->request_pool);
    if (slot == STORAGE_MAX_PENDING) {
        ESP_LOGW(TAG, "all %d save slots in flight", STORAGE_MAX_PENDING);
        return ESP_ERR_NO_MEM;
    }

    SaveRequest *request = &pthis->requests[slot];
    request->storage = pthis;
    request->config = *config;   /* copied: the caller's may not outlive us */
    request->on_done = on_done;
    request->user_data = user_data;
    request->slot = slot;

    const int rc = nn20_worker_post(pthis->worker, save_config_async_private,
                                    request);
    if (rc != 0) {
        nn20clock_reqpool_release(&pthis->request_pool, slot);
        return post_result(rc);
    }
    return ESP_OK;
}

/* ------------------------------------------------------------ alarms -- */

typedef struct {
    NN20ClockStorage *storage;
    NN20ClockAlarmList *out_list;        /* list */
    const NN20ClockAlarmConfig *alarm;   /* save */
    uint32_t alarm_id;                   /* save (out) / delete (in) */
    esp_err_t result;
} AlarmRequest;

static int list_alarms_private(nn20_worker_ctx *worker, void *user_data)
{
    (void)worker;
    AlarmRequest *request = user_data;
    *request->out_list = request->storage->alarms;
    request->result = ESP_OK;
    return 0;
}

esp_err_t nn20clock_storage_list_alarms(NN20ClockStorage *pthis,
                                        NN20ClockAlarmList *out_alarms)
{
    if (pthis == NULL || out_alarms == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    AlarmRequest request = {
        .storage = pthis,
        .out_list = out_alarms,
        .result = ESP_FAIL,
    };
    const esp_err_t posted = post_result(
        nn20_worker_post_sync(pthis->worker, list_alarms_private, &request));
    return (posted != ESP_OK) ? posted : request.result;
}

typedef struct {
    NN20ClockStorage *storage;
    uint32_t id;
    NN20ClockAlarmConfig *out_alarm;
    esp_err_t result;
} GetAlarmRequest;

static int get_alarm_private(nn20_worker_ctx *worker, void *user_data)
{
    (void)worker;
    GetAlarmRequest *request = user_data;

    size_t index = 0;
    request->result = nn20clock_alarm_list_find(&request->storage->alarms,
                                                request->id, &index);
    if (request->result != ESP_OK) {
        return -1;
    }

    *request->out_alarm = request->storage->alarms.alarms[index];
    return 0;
}

esp_err_t nn20clock_storage_get_alarm(NN20ClockStorage *pthis, uint32_t id,
                                      NN20ClockAlarmConfig *out_alarm)
{
    if (pthis == NULL || out_alarm == NULL ||
        id == NN20CLOCK_ALARM_ID_NONE) {
        return ESP_ERR_INVALID_ARG;
    }

    GetAlarmRequest request = {
        .storage = pthis,
        .id = id,
        .out_alarm = out_alarm,
        .result = ESP_FAIL,
    };
    const esp_err_t posted = post_result(
        nn20_worker_post_sync(pthis->worker, get_alarm_private, &request));
    return (posted != ESP_OK) ? posted : request.result;
}

/*
 * Runs on the worker. The list is edited on a copy and only adopted once
 * the flash write succeeds, so a failed write leaves the schedule as it
 * was rather than half-changed.
 *
 * The copy is on the heap, not the stack: an NN20ClockAlarmList is over
 * 5 KB and the StorageWorker's stack is 6 KB. A stack copy overflowed it
 * on the board - the host build has megabytes of stack and never
 * noticed.
 */
static int save_alarm_private(nn20_worker_ctx *worker, void *user_data)
{
    (void)worker;
    AlarmRequest *request = user_data;
    NN20ClockStorage *pthis = request->storage;

    NN20ClockAlarmList *updated = malloc(sizeof(*updated));
    if (updated == NULL) {
        request->result = ESP_ERR_NO_MEM;
        return -1;
    }
    *updated = pthis->alarms;
    uint32_t assigned = NN20CLOCK_ALARM_ID_NONE;

    /* Validates on the way in, so an unfireable alarm never reaches
     * flash. */
    request->result = nn20clock_alarm_list_put(updated, request->alarm,
                                               &assigned);
    if (request->result == ESP_OK) {
        request->result = nvs_store(pthis, NVS_KEY_ALARMS, updated,
                                    sizeof(*updated));
    }
    if (request->result != ESP_OK) {
        free(updated);
        return -1;
    }

    pthis->alarms = *updated;
    free(updated);
    request->alarm_id = assigned;
    ESP_LOGI(TAG, "alarm %u saved (%u total)", (unsigned)assigned,
             (unsigned)pthis->alarms.count);
    return 0;
}

esp_err_t nn20clock_storage_save_alarm(NN20ClockStorage *pthis,
                                       const NN20ClockAlarmConfig *alarm,
                                       uint32_t *out_id)
{
    if (pthis == NULL || alarm == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    AlarmRequest request = {
        .storage = pthis,
        .alarm = alarm,
        .result = ESP_FAIL,
    };
    const esp_err_t posted = post_result(
        nn20_worker_post_sync(pthis->worker, save_alarm_private, &request));
    if (posted != ESP_OK) {
        return posted;
    }
    if (request.result == ESP_OK && out_id != NULL) {
        *out_id = request.alarm_id;
    }
    return request.result;
}

static int delete_alarm_private(nn20_worker_ctx *worker, void *user_data)
{
    (void)worker;
    AlarmRequest *request = user_data;
    NN20ClockStorage *pthis = request->storage;

    /* Heap, not stack: see save_alarm_private above. */
    NN20ClockAlarmList *updated = malloc(sizeof(*updated));
    if (updated == NULL) {
        request->result = ESP_ERR_NO_MEM;
        return -1;
    }
    *updated = pthis->alarms;

    request->result = nn20clock_alarm_list_remove(updated,
                                                  request->alarm_id);
    if (request->result == ESP_OK) {
        request->result = nvs_store(pthis, NVS_KEY_ALARMS, updated,
                                    sizeof(*updated));
    }
    if (request->result != ESP_OK) {
        free(updated);
        return -1;
    }

    pthis->alarms = *updated;
    free(updated);
    ESP_LOGI(TAG, "alarm %u deleted (%u left)", (unsigned)request->alarm_id,
             (unsigned)pthis->alarms.count);
    return 0;
}

esp_err_t nn20clock_storage_delete_alarm(NN20ClockStorage *pthis,
                                         uint32_t alarm_id)
{
    if (pthis == NULL || alarm_id == NN20CLOCK_ALARM_ID_NONE) {
        return ESP_ERR_INVALID_ARG;
    }

    AlarmRequest request = {
        .storage = pthis,
        .alarm_id = alarm_id,
        .result = ESP_FAIL,
    };
    const esp_err_t posted = post_result(
        nn20_worker_post_sync(pthis->worker, delete_alarm_private, &request));
    return (posted != ESP_OK) ? posted : request.result;
}

/* ------------------------------------------------- not implemented -- */

esp_err_t nn20clock_storage_list_media(NN20ClockStorage *pthis, void *out_media)
{
    if (pthis == NULL || out_media == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_ERR_NOT_SUPPORTED;   /* SD media, design 13 - Milestone 6 */
}

esp_err_t nn20clock_storage_list_media_sets(NN20ClockStorage *pthis,
                                            void *out_sets)
{
    if (pthis == NULL || out_sets == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_ERR_NOT_SUPPORTED;
}
