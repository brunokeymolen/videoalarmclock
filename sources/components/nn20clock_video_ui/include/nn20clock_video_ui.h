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
 * nn20clock_video_ui.h - the playback screen (design 11,
 * VideoPlayerUi).
 *
 * FIRMWARE ONLY: it drives the player and draws with LVGL.
 *
 * ------------------------------------------------------------------
 * Two views, one screen
 * ------------------------------------------------------------------
 *
 * An alarm has to be worth waking up to and it has to be easy to turn
 * off, and those two things want the whole panel each. So this screen
 * has two views and shows one at a time:
 *
 *   playback   the video, filling the panel, with nothing on top of it;
 *              or, when there is no video to play, a plain ringing face
 *              over the built-in tone.
 *   actions    two buttons, snooze and off, big enough to hit half
 *              asleep in the dark.
 *
 * A touch anywhere moves from the first to the second, and a touch
 * outside the buttons moves back. Sound continues throughout: the views
 * are about who owns the panel, not about whether the alarm is ringing.
 *
 * This is why it is two views of one screen rather than two states of
 * the manager. Nothing about the alarm changes when the buttons appear
 * - the same alarm is ringing, the same file is playing - so a state
 * transition would be describing the panel, not the device.
 *
 * ------------------------------------------------------------------
 * Two origins, one screen
 * ------------------------------------------------------------------
 *
 * Design 11 gives this screen two ways in: an alarm ringing, and media
 * the user chose to watch. They are the same picture on the same panel
 * with the same touch behaviour, and they differ in two things - what
 * the buttons say, and when playback ends:
 *
 *   an alarm   snooze and off; plays until somebody stops it, and falls
 *              back to the built-in tone rather than fall silent. This
 *              screen has no end of its own: the hour after which an
 *              unanswered alarm is dismissed is the ClockManager's, on
 *              a worker that keeps running while the player owns the
 *              panel - see NN20CLOCK_MANAGER_MAX_RING_SECONDS.
 *   media      stop; plays the file once and ends by itself, or stops
 *              early when the sleep timer runs out.
 *
 * Both of those are one field each on the request the player is handed,
 * so the origin is a parameter here rather than a second screen.
 *
 * ------------------------------------------------------------------
 * Touch while the video has the panel
 * ------------------------------------------------------------------
 *
 * LVGL does not run while the player owns the panel, so in the playback
 * view LVGL's own touch handling is not available. The player polls the
 * controller instead and calls back here, on the UiWorker. In the
 * actions view LVGL has the panel back and its buttons work normally.
 */
#ifndef NN20CLOCK_VIDEO_UI_H
#define NN20CLOCK_VIDEO_UI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nn20clock_manager.h"
#include "nn20clock_platform.h"
#include "nn20clock_player.h"
#include "nn20clock_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    /* CLOCK_STATE_ALARM_RINGING: design 13's rules apply. */
    NN20CLOCK_VIDEO_UI_ALARM,
    /* CLOCK_STATE_MEDIA_PLAYBACK: design 17's Milestone 8 rules apply. */
    NN20CLOCK_VIDEO_UI_MEDIA
} NN20ClockVideoUiOrigin;

/*
 * What to play next, asked when the media playing has run out.
 *
 * This is design 13's <random>, and it is the difference between
 * "a film chosen at random" and "the film chosen at random": one that
 * went round again would be a random choice made once and then played
 * all morning. So the caller - who is the one that knows the alarm is a
 * random set and what is on the card - is asked again at every end.
 *
 * Fill `out_path` with a full path on the card and return true. False,
 * or an empty path, means there is nothing else to offer and the clip
 * that just ended plays again: an alarm has no silent option, and a
 * sleep timer with time left on it has nothing to gain by stopping
 * early. Either way this is never a reason to end playback.
 *
 * Called on the UiWorker, from the player's end-of-media callback.
 * Reading the card's listing there is what the media and alarm screens
 * already do, and nothing is on the panel to be held up by it - the
 * film has ended and the next one has not started.
 *
 * NULL for a playback with one fixed file, whichever origin: an alarm
 * plays it round and round, and manual playback loops it for as long as
 * the sleep timer asks.
 */
typedef bool (*NN20ClockVideoUiNextMediaFn)(void *ctx, char *out_path,
                                            size_t out_size);

/*
 * Build the playback screen and, when it is shown, start playing.
 *
 * `player` is borrowed and must outlive the screen. `media_path` is the
 * file to play. For an alarm, NULL/empty asks for the built-in tone -
 * which is also what happens if the file turns out to be unplayable, so
 * the caller does not have to check it first; for media there is no
 * tone and an unplayable file ends the screen. `volume` is 0-100.
 *
 * `sleep_ms` is design 11's sleep timer, and it is a DURATION rather
 * than a cap on one clip: playback keeps going for this long - looping
 * the file, or drawing the next one when `next_media` is set - and is
 * cut wherever it has got to when the time is up. Zero is its default,
 * `all`, which plays the media once and stops. Ignored for an alarm,
 * which has no such end.
 *
 * `next_media` is how a <random> playback gets a different clip each
 * time one ends - see NN20ClockVideoUiNextMediaFn. It is honoured for
 * both origins: an alarm draws until somebody stops it, manual playback
 * until the sleep timer runs out. NULL means the one file, repeated or
 * played once as above.
 *
 * `device` is how the volume and brightness sliders reach the hardware
 * and the stored settings; get it from
 * nn20clock_manager_device_service(). Design 11 keeps both out of
 * screens, so this screen is handed the service rather than the codec
 * and the backlight.
 *
 * Returns the embedded UiBase, which is what ClockManager holds and
 * what nn20clock_ui_destroy() takes. NULL if the worker or the player
 * is missing, or memory runs out.
 */
NN20ClockUiBase *nn20clock_video_ui_ctor(nn20_worker_ctx *ui_worker,
                                         NN20ClockManager *manager,
                                         NN20ClockUiCommandFn on_command,
                                         NN20ClockPlayer *player,
                                         NN20ClockVideoUiOrigin origin,
                                         const char *media_path,
                                         uint8_t volume,
                                         uint32_t sleep_ms,
                                         NN20ClockVideoUiNextMediaFn next_media,
                                         void *next_media_ctx,
                                         NN20ClockDeviceService device);

#ifdef __cplusplus
}
#endif

#endif /* NN20CLOCK_VIDEO_UI_H */
