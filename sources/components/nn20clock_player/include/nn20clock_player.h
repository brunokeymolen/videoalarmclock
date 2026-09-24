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
 * nn20clock_player.h - playing an alarm (design 16, 17 Milestone 7).
 *
 * FIRMWARE ONLY. The hardware JPEG decoder, the panel, and the codec.
 * The part worth testing without a board - reading the container - is
 * nn20clock_avi, which is dual-mode and has its own suite.
 *
 * ------------------------------------------------------------------
 * What playing an alarm means
 * ------------------------------------------------------------------
 *
 * Design 13 sets the chain: the alarm's own media, then the built-in
 * tone, and never silence. So this takes a path and does whatever it
 * takes to make a noise:
 *
 *   - a readable AVI of MJPEG and PCM: video on the panel, sound on the
 *     codec, looped until stopped;
 *   - anything else, or no path at all: the synthesised tone, also
 *     until stopped.
 *
 * An alarm ends when someone ends it, not when the file does. A
 * two-minute video that stopped on its own would be an alarm that gives
 * up, so the file is played again from the top.
 *
 * Design 17's Milestone 8 asks for the other kind as well: media the
 * user chose to watch, which should end by itself. Both are the same
 * loop with a different floor under it - see NN20ClockPlayerRequest.
 *
 * ------------------------------------------------------------------
 * Threading
 * ------------------------------------------------------------------
 *
 * Playback runs on its own worker, posted as a single long-running
 * call: it blocks on the card, on the decoder, and on the I2S buffer
 * draining, and none of that can sit on a shared thread. It is the
 * fifth worker in design 4's set, which names four - see
 * nn20clock_workers.h.
 *
 * Nothing here is locked. The player's task owns playback; the panel
 * stays owned by the UiWorker, which is why frames are handed to
 * nn20clock_display_video_draw() rather than drawn here; and the two
 * things another thread asks for - stop, and whether the video is
 * on screen - are single atomic flags the loop reads at frame
 * boundaries.
 *
 * Stopping is deliberately asynchronous. The player's loop calls into
 * the UiWorker for every frame, so a caller on the UiWorker that waited
 * for the player would be waiting for itself - the sync cycle design 4
 * forbids. _halt() asks and returns; _is_playing() reports.
 */
#ifndef NN20CLOCK_PLAYER_H
#define NN20CLOCK_PLAYER_H

#include <stdbool.h>
#include <stdint.h>

#include "nn20clock_audio.h"
#include "nn20clock_display.h"
#include "nn20clock_platform.h"
#include "worker.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NN20ClockPlayer NN20ClockPlayer;

/*
 * Side of the square thumbnail kept of the last frame shown.
 *
 * The panel is 720 wide, so 120 divides it exactly and the shrink is a
 * plain every-sixth-pixel decimation - no filtering, no arithmetic that
 * has to be right. It is a reminder on the clock face, not a picture.
 */
#define NN20CLOCK_PLAYER_THUMBNAIL_SIZE 120

/*
 * Called on the UiWorker when the screen is touched during video
 * playback.
 *
 * It exists because LVGL is not running while the player owns the panel
 * (see nn20clock_display_video_begin), so the screen cannot learn about
 * the touch the usual way. The handler is set and cleared on the
 * UiWorker and called on the UiWorker, so a screen clearing it in
 * hide() cannot race with a touch arriving.
 */
typedef void (*NN20ClockPlayerTouchFn)(void *ctx);

/*
 * Called on the UiWorker when playback ends of its own accord - the
 * media ran out with its minimum behind it, or the file failed and
 * there was no tone to fall back to.
 *
 * NOT called when someone halted playback: whoever pressed stop already
 * knows, and a screen that acted on the press does not want to be told
 * again a second later. Set and cleared on the UiWorker, like the touch
 * handler and for the same reason.
 */
typedef void (*NN20ClockPlayerDoneFn)(void *ctx);

/* --------------------------------------------------------- lifecycle -- */

