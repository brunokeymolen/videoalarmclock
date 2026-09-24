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
 * nn20clock_app_main.c - the entry point.
 *
 * It does two things: it builds NN20ClockApp, and it watches it. All the
 * wiring lives in the app (design 4's CoreWorker), so this file stays
 * the same size as the clock grows.
 *
 * What comes up: the four workers, Storage on the StorageWorker, the
 * panel, LVGL and touch on the UiWorker, the Timer on its own worker,
 * ClockManager reaching design 5's TIME state and showing TimeUi, and
 * Wi-Fi with SNTP last. Alarms are scheduled and editable on screen;
 * a ringing alarm still plays no media (Milestone 7), and device
 * settings have no screen yet (Milestone 5).
 *
 * If a thread will not start, the panel will not come up, or the
 * services cannot be wired in dependency order, this is where it shows
 * up: on the serial monitor, at boot.
 */
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_system.h"

#include "nn20clock_app.h"
#include "nn20clock_platform.h"

static const char *TAG = "NN20CLOCK";

/* How often the heartbeat below reports in. Long enough not to drown the
 * monitor, short enough to notice a silent reset. */
#define HEARTBEAT_SECONDS 10

static void log_board_summary(void)
{
    esp_chip_info_t chip = {0};
    esp_chip_info(&chip);

    uint32_t flash_bytes = 0;
    if (esp_flash_get_physical_size(NULL, &flash_bytes) != ESP_OK) {
        flash_bytes = 0;
    }

    ESP_LOGI(TAG, "ESP32-P4, %d core(s), silicon revision v%d.%d",
             chip.cores, chip.revision / 100, chip.revision % 100);
    ESP_LOGI(TAG, "flash: %" PRIu32 " MB", flash_bytes / (1024U * 1024U));
    ESP_LOGI(TAG, "PSRAM: %u KB free",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024U));
    ESP_LOGI(TAG, "internal heap: %u KB free",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024U));
}

/*
 * Nothing here recovers from a failure. Milestone 12 owns recovery; for
 * now a dead app should stay dead and visible on the monitor rather than
 * reboot-looping past the evidence.
 */
static void heartbeat(NN20ClockApp *app)
{
    for (uint32_t seconds = 0;; seconds += HEARTBEAT_SECONDS) {
        ESP_LOGI(TAG, "alive, %" PRIu32 "s, internal heap %u KB", seconds,
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)
                            / 1024U));

        /*
         * The expensive half of the heartbeat, and it does not run
         * while something is playing.
         *
         * Both parts below block for far longer than the player can
         * afford. The integrity walk was measured at 220 ms - it
         * crosses every block of every heap, 31 MB of PSRAM included -
         * and the status block is a dozen lines out of a 115200 baud
         * port, another 75. The player's thread has 32.6 ms of audio
         * buffered under it and a frame due every 50, so either one
         * empties the I2S buffer into an audible tick and costs four
         * frames on the panel. Every ten seconds, for as long as a film
         * runs. That is what this guard is for; turning the poisoning
         * off (see sdkconfig.defaults) made the walk cheaper but did
         * not make it free, and the log block was never about
         * poisoning at all.
         *
         * Skipped, not cancelled: the next tick after playback ends
         * does it. Nothing is being watched any less closely than it
         * was, only later - and a film is at most an hour of ticks.
         *
         * The `alive` line above is deliberately outside this. It is
         * one short line, it is how a silent reset is noticed, and
         * playback is exactly when you would want to know.
         */
        const bool playing = nn20clock_app_is_playing(app);

        /*
         * Heap corruption is silent until something unrelated allocates
         * and falls over a damaged free list - which is exactly how it
         * first showed up here: a panic inside LVGL's glyph allocation,
         * nowhere near whatever did the damage. Checking here turns
         * that into a report at the next heartbeat, with the offending
         * block named, ten seconds from the write instead of minutes
         * and one stack trace away from it.
         */
        if (!playing && !heap_caps_check_integrity_all(true)) {
            ESP_LOGE(TAG, "heap corrupt at %" PRIu32 "s", seconds);
        }

        if (app != NULL) {
            /*
             * Supervision runs either way: it is a handful of flag
             * reads behind one post, it is cheap enough not to be
             * heard, and an hour-long alarm is not an hour with nobody
             * watching the workers.
             */
            if (nn20clock_app_supervise(app) != ESP_OK) {
                ESP_LOGE(TAG, "supervision found a problem");
            }
            if (!playing) {
                nn20clock_app_log_status(app);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_SECONDS * 1000));
    }
}

void app_main(void)
{
    /*
     * The version comes from the build, not from a string kept up to
     * date by hand - ESP-IDF derives PROJECT_VER from `git describe`, so
     * this says v0.1.0 on the tag and v0.1.0-2-gb741df4-dirty two
     * commits later. A board that can be asked what is on it is worth
     * more than a tidy banner.
     */
    ESP_LOGI(TAG, "Video Alarm Clock %s",
             esp_app_get_description()->version);
    log_board_summary();

    NN20ClockApp *app = nn20clock_app_ctor();
    if (app == NULL) {
        ESP_LOGE(TAG, "application construction FAILED");
        heartbeat(NULL);   /* keep the board talking so the log is read */
        return;
    }

    if (nn20clock_app_start(app) != ESP_OK) {
        ESP_LOGE(TAG, "application start FAILED");
        nn20clock_app_log_status(app);
        heartbeat(NULL);
        return;
    }

    ESP_LOGI(TAG, "application running in state %s",
             nn20clock_manager_state_name(
                 nn20clock_manager_state(nn20clock_app_manager(app))));
    /*
     * The clock is showing a time either way. Whether it is the *right*
     * time depends on NTP, which is still connecting at this point - so
     * say which it is rather than let the screen imply certainty.
     */
    ESP_LOGI(TAG, "clock face up; time %s",
             nn20clock_timer_is_time_synced(nn20clock_app_timer(app))
                 ? "synced"
                 : "from the RTC, not yet synced");
    ESP_LOGI(TAG, "%u alarm(s) scheduled; tap the screen for the icons",
             (unsigned)nn20clock_timer_alarm_count(nn20clock_app_timer(app)));

    /* Never returns. The app is deliberately not destroyed: there is no
     * shutdown path on a clock, and freeing it here would only hide a
     * teardown bug behind a board that has stopped doing anything. */
    heartbeat(app);
}
