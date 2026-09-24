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
 * nn20clock_net.c - Wi-Fi station and SNTP (design 14).
 *
 * Deliberately unambitious. It joins one network, runs SNTP, and gives
 * up after a bounded number of attempts. A clock that blocks boot on a
 * network it cannot reach is worse than a clock showing the wrong time,
 * so nothing here waits and nothing retries forever.
 *
 * Wi-Fi provisioning is still an open question in design 18; when it is
 * answered, the credential source changes and the rest of this file
 * should not have to.
 */
#include "nn20clock_net.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

static const char *TAG = "NN20CLOCK_NET";

/*
 * esp_netif_sntp's sync callback carries no user pointer, so the one
 * instance has to be reachable from a file-static. There is one radio
 * and one clock in this device; a second instance would be a design
 * change rather than a plumbing one, and the constructor refuses it
 * outright instead of leaving this pointer ambiguous.
 */
static NN20ClockNet *g_instance;

/*
 * There are no build-time credentials any more. The network is chosen on
 * the device - gear icon -> Wi-Fi - and kept in NVS by design 9's
 * storage, so the only SSID this component knows is the one handed to
 * _start() or _connect_to(). The Kconfig options that used to supply a
 * fallback were removed on 2026-09-06: they put a real SSID and password
 * in .rodata, which is published on every release asset.
 */

/* Kconfig defaults keep these defined even if the menu is absent. */
#ifndef CONFIG_NN20CLOCK_WIFI_MAX_RETRY
#define CONFIG_NN20CLOCK_WIFI_MAX_RETRY 5
#endif
#ifndef CONFIG_NN20CLOCK_SNTP_SERVER
#define CONFIG_NN20CLOCK_SNTP_SERVER "pool.ntp.org"
#endif

struct NN20ClockNet {
    nn20_worker_ctx *worker;   /* the CoreWorker; borrowed */

    /* The credentials actually in use. Copied in, so the caller's
     * storage need not outlive the call. The password is never logged
     * and never returned. */
    char ssid[33];
    char password[64];

    NN20ClockNetSyncedFn on_synced;
    void *synced_user_data;

    /* Written from the Wi-Fi/SNTP event tasks, read from anywhere. */
    atomic_int state;
    atomic_bool time_synced;
    atomic_int retries;
    /*
     * Whether `ssid` names anything. Written by the CoreWorker before it
     * starts or reconnects the radio, read by the event task on
     * STA_START - which is the one decision that crosses that boundary,
     * and the reason it is not simply strlen(ssid) at the point of use.
     */
    atomic_bool have_network;
    /* Raw IPv4, network order, as the event task received it. Zero when
     * there is no address. A number rather than a string so it crosses
     * the thread boundary whole. */
    atomic_uint ip4;

    bool started;              /* CoreWorker-owned */
    bool radio_up;             /* esp_wifi_init has succeeded */
    /*
     * Whether the user wants NTP, and whether esp_netif_sntp_init() has
     * actually been called. Two separate things: the wish survives the
     * network being down, and deinit must only ever follow an init.
     * Both CoreWorker-owned.
     */
    bool ntp_wanted;
    bool sntp_up;
    bool scanning;             /* CoreWorker-owned */

    NN20ClockNetScanFn on_scan;
    void *scan_user_data;
    esp_netif_t *netif;        /* owned once started */
    esp_event_handler_instance_t any_wifi;
    esp_event_handler_instance_t got_ip;
};

const char *nn20clock_net_state_name(NN20ClockNetState state)
{
    switch (state) {
    case NN20CLOCK_NET_DISABLED:   return "DISABLED";
    case NN20CLOCK_NET_CONNECTING: return "CONNECTING";
    case NN20CLOCK_NET_CONNECTED:  return "CONNECTED";
    case NN20CLOCK_NET_FAILED:     return "FAILED";
    }
    return "UNKNOWN";
}

static void set_state(NN20ClockNet *pthis, NN20ClockNetState state)
{
    atomic_store_explicit(&pthis->state, (int)state, memory_order_release);
}

/* ------------------------------------------------------------ events -- */

