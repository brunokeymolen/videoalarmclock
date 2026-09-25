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
 * nn20clock_timer.c - the Timer (design 6, 7).
 *
 * The tick is a poll, not a counter. Every poll samples the clock and
 * compares it with the previous reading; a changed second emits a SECOND
 * event and a changed minute emits a MINUTE event. Nothing accumulates,
 * so nothing drifts, and an NTP sync that moves the clock by hours is
 * just another reading rather than a special case to unwind.
 *
 * Everything below the public functions runs on the Timer's worker. The
 * subscriber list, the last reading, and the synced flag are owned by
 * that thread; no mutex appears in this file, and one would mean the
 * ownership had been broken.
 */
#include "nn20clock_timer.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "worker.h"

#if defined(ESP_PLATFORM)
#include "esp_timer.h"
#endif

static const char *TAG = "NN20CLOCK_TIMER";

/*
 * How often the clock is sampled on the target. Five times a second: a
 * minute change is noticed within 200 ms, which is well under what
 * anyone perceives as the minute "turning over", and it costs five
 * comparisons a second.
 */
#define TICK_PERIOD_US 200000

/*
 * What the previous poll saw. Both are instants, not clock fields:
 * comparing fields would miss a jump of exactly one minute or one hour,
 * which is precisely what an NTP correction tends to look like.
 *
 * minute_stamp is the instant the current local minute began - the
 * reading with its seconds removed. Two readings share a minute exactly
 * when their stamps match, which stays true across an hour change, a
 * date change, and a DST shift, none of which comparing tm_min
 * survives.
 */
typedef struct {
    bool valid;
    time_t when;
    time_t minute_stamp;
} Reading;

struct NN20ClockTimer {
    NN20ClockStorage *storage;   /* borrowed; may be NULL */
    nn20_worker_ctx *worker;     /* owned */

    /* ---- worker-owned. ---------------------------------------------- */
    NN20ClockTimerSubscriber *subscribers[NN20CLOCK_TIMER_MAX_SUBSCRIBERS];
    size_t subscriber_count;

    NN20ClockTimerClockFn clock_fn;
    void *clock_user_data;

    Reading last;         /* .valid is false before the first poll */
    bool force_emit;      /* set by a time sync: emit even if unchanged */

    NN20ClockAlarmList alarms;
    NN20ClockTimerAlarmSourceFn alarm_source;
    void *alarm_source_user_data;

    /*
     * The pending snooze: an occurrence that exists only here.
     *
     * Not a stored alarm, deliberately - design 10 requires a reboot to
     * forget it. Not in last_fired[] either: that records occurrences
     * already fired, and this one has not happened yet.
     */
    uint32_t snooze_alarm;    /* NN20CLOCK_ALARM_ID_NONE when none */
    time_t snooze_at;

    /*
     * The occurrence each alarm last fired at, by alarm id. The
     * half-open interval already guarantees an instant is scanned once,
     * so this is the second line of defence: it survives a schedule
     * reload and a stop/start, neither of which the interval knows
     * about.
     *
     * RAM only, deliberately. A reboot forgets it, which within the
     * grace window could re-ring an alarm - accepted rather than
     * writing to flash on every firing. One-off alarms are still
     * removed from storage once dismissed (design 10), which is the
     * durable half of "do not fire twice".
     */
    time_t last_fired[NN20CLOCK_ALARM_MAX + 1];

    /* ---- read from other threads. ----------------------------------- */
    atomic_bool running;
    atomic_bool time_synced;
    atomic_uint missed_alarms;

#if defined(ESP_PLATFORM)
    esp_timer_handle_t tick_timer;   /* owned; NULL until _start() */
#endif
};

/* --------------------------------------------------------- plumbing -- */

static esp_err_t post_result(int rc)
{
    if (rc == 0) {
        return ESP_OK;
    }
    return (rc == 1) ? ESP_ERR_TIMEOUT : ESP_ERR_INVALID_STATE;
}

static time_t system_clock(void *user_data)
{
    (void)user_data;
    return time(NULL);
}

/* Empty; posted only to be waited on, which flushes everything queued
 * ahead of it. */
static int drain_private(nn20_worker_ctx *worker, void *user_data)
{
    (void)worker;
    (void)user_data;
    return 0;
}

/* ------------------------------------------------------------- time -- */

