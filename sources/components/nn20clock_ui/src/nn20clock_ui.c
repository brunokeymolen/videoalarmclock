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
 * nn20clock_ui.c - UiBase dispatch onto the shared UiWorker (design 11).
 *
 * Every function here divides into two halves. The public half runs on
 * the caller's thread, copies whatever the hook will need, and posts.
 * The `_private` half runs on the UiWorker and is the only code allowed
 * to call into a screen - and, from Milestone 2, into LVGL.
 *
 * No mutex. `visible` and everything a screen owns belong to the
 * UiWorker thread, and the queue orders the accesses.
 */
#include "nn20clock_ui.h"

#include <string.h>

static const char *TAG = "NN20CLOCK_UI";

/* Same mapping as Storage's: 1 is a full queue (backpressure), -1 is a
 * worker on its way out. */
static esp_err_t post_result(int rc)
{
    if (rc == 0) {
        return ESP_OK;
    }
    return (rc == 1) ? ESP_ERR_TIMEOUT : ESP_ERR_INVALID_STATE;
}

/* --------------------------------------------------------- lifecycle -- */

esp_err_t nn20clock_ui_base_init(NN20ClockUiBase *base,
                                 const NN20ClockUiBaseConfig *config)
{
    if (base == NULL || config == NULL || config->vtable == NULL ||
        config->name == NULL || config->worker == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(base, 0, sizeof(*base));
    base->vtable = config->vtable;
    base->name = config->name;
    base->worker = config->worker;
    base->manager = config->manager;
    base->on_command = config->on_command;
    atomic_init(&base->visible, false);

    nn20clock_reqpool_init(&base->dispatch_pool, base->dispatch_in_use,
                           NN20CLOCK_UI_MAX_PENDING);

    ESP_LOGD(TAG, "%s initialized", base->name);
    return ESP_OK;
}

void nn20clock_ui_base_deinit(NN20ClockUiBase *base)
{
    if (base == NULL) {
        return;
    }

    /* The screen is about to be freed; a dispatch still queued would
     * reach a dead object. This runs on the UiWorker (see
     * nn20clock_ui_destroy), so everything posted before it has already
     * been executed - there is nothing left to drain, only to check. */
    const size_t in_flight = nn20clock_reqpool_in_flight(&base->dispatch_pool);
    if (in_flight != 0) {
        ESP_LOGW(TAG, "%s: %u dispatch(es) still in flight at deinit",
                 base->name, (unsigned)in_flight);
    }

    base->vtable = NULL;
    base->worker = NULL;
    base->manager = NULL;
    base->on_command = NULL;
}

static int destroy_private(nn20_worker_ctx *worker, void *user_data)
{
    (void)worker;
    NN20ClockUiBase *base = user_data;

    /* Hide first: a screen should not have to handle being destroyed
     * while it still believes it is on the display. */
    if (atomic_load_explicit(&base->visible, memory_order_relaxed) &&
        base->vtable->hide != NULL) {
        (void)base->vtable->hide(base);
        atomic_store_explicit(&base->visible, false, memory_order_release);
    }

    if (base->vtable->destroy != NULL) {
        base->vtable->destroy(base);   /* frees the screen, base included */
    }
    return 0;
}

void nn20clock_ui_destroy(NN20ClockUiBase *base)
{
    if (base == NULL) {
        return;
    }

    /*
     * Synchronous on purpose. ClockManager destroys the old screen
     * before building the next one (design 8), and it can only know the
     * display is free once the old screen's LVGL objects are actually
     * gone. Posting and hoping would let two screens exist at once.
     */
    nn20_worker_ctx *const worker = base->worker;
    const int rc = nn20_worker_post_sync(worker, destroy_private, base);
    if (rc != 0) {
        /* The worker is stopping or full, so nothing will run the hook.
         * Run it here rather than leaking the screen: the UI thread is
         * on its way out, so there is no one left to race with. */
        ESP_LOGW(TAG, "destroy could not reach the UiWorker (%d); "
                      "running inline", rc);
        (void)destroy_private(NULL, base);
    }
}

/* --------------------------------------------------------- dispatch --- */

/* show/hide need no payload, so the base pointer is the whole request
 * and no slot is claimed. */

static int show_private(nn20_worker_ctx *worker, void *user_data)
{
    (void)worker;
    NN20ClockUiBase *base = user_data;

    if (base->vtable->show == NULL) {
        atomic_store_explicit(&base->visible, true, memory_order_release);
        return 0;
    }

    const esp_err_t err = base->vtable->show(base);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s: show failed (0x%x)", base->name, (unsigned)err);
        return -1;
    }
    atomic_store_explicit(&base->visible, true, memory_order_release);
    return 0;
}

