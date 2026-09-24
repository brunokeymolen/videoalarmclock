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
 * nn20clock_ota.c - see nn20clock_ota.h.
 *
 * Two operations, one worker, and a published snapshot the UI reads.
 */
#include "nn20clock_ota.h"
#include "nn20clock_ota_version.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "mbedtls/sha256.h"
#include "worker.h"

static const char *TAG = "NN20CLOCK_OTA";

#ifndef CONFIG_NN20CLOCK_OTA_MANIFEST_URL
#define CONFIG_NN20CLOCK_OTA_MANIFEST_URL ""
#endif

/*
 * A manifest is a few hundred bytes. The cap is here so a wrong URL
 * pointing at something enormous is refused rather than read into the
 * heap a chunk at a time until it runs out.
 */
#define MANIFEST_MAX 2048

/*
 * How much of the image is read from the socket and handed to
 * esp_ota_write() at a time. Larger buffers stop helping well before
 * this - the transfer is bounded by TLS and the network, not by the
 * number of flash writes.
 */
#define IMAGE_CHUNK 4096

/* An image smaller than this is not a firmware, whatever the manifest
 * says. The smallest thing this project has ever built is over 1 MB. */
#define IMAGE_MIN_BYTES (256u * 1024u)

/* GitHub answers the release-asset URL with a redirect to its object
 * store, which redirects once more. Five is room to spare; it is a
 * bound so a redirect loop ends in an error rather than a hang. */
#define MAX_REDIRECTS 5

#define HTTP_TIMEOUT_MS 20000

#define SHA256_HEX_LEN 64

/* What a parsed manifest holds. OtaWorker-owned; never read off it. */
typedef struct {
    char version[NN20CLOCK_OTA_VERSION_MAX];
    char notes[NN20CLOCK_OTA_NOTES_MAX];
    char url[256];
    uint32_t size;
    char sha256[SHA256_HEX_LEN + 1];
} Release;

struct NN20ClockOta {
    nn20_worker_ctx *worker;   /* owned; created in the ctor */

    /* OtaWorker-owned: only its callbacks read or write this. */
    Release offered;

    /*
     * The published snapshot, for _status().
     *
     * These are atomics rather than plain fields because they are
     * written on the OtaWorker and read on the UiWorker, and design 4
     * allows exactly that use - a snapshot read of a flag or counter
     * from another thread. They are not a lock and nothing waits on
     * them.
     *
     * `pub_version` and `pub_notes` are not atomic and cannot be. They
     * are published by ordering instead: the OtaWorker writes them only
     * while `state` reads CHECKING, and stores the new state afterwards
     * with release ordering; a reader loads `state` with acquire and
     * copies the strings only when it did not see CHECKING. The header
     * states the one constraint that makes that airtight - a single
     * calling thread, so a check cannot start between a reader's load
     * and its copy.
     */
    atomic_int state;
    atomic_uint percent;
    atomic_int error;
    char pub_version[NN20CLOCK_OTA_VERSION_MAX];
    char pub_notes[NN20CLOCK_OTA_NOTES_MAX];
};

const char *nn20clock_ota_state_name(NN20ClockOtaState state)
{
    switch (state) {
    case NN20CLOCK_OTA_IDLE:        return "idle";
    case NN20CLOCK_OTA_CHECKING:    return "checking";
    case NN20CLOCK_OTA_UP_TO_DATE:  return "up to date";
    case NN20CLOCK_OTA_AVAILABLE:   return "available";
    case NN20CLOCK_OTA_DOWNLOADING: return "downloading";
    case NN20CLOCK_OTA_INSTALLED:   return "installed";
    case NN20CLOCK_OTA_FAILED:      return "failed";
    }
    return "unknown";
}

const char *nn20clock_ota_running_version(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    return (app != NULL) ? app->version : "unknown";
}

/* ------------------------------------------------------ publication -- */

static NN20ClockOtaState load_state(const NN20ClockOta *pthis)
{
    return (NN20ClockOtaState)atomic_load_explicit(&pthis->state,
                                                   memory_order_acquire);
}