esp_err_t nn20clock_timer_to_local(time_t when, NN20ClockDateTime *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    struct tm local = {0};
    if (localtime_r(&when, &local) == NULL) {
        memset(out, 0, sizeof(*out));
        return ESP_FAIL;
    }

    out->year   = (uint16_t)(local.tm_year + 1900);
    out->month  = (uint8_t)(local.tm_mon + 1);
    out->day    = (uint8_t)local.tm_mday;
    out->hour   = (uint8_t)local.tm_hour;
    out->minute = (uint8_t)local.tm_min;
    out->second = (uint8_t)local.tm_sec;
    /* struct tm counts from Sunday; design 10's weekdays_mask counts
     * from Monday, so that a weekday indexes the mask directly. */
    out->weekday = (uint8_t)((local.tm_wday + 6) % 7);
    return ESP_OK;
}

/* --------------------------------------------------------- dispatch -- */

/* Runs on the worker. Every subscriber sees every event and filters by
 * type itself (design 6). */
static void emit(NN20ClockTimer *pthis, NN20ClockTimerEventType type,
                 time_t when)
{
    const NN20ClockTimerPayload payload = {
        .displayed_time = when,
        .time_synced = nn20clock_timer_is_time_synced(pthis),
        /* Read straight off the worker's own state - this runs on the
         * thread that owns it. */
        .snooze_pending = (pthis->snooze_alarm != NN20CLOCK_ALARM_ID_NONE),
    };
    const NN20ClockTimerEvent event = { .type = type, .payload = &payload };

    for (size_t i = 0; i < pthis->subscriber_count; i++) {
        NN20ClockTimerSubscriber *subscriber = pthis->subscribers[i];
        if (subscriber != NULL && subscriber->on_event != NULL) {
            subscriber->on_event(&event, subscriber->user_data);
        }
    }
}

/* Runs on the worker. Emits an alarm event with design 6's derived
 * payload. */
static void emit_alarm(NN20ClockTimer *pthis, uint32_t alarm_id,
                       time_t scheduled, uint32_t late_seconds)
{
    const NN20ClockTimerAlarmPayload payload = {
        .super = {
            .displayed_time = scheduled,
            .time_synced = nn20clock_timer_is_time_synced(pthis),
            .snooze_pending =
                (pthis->snooze_alarm != NN20CLOCK_ALARM_ID_NONE),
        },
        .alarm_id = alarm_id,
        .late_seconds = late_seconds,
    };
    const NN20ClockTimerEvent event = {
        .type = NN20CLOCK_TIMER_EVENT_ALARM,
        /* The derived payload upcasts to the base, which is what
         * subscribers that do not care about alarms will see. */
        .payload = &payload.super,
    };

    for (size_t i = 0; i < pthis->subscriber_count; i++) {
        NN20ClockTimerSubscriber *subscriber = pthis->subscribers[i];
        if (subscriber != NULL && subscriber->on_event != NULL) {
            subscriber->on_event(&event, subscriber->user_data);
        }
    }
}

/*
 * Runs on the worker. The heart of design 10's scheduling: which alarms
 * occurred in the span since the previous reading, and is any of them
 * still worth ringing.
 *
 * Only the earliest occurrence is fired per tick. Two alarms going off
 * in the same second is a tie one speaker cannot resolve anyway, and
 * firing a queue of them after a long gap would be worse than useless.
 */