/*
 * `worker` is the player's own thread and must not be shared - playback
 * occupies it for as long as an alarm rings. `reader` is a second
 * thread of its own, which fills the read-ahead window while `worker`
 * decodes and feeds the codec; sharing either would put the two back in
 * the same queue and undo the point of having them. `ui_worker`,
 * `display` and `audio` are borrowed and must outlive this.
 *
 * Constructing allocates nothing large; _start() does.
 */
NN20ClockPlayer *nn20clock_player_ctor(nn20_worker_ctx *worker,
                                       nn20_worker_ctx *reader,
                                       nn20_worker_ctx *ui_worker,
                                       NN20ClockDisplay *display,
                                       NN20ClockAudio *audio);

/* Halts playback and waits for it to finish. Safe with NULL. */
void nn20clock_player_dtor(NN20ClockPlayer *pthis);

/*
 * Claim the JPEG decoder and the frame buffers - about three megabytes
 * of PSRAM, held for the life of the player.
 *
 * They are taken at startup rather than when an alarm fires because
 * that is the one moment they must not fail: an out-of-memory at 7am is
 * a silent alarm, and design 13 is explicit that silence is the worst
 * outcome this device has.
 */
esp_err_t nn20clock_player_start(NN20ClockPlayer *pthis);

/* Halt playback and release the decoder and buffers. Idempotent. */
esp_err_t nn20clock_player_stop(NN20ClockPlayer *pthis);

bool nn20clock_player_is_running(const NN20ClockPlayer *pthis);

/* --------------------------------------------------------- playback -- */

/*
 * What to play, and what ends it.
 *
 * The two callers want the same loop with different ends to it, and
 * this is where they differ rather than in two code paths:
 *
 *   an alarm   loop, no deadline, tone fallback, position remembered;
 *   the user's own media (design 17, Milestone 8)
 *              at the sleep timer's `whole video`, played once and
 *              stopped; with a timer set, looped until the deadline
 *              cuts it - and none of the other three.
 */
typedef struct {
    /*
     * A full path on the mounted card. NULL or empty asks for the
     * built-in tone directly; so does a path that turns out to be
     * missing, unreadable, or in a format this device cannot decode -
     * the fallback is not an error path the caller has to handle.
     */
    const char *path;

    /* 0-100, applying until the next call. */
    uint8_t volume;

    /*
     * Come up to `volume` from `fade_from_volume` over this long,
     * rather than starting there. Zero: start at `volume`.
     *
     * Design 13's alarm does not begin at the volume it ends at - it
     * comes up out of the quiet over the first few seconds, so that
     * being woken is not the same as being startled. It is expressed
     * here, on the request, because it belongs to the alarm and not to
     * playback: media the user chose to watch starts at the volume
     * they set, and leaves these two zero.
     *
     * None of it is stored. The ramp lives in the codec's register for
     * as long as it runs; the volume setting is `volume` throughout,
     * before, during and after, so nothing a fade passes through can
     * be read back as a preference or written to the card.
     */
    uint32_t fade_in_ms;
    uint8_t fade_from_volume;

    /*
     * Play the media again when it ends, until somebody stops it or
     * `stop_after_ms` runs out. Design 13's alarm, which sets no
     * deadline: one that gave up on its own would be an alarm that lets
     * you sleep in. Manual playback sets both, so a sleep timer set in
     * minutes fills the time rather than merely capping it.
     *
     * False plays it once and stops, which is design 11's sleep timer
     * at its default of `all`.
     */
    bool loop;

    /*
     * Stop this many milliseconds in, wherever the media has got to.
     * Zero means no deadline.
     *
     * This is the sleep timer, and unlike the end of the media it cuts
     * where it falls: somebody who set it is going to sleep, and
     * waiting for a natural break would defeat the point.
     *
     * Combines with `loop`: together they mean "keep playing for this
     * long". A caller that restarts playback itself between clips - the
     * <random> screens do - must pass the time REMAINING rather than
     * the timer it started with, or the deadline begins again at every
     * clip.
     */
    uint32_t stop_after_ms;

    /*
     * Ring the built-in tone when the file cannot be played. Design 13
     * requires it of an alarm and forbids it of everything else: media
     * the user chose to watch failing is a reason to stop, not to start
     * making a noise they did not ask for.
     */
    bool tone_fallback;

    /*
     * Resume this file where it was left, remember where it stops, and
     * keep the last frame for the clock face - all of which exist for
     * the snooze, and none of which manual playback may touch. See
     * "across a snooze" below.
     */
    bool remember;
} NN20ClockPlayerRequest;

