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
 * nn20clock_manager.c - the ClockManager state machine (design 5, 8).
 *
 * Same two-halves shape as the rest of the project: a public function
 * copies its arguments and posts; a `_private` function runs on the
 * ClockManagerWorker and is the only code that reads or writes the
 * manager's state.
 *
 * The few fields other threads read - the current state and the
 * counters - are atomics rather than plain members. That is not
 * locking and it is not how the manager synchronizes anything: writes
 * still happen only on the worker, and the atomics exist so a snapshot
 * read from another thread is a defined value instead of a data race.
 */
#include "nn20clock_manager.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "nn20clock_reqpool.h"

static const char *TAG = "NN20CLOCK_MANAGER";

/* Requests in flight. Timer events arrive at 1 Hz and UI commands at
 * human speed, so this is headroom for a burst, not a throughput knob. */
#define MANAGER_MAX_PENDING 8

typedef enum {
    REQUEST_STATE,
    REQUEST_SCREEN,
    REQUEST_COMMAND,
    REQUEST_TIMER_EVENT
} RequestKind;

typedef struct {
    NN20ClockManager *manager;
    RequestKind kind;
    size_t slot;

    NN20ClockManagerState state;          /* REQUEST_STATE */
    NN20ClockUiBase *screen;              /* REQUEST_SCREEN */
    NN20ClockUiCommand command;           /* REQUEST_COMMAND */
    NN20ClockTimerEventType timer_type;   /* REQUEST_TIMER_EVENT */
    NN20ClockTimerPayload payload;        /* REQUEST_TIMER_EVENT */
    /* Only set for an alarm event, whose payload derives from the base
     * and carries more than it does. */
    uint32_t alarm_id;
    uint32_t alarm_late_seconds;
} ManagerRequest;

struct NN20ClockManager {
    nn20_worker_ctx *worker;      /* borrowed; owned by NN20ClockWorkers */
    NN20ClockStorage *storage;    /* borrowed; NULL until Milestone 3 */
    NN20ClockTimer *timer;        /* borrowed */

    /* ---- worker-owned: only manager-worker callbacks touch these. --- */
    NN20ClockManagerStateFn on_state;
    void *state_user_data;
    NN20ClockManagerRingingEndedFn on_ringing_ended;
    void *ringing_ended_user_data;
    NN20ClockManagerScreenFn screen_factory;
    void *screen_factory_user_data;
    /* Set before start; read from the UiWorker afterwards, never
     * written again. */
    NN20ClockDeviceHooks device_hooks;
    NN20ClockMediaHooks media_hooks;

    /* The Timer copies the pointer, not the struct, so this must live as
     * long as the subscription does - hence a member, not a local. */
    NN20ClockTimerSubscriber subscriber;

    /* The alarm currently ringing; worker-owned, but read as a snapshot
     * from elsewhere, so atomic. */
    atomic_uint ringing_alarm;
    atomic_uint alarm_count;

    /*
     * When the alarm in flight stops being allowed to ring, and which
     * alarm that deadline belongs to. Zero and ID_NONE when none is.
     *
     * Kept here rather than on the ringing screen because a snooze
     * destroys that screen and this must survive it - see
     * NN20CLOCK_MANAGER_MAX_RING_SECONDS - and because the screen's
     * timers stop running the moment the player takes the panel.
     */
    _Atomic(time_t) ring_deadline;
    uint32_t ring_deadline_alarm;   /* worker-owned */

    /* ---- snapshots other threads read. ------------------------------
     *
     * All of these are written only on the manager worker. They are
     * atomic so that a read from another thread is a defined value
     * rather than a data race - that is all. Nothing here is a lock, and
     * no decision is made on one of these off the worker: a caller that
     * wants to act on the state posts a request and lets the worker
     * decide with the value it alone owns. */
    _Atomic(NN20ClockUiBase *) screen;   /* owned once handed over */
    atomic_bool running;
    atomic_int state;
    atomic_uint rejected_transitions;
    atomic_uint command_count;
    atomic_uint timer_event_count;

    /* ---- claimed from any thread, read on the manager worker. ------- */
    ManagerRequest requests[MANAGER_MAX_PENDING];
    atomic_bool request_in_use[MANAGER_MAX_PENDING];
    NN20ClockReqPool request_pool;
};

/* ------------------------------------------------------------ states -- */

const char *nn20clock_manager_state_name(NN20ClockManagerState state)
{
    switch (state) {
    case NN20CLOCK_STATE_BOOT:              return "BOOT";
    case NN20CLOCK_STATE_TIME:              return "TIME";
    case NN20CLOCK_STATE_ALARM_SETTINGS:    return "ALARM_SETTINGS";
    case NN20CLOCK_STATE_DEVICE_SETTINGS:   return "DEVICE_SETTINGS";
    case NN20CLOCK_STATE_ALARM_RINGING:     return "ALARM_RINGING";
    case NN20CLOCK_STATE_MEDIA_PLAYBACK:    return "MEDIA_PLAYBACK";
    case NN20CLOCK_STATE_MEDIA_MANAGEMENT:  return "MEDIA_MANAGEMENT";
    case NN20CLOCK_STATE_ERROR:             return "ERROR";
    case NN20CLOCK_STATE_COUNT:             break;
    }
    return "UNKNOWN";
}

/*
 * Design 5's state diagram, one row per source state. Read it against
 * the mermaid diagram in the design - if the two ever disagree, the
 * design wins and this table is the bug.
 *
 * Two edges are worth calling out. ALARM_RINGING -> ALARM_RINGING is
 * real: it is the playback-fallback edge, where media fails and the
 * alarm restarts with the fallback sound. And every state can reach
 * ERROR, because a storage or playback failure can happen anywhere.
 */
