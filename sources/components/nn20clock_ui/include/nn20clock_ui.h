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
 * nn20clock_ui.h - UiBase, the common base of every screen (design 11).
 *
 * MILESTONE 1 SKELETON. The base class, its vtable, the UI command path
 * back into ClockManager, and the UiWorker seam are real. There is no
 * LVGL here yet and no concrete screen - TimeUi arrives at Milestone 2,
 * AlarmSettingsUi at 4, VideoPlayerUi at 6.
 *
 * Threading (design 4, 11): all LVGL work runs on the one UiWorker,
 * created by NN20ClockWorkers and handed to every screen through
 * UiBase. No other thread - not the Timer's, not the manager's, not the
 * storage worker's - may touch LVGL. That is why the vtable is never
 * called directly: nn20clock_ui_show() and friends post the call to the
 * UiWorker, so a screen's methods always run on the right thread no
 * matter who asked.
 *
 * There are no mutexes here and there must never be one. LVGL is not
 * thread-safe, and the answer to that is single-threaded ownership, not
 * a lock around it: a lock would let two threads take turns inside LVGL,
 * which is exactly what the design forbids.
 *
 * A concrete screen embeds UiBase as its first member:
 *
 *     typedef struct {
 *         NN20ClockUiBase super;
 *         lv_obj_t *label;
 *     } TimeUi;
 *
 * so a NN20ClockUiBase* can be cast to the screen and back.
 */
#ifndef NN20CLOCK_UI_H
#define NN20CLOCK_UI_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nn20clock_platform.h"
#include "nn20clock_reqpool.h"
#include "nn20clock_timer.h"
#include "worker.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NN20ClockUiBase NN20ClockUiBase;

/*
 * Forward-declared, not included: UiBase holds a ClockManager pointer
 * and calls back into it, while ClockManager includes this header to
 * build screens. Including each other's headers would be a cycle, and
 * the manager is opaque to the UI anyway - a screen may only reach it
 * through the command callback below.
 */
typedef struct NN20ClockManager NN20ClockManager;

/* ----------------------------------------------------------- input --- */

/*
 * Touch, from design 11's handle_touch hook. Coordinates are in display
 * pixels, origin top-left, so 0..719 on this 720x720 panel.
 */
typedef enum {
    NN20CLOCK_TOUCH_PRESS,
    NN20CLOCK_TOUCH_MOVE,
    NN20CLOCK_TOUCH_RELEASE
} NN20ClockTouchType;

typedef struct {
    NN20ClockTouchType type;
    uint16_t x;
    uint16_t y;
} NN20ClockTouchEvent;

/* -------------------------------------------------------- commands --- */

/*
 * What a screen may ask the application to do (design 11). A screen
 * consumes its own input locally where it can; anything touching a
 * shared service - audio, the alarm scheduler, stored settings,
 * brightness policy - goes through here so ClockManager can triage it.
 * Screens own none of those.
 */
typedef enum {
    NN20CLOCK_UI_COMMAND_MUTE,
    NN20CLOCK_UI_COMMAND_UNMUTE,
    NN20CLOCK_UI_COMMAND_STOP_ALARM,
    NN20CLOCK_UI_COMMAND_SNOOZE_ALARM,
    /* Give up on a snooze that is already waiting: the alarm is over
     * rather than resting, and must not come back. */
    NN20CLOCK_UI_COMMAND_CANCEL_SNOOZE,
    NN20CLOCK_UI_COMMAND_VOLUME_UP,
    NN20CLOCK_UI_COMMAND_VOLUME_DOWN,
    NN20CLOCK_UI_COMMAND_BRIGHTNESS_UP,
    NN20CLOCK_UI_COMMAND_BRIGHTNESS_DOWN,
    NN20CLOCK_UI_COMMAND_OPEN_SETTINGS,
    NN20CLOCK_UI_COMMAND_OPEN_DEVICE_SETTINGS,
    NN20CLOCK_UI_COMMAND_CLOSE_SETTINGS,
    /* Media the user chose to watch, rather than an alarm (design 17,
     * Milestone 8). Separate from CLOSE_SETTINGS, which happens to lead
     * to the same state: closing a settings screen and stopping a film
     * are different events, and only one of them will still mean
     * "return to the clock" once there is more than one way out. */
    NN20CLOCK_UI_COMMAND_OPEN_MEDIA_PLAYBACK,
    NN20CLOCK_UI_COMMAND_CLOSE_MEDIA_PLAYBACK,
    /* The card and the FTP server (design 17, Milestone 10). Reached
     * from DeviceSettingsUi and closing back into it, which is why
     * these are their own pair rather than OPEN/CLOSE_SETTINGS: closing
     * this one returns to the settings menu, not to the clock. */
    NN20CLOCK_UI_COMMAND_OPEN_MEDIA_MANAGEMENT,
    NN20CLOCK_UI_COMMAND_CLOSE_MEDIA_MANAGEMENT
} NN20ClockUiCommandType;

