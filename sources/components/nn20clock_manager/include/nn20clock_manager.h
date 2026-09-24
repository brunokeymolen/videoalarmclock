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
 * nn20clock_manager.h - the ClockManager (design 5, 8).
 *
 * MILESTONE 1 SKELETON. The state machine, the transition rules, the
 * timer subscription, the screen switch, and the UI command entry point
 * are real and tested. What they drive is not: there are no screens to
 * switch to yet, no alarm to ring, and the Timer does not tick. The
 * command handlers log and return ESP_ERR_NOT_SUPPORTED where the
 * service behind them does not exist.
 *
 * Threading (design 4, 8). The manager owns its state on the
 * ClockManagerWorker and nothing else touches it. Three threads talk to
 * it, and all three only post:
 *
 *   Timer worker  -> a subscriber callback copies the event and posts.
 *                    Design 6 forbids doing anything else there, and
 *                    design 8 forbids letting a timer callback reach
 *                    LVGL.
 *   UiWorker      -> a screen raises a command; it is posted here.
 *   CoreWorker    -> start, stop, and state requests.
 *
 * The only synchronous cross-worker call in the other direction is
 * destroying a screen, which waits on the UiWorker (see design 8: the
 * old UI must be gone before the next one is built). Nothing on the
 * UiWorker ever waits on this worker, so the two cannot deadlock - the
 * one rule in the worker README that has to be enforced by hand.
 *
 * There are no mutexes in this component. The queue is the
 * synchronization; a lock here would mean state escaped its worker.
 */
#ifndef NN20CLOCK_MANAGER_H
#define NN20CLOCK_MANAGER_H

#include <stdbool.h>
#include <time.h>
#include <stdint.h>

#include "nn20clock_alarm.h"
#include "nn20clock_platform.h"
#include "nn20clock_storage.h"
#include "nn20clock_timer.h"
#include "nn20clock_ui.h"
#include "worker.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Declared in nn20clock_ui.h, which UiBase needs without knowing what a
 * manager is; repeated here so this header stands alone. */
typedef struct NN20ClockManager NN20ClockManager;

/* ----------------------------------------------------------- states -- */

/* Design 5. NN20CLOCK_STATE_TIME is the normal running state; design 10
 * notes it may be renamed RUN later. */
typedef enum {
    NN20CLOCK_STATE_BOOT,
    NN20CLOCK_STATE_TIME,
    NN20CLOCK_STATE_ALARM_SETTINGS,
    NN20CLOCK_STATE_DEVICE_SETTINGS,
    NN20CLOCK_STATE_ALARM_RINGING,
    NN20CLOCK_STATE_MEDIA_PLAYBACK,
    NN20CLOCK_STATE_MEDIA_MANAGEMENT,
    NN20CLOCK_STATE_ERROR,
    NN20CLOCK_STATE_COUNT
} NN20ClockManagerState;

/* "BOOT", "TIME", ... - for logs and test failures. */
const char *nn20clock_manager_state_name(NN20ClockManagerState state);

/*
 * Whether design 5 allows this edge. Pure and side-effect free, so the
 * transition table can be tested without building a manager.
 */
bool nn20clock_manager_transition_allowed(NN20ClockManagerState from,
                                          NN20ClockManagerState to);

/*
 * Called after every accepted transition, on the ClockManagerWorker
 * thread. For tests, logging, and (later) the screen switch. Keep it
 * short and post elsewhere for real work.
 */
typedef void (*NN20ClockManagerStateFn)(NN20ClockManagerState from,
                                        NN20ClockManagerState to,
                                        void *user_data);

/*
 * Called when an alarm occurrence has ended for good, on the
 * ClockManagerWorker thread, with the id of the alarm that was ringing.
 *
 * "For good" is the distinction a state change cannot make. All three
 * of these end an occurrence and all three report here:
 *
 *   - somebody pressed Off,
 *   - it rang its hour out to an empty room (the ring deadline),
 *   - a snooze came back past that hour and was ended without ringing,
 *   - a pending snooze was cancelled from the clock face.
 *
 * The last two are not state changes at all, because the clock face was
 * already showing - which is why this exists rather than the
 * ALARM_RINGING edge being read for it.
 *
 * A snooze does not report: it interrupts the occurrence rather than
 * ending it, and whatever the subscriber keeps for the occurrence - the
 * film a <random> alarm rolled, the position the player is holding - is
 * exactly what the snooze is coming back for.
 *
 * Keep it short and post elsewhere for real work, as with the state
 * callback.
 */
