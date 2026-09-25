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
 * nn20clock_device_ui.h - the system settings screen (design 11,
 * DeviceSettingsUi).
 *
 * FIRMWARE ONLY: it draws with LVGL.
 *
 * Three views, in the same one-screen style as AlarmSettingsUi:
 *
 *   the menu      Wi-Fi (showing the current network), brightness, and
 *                 volume; plus back.
 *   the networks  a scan result list, then a passphrase keyboard for
 *                 the chosen network.
 *   the sliders   brightness, which dims the panel live as it moves.
 *
 * What is real and what is not, so nothing on screen lies:
 *
 *   Wi-Fi        real. Scans, joins, and stores the credentials in
 *                flash, replacing the build-time values.
 *   brightness   real. LEDC PWM behind it; moving the slider dims the
 *                panel immediately, and the value is applied at boot.
 *   volume       stored only. There is no audio path until the video
 *                alarm milestone, so the screen says so rather than
 *                pretending.
 *   time zone    real. Picked from nn20clock_timezones.h under Time,
 *                applied at once and stored; the face follows it.
 *   updates      real. The About view checks for a newer release and
 *                installs it; see nn20clock_ota.h for what it talks to.
 *
 * Threading: the constructor allocates and nothing more; widgets are
 * built in show(), which the UiBase dispatcher runs on the UiWorker.
 * Settings are read and written through NN20ClockDeviceService, never
 * through Storage or the hardware directly - design 11 keeps both out
 * of screens.
 */
#ifndef NN20CLOCK_DEVICE_UI_H
#define NN20CLOCK_DEVICE_UI_H

#include "nn20clock_manager.h"
#include "nn20clock_net.h"
#include "nn20clock_ota.h"
#include "nn20clock_platform.h"
#include "nn20clock_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * `service` comes from nn20clock_manager_device_service(). `net` and
 * `ota` are borrowed and are passed separately from the service because
 * neither is a setting: one answers "which networks are in range" and
 * the other "is there a newer version of me", and design 11 keeps
 * settings and queries apart.
 *
 * Either may be NULL. The Wi-Fi section then reports that scanning is
 * unavailable rather than offering a list that can never fill, and the
 * About view says the build cannot update itself rather than offering a
 * button that cannot work.
 */
NN20ClockUiBase *nn20clock_device_ui_ctor(nn20_worker_ctx *ui_worker,
                                          NN20ClockManager *manager,
                                          NN20ClockUiCommandFn on_command,
                                          NN20ClockDeviceService service,
                                          NN20ClockNet *net,
                                          NN20ClockOta *ota);

#ifdef __cplusplus
}
#endif

#endif /* NN20CLOCK_DEVICE_UI_H */