static const bool TRANSITIONS[NN20CLOCK_STATE_COUNT][NN20CLOCK_STATE_COUNT] = {
    /* from BOOT */
    [NN20CLOCK_STATE_BOOT] = {
        [NN20CLOCK_STATE_TIME] = true,
        [NN20CLOCK_STATE_ERROR] = true,
    },
    /* from TIME */
    [NN20CLOCK_STATE_TIME] = {
        [NN20CLOCK_STATE_ALARM_SETTINGS] = true,
        [NN20CLOCK_STATE_DEVICE_SETTINGS] = true,
        [NN20CLOCK_STATE_MEDIA_PLAYBACK] = true,
        [NN20CLOCK_STATE_ALARM_RINGING] = true,
        [NN20CLOCK_STATE_ERROR] = true,
    },
    /* from ALARM_SETTINGS */
    [NN20CLOCK_STATE_ALARM_SETTINGS] = {
        [NN20CLOCK_STATE_TIME] = true,
        [NN20CLOCK_STATE_ALARM_RINGING] = true,
        [NN20CLOCK_STATE_ERROR] = true,
    },
    /* from DEVICE_SETTINGS. Mirrors ALARM_SETTINGS: leave by saving or
     * cancelling, be interrupted by an alarm, or fall into ERROR on a
     * storage failure - plus the one door only this screen has, into
     * media management. */
    [NN20CLOCK_STATE_DEVICE_SETTINGS] = {
        [NN20CLOCK_STATE_TIME] = true,
        [NN20CLOCK_STATE_MEDIA_MANAGEMENT] = true,
        [NN20CLOCK_STATE_ALARM_RINGING] = true,
        [NN20CLOCK_STATE_ERROR] = true,
    },

    /* from ALARM_RINGING */
    [NN20CLOCK_STATE_ALARM_RINGING] = {
        [NN20CLOCK_STATE_TIME] = true,           /* dismiss, or snoozed */
        [NN20CLOCK_STATE_ALARM_RINGING] = true,  /* playback fallback */
        [NN20CLOCK_STATE_ERROR] = true,
    },
    /* from MEDIA_PLAYBACK. Design 5: stop, or the sleep timer, or an
     * alarm that outranks whatever the user was watching. */
    [NN20CLOCK_STATE_MEDIA_PLAYBACK] = {
        [NN20CLOCK_STATE_TIME] = true,
        [NN20CLOCK_STATE_ALARM_RINGING] = true,
        [NN20CLOCK_STATE_ERROR] = true,
    },

    /*
     * from MEDIA_MANAGEMENT. Back to DEVICE_SETTINGS rather than to the
     * clock face: design 5 draws the door both ways, and a screen
     * reached from inside settings should return there rather than drop
     * the user two levels out.
     */
    [NN20CLOCK_STATE_MEDIA_MANAGEMENT] = {
        [NN20CLOCK_STATE_DEVICE_SETTINGS] = true,
        [NN20CLOCK_STATE_ALARM_RINGING] = true,
        [NN20CLOCK_STATE_ERROR] = true,
    },
    /* from ERROR */
    [NN20CLOCK_STATE_ERROR] = {
        [NN20CLOCK_STATE_TIME] = true,            /* recovered */
        [NN20CLOCK_STATE_ALARM_SETTINGS] = true,  /* user edits config */
    },
};

bool nn20clock_manager_transition_allowed(NN20ClockManagerState from,
                                          NN20ClockManagerState to)
{
    if (from < 0 || from >= NN20CLOCK_STATE_COUNT ||
        to < 0 || to >= NN20CLOCK_STATE_COUNT) {
        return false;
    }
    return TRANSITIONS[from][to];
}

/* ---------------------------------------------------------- plumbing -- */

static esp_err_t post_result(int rc)
{
    if (rc == 0) {
        return ESP_OK;
    }
    return (rc == 1) ? ESP_ERR_TIMEOUT : ESP_ERR_INVALID_STATE;
}

static ManagerRequest *claim_request(NN20ClockManager *pthis, RequestKind kind)
{
    const size_t slot = nn20clock_reqpool_claim(&pthis->request_pool);
    if (slot == MANAGER_MAX_PENDING) {
        ESP_LOGW(TAG, "all %d request slots in flight", MANAGER_MAX_PENDING);
        return NULL;
    }

    ManagerRequest *request = &pthis->requests[slot];
    memset(request, 0, sizeof(*request));
    request->manager = pthis;
    request->kind = kind;
    request->slot = slot;
    return request;
}

static void apply_screen(NN20ClockManager *pthis, NN20ClockUiBase *screen);

/* Runs on the manager worker. Applies the transition if design 5 has the
 * edge; otherwise counts it and leaves the state alone. */
static void apply_state(NN20ClockManager *pthis, NN20ClockManagerState to)
{
    const NN20ClockManagerState from =
        (NN20ClockManagerState)atomic_load_explicit(&pthis->state,
                                                    memory_order_relaxed);

    if (!nn20clock_manager_transition_allowed(from, to)) {
        atomic_fetch_add_explicit(&pthis->rejected_transitions, 1u,
                                  memory_order_relaxed);
        ESP_LOGW(TAG, "rejected %s -> %s",
                 nn20clock_manager_state_name(from),
                 nn20clock_manager_state_name(to));
        return;
    }

    atomic_store_explicit(&pthis->state, (int)to, memory_order_relaxed);
    ESP_LOGI(TAG, "%s -> %s", nn20clock_manager_state_name(from),
             nn20clock_manager_state_name(to));

    /*
     * The screen follows the state. apply_screen() destroys the
     * outgoing one before showing the new one, which is design 8's
     * required order; a factory that has no screen for this state
     * returns NULL and the display is simply left empty.
     *
     * A repeat of the same state - the ALARM_RINGING playback-fallback
     * edge - deliberately rebuilds, because that edge exists precisely
     * to start the media over.
     */
    if (pthis->screen_factory != NULL) {
        apply_screen(pthis,
                     pthis->screen_factory(to, pthis->screen_factory_user_data));
    }

    if (pthis->on_state != NULL) {
        pthis->on_state(from, to, pthis->state_user_data);
    }
}

/* Runs on the manager worker. Destroys the outgoing screen before the
 * incoming one is shown (design 8). */
static void apply_screen(NN20ClockManager *pthis, NN20ClockUiBase *screen)
{
    NN20ClockUiBase *const current =
        atomic_load_explicit(&pthis->screen, memory_order_relaxed);
    if (current == screen) {
        return;
    }

    if (current != NULL) {
        ESP_LOGD(TAG, "destroying screen %s", nn20clock_ui_name(current));
        /* Cleared before the destroy, not after: the screen is freed in
         * there, so anything reading the pointer meanwhile must not find
         * it. Waits on the UiWorker - safe in this direction only,
         * because nothing on the UiWorker ever waits on this worker. */
        atomic_store_explicit(&pthis->screen, NULL, memory_order_release);
        nn20clock_ui_destroy(current);
    }

    atomic_store_explicit(&pthis->screen, screen, memory_order_release);
    if (screen != NULL) {
        ESP_LOGD(TAG, "showing screen %s", nn20clock_ui_name(screen));
        (void)nn20clock_ui_show(screen);

        /*
         * A screen that has just been built has drawn no time. Asking
         * the Timer to report on its next tick fills it in within a
         * fraction of a second, instead of leaving a clock face showing
         * placeholders until the minute happens to turn.
         */
        (void)nn20clock_timer_refresh(pthis->timer);
    }
}