static void publish_state(NN20ClockOta *pthis, NN20ClockOtaState state)
{
    atomic_store_explicit(&pthis->state, (int)state, memory_order_release);
}

static void publish_failure(NN20ClockOta *pthis, esp_err_t err)
{
    atomic_store_explicit(&pthis->error, (int)err, memory_order_relaxed);
    publish_state(pthis, NN20CLOCK_OTA_FAILED);
}

void nn20clock_ota_status(const NN20ClockOta *pthis, NN20ClockOtaStatus *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (pthis == NULL) {
        out->state = NN20CLOCK_OTA_FAILED;
        out->error = ESP_ERR_INVALID_ARG;
        return;
    }

    out->state = load_state(pthis);
    out->percent = (uint8_t)atomic_load_explicit(&pthis->percent,
                                                 memory_order_relaxed);
    out->error = (esp_err_t)atomic_load_explicit(&pthis->error,
                                                 memory_order_relaxed);

    /* The acquire above is what makes these safe to read: the strings
     * were written before the state that says they are meaningful. */
    if (out->state != NN20CLOCK_OTA_CHECKING &&
        out->state != NN20CLOCK_OTA_IDLE) {
        snprintf(out->version, sizeof(out->version), "%s", pthis->pub_version);
        snprintf(out->notes, sizeof(out->notes), "%s", pthis->pub_notes);
    }
}

/* -------------------------------------------------------- versions -- */

/*
 * Whether `offered` is a release the running image is not.
 *
 * The comparison itself lives in nn20clock_ota_version.c, which is
 * portable and tested on the host; what is here is only what to say
 * about each verdict.
 */
static bool is_newer(const char *offered, const char *running)
{
    switch (nn20clock_ota_compare_versions(offered, running)) {
    case NN20CLOCK_OTA_VERSION_NEWER:
        return true;
    case NN20CLOCK_OTA_VERSION_OFFER_UNREADABLE:
        ESP_LOGW(TAG, "manifest version '%s' is not vMAJOR.MINOR.PATCH",
                 (offered != NULL) ? offered : "(none)");
        return false;
    case NN20CLOCK_OTA_VERSION_RUNNING_UNREADABLE:
        ESP_LOGW(TAG, "running version '%s' is not a tag; offering %s",
                 (running != NULL) ? running : "(none)", offered);
        return true;
    case NN20CLOCK_OTA_VERSION_NOT_NEWER:
        break;
    }
    return false;
}

/* ------------------------------------------------------------- HTTP -- */

/*
 * Open `client` and follow redirects until something that is not one
 * answers.
 *
 * esp_http_client follows redirects by itself only under
 * esp_http_client_perform(), which reads the whole body into memory.
 * An eight-megabyte firmware cannot be read that way, so the streaming
 * API is used and the redirects are followed here - which is what
 * esp_https_ota does internally for the same reason.
 */
static esp_err_t open_following_redirects(esp_http_client_handle_t client,
                                          int64_t *out_length)
{
    for (int hop = 0; hop <= MAX_REDIRECTS; hop++) {
        esp_err_t err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "connect failed (%s)", esp_err_to_name(err));
            return err;
        }

        const int64_t length = esp_http_client_fetch_headers(client);
        if (length < 0) {
            ESP_LOGE(TAG, "no response headers");
            esp_http_client_close(client);
            return ESP_ERR_INVALID_RESPONSE;
        }

        const int status = esp_http_client_get_status_code(client);
        if (status == 301 || status == 302 || status == 303 ||
            status == 307 || status == 308) {
            err = esp_http_client_set_redirection(client);
            esp_http_client_close(client);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "cannot follow redirect (%s)",
                         esp_err_to_name(err));
                return err;
            }
            continue;
        }

        if (status != 200) {
            ESP_LOGE(TAG, "server answered HTTP %d", status);
            esp_http_client_close(client);
            return ESP_ERR_INVALID_RESPONSE;
        }

        if (out_length != NULL) {
            *out_length = length;
        }
        return ESP_OK;
    }

    ESP_LOGE(TAG, "too many redirects");
    return ESP_ERR_INVALID_RESPONSE;
}

