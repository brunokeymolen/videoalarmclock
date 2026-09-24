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
 * nn20clock_target_test_main.c - runs the component suites on the board.
 *
 * The suites in test/ define main() through TEST_MAIN. The build renames
 * each one (see this directory's CMakeLists.txt) so they can be called in
 * turn from app_main and their results printed over serial.
 *
 * Use this when something is genuinely target-specific - FreeRTOS task
 * behavior, PSRAM, alignment, real toolchain codegen. The host run
 * (tools/test.sh) covers the same assertions in seconds.
 */
#include "esp_heap_caps.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "nn20clock_platform.h"

static const char *TAG = "NN20CLOCK_TEST";

/* Renamed main() of each suite. Add one line per suite. */
int nn20clock_test_main_workers(void);
int nn20clock_test_main_timer(void);
int nn20clock_test_main_timefmt(void);
int nn20clock_test_main_alarm(void);
int nn20clock_test_main_media(void);
int nn20clock_test_main_sd(void);
int nn20clock_test_main_avi(void);
int nn20clock_test_main_ringbuf(void);
int nn20clock_test_main_storage(void);
int nn20clock_test_main_ui(void);
int nn20clock_test_main_ota(void);
int nn20clock_test_main_manager(void);
int nn20clock_test_main_app(void);

typedef struct {
    const char *name;
    int (*run)(void);
} TargetSuite;

/* Bottom-up, same order the components are built in: if the worker set
 * is broken, its failure should be the first thing on the monitor. */
static const TargetSuite SUITES[] = {
    { "nn20clock_workers", nn20clock_test_main_workers },
    { "nn20clock_timer",   nn20clock_test_main_timer },
    { "nn20clock_timefmt", nn20clock_test_main_timefmt },
    { "nn20clock_alarm",   nn20clock_test_main_alarm },
    { "nn20clock_media",   nn20clock_test_main_media },
    /* The one suite with no host counterpart: it needs a real card.
     * With no card in the slot it says so and passes. */
    { "nn20clock_sd",      nn20clock_test_main_sd },
    { "nn20clock_avi",     nn20clock_test_main_avi },
    { "nn20clock_ringbuf", nn20clock_test_main_ringbuf },
    { "nn20clock_storage", nn20clock_test_main_storage },
    { "nn20clock_ui",      nn20clock_test_main_ui },
    { "nn20clock_ota",     nn20clock_test_main_ota },
    { "nn20clock_manager", nn20clock_test_main_manager },
    { "nn20clock_app",     nn20clock_test_main_app },
};

void app_main(void)
{
    ESP_LOGI(TAG, "NN20Clock component tests, on target");

    int failed_suites = 0;
    for (size_t i = 0; i < sizeof(SUITES) / sizeof(SUITES[0]); i++) {
        /*
         * Internal RAM per suite, because this binary is where it runs
         * out first: it links every suite's fixtures at once and the
         * app suite then builds the whole application several times
         * over. A suite that fails for want of memory looks like a
         * suite that fails, and the two want very different fixes.
         */
        ESP_LOGI(TAG, "---- %s ---- (internal heap %u KB, largest block %u KB)",
                 SUITES[i].name,
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)
                            / 1024U),
                 (unsigned)(heap_caps_get_largest_free_block(
                                MALLOC_CAP_INTERNAL) / 1024U));
        if (SUITES[i].run() != 0) {
            failed_suites++;
        }
    }

    ESP_LOGI(TAG, "internal heap after all suites: %u KB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024U));

    if (failed_suites == 0) {
        ESP_LOGI(TAG, "PASS: all %u suite(s)",
                 (unsigned)(sizeof(SUITES) / sizeof(SUITES[0])));
    } else {
        ESP_LOGE(TAG, "FAIL: %d suite(s)", failed_suites);
    }

    /* Hold, so the result stays on screen instead of scrolling past a
     * reset loop. */
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
