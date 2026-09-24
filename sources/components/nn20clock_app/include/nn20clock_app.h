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
 * nn20clock_app.h - the composition root, running on the CoreWorker.
 *
 * Design 4 gives the CoreWorker "application-level orchestration that is
 * not tied to UI, storage, or ClockManager ownership. Initially this
 * includes starting and supervising ClockManager and other long-lived
 * services." That is this component, and it is the only place in the
 * project that knows how the pieces fit together:
 *
 *     NN20ClockWorkers   the four threads (design 4)
 *       -> NN20ClockStorage   on the StorageWorker
 *       -> NN20ClockDisplay   on the UiWorker       (firmware only)
 *       -> NN20ClockTimer     on its own worker (design 6, 7)
 *       -> NN20ClockManager   on the ClockManagerWorker
 *                             screens on the UiWorker
 *       -> NN20ClockNet       on the CoreWorker     (firmware only)
 *
 * Construction runs in that order and teardown in the exact reverse, so
 * nothing is ever left holding a worker or a service that has already
 * gone away. app_main() builds one of these and nothing else.
 *
 * The start order is not arbitrary either. The display comes up before
 * the manager, because the manager's first transition shows a screen;
 * the network goes last, because it is the slowest and nothing waits
 * for it - design 14's point is that the clock runs without one.
 *
 * There is no locking here either. Start and stop are posted to the
 * CoreWorker and waited on, which is what makes them safe to call from
 * app_main's task without a lock in sight.
 */
#ifndef NN20CLOCK_APP_H
#define NN20CLOCK_APP_H

#include <stdbool.h>

#include "nn20clock_manager.h"
#include "nn20clock_platform.h"
#include "nn20clock_storage.h"
#include "nn20clock_timer.h"
#include "nn20clock_workers.h"

#if defined(ESP_PLATFORM)
#include "nn20clock_brightness.h"
#include "nn20clock_display.h"
#include "nn20clock_ftp.h"
#include "nn20clock_net.h"
#include "nn20clock_sd.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NN20ClockApp NN20ClockApp;

/* --------------------------------------------------------- lifecycle -- */

/*
 * Create the workers and every long-lived service, but start nothing.
 * Returns NULL if any piece cannot be built, having torn down the ones
 * that were - there is no half-built app.
 */
NN20ClockApp *nn20clock_app_ctor(void);

/* Stops first if still running. Safe with NULL. */
void nn20clock_app_dtor(NN20ClockApp *pthis);

/*
 * Start the services in dependency order, on the CoreWorker, and wait
 * for it. On failure the app is left stopped, not half-started, and the
 * manager is put into NN20CLOCK_STATE_ERROR so the failure is visible in
 * the state machine rather than only in the log.
 */
esp_err_t nn20clock_app_start(NN20ClockApp *pthis);

/* Stop in reverse order. Blocking, idempotent. */
esp_err_t nn20clock_app_stop(NN20ClockApp *pthis);

bool nn20clock_app_is_running(const NN20ClockApp *pthis);

/* --------------------------------------------------------- services -- */

/*
 * Borrowed, and only valid while the app is alive. These exist so
 * app_main and the tests can look at what was built; components get
 * their dependencies through their constructors, not from here.
 */
NN20ClockWorkers *nn20clock_app_workers(const NN20ClockApp *pthis);

#if defined(ESP_PLATFORM)
/*
 * The two hardware-bound services. They exist only in the firmware
 * build - the panel and the ESP32-C6 radio have no host equivalent - so
 * the host suite exercises the app without them, which is also a fair
 * description of a board whose screen has failed.
 */
NN20ClockDisplay *nn20clock_app_display(const NN20ClockApp *pthis);
NN20ClockNet *nn20clock_app_net(const NN20ClockApp *pthis);
NN20ClockSd *nn20clock_app_sd(const NN20ClockApp *pthis);
NN20ClockFtp *nn20clock_app_ftp(const NN20ClockApp *pthis);
#endif

NN20ClockStorage *nn20clock_app_storage(const NN20ClockApp *pthis);
NN20ClockTimer *nn20clock_app_timer(const NN20ClockApp *pthis);
NN20ClockManager *nn20clock_app_manager(const NN20ClockApp *pthis);

/* ------------------------------------------------------- supervision -- */

/*
 * One health pass on the CoreWorker: are all four workers still running,
 * is the Timer still ticking over, is the manager still in a sane state.
 * Returns ESP_OK when everything checks out, ESP_FAIL otherwise, with
 * the specifics logged.
 *
 * Design 4 makes supervision the CoreWorker's job; this is the hook for
 * it. Call it from a heartbeat. It does not restart anything yet -
 * recovery policy is Milestone 12.
 */
esp_err_t nn20clock_app_supervise(NN20ClockApp *pthis);

/* One block of INFO lines: state, services, and per-worker statistics. */
void nn20clock_app_log_status(const NN20ClockApp *pthis);

/*
 * Whether a film or an alarm is playing right now.
 *
 * For the heartbeat, which has work that must not run while it is. The
 * player's thread has a 32.6 ms audio buffer under it and a frame due
 * every 50; anything that blocks it for longer than that is heard as a
 * tick and seen as a stall. A heap integrity walk over 31 MB of PSRAM
 * is one such thing - measured at 220 ms - and so, to a lesser degree,
 * is a dozen log lines going out of a 115200 baud port.
 *
 * Always false on the host, which has no player.
 */
bool nn20clock_app_is_playing(const NN20ClockApp *pthis);

#ifdef __cplusplus
}
#endif

#endif /* NN20CLOCK_APP_H */