/*
 * Start playing, and return immediately.
 *
 * ESP_ERR_INVALID_STATE if something is already playing: an alarm
 * ringing over another alarm is not a thing this device does.
 */
esp_err_t nn20clock_player_play(NN20ClockPlayer *pthis,
                                const NN20ClockPlayerRequest *request);

/*
 * Ask playback to stop. Returns at once, before it has: see the
 * threading note above for why this cannot wait.
 */
esp_err_t nn20clock_player_halt(NN20ClockPlayer *pthis);

bool nn20clock_player_is_playing(const NN20ClockPlayer *pthis);

/* ------------------------------------------------- across a snooze -- */

/*
 * A snooze interrupts an alarm rather than ending it, so what it
 * interrupted is remembered: the file, where in it playback had got to,
 * and the frame that was on screen.
 *
 * All of it is RAM only, to match design 10's snooze - a reboot forgets
 * the snooze, and forgets this with it. _play() picks the position up
 * again by itself when handed the same file; there is nothing for the
 * caller to pass.
 */

/*
 * Throw away the remembered position and frame.
 *
 * Called when an alarm is dismissed rather than snoozed: the next time
 * that file plays it should start at the beginning, and the clock face
 * has nothing left to show.
 *
 * Safe to call from any thread, and safe to call with playback still
 * stopping: the frame is dropped at once, and the position is dropped
 * on the player's own worker once whatever was playing has finished
 * recording where it got to. Both are therefore certain by the time
 * anything could play again, which is the only deadline that matters.
 */
void nn20clock_player_forget_position(NN20ClockPlayer *pthis);

/*
 * The last frame shown, shrunk to NN20CLOCK_PLAYER_THUMBNAIL_SIZE
 * square, as RGB565 - or NULL when there is none, which is the normal
 * case for an alarm that rang on the built-in tone.
 *
 * Valid until the next alarm starts playing. Safe to read from the
 * UiWorker: it is written by the player's task while an alarm rings,
 * and the clock face that reads it only exists when none is.
 */
const void *nn20clock_player_thumbnail(const NN20ClockPlayer *pthis);

/*
 * Whether the video is on the panel, and whether it should be.
 *
 * The screen sets this to false when it wants to draw over the video -
 * its snooze and off buttons - and back to true to return to the
 * picture. Sound continues either way: this is about who owns the
 * panel, not about whether the alarm is ringing.
 *
 * Setting it only records the wish. The player acts on it between
 * frames, because handing the panel over is its to do.
 *
 * The wish is the screen's for as long as the screen lives: the player
 * never writes it, so it survives the end of one film and the start of
 * the next. A screen that wants the picture says so once, when it comes
 * up, and again whenever it takes the panel back for its buttons. It
 * may be set before anything is playing; nothing happens until there is
 * a picture to show.
 */
esp_err_t nn20clock_player_set_video_visible(NN20ClockPlayer *pthis,
                                             bool visible);

bool nn20clock_player_video_visible(const NN20ClockPlayer *pthis);

/* Whether this alarm is playing a file rather than the built-in tone.
 * The screen shows something to look at in the second case. */
bool nn20clock_player_has_video(const NN20ClockPlayer *pthis);

/* Set and cleared on the UiWorker, by the screen that wants to know. */
void nn20clock_player_set_touch_handler(NN20ClockPlayer *pthis,
                                        NN20ClockPlayerTouchFn handler,
                                        void *ctx);

void nn20clock_player_set_done_handler(NN20ClockPlayer *pthis,
                                       NN20ClockPlayerDoneFn handler,
                                       void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* NN20CLOCK_PLAYER_H */
