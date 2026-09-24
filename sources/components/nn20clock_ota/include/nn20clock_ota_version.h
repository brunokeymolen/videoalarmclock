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
 * nn20clock_ota_version.h - comparing `git describe` version strings.
 *
 * Split out of nn20clock_ota.c, and portable where the rest of that
 * component is not, for one reason: this is the decision that says
 * whether the clock replaces its own firmware. Getting it wrong in the
 * cautious direction means a clock that never updates; getting it wrong
 * in the other means one that installs an older build over a newer one
 * and does it again at every check. Neither announces itself, and both
 * are cheap to test on the host - so they are, in test_nn20clock_ota.c.
 *
 * No ESP-IDF, no logging, no allocation. The caller decides what to say
 * about the verdict; this only reaches it.
 */
#ifndef NN20CLOCK_OTA_VERSION_H
#define NN20CLOCK_OTA_VERSION_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Pull the leading MAJOR.MINOR.PATCH out of a version string, with an
 * optional leading 'v'.
 *
 * "v0.2.0" and "v0.2.0-7-g1a2b3c4-dirty" both parse to {0,2,0}.
 * `out_extra`, when not NULL, says whether anything followed the patch
 * number - which is the difference between a released build and a
 * working-tree build sitting on top of that release.
 *
 * false when there is no such prefix, and `out` is then untouched.
 */
bool nn20clock_ota_parse_version(const char *text, unsigned out[3],
                                 bool *out_extra);

typedef enum {
    /* The offered version is not newer: older, or the same release. */
    NN20CLOCK_OTA_VERSION_NOT_NEWER,
    /* Install it. */
    NN20CLOCK_OTA_VERSION_NEWER,
    /*
     * The manifest's version is not a version. Nothing is offered - a
     * release that cannot name itself is not one to install.
     */
    NN20CLOCK_OTA_VERSION_OFFER_UNREADABLE,
    /*
     * The *running* version has no tag to compare against, so the
     * offered one is all either side can name. Treated as newer, and
     * worth saying out loud, which is why it is not folded into NEWER.
     */
    NN20CLOCK_OTA_VERSION_RUNNING_UNREADABLE
} NN20ClockOtaVersionVerdict;

/*
 * Whether `offered` is a release that `running` is not.
 *
 * A build made between tags - "v0.2.0-7-g1a2b3c4" - is *ahead* of
 * v0.2.0, so a manifest offering v0.2.0 to it is NOT_NEWER. That is the
 * honest answer and it stops a development board quietly replacing its
 * own firmware with the last release every time somebody opens the
 * About screen.
 */
NN20ClockOtaVersionVerdict nn20clock_ota_compare_versions(const char *offered,
                                                          const char *running);

#ifdef __cplusplus
}
#endif

#endif /* NN20CLOCK_OTA_VERSION_H */
