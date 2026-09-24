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
 * nn20clock_display.h - the 720x720 panel and LVGL (design 11).
 *
 * FIRMWARE ONLY. This is the ST7703 over MIPI-DSI, the pin numbers for
 * this board, and LVGL itself; none of it means anything on a desktop,
 * so it is absent from the host build rather than shimmed. The panel
 * bring-up is the sequence proven in tryout/videoplayback, which is
 * where the DSI, LDO, and timing values were established on real
 * hardware.
 *
 * Threading is the whole point of this component. Design 11 requires
 * every LVGL call to happen on one thread, and here that thread is the
 * UiWorker:
 *
 *   - lv_init(), display creation, and every later lv_* call are posted
 *     to the UiWorker and run there.
 *   - lv_timer_handler() is driven by an esp_timer that posts to the
 *     UiWorker; the timer callback itself touches no LVGL.
 *   - Screens draw from their show()/hide()/handle_* hooks, which the
 *     UiBase dispatcher already runs on the UiWorker.
 *
 * So there is no lvgl_port_lock() here and no mutex of any kind. That
 * is deliberate: a lock would let two threads take turns inside LVGL,
 * which is exactly what design 11 forbids. LVGL is configured
 * single-threaded and stays that way by ownership.
 *
 * Touch is not wired yet - Milestone 4, when there is a settings screen
 * to touch. UiBase already routes touch events, so the seam exists.
 */
#ifndef NN20CLOCK_DISPLAY_H
#define NN20CLOCK_DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"

#include "nn20clock_platform.h"
#include "worker.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NN20ClockDisplay NN20ClockDisplay;

/* The panel, from tryout/videoplayback's proven configuration. */
#define NN20CLOCK_DISPLAY_WIDTH  720
#define NN20CLOCK_DISPLAY_HEIGHT 720

/* --------------------------------------------------------- lifecycle -- */

/*
 * ui_worker is design 4's UiWorker, borrowed, and must outlive this.
 * NULL is rejected: a display without the shared UI thread has no safe
 * way to call LVGL.
 *
 * Constructing does not touch the hardware; _start() does.
 */
NN20ClockDisplay *nn20clock_display_ctor(nn20_worker_ctx *ui_worker);

/* Stops first if needed. Safe with NULL. */
void nn20clock_display_dtor(NN20ClockDisplay *pthis);

/*
 * Power the panel, initialize LVGL, and begin the refresh timer. Runs
 * on the UiWorker and waits for it, so when this returns LVGL is usable
 * and screens may be shown.
 */
esp_err_t nn20clock_display_start(NN20ClockDisplay *pthis);

/* Stop refreshing and blank the panel. Idempotent. */
esp_err_t nn20clock_display_stop(NN20ClockDisplay *pthis);

bool nn20clock_display_is_running(const NN20ClockDisplay *pthis);

/*
 * Backlight brightness, 0-100 percent. 0 is off.
 *
 * The backlight is driven by LEDC PWM rather than a GPIO level, so this
 * is a real dimmer: DeviceSettingsUi moves it live as the slider is
 * dragged, and the stored default is applied at boot.
 *
 * Below NN20CLOCK_DISPLAY_MIN_ON_BRIGHTNESS but above zero, the value is
 * raised to that floor - a panel dimmed to 1% looks broken rather than
 * dim, and someone who wanted it off would have chosen off.
 */
esp_err_t nn20clock_display_set_brightness(NN20ClockDisplay *pthis,
                                           uint8_t percent);

/* The brightness currently applied, 0-100. */
uint8_t nn20clock_display_brightness(const NN20ClockDisplay *pthis);

/*
 * Anything dimmer than this, but not off, is raised to it.
 *
 * 25, so the panel can be dimmed far enough to live with at night -
 * which is the point of a bedside clock.
 *
 * This number is only reachable because of the backlight PWM frequency.
 * At the 5 kHz this started out with, the panel went black in the low
 * forties and 50 was taken as its floor; at 240 Hz it dims smoothly to
 * 14. The cliff was the drive frequency, not the hardware - see
 * BACKLIGHT_LEDC_HZ in the source, which has the measurements. Anyone
 * changing that frequency must re-measure this floor, because it is
 * derived from it rather than from the panel.
 *
 * 25 rather than 14 leaves margin. 14 is where this one board stopped;
 * a floor sitting exactly on a measured cliff has nothing left for
 * another panel or a cold morning.
 *
 * A floor exists at all because of what is below it: a screen you
 * cannot read is a device you cannot recover through its own settings,
 * which is exactly where the brightness control lives.
 */
