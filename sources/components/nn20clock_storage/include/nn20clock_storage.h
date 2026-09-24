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
 * nn20clock_storage.h - the Storage facade (design 9).
 *
 * The config record and the alarm list (design 10) are persisted in
 * flash through NVS on the target, so they survive a reboot. On the
 * host build there is no NVS and the same records live in RAM only -
 * which is why nn20clock_storage_is_persistent() exists rather than
 * being assumed.
 *
 * SD-backed media (design 13) is still not implemented; those calls
 * report ESP_ERR_NOT_SUPPORTED - Milestone 6.
 *
 * Threading (design 4): Storage runs on the StorageWorker, which is
 * created by NN20ClockWorkers and passed to the constructor. Flash, SD,
 * and filesystem calls block, which is exactly why they are not on the
 * UI worker.
 *
 * There are no mutexes in this component, and there should never be
 * one. Every byte of Storage's state is read and written only from
 * callbacks running on its own worker thread; the worker queue is the
 * synchronization. Public functions here are safe to call from any
 * thread because they do not touch that state - they package a request
 * and hand it to the worker.
 */
#ifndef NN20CLOCK_STORAGE_H
#define NN20CLOCK_STORAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "nn20clock_alarm.h"
#include "nn20clock_platform.h"
#include "worker.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NN20ClockStorage NN20ClockStorage;

/* Bumped whenever the persisted layout changes; design 9 lists a schema
 * version among the flash-backed settings, and design 17 makes storage
 * migration a Milestone 12 concern. */
/*
 * Bumped to 2 when Wi-Fi credentials joined the config record
 * (Milestone 5). A stored record with a different version is discarded
 * and the defaults are used - so a device upgrading across this bump
 * forgets its settings once, which is the documented behaviour until
 * design 17's migration work.
 */
/*
 * Bumped to 3 when brightness became a day/night schedule, and to 4
 * when the NTP switch and the last-known time were added. The config
 * blob carries this field and load() checks it, so an older record is
 * discarded and the defaults are used - which is the whole reason this
 * struct does not need the new-NVS-key dance the alarm blob did.
 *
 * The cost of that simplicity is real and worth stating: every bump
 * resets brightness, volume, timezone and the stored Wi-Fi network on
 * the next boot. Alarms are a separate record and survive. If settings
 * start mattering more than the simplicity does, the answer is a
 * migration that reads the previous layout rather than a cleverer
 * check here.
 */
/*
 * Bumped to 5 for the playback brightness minimum - and the first bump
 * that does not cost the settings. The new field goes after everything
 * version 4 had, so a version-4 record is a version-5 record with that
 * one field missing, and load() takes it as such and fills in the
 * default. Anything added later should do the same: append, bump, and
 * accept the previous version by the offset of the new field.
 */
#define NN20CLOCK_STORAGE_SCHEMA_VERSION 5u

/* Long enough for a POSIX TZ string with both DST rules, e.g.
 * "CET-1CEST,M3.5.0,M10.5.0/3". */
#define NN20CLOCK_TIMEZONE_MAX 64

/* 802.11 caps an SSID at 32 bytes and a WPA2 passphrase at 63, both
 * without a terminator. These include one. */
#define NN20CLOCK_WIFI_SSID_MAX 33
#define NN20CLOCK_WIFI_PASSWORD_MAX 64

/*
 * The small, critical settings design 9 puts in flash. Alarms are a
 * separate record (design 10) and are not part of this struct.
 */
