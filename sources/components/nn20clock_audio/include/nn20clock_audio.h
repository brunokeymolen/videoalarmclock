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
 * nn20clock_audio.h - the ES8311 codec and I2S output (design 7, 13).
 *
 * FIRMWARE ONLY. This is a codec on an I2C bus and a serial audio bus
 * with this board's pin numbers; none of it means anything on a
 * desktop. The configuration is the one proven in tryout/videoplayback.
 *
 * ------------------------------------------------------------------
 * The bus this does not own
 * ------------------------------------------------------------------
 *
 * The codec shares I2C port 0 with the touch controller (design 2), and
 * ESP-IDF allows one owner per bus. The display creates it and this
 * borrows it, which is why the ctor takes a handle it did not make and
 * why this must be constructed after the display and destroyed before
 * it. Design 4's bottom-up construction and reverse teardown already
 * give that, as long as the composition root keeps the order.
 *
 * ------------------------------------------------------------------
 * Threading
 * ------------------------------------------------------------------
 *
 * There is no worker here and no lock. The player task owns playback -
 * it is the only caller of _configure(), _write() and _tone(), and it
 * calls them in sequence. Volume is the one thing another thread
 * touches, from the settings screen on the UiWorker; the percentage
 * itself is atomic, and the register write underneath goes through the
 * i2c_master driver, which serialises access to the shared bus.
 *
 * A volume ramp (_fade_volume()) is started and advanced by the player
 * task alone, so the numbers describing one need nothing around them.
 * The one thing that crosses threads is cancelling it, and that is a
 * single flag.
 *
 * ------------------------------------------------------------------
 * The built-in tone
 * ------------------------------------------------------------------
 *
 * Design 13: an alarm that makes no sound is the worst outcome this
 * device has. The fallback is synthesised here rather than read from a
 * file, because the usual reason for needing a fallback is that the
 * card is missing or unreadable - so a fallback stored on the card
 * would fail exactly when it is needed.
 */
#ifndef NN20CLOCK_AUDIO_H
#define NN20CLOCK_AUDIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nn20clock_display.h"
#include "nn20clock_platform.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NN20ClockAudio NN20ClockAudio;

/* What the output is set to until a file says otherwise, and what the
 * built-in tone is generated at. */
#define NN20CLOCK_AUDIO_DEFAULT_SAMPLE_RATE 44100u
#define NN20CLOCK_AUDIO_DEFAULT_CHANNELS    2u
#define NN20CLOCK_AUDIO_DEFAULT_BITS        16u

/* --------------------------------------------------------- lifecycle -- */

/*
 * `display` is borrowed and must outlive this. It is taken whole rather
 * than as a bus handle because the bus does not exist until the display
 * has started, and this is constructed before that - so the handle is
 * fetched in _start(), by which time there is one.
 *
 * Constructing does not touch the hardware; _start() does.
 */
NN20ClockAudio *nn20clock_audio_ctor(NN20ClockDisplay *display);

/* Stops first if needed. Safe with NULL. */
void nn20clock_audio_dtor(NN20ClockAudio *pthis);

/*
 * Bring up the codec and the I2S output at the default format, with the
 * amplifier enabled and nothing playing.
 *
 * The amplifier is left on: it draws little, and switching it per play
 * puts a click at the start of every alarm.
 */
esp_err_t nn20clock_audio_start(NN20ClockAudio *pthis);

/* Silence the output and disable the amplifier. Idempotent. */
esp_err_t nn20clock_audio_stop(NN20ClockAudio *pthis);

bool nn20clock_audio_is_running(const NN20ClockAudio *pthis);

/* -------------------------------------------------------- playback -- */

/*
 * Set the output format for what is about to be played, and rewind the
 * built-in tone.
 *
 * Reconfiguring stops the output briefly, so call it between plays and
 * not during one. A format the codec cannot be clocked to is refused
 * with ESP_ERR_NOT_SUPPORTED and the previous one is left in place -
 * the caller can still fall back to the tone.
 *
 * `bits` must be 16: it is what the AVI reader accepts and what the
 * I2S slots are configured for.
 */
esp_err_t nn20clock_audio_configure(NN20ClockAudio *pthis,
                                    uint32_t sample_rate,
                                    uint16_t channels,
                                    uint16_t bits);

/*
 * Write PCM to the output, blocking until the driver has taken it all.
 * That blocking is the player's clock: the I2S buffer draining at the
 * sample rate is what keeps a video from running ahead of its sound.
 */
esp_err_t nn20clock_audio_write(NN20ClockAudio *pthis,
                                const void *samples, size_t bytes);

/*
 * Play `duration_ms` of the built-in alarm tone, blocking for about
 * that long.
 *
 * The tone continues where the previous call left off - phase and
 * beeping envelope both - so a caller wanting to stay responsive can
 * ask for a couple of hundred milliseconds at a time in a loop, and
 * check for a dismissal between calls, without the sound breaking up.
 */
esp_err_t nn20clock_audio_tone(NN20ClockAudio *pthis, uint32_t duration_ms);

/* ---------------------------------------------------------- volume -- */

/*
 * Output volume, 0-100 percent. 0 is silence.
 *
 * Applied to the codec immediately and remembered, so it survives the
 * reconfiguration between plays.
 */
/*
 * What to ring at when the stored setting cannot be read at all. Loud
 * enough to wake somebody: design 13 would rather be wrong about the
 * volume than silent about the alarm.
 */
#define NN20CLOCK_AUDIO_FALLBACK_VOLUME 70u

esp_err_t nn20clock_audio_set_volume(NN20ClockAudio *pthis, uint8_t percent);

uint8_t nn20clock_audio_volume(const NN20ClockAudio *pthis);

/*
 * How often a ramp moves, in milliseconds.
 *
 * Every move is an I2C register write on the bus the touch controller
 * shares, so a ramp is walked in steps rather than followed
 * continuously: a step a second is seven writes for a whole fade-in.
 * The codec's volume is coarse enough that nothing smoother would be
 * heard for the traffic it cost.
 */
#define NN20CLOCK_AUDIO_FADE_STEP_MS 1000u

/*
 * Come up to `to` from `from` over `ms`, instead of being there
 * already. This is design 13's alarm that does not start at full
 * volume.
 *
 * `to` becomes the volume, immediately and for _volume() to read back:
 * the levels on the way up are not settings anybody chose, so nothing
 * outside this component ever sees one and nothing can store one. Only
 * the register the codec plays at moves.
 *
 * The ramp advances in the write path - it exists to shape sound that
 * is going out, and both things that make sound come through there -
 * so it does not run while nothing is playing and cannot outlive the
 * playback it belongs to. It ends by itself at `to`, and anyone
 * setting the volume by hand ends it early: _set_volume() cancels a
 * ramp in progress, which is what makes the slider work during the
 * first seconds of an alarm.
 *
 * `from` at or above `to`, or `ms` shorter than one step, is not a
 * fade-in and is not treated as one: the volume simply goes to `to`.
 * Somebody whose setting is quieter than the starting point wants it
 * quiet, and coming down to it from louder would be worse than not
 * ramping at all.
 *
 * Call it instead of _set_volume(), not before or after it.
 */
esp_err_t nn20clock_audio_fade_volume(NN20ClockAudio *pthis, uint8_t from,
                                      uint8_t to, uint32_t ms);

#ifdef __cplusplus
}
#endif

#endif /* NN20CLOCK_AUDIO_H */