/* Runs on the Wi-Fi event task. */
static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id,
                          void *data)
{
    (void)base;
    (void)data;
    NN20ClockNet *pthis = arg;

    if (id == WIFI_EVENT_STA_START) {
        /*
         * The radio comes up even with no network chosen, so that the
         * settings screen can scan - see start_private(). Connecting
         * with an empty SSID would fail, retry, and settle in FAILED,
         * which reads as a broken radio rather than as a clock waiting
         * to be told which network to join.
         */
        if (atomic_load_explicit(&pthis->have_network, memory_order_acquire)) {
            (void)esp_wifi_connect();
        }
        return;
    }

    if (id == WIFI_EVENT_STA_DISCONNECTED) {
        /* The address is gone with the association; showing a stale one
         * would be worse than showing none. */
        atomic_store_explicit(&pthis->ip4, 0u, memory_order_release);

        const int attempt = atomic_fetch_add_explicit(&pthis->retries, 1,
                                                      memory_order_relaxed) + 1;
        if (attempt <= CONFIG_NN20CLOCK_WIFI_MAX_RETRY) {
            ESP_LOGW(TAG, "disconnected, retry %d/%d", attempt,
                     CONFIG_NN20CLOCK_WIFI_MAX_RETRY);
            (void)esp_wifi_connect();
        } else {
            /* Stop here rather than retrying forever: the clock has a
             * job to do without a network, and a radio retrying in a
             * tight loop is worse than no radio. */
            ESP_LOGE(TAG, "giving up after %d attempts; running without NTP",
                     CONFIG_NN20CLOCK_WIFI_MAX_RETRY);
            set_state(pthis, NN20CLOCK_NET_FAILED);
        }
    }
}

/* Runs on the event task once DHCP has finished. */
static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)base;
    (void)id;
    NN20ClockNet *pthis = arg;
    ip_event_got_ip_t *event = data;

    atomic_store_explicit(&pthis->retries, 0, memory_order_relaxed);
    atomic_store_explicit(&pthis->ip4, (unsigned)event->ip_info.ip.addr,
                          memory_order_release);
    set_state(pthis, NN20CLOCK_NET_CONNECTED);
    ESP_LOGI(TAG, "connected, ip " IPSTR, IP2STR(&event->ip_info.ip));
}

/* Runs on the SNTP task when the system clock has been set. */
static void on_sntp_synced(struct timeval *tv)
{
    (void)tv;
    NN20ClockNet *pthis = g_instance;
    if (pthis == NULL) {
        return;
    }

    atomic_store_explicit(&pthis->time_synced, true, memory_order_release);
    ESP_LOGI(TAG, "system clock set from %s", CONFIG_NN20CLOCK_SNTP_SERVER);

    if (pthis->on_synced != NULL) {
        /* Contract: this only posts. */
        pthis->on_synced(pthis->synced_user_data);
    }
}

/*
 * Bring SNTP up and down. Both are idempotent, and both are the only
 * places esp_netif_sntp_init/deinit are called: the pairing is what
 * `sntp_up` tracks, and calling deinit without an init is a crash rather
 * than an error code.
 */
static esp_err_t sntp_up(NN20ClockNet *pthis)
{
    if (pthis->sntp_up) {
        return ESP_OK;
    }

    esp_sntp_config_t sntp_config =
        ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_NN20CLOCK_SNTP_SERVER);
    sntp_config.start = true;
    sntp_config.server_from_dhcp = false;
    sntp_config.sync_cb = on_sntp_synced;

    const esp_err_t err = esp_netif_sntp_init(&sntp_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sntp init failed (0x%x)", (unsigned)err);
        return err;
    }
    pthis->sntp_up = true;
    return ESP_OK;
}

static void sntp_down(NN20ClockNet *pthis)
{
    if (!pthis->sntp_up) {
        return;
    }
    esp_netif_sntp_deinit();
    pthis->sntp_up = false;
}

/* Runs on the CoreWorker, posted by nn20clock_net_set_ntp(). */
static int set_ntp_private(nn20_worker_ctx *worker, void *user_data)
{
    (void)worker;
    NN20ClockNet *pthis = user_data;

    if (pthis->ntp_wanted) {
        /*
         * Only if the network is actually up. If it is not, the wish is
         * recorded and start_private() acts on it - which is what makes
         * this safe to call before _start().
         */
        if (pthis->started) {
            (void)sntp_up(pthis);
        }
        ESP_LOGI(TAG, "time sync on");
    } else {
        sntp_down(pthis);
        /*
         * The clock stays wherever it is. Turning the sync off does not
         * unset the time - it stops anything moving it, which is the
         * whole point when somebody has just typed it in by hand.
         */
        ESP_LOGI(TAG, "time sync off; the clock is set by hand from here");
    }
    return 0;
}

