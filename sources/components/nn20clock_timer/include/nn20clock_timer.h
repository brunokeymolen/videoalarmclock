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
 * nn20clock_timer.h - the clock's central timing component (design 6, 7).
 *
 * The Timer keeps the current local time, emits second and minute
 * events, and will emit alarm events once the alarm model exists
 * (Milestone 3). It runs on its own worker thread rather than one of
 * design 4's four: a one-second tick must not queue behind SD card I/O
 * or a screen redraw.
 *
 * Time comes from the system clock, which design 14 requires to already
 * be correct - NTP sets it, and the fallbacks behind it are the RTC and
 * the last known good time. The Timer does not convert timezones
 * itself; it applies the configured POSIX TZ to the process once and
 * reads local time from then on.
 *
 * Threading: everything the Timer owns lives on its worker, and
 * subscriber callbacks run there too. A subscriber must not touch LVGL
 * from one - see the note on NN20ClockTimerEventFn. There are no
 * mutexes here; the queue is the synchronization.
 *
 * Alarm scheduling (Milestone 3) is here too: the Timer holds the
 * schedule, and every tick asks which alarms occur in the span since
 * the previous reading. See nn20clock_alarm.h for why that is an
 * interval rather than an instant - it is the difference between an
 * alarm that survives a late tick and one that silently does not.
 */
#ifndef NN20CLOCK_TIMER_H
#define NN20CLOCK_TIMER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "nn20clock_alarm.h"
#include "nn20clock_platform.h"
#include "nn20clock_time.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Storage is design 9; not implemented in Track 1. The Timer takes one so
 * its constructor already has the shape design 7 specifies, and tolerates
 * NULL until alarm schedules actually load (Milestone 3). */
typedef struct NN20ClockStorage NN20ClockStorage;

typedef struct NN20ClockTimer NN20ClockTimer;

/* -------------------------------------------------------------- time -- */

/* NN20ClockDate and NN20ClockDateTime come from nn20clock_time.h, which
 * both this and the alarm model include - see the note there. */

/* ------------------------------------------------------------ events -- */

typedef enum {
    NN20CLOCK_TIMER_EVENT_SECOND,
    NN20CLOCK_TIMER_EVENT_MINUTE,
    NN20CLOCK_TIMER_EVENT_TIME_SYNCED,
    NN20CLOCK_TIMER_EVENT_ALARM,
    NN20CLOCK_TIMER_EVENT_ERROR
} NN20ClockTimerEventType;

/*
 * The base payload every event payload derives from (design 6). Payload
 * structs embed it as their first field:
 *
 *     typedef struct {
 *         NN20ClockTimerPayload super;
 *         uint32_t alarm_id;
 *     } NN20ClockTimerAlarmPayload;
 *
 * displayed_time carries both date and time, and is what consumers use
 * for alarm matching, snooze arithmetic, and display formatting. A UI
 * that wants a compact string formats it as "HHMMSSyyyymmdd" itself.
 *
 * Do not apply a timezone offset or any second conversion to it: design
 * 6 requires the system clock and TZ to already be correct before an
 * event is emitted.
 */
typedef struct {
    time_t displayed_time;
    /*
     * Whether the clock has been set from an authoritative source
     * (design 14). Carried on every event, not just the synced one,
     * because a screen built after the sync happened would otherwise
     * never learn it - and a clock that says "not set" forever is
     * worse than one that says nothing.
     */
    bool time_synced;
    /*
     * Whether a snooze is waiting to go off (design 10).
     *
     * Carried on every event for the same reason time_synced is: the
     * clock face shows that an alarm is only resting, and a screen
     * built after the snooze was set would otherwise never learn about
     * it. RAM-only, so it is false again after a reboot.
     */
    bool snooze_pending;
} NN20ClockTimerPayload;

/*
 * The payload for NN20CLOCK_TIMER_EVENT_ALARM, deriving from the base
 * as design 6 describes. `displayed_time` is the moment the alarm was
 * scheduled for, not the moment it was noticed - those differ by
 * however late the tick was, and it is the scheduled time a user
 * recognises.
 */
