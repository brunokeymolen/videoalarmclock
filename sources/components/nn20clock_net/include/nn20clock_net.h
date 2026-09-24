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
 * nn20clock_net.h - Wi-Fi and NTP (design 14).
 *
 * FIRMWARE ONLY. The ESP32-P4 has no radio of its own: Wi-Fi runs on
 * the ESP32-C6 co-processor over SDIO, reached through esp_hosted and
 * esp_wifi_remote, so there is nothing here a host build could stand in
 * for. It is left out of the host build entirely rather than shimmed.
 *
 * Design 14 puts NTP first among time sources, with the RTC and the
 * last known time behind it. This component owns only the first step:
 * join the network, run SNTP, and report when the system clock has been
 * set. What to do when it never is belongs to the Timer and the
 * ClockManager - a clock must keep telling the time it has rather than
 * waiting for a network.
 *
 * Threading: this runs on the CoreWorker (design 4's "other long-lived
 * services"). Wi-Fi and SNTP are event-driven inside ESP-IDF and have
 * their own task; the callback below is invoked from that task, so it
 * must only post. There are no mutexes here.
 *
 * Credentials come from the device: gear icon -> Wi-Fi picks a network
 * from a scan, and design 9's storage keeps it in NVS. There are no
 * build-time credentials - the Kconfig options that used to hold them
 * were removed on 2026-09-06, because a real SSID and password in
 * .rodata ship inside every public release asset.
 *
 * An empty SSID is not an error, and it is the state a new board is in:
 * the radio still comes up, so the settings screen can scan, and the
 * clock runs on its RTC until somebody chooses a network.
 */
#ifndef NN20CLOCK_NET_H
#define NN20CLOCK_NET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nn20clock_platform.h"
#include "worker.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NN20ClockNet NN20ClockNet;

/*
 * Called when SNTP has set the system clock. Runs on the SNTP task, not
 * the CoreWorker: post and return. The Timer's
 * nn20clock_timer_notify_time_synced() is exactly that shape and is
 * what this is normally wired to.
 */
typedef void (*NN20ClockNetSyncedFn)(void *user_data);

typedef enum {
    /* No network chosen. The radio is up - a scan works - but nothing
     * is being associated with. */
    NN20CLOCK_NET_DISABLED,
    NN20CLOCK_NET_CONNECTING,
    NN20CLOCK_NET_CONNECTED,    /* associated, has an IP */
    NN20CLOCK_NET_FAILED        /* gave up after the configured retries */
} NN20ClockNetState;

const char *nn20clock_net_state_name(NN20ClockNetState state);

/* ------------------------------------------------------------- scan -- */

/* What a scan found. Enough to choose a network and know whether it
 * needs a passphrase. */
typedef struct {
    char ssid[33];        /* 32 bytes plus a terminator */
    int8_t rssi;          /* dBm; less negative is stronger */
    bool needs_password;  /* false only for a genuinely open network */
} NN20ClockNetAp;

/* Scanning every band and channel finds far more than a settings list
 * can show; the strongest are the ones worth offering. */
#define NN20CLOCK_NET_MAX_APS 20

typedef struct {
    NN20ClockNetAp aps[NN20CLOCK_NET_MAX_APS];
    size_t count;
} NN20ClockNetApList;

/*
 * Called when a scan finishes, with the results sorted strongest first.
 * `status` is ESP_OK when `aps` is valid.
 *
 * Runs on the CoreWorker, not the caller's thread. The settings screen
 * hands in a callback that posts to the UiWorker rather than touching
 * LVGL here.
 */
typedef void (*NN20ClockNetScanFn)(esp_err_t status,
                                   const NN20ClockNetApList *aps,
                                   void *user_data);

/*
 * Start a scan. Returns as soon as it is under way; the result arrives
 * through the callback, typically a second or two later.
 *
 * Asynchronous on purpose. A scan blocks for as long as it takes to
 * sweep the channels, and the UI thread is also the display thread -
 * blocking it would freeze the screen mid-scan.
 *
 * ESP_ERR_INVALID_STATE if a scan is already running or the radio is
 * not up.
 */
esp_err_t nn20clock_net_scan(NN20ClockNet *pthis, NN20ClockNetScanFn on_done,
                             void *user_data);

/* ------------------------------------------------------ credentials -- */

/*
 * Join a different network, replacing whatever is configured.
 *
 * Disconnects, applies the new credentials, and reconnects; the state
 * goes back to CONNECTING and the usual events report what happens.
 * Storing the credentials is the caller's job - this only applies them.
 *
 * password may be empty for an open network. Neither value is ever
 * logged.
 */
esp_err_t nn20clock_net_connect_to(NN20ClockNet *pthis, const char *ssid,
                                   const char *password);

/* --------------------------------------------------------- lifecycle -- */

/*
 * worker is the CoreWorker, borrowed, and must outlive this. NULL is
 * rejected.
 *
 * Constructing does not touch the radio; _start() does.
 */
NN20ClockNet *nn20clock_net_ctor(nn20_worker_ctx *worker);

/* Stops first if needed. Safe with NULL. */
void nn20clock_net_dtor(NN20ClockNet *pthis);

/* Called when the clock is set. Install before _start(). */
esp_err_t nn20clock_net_set_synced_callback(NN20ClockNet *pthis,
                                            NN20ClockNetSyncedFn on_synced,
                                            void *user_data);

/*
 * Bring up the network stack, join a network, and start SNTP. Returns as
 * soon as the attempt is under way - it does not wait for an IP, because
 * nothing else should wait for one either.
 *
 * `ssid` and `password` are the stored credentials. NULL or an empty
 * SSID means a device that has never been given a network, which is not
 * an error and has nothing behind it to fall back to: the radio comes up
 * anyway so that the settings screen can scan, the state becomes
 * NN20CLOCK_NET_DISABLED, and ESP_OK is returned.
 *
 * Once a network is chosen there, _connect_to() joins it without a
 * restart.
 */
esp_err_t nn20clock_net_start(NN20ClockNet *pthis, const char *ssid,
                              const char *password);

/* Disconnect and stop SNTP. Idempotent. */
esp_err_t nn20clock_net_stop(NN20ClockNet *pthis);

NN20ClockNetState nn20clock_net_state(const NN20ClockNet *pthis);
bool nn20clock_net_is_connected(const NN20ClockNet *pthis);

/* True once SNTP has set the system clock at least once. */
bool nn20clock_net_is_time_synced(const NN20ClockNet *pthis);

/* --------------------------------------------------------------- NTP -- */

/*
 * Turn the time sync on or off without touching the radio.
 *
 * Separate from the connection on purpose. A clock with no internet
 * still wants its network - the media management screen serves files
 * over it - and a clock whose time is set by hand must not have SNTP
 * quietly overwrite it a few minutes later. Those are two different
 * questions and this is the second one.
 *
 * Safe before or after _start(): the setting is remembered and applied
 * when the network comes up. Turning it back on starts a fresh sync,
 * which will move the clock as soon as a server answers.
 *
 * ESP_ERR_INVALID_ARG on a NULL net; ESP_OK when it was already in the
 * requested state.
 */
esp_err_t nn20clock_net_set_ntp(NN20ClockNet *pthis, bool enabled);

bool nn20clock_net_ntp_enabled(const NN20ClockNet *pthis);

/* The SSID in use, or "" when none is set. Never the password - nothing
 * in this component logs or returns that. */
const char *nn20clock_net_ssid(const NN20ClockNet *pthis);

/* "192.168.0.204" plus a terminator. */
#define NN20CLOCK_NET_IP_STRING_MAX 16

/*
 * The current IPv4 address as text.
 *
 * ESP_ERR_INVALID_STATE when there is no address - not connected, or
 * DHCP has not finished. The buffer is left as an empty string in that
 * case, so a caller that ignores the return still prints nothing rather
 * than rubbish.
 *
 * The address is held as a number and formatted here, rather than kept
 * as a string: it is written on the network event task and read from
 * the UI thread, and a four-byte value can cross that boundary without
 * a torn read where a string cannot.
 */
esp_err_t nn20clock_net_ip_string(const NN20ClockNet *pthis, char *out,
                                  size_t size);

#ifdef __cplusplus
}
#endif

#endif /* NN20CLOCK_NET_H */