typedef void (*NN20ClockManagerRingingEndedFn)(uint32_t alarm_id,
                                               void *user_data);

/* --------------------------------------------------------- lifecycle -- */

/*
 * worker, storage, and timer are all borrowed and must outlive the
 * manager. worker and timer are required; storage may be NULL until
 * Milestone 3 gives the manager something to load.
 *
 * The manager starts in NN20CLOCK_STATE_BOOT.
 */
NN20ClockManager *nn20clock_manager_ctor(nn20_worker_ctx *worker,
                                         NN20ClockStorage *storage,
                                         NN20ClockTimer *timer);

/* Stops first if still running. Safe with NULL. */
void nn20clock_manager_dtor(NN20ClockManager *pthis);

/*
 * Subscribe to the Timer and leave BOOT for TIME - design 5's "init
 * complete" edge. Blocks until the transition has been applied on the
 * manager worker, so a caller that returns from this knows the machine
 * is running.
 */
esp_err_t nn20clock_manager_start(NN20ClockManager *pthis);

/*
 * Unsubscribe from the Timer, drop the active screen, and return to
 * BOOT. Blocking, as above. Idempotent.
 */
esp_err_t nn20clock_manager_stop(NN20ClockManager *pthis);

bool nn20clock_manager_is_running(const NN20ClockManager *pthis);

/* ---------------------------------------------------------- states --- */

/*
 * Ask for a transition. Returns as soon as the request is queued: the
 * state has probably not changed yet when this returns. Callable from
 * any thread.
 *
 * A request that design 5 does not allow is rejected on the worker, not
 * here - the check needs the current state, which only the worker may
 * read. It is logged and counted; use the state callback to observe what
 * actually happened.
 */
esp_err_t nn20clock_manager_request_state(NN20ClockManager *pthis,
                                          NN20ClockManagerState state);

/*
 * The current state. A snapshot: by the time the caller reads it, the
 * worker may have moved on. Fine for logs and tests, not for making
 * decisions - post a request and let the worker decide instead.
 */
NN20ClockManagerState nn20clock_manager_state(const NN20ClockManager *pthis);

/*
 * Transitions rejected since construction. A steadily rising count means
 * something is asking for edges design 5 does not have.
 */
uint32_t nn20clock_manager_rejected_transitions(const NN20ClockManager *pthis);

/* Set before _start(); the callback then runs on the manager worker. */
esp_err_t nn20clock_manager_set_state_callback(NN20ClockManager *pthis,
                                               NN20ClockManagerStateFn on_state,
                                               void *user_data);

/* Set before _start(), like the state callback, and for the same
 * reason - see NN20ClockManagerRingingEndedFn. */
esp_err_t nn20clock_manager_set_ringing_ended_callback(
    NN20ClockManager *pthis, NN20ClockManagerRingingEndedFn on_ringing_ended,
    void *user_data);

/* --------------------------------------------------------- screens --- */

/*
 * Build the screen for a state, or return NULL when that state has no
 * screen yet. Design 8 makes choosing the active UI the manager's job;
 * this is how it does so without depending on the concrete screens,
 * which need LVGL and therefore only exist in the firmware build.
 *
 * Runs on the ClockManagerWorker, so it MUST NOT touch LVGL. A screen
 * constructor allocates its struct and calls nn20clock_ui_base_init();
 * the widgets are built in the screen's show() hook, which the UI
 * dispatcher already runs on the UiWorker. Keeping to that split is
 * what lets the manager own screen selection without owning the display
 * thread.
 *
 * Ownership of the returned screen passes to the manager.
 */
typedef NN20ClockUiBase *(*NN20ClockManagerScreenFn)(
    NN20ClockManagerState state, void *user_data);

/*
 * Install the screen factory. Set it before _start(); after that the
 * manager reads it from its own worker.
 *
 * With no factory installed the manager runs headless - every
 * transition happens, nothing is drawn. That is the host test
 * configuration, and it is also what the firmware looked like through
 * Milestone 1.
 */
esp_err_t nn20clock_manager_set_screen_factory(
    NN20ClockManager *pthis, NN20ClockManagerScreenFn factory,
    void *user_data);

