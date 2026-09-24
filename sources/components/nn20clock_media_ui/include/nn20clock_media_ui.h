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
 * nn20clock_media_ui.h - the media selection screen (design 11,
 * MediaPlaybackUi; design 17, Milestone 8).
 *
 * FIRMWARE ONLY: it draws with LVGL.
 *
 * What is in one folder on the card, one row each, and a back button.
 * Choosing a file starts it playing; this screen then goes away and the
 * playback screen takes its place, which is why there is nothing here
 * about stopping. Choosing a folder enters it - the same browsing the
 * alarm editor's sound picker does.
 *
 * <random> is here too, and means "keep drawing from this folder until the
 * sleep timer stops you" rather than the alarm's "a different film each
 * morning". It is drawn beside the sleep timer because the two are one
 * decision: an evening's films, and when to stop.
 *
 * ------------------------------------------------------------------
 * Why this is a separate screen from the player
 * ------------------------------------------------------------------
 *
 * Design 11 switches views inside VideoPlayerUi rather than changing
 * state, because a state change would destroy the screen and stop the
 * alarm at the worst possible moment. That reasoning does not reach
 * here: nothing is playing while the list is up, so there is nothing to
 * interrupt, and the two screens have nothing in common but the state
 * they live in.
 *
 * Design 5 gives both of them CLOCK_STATE_MEDIA_PLAYBACK. The manager
 * swaps one for the other with nn20clock_manager_set_screen(), which
 * already destroys the outgoing screen before building the next.
 *
 * Milestone 8 scope: the list and the choosing. The sleep-timer control
 * design 11 puts above the list is Milestone 9, along with the playback
 * sliders it shares a window with; until then playback runs for the
 * hour Milestone 8 specifies. A control that cannot change anything is
 * worse than an absent one.
 *
 * Threading: like every screen, the constructor allocates and nothing
 * more; the widgets are built in show(), on the UiWorker. It starts
 * playback through the NN20ClockMediaService it is given, never through
 * a player of its own - design 11 keeps hardware out of screens.
 */
#ifndef NN20CLOCK_MEDIA_UI_H
#define NN20CLOCK_MEDIA_UI_H

#include "nn20clock_manager.h"
#include "nn20clock_platform.h"
#include "nn20clock_sd.h"
#include "nn20clock_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * `service` is how a chosen file is started; get it from
 * nn20clock_manager_media_service() - it takes a path relative to the
 * card's root, which is what this screen hands it. `sd` is how the card
 * is read and is borrowed; NULL, or an unmounted card, leaves the
 * screen saying so rather than showing an empty list.
 *
 * manager and on_command carry the back button's command, so both are
 * required for the screen to be leavable.
 *
 * Returns the embedded UiBase. NULL if the worker is missing or memory
 * runs out.
 */
NN20ClockUiBase *nn20clock_media_ui_ctor(nn20_worker_ctx *ui_worker,
                                         NN20ClockManager *manager,
                                         NN20ClockUiCommandFn on_command,
                                         NN20ClockMediaService service,
                                         NN20ClockSd *sd);

#ifdef __cplusplus
}
#endif

#endif /* NN20CLOCK_MEDIA_UI_H */
