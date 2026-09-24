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
 * nn20clock_ota.h - updating the clock over the network.
 *
 * FIRMWARE ONLY. It writes flash partitions and terminates in a reboot;
 * there is nothing a host build could stand in for, so like
 * nn20clock_net it is absent from that build rather than shimmed.
 *
 * WHAT THIS TALKS TO
 *
 * A public repository - `videoalarmclock` - carries no source, only
 * the things somebody who owns one of these clocks needs: the firmware
 * binaries, the printable base, and the container that converts and
 * uploads video. This component reads two files from it:
 *
 *   the manifest   a small JSON document at a fixed URL, naming the
 *                  current release. Fetched by _check().
 *   the image      the .bin named by the manifest, a GitHub release
 *                  asset. Fetched by _install().
 *
 * A manifest rather than the GitHub releases API on purpose. It is a
 * document this project defines, so it can carry the SHA-256 the device
 * verifies and it does not change shape when GitHub's API does; and a
 * release can be uploaded and tested before the manifest points at it,
 * which the API's idea of "latest" does not allow.
 *
 *   {
 *     "version": "v0.2.0",
 *     "notes":   "Brightness schedule",
 *     "url":     "https://github.com/<owner>/videoalarmclock/releases/...",
 *     "size":    1543312,
 *     "sha256":  "a3f1..."
 *   }
 *
 * WHAT IT DOES NOT DO
 *
 * It does not decide when to update. Nothing here polls, and nothing
 * updates by itself: both steps are a button on the About screen. A
 * clock that reboots into different software while its owner is asleep
 * is a worse clock, whatever the software.
 *
 * THREADING
 *
 * This component owns a worker of its own, for the same reason the
 * Timer does (design 4): downloading and flashing eight megabytes
 * occupies a thread for the better part of a minute, and nothing else -
 * not the network service, not a settings write - may queue behind
 * that. _check() and _install() copy nothing, post, and return at once.
 *
 * There is no completion callback. Progress is read with _status(),
 * which the About screen calls from the one-second timer event it
 * already receives. That is deliberate: a callback carrying a screen
 * pointer across a sixty-second download outlives the screen, and the
 * only safe answers to that are a lock (forbidden - see design 4) or a
 * cancellation handshake nobody would get right twice. Polling a
 * published snapshot has neither problem.
 */
#ifndef NN20CLOCK_OTA_H
#define NN20CLOCK_OTA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nn20clock_platform.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NN20ClockOta NN20ClockOta;

typedef enum {
    NN20CLOCK_OTA_IDLE,          /* nothing asked for yet */
    NN20CLOCK_OTA_CHECKING,      /* fetching the manifest */
    NN20CLOCK_OTA_UP_TO_DATE,    /* the release is what is running */
    NN20CLOCK_OTA_AVAILABLE,     /* a newer release exists; not installed */
    NN20CLOCK_OTA_DOWNLOADING,   /* writing the other app slot */
    NN20CLOCK_OTA_INSTALLED,     /* written and verified; reboot to run it */
    NN20CLOCK_OTA_FAILED         /* see NN20ClockOtaStatus::error */
} NN20ClockOtaState;

const char *nn20clock_ota_state_name(NN20ClockOtaState state);

/* Long enough for "v10.20.30-40-gdeadbeef-dirty", which is what
 * `git describe` produces on a working tree between tags. */
#define NN20CLOCK_OTA_VERSION_MAX 40

/* One line of release notes. The About screen has room for one line,
 * so anything longer is truncated here rather than clipped there. */
#define NN20CLOCK_OTA_NOTES_MAX 96

typedef struct {
    NN20ClockOtaState state;

    /*
     * The release the manifest offered. Empty until a check has
     * succeeded, and meaningless while the state is CHECKING.
     */
    char version[NN20CLOCK_OTA_VERSION_MAX];
    char notes[NN20CLOCK_OTA_NOTES_MAX];

    /* 0..100 while DOWNLOADING; 100 once INSTALLED. */
    uint8_t percent;

    /* Why it failed. ESP_OK unless the state is FAILED. */
    esp_err_t error;
} NN20ClockOtaStatus;

/* -------------------------------------------------------- lifecycle -- */

/*
 * Creating one starts its worker but touches neither the network nor
 * the flash. NULL if the worker could not be started.
 */
NN20ClockOta *nn20clock_ota_ctor(void);

/*
 * Stops the worker, waiting for a download in flight to notice and
 * abandon the partition it was writing. Safe with NULL.
 *
 * An abandoned download leaves the inactive slot half-written, which is
 * harmless: nothing points the bootloader at it, and the next install
 * erases it again.
 */
void nn20clock_ota_dtor(NN20ClockOta *pthis);

/* ------------------------------------------------------------ query -- */

/*
 * Fetch the manifest and compare its version with the running one.
 *
 * Returns as soon as the fetch is under way. ESP_ERR_INVALID_STATE if a
 * check or an install is already running - the two are one worker and
 * there is no queue of them worth having.
 */
esp_err_t nn20clock_ota_check(NN20ClockOta *pthis);

/*
 * Download the release the last check found, write it to the inactive
 * app slot, verify its SHA-256, and make it the boot choice.
 *
 * The clock is NOT restarted. The state becomes INSTALLED and the new
 * firmware runs at the next boot, which is the owner's decision to make
 * - see nn20clock_ota_restart().
 *
 * ESP_ERR_INVALID_STATE unless a check has just reported AVAILABLE.
 */
esp_err_t nn20clock_ota_install(NN20ClockOta *pthis);

/*
 * A snapshot of where the above got to. Never blocks; never waits on
 * the OTA worker, which is the point - it is called from the UiWorker
 * while that worker is busy.
 *
 * Call it from one thread only, and the same one that calls _check()
 * and _install(). The published version and notes are written while the
 * state reads CHECKING and read only when it does not, and it is the
 * single caller that orders those two against each other.
 */
void nn20clock_ota_status(const NN20ClockOta *pthis,
                          NN20ClockOtaStatus *out);

/* ----------------------------------------------------------- reboot -- */

/*
 * Restart into whatever the bootloader will now choose. Does not
 * return.
 */
void nn20clock_ota_restart(void);

/* ------------------------------------------------- rollback control -- */

/*
 * Tell the bootloader this image works.
 *
 * With CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE a freshly installed image
 * boots as "pending verify" and is reverted to the previous slot at the
 * next reset unless this is called. The app root calls it once, after
 * every service has started - so the test the new firmware has to pass
 * to keep itself installed is "it reaches a running clock".
 *
 * Idempotent, and ESP_OK on an image that was not pending, which is
 * every image flashed over serial.
 */
esp_err_t nn20clock_ota_mark_valid(void);

/*
 * The version string of the running image, from `git describe` by way
 * of ESP-IDF's PROJECT_VER. Never NULL.
 */
const char *nn20clock_ota_running_version(void);

#ifdef __cplusplus
}
#endif

#endif /* NN20CLOCK_OTA_H */