/*
 * Make `screen` the active one. The manager destroys whatever screen was
 * active first - design 8 requires exactly that order - then shows the
 * new one on the UiWorker. Ownership of `screen` transfers to the
 * manager; NULL just clears the current screen.
 *
 * Queued, not immediate. Callable from any thread.
 */
esp_err_t nn20clock_manager_set_screen(NN20ClockManager *pthis,
                                       NN20ClockUiBase *screen);

/* The active screen, or NULL. A snapshot, as with the state. */
NN20ClockUiBase *nn20clock_manager_screen(const NN20ClockManager *pthis);

/* ----------------------------------------------------------- alarms -- */

/*
 * How long an alarm may go on before the manager dismisses it, in
 * seconds.
 *
 * Design 17 says an alarm loops until somebody stops it, on the
 * principle that an alarm which gives up is worse than one that
 * repeats. That is right for the first few minutes and wrong for the
 * rest of the day: an alarm left ringing to an empty house is only
 * wasting the speaker and the panel.
 *
 * Measured from the alarm's own time - the moment it was scheduled for,
 * not the moment the device noticed it - and NOT restarted by a snooze.
 * A 07:00 alarm is silent from 08:00 whatever happened in between, so
 * the bound is one the user can read off the alarm they set rather than
 * a sum of however many times they hit snooze.
 *
 * The bound lives here, on the ClockManagerWorker, and not on the
 * ringing screen, because it has to hold while the player owns the
 * panel. LVGL's timers do not run then - see
 * nn20clock_display_video_begin() - so a video alarm bounded by an
 * lv_timer was not bounded at all.
 */
#define NN20CLOCK_MANAGER_MAX_RING_SECONDS 3600

/*
 * The alarm currently ringing, or NN20CLOCK_ALARM_ID_NONE. Only
 * meaningful in NN20CLOCK_STATE_ALARM_RINGING; a snapshot, like the
 * state itself.
 */
uint32_t nn20clock_manager_ringing_alarm(const NN20ClockManager *pthis);

/*
 * When the alarm now ringing - or snoozed - stops being allowed to,
 * or 0 when no alarm is in flight. A snapshot, like the state.
 *
 * Set from the first occurrence's own time and carried across snoozes;
 * cleared by a dismissal. Exposed for the tests and the logs, not as
 * something to make decisions on: the worker is the one that acts on
 * it.
 */
time_t nn20clock_manager_ring_deadline(const NN20ClockManager *pthis);

/*
 * Re-read the schedule from storage into the Timer (design 8:
 * "coordinate storage reloads after settings changes"). Call after
 * adding, editing, or deleting an alarm; the settings UI will, at
 * Milestone 4.
 *
 * Blocking, because a caller that has just saved an alarm needs to know
 * the Timer is running the new schedule before it says so.
 */
esp_err_t nn20clock_manager_reload_alarms(NN20ClockManager *pthis);

/* Alarms that have fired since construction, for tests and diagnostics. */
uint32_t nn20clock_manager_alarm_count(const NN20ClockManager *pthis);

/*
 * How a settings screen reads and edits alarms.
 *
 * Design 11 is explicit that screens must not own persistent settings,
 * so AlarmSettingsUi does not touch Storage. It is handed this instead:
 * the manager's own implementation, which saves through Storage and
 * reloads the Timer's schedule afterwards, so a screen cannot forget
 * the second half.
 *
 * THREADING. These are called from the UiWorker and block on the
 * StorageWorker and the Timer's worker. That direction is safe -
 * neither of those ever waits on the UI. What they must never do is
 * wait on the ClockManagerWorker: the manager waits on the UiWorker to
 * destroy a screen, and a wait in the other direction would close the
 * cycle and deadlock both. That is why these go straight to the
 * services rather than hopping through the manager's own worker.
 *
 * `list` fills a caller-provided NN20ClockAlarmList, which is over 5 KB
 * - it belongs on the heap or in a screen's struct, never on a stack.
 */
typedef struct {
    esp_err_t (*list)(void *ctx, NN20ClockAlarmList *out_alarms);
    esp_err_t (*save)(void *ctx, const NN20ClockAlarmConfig *alarm,
                      uint32_t *out_id);
    esp_err_t (*remove)(void *ctx, uint32_t alarm_id);
    void *ctx;
} NN20ClockAlarmService;

/*
 * The manager's implementation of the above. Valid for the manager's
 * lifetime; the returned struct is by value and may be copied.
 */