#define NN20CLOCK_DISPLAY_MIN_ON_BRIGHTNESS 14u

/*
 * The floor applied at startup, above the one above.
 *
 * A stored brightness is applied at boot, and a value that leaves the
 * panel unreadable would make the device unrecoverable without a
 * reflash. Coming up brighter than the user asked is a small annoyance;
 * coming up invisible is a brick. The startup path raises anything
 * lower to this and writes it back, so the screen and the stored
 * setting agree.
 *
 * It stays well clear of the floor above rather than tracking it. The
 * dimmest the panel can go is a setting somebody chose while looking at
 * it; the dimmest it may come up on its own is a recovery guarantee,
 * and those are different questions. A clock left at 25 overnight comes
 * up at 50 after a power cut, which is readable from across a room.
 */
#define NN20CLOCK_DISPLAY_BOOT_MIN_BRIGHTNESS 20u

/* Convenience for full on / off; equivalent to 100 and 0 percent. */
esp_err_t nn20clock_display_set_backlight(NN20ClockDisplay *pthis, bool on);

/*
 * The board's I2C bus, borrowed.
 *
 * The audio codec and the touch controller are on the same two pins
 * (design 2), and ESP-IDF allows exactly one owner of a bus. The
 * display creates it because it starts first and needs it for touch;
 * nn20clock_audio takes it from here rather than creating a second one,
 * which would fail.
 *
 * The handle is valid from _start() until the dtor, so a borrower must
 * be constructed after the display and destroyed before it - which is
 * what design 4's bottom-up construction and reverse teardown already
 * give, as long as audio is placed after display in the composition
 * root.
 *
 * NULL before _start(), or if the bus could not be brought up - in
 * which case there is no touch and no sound, and the clock still tells
 * the time.
 */
i2c_master_bus_handle_t nn20clock_display_i2c_bus(const NN20ClockDisplay *pthis);

/* ------------------------------------------------------------ video -- */

/*
 * Hand the panel to the video player (design 17, Milestone 7).
 *
 * The panel has exactly one owner, as it must: two writers is what
 * produced the missing bands at Milestone 2. So while video is playing,
 * LVGL stops redrawing entirely and the player draws whole frames with
 * _video_draw(). Nothing here takes a lock - the drawing still happens
 * on the UiWorker, it is simply a different thing being drawn.
 *
 * LVGL not running also means LVGL's touch handling is not running, and
 * the ringing screen has to be dismissable. So touch is reduced to one
 * question - was the screen touched - answered by
 * _video_take_touch(). When the answer is yes the caller switches to
 * its buttons, which means ending video mode and letting LVGL draw them
 * properly.
 *
 * Both handovers pause briefly for the panel to settle; see the source.
 */
esp_err_t nn20clock_display_video_begin(NN20ClockDisplay *pthis);

/* Give the panel back and mark the screen for a full redraw. Idempotent. */
esp_err_t nn20clock_display_video_end(NN20ClockDisplay *pthis);

bool nn20clock_display_video_active(const NN20ClockDisplay *pthis);

/*
 * Draw one full 720x720 RGB565 frame.
 *
 * `pixels` must hold a whole frame and stay put until this returns.
 * A frame arriving while the previous one is still being copied is
 * dropped rather than waited for, and reported as ESP_OK: at 15 frames
 * a second the next one is already on its way, and a video that
 * stutters beats a video that falls behind.
 */
esp_err_t nn20clock_display_video_draw(NN20ClockDisplay *pthis,
                                       const void *pixels);

/*
 * Whether the screen was touched since this was last asked. Reading
 * clears it, so a touch is reported exactly once.
 */
bool nn20clock_display_video_take_touch(NN20ClockDisplay *pthis);

/*
 * Whether a finger is on the glass at this moment.
 *
 * Only tracked while the player owns the panel, which is the one place
 * it matters: handing the panel back mid-press means LVGL resumes,
 * finds a press already in progress, and delivers a click nobody made
 * to whatever ends up under the finger. Wait for this to go false
 * before ending video mode.
 */
bool nn20clock_display_touch_is_down(const NN20ClockDisplay *pthis);

/*
 * Frames LVGL has flushed since start. The cheapest evidence that the
 * display pipeline is alive, which is why the heartbeat logs it.
 */
uint32_t nn20clock_display_flush_count(const NN20ClockDisplay *pthis);

#ifdef __cplusplus
}
#endif

#endif /* NN20CLOCK_DISPLAY_H */
