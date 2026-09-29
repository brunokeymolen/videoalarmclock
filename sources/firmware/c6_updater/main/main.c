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
 * c6_updater - push the embedded esp_hosted image to the ESP32-C6.
 *
 * A one-off, not part of the clock. It brings up the same esp_hosted
 * link the clock uses, asks the C6 which firmware it runs, and only if
 * that is older than 3.x streams the image over with the co-processor
 * OTA calls (begin, write, end, activate). Then it restarts once so the
 * C6 is reset and its new version can be read back, and idles, printing
 * the result, until the clock is flashed again.
 *
 * Nothing is written to the P4's own NVS: the clock's Wi-Fi and alarms
 * are still there when it goes back on.
 */
#include <inttypes.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_attr.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"

#include "eh_host_api_types.h"
#include "eh_host_core.h"
#include "eh_host_cp_ota.h"
#include "eh_host_sys.h"

static const char *TAG = "C6_UPDATE";

/* The first major version with the protocol the clock's host speaks
 * natively. Anything at or above it is left alone. */
#define CURRENT_MAJOR 3u

/* Well under EH_RPC_OTA_CHUNK_MAX (1536). */
#define CHUNK_BYTES 1024u

extern const uint8_t c6_image_start[] asm("_binary_c6_cp_bin_start");
extern const uint8_t c6_image_end[]   asm("_binary_c6_cp_bin_end");

/*
 * Survives esp_restart() but not a power cycle: the update is tried at
 * most once per power-up, so an image that does not take cannot turn
 * this into a loop of writes to the C6's flash.
 */
#define ATTEMPTED_MAGIC 0xC6C6A11Du
static RTC_NOINIT_ATTR uint32_t s_attempted;

static void idle_forever(const char *result)
{
    for (;;) {
        ESP_LOGW(TAG, "RESULT: %s", result);
        ESP_LOGW(TAG, "Now reinstall the clock: sources/tools/flash.sh");
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

/* True and fills *major if the C6 answered the version request. Firmware
 * older than the request itself does not answer at all. */
static bool read_version(uint32_t *major)
{
    eh_host_coprocessor_fwver_t v = {0};
    const esp_err_t err = eh_host_sys_get_cp_fw_version(&v);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "C6 firmware version: not reported (old firmware)");
        return false;
    }
    ESP_LOGW(TAG, "C6 firmware version: %" PRIu32 ".%" PRIu32 ".%" PRIu32,
             v.major1, v.minor1, v.patch1);
    *major = v.major1;
    return true;
}

void app_main(void)
{
    /* Deliberately no erase on failure: this NVS is the clock's. */
    const esp_err_t nvs = nvs_flash_init();
    if (nvs != ESP_OK) {
        ESP_LOGW(TAG, "NVS: %s (not needed here, carrying on)",
                 esp_err_to_name(nvs));
    }
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    if (eh_host_init(NULL) != 0 || eh_host_connect_to_slave() != 0) {
        idle_forever("FAILED - no link to the C6. Nothing was written.");
    }
    vTaskDelay(pdMS_TO_TICKS(1000));   /* let the C6 finish its boot */

    uint32_t major = 0;
    const bool reported = read_version(&major);

    if (s_attempted == ATTEMPTED_MAGIC) {
        s_attempted = 0;
        idle_forever((reported && major >= CURRENT_MAJOR)
                         ? "UPDATED - the C6 runs the new firmware."
                         : "FAILED - the C6 did not switch to the new "
                           "firmware. It still runs the old one.");
    }
    if (reported && major >= CURRENT_MAJOR) {
        idle_forever("NOTHING TO DO - the C6 is already up to date.");
    }

    const size_t total = (size_t)(c6_image_end - c6_image_start);
    ESP_LOGW(TAG, "updating the C6: %u bytes - do not unplug the board",
             (unsigned)total);
    s_attempted = ATTEMPTED_MAGIC;

    if (eh_host_cp_ota_begin() != ESP_OK) {
        s_attempted = 0;
        idle_forever("FAILED - the C6 refused the update. "
                     "Nothing was written; it still runs the old firmware.");
    }

    for (size_t off = 0; off < total; off += CHUNK_BYTES) {
        const size_t n = (total - off < CHUNK_BYTES) ? total - off
                                                     : CHUNK_BYTES;
        if (eh_host_cp_ota_write(c6_image_start + off, (uint32_t)n)
            != ESP_OK) {
            ESP_LOGE(TAG, "write failed at byte %u", (unsigned)off);
            idle_forever("FAILED - transfer interrupted. The C6 keeps "
                         "running its old firmware.");
        }
        if ((off / CHUNK_BYTES) % 100 == 0) {
            ESP_LOGI(TAG, "%3u%%", (unsigned)(off * 100 / total));
        }
    }

    /* end() has the C6 verify the image; only then is it bootable. */
    if (eh_host_cp_ota_end() != ESP_OK) {
        idle_forever("FAILED - the C6 rejected the image. It keeps "
                     "running its old firmware.");
    }
    ESP_LOGW(TAG, "image written and verified by the C6");

    /*
     * Firmware from 2.6 on waits for activate; older firmware already
     * switched in end() and has no activate, so a failure here is
     * expected on exactly the boards this is for. The restart below
     * reads the version back either way.
     */
    const esp_err_t act = eh_host_cp_ota_activate();
    ESP_LOGI(TAG, "activate: %s%s", esp_err_to_name(act),
             (act != ESP_OK) ? " (expected on old firmware)" : "");

    ESP_LOGW(TAG, "restarting to reset the C6 and read its version back");
    (void)eh_host_deinit();
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
}