static void scan_alarms(NN20ClockTimer *pthis, time_t now)
{
    if (!pthis->last.valid) {
        /* No previous reading, so no interval - and nothing to scan.
         * This is also the path taken right after a clock correction,
         * which is what stops a sync from firing everything it skipped
         * over. */
        return;
    }

    uint32_t alarm_id = NN20CLOCK_ALARM_ID_NONE;
    time_t scheduled = (time_t)0;
    const bool scheduled_alarm =
        nn20clock_alarm_list_first_in(&pthis->alarms, pthis->last.when, now,
                                      &alarm_id, &scheduled);

    /*
     * A pending snooze is checked against the same half-open interval,
     * so it cannot be missed by a late tick any more than a real alarm
     * can.
     */
    const bool snooze_due =
        (pthis->snooze_alarm != NN20CLOCK_ALARM_ID_NONE) &&
        (pthis->snooze_at > pthis->last.when) && (pthis->snooze_at <= now);

    if (snooze_due && (!scheduled_alarm || pthis->snooze_at < scheduled)) {
        /* The snooze is the earlier of the two - or the only one. */
        alarm_id = pthis->snooze_alarm;
        scheduled = pthis->snooze_at;

        /* Consumed either way: it fires now, or it was too late and is
         * reported missed below. A snooze does not get a second try. */
        pthis->snooze_alarm = NN20CLOCK_ALARM_ID_NONE;
        pthis->snooze_at = (time_t)0;

        const time_t late = now - scheduled;
        if (late > (time_t)NN20CLOCK_TIMER_ALARM_GRACE_SECONDS) {
            atomic_fetch_add_explicit(&pthis->missed_alarms, 1u,
                                      memory_order_relaxed);
            ESP_LOGW(TAG, "snoozed alarm %u missed: %lld s late",
                     (unsigned)alarm_id, (long long)late);
            return;
        }

        ESP_LOGI(TAG, "snoozed alarm %u fires again", (unsigned)alarm_id);
        emit_alarm(pthis, alarm_id, scheduled, (uint32_t)late);
        return;
    }

    if (!scheduled_alarm) {
        return;
    }

    if (alarm_id > NN20CLOCK_ALARM_MAX) {
        ESP_LOGE(TAG, "alarm id %u out of range", (unsigned)alarm_id);
        return;
    }

    /* Already fired this exact occurrence: a reload or a restart brought
     * the interval back over ground it had covered. */
    if (pthis->last_fired[alarm_id] == scheduled) {
        return;
    }

    const time_t late = now - scheduled;
    if (late > (time_t)NN20CLOCK_TIMER_ALARM_GRACE_SECONDS) {
        /*
         * Too late to ring. The device was off, or the clock was
         * corrected across the alarm - and waking someone at the wrong
         * time of day is worse than not waking them. Recorded as fired
         * so it is not reconsidered on the next tick.
         */
        atomic_fetch_add_explicit(&pthis->missed_alarms, 1u,
                                  memory_order_relaxed);
        pthis->last_fired[alarm_id] = scheduled;
        ESP_LOGW(TAG, "alarm %u missed: %lld s late (grace is %d s)",
                 (unsigned)alarm_id, (long long)late,
                 NN20CLOCK_TIMER_ALARM_GRACE_SECONDS);
        return;
    }

    pthis->last_fired[alarm_id] = scheduled;
    ESP_LOGI(TAG, "alarm %u fires (%lld s late)", (unsigned)alarm_id,
             (long long)late);
    emit_alarm(pthis, alarm_id, scheduled, (uint32_t)late);
}

/* Runs on the worker. This is the whole tick. */
static int poll_private(nn20_worker_ctx *worker, void *user_data)
{
    (void)worker;
    NN20ClockTimer *pthis = user_data;

    if (!atomic_load_explicit(&pthis->running, memory_order_acquire)) {
        return 0;   /* stopped between the post and now */
    }

    const time_t now = pthis->clock_fn(pthis->clock_user_data);

    NN20ClockDateTime local;
    if (nn20clock_timer_to_local(now, &local) != ESP_OK) {
        ESP_LOGE(TAG, "cannot convert %lld to local time", (long long)now);
        emit(pthis, NN20CLOCK_TIMER_EVENT_ERROR, now);
        return -1;
    }

    const time_t minute_stamp = now - (time_t)local.second;

    const bool first = !pthis->last.valid;
    const bool forced = pthis->force_emit;
    const bool second_changed = first || forced || now != pthis->last.when;
    const bool minute_changed = first || forced ||
                                minute_stamp != pthis->last.minute_stamp;

    /* Before `last` moves: the scan needs the interval that just
     * elapsed, which is exactly (last.when, now]. */
    scan_alarms(pthis, now);

    pthis->last.valid = true;
    pthis->last.when = now;
    pthis->last.minute_stamp = minute_stamp;
    pthis->force_emit = false;

    /* SECOND first: a subscriber that wants both sees the finer event
     * before the coarser one, and the minute event is the one that
     * usually triggers a redraw. */
    if (second_changed) {
        emit(pthis, NN20CLOCK_TIMER_EVENT_SECOND, now);
    }
    if (minute_changed) {
        emit(pthis, NN20CLOCK_TIMER_EVENT_MINUTE, now);
    }
    return 0;
}