/* ------------------------------------------------- the ringing bound -- */

/*
 * Runs on the manager worker. Starts the clock on how long this alarm
 * may hold the device - see NN20CLOCK_MANAGER_MAX_RING_SECONDS.
 *
 * `when` is the occurrence's own time, straight off the alarm event, so
 * an alarm noticed a few seconds late still stops on the hour after the
 * time the user set rather than the hour after the device caught up.
 *
 * The deadline belongs to an alarm id and is only ever set once for it.
 * That is what makes a snooze not extend it: the alarm comes back with
 * the same id and finds its deadline already standing. A different
 * alarm ringing replaces it, because it is a different hour.
 */
static void arm_ring_deadline(NN20ClockManager *pthis, uint32_t alarm_id,
                              time_t when)
{
    const time_t standing = atomic_load_explicit(&pthis->ring_deadline,
                                                 memory_order_relaxed);
    if (standing != 0 && pthis->ring_deadline_alarm == alarm_id) {
        return;   /* the same alarm, resumed: its hour is already running */
    }

    /*
     * A timer event with no clock behind it - the host tests build one,
     * and so does a device whose time is not set. Bounding against a
     * zero epoch would stop the alarm in its first second, which is far
     * worse than not bounding it, so there is no deadline until there
     * is a time to measure from.
     */
    if (when <= 0) {
        pthis->ring_deadline_alarm = NN20CLOCK_ALARM_ID_NONE;
        atomic_store_explicit(&pthis->ring_deadline, (time_t)0,
                              memory_order_release);
        return;
    }

    pthis->ring_deadline_alarm = alarm_id;
    atomic_store_explicit(&pthis->ring_deadline,
                          when + (time_t)NN20CLOCK_MANAGER_MAX_RING_SECONDS,
                          memory_order_release);
}

/* Runs on the manager worker. The alarm is over; nothing is owed a
 * deadline until the next one goes off. */
static void clear_ring_deadline(NN20ClockManager *pthis)
{
    pthis->ring_deadline_alarm = NN20CLOCK_ALARM_ID_NONE;
    atomic_store_explicit(&pthis->ring_deadline, (time_t)0,
                          memory_order_release);
}

/* Runs on the manager worker. Has the alarm in flight outstayed its
 * hour, as of `now`? False when no deadline stands. */
static bool ring_deadline_passed(const NN20ClockManager *pthis, time_t now)
{
    const time_t deadline = atomic_load_explicit(&pthis->ring_deadline,
                                                 memory_order_relaxed);
    return deadline != 0 && now >= deadline;
}

/*
 * Runs on the manager worker. Schedules the ringing alarm to come back.
 *
 * Distinct from dismissing, and not just in name: the alarm is left
 * exactly as it is - a one-off is NOT deleted, because it has not
 * finished happening yet - and the Timer is asked to fire it again
 * shortly. Design 10 puts the snooze in RAM, so a reboot in the
 * meantime forgets it.
 */
static void snooze_alarm(NN20ClockManager *pthis)
{
    const uint32_t alarm_id =
        atomic_exchange_explicit(&pthis->ringing_alarm,
                                 NN20CLOCK_ALARM_ID_NONE,
                                 memory_order_acq_rel);
    if (alarm_id == NN20CLOCK_ALARM_ID_NONE) {
        return;   /* nothing was ringing */
    }

    /*
     * The alarm's own snooze length, falling back to the model's
     * default when the record cannot be read - a snooze that silently
     * does not happen would be worse than one of the wrong length.
     */
    uint16_t minutes = NN20CLOCK_ALARM_DEFAULT_SNOOZE_MINUTES;
    if (pthis->storage != NULL) {
        NN20ClockAlarmConfig alarm;
        if (nn20clock_storage_get_alarm(pthis->storage, alarm_id, &alarm)
                == ESP_OK &&
            alarm.snooze_minutes > 0u) {
            minutes = alarm.snooze_minutes;
        }
    }

    if (nn20clock_timer_snooze(pthis->timer, alarm_id, minutes) != ESP_OK) {
        ESP_LOGE(TAG, "could not snooze alarm %u", (unsigned)alarm_id);
        return;
    }
    /*
     * The deadline is deliberately left standing. A snooze is the same
     * alarm continuing, so the hour it was given at 07:00 is the hour
     * it still has - see NN20CLOCK_MANAGER_MAX_RING_SECONDS. Nine
     * minutes of quiet is not nine minutes of credit.
     */
    ESP_LOGI(TAG, "alarm %u snoozed for %u minute(s)", (unsigned)alarm_id,
             (unsigned)minutes);
}

/*
 * Tell the subscriber that an alarm occurrence has ended for good - see
 * NN20ClockManagerRingingEndedFn. Runs on the manager worker, like
 * everything that calls it.
 *
 * Silent when no alarm was ringing, so callers can hand over whatever
 * they have without checking first.
 */
static void report_ringing_ended(NN20ClockManager *pthis, uint32_t alarm_id)
{
    if (alarm_id == NN20CLOCK_ALARM_ID_NONE ||
        pthis->on_ringing_ended == NULL) {
        return;
    }
    pthis->on_ringing_ended(alarm_id, pthis->ringing_ended_user_data);
}

/*
 * Runs on the manager worker. Completes design 10's one-off lifecycle:
 * "after the user dismisses the alarm ... the fired one-off alarm
 * should be removed from storage".
 *
 * A recurrent alarm is left alone - it is meant to come back tomorrow.
 * Only the one-off is deleted, and the Timer is then told to reload so
 * its schedule matches what is actually stored.
 */