static int hide_private(nn20_worker_ctx *worker, void *user_data)
{
    (void)worker;
    NN20ClockUiBase *base = user_data;

    if (base->vtable->hide != NULL) {
        const esp_err_t err = base->vtable->hide(base);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "%s: hide failed (0x%x)", base->name, (unsigned)err);
            atomic_store_explicit(&base->visible, false, memory_order_release);
            return -1;
        }
    }
    atomic_store_explicit(&base->visible, false, memory_order_release);
    return 0;
}

esp_err_t nn20clock_ui_show(NN20ClockUiBase *base)
{
    if (base == NULL || base->worker == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return post_result(nn20_worker_post(base->worker, show_private, base));
}

esp_err_t nn20clock_ui_hide(NN20ClockUiBase *base)
{
    if (base == NULL || base->worker == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return post_result(nn20_worker_post(base->worker, hide_private, base));
}

/* Claim a dispatch slot and fill in the part every dispatch shares.
 * Returns NULL when the pool is full. */
static NN20ClockUiDispatch *claim_dispatch(NN20ClockUiBase *base)
{
    const size_t slot = nn20clock_reqpool_claim(&base->dispatch_pool);
    if (slot == NN20CLOCK_UI_MAX_PENDING) {
        ESP_LOGW(TAG, "%s: all %d dispatch slots in flight", base->name,
                 NN20CLOCK_UI_MAX_PENDING);
        return NULL;
    }

    NN20ClockUiDispatch *dispatch = &base->dispatches[slot];
    memset(dispatch, 0, sizeof(*dispatch));
    dispatch->base = base;
    dispatch->slot = slot;
    return dispatch;
}

static int touch_private(nn20_worker_ctx *worker, void *user_data)
{
    (void)worker;
    NN20ClockUiDispatch *dispatch = user_data;
    NN20ClockUiBase *base = dispatch->base;

    esp_err_t err = ESP_OK;
    if (base->vtable->handle_touch != NULL) {
        err = base->vtable->handle_touch(base, &dispatch->touch);
    }

    nn20clock_reqpool_release(&base->dispatch_pool, dispatch->slot);
    return (err == ESP_OK) ? 0 : -1;
}

esp_err_t nn20clock_ui_handle_touch(NN20ClockUiBase *base,
                                    const NN20ClockTouchEvent *event)
{
    if (base == NULL || base->worker == NULL || event == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    NN20ClockUiDispatch *dispatch = claim_dispatch(base);
    if (dispatch == NULL) {
        /* Dropping a touch is the right failure here: the panel will
         * send another one, and queueing stale input is worse. */
        return ESP_ERR_NO_MEM;
    }
    dispatch->touch = *event;

    const int rc = nn20_worker_post(base->worker, touch_private, dispatch);
    if (rc != 0) {
        nn20clock_reqpool_release(&base->dispatch_pool, dispatch->slot);
        return post_result(rc);
    }
    return ESP_OK;
}

static int timer_event_private(nn20_worker_ctx *worker, void *user_data)
{
    (void)worker;
    NN20ClockUiDispatch *dispatch = user_data;
    NN20ClockUiBase *base = dispatch->base;

    esp_err_t err = ESP_OK;
    if (base->vtable->handle_timer_event != NULL) {
        /*
         * Rebuilt from the copy, so the screen sees the same shape the
         * Timer emitted - but pointing at storage that is ours. The
         * union's base member is at offset zero of every alternative,
         * which is exactly the property design 6 requires of derived
         * payloads, so this pointer is valid whichever one was copied.
         */
        const NN20ClockTimerEvent event = {
            .type = dispatch->timer_type,
            .payload = &dispatch->payload.base,
        };
        err = base->vtable->handle_timer_event(base, &event);
    }

    nn20clock_reqpool_release(&base->dispatch_pool, dispatch->slot);
    return (err == ESP_OK) ? 0 : -1;
}

esp_err_t nn20clock_ui_handle_timer_event(NN20ClockUiBase *base,
                                          const NN20ClockTimerEvent *event)
{
    if (base == NULL || base->worker == NULL || event == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    NN20ClockUiDispatch *dispatch = claim_dispatch(base);
    if (dispatch == NULL) {
        /* At 1 Hz this should never happen; if it does, the UI worker is
         * badly behind and dropping a tick is the least of it. */
        return ESP_ERR_NO_MEM;
    }

    dispatch->timer_type = event->type;
    if (event->payload != NULL) {
        /* Copy the whole derived payload for the types that have one;
         * the base alone would lose the extra fields. */
        if (event->type == NN20CLOCK_TIMER_EVENT_ALARM) {
            dispatch->payload.alarm =
                *(const NN20ClockTimerAlarmPayload *)event->payload;
        } else {
            dispatch->payload.base = *event->payload;
        }
    }

    const int rc = nn20_worker_post(base->worker, timer_event_private,
                                    dispatch);
    if (rc != 0) {
        nn20clock_reqpool_release(&base->dispatch_pool, dispatch->slot);
        return post_result(rc);
    }
    return ESP_OK;
}

esp_err_t nn20clock_ui_send_command(NN20ClockUiBase *base,
                                    const NN20ClockUiCommand *command)
{
    if (base == NULL || command == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (base->manager == NULL || base->on_command == NULL) {
        /* No manager wired up - a screen under test, or a screen built
         * before the manager exists. Say so rather than crash. */
        ESP_LOGD(TAG, "%s: command %s dropped, no manager", base->name,
                 nn20clock_ui_command_name(command->type));
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGD(TAG, "%s: command %s", base->name,
             nn20clock_ui_command_name(command->type));
    return base->on_command(base->manager, command);
}

/* --------------------------------------------------------- accessors -- */

bool nn20clock_ui_is_visible(const NN20ClockUiBase *base)
{
    return base != NULL &&
           atomic_load_explicit(&base->visible, memory_order_acquire);
}

const char *nn20clock_ui_name(const NN20ClockUiBase *base)
{
    return (base != NULL && base->name != NULL) ? base->name : "(none)";
}

nn20_worker_ctx *nn20clock_ui_worker(const NN20ClockUiBase *base)
{
    return (base != NULL) ? base->worker : NULL;
}

NN20ClockManager *nn20clock_ui_manager(const NN20ClockUiBase *base)
{
    return (base != NULL) ? base->manager : NULL;
}

const char *nn20clock_ui_command_name(NN20ClockUiCommandType type)
{
    switch (type) {
    case NN20CLOCK_UI_COMMAND_MUTE:             return "MUTE";
    case NN20CLOCK_UI_COMMAND_UNMUTE:           return "UNMUTE";
    case NN20CLOCK_UI_COMMAND_STOP_ALARM:       return "STOP_ALARM";
    case NN20CLOCK_UI_COMMAND_SNOOZE_ALARM:     return "SNOOZE_ALARM";
    case NN20CLOCK_UI_COMMAND_CANCEL_SNOOZE:    return "CANCEL_SNOOZE";
    case NN20CLOCK_UI_COMMAND_VOLUME_UP:        return "VOLUME_UP";
    case NN20CLOCK_UI_COMMAND_VOLUME_DOWN:      return "VOLUME_DOWN";
    case NN20CLOCK_UI_COMMAND_BRIGHTNESS_UP:    return "BRIGHTNESS_UP";
    case NN20CLOCK_UI_COMMAND_BRIGHTNESS_DOWN:  return "BRIGHTNESS_DOWN";
    case NN20CLOCK_UI_COMMAND_OPEN_SETTINGS:    return "OPEN_SETTINGS";
    case NN20CLOCK_UI_COMMAND_OPEN_DEVICE_SETTINGS:
        return "OPEN_DEVICE_SETTINGS";
    case NN20CLOCK_UI_COMMAND_CLOSE_SETTINGS:   return "CLOSE_SETTINGS";
    case NN20CLOCK_UI_COMMAND_OPEN_MEDIA_PLAYBACK:
        return "OPEN_MEDIA_PLAYBACK";
    case NN20CLOCK_UI_COMMAND_CLOSE_MEDIA_PLAYBACK:
        return "CLOSE_MEDIA_PLAYBACK";
    case NN20CLOCK_UI_COMMAND_OPEN_MEDIA_MANAGEMENT:
        return "OPEN_MEDIA_MANAGEMENT";
    case NN20CLOCK_UI_COMMAND_CLOSE_MEDIA_MANAGEMENT:
        return "CLOSE_MEDIA_MANAGEMENT";
    }
    return "UNKNOWN";
}