static esp_http_client_handle_t make_client(const char *url)
{
    const esp_http_client_config_t config = {
        .url = url,
        /* The compiled-in root bundle. Without it every fetch fails
         * closed, which is the right direction to fail. */
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .keep_alive_enable = true,
        /* GitHub's object-store URLs carry a long signed query string
         * and do not fit the 512-byte default. */
        .buffer_size_tx = 2048,
    };
    return esp_http_client_init(&config);
}

/* --------------------------------------------------------- manifest -- */

static void copy_string_field(const cJSON *root, const char *name, char *out,
                              size_t size)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        snprintf(out, size, "%s", item->valuestring);
    } else {
        out[0] = '\0';
    }
}

static esp_err_t parse_manifest(const char *text, Release *out)
{
    cJSON *root = cJSON_Parse(text);
    if (root == NULL) {
        ESP_LOGE(TAG, "the manifest is not JSON");
        return ESP_ERR_INVALID_RESPONSE;
    }

    memset(out, 0, sizeof(*out));
    copy_string_field(root, "version", out->version, sizeof(out->version));
    copy_string_field(root, "notes", out->notes, sizeof(out->notes));
    copy_string_field(root, "url", out->url, sizeof(out->url));
    copy_string_field(root, "sha256", out->sha256, sizeof(out->sha256));

    const cJSON *size = cJSON_GetObjectItemCaseSensitive(root, "size");
    if (cJSON_IsNumber(size) && size->valuedouble > 0.0) {
        out->size = (uint32_t)size->valuedouble;
    }

    cJSON_Delete(root);

    /*
     * Every field is required. A manifest missing its checksum is not a
     * manifest this device will act on - the checksum is the whole
     * reason for having a manifest rather than reading GitHub's API.
     */
    if (out->version[0] == '\0' || out->url[0] == '\0') {
        ESP_LOGE(TAG, "the manifest names no version or no image");
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (strlen(out->sha256) != SHA256_HEX_LEN) {
        ESP_LOGE(TAG, "the manifest carries no usable sha256");
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (out->size < IMAGE_MIN_BYTES) {
        ESP_LOGE(TAG, "the manifest claims a %" PRIu32 "-byte firmware",
                 out->size);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (strncmp(out->url, "https://", 8) != 0) {
        ESP_LOGE(TAG, "the image URL is not https");
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

static esp_err_t fetch_manifest(Release *out)
{
    const char *url = CONFIG_NN20CLOCK_OTA_MANIFEST_URL;
    if (url[0] == '\0') {
        ESP_LOGE(TAG, "no manifest URL is configured");
        return ESP_ERR_INVALID_STATE;
    }
    /* Checked here as well as in Kconfig's help text, because the
     * manifest is what names the image: fetching it over a channel
     * anyone on the path can rewrite would make the checksum inside it
     * worth nothing. */
    if (strncmp(url, "https://", 8) != 0) {
        ESP_LOGE(TAG, "the manifest URL is not https");
        return ESP_ERR_INVALID_STATE;
    }

    esp_http_client_handle_t client = make_client(url);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    char *body = calloc(1, MANIFEST_MAX + 1);
    if (body == NULL) {
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    int64_t length = 0;
    esp_err_t err = open_following_redirects(client, &length);
    if (err == ESP_OK) {
        if (length > MANIFEST_MAX) {
            ESP_LOGE(TAG, "the manifest is %lld bytes; that is not one",
                     (long long)length);
            err = ESP_ERR_INVALID_RESPONSE;
        }
    }

    if (err == ESP_OK) {
        size_t filled = 0;
        while (filled < MANIFEST_MAX) {
            const int got = esp_http_client_read(client, body + filled,
                                                 (int)(MANIFEST_MAX - filled));
            if (got < 0) {
                err = ESP_ERR_INVALID_RESPONSE;
                break;
            }
            if (got == 0) {
                break;   /* end of body */
            }
            filled += (size_t)got;
        }
        body[filled] = '\0';
        if (err == ESP_OK && filled == 0) {
            ESP_LOGE(TAG, "the manifest is empty");
            err = ESP_ERR_INVALID_RESPONSE;
        }
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (err == ESP_OK) {
        err = parse_manifest(body, out);
    }
    free(body);
    return err;
}

static int check_private(nn20_worker_ctx *worker, void *user_data)
{
    (void)worker;
    NN20ClockOta *pthis = user_data;

    Release release;
    const esp_err_t err = fetch_manifest(&release);
    if (err != ESP_OK) {
        publish_failure(pthis, err);
        return 0;
    }

    pthis->offered = release;

    /* Written while the published state still reads CHECKING - see the
     * note on the struct. The store below is what makes them visible. */
    snprintf(pthis->pub_version, sizeof(pthis->pub_version), "%s",
             release.version);
    snprintf(pthis->pub_notes, sizeof(pthis->pub_notes), "%s", release.notes);

    const char *running = nn20clock_ota_running_version();
    if (is_newer(release.version, running)) {
        ESP_LOGI(TAG, "running %s, %s is available", running,
                 release.version);
        publish_state(pthis, NN20CLOCK_OTA_AVAILABLE);
    } else {
        ESP_LOGI(TAG, "running %s; %s is the current release", running,
                 release.version);
        publish_state(pthis, NN20CLOCK_OTA_UP_TO_DATE);
    }
    return 0;
}

esp_err_t nn20clock_ota_check(NN20ClockOta *pthis)
{
    if (pthis == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const NN20ClockOtaState state = load_state(pthis);
    if (state == NN20CLOCK_OTA_CHECKING ||
        state == NN20CLOCK_OTA_DOWNLOADING) {
        return ESP_ERR_INVALID_STATE;
    }

    atomic_store_explicit(&pthis->error, ESP_OK, memory_order_relaxed);
    atomic_store_explicit(&pthis->percent, 0u, memory_order_relaxed);
    publish_state(pthis, NN20CLOCK_OTA_CHECKING);

    if (nn20_worker_post(pthis->worker, check_private, pthis) != 0) {
        publish_failure(pthis, ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* ---------------------------------------------------------- install -- */

static void hex_encode(const uint8_t *bytes, size_t count, char *out)
{
    static const char DIGITS[] = "0123456789abcdef";
    for (size_t i = 0; i < count; i++) {
        out[i * 2] = DIGITS[bytes[i] >> 4];
        out[(i * 2) + 1] = DIGITS[bytes[i] & 0x0fu];
    }
    out[count * 2] = '\0';
}

/*
 * Download the image into the inactive app slot.
 *
 * The SHA-256 is taken over the bytes as they arrive rather than by
 * reading the partition back afterwards, because those are not the same
 * bytes until the very end: esp_ota_write() holds the image header back
 * and writes it in esp_ota_end(), precisely so a half-written slot
 * cannot look bootable. Hashing the stream also means a corrupted
 * download is caught before esp_ota_end() is asked to accept it.
 *
 * Two failures happen earlier than you would expect, both measured on
 * the board rather than assumed:
 *
 *   Something that is not an ESP application is rejected by the FIRST
 *   esp_ota_write(), not by esp_ota_end() - ESP-IDF checks the image
 *   header as soon as it has one. A URL pointing at the wrong file
 *   therefore costs one chunk, not the whole download.
 *
 *   A wrong checksum is caught here, before esp_ota_set_boot_partition()
 *   is reached, so a tampered or truncated image never becomes the boot
 *   choice - the slot is simply left as rubbish for the next attempt to
 *   erase.
 *
 * Measured over Wi-Fi on this board: about 230 kB/s from GitHub's object
 * store, so a 1.9 MB firmware takes roughly eight seconds.
 */
static esp_err_t download_image(NN20ClockOta *pthis, const Release *release)
{
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (target == NULL) {
        ESP_LOGE(TAG, "no free app slot; is this an OTA partition table?");
        return ESP_ERR_NOT_FOUND;
    }
    if (release->size > target->size) {
        ESP_LOGE(TAG, "the image does not fit %s", target->label);
        return ESP_ERR_INVALID_SIZE;
    }

    esp_http_client_handle_t client = make_client(release->url);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    uint8_t *chunk = malloc(IMAGE_CHUNK);
    if (chunk == NULL) {
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    esp_ota_handle_t ota = 0;
    bool ota_open = false;
    bool http_open = false;

    esp_err_t err = open_following_redirects(client, NULL);
    http_open = (err == ESP_OK);

    if (err == ESP_OK && mbedtls_sha256_starts(&sha, 0) != 0) {
        err = ESP_FAIL;
    }

    if (err == ESP_OK) {
        /* The exact size, not OTA_SIZE_UNKNOWN: that erases all eight
         * megabytes of the slot up front and costs seconds nobody is
         * watching a progress bar for. */
        err = esp_ota_begin(target, release->size, &ota);
        ota_open = (err == ESP_OK);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "cannot open %s (%s)", target->label,
                     esp_err_to_name(err));
        }
    }

    uint32_t written = 0;
    while (err == ESP_OK && written < release->size) {
        const uint32_t want = ((release->size - written) < IMAGE_CHUNK)
                                  ? (release->size - written)
                                  : IMAGE_CHUNK;
        const int got = esp_http_client_read(client, (char *)chunk, (int)want);
        if (got < 0) {
            ESP_LOGE(TAG, "the connection broke after %" PRIu32 " bytes",
                     written);
            err = ESP_ERR_INVALID_RESPONSE;
            break;
        }
        if (got == 0) {
            ESP_LOGE(TAG, "the download stopped short: %" PRIu32 " of %"
                          PRIu32 " bytes", written, release->size);
            err = ESP_ERR_INVALID_SIZE;
            break;
        }

        if (mbedtls_sha256_update(&sha, chunk, (size_t)got) != 0) {
            err = ESP_FAIL;
            break;
        }
        err = esp_ota_write(ota, chunk, (size_t)got);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "flash write failed (%s)", esp_err_to_name(err));
            break;
        }

        written += (uint32_t)got;
        atomic_store_explicit(&pthis->percent,
                              (unsigned)((written * 100u) / release->size),
                              memory_order_relaxed);

        /* The worker being asked to stop is the one way a download ends
         * early. Checking here means it ends within one chunk. */
        if (!nn20_worker_is_running(pthis->worker)) {
            ESP_LOGW(TAG, "abandoning the download; the worker is stopping");
            err = ESP_ERR_INVALID_STATE;
            break;
        }
    }

    if (err == ESP_OK) {
        uint8_t digest[32];
        char hex[SHA256_HEX_LEN + 1];
        if (mbedtls_sha256_finish(&sha, digest) != 0) {
            err = ESP_FAIL;
        } else {
            hex_encode(digest, sizeof(digest), hex);
            if (strcasecmp(hex, release->sha256) != 0) {
                ESP_LOGE(TAG, "checksum mismatch: got %s, expected %s", hex,
                         release->sha256);
                err = ESP_ERR_INVALID_CRC;
            }
        }
    }

    if (http_open) {
        esp_http_client_close(client);
    }
    esp_http_client_cleanup(client);
    mbedtls_sha256_free(&sha);
    free(chunk);

    if (ota_open) {
        if (err == ESP_OK) {
            /* Only now: this is what validates the image and writes the
             * header that makes the slot bootable. */
            err = esp_ota_end(ota);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "ESP-IDF rejected the image (%s)",
                         esp_err_to_name(err));
            }
        } else {
            (void)esp_ota_abort(ota);
        }
    }

    if (err != ESP_OK) {
        return err;
    }

    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cannot select %s to boot (%s)", target->label,
                 esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "%s written to %s; it runs at the next boot",
             release->version, target->label);
    return ESP_OK;
}

static int install_private(nn20_worker_ctx *worker, void *user_data)
{
    (void)worker;
    NN20ClockOta *pthis = user_data;

    const esp_err_t err = download_image(pthis, &pthis->offered);
    if (err != ESP_OK) {
        atomic_store_explicit(&pthis->percent, 0u, memory_order_relaxed);
        publish_failure(pthis, err);
        return 0;
    }

    atomic_store_explicit(&pthis->percent, 100u, memory_order_relaxed);
    publish_state(pthis, NN20CLOCK_OTA_INSTALLED);
    return 0;
}

esp_err_t nn20clock_ota_install(NN20ClockOta *pthis)
{
    if (pthis == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * AVAILABLE only. Installing from any other state would either have
     * no release to install or reinstall one already written, and both
     * are eight megabytes of somebody's bandwidth.
     */
    if (load_state(pthis) != NN20CLOCK_OTA_AVAILABLE) {
        return ESP_ERR_INVALID_STATE;
    }

    atomic_store_explicit(&pthis->error, ESP_OK, memory_order_relaxed);
    atomic_store_explicit(&pthis->percent, 0u, memory_order_relaxed);
    publish_state(pthis, NN20CLOCK_OTA_DOWNLOADING);

    if (nn20_worker_post(pthis->worker, install_private, pthis) != 0) {
        publish_failure(pthis, ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* ----------------------------------------------------------- reboot -- */

void nn20clock_ota_restart(void)
{
    ESP_LOGI(TAG, "restarting into the selected image");
    esp_restart();
}

esp_err_t nn20clock_ota_mark_valid(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;

    if (running == NULL ||
        esp_ota_get_state_partition(running, &state) != ESP_OK) {
        /* No otadata entry: an image flashed over serial, which the
         * bootloader never had a reason to doubt. */
        return ESP_OK;
    }
    if (state != ESP_OTA_IMG_PENDING_VERIFY) {
        return ESP_OK;
    }

    const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "this update started cleanly; rollback cancelled");
    } else {
        ESP_LOGE(TAG, "cannot confirm this image (%s)", esp_err_to_name(err));
    }
    return err;
}

/* -------------------------------------------------------- lifecycle -- */

NN20ClockOta *nn20clock_ota_ctor(void)
{
    NN20ClockOta *pthis = calloc(1, sizeof(*pthis));
    if (pthis == NULL) {
        ESP_LOGE(TAG, "out of memory");
        return NULL;
    }

    atomic_init(&pthis->state, (int)NN20CLOCK_OTA_IDLE);
    atomic_init(&pthis->percent, 0u);
    atomic_init(&pthis->error, (int)ESP_OK);

    const nn20_worker_config config = {
        .struct_size = sizeof(config),
        .name = "nn20clock-ota",
        /* Two operations exist and neither queues behind the other, so
         * the default 256 slots would be 254 of them unused. */
        .queue_capacity = 4u,
        /* mbedTLS handshake state, the JSON parser, and the HTTP client
         * all live on this stack. 4 KB is not enough for a TLS
         * connection; 8 KB is what the ESP-IDF OTA examples use. */
        .stack_bytes = 8192u,
        /* Below the default. Nothing waits on this thread and it holds
         * the CPU for a minute at a time; the clock face, the touch
         * screen and any alarm that fires mid-download all matter
         * more. */
        .priority = NN20_WORKER_DEFAULT_PRIORITY - 1,
        .core_id = NN20_WORKER_CORE_ANY,
    };

    pthis->worker = nn20_worker_create_with(&config);
    if (pthis->worker == NULL) {
        ESP_LOGE(TAG, "worker thread would not start");
        free(pthis);
        return NULL;
    }
    return pthis;
}

void nn20clock_ota_dtor(NN20ClockOta *pthis)
{
    if (pthis == NULL) {
        return;
    }

    /*
     * _stop() flips the running flag and waits for the callback in
     * flight to return. A download notices within one chunk - see the
     * check in the loop - so this waits for a network read, not for
     * eight megabytes.
     */
    (void)nn20_worker_stop(pthis->worker);
    nn20_worker_delete(pthis->worker);
    free(pthis);
}