static void dismiss_alarm(NN20ClockManager *pthis)
{
    const uint32_t alarm_id =
        atomic_exchange_explicit(&pthis->ringing_alarm,
                                 NN20CLOCK_ALARM_ID_NONE,
                                 memory_order_acq_rel);

    /*
     * Unconditionally, and before anything else: dismissing ends the
     * alarm, and an alarm that was snoozed and then dismissed must not
     * come back. Safe when nothing is pending, which is why the caller
     * does not have to know whether there was.
     */
    (void)nn20clock_timer_cancel_snooze(pthis->timer);
    clear_ring_deadline(pthis);

    if (alarm_id == NN20CLOCK_ALARM_ID_NONE) {
        return;   /* nothing was ringing */
    }

    /*
     * Before the storage work below, and before the caller's
     * apply_state(): the occurrence is already over, and deleting a
     * one-off is bookkeeping that should not decide whether anybody
     * hears about it.
     */
    report_ringing_ended(pthis, alarm_id);
    if (pthis->storage == NULL) {
        return;   /* headless, as in the host tests */
    }

    /*
     * One record, not the whole list: an NN20ClockAlarmList is over 5 KB
     * and this worker's stack is 4 KB. Listing here overflowed it on the
     * board.
     */
    NN20ClockAlarmConfig alarm;
    const esp_err_t found = nn20clock_storage_get_alarm(pthis->storage,
                                                        alarm_id, &alarm);
    if (found == ESP_ERR_NOT_FOUND) {
        /* Deleted or edited while it was ringing. Nothing to do. */
        return;
    }
    if (found != ESP_OK) {
        ESP_LOGE(TAG, "cannot read alarm %u to dismiss it",
                 (unsigned)alarm_id);
        return;
    }
    if (alarm.kind != NN20CLOCK_ALARM_KIND_ONE_OFF) {
        ESP_LOGI(TAG, "alarm %u dismissed", (unsigned)alarm_id);
        return;
    }

    if (nn20clock_storage_delete_alarm(pthis->storage, alarm_id) != ESP_OK) {
        ESP_LOGE(TAG, "could not delete one-off alarm %u",
                 (unsigned)alarm_id);
        return;
    }

    ESP_LOGI(TAG, "one-off alarm %u dismissed and removed",
             (unsigned)alarm_id);
    /* The Timer is holding a schedule that still contains it. */
    (void)nn20clock_timer_reload_schedule(pthis->timer);
}

/* Runs on the manager worker. Milestone 1 has no audio, no alarm, and no
 * brightness control, so most of these are routed nowhere yet - but the
 * routing itself, which is design 8's job, is here and exercised. */
static void apply_command(NN20ClockManager *pthis,
                          const NN20ClockUiCommand *command)
{
    atomic_fetch_add_explicit(&pthis->command_count, 1u, memory_order_relaxed);

    switch (command->type) {
    case NN20CLOCK_UI_COMMAND_OPEN_SETTINGS:
        apply_state(pthis, NN20CLOCK_STATE_ALARM_SETTINGS);
        return;
    case NN20CLOCK_UI_COMMAND_OPEN_DEVICE_SETTINGS:
        apply_state(pthis, NN20CLOCK_STATE_DEVICE_SETTINGS);
        return;
    case NN20CLOCK_UI_COMMAND_CLOSE_SETTINGS:
        apply_state(pthis, NN20CLOCK_STATE_TIME);
        return;
    case NN20CLOCK_UI_COMMAND_OPEN_MEDIA_PLAYBACK:
        apply_state(pthis, NN20CLOCK_STATE_MEDIA_PLAYBACK);
        return;
    case NN20CLOCK_UI_COMMAND_CLOSE_MEDIA_PLAYBACK:
        apply_state(pthis, NN20CLOCK_STATE_TIME);
        return;
    case NN20CLOCK_UI_COMMAND_OPEN_MEDIA_MANAGEMENT:
        apply_state(pthis, NN20CLOCK_STATE_MEDIA_MANAGEMENT);
        return;
    case NN20CLOCK_UI_COMMAND_CLOSE_MEDIA_MANAGEMENT:
        /* Back into the settings menu it was opened from, which is
         * design 5's edge - not to the clock face. */
        apply_state(pthis, NN20CLOCK_STATE_DEVICE_SETTINGS);
        return;
    case NN20CLOCK_UI_COMMAND_STOP_ALARM:
        dismiss_alarm(pthis);
        apply_state(pthis, NN20CLOCK_STATE_TIME);
        return;
    case NN20CLOCK_UI_COMMAND_SNOOZE_ALARM:
        snooze_alarm(pthis);
        apply_state(pthis, NN20CLOCK_STATE_TIME);
        return;
    case NN20CLOCK_UI_COMMAND_CANCEL_SNOOZE: {
        /*
         * The clock face is already showing; there is no state change
         * here, only a scheduled occurrence that stops existing. The
         * Timer is refreshed so the screen hears about it on the next
         * event rather than a minute later.
         */
        /* Read before the cancel, which is what forgets it. */
        const uint32_t snoozed = nn20clock_timer_snoozed_alarm(pthis->timer);
        if (nn20clock_timer_cancel_snooze(pthis->timer) == ESP_OK) {
            ESP_LOGI(TAG, "snooze cancelled");
            /* The occurrence is over, so its hour is too - otherwise
             * tomorrow's alarm would inherit today's deadline. */
            clear_ring_deadline(pthis);
            /* And so is everything anybody was keeping for it: this is
             * the last thing that was going to come back. */
            report_ringing_ended(pthis, snoozed);
            (void)nn20clock_timer_refresh(pthis->timer);
        }
        return;
    }
    case NN20CLOCK_UI_COMMAND_MUTE:
    case NN20CLOCK_UI_COMMAND_UNMUTE:
    case NN20CLOCK_UI_COMMAND_VOLUME_UP:
    case NN20CLOCK_UI_COMMAND_VOLUME_DOWN:
        /* Needs the audio path - Milestone 7. */
        ESP_LOGD(TAG, "command %s: no audio path yet",
                 nn20clock_ui_command_name(command->type));
        return;
    case NN20CLOCK_UI_COMMAND_BRIGHTNESS_UP:
    case NN20CLOCK_UI_COMMAND_BRIGHTNESS_DOWN:
        /* Needs the display backlight and the stored preference -
         * Milestone 5. */
        ESP_LOGD(TAG, "command %s: no brightness control yet",
                 nn20clock_ui_command_name(command->type));
        return;
    }

    ESP_LOGW(TAG, "unknown UI command %d", (int)command->type);
}