typedef struct {
    NN20ClockUiCommandType type;
    int value;   /* command-specific; 0 when unused */
} NN20ClockUiCommand;

/*
 * Called on the UiWorker thread, from whichever screen raised the
 * command. Implementations must not block: ClockManager's version posts
 * the command to its own worker and returns.
 */
typedef esp_err_t (*NN20ClockUiCommandFn)(NN20ClockManager *manager,
                                          const NN20ClockUiCommand *command);

/* ---------------------------------------------------------- vtable --- */

/*
 * Every hook runs on the UiWorker thread. None of them may block: the
 * whole display pipeline is behind this one thread.
 *
 * Any hook may be NULL, in which case the dispatcher treats the call as
 * a no-op returning ESP_OK - a screen with nothing to do on hide should
 * not have to write an empty function.
 */
typedef struct {
    esp_err_t (*show)(NN20ClockUiBase *ui);
    esp_err_t (*hide)(NN20ClockUiBase *ui);
    esp_err_t (*handle_touch)(NN20ClockUiBase *ui,
                              const NN20ClockTouchEvent *event);
    esp_err_t (*handle_timer_event)(NN20ClockUiBase *ui,
                                    const NN20ClockTimerEvent *event);
    /* Releases the screen's own resources. UiBase's members are cleaned
     * up by nn20clock_ui_base_deinit(), which the screen's destroy must
     * call before freeing itself. */
    void (*destroy)(NN20ClockUiBase *ui);
} NN20ClockUiVTable;

/*
 * A dispatch in flight. The event a hook is called with lives only for
 * the duration of the caller's callback, so it is copied in here and the
 * copy is what reaches the UiWorker.
 *
 * The payload is a union of every shape design 6 defines, not just the
 * base. Copying only the base would silently drop an alarm event's id
 * on the way to the screen - the sort of loss that produces a screen
 * showing the wrong alarm rather than an error.
 *
 * Every new derived payload must be added here. The union is sized by
 * its largest member, so this is the one place that needs to know they
 * exist.
 */
typedef union {
    NN20ClockTimerPayload base;
    NN20ClockTimerAlarmPayload alarm;
} NN20ClockUiPayload;

typedef struct {
    NN20ClockUiBase *base;
    NN20ClockTimerEventType timer_type;
    NN20ClockUiPayload payload;
    NN20ClockTouchEvent touch;
    size_t slot;
} NN20ClockUiDispatch;

/* Dispatches in flight per screen. Touch arrives at up to the panel's
 * report rate and timer events at 1 Hz, both far slower than the UI
 * worker drains them; four is headroom, not a budget. */
#define NN20CLOCK_UI_MAX_PENDING 4

/*
 * Deliberately public: concrete screens embed this struct, so its size
 * has to be known at their compile time. Treat the fields as private -
 * use the accessors.
 */
struct NN20ClockUiBase {
    const NN20ClockUiVTable *vtable;   /* borrowed; normally static const */
    const char *name;                  /* for logs */

    nn20_worker_ctx *worker;           /* the shared UiWorker; borrowed */
    NN20ClockManager *manager;         /* borrowed; may be NULL in tests */
    NN20ClockUiCommandFn on_command;   /* may be NULL */