/* Runs on the CoreWorker. Never logs the passphrase. */
static esp_err_t apply_credentials(NN20ClockNet *pthis)
{
    /*
     * memcpy with an explicit length, not strncpy.
     *
     * The driver's ssid and password are fixed-size byte fields, not C
     * strings: an SSID may legitimately fill all 32 bytes with no
     * terminator. strncpy(..., sizeof(field) - 1) would silently drop
     * the last character of a maximum-length SSID, which is exactly
     * what GCC's stringop-truncation warning is pointing at. The struct
     * is zero-initialised, so anything shorter is padded with zeros.
     */
    wifi_config_t wifi_config = {0};

    const size_t ssid_len = strnlen(pthis->ssid,
                                    sizeof(wifi_config.sta.ssid));
    memcpy(wifi_config.sta.ssid, pthis->ssid, ssid_len);

    const size_t password_len = strnlen(pthis->password,
                                        sizeof(wifi_config.sta.password));
    memcpy(wifi_config.sta.password, pthis->password, password_len);

    return esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
}

/* --------------------------------------------------------- lifecycle -- */

NN20ClockNet *nn20clock_net_ctor(nn20_worker_ctx *worker)
{
    if (worker == NULL) {
        ESP_LOGE(TAG, "no CoreWorker (design 4)");
        return NULL;
    }
    if (g_instance != NULL) {
        ESP_LOGE(TAG, "only one instance is supported");
        return NULL;
    }

    NN20ClockNet *pthis = calloc(1, sizeof(*pthis));
    if (pthis == NULL) {
        ESP_LOGE(TAG, "out of memory");
        return NULL;
    }

    pthis->worker = worker;
    atomic_init(&pthis->state, (int)NN20CLOCK_NET_DISABLED);
    atomic_init(&pthis->time_synced, false);
    atomic_init(&pthis->retries, 0);
    atomic_init(&pthis->ip4, 0u);
    /* On by default: a clock that has never been configured should get
     * the right time by itself. */
    pthis->ntp_wanted = true;

    g_instance = pthis;
    return pthis;
}

void nn20clock_net_dtor(NN20ClockNet *pthis)
{
    if (pthis == NULL) {
        return;
    }
    if (pthis->started) {
        (void)nn20clock_net_stop(pthis);
    }
    if (g_instance == pthis) {
        g_instance = NULL;
    }
    free(pthis);
}

esp_err_t nn20clock_net_set_synced_callback(NN20ClockNet *pthis,
                                            NN20ClockNetSyncedFn on_synced,
                                            void *user_data)
{
    if (pthis == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (pthis->started) {
        return ESP_ERR_INVALID_STATE;
    }

    pthis->on_synced = on_synced;
    pthis->synced_user_data = user_data;
    return ESP_OK;
}

/* Runs on the CoreWorker. */
static int start_private(nn20_worker_ctx *worker, void *user_data)
{
    (void)worker;
    NN20ClockNet *pthis = user_data;

    /*
     * A board that has never been told which network to join still
     * brings its radio all the way up, and stops one step short of
     * associating.
     *
     * That step is the whole point. The network is chosen on the device
     * now, and choosing it means scanning - which needs esp_wifi_start()
     * to have run. Returning early here, as this did while credentials
     * came from the build, leaves a fresh board unable to scan, and
     * therefore unable to reach the screen that would give it a network:
     * "Scan unavailable" with no way past it, for ever.
     */
    const bool have_network = (pthis->ssid[0] != '\0');
    if (!have_network) {
        ESP_LOGW(TAG, "no network chosen; the radio comes up for scanning, "
                      "and the clock runs on its RTC until one is set in "
                      "the settings screen");
    }

    /* NVS holds the Wi-Fi driver's own calibration data; it is not
     * design 9's storage, which arrives at Milestone 3. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs init failed (0x%x)", (unsigned)err);
        set_state(pthis, NN20CLOCK_NET_FAILED);
        return -1;
    }

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event loop failed (0x%x)", (unsigned)err);
        set_state(pthis, NN20CLOCK_NET_FAILED);
        return -1;
    }

    pthis->netif = esp_netif_create_default_wifi_sta();
    if (pthis->netif == NULL) {
        ESP_LOGE(TAG, "no station netif");
        set_state(pthis, NN20CLOCK_NET_FAILED);
        return -1;
    }

    /* On the P4 this reaches the C6 over SDIO via esp_wifi_remote; the
     * API is the same as a chip with its own radio. */
    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_config), TAG, "wifi init");

    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                            on_wifi_event, pthis,
                                            &pthis->any_wifi),
        TAG, "wifi handler");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                            on_got_ip, pthis, &pthis->got_ip),
        TAG, "ip handler");

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "wifi mode");
    if (have_network) {
        ESP_RETURN_ON_ERROR(apply_credentials(pthis), TAG, "wifi config");
    }

    /* Read by the event task when the driver reports STA_START, which
     * esp_wifi_start() below is about to cause - so it is published
     * first. */
    atomic_store_explicit(&pthis->have_network, have_network,
                          memory_order_release);

    pthis->radio_up = true;
    set_state(pthis, have_network ? NN20CLOCK_NET_CONNECTING
                                  : NN20CLOCK_NET_DISABLED);
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");

    /* Started before the association completes on purpose: SNTP waits
     * for the interface itself, and nothing here should block. Skipped
     * entirely when the user has taken the clock off NTP - see
     * nn20clock_net_set_ntp(). */
    if (pthis->ntp_wanted) {
        (void)sntp_up(pthis);
    }

    /* The SSID is fine to log; the password is never logged anywhere in
     * this component. */
    if (have_network) {
        ESP_LOGI(TAG, "connecting to '%s', ntp %s", pthis->ssid,
                 pthis->ntp_wanted ? CONFIG_NN20CLOCK_SNTP_SERVER : "off");
    } else {
        ESP_LOGI(TAG, "radio up with no network, ntp %s",
                 pthis->ntp_wanted ? CONFIG_NN20CLOCK_SNTP_SERVER : "off");
    }
    pthis->started = true;
    return 0;
}