typedef struct {
    uint32_t schema_version;
    char timezone[NN20CLOCK_TIMEZONE_MAX];  /* POSIX TZ, design 14 */

    /*
     * Brightness is a schedule, not a number: a clock readable across a
     * sunlit room is a lamp at three in the morning. Two levels and the
     * two local times they change at - see nn20clock_brightness.h,
     * which owns what these mean and every edge in reading them.
     *
     * Times are minutes since local midnight. Equal times mean a
     * schedule with no night in it, which is how the settings screen
     * says "never dim" without a separate switch.
     */
    uint8_t brightness_day;                 /* 0-100 percent */
    uint8_t brightness_night;               /* 0-100 percent */
    uint16_t day_start_minutes;             /* 0-1439, local */
    uint16_t night_start_minutes;           /* 0-1439, local */

    uint8_t volume;                         /* 0-100 percent */

    /*
     * Whether the clock sets itself from the network.
     *
     * On by default: a device that has never been configured should get
     * the right time by itself. Turned off by somebody with no internet
     * who has typed the time in, and it must stay off - otherwise SNTP
     * would quietly overwrite what they set the moment a network
     * appeared.
     */
    bool ntp_enabled;

    /*
     * The wall clock as it was last known to be right, whether that came
     * from NTP or from somebody typing it in. Zero when it never has
     * been.
     *
     * This is what a clock with no network comes back to after a power
     * cut. It will be wrong by however long the power was off - a
     * plausible wrong time that can then be corrected by hand, rather
     * than 1970.
     */
    time_t last_known_time;

    /*
     * Wi-Fi, set from DeviceSettingsUi (Milestone 5). An empty SSID
     * means "not configured", in which case the build-time Kconfig
     * value is used instead - which is how a device that has never
     * visited the settings screen keeps working.
     *
     * NVS IS NOT ENCRYPTED. The passphrase is readable by anyone with
     * physical access and a serial cable. That is the same exposure as
     * ESP-IDF's own Wi-Fi driver, which stores credentials in NVS too,
     * and it is a deliberate choice - flash encryption burns eFuses and
     * is irreversible. Nothing in this project ever logs this field.
     */
    char wifi_ssid[NN20CLOCK_WIFI_SSID_MAX];
    char wifi_password[NN20CLOCK_WIFI_PASSWORD_MAX];

    /*
     * The least a film is shown at, 0-100 percent; 0 means no minimum.
     * Part of the brightness schedule above - see nn20clock_brightness.h
     * for what it means - but last in the struct rather than beside the
     * others, because that is what lets a version-4 record be read
     * without losing the rest (see NN20CLOCK_STORAGE_SCHEMA_VERSION).
     */
    uint8_t brightness_playback_min;
} NN20ClockConfig;

/*
 * Continuation for the async calls. Runs on the StorageWorker thread,
 * not the caller's: keep it short, and post elsewhere rather than doing
 * real work in it.
 */
typedef void (*NN20ClockStorageDoneFn)(esp_err_t status, void *user_data);

/* --------------------------------------------------------- lifecycle -- */

/*
 * worker is borrowed and must outlive the Storage. NULL is rejected:
 * design 4 requires the StorageWorker to be passed in, and a Storage
 * without one has nowhere to run.
 */
NN20ClockStorage *nn20clock_storage_ctor(nn20_worker_ctx *worker);

/* The NVS namespace the product uses. */
#define NN20CLOCK_STORAGE_NAMESPACE "nn20clock"

/*
 * As above, but in a named NVS namespace.
 *
 * This exists for the on-target tests. They exercise real flash, which
 * is the point of running them on the board - but writing to the
 * product's namespace means a test run reconfigures the actual device.
 * That is not hypothetical: a test that stored a fake network left the
 * clock trying to join it, with no Wi-Fi and no NTP, until someone
 * noticed.
 *
 * `name` must be a string literal or otherwise outlive the Storage; it
 * is not copied. NVS caps namespace names at 15 characters.
 */
NN20ClockStorage *nn20clock_storage_ctor_named(nn20_worker_ctx *worker,
                                               const char *name);

/*
 * Safe with NULL. Does not stop the worker - Storage borrows it, and
 * NN20ClockWorkers owns it.
 */
void nn20clock_storage_dtor(NN20ClockStorage *pthis);

/*
 * Bring the backing stores up. Today this only seeds the defaults, so it
 * always succeeds; from Milestone 3 it opens NVS and reports a real
 * failure. Runs on the StorageWorker and waits for it.
 */
esp_err_t nn20clock_storage_start(NN20ClockStorage *pthis);

/* True once _start() has completed. */
bool nn20clock_storage_is_ready(const NN20ClockStorage *pthis);

/*
 * True on the target, where records go to flash; false on the host,
 * where they live in RAM for the life of the process. Callers that care
 * whether a save survives a power cycle should ask rather than assume.
 */
bool nn20clock_storage_is_persistent(const NN20ClockStorage *pthis);

/* ------------------------------------------------------------ config -- */

/* The compiled-in defaults, before anything is loaded. */
esp_err_t nn20clock_storage_default_config(NN20ClockConfig *out_config);

