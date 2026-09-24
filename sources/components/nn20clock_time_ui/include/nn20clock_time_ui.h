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
 * nn20clock_time_ui.h - the default clock face (design 11, TimeUi).
 *
 * FIRMWARE ONLY: it draws with LVGL. What is worth testing without a
 * board - formatting a time into "HH:MM" - lives in nn20clock_timefmt,
 * which is dual-mode and has its own suite.
 *
 * Design 11 asks for a black background, large HH:MM digits in light
 * blue, and status indicators. Milestone 2 delivers the time itself;
 * the alarm, Wi-Fi, and SD indicators arrive with the services behind
 * them.
 *
 * Threading: like every screen, the constructor allocates and nothing
 * more. The LVGL objects are built in show(), which the UiBase
 * dispatcher runs on the UiWorker - the constructor is called by
 * ClockManager on its own worker and must not touch LVGL. Timer events
 * arrive through the same dispatcher and are already on the UiWorker by
 * the time this screen sees them.
 */
#ifndef NN20CLOCK_TIME_UI_H
#define NN20CLOCK_TIME_UI_H

#include "nn20clock_platform.h"
#include "nn20clock_player.h"
#include "nn20clock_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * `player` is borrowed and may be NULL. It is here for one thing: the
 * frame a snoozed alarm was interrupted on, which this screen shows in
 * the corner while the snooze is pending. Without it the reminder falls
 * back to a bell.
 *
 * Build the clock screen. manager and on_command may be NULL, in which
 * case the screen simply raises no commands - there is nowhere to send
 * them until Milestone 4 gives it a settings screen to open.
 *
 * Returns the embedded UiBase, which is what ClockManager holds and
 * what nn20clock_ui_destroy() takes. NULL if the worker is missing or
 * memory runs out.
 */
NN20ClockUiBase *nn20clock_time_ui_ctor(nn20_worker_ctx *ui_worker,
                                        NN20ClockManager *manager,
                                        NN20ClockUiCommandFn on_command,
                                        NN20ClockPlayer *player);

#ifdef __cplusplus
}
#endif

#endif /* NN20CLOCK_TIME_UI_H */
