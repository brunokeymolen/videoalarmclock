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
 * nn20clock_alarm_ui.h - the alarm settings screen (design 11).
 *
 * FIRMWARE ONLY: it draws with LVGL.
 *
 * Two views in one screen, because they are one task from the user's
 * point of view:
 *
 *   the list    every stored alarm, each row showing its time, its days,
 *               and an enable switch; plus add and back.
 *   the editor  one alarm: hour and minute rollers, seven weekday
 *               toggles, an enable switch, the sound it plays, and
 *               delete.
 *
 * The editor has no save button. Leaving it by the back arrow keeps the
 * edits, and the cancel beside the delete is how they are thrown away -
 * the way round that loses nothing by accident. The one edit that
 * cannot be kept is a recurrent alarm with no weekday, which could
 * never fire; the editor says so and stays up rather than dropping it
 * silently on the way out.
 *
 * Milestone 4 scope: recurrent alarms only. The model and the scheduler
 * handle one-off alarms fully (design 10), but creating one needs a date
 * picker, and volume, snooze, and media selection all need services that
 * do not exist yet - the audio path is Milestone 7 and the SD card is
 * Milestone 6. Controls that cannot change anything are worse than
 * absent ones.
 *
 * Threading: like every screen, the constructor allocates and nothing
 * more; the widgets are built in show(), which the UiBase dispatcher
 * runs on the UiWorker. It reads and writes alarms through the
 * NN20ClockAlarmService it is given, never through Storage directly -
 * design 11 keeps persistent settings out of screens.
 */
#ifndef NN20CLOCK_ALARM_UI_H
#define NN20CLOCK_ALARM_UI_H

#include "nn20clock_manager.h"
#include "nn20clock_platform.h"
#include "nn20clock_sd.h"
#include "nn20clock_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * `service` is how this screen reads and edits alarms; get it from
 * nn20clock_manager_alarm_service(). manager and on_command carry the
 * back button's CLOSE_SETTINGS command, so both are required for the
 * screen to be leavable.
 *
 * `sd` is how the editor lists what an alarm could play (design 13). It
 * is borrowed and may be NULL - without a card the only sound on offer
 * is the built-in tone, which is what an alarm falls back to anyway.
 *
 * Returns the embedded UiBase. NULL if the worker is missing or memory
 * runs out.
 */
NN20ClockUiBase *nn20clock_alarm_ui_ctor(nn20_worker_ctx *ui_worker,
                                         NN20ClockManager *manager,
                                         NN20ClockUiCommandFn on_command,
                                         NN20ClockAlarmService service,
                                         NN20ClockSd *sd);

#ifdef __cplusplus
}
#endif

#endif /* NN20CLOCK_ALARM_UI_H */