typedef struct {
    NN20ClockTimerPayload super;
    uint32_t alarm_id;
    /* How late the event is, in seconds. Normally 0; non-zero after a
     * busy moment. Always within the grace window below, because
     * anything later is reported as missed instead of fired. */
    uint32_t late_seconds;
} NN20ClockTimerAlarmPayload;

typedef struct {
    NN20ClockTimerEventType type;
    /* Points at a NN20ClockTimerPayload or a struct deriving from one,
     * per type. Valid only for the duration of the callback. */
    const NN20ClockTimerPayload *payload;
} NN20ClockTimerEvent;

/*
 * How late an alarm may be and still ring.
 *
 * A tick can be delayed by a busy worker or a slow read, and an alarm
 * that is a second or two late should still go off. An alarm that is
 * hours late should not: the device was off, or the clock was corrected,
 * and ringing a 07:00 alarm at 09:30 is worse than not ringing it. Past
 * this window the alarm is reported as missed and skipped.
 */
#define NN20CLOCK_TIMER_ALARM_GRACE_SECONDS 60

/*
 * Runs on the Timer's worker thread, not the caller's and not the LVGL
 * task. Keep it short, copy anything you need to outlive the call, and
 * hand UI work to the UI thread rather than doing it here.
 *
 * Design 6 leaves filtering to the subscriber: every subscriber is
 * called for every event and inspects event->type itself.
 */
typedef void (*NN20ClockTimerEventFn)(const NN20ClockTimerEvent *event,
                                      void *user_data);

/*
 * Caller-owned. It must stay alive and unmoved from
 * nn20clock_timer_subscribe() until nn20clock_timer_unsubscribe(); the
 * Timer stores the pointer, not a copy.
 */
typedef struct {
    NN20ClockTimerEventFn on_event;
    void *user_data;
} NN20ClockTimerSubscriber;

/* Fixed capacity keeps the subscriber list allocation-free. The design
 * names four consumers (ClockManager plus the three UIs); 8 leaves room. */
#define NN20CLOCK_TIMER_MAX_SUBSCRIBERS 8

/* ------------------------------------------------------ time source -- */

/*
 * Where "now" comes from, as seconds since the epoch. Replaceable so a
 * test can hand the Timer any instant it likes - midnight, a leap day,
 * the far side of 2038 - without waiting for a real clock or touching
 * the system time.
 *
 * The default reads the system clock, which is what design 14's NTP,
 * RTC, and last-known-time chain all ultimately set.
 */
typedef time_t (*NN20ClockTimerClockFn)(void *user_data);

/* --------------------------------------------------------- lifecycle -- */

/*
 * storage may be NULL until Milestone 3. Returns NULL if the worker
 * thread cannot be created.
 */
NN20ClockTimer *nn20clock_timer_ctor(NN20ClockStorage *storage);

/*
 * Replace the time source. Must be called before _start(); the Timer
 * reads it from its own worker afterwards.
 *
 * ESP_ERR_INVALID_STATE if the Timer is already running,
 * ESP_ERR_INVALID_ARG on a NULL timer or clock function.
 */
esp_err_t nn20clock_timer_set_clock_fn(NN20ClockTimer *pthis,
                                       NN20ClockTimerClockFn clock_fn,
                                       void *user_data);

/*
 * Apply a POSIX TZ string, e.g. "CET-1CEST,M3.5.0,M10.5.0/3" (design
 * 14). This sets the process timezone, so every later conversion to
 * local time uses it. Call it before _start() so the first tick is
 * already correct.
 *
 * Design 9 stores the timezone; until storage can supply one
 * (Milestone 3) the value comes from Kconfig.
 */
esp_err_t nn20clock_timer_set_timezone(NN20ClockTimer *pthis, const char *tz);

/* Stops the worker if it is still running. Safe with NULL. */
void nn20clock_timer_dtor(NN20ClockTimer *pthis);

/*
 * Begin ticking. On the target this starts a periodic esp_timer that
 * polls the clock several times a second; on the host there is no
 * automatic tick and a test drives nn20clock_timer_poll() itself, which
 * is what makes the event logic testable without waiting in real time.
 *
 * The first poll emits a SECOND and a MINUTE event, because there is no
 * previous reading to compare against and a fresh subscriber needs the
 * current time.
 *
 * ESP_ERR_INVALID_ARG on NULL, ESP_ERR_INVALID_STATE if already running.
 */