    /* Written only on the UiWorker. Atomic so nn20clock_ui_is_visible()
     * from another thread is a defined snapshot rather than a data race
     * - not a lock, and not something to make a decision on off the
     * worker. */
    atomic_bool visible;

    /* Claimed from any thread, read on the UiWorker. See
     * nn20clock_reqpool.h - the claim is one atomic exchange, never a
     * lock. */
    NN20ClockUiDispatch dispatches[NN20CLOCK_UI_MAX_PENDING];
    atomic_bool dispatch_in_use[NN20CLOCK_UI_MAX_PENDING];
    NN20ClockReqPool dispatch_pool;
};

typedef struct {
    const NN20ClockUiVTable *vtable;   /* required */
    const char *name;                  /* required */
    nn20_worker_ctx *worker;           /* required: the shared UiWorker */
    NN20ClockManager *manager;         /* may be NULL */
    NN20ClockUiCommandFn on_command;   /* may be NULL */
} NN20ClockUiBaseConfig;

/* --------------------------------------------------------- lifecycle -- */

/*
 * Initialize the embedded base. A concrete screen's constructor calls
 * this first, then sets up its own members.
 *
 * ESP_ERR_INVALID_ARG if base, config, vtable, name, or worker is NULL.
 */
esp_err_t nn20clock_ui_base_init(NN20ClockUiBase *base,
                                 const NN20ClockUiBaseConfig *config);

/*
 * Release the base's own state. A concrete screen's destroy hook calls
 * this last, just before freeing itself. Safe with NULL.
 */
void nn20clock_ui_base_deinit(NN20ClockUiBase *base);

/*
 * Run the screen's destroy hook on the UiWorker and wait for it, then
 * the screen is gone. Callable from any thread; safe with NULL.
 *
 * This is what ClockManager calls when switching screens - design 8
 * requires the old UI to be destroyed before the next one is built.
 */
void nn20clock_ui_destroy(NN20ClockUiBase *base);

/* --------------------------------------------------------- dispatch --- */

/*
 * All four post to the UiWorker, so they are safe to call from any
 * thread - including from a Timer subscriber callback, which is the
 * whole reason they exist.
 *
 * They report whether the call was *dispatched*, not what the screen
 * returned: ESP_OK when the hook was queued, ESP_ERR_TIMEOUT when the UI
 * queue is full, ESP_ERR_INVALID_STATE when the worker is stopping. A
 * hook returning non-zero is logged and counted in the worker's `failed`
 * statistic.
 *
 * Called from the UiWorker itself, the hook still runs in order behind
 * whatever is already queued - it is not run inline.
 */
esp_err_t nn20clock_ui_show(NN20ClockUiBase *base);
esp_err_t nn20clock_ui_hide(NN20ClockUiBase *base);
esp_err_t nn20clock_ui_handle_touch(NN20ClockUiBase *base,
                                    const NN20ClockTouchEvent *event);
esp_err_t nn20clock_ui_handle_timer_event(NN20ClockUiBase *base,
                                          const NN20ClockTimerEvent *event);

/*
 * Raise a command toward ClockManager. Called from a screen's own hooks,
 * so already on the UiWorker; it does not post again.
 *
 * ESP_ERR_INVALID_STATE when no manager or callback was supplied, which
 * is the normal case in a unit test.
 */
esp_err_t nn20clock_ui_send_command(NN20ClockUiBase *base,
                                    const NN20ClockUiCommand *command);

/* --------------------------------------------------------- accessors -- */

/*
 * A snapshot. Written only on the UiWorker, so a reader on another
 * thread may see it change the moment after it looks; fine for logs and
 * tests, not for deciding anything.
 */
bool nn20clock_ui_is_visible(const NN20ClockUiBase *base);

const char *nn20clock_ui_name(const NN20ClockUiBase *base);
nn20_worker_ctx *nn20clock_ui_worker(const NN20ClockUiBase *base);
NN20ClockManager *nn20clock_ui_manager(const NN20ClockUiBase *base);

/* "MUTE", "STOP_ALARM", ... - for logs and test failures. */
const char *nn20clock_ui_command_name(NN20ClockUiCommandType type);

#ifdef __cplusplus
}
#endif

#endif /* NN20CLOCK_UI_H */
