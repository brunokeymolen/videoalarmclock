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
 * nn20clock_media_mgmt_ui.h - the media management screen (design 11,
 * MediaManagementUi).
 *
 * FIRMWARE ONLY: it draws with LVGL.
 *
 * One view, reached from DeviceSettingsUi and returning to it. It is a
 * status board rather than a control panel: the files themselves are
 * managed over FTP from a laptop or a phone, and this screen is what
 * you look at to find out where to connect, whether anybody is
 * connected, and whether the upload landed.
 *
 * What it shows (design 11):
 *
 *   FTP server     running or stopped, and the address to reach it on.
 *   Connections    who is connected, and what each one is doing.
 *   SD card        mounted or not, and how much room is left.
 *   Media          how many clips are on the card, and how many files
 *                  were skipped as not-media.
 *   Transfers      uploads and downloads since the server started.
 *
 * ------------------------------------------------------------------
 * There is no username or password, and the screen says so
 * ------------------------------------------------------------------
 *
 * Design 11 asks this screen for "device IP address, username, and
 * password". There are no credentials to show: Milestone 6 settled on
 * anonymous access, and nn20clock_ftp.h argues the case - FTP sends
 * credentials in clear text, so a password would be theatre rather than
 * protection. Two blank fields would be worse than useless, so the
 * screen states the situation in words instead: any device on the
 * network can read and write the card's root. That is a real
 * property of this clock and the person standing in front of it should
 * be able to see it.
 *
 * ------------------------------------------------------------------
 * Refreshing
 * ------------------------------------------------------------------
 *
 * Two rates, because the two kinds of question cost very different
 * amounts to answer.
 *
 * The cheap ones - is the server up, who is connected, what are they
 * doing, how many transfers - are atomics in the FTP component, so they
 * are re-read every second.
 *
 * The expensive ones - free space, and the number of clips - mean
 * reading the card, which is the same bus playback competes for. Those
 * are refreshed when something could actually have changed them: on
 * open, when the upload count moves, when the last client disconnects
 * (a delete leaves no counter behind, but it does end in a
 * disconnection), and whenever the user taps the row.
 *
 * Threading: the constructor allocates and nothing more; widgets are
 * built in show(), which the UiBase dispatcher runs on the UiWorker.
 */
#ifndef NN20CLOCK_MEDIA_MGMT_UI_H
#define NN20CLOCK_MEDIA_MGMT_UI_H

#include "nn20clock_ftp.h"
#include "nn20clock_manager.h"
#include "nn20clock_net.h"
#include "nn20clock_platform.h"
#include "nn20clock_sd.h"
#include "nn20clock_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * `ftp`, `net` and `sd` are borrowed and only ever queried - nothing on
 * this screen changes any of them. They follow the precedent
 * DeviceSettingsUi set with the radio: a service struct is for settings
 * a screen may change, and asking a component how it is doing is not
 * that.
 *
 * Any of the three may be NULL, in which case its section says what is
 * missing rather than showing zeroes that read like facts.
 */
NN20ClockUiBase *nn20clock_media_mgmt_ui_ctor(nn20_worker_ctx *ui_worker,
                                              NN20ClockManager *manager,
                                              NN20ClockUiCommandFn on_command,
                                              NN20ClockFtp *ftp,
                                              NN20ClockNet *net,
                                              NN20ClockSd *sd);

#ifdef __cplusplus
}
#endif

#endif /* NN20CLOCK_MEDIA_MGMT_UI_H */