esp_err_t nn20clock_timer_start(NN20ClockTimer *pthis);

/*
 * Sample the clock once and emit whatever changed: a SECOND event when
 * the second differs from the last reading, a MINUTE event when the
 * minute does. Queued on the worker, so it is safe from any thread and
 * from an ISR-adjacent context like an esp_timer callback.
 *
 * Polling faster than once a second costs nothing but a comparison and
 * bounds how late a minute change can be noticed; polling is what the
 * tick actually is, so the events stay aligned to the real clock rather
 * than drifting with a counter.
 */
esp_err_t nn20clock_timer_poll(NN20ClockTimer *pthis);

/*
 * Make the next poll report the time even if nothing has changed.
 *
 * For a screen that has just been built: it has drawn no time yet, and
 * without this it would show a placeholder until the minute happened to
 * turn - up to a minute of a clock showing nothing. ClockManager calls
 * it after showing a screen.
 */
esp_err_t nn20clock_timer_refresh(NN20ClockTimer *pthis);

/*
 * Wait until everything already queued on the Timer's worker has run.
 * For tests and for orderly shutdown; not needed in normal operation.
 */
esp_err_t nn20clock_timer_flush(NN20ClockTimer *pthis);

/*
 * Report that the system clock has been set from an authoritative
 * source - the SNTP callback calls this (design 14). Emits
 * NN20CLOCK_TIMER_EVENT_TIME_SYNCED, and forces the next poll to emit a
 * SECOND and MINUTE event even if the reading looks unchanged, because
 * a sync can move the clock by hours.
 */
esp_err_t nn20clock_timer_notify_time_synced(NN20ClockTimer *pthis);

/*
 * The same thing, done by the user at the settings screen rather than
 * by SNTP. A device with no internet has no other way to become
 * correct, so a hand-set clock is authoritative too: it counts as
 * synced, and the jump it just made is re-baselined exactly as a sync's
 * is. Use this rather than nn20clock_timer_refresh() after moving the
 * clock - refresh only asks for a redraw, and leaves the schedule
 * believing the jump was elapsed time.
 */
esp_err_t nn20clock_timer_notify_time_set(NN20ClockTimer *pthis);

/* True once the clock has been set from an authoritative source -
 * SNTP, or by hand. Until then the displayed time is whatever the RTC
 * believed. */
bool nn20clock_timer_is_time_synced(const NN20ClockTimer *pthis);

/*
 * Drain the worker queue and stop the thread. ESP_ERR_INVALID_ARG on
 * NULL, ESP_ERR_INVALID_STATE if not running. Idempotent from the
 * caller's point of view only in that stopping twice reports the state,
 * it does not fail destructively.
 */
esp_err_t nn20clock_timer_stop(NN20ClockTimer *pthis);

bool nn20clock_timer_is_running(const NN20ClockTimer *pthis);

/* ----------------------------------------------------------- alarms -- */

/*
 * Where the Timer gets its alarms. Called on the Timer's worker from
 * nn20clock_timer_reload_schedule(); it must fill `out_alarms` and
 * return ESP_OK, or return an error to leave the current schedule
 * alone.
 *
 * A callback rather than a direct call into Storage on purpose: it
 * keeps the Timer independent of the storage layer (design 7 says the
 * Timer loads schedules, not that it knows where from), and lets a test
 * supply any schedule it likes without a filesystem.
 */
typedef esp_err_t (*NN20ClockTimerAlarmSourceFn)(NN20ClockAlarmList *out_alarms,
                                                 void *user_data);

/* Install before _start(). */
esp_err_t nn20clock_timer_set_alarm_source(NN20ClockTimer *pthis,
                                           NN20ClockTimerAlarmSourceFn source,
                                           void *user_data);

/*
 * Re-read the schedule from the alarm source (design 7, 8). Runs on the
 * Timer's worker and waits for it, so when this returns the new
 * schedule is live.
 *
 * ClockManager calls this after alarms are added, edited, or deleted.
 * ESP_ERR_INVALID_STATE if no alarm source is installed.
 */