esp_err_t nn20clock_timer_poll(NN20ClockTimer *pthis)
{
    if (pthis == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return post_result(nn20_worker_post(pthis->worker, poll_private, pthis));
}

/* Runs on the worker: force_emit is worker-owned state. */
static int refresh_private(nn20_worker_ctx *worker, void *user_data)
{
    (void)worker;
    NN20ClockTimer *pthis = user_data;
    pthis->force_emit = true;
    return 0;
}

esp_err_t nn20clock_timer_refresh(NN20ClockTimer *pthis)
{
    if (pthis == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Asynchronous: the caller wants the next tick to report, not to
     * wait for one. */
    return post_result(
        nn20_worker_post(pthis->worker, refresh_private, pthis));
}

esp_err_t nn20clock_timer_flush(NN20ClockTimer *pthis)
{
    if (pthis == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return post_result(
        nn20_worker_post_sync(pthis->worker, drain_private, NULL));
}

/*
 * Runs on the worker. The clock has just been given a time from a
 * source worth believing - SNTP, or the user at the settings screen.
 * Both mean the same three things to the schedule, which is why they
 * share this rather than each doing two of them.
 */
static void adopt_authoritative_time(NN20ClockTimer *pthis, const char *how)
{
    atomic_store_explicit(&pthis->time_synced, true, memory_order_release);
    /* A correction can move the clock by hours; the next poll must
     * report the new time even if the second happens to match the old
     * reading. */
    pthis->force_emit = true;

    /*
     * And it must NOT treat the correction as elapsed time. The span
     * between the old reading and the new one contains no real
     * occurrences - the clock was simply wrong - so dropping the
     * baseline makes the next tick start afresh instead of firing
     * every alarm it appears to have jumped over.
     *
     * Dropping it also stops the next tick being *consumed* by that
     * span. Only the earliest occurrence in the interval is considered
     * per tick, so a stale baseline would spend the tick marking some
     * occurrence from before the correction missed - and an alarm set
     * for a minute later would find its own tick already spent.
     */
    pthis->last.valid = false;

    const time_t now = pthis->clock_fn(pthis->clock_user_data);
    ESP_LOGI(TAG, "%s", how);
    emit(pthis, NN20CLOCK_TIMER_EVENT_TIME_SYNCED, now);
}

static int time_synced_private(nn20_worker_ctx *worker, void *user_data)
{
    (void)worker;
    adopt_authoritative_time(user_data, "time synced");
    return 0;
}

static int time_set_private(nn20_worker_ctx *worker, void *user_data)
{
    (void)worker;
    adopt_authoritative_time(user_data, "time set by hand");
    return 0;
}

esp_err_t nn20clock_timer_notify_time_synced(NN20ClockTimer *pthis)
{
    if (pthis == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return post_result(
        nn20_worker_post(pthis->worker, time_synced_private, pthis));
}

esp_err_t nn20clock_timer_notify_time_set(NN20ClockTimer *pthis)
{
    if (pthis == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return post_result(
        nn20_worker_post(pthis->worker, time_set_private, pthis));
}

bool nn20clock_timer_is_time_synced(const NN20ClockTimer *pthis)
{
    return pthis != NULL &&
           atomic_load_explicit(&pthis->time_synced, memory_order_acquire);
}

/* --------------------------------------------------------- lifecycle -- */

NN20ClockTimer *nn20clock_timer_ctor(NN20ClockStorage *storage)
{
    NN20ClockTimer *pthis = calloc(1, sizeof(*pthis));
    if (pthis == NULL) {
        ESP_LOGE(TAG, "out of memory");
        return NULL;
    }

    pthis->storage = storage;
    pthis->clock_fn = system_clock;
    pthis->snooze_alarm = NN20CLOCK_ALARM_ID_NONE;
    atomic_init(&pthis->running, false);
    atomic_init(&pthis->time_synced, false);
    atomic_init(&pthis->missed_alarms, 0u);

    const nn20_worker_config config = {
        .struct_size = sizeof(config),
        .name = "nn20clock-timer",
        .queue_capacity = NN20_WORKER_DEFAULT_QUEUE,
        .stack_bytes = NN20_WORKER_DEFAULT_STACK,
        .priority = NN20_WORKER_DEFAULT_PRIORITY,
        /* Unpinned. The tick is a handful of comparisons; what matters
         * is that it is not behind the UI queue, which owning a separate
         * worker already guarantees. Revisit at Milestone 7, when video
         * decode starts competing for cores. */
        .core_id = NN20_WORKER_CORE_ANY,
    };

    /* Created here rather than in _start() so a construction failure is
     * reported as a NULL return, the one error path a ctor has. */
    pthis->worker = nn20_worker_create_with(&config);
    if (pthis->worker == NULL) {
        ESP_LOGE(TAG, "worker thread would not start");
        free(pthis);
        return NULL;
    }

    return pthis;
}

void nn20clock_timer_dtor(NN20ClockTimer *pthis)
{
    if (pthis == NULL) {
        return;
    }

    if (nn20clock_timer_is_running(pthis)) {
        (void)nn20clock_timer_stop(pthis);
    }

    /* Deletes without draining and joins the thread. Safe whether or not
     * _stop() already ran. */
    nn20_worker_delete(pthis->worker);
    free(pthis);
}

esp_err_t nn20clock_timer_set_clock_fn(NN20ClockTimer *pthis,
                                       NN20ClockTimerClockFn clock_fn,
                                       void *user_data)
{
    if (pthis == NULL || clock_fn == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (nn20clock_timer_is_running(pthis)) {
        /* Would be a write to worker-owned state from another thread. */
        return ESP_ERR_INVALID_STATE;
    }

    pthis->clock_fn = clock_fn;
    pthis->clock_user_data = user_data;
    return ESP_OK;
}

/* Process-wide, not per-Timer: localtime_r reads the process timezone,
 * and design 14 wants one timezone applied consistently everywhere. */
static esp_err_t apply_timezone(const char *tz)
{
    if (setenv("TZ", tz, 1) != 0) {
        ESP_LOGE(TAG, "cannot set TZ to '%s'", tz);
        return ESP_FAIL;
    }
    tzset();
    ESP_LOGI(TAG, "timezone %s", tz);
    return ESP_OK;
}

typedef struct {
    NN20ClockTimer *timer;
    const char *tz;
    esp_err_t result;
} TimezoneRequest;

/*
 * Runs on the worker, so the Timer's own conversions - the ones that
 * decide when an alarm rings - never see the zone half changed.
 *
 * A new zone moves the local clock by hours without the instant moving
 * at all, so it is treated like a correction: the face is told at once,
 * because the minute stamp need not change and nothing else would, and
 * the baseline is dropped so the jump is not read as elapsed time. It
 * does not make the clock synced - the instant is exactly as right or
 * as wrong as it was.
 */
static int timezone_private(nn20_worker_ctx *worker, void *user_data)
{
    (void)worker;
    TimezoneRequest *request = user_data;
    NN20ClockTimer *pthis = request->timer;

    request->result = apply_timezone(request->tz);
    if (request->result == ESP_OK) {
        pthis->force_emit = true;
        pthis->last.valid = false;
    }
    return 0;
}

esp_err_t nn20clock_timer_set_timezone(NN20ClockTimer *pthis, const char *tz)
{
    if (pthis == NULL || tz == NULL || tz[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    /* Synchronous: the settings screen redraws straight after, and must
     * draw in the zone it just chose. */
    TimezoneRequest request = { .timer = pthis, .tz = tz, .result = ESP_FAIL };
    const esp_err_t posted = post_result(
        nn20_worker_post_sync(pthis->worker, timezone_private, &request));
    if (posted == ESP_ERR_INVALID_STATE) {
        /* The worker has stopped: nobody to race with. */
        return apply_timezone(tz);
    }
    if (posted != ESP_OK) {
        return posted;
    }
    return request.result;
}

#if defined(ESP_PLATFORM)
/* esp_timer callback context: keep it to a post and return. */
static void tick_timer_cb(void *arg)
{
    (void)nn20clock_timer_poll((NN20ClockTimer *)arg);
}
#endif

esp_err_t nn20clock_timer_start(NN20ClockTimer *pthis)
{
    if (pthis == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (nn20clock_timer_is_running(pthis)) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Set before the first poll, which checks it. */
    atomic_store_explicit(&pthis->running, true, memory_order_release);

#if defined(ESP_PLATFORM)
    const esp_timer_create_args_t args = {
        .callback = tick_timer_cb,
        .arg = pthis,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "nn20clock-tick",
    };
    esp_err_t err = esp_timer_create(&args, &pthis->tick_timer);
    if (err == ESP_OK) {
        err = esp_timer_start_periodic(pthis->tick_timer, TICK_PERIOD_US);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tick timer would not start (0x%x)", (unsigned)err);
        atomic_store_explicit(&pthis->running, false, memory_order_release);
        if (pthis->tick_timer != NULL) {
            esp_timer_delete(pthis->tick_timer);
            pthis->tick_timer = NULL;
        }
        return err;
    }
#endif

    /* Emit the current time immediately rather than making a subscriber
     * wait up to a minute for the first MINUTE event. */
    (void)nn20clock_timer_poll(pthis);

    ESP_LOGI(TAG, "started with %u subscriber(s)",
             (unsigned)pthis->subscriber_count);
    return ESP_OK;
}

esp_err_t nn20clock_timer_stop(NN20ClockTimer *pthis)
{
    if (pthis == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!nn20clock_timer_is_running(pthis)) {
        return ESP_ERR_INVALID_STATE;
    }

#if defined(ESP_PLATFORM)
    if (pthis->tick_timer != NULL) {
        /* Stopped before the flag, so no further polls are queued after
         * the drain below. */
        (void)esp_timer_stop(pthis->tick_timer);
        (void)esp_timer_delete(pthis->tick_timer);
        pthis->tick_timer = NULL;
    }
#endif

    atomic_store_explicit(&pthis->running, false, memory_order_release);

    /* Let any poll already queued run to completion, so no subscriber
     * callback is in flight when this returns. */
    (void)nn20clock_timer_flush(pthis);

    /* Next start reports the time afresh rather than comparing against a
     * reading from before the stop. */
    pthis->last.valid = false;

    /* And a stop forgets any pending snooze, for the same reason a
     * reboot does: it is scheduler state, not a stored alarm. */
    pthis->snooze_alarm = NN20CLOCK_ALARM_ID_NONE;
    pthis->snooze_at = (time_t)0;

    ESP_LOGI(TAG, "stopped");
    return ESP_OK;
}

bool nn20clock_timer_is_running(const NN20ClockTimer *pthis)
{
    return pthis != NULL &&
           atomic_load_explicit(&pthis->running, memory_order_acquire);
}

/* ------------------------------------------------------- subscribers -- */

typedef struct {
    NN20ClockTimer *timer;
    NN20ClockTimerSubscriber *subscriber;
    esp_err_t result;
} SubscriberRequest;

static size_t find_subscriber(const NN20ClockTimer *pthis,
                              const NN20ClockTimerSubscriber *subscriber)
{
    for (size_t i = 0; i < pthis->subscriber_count; i++) {
        if (pthis->subscribers[i] == subscriber) {
            return i;
        }
    }
    return pthis->subscriber_count;   /* == count means "not found" */
}

static int subscribe_private(nn20_worker_ctx *worker, void *user_data)
{
    (void)worker;
    SubscriberRequest *request = user_data;
    NN20ClockTimer *pthis = request->timer;

    if (find_subscriber(pthis, request->subscriber) !=
        pthis->subscriber_count) {
        request->result = ESP_ERR_INVALID_STATE;
        return -1;
    }
    if (pthis->subscriber_count >= NN20CLOCK_TIMER_MAX_SUBSCRIBERS) {
        ESP_LOGE(TAG, "subscriber list full (%d)",
                 NN20CLOCK_TIMER_MAX_SUBSCRIBERS);
        request->result = ESP_ERR_NO_MEM;
        return -1;
    }

    pthis->subscribers[pthis->subscriber_count++] = request->subscriber;
    request->result = ESP_OK;
    return 0;
}

esp_err_t nn20clock_timer_subscribe(NN20ClockTimer *pthis,
                                    NN20ClockTimerSubscriber *subscriber)
{
    if (pthis == NULL || subscriber == NULL || subscriber->on_event == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Synchronous: the caller is entitled to know the subscription is
     * live before it returns. The request lives on this stack, which is
     * safe precisely because the caller waits. */
    SubscriberRequest request = {
        .timer = pthis,
        .subscriber = subscriber,
        .result = ESP_FAIL,
    };
    const esp_err_t posted = post_result(
        nn20_worker_post_sync(pthis->worker, subscribe_private, &request));
    return (posted != ESP_OK) ? posted : request.result;
}

static int unsubscribe_private(nn20_worker_ctx *worker, void *user_data)
{
    (void)worker;
    SubscriberRequest *request = user_data;
    NN20ClockTimer *pthis = request->timer;

    const size_t index = find_subscriber(pthis, request->subscriber);
    if (index == pthis->subscriber_count) {
        request->result = ESP_ERR_NOT_FOUND;
        return -1;
    }

    /* Order is not part of the contract, so fill the hole with the last
     * entry rather than shifting the tail down. */
    pthis->subscribers[index] = pthis->subscribers[pthis->subscriber_count - 1];
    pthis->subscribers[--pthis->subscriber_count] = NULL;
    request->result = ESP_OK;
    return 0;
}

esp_err_t nn20clock_timer_unsubscribe(NN20ClockTimer *pthis,
                                      NN20ClockTimerSubscriber *subscriber)
{
    if (pthis == NULL || subscriber == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Synchronous for a reason that matters: callers unsubscribe just
     * before freeing the memory a callback would touch. When this
     * returns, no callback is running or queued against it. */
    SubscriberRequest request = {
        .timer = pthis,
        .subscriber = subscriber,
        .result = ESP_FAIL,
    };
    const esp_err_t posted = post_result(
        nn20_worker_post_sync(pthis->worker, unsubscribe_private, &request));
    return (posted != ESP_OK) ? posted : request.result;
}

size_t nn20clock_timer_subscriber_count(const NN20ClockTimer *pthis)
{
    /* Read without a hop for logs and tests. The list only changes from
     * subscribe/unsubscribe, both of which the caller waits on. */
    return pthis == NULL ? 0u : pthis->subscriber_count;
}

/* ----------------------------------------------------------- alarms -- */

esp_err_t nn20clock_timer_set_alarm_source(NN20ClockTimer *pthis,
                                           NN20ClockTimerAlarmSourceFn source,
                                           void *user_data)
{
    if (pthis == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (nn20clock_timer_is_running(pthis)) {
        /* Worker-owned state; installing it under a running Timer would
         * be a write from the wrong thread. */
        return ESP_ERR_INVALID_STATE;
    }

    pthis->alarm_source = source;
    pthis->alarm_source_user_data = user_data;
    return ESP_OK;
}

typedef struct {
    NN20ClockTimer *timer;
    esp_err_t result;
} ReloadRequest;

static int reload_private(nn20_worker_ctx *worker, void *user_data)
{
    (void)worker;
    ReloadRequest *request = user_data;
    NN20ClockTimer *pthis = request->timer;

    /*
     * On the heap, not the stack. An NN20ClockAlarmList is over 5 KB and
     * this worker's stack is 4 KB - a local overflowed it and reset the
     * board, which only the on-target run revealed. Loading straight
     * into pthis->alarms would avoid the copy but would also destroy the
     * live schedule when a read fails, which is the case this
     * indirection exists for.
     */
    NN20ClockAlarmList *loaded = calloc(1, sizeof(*loaded));
    if (loaded == NULL) {
        request->result = ESP_ERR_NO_MEM;
        return -1;
    }

    request->result = pthis->alarm_source(loaded,
                                          pthis->alarm_source_user_data);
    if (request->result != ESP_OK) {
        /* Keep the schedule that is already live: a clock that forgets
         * its alarms because a read failed is worse than one running a
         * slightly stale list. */
        ESP_LOGE(TAG, "alarm reload failed (0x%x); keeping %u alarm(s)",
                 (unsigned)request->result, (unsigned)pthis->alarms.count);
        free(loaded);
        return -1;
    }

    pthis->alarms = *loaded;
    free(loaded);
    ESP_LOGI(TAG, "schedule reloaded: %u alarm(s)",
             (unsigned)pthis->alarms.count);
    return 0;
}

esp_err_t nn20clock_timer_reload_schedule(NN20ClockTimer *pthis)
{
    if (pthis == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (pthis->alarm_source == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Synchronous: the manager reloads after editing an alarm and is
     * entitled to know the new schedule is live before it says so. */
    ReloadRequest request = { .timer = pthis, .result = ESP_FAIL };
    const esp_err_t posted = post_result(
        nn20_worker_post_sync(pthis->worker, reload_private, &request));
    return (posted != ESP_OK) ? posted : request.result;
}

size_t nn20clock_timer_alarm_count(const NN20ClockTimer *pthis)
{
    return (pthis == NULL) ? 0u : pthis->alarms.count;
}

typedef struct {
    NN20ClockTimer *timer;
    uint32_t alarm_id;
    uint16_t minutes;
    esp_err_t result;
} SnoozeRequest;

static int snooze_private(nn20_worker_ctx *worker, void *user_data)
{
    (void)worker;
    SnoozeRequest *request = user_data;
    NN20ClockTimer *pthis = request->timer;

    const time_t now = pthis->clock_fn(pthis->clock_user_data);

    pthis->snooze_alarm = request->alarm_id;
    pthis->snooze_at = now + ((time_t)request->minutes * 60);
    request->result = ESP_OK;

    ESP_LOGI(TAG, "alarm %u snoozed for %u minute(s)",
             (unsigned)request->alarm_id, (unsigned)request->minutes);
    return 0;
}

esp_err_t nn20clock_timer_snooze(NN20ClockTimer *pthis, uint32_t alarm_id,
                                 uint16_t minutes)
{
    if (pthis == NULL || alarm_id == NN20CLOCK_ALARM_ID_NONE ||
        minutes == 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Synchronous: the caller is about to tell the user the alarm will
     * come back, and should know it is scheduled before it does. */
    SnoozeRequest request = {
        .timer = pthis,
        .alarm_id = alarm_id,
        .minutes = minutes,
        .result = ESP_FAIL,
    };
    const esp_err_t posted = post_result(
        nn20_worker_post_sync(pthis->worker, snooze_private, &request));
    return (posted != ESP_OK) ? posted : request.result;
}

static int cancel_snooze_private(nn20_worker_ctx *worker, void *user_data)
{
    (void)worker;
    NN20ClockTimer *pthis = user_data;

    if (pthis->snooze_alarm != NN20CLOCK_ALARM_ID_NONE) {
        ESP_LOGI(TAG, "snooze for alarm %u cancelled",
                 (unsigned)pthis->snooze_alarm);
    }
    pthis->snooze_alarm = NN20CLOCK_ALARM_ID_NONE;
    pthis->snooze_at = (time_t)0;
    return 0;
}

esp_err_t nn20clock_timer_cancel_snooze(NN20ClockTimer *pthis)
{
    if (pthis == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return post_result(
        nn20_worker_post_sync(pthis->worker, cancel_snooze_private, pthis));
}

uint32_t nn20clock_timer_snoozed_alarm(const NN20ClockTimer *pthis)
{
    return (pthis == NULL) ? NN20CLOCK_ALARM_ID_NONE : pthis->snooze_alarm;
}

time_t nn20clock_timer_snooze_time(const NN20ClockTimer *pthis)
{
    return (pthis == NULL) ? (time_t)0 : pthis->snooze_at;
}

uint32_t nn20clock_timer_missed_alarm_count(const NN20ClockTimer *pthis)
{
    if (pthis == NULL) {
        return 0u;
    }
    return atomic_load_explicit(&pthis->missed_alarms, memory_order_relaxed);
}

/* ------------------------------------------------------------- now --- */

typedef struct {
    NN20ClockTimer *timer;
    NN20ClockDateTime *out;
    esp_err_t result;
} NowRequest;

static int get_now_private(nn20_worker_ctx *worker, void *user_data)
{
    (void)worker;
    NowRequest *request = user_data;
    NN20ClockTimer *pthis = request->timer;

    const time_t now = pthis->clock_fn(pthis->clock_user_data);
    request->result = nn20clock_timer_to_local(now, request->out);
    return (request->result == ESP_OK) ? 0 : -1;
}

esp_err_t nn20clock_timer_get_now(NN20ClockTimer *pthis,
                                  NN20ClockDateTime *out_now)
{
    if (pthis == NULL || out_now == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    NowRequest request = {
        .timer = pthis,
        .out = out_now,
        .result = ESP_FAIL,
    };
    const esp_err_t posted = post_result(
        nn20_worker_post_sync(pthis->worker, get_now_private, &request));
    if (posted != ESP_OK) {
        memset(out_now, 0, sizeof(*out_now));
        return posted;
    }
    return request.result;
}

