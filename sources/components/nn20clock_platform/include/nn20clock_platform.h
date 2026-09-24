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
 * nn20clock_platform.h - the ESP-IDF / host build seam.
 *
 * Every NN20Clock component includes this instead of <esp_err.h> and
 * <esp_log.h> directly. On the target it is those two headers. On a
 * desktop it supplies just enough of them that the logic components
 * compile and run under ctest without any part of ESP-IDF present.
 *
 * This is what lets the fast inner loop (tools/test.sh, seconds, no
 * board) test the same sources that the slow confirmation
 * (tools/idf.sh build) compiles for the ESP32-P4. Only components that
 * are genuinely hardware-bound - display, touch, codec, SD - should be
 * firmware-only; everything else belongs on both sides of this seam.
 *
 * Keep the host side minimal and honest: add a shim only when a
 * component actually needs the symbol, and give it the same semantics
 * ESP-IDF does. A shim that quietly behaves differently from the target
 * is worse than no host build at all.
 */
#ifndef NN20CLOCK_PLATFORM_H
#define NN20CLOCK_PLATFORM_H

#if defined(ESP_PLATFORM)

#include "esp_err.h"
#include "esp_log.h"

#else /* ------------------------------------------------ host build -- */

#include <stdio.h>

typedef int esp_err_t;

/* Values match ESP-IDF's, so a test asserting on a code is asserting on
 * the same number the firmware returns. See esp_err.h. */
#define ESP_OK                  0
#define ESP_FAIL                (-1)
#define ESP_ERR_NO_MEM          0x101
#define ESP_ERR_INVALID_ARG     0x102
#define ESP_ERR_INVALID_STATE   0x103
#define ESP_ERR_INVALID_SIZE    0x104
#define ESP_ERR_NOT_FOUND       0x105
#define ESP_ERR_NOT_SUPPORTED   0x106
#define ESP_ERR_TIMEOUT         0x107

/* Logging goes to stdout on the host. The level prefix is kept so test
 * output reads the same way the serial monitor does. */
#define NN20CLOCK_HOST_LOG(level, tag, fmt, ...) \
    printf(level " (%s) " fmt "\n", tag, ##__VA_ARGS__)

#define ESP_LOGE(tag, fmt, ...) NN20CLOCK_HOST_LOG("E", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) NN20CLOCK_HOST_LOG("W", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) NN20CLOCK_HOST_LOG("I", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) NN20CLOCK_HOST_LOG("D", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGV(tag, fmt, ...) NN20CLOCK_HOST_LOG("V", tag, fmt, ##__VA_ARGS__)

#endif /* ESP_PLATFORM */

#endif /* NN20CLOCK_PLATFORM_H */