esp_err_t nn20clock_timer_reload_schedule(NN20ClockTimer *pthis);

/* Alarms currently scheduled. */
size_t nn20clock_timer_alarm_count(const NN20ClockTimer *pthis);

/* ----------------------------------------------------------- snooze -- */

/*
 * Schedule `alarm_id` to fire again in `minutes`.
 *
 * A pending snooze is RAM-only scheduler state, not a stored alarm: a
 * reboot forgets it, which is what design 10 specifies. It is matched
 * by the same interval scan as everything else, so it inherits the
 * skipped-second behaviour and the grace window.
 *
 * Only one snooze is pending at a time - there is one speaker - so this
 * replaces any previous one. The event it eventually emits carries the
 * original alarm_id, so the ClockManager can tell which alarm came
 * back.
 *
 * ESP_ERR_INVALID_ARG for an unknown id or zero minutes.
 */
esp_err_t nn20clock_timer_snooze(NN20ClockTimer *pthis, uint32_t alarm_id,
                                 uint16_t minutes);

/*
 * Forget any pending snooze. Idempotent, and safe to call when nothing
 * is pending - which is what dismissing an alarm does, without having
 * to know whether one was set.
 */
esp_err_t nn20clock_timer_cancel_snooze(NN20ClockTimer *pthis);

/* The alarm a snooze is pending for, or NN20CLOCK_ALARM_ID_NONE. */
uint32_t nn20clock_timer_snoozed_alarm(const NN20ClockTimer *pthis);

/* When the pending snooze fires, or 0 when none is pending. */
time_t nn20clock_timer_snooze_time(const NN20ClockTimer *pthis);

/*
 * Alarms that were found in a scanned interval but were too late to
 * ring - see NN20CLOCK_TIMER_ALARM_GRACE_SECONDS. Counted rather than
 * hidden: a rising number means the device is losing alarms, which is
 * the failure a user would notice and the log would otherwise not
 * explain.
 */
uint32_t nn20clock_timer_missed_alarm_count(const NN20ClockTimer *pthis);

/* ------------------------------------------------------- subscribers -- */

/*
 * Both are synchronous: they run on the Timer's worker and wait for it,
 * so when subscribe() returns the subscriber is live, and when
 * unsubscribe() returns no callback is running or queued against it.
 * That second guarantee is the important one - a subscriber usually
 * unsubscribes just before it frees the memory the callback would
 * touch.
 *
 * Safe to call from any thread, and while the Timer is running.
 *
 * ESP_ERR_INVALID_ARG on a NULL timer, NULL subscriber, or a subscriber
 * with no on_event. ESP_ERR_NO_MEM when the list is full.
 * ESP_ERR_INVALID_STATE if this exact subscriber is already subscribed.
 */
esp_err_t nn20clock_timer_subscribe(NN20ClockTimer *pthis,
                                    NN20ClockTimerSubscriber *subscriber);

/* ESP_ERR_NOT_FOUND if the subscriber is not currently subscribed. */
esp_err_t nn20clock_timer_unsubscribe(NN20ClockTimer *pthis,
                                      NN20ClockTimerSubscriber *subscriber);

size_t nn20clock_timer_subscriber_count(const NN20ClockTimer *pthis);

/*
 * The current local time. Runs on the Timer's worker and waits for it,
 * so the result is a coherent reading rather than a struct assembled
 * across a tick.
 *
 * ESP_ERR_INVALID_ARG on NULL. Succeeds whether or not the clock has
 * been synced - use nn20clock_timer_is_time_synced() to find out
 * whether to trust it.
 */
esp_err_t nn20clock_timer_get_now(NN20ClockTimer *pthis,
                                  NN20ClockDateTime *out_now);

/* Convert a unix time to local broken-down time, using the timezone
 * currently in force. Pure and free-standing, so the conversion and the
 * weekday convention can be tested directly. */
esp_err_t nn20clock_timer_to_local(time_t when, NN20ClockDateTime *out);



#ifdef __cplusplus
}
#endif

#endif /* NN20CLOCK_TIMER_H */