NN20ClockAlarmService nn20clock_manager_alarm_service(
    NN20ClockManager *pthis);

/* -------------------------------------------------- device settings -- */

/*
 * How DeviceSettingsUi reads and changes device configuration.
 *
 * The same shape, and the same reasoning, as NN20ClockAlarmService:
 * design 11 keeps persistent settings and hardware out of screens, so
 * the screen is handed this instead of a Storage or Display pointer.
 * Applying a setting and storing it are one operation here, so a screen
 * cannot do half of it.
 *
 * THREADING: called from the UiWorker; these block on the StorageWorker
 * and touch the display directly, and must never wait on the
 * ClockManagerWorker - see the note on NN20ClockAlarmService.
 *
 * `apply_brightness` and `apply_volume` deliberately do NOT store:
 * they run while a slider is being dragged, and writing to flash on
 * every pixel of travel would be absurd. `save_config` is what
 * persists, once the finger comes off.
 */
typedef struct {
    esp_err_t (*load_config)(void *ctx, NN20ClockConfig *out_config);
    esp_err_t (*save_config)(void *ctx, const NN20ClockConfig *config);
    /* Live preview while dragging; not persisted. */
    esp_err_t (*apply_brightness)(void *ctx, uint8_t percent);
    /*
     * What the backlight is at right now.
     *
     * Not the same question as "what is stored": brightness follows a
     * day/night schedule, so the stored day value is wrong at 3am and
     * the stored night value is wrong at noon. A screen putting a
     * brightness slider under the user's finger has to start it where
     * the panel actually is. Zero when there is no display.
     */
    uint8_t (*current_brightness)(void *ctx);
    /*
     * The same, for sound. There is one volume on this device and this
     * moves it - the playback slider of design 11 is not a per-session
     * copy, because a second source of truth for one speaker is what
     * made an alarm's volume leak into everything else.
     */
    esp_err_t (*apply_volume)(void *ctx, uint8_t percent);
    /* Join a network now. Storing it is save_config's job. */
    esp_err_t (*connect_wifi)(void *ctx, const char *ssid,
                              const char *password);

    /*
     * Turn the network time sync on or off now. Storing the choice is
     * save_config's job, exactly as with Wi-Fi.
     */
    esp_err_t (*set_ntp)(void *ctx, bool enabled);

    /*
     * Set the wall clock, as seconds since the epoch, for a device with
     * no internet. Also records it as the last known good time, so a
     * power cut comes back to something plausible rather than to 1970.
     *
     * Refused while NTP is on: two things setting the same clock is how
     * you get a time that changes back a minute later.
     */
    esp_err_t (*set_time)(void *ctx, time_t when);

    void *ctx;
} NN20ClockDeviceService;

NN20ClockDeviceService nn20clock_manager_device_service(
    NN20ClockManager *pthis);

/* --------------------------------------------------- media playback -- */

/*
 * How MediaPlaybackUi starts something playing (design 17, Milestone 8).
 *
 * The third of the same shape, and for the same reason as the other
 * two: design 11 keeps the player and the card out of screens, so the
 * picker is handed this instead of a player pointer. `relative_path` is
 * a file path from the card's root - "wake.avi", "morning/wake.avi" -
 * exactly as it appears in an NN20ClockMediaList entry and exactly as
 * an alarm stores it. Resolving it to a VFS path is the composition
 * root's job, and refusing one that tries to leave the card is the SD
 * component's; a screen never builds either.
 *
 * `play_random` is the same request with a folder instead of a file:
 * draw a clip from `folder_path` now, and draw another every time one
 * ends for as long as the sleep timer says to. It is what makes the
 * sleep timer worth having on an evening.
 *
 * `sleep_ms` is the sleep timer the screen was showing, in
 * milliseconds; zero is its default `all`, meaning play the media once
 * and stop. It travels with the request rather than being stored
 * anywhere: it is a property of this playback, not a device setting.
 *
 * Zero means that for `play_random` too: ONE clip, then stop. The
 * drawing is what the timer buys, and `all` is the setting that asks
 * for one thing to be played - the same answer for a folder as for a
 * file, so the control means one thing in both places. On a folder with
 * openers that is the first opener and nothing after it.
 *
 * This paragraph used to say the opposite - that zero with
 * `play_random` meant no timer at all, "a night of films". It never did:
 * nn20clock_video_ui's on_media_ended() has closed playback on a zero
 * `sleep_ms` since Milestone 8, before this service existed. The text
 * arrived with manual <random> and was not revisited when the timer
 * became a duration rather than a cut-off.
 *
 * THREADING: called from the UiWorker. Like the other services it must
 * not wait on the ClockManagerWorker, and it does not: the manager's
 * implementation posts and returns.
 */