/* Runs on the manager worker, one queue hop after the Timer's. */
static void apply_timer_event(NN20ClockManager *pthis,
                              NN20ClockTimerEventType type,
                              const NN20ClockTimerPayload *payload,
                              uint32_t alarm_id)
{
    atomic_fetch_add_explicit(&pthis->timer_event_count, 1u,
                              memory_order_relaxed);

    switch (type) {
    case NN20CLOCK_TIMER_EVENT_SECOND:
    case NN20CLOCK_TIMER_EVENT_MINUTE:
    case NN20CLOCK_TIMER_EVENT_TIME_SYNCED:
        /*
         * Before the screen sees the tick: an alarm that has run out
         * its hour stops here, on this worker, which keeps running
         * whoever owns the panel. That is the whole reason the bound
         * is not on the ringing screen - see
         * NN20CLOCK_MANAGER_MAX_RING_SECONDS.
         *
         * Dismissed rather than snoozed. The alarm has rung itself out
         * to an empty room; a snooze would set the whole thing going
         * again in nine minutes to the same room, and dismissing is
         * also what design 10's one-off lifecycle expects.
         */
        if (payload != NULL &&
            (NN20ClockManagerState)atomic_load_explicit(&pthis->state,
                                                        memory_order_relaxed)
                == NN20CLOCK_STATE_ALARM_RINGING &&
            ring_deadline_passed(pthis, payload->displayed_time)) {
            ESP_LOGW(TAG, "alarm %u rang for %u minutes with no answer; "
                          "stopping",
                     (unsigned)atomic_load_explicit(&pthis->ringing_alarm,
                                                    memory_order_relaxed),
                     (unsigned)(NN20CLOCK_MANAGER_MAX_RING_SECONDS / 60));
            dismiss_alarm(pthis);
            apply_state(pthis, NN20CLOCK_STATE_TIME);
            /* The screen this event was going to is gone; the clock
             * face fills itself in from the refresh apply_screen()
             * asks for. */
            return;
        }

        /* Forward to the active screen. This is the hop design 8 is
         * about: the event arrived on the Timer worker, it is applied
         * here, and the screen sees it on the UiWorker - so nothing
         * reaches LVGL from a timer callback. */
        {
            NN20ClockUiBase *const screen =
                atomic_load_explicit(&pthis->screen, memory_order_relaxed);
            if (screen != NULL) {
                const NN20ClockTimerEvent event = {
                    .type = type,
                    .payload = payload,
                };
                (void)nn20clock_ui_handle_timer_event(screen, &event);
            }
        }
        return;
    case NN20CLOCK_TIMER_EVENT_ALARM:
        /* Milestone 7 resolves the media and builds VideoPlayerUi here,
         * per design 8's five steps. Recording which alarm is ringing
         * and entering the state is what exists today - and the id is
         * what design 10's one-off lifecycle needs when the alarm is
         * later dismissed. */
        atomic_store_explicit(&pthis->ringing_alarm, alarm_id,
                              memory_order_release);
        arm_ring_deadline(pthis, alarm_id,
                          (payload != NULL) ? payload->displayed_time : 0);

        /*
         * A snooze that lands after the hour is up. It is not a new
         * occurrence and it does not get to ring: the alarm ends here
         * instead, silently, which is what the deadline meant.
         */
        if (payload != NULL &&
            ring_deadline_passed(pthis, payload->displayed_time)) {
            ESP_LOGW(TAG, "alarm %u came back past its hour; ending it",
                     (unsigned)alarm_id);
            dismiss_alarm(pthis);
            return;
        }

        atomic_fetch_add_explicit(&pthis->alarm_count, 1u,
                                  memory_order_relaxed);
        ESP_LOGI(TAG, "alarm %u ringing", (unsigned)alarm_id);
        apply_state(pthis, NN20CLOCK_STATE_ALARM_RINGING);
        return;
    case NN20CLOCK_TIMER_EVENT_ERROR:
        apply_state(pthis, NN20CLOCK_STATE_ERROR);
        return;
    }

    ESP_LOGW(TAG, "unknown timer event %d", (int)type);
}

/* The one worker callback: every request kind arrives here in order. */
static int request_private(nn20_worker_ctx *worker, void *user_data)
{
    (void)worker;
    ManagerRequest *request = user_data;
    NN20ClockManager *pthis = request->manager;

    switch (request->kind) {
    case REQUEST_STATE:
        apply_state(pthis, request->state);
        break;
    case REQUEST_SCREEN:
        apply_screen(pthis, request->screen);
        break;
    case REQUEST_COMMAND:
        apply_command(pthis, &request->command);
        break;
    case REQUEST_TIMER_EVENT:
        apply_timer_event(pthis, request->timer_type, &request->payload,
                          request->alarm_id);
        break;
    }

    nn20clock_reqpool_release(&pthis->request_pool, request->slot);
    return 0;
}

/* --------------------------------------------------------- lifecycle -- */

NN20ClockManager *nn20clock_manager_ctor(nn20_worker_ctx *worker,
                                         NN20ClockStorage *storage,
                                         NN20ClockTimer *timer)
{
    if (worker == NULL) {
        ESP_LOGE(TAG, "no ClockManagerWorker (design 4)");
        return NULL;
    }
    if (timer == NULL) {
        ESP_LOGE(TAG, "no Timer");
        return NULL;
    }

    NN20ClockManager *pthis = calloc(1, sizeof(*pthis));
    if (pthis == NULL) {
        ESP_LOGE(TAG, "out of memory");
        return NULL;
    }

    pthis->worker = worker;
    pthis->storage = storage;   /* NULL is allowed until Milestone 3 */
    pthis->timer = timer;

    atomic_init(&pthis->ringing_alarm, NN20CLOCK_ALARM_ID_NONE);
    atomic_init(&pthis->ring_deadline, (time_t)0);
    pthis->ring_deadline_alarm = NN20CLOCK_ALARM_ID_NONE;
    atomic_init(&pthis->alarm_count, 0u);
    atomic_init(&pthis->screen, NULL);
    atomic_init(&pthis->running, false);
    atomic_init(&pthis->state, (int)NN20CLOCK_STATE_BOOT);
    atomic_init(&pthis->rejected_transitions, 0u);
    atomic_init(&pthis->command_count, 0u);
    atomic_init(&pthis->timer_event_count, 0u);

    pthis->subscriber.on_event = nn20clock_manager_on_timer_event;
    pthis->subscriber.user_data = pthis;

    nn20clock_reqpool_init(&pthis->request_pool, pthis->request_in_use,
                           MANAGER_MAX_PENDING);

    if (storage == NULL) {
        ESP_LOGW(TAG, "no Storage; settings are unavailable (Milestone 3)");
    }
    return pthis;
}