/*
 * Copy the current config out. Runs on the StorageWorker and blocks
 * until it has, so out_config is a snapshot nothing else is writing.
 *
 * ESP_ERR_INVALID_STATE if the worker is stopping, ESP_ERR_TIMEOUT if
 * its queue is full.
 */
esp_err_t nn20clock_storage_load_config(NN20ClockStorage *pthis,
                                        NN20ClockConfig *out_config);

/*
 * Validate and store a config. Blocking, as above.
 * ESP_ERR_INVALID_ARG if a field is out of range; the stored config is
 * left untouched in that case.
 */
esp_err_t nn20clock_storage_save_config(NN20ClockStorage *pthis,
                                        const NN20ClockConfig *config);

/*
 * The same save without blocking the caller: the config is copied into
 * the request, so it need not outlive the call. on_done may be NULL.
 *
 * This is the form runtime code should use. The blocking variants exist
 * for boot, tests, and shutdown - design 4 puts storage on its own
 * worker precisely so the rest of the system does not wait on it.
 *
 * ESP_ERR_NO_MEM when every request slot is in flight; that is
 * backpressure, not a fault.
 */
esp_err_t nn20clock_storage_save_config_async(NN20ClockStorage *pthis,
                                              const NN20ClockConfig *config,
                                              NN20ClockStorageDoneFn on_done,
                                              void *user_data);

/* ------------------------------------------------------------ alarms -- */

/*
 * The stored alarms (design 10). All three run on the StorageWorker and
 * block until it has finished, because a caller editing an alarm needs
 * to know the change is in flash before it tells the Timer to reload.
 *
 * A write that fails leaves the stored list untouched: the in-memory
 * copy is only replaced once the flash write has succeeded, so a power
 * loss mid-save costs the change, not the schedule.
 */
/*
 * NOTE ON SIZE. NN20ClockAlarmList is a fixed array of 16 records and is
 * over 5 KB - larger than the stack of any worker in this project. It
 * must never be declared as a local. Callers that want the whole list
 * pass a heap or static buffer; callers that want one alarm use
 * nn20clock_storage_get_alarm() below.
 */
esp_err_t nn20clock_storage_list_alarms(NN20ClockStorage *pthis,
                                        NN20ClockAlarmList *out_alarms);

/*
 * One alarm by id. ESP_ERR_NOT_FOUND if it is not stored.
 *
 * Prefer this to listing when only one record is wanted: an
 * NN20ClockAlarmList is over 5 KB, which is more than a worker thread's
 * whole stack, while one NN20ClockAlarmConfig is a few hundred bytes.
 */
esp_err_t nn20clock_storage_get_alarm(NN20ClockStorage *pthis, uint32_t id,
                                      NN20ClockAlarmConfig *out_alarm);

/*
 * Add or replace one alarm. An id of NN20CLOCK_ALARM_ID_NONE means "new
 * alarm"; the assigned id comes back in out_id, which may be NULL.
 *
 * The alarm is validated first, so an unfireable record - a recurrent
 * alarm with no weekdays, an hour of 25 - never reaches flash.
 * ESP_ERR_NO_MEM when the list is full.
 */
esp_err_t nn20clock_storage_save_alarm(NN20ClockStorage *pthis,
                                       const NN20ClockAlarmConfig *alarm,
                                       uint32_t *out_id);

/*
 * Remove one alarm. ESP_ERR_NOT_FOUND if the id is not stored.
 *
 * This is also how design 10's one-off lifecycle ends: once a one-off
 * has fired and been dismissed, the ClockManager deletes it.
 */
esp_err_t nn20clock_storage_delete_alarm(NN20ClockStorage *pthis,
                                         uint32_t alarm_id);

/* ------------------------------------------- not implemented yet -------
 *
 * Design 9's SD-backed media, present so callers can be written against
 * the real shape. Both return ESP_ERR_NOT_SUPPORTED - Milestone 6.
 */
esp_err_t nn20clock_storage_list_media(NN20ClockStorage *pthis,
                                       void *out_media);
esp_err_t nn20clock_storage_list_media_sets(NN20ClockStorage *pthis,
                                            void *out_sets);

#ifdef __cplusplus
}
#endif

#endif /* NN20CLOCK_STORAGE_H */
