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
 * nn20clock_sd.h - the SD card (design 9's SD side).
 *
 * FIRMWARE ONLY: SDMMC and FATFS. The rules about what media is called
 * and which names are safe live in nn20clock_media, which is dual-mode
 * and tested on the host; this component is the part that actually
 * touches a card.
 *
 * The mount sequence - 4-bit SDMMC on this board's pins, powered
 * through an on-chip LDO - is the one proven in tryout/videoplayback.
 *
 * Media is organised in folders - directories on the card, the root
 * included. Every path here is relative to the mount point and is
 * validated by nn20clock_media before the filesystem sees it, so no
 * caller - and no FTP client behind one - can address anything outside
 * the card.
 *
 * ------------------------------------------------------------------
 * NAMES ON THE CARD ARE 8.3
 * ------------------------------------------------------------------
 *
 * The firmware builds FATFS with CONFIG_FATFS_LFN_NONE, so long file
 * names are off: FAT accepts at most eight characters plus a
 * three-character extension, and hands every name back in UPPER CASE
 * whatever it was written as.
 *
 * nn20clock_media caps a name at 255 bytes because that is what FAT
 * would allow with long names on, and it is filesystem-agnostic. This
 * is the layer that meets the actual filesystem, so the tighter limit
 * is written down here: a folder called "weekend" is fine, one called
 * "weekend films" cannot be created, and one uploaded as "Morning"
 * comes back as "MORNING". Turning long names on is a Kconfig change
 * and a rebuild, not a code change - but it is a decision about the
 * product, so it is not made quietly here.
 *
 * Threading: every call here blocks on the filesystem, sometimes for a
 * long time on a slow card. They run on the StorageWorker (design 4
 * puts flash and SD there precisely so nothing else waits on them) or,
 * for the FTP server, on its own task. Never on the UiWorker.
 *
 * A missing or unreadable card is not a failure of the clock: design 15
 * treats it as a recoverable condition, so mounting reports it and the
 * rest of the device carries on.
 */
#ifndef NN20CLOCK_SD_H
#define NN20CLOCK_SD_H

#include <stdbool.h>
#include <stdint.h>

#include "nn20clock_media.h"
#include "nn20clock_platform.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NN20ClockSd NN20ClockSd;

/* Where the card is mounted in the VFS. Media lives in it and in the
 * folders below it, so a clip is at "/sdcard/<relative path>". */
#define NN20CLOCK_SD_MOUNT_POINT "/sdcard"

/* --------------------------------------------------------- lifecycle -- */

NN20ClockSd *nn20clock_sd_ctor(void);

/* Unmounts if mounted. Safe with NULL. */
void nn20clock_sd_dtor(NN20ClockSd *pthis);

/*
 * Power and mount the card. Folders are whatever directories are already
 * on it, so there is nothing to create.
 *
 * Returns the underlying error when there is no card or it cannot be
 * read - the caller is expected to carry on without media rather than
 * treat it as fatal.
 */
esp_err_t nn20clock_sd_mount(NN20ClockSd *pthis);

/* Unmount and power down. Idempotent. */
esp_err_t nn20clock_sd_unmount(NN20ClockSd *pthis);

bool nn20clock_sd_is_mounted(const NN20ClockSd *pthis);

/* Card capacity and free space, in bytes. Zero when not mounted. */
esp_err_t nn20clock_sd_usage(NN20ClockSd *pthis, uint64_t *out_total,
                             uint64_t *out_free);

/* ------------------------------------------------------------ media -- */

/*
 * Everything directly in one folder, sorted folders-first and then by name.
 *
 * `folder_path` is a relative folder path - "" for the card's root, "morning"
 * for a folder, "weekend/kids" for a nested one. This does NOT recurse:
 * listing "morning" gives what is in "morning" and never what is in
 * "morning/kids". Child folders appear as entries to enter, which is what
 * makes a deep card browsable from a screen that can show six rows.
 *
 * Entries this device will not serve - unsafe names, and anything past
 * NN20CLOCK_MEDIA_MAX - are counted in `skipped` rather than hidden: a
 * card with twelve clips and three stray files should say so, because
 * "where did my file go" is a worse experience than a listing that
 * mentions it.
 *
 * ESP_ERR_INVALID_ARG for an unsafe folder path, ESP_ERR_NOT_FOUND when
 * there is no such folder.
 *
 * out_list is over 30 KB - it belongs on the heap, never on a stack.
 */
esp_err_t nn20clock_sd_list_media(NN20ClockSd *pthis, const char *folder_path,
                                  NN20ClockMediaList *out_list);

/*
 * Delete one file. `relative_path` is a file path, not a folder path:
 * ESP_ERR_INVALID_ARG when it is not safe, ESP_ERR_NOT_FOUND when there
 * is no such file, and ESP_ERR_INVALID_ARG again when it names a
 * directory - removing one is nn20clock_sd_delete_folder()'s job, and
 * it has a rule of its own.
 */
esp_err_t nn20clock_sd_delete_file(NN20ClockSd *pthis,
                                   const char *relative_path);

/*
 * Create a folder. The parent must already exist: this makes one
 * directory, not a chain of them, because an FTP client that mistypes a
 * path should get an error rather than a tree.
 *
 * ESP_ERR_INVALID_ARG for an unsafe or empty path (the root folder already
 * exists), ESP_ERR_INVALID_STATE when something is already there.
 */
esp_err_t nn20clock_sd_make_folder(NN20ClockSd *pthis,
                                   const char *folder_path);

/*
 * Delete an EMPTY folder. ESP_ERR_INVALID_STATE when it still has
 * anything in it.
 *
 * Deliberately not recursive. "Delete this directory and everything
 * under it" is one mistyped path away from an empty card, and an FTP
 * client will happily send it without asking. A recursive delete can be
 * an explicit feature the day a screen asks for one.
 */
esp_err_t nn20clock_sd_delete_folder(NN20ClockSd *pthis,
                                     const char *folder_path);

/*
 * Rename or move a file or a folder. Both paths are relative and both must
 * be safe, so a move can cross folders but can never leave the card:
 *
 *     morning/wake.avi  ->  weekend/wake.avi
 *
 * The target's parent must already exist, and an existing target is
 * REFUSED (ESP_ERR_INVALID_STATE) rather than overwritten. FTP clients
 * expect overwrite; a clock whose only copy of a clip can be replaced
 * by a typo is worse than a client showing an error.
 */
esp_err_t nn20clock_sd_rename(NN20ClockSd *pthis, const char *from_path,
                              const char *to_path);

/* Size in bytes; ESP_ERR_NOT_FOUND when there is no such file. */
esp_err_t nn20clock_sd_file_size(NN20ClockSd *pthis,
                                 const char *relative_path,
                                 uint32_t *out_size);

/*
 * Show or hide one file or folder.
 *
 * Sets or clears FAT's hidden attribute, which is what keeps an entry
 * out of every listing this device produces - the picker, the playback
 * screen and FTP alike. It does NOT make the entry unreachable: delete,
 * rename and size still take a path and do not consult the bit, so
 * something hidden can still be cleaned up by name. Hiding is about
 * what this device offers, not about what it protects.
 *
 * There is no POSIX way to do this and no standard FTP command for it;
 * see the SITE HIDE extension in nn20clock_ftp.h for how it is reached
 * from a client.
 *
 * ESP_ERR_NOT_SUPPORTED when the card's FatFs volume could not be
 * identified, which is the same condition that leaves the hidden bit
 * unreadable - see the listing.
 */
esp_err_t nn20clock_sd_set_hidden(NN20ClockSd *pthis,
                                  const char *relative_path, bool hidden);

/*
 * What is at a path, if anything: a file with a size, or a folder.
 * ESP_ERR_NOT_FOUND when there is nothing there. The FTP server needs
 * this to answer LIST and SIZE for a path it was handed rather than one
 * it is sitting in.
 */
esp_err_t nn20clock_sd_stat(NN20ClockSd *pthis, const char *relative_path,
                            bool *out_is_dir, uint32_t *out_size);

/*
 * Full VFS path for a relative file path, e.g.
 * "/sdcard/morning/steam720.avi". The FTP server uses this to open
 * files; it refuses anything that is not a safe relative path under the
 * card's root.
 */
esp_err_t nn20clock_sd_path(const char *relative_path, char *out,
                            size_t size);

/* The same for a folder path, where "" is the mount point itself. */
esp_err_t nn20clock_sd_folder_path(const char *folder_path, char *out,
                                size_t size);

#ifdef __cplusplus
}
#endif

#endif /* NN20CLOCK_SD_H */