/* Empty by design: posted only to be waited on, to flush the queue. */
static int drain_private(nn20_worker_ctx *worker, void *user_data)
{
    (void)worker;
    (void)user_data;
    return 0;
}

void nn20clock_manager_dtor(NN20ClockManager *pthis)
{
    if (pthis == NULL) {
        return;
    }

    if (nn20clock_manager_is_running(pthis)) {
        (void)nn20clock_manager_stop(pthis);
    }

    /* Anything still queued would run against freed memory; the worker
     * is FIFO, so waiting on an empty task means everything posted
     * before it is done. */
    (void)nn20_worker_post_sync(pthis->worker, drain_private, NULL);
    free(pthis);
}

static int start_private(nn20_worker_ctx *worker, void *user_data)
{
    (void)worker;
    NN20ClockManager *pthis = user_data;

    /* Subscribed from the manager worker, not the caller's thread: the
     * Timer's Track 1 contract is that subscribe happens before it
     * starts ticking, and this keeps all of it on one thread. */
    const esp_err_t err = nn20clock_timer_subscribe(pthis->timer,
                                                    &pthis->subscriber);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        /* INVALID_STATE means already subscribed, which is fine. */
        ESP_LOGE(TAG, "timer subscribe failed (0x%x)", (unsigned)err);
        apply_state(pthis, NN20CLOCK_STATE_ERROR);
        return -1;
    }

    atomic_store_explicit(&pthis->running, true, memory_order_release);
    apply_state(pthis, NN20CLOCK_STATE_TIME);   /* design 5: init complete */
    return 0;
}