typedef struct {
    esp_err_t (*play)(void *ctx, const char *relative_path,
                      uint32_t sleep_ms);
    /* `folder_path` is a folder, "" being the root folder. Draws now and again
     * at every clip's end, from that folder only. */
    esp_err_t (*play_random)(void *ctx, const char *folder_path,
                             uint32_t sleep_ms);
    void *ctx;
} NN20ClockMediaService;

NN20ClockMediaService nn20clock_manager_media_service(
    NN20ClockManager *pthis);

/*
 * What the manager calls to actually start it.
 *
 * The same arrangement as NN20ClockDeviceHooks: the player and the
 * screen it needs exist only in the firmware build, so the composition
 * root installs this and a headless build leaves it NULL and gets
 * ESP_ERR_NOT_SUPPORTED - the honest answer there.
 *
 * `play_media` runs on the UiWorker and must not block it. What it does
 * is build the playback screen and hand it to
 * nn20clock_manager_set_screen(), which is queued.
 */
typedef struct {
    esp_err_t (*play_media)(void *ctx, const char *relative_path,
                            uint32_t sleep_ms);
    esp_err_t (*play_random_media)(void *ctx, const char *folder_path,
                                   uint32_t sleep_ms);
    void *ctx;
} NN20ClockMediaHooks;

/* Install before _start(). */
esp_err_t nn20clock_manager_set_media_hooks(NN20ClockManager *pthis,
                                            NN20ClockMediaHooks hooks);

/*
 * What the manager calls to reach the display and the radio.
 *
 * The manager routes device settings (design 11 keeps them out of
 * screens) but does not own the hardware, and the display and network
 * components exist only in the firmware build. Hooks keep this
 * component platform-neutral and let a host test install fakes; the
 * composition root installs the real ones.
 *
 * Either may be NULL, in which case the corresponding service call
 * reports ESP_ERR_NOT_SUPPORTED - which is the honest answer for a
 * headless build.
 */
typedef struct {
    esp_err_t (*apply_brightness)(void *ctx, uint8_t percent);
    /* May be NULL, in which case the service reports zero. */
    uint8_t (*current_brightness)(void *ctx);
    esp_err_t (*apply_volume)(void *ctx, uint8_t percent);
    esp_err_t (*connect_wifi)(void *ctx, const char *ssid,
                              const char *password);
    esp_err_t (*set_ntp)(void *ctx, bool enabled);
    esp_err_t (*set_time)(void *ctx, time_t when);
    void *ctx;
} NN20ClockDeviceHooks;

/* Install before _start(). */
esp_err_t nn20clock_manager_set_device_hooks(NN20ClockManager *pthis,
                                             NN20ClockDeviceHooks hooks);

/* -------------------------------------------------------- commands --- */

/*
 * The NN20ClockUiCommandFn a screen is constructed with. Copies the
 * command, posts it to the manager worker, and returns - it is called on
 * the UiWorker and must not block it.
 *
 * Pass this straight into NN20ClockUiBaseConfig::on_command.
 */
esp_err_t nn20clock_manager_handle_ui_command(
    NN20ClockManager *manager, const NN20ClockUiCommand *command);

/* Commands accepted since construction, for tests and diagnostics. */
uint32_t nn20clock_manager_command_count(const NN20ClockManager *pthis);

/* ------------------------------------------------------ timer events -- */

/*
 * The Timer subscriber the manager registers in _start(). Exposed only
 * so a test can drive it directly without a running Timer; production
 * code has no reason to call it.
 *
 * Runs on the Timer worker: it copies the event and posts, nothing more.
 */
void nn20clock_manager_on_timer_event(const NN20ClockTimerEvent *event,
                                      void *user_data);

/* Timer events that reached the manager worker. */
uint32_t nn20clock_manager_timer_event_count(const NN20ClockManager *pthis);

#ifdef __cplusplus
}
#endif

#endif /* NN20CLOCK_MANAGER_H */