esp_err_t nn20clock_net_start(NN20ClockNet *pthis, const char *ssid,
                              const char *password)
{
    if (pthis == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (pthis->started) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * Whatever storage had, and nothing else. There is no build-time
     * fallback behind this any more: an empty SSID means the clock has
     * never been told which network to join, which start_private()
     * handles by bringing the radio up for scanning and leaving it
     * there.
     */
    const bool stored = (ssid != NULL && ssid[0] != '\0');
    snprintf(pthis->ssid, sizeof(pthis->ssid), "%s", stored ? ssid : "");
    snprintf(pthis->password, sizeof(pthis->password), "%s",
             (stored && password != NULL) ? password : "");
    if (stored) {
        ESP_LOGI(TAG, "using the stored network");
    }

    const int rc = nn20_worker_post_sync(pthis->worker, start_private, pthis);
    if (rc != 0) {
        return (rc == 1) ? ESP_ERR_TIMEOUT : ESP_ERR_INVALID_STATE;
    }
    return pthis->started ? ESP_OK : ESP_FAIL;
}

static int stop_private(nn20_worker_ctx *worker, void *user_data)
{
    (void)worker;
    NN20ClockNet *pthis = user_data;

    /*
     * radio_up, not the state: DISABLED now means "no network chosen",
     * and the radio is up in that state. Testing the state here would
     * leak the driver and the netif on exactly the boards that have
     * never been configured.
     */
    if (pthis->radio_up) {
        sntp_down(pthis);
        (void)esp_wifi_disconnect();
        (void)esp_wifi_stop();
        atomic_store_explicit(&pthis->ip4, 0u, memory_order_release);

        if (pthis->any_wifi != NULL) {
            (void)esp_event_handler_instance_unregister(
                WIFI_EVENT, ESP_EVENT_ANY_ID, pthis->any_wifi);
            pthis->any_wifi = NULL;
        }
        if (pthis->got_ip != NULL) {
            (void)esp_event_handler_instance_unregister(
                IP_EVENT, IP_EVENT_STA_GOT_IP, pthis->got_ip);
            pthis->got_ip = NULL;
        }

        /*
         * The driver and the network interface, in that order.
         *
         * Not optional, and not merely tidy: the station netif is
         * registered under a fixed key, and creating a second one
         * without destroying the first does not fail politely - 
         * esp_netif_create_default_wifi_sta() asserts and resets the
         * board. The device never does this, but the on-target test
         * suite builds more than one application, and that is exactly
         * how it showed up.
         */
        (void)esp_wifi_deinit();
        if (pthis->netif != NULL) {
            esp_netif_destroy_default_wifi(pthis->netif);
            pthis->netif = NULL;
        }
    }

    pthis->started = false;
    pthis->radio_up = false;
    atomic_store_explicit(&pthis->have_network, false, memory_order_release);
    set_state(pthis, NN20CLOCK_NET_DISABLED);
    ESP_LOGI(TAG, "stopped");
    return 0;
}

esp_err_t nn20clock_net_stop(NN20ClockNet *pthis)
{
    if (pthis == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!pthis->started) {
        return ESP_ERR_INVALID_STATE;
    }

    const int rc = nn20_worker_post_sync(pthis->worker, stop_private, pthis);
    return (rc == 0) ? ESP_OK
                     : ((rc == 1) ? ESP_ERR_TIMEOUT : ESP_ERR_INVALID_STATE);
}

/* --------------------------------------------------------- accessors -- */

NN20ClockNetState nn20clock_net_state(const NN20ClockNet *pthis)
{
    if (pthis == NULL) {
        return NN20CLOCK_NET_DISABLED;
    }
    return (NN20ClockNetState)atomic_load_explicit(&pthis->state,
                                                   memory_order_acquire);
}

bool nn20clock_net_is_connected(const NN20ClockNet *pthis)
{
    return nn20clock_net_state(pthis) == NN20CLOCK_NET_CONNECTED;
}

bool nn20clock_net_is_time_synced(const NN20ClockNet *pthis)
{
    return pthis != NULL &&
           atomic_load_explicit(&pthis->time_synced, memory_order_acquire);
}

esp_err_t nn20clock_net_set_ntp(NN20ClockNet *pthis, bool enabled)
{
    if (pthis == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (pthis->ntp_wanted == enabled) {
        return ESP_OK;
    }

    /*
     * Written here rather than inside the posted work so that the wish
     * is recorded even if the post is refused - the worst case is then
     * a setting that takes effect at the next start, not one that is
     * silently dropped.
     */
    pthis->ntp_wanted = enabled;
    const int rc = nn20_worker_post_sync(pthis->worker, set_ntp_private,
                                         pthis);
    return (rc == 0) ? ESP_OK
                     : ((rc == 1) ? ESP_ERR_TIMEOUT : ESP_ERR_INVALID_STATE);
}

bool nn20clock_net_ntp_enabled(const NN20ClockNet *pthis)
{
    return pthis != NULL && pthis->ntp_wanted;
}

const char *nn20clock_net_ssid(const NN20ClockNet *pthis)
{
    return (pthis != NULL) ? pthis->ssid : "";
}

esp_err_t nn20clock_net_ip_string(const NN20ClockNet *pthis, char *out,
                                  size_t size)
{
    if (out == NULL || size < NN20CLOCK_NET_IP_STRING_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';

    if (pthis == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const uint32_t raw = (uint32_t)atomic_load_explicit(&pthis->ip4,
                                                        memory_order_acquire);
    if (raw == 0u) {
        return ESP_ERR_INVALID_STATE;   /* no address yet */
    }

    const esp_ip4_addr_t address = { .addr = raw };
    snprintf(out, size, IPSTR, IP2STR(&address));
    return ESP_OK;
}

/* ------------------------------------------------------ credentials -- */

typedef struct {
    NN20ClockNet *net;
    char ssid[33];
    char password[64];
    esp_err_t result;
} ConnectRequest;

static int connect_to_private(nn20_worker_ctx *worker, void *user_data)
{
    (void)worker;
    ConnectRequest *request = user_data;
    NN20ClockNet *pthis = request->net;

    if (!pthis->radio_up) {
        /*
         * The radio is not up, which since start_private() brings it up
         * with or without a network means it failed to initialise at
         * all. Nothing here can fix that, and a restart is the honest
         * answer; the caller says so.
         */
        request->result = ESP_ERR_INVALID_STATE;
        return -1;
    }

    snprintf(pthis->ssid, sizeof(pthis->ssid), "%s", request->ssid);
    snprintf(pthis->password, sizeof(pthis->password), "%s",
             request->password);
    /* Before anything can cause a STA_START: this is what tells the
     * event task there is now a network worth connecting to. */
    atomic_store_explicit(&pthis->have_network, true, memory_order_release);

    /* Drop the current association before changing the config, or the
     * driver keeps the old one until it happens to disconnect. */
    (void)esp_wifi_disconnect();
    atomic_store_explicit(&pthis->retries, 0, memory_order_relaxed);

    request->result = apply_credentials(pthis);
    if (request->result != ESP_OK) {
        return -1;
    }

    set_state(pthis, NN20CLOCK_NET_CONNECTING);
    request->result = esp_wifi_connect();
    if (request->result != ESP_OK) {
        return -1;
    }

    ESP_LOGI(TAG, "reconnecting to '%s'", pthis->ssid);
    return 0;
}

esp_err_t nn20clock_net_connect_to(NN20ClockNet *pthis, const char *ssid,
                                   const char *password)
{
    if (pthis == NULL || ssid == NULL || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    ConnectRequest request = { .net = pthis, .result = ESP_FAIL };
    snprintf(request.ssid, sizeof(request.ssid), "%s", ssid);
    snprintf(request.password, sizeof(request.password), "%s",
             (password != NULL) ? password : "");

    const int rc = nn20_worker_post_sync(pthis->worker, connect_to_private,
                                         &request);
    if (rc != 0) {
        return (rc == 1) ? ESP_ERR_TIMEOUT : ESP_ERR_INVALID_STATE;
    }
    return request.result;
}

/* ------------------------------------------------------------- scan -- */

static int scan_private(nn20_worker_ctx *worker, void *user_data)
{
    (void)worker;
    NN20ClockNet *pthis = user_data;

    /*
     * Blocking scan, on the CoreWorker. It takes a second or two, which
     * is why this is not done on the UI thread - that thread also draws
     * the screen.
     */
    esp_err_t err = esp_wifi_scan_start(NULL, true);

    NN20ClockNetApList *list = calloc(1, sizeof(*list));
    if (list == NULL) {
        err = ESP_ERR_NO_MEM;
    }

    if (err == ESP_OK) {
        uint16_t found = 0;
        (void)esp_wifi_scan_get_ap_num(&found);

        /* The driver sorts by RSSI, so taking the first N takes the
         * strongest - which are the ones worth offering. */
        uint16_t wanted = (found > NN20CLOCK_NET_MAX_APS)
                              ? NN20CLOCK_NET_MAX_APS
                              : found;
        wifi_ap_record_t *records = calloc(wanted ? wanted : 1,
                                           sizeof(*records));
        if (records == NULL) {
            err = ESP_ERR_NO_MEM;
        } else {
            err = esp_wifi_scan_get_ap_records(&wanted, records);
            if (err == ESP_OK) {
                for (uint16_t i = 0; i < wanted; i++) {
                    NN20ClockNetAp *ap = &list->aps[list->count];
                    snprintf(ap->ssid, sizeof(ap->ssid), "%s",
                             (const char *)records[i].ssid);
                    if (ap->ssid[0] == '\0') {
                        continue;   /* hidden network; nothing to show */
                    }
                    ap->rssi = records[i].rssi;
                    ap->needs_password =
                        (records[i].authmode != WIFI_AUTH_OPEN);
                    list->count++;
                }
            }
            free(records);
        }
        ESP_LOGI(TAG, "scan found %u network(s), showing %u",
                 (unsigned)found, (unsigned)list->count);
    } else {
        ESP_LOGE(TAG, "scan failed (0x%x)", (unsigned)err);
    }

    pthis->scanning = false;
    if (pthis->on_scan != NULL) {
        pthis->on_scan(err, (err == ESP_OK) ? list : NULL,
                       pthis->scan_user_data);
    }
    free(list);
    return (err == ESP_OK) ? 0 : -1;
}

esp_err_t nn20clock_net_scan(NN20ClockNet *pthis, NN20ClockNetScanFn on_done,
                             void *user_data)
{
    if (pthis == NULL || on_done == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!pthis->radio_up) {
        return ESP_ERR_INVALID_STATE;
    }
    if (pthis->scanning) {
        return ESP_ERR_INVALID_STATE;
    }

    pthis->scanning = true;
    pthis->on_scan = on_done;
    pthis->scan_user_data = user_data;

    /* Asynchronous: the scan itself blocks for seconds, and the caller
     * is usually the UI thread, which must keep drawing. */
    const int rc = nn20_worker_post(pthis->worker, scan_private, pthis);
    if (rc != 0) {
        pthis->scanning = false;
        return (rc == 1) ? ESP_ERR_TIMEOUT : ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}