esp_err_t nn20clock_manager_start(NN20ClockManager *pthis)
{
    if (pthis == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (nn20clock_manager_is_running(pthis)) {
        return ESP_ERR_INVALID_STATE;
    }
    return post_result(
        nn20_worker_post_sync(pthis->worker, start_private, pthis));
}

static int stop_private(nn20_worker_ctx *worker, void *user_data)
{
    (void)worker;
    NN20ClockManager *pthis = user_data;

    (void)nn20clock_timer_unsubscribe(pthis->timer, &pthis->subscriber);
    (void)nn20clock_timer_cancel_snooze(pthis->timer);
    clear_ring_deadline(pthis);
    atomic_store_explicit(&pthis->ringing_alarm, NN20CLOCK_ALARM_ID_NONE,
                          memory_order_release);
    apply_screen(pthis, NULL);   /* destroys the active screen, if any */

    atomic_store_explicit(&pthis->running, false, memory_order_release);
    /* Back to BOOT directly. This is not a design 5 edge - it is the
     * machine being torn down, not transitioning - so it goes around
     * apply_state() rather than being rejected by it. */
    atomic_store_explicit(&pthis->state, (int)NN20CLOCK_STATE_BOOT,
                          memory_order_relaxed);
    ESP_LOGI(TAG, "stopped");
    return 0;
}

esp_err_t nn20clock_manager_stop(NN20ClockManager *pthis)
{
    if (pthis == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!nn20clock_manager_is_running(pthis)) {
        return ESP_ERR_INVALID_STATE;
    }
    return post_result(
        nn20_worker_post_sync(pthis->worker, stop_private, pthis));
}

bool nn20clock_manager_is_running(const NN20ClockManager *pthis)
{
    return pthis != NULL &&
           atomic_load_explicit(&pthis->running, memory_order_acquire);
}

/* ---------------------------------------------------------- states --- */

esp_err_t nn20clock_manager_request_state(NN20ClockManager *pthis,
                                          NN20ClockManagerState state)
{
    if (pthis == NULL || state < 0 || state >= NN20CLOCK_STATE_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    ManagerRequest *request = claim_request(pthis, REQUEST_STATE);
    if (request == NULL) {
        return ESP_ERR_NO_MEM;
    }
    request->state = state;

    const int rc = nn20_worker_post(pthis->worker, request_private, request);
    if (rc != 0) {
        nn20clock_reqpool_release(&pthis->request_pool, request->slot);
        return post_result(rc);
    }
    return ESP_OK;
}

NN20ClockManagerState nn20clock_manager_state(const NN20ClockManager *pthis)
{
    if (pthis == NULL) {
        return NN20CLOCK_STATE_BOOT;
    }
    return (NN20ClockManagerState)atomic_load_explicit(&pthis->state,
                                                       memory_order_relaxed);
}

uint32_t nn20clock_manager_rejected_transitions(const NN20ClockManager *pthis)
{
    if (pthis == NULL) {
        return 0u;
    }
    return atomic_load_explicit(&pthis->rejected_transitions,
                                memory_order_relaxed);
}

esp_err_t nn20clock_manager_set_state_callback(NN20ClockManager *pthis,
                                               NN20ClockManagerStateFn on_state,
                                               void *user_data)
{
    if (pthis == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (nn20clock_manager_is_running(pthis)) {
        /* Would be a write to worker-owned state from another thread.
         * Set it before _start(), where nothing is running yet. */
        return ESP_ERR_INVALID_STATE;
    }

    pthis->on_state = on_state;
    pthis->state_user_data = user_data;
    return ESP_OK;
}

esp_err_t nn20clock_manager_set_ringing_ended_callback(
    NN20ClockManager *pthis, NN20ClockManagerRingingEndedFn on_ringing_ended,
    void *user_data)
{
    if (pthis == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (nn20clock_manager_is_running(pthis)) {
        /* Worker-owned once running, exactly as above. */
        return ESP_ERR_INVALID_STATE;
    }

    pthis->on_ringing_ended = on_ringing_ended;
    pthis->ringing_ended_user_data = user_data;
    return ESP_OK;
}

/* --------------------------------------------------------- screens --- */

esp_err_t nn20clock_manager_set_screen_factory(
    NN20ClockManager *pthis, NN20ClockManagerScreenFn factory,
    void *user_data)
{
    if (pthis == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (nn20clock_manager_is_running(pthis)) {
        /* Worker-owned state; setting it under a running manager would
         * be a write from the wrong thread. */
        return ESP_ERR_INVALID_STATE;
    }

    pthis->screen_factory = factory;
    pthis->screen_factory_user_data = user_data;
    return ESP_OK;
}

esp_err_t nn20clock_manager_set_screen(NN20ClockManager *pthis,
                                       NN20ClockUiBase *screen)
{
    if (pthis == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ManagerRequest *request = claim_request(pthis, REQUEST_SCREEN);
    if (request == NULL) {
        return ESP_ERR_NO_MEM;
    }
    request->screen = screen;

    const int rc = nn20_worker_post(pthis->worker, request_private, request);
    if (rc != 0) {
        nn20clock_reqpool_release(&pthis->request_pool, request->slot);
        return post_result(rc);
    }
    return ESP_OK;
}

NN20ClockUiBase *nn20clock_manager_screen(const NN20ClockManager *pthis)
{
    if (pthis == NULL) {
        return NULL;
    }
    return atomic_load_explicit(&pthis->screen, memory_order_acquire);
}

/* ----------------------------------------------------------- alarms -- */

uint32_t nn20clock_manager_ringing_alarm(const NN20ClockManager *pthis)
{
    if (pthis == NULL) {
        return NN20CLOCK_ALARM_ID_NONE;
    }
    return atomic_load_explicit(&pthis->ringing_alarm, memory_order_acquire);
}

time_t nn20clock_manager_ring_deadline(const NN20ClockManager *pthis)
{
    if (pthis == NULL) {
        return (time_t)0;
    }
    return atomic_load_explicit(&pthis->ring_deadline, memory_order_acquire);
}

uint32_t nn20clock_manager_alarm_count(const NN20ClockManager *pthis)
{
    if (pthis == NULL) {
        return 0u;
    }
    return atomic_load_explicit(&pthis->alarm_count, memory_order_relaxed);
}

/*
 * The three below run on whichever thread called them - in practice the
 * UiWorker, from a settings screen. They touch no manager-owned state,
 * which is what makes that safe: see the threading note on
 * NN20ClockAlarmService.
 */

static esp_err_t alarm_service_list(void *ctx, NN20ClockAlarmList *out_alarms)
{
    NN20ClockManager *pthis = ctx;
    if (pthis->storage == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return nn20clock_storage_list_alarms(pthis->storage, out_alarms);
}

static esp_err_t alarm_service_save(void *ctx,
                                    const NN20ClockAlarmConfig *alarm,
                                    uint32_t *out_id)
{
    NN20ClockManager *pthis = ctx;
    if (pthis->storage == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t err = nn20clock_storage_save_alarm(pthis->storage, alarm,
                                                       out_id);
    if (err != ESP_OK) {
        return err;
    }
    /* The screen cannot forget this: a saved alarm that the Timer has
     * not reloaded is an alarm that will not ring. */
    return nn20clock_timer_reload_schedule(pthis->timer);
}

static esp_err_t alarm_service_remove(void *ctx, uint32_t alarm_id)
{
    NN20ClockManager *pthis = ctx;
    if (pthis->storage == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t err = nn20clock_storage_delete_alarm(pthis->storage,
                                                         alarm_id);
    if (err != ESP_OK) {
        return err;
    }
    return nn20clock_timer_reload_schedule(pthis->timer);
}

NN20ClockAlarmService nn20clock_manager_alarm_service(NN20ClockManager *pthis)
{
    const NN20ClockAlarmService service = {
        .list = alarm_service_list,
        .save = alarm_service_save,
        .remove = alarm_service_remove,
        .ctx = pthis,
    };
    return service;
}

/* ---------------------------------------------------- device service -- */

/*
 * Like the alarm service: called on the UiWorker, touching no
 * manager-owned state, so it never waits on the manager's own worker.
 *
 * The display and network halves live in the firmware build only; on
 * the host they report ESP_ERR_NOT_SUPPORTED, which is the truth there.
 */

static esp_err_t device_service_load(void *ctx, NN20ClockConfig *out_config)
{
    NN20ClockManager *pthis = ctx;
    if (pthis->storage == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return nn20clock_storage_load_config(pthis->storage, out_config);
}

static esp_err_t device_service_save(void *ctx, const NN20ClockConfig *config)
{
    NN20ClockManager *pthis = ctx;
    if (pthis->storage == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return nn20clock_storage_save_config(pthis->storage, config);
}

static esp_err_t device_service_brightness(void *ctx, uint8_t percent)
{
    NN20ClockManager *pthis = ctx;
    if (pthis->device_hooks.apply_brightness == NULL) {
        return ESP_ERR_NOT_SUPPORTED;   /* headless build */
    }
    return pthis->device_hooks.apply_brightness(pthis->device_hooks.ctx,
                                                percent);
}

static uint8_t device_service_current_brightness(void *ctx)
{
    NN20ClockManager *pthis = ctx;
    if (pthis->device_hooks.current_brightness == NULL) {
        return 0u;   /* headless build: there is no panel to report on */
    }
    return pthis->device_hooks.current_brightness(pthis->device_hooks.ctx);
}

static esp_err_t device_service_volume(void *ctx, uint8_t percent)
{
    NN20ClockManager *pthis = ctx;
    if (pthis->device_hooks.apply_volume == NULL) {
        return ESP_ERR_NOT_SUPPORTED;   /* headless build */
    }
    return pthis->device_hooks.apply_volume(pthis->device_hooks.ctx, percent);
}

static esp_err_t device_service_connect(void *ctx, const char *ssid,
                                        const char *password)
{
    NN20ClockManager *pthis = ctx;
    if (pthis->device_hooks.connect_wifi == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return pthis->device_hooks.connect_wifi(pthis->device_hooks.ctx, ssid,
                                            password);
}

static esp_err_t device_service_set_ntp(void *ctx, bool enabled)
{
    NN20ClockManager *pthis = ctx;
    if (pthis->device_hooks.set_ntp == NULL) {
        return ESP_ERR_NOT_SUPPORTED;   /* headless build */
    }
    return pthis->device_hooks.set_ntp(pthis->device_hooks.ctx, enabled);
}

static esp_err_t device_service_set_time(void *ctx, time_t when)
{
    NN20ClockManager *pthis = ctx;
    if (pthis->device_hooks.set_time == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return pthis->device_hooks.set_time(pthis->device_hooks.ctx, when);
}

static esp_err_t device_service_set_timezone(void *ctx, const char *posix_tz)
{
    NN20ClockManager *pthis = ctx;
    if (pthis->timer == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return nn20clock_timer_set_timezone(pthis->timer, posix_tz);
}

esp_err_t nn20clock_manager_set_device_hooks(NN20ClockManager *pthis,
                                             NN20ClockDeviceHooks hooks)
{
    if (pthis == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (nn20clock_manager_is_running(pthis)) {
        return ESP_ERR_INVALID_STATE;
    }

    pthis->device_hooks = hooks;
    return ESP_OK;
}

NN20ClockDeviceService nn20clock_manager_device_service(
    NN20ClockManager *pthis)
{
    const NN20ClockDeviceService service = {
        .load_config = device_service_load,
        .save_config = device_service_save,
        .apply_brightness = device_service_brightness,
        .current_brightness = device_service_current_brightness,
        .apply_volume = device_service_volume,
        .connect_wifi = device_service_connect,
        .set_ntp = device_service_set_ntp,
        .set_time = device_service_set_time,
        .set_timezone = device_service_set_timezone,
        .ctx = pthis,
    };
    return service;
}

/* --------------------------------------------------- media playback -- */

/*
 * Called on the UiWorker by MediaPlaybackUi, and like the other
 * services it goes straight to the thing that can do the work rather
 * than through the manager's own worker - which is what keeps the
 * UiWorker from ever waiting on the manager.
 */
static esp_err_t media_service_play(void *ctx, const char *relative_path,
                                    uint32_t sleep_ms)
{
    NN20ClockManager *pthis = ctx;

    if (relative_path == NULL || relative_path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (pthis->media_hooks.play_media == NULL) {
        return ESP_ERR_NOT_SUPPORTED;   /* headless build */
    }
    return pthis->media_hooks.play_media(pthis->media_hooks.ctx,
                                         relative_path, sleep_ms);
}

/*
 * The same, for a folder rather than a file.
 *
 * An empty folder_path is the root folder and is therefore valid, which is
 * the one place this differs from media_service_play() - there, an
 * empty string is a file with no name.
 */
static esp_err_t media_service_play_random(void *ctx, const char *folder_path,
                                           uint32_t sleep_ms)
{
    NN20ClockManager *pthis = ctx;

    if (folder_path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (pthis->media_hooks.play_random_media == NULL) {
        return ESP_ERR_NOT_SUPPORTED;   /* headless build */
    }
    return pthis->media_hooks.play_random_media(pthis->media_hooks.ctx,
                                                folder_path, sleep_ms);
}

esp_err_t nn20clock_manager_set_media_hooks(NN20ClockManager *pthis,
                                            NN20ClockMediaHooks hooks)
{
    if (pthis == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (nn20clock_manager_is_running(pthis)) {
        return ESP_ERR_INVALID_STATE;
    }

    pthis->media_hooks = hooks;
    return ESP_OK;
}

NN20ClockMediaService nn20clock_manager_media_service(NN20ClockManager *pthis)
{
    const NN20ClockMediaService service = {
        .play = media_service_play,
        .play_random = media_service_play_random,
        .ctx = pthis,
    };
    return service;
}

esp_err_t nn20clock_manager_reload_alarms(NN20ClockManager *pthis)
{
    if (pthis == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    /*
     * Straight through to the Timer rather than hopping via the manager
     * worker: reload_schedule is itself synchronous and reads storage,
     * neither of which touches manager-owned state. Going through the
     * worker would only add a hop - and a manager worker blocked on the
     * Timer while the Timer is trying to deliver an event to it is the
     * one shape worth avoiding here.
     */
    return nn20clock_timer_reload_schedule(pthis->timer);
}

/* -------------------------------------------------------- commands --- */

esp_err_t nn20clock_manager_handle_ui_command(
    NN20ClockManager *manager, const NN20ClockUiCommand *command)
{
    if (manager == NULL || command == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ManagerRequest *request = claim_request(manager, REQUEST_COMMAND);
    if (request == NULL) {
        return ESP_ERR_NO_MEM;
    }
    request->command = *command;

    const int rc = nn20_worker_post(manager->worker, request_private, request);
    if (rc != 0) {
        nn20clock_reqpool_release(&manager->request_pool, request->slot);
        return post_result(rc);
    }
    return ESP_OK;
}

uint32_t nn20clock_manager_command_count(const NN20ClockManager *pthis)
{
    if (pthis == NULL) {
        return 0u;
    }
    return atomic_load_explicit(&pthis->command_count, memory_order_relaxed);
}

/* ------------------------------------------------------ timer events -- */

void nn20clock_manager_on_timer_event(const NN20ClockTimerEvent *event,
                                      void *user_data)
{
    NN20ClockManager *pthis = user_data;
    if (pthis == NULL || event == NULL) {
        return;
    }

    /*
     * On the Timer worker. Design 6 allows exactly this much here: copy
     * and hand over. No state is read, nothing is decided, and above all
     * nothing touches the display.
     */
    ManagerRequest *request = claim_request(pthis, REQUEST_TIMER_EVENT);
    if (request == NULL) {
        /* The manager is behind. Dropping a tick beats stalling the
         * Timer, which still has alarms to check. */
        return;
    }

    request->timer_type = event->type;
    if (event->payload != NULL) {
        request->payload = *event->payload;

        /* An alarm event's payload derives from the base (design 6), so
         * the extra fields are only there for this type - reading them
         * on any other event would be reading past the struct. */
        if (event->type == NN20CLOCK_TIMER_EVENT_ALARM) {
            const NN20ClockTimerAlarmPayload *alarm =
                (const NN20ClockTimerAlarmPayload *)event->payload;
            request->alarm_id = alarm->alarm_id;
            request->alarm_late_seconds = alarm->late_seconds;
        }
    }

    if (nn20_worker_post(pthis->worker, request_private, request) != 0) {
        nn20clock_reqpool_release(&pthis->request_pool, request->slot);
    }
}

uint32_t nn20clock_manager_timer_event_count(const NN20ClockManager *pthis)
{
    if (pthis == NULL) {
        return 0u;
    }
    return atomic_load_explicit(&pthis->timer_event_count,
                                memory_order_relaxed);
}
