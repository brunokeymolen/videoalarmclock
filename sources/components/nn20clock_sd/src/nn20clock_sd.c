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
 * nn20clock_sd.c - mounting the card and reading the folders on it.
 *
 * The mount sequence is the one from tryout/videoplayback, which is the
 * configuration proven on this board: 4-bit SDMMC on the pins below,
 * with the card powered through an on-chip LDO. Design 3 is explicit
 * that hardware configuration is verified rather than assumed, and
 * these values were.
 *
 * The media half of this file has one rule: every path arrives relative
 * to the card's root and is put through nn20clock_media before the
 * filesystem sees it. resolve() below is the only place a VFS path is
 * built, which is what makes "can a client escape the card" a question
 * with one place to look.
 */
#include "nn20clock_sd.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/unistd.h>

#include "driver/sdmmc_host.h"
#include "esp_check.h"
#include "esp_vfs_fat.h"
#include "esp_ldo_regulator.h"
#include "ff.h"
#include "sdmmc_cmd.h"

static const char *TAG = "NN20CLOCK_SD";

/*
 * SLOT 0, AND THAT IS THE WHOLE POINT.
 *
 * The ESP32-P4 has two SDMMC slots. On this board the card is wired to
 * slot 0's IO MUX pins (GPIO 39-44), and the ESP32-C6 Wi-Fi
 * co-processor is reached over SDIO on the other slot (GPIO 14-19).
 *
 * SDMMC_HOST_DEFAULT() selects slot 1. Mounting the card that way puts
 * it on top of the co-processor's link: the card mounts and works
 * perfectly, then esp_hosted reports "Unrecoverable host sdio state"
 * and restarts, and the device reboot-loops. tryout/videoplayback used
 * the default because it has no Wi-Fi to conflict with - which is
 * exactly the kind of hardware configuration design 3 says must be
 * verified rather than copied by analogy.
 *
 * Slot 0 uses the IO MUX, so the pins are not assigned here: naming
 * them would route them through the GPIO matrix instead, which is
 * slower and unnecessary. They are recorded for reference only.
 *
 *   CLK 43, CMD 44, D0 39, D1 40, D2 41, D3 42
 *
 * The card is powered from the on-chip LDO VO4 at 3.3 V, acquired
 * directly. The sd_pwr_ctrl helper leaves the channel at 0 V on this
 * part ("The voltage value 0 is out of the recommended range"), which
 * is the other half of why the first attempt failed.
 */
#define SD_LDO_CHANNEL    4
#define SD_LDO_VOLTAGE_MV 3300

/*
 * Open files at once. The FTP server holds one for a transfer, playback
 * will hold another, and a couple spare costs almost nothing.
 */
#define SD_MAX_OPEN_FILES 6

static void find_fat_drive(NN20ClockSd *pthis);

struct NN20ClockSd {
    sdmmc_card_t *card;
    esp_ldo_channel_handle_t power;
    bool mounted;
    /*
     * The FatFs volume the card mounted on, as a prefix: "0:", "1:".
     *
     * Needed because listing reads the directory through FatFs rather
     * than through the VFS - see list_media() for why - and FatFs paths
     * carry their own drive number. Empty until a successful mount.
     */
    char fat_drive[4];
};

/* --------------------------------------------------------- lifecycle -- */

NN20ClockSd *nn20clock_sd_ctor(void)
{
    NN20ClockSd *pthis = calloc(1, sizeof(*pthis));
    if (pthis == NULL) {
        ESP_LOGE(TAG, "out of memory");
        return NULL;
    }
    return pthis;
}

void nn20clock_sd_dtor(NN20ClockSd *pthis)
{
    if (pthis == NULL) {
        return;
    }
    if (pthis->mounted) {
        (void)nn20clock_sd_unmount(pthis);
    }
    free(pthis);
}

esp_err_t nn20clock_sd_mount(NN20ClockSd *pthis)
{
    if (pthis == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (pthis->mounted) {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        /* Never format: a card that will not mount is a card to look
         * at, not one to erase. */
        .format_if_mount_failed = false,
        .max_files = SD_MAX_OPEN_FILES,
        .allocation_unit_size = 16 * 1024,
    };

    /* The card's rail, before the host touches it. */
    esp_ldo_channel_config_t ldo_config = {
        .chan_id = SD_LDO_CHANNEL,
        .voltage_mv = SD_LDO_VOLTAGE_MV,
    };
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo_config, &pthis->power),
                        TAG, "SD LDO VO4");

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;   /* not the default - see above */
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

    const sdmmc_slot_config_t slot_config = {
        .cd = SDMMC_SLOT_NO_CD,
        .wp = SDMMC_SLOT_NO_WP,
        .width = 4,
        /* Slot 0 is on the IO MUX: naming pins here would route them
         * through the GPIO matrix instead. */
        .flags = 0,
    };

    const esp_err_t err = esp_vfs_fat_sdmmc_mount(NN20CLOCK_SD_MOUNT_POINT,
                                                  &host, &slot_config,
                                                  &mount_config,
                                                  &pthis->card);
    if (err != ESP_OK) {
        /*
         * Not fatal. Design 15 treats a missing card as a recoverable
         * condition: the clock keeps time, and only alarm media is
         * unavailable.
         */
        ESP_LOGW(TAG, "no SD card (0x%x); media is unavailable",
                 (unsigned)err);
        if (pthis->power != NULL) {
            (void)esp_ldo_release_channel(pthis->power);
            pthis->power = NULL;
        }
        return err;
    }

    pthis->mounted = true;
    find_fat_drive(pthis);

    /* Nothing to create: the root of the card is the root folder, and a
     * card an FTP client can upload to is any card that mounts. */
    ESP_LOGI(TAG, "SD card mounted at %s; its root is the root folder",
             NN20CLOCK_SD_MOUNT_POINT);
    if (pthis->card != NULL) {
        ESP_LOGI(TAG, "card: %s, %llu MB", pthis->card->cid.name,
                 ((uint64_t)pthis->card->csd.capacity *
                  pthis->card->csd.sector_size) / (1024ULL * 1024ULL));
    }
    return ESP_OK;
}

esp_err_t nn20clock_sd_unmount(NN20ClockSd *pthis)
{
    if (pthis == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!pthis->mounted) {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t err = esp_vfs_fat_sdcard_unmount(NN20CLOCK_SD_MOUNT_POINT,
                                                     pthis->card);
    pthis->card = NULL;
    pthis->mounted = false;
    pthis->fat_drive[0] = '\0';

    if (pthis->power != NULL) {
        (void)esp_ldo_release_channel(pthis->power);
        pthis->power = NULL;
    }

    ESP_LOGI(TAG, "SD card unmounted");
    return err;
}

bool nn20clock_sd_is_mounted(const NN20ClockSd *pthis)
{
    return pthis != NULL && pthis->mounted;
}

esp_err_t nn20clock_sd_usage(NN20ClockSd *pthis, uint64_t *out_total,
                             uint64_t *out_free)
{
    if (pthis == NULL || out_total == NULL || out_free == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_total = 0;
    *out_free = 0;
    if (!pthis->mounted) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * esp_vfs_fat_info()'s third argument is FREE bytes, not used ones.
     *
     * Worth spelling out, because this read the other way for a long
     * time: the second parameter was taken for "used" and free was
     * computed as total minus it, which reported exactly the used space
     * as free. It looks right on a nearly-full card and absurd on an
     * empty one - a fresh 128 GB card with 3 GB of clips on it claimed
     * 3 GB free.
     */
    uint64_t total = 0;
    uint64_t free_bytes = 0;
    const esp_err_t err = esp_vfs_fat_info(NN20CLOCK_SD_MOUNT_POINT, &total,
                                           &free_bytes);
    if (err != ESP_OK) {
        return err;
    }

    *out_total = total;
    *out_free = (free_bytes > total) ? total : free_bytes;
    return ESP_OK;
}

/*
 * Which FatFs volume the card ended up on.
 *
 * esp_vfs_fat_sdmmc_mount() picks a drive number internally and does
 * not hand it back, and nothing in esp_vfs_fat.h folders a mount point to
 * one. So it is found by asking: a mounted volume opens its root, an
 * unmounted one answers FR_NOT_ENABLED.
 *
 * The SD card is the only FAT volume this application mounts - NVS and
 * the OTA partitions are not FAT - so the first drive that answers is
 * this card. If a second FAT volume is ever added, this has to be told
 * which one it wants rather than taking the first.
 */
static void find_fat_drive(NN20ClockSd *pthis)
{
    pthis->fat_drive[0] = '\0';

    for (int drive = 0; drive < FF_VOLUMES; drive++) {
        char prefix[8];
        (void)snprintf(prefix, sizeof(prefix), "%d:/", drive);

        FF_DIR dir;
        if (f_opendir(&dir, prefix) == FR_OK) {
            (void)f_closedir(&dir);
            (void)snprintf(pthis->fat_drive, sizeof(pthis->fat_drive), "%d:",
                           drive);
            ESP_LOGI(TAG, "card is FatFs volume %s", pthis->fat_drive);
            return;
        }
    }

    /* Listing falls back to the VFS, which cannot see the hidden bit.
     * Everything else keeps working. */
    ESP_LOGW(TAG, "cannot tell which FatFs volume the card is; hidden "
                  "entries will be listed");
}

/* ------------------------------------------------------------ media -- */

esp_err_t nn20clock_sd_path(const char *relative_path, char *out, size_t size)
{
    return nn20clock_media_path(NN20CLOCK_SD_MOUNT_POINT, relative_path, out,
                                size);
}

esp_err_t nn20clock_sd_folder_path(const char *folder_path, char *out,
                                   size_t size)
{
    return nn20clock_media_folder_path(NN20CLOCK_SD_MOUNT_POINT, folder_path,
                                       out, size);
}

/*
 * Everything a call here needs before it touches the filesystem: a
 * mounted card, and a path this device will carry.
 *
 * The order matters. The path is checked before the mount is used, so a
 * hostile path is refused identically whether or not there is a card in
 * the slot - a difference in behaviour is a difference an attacker can
 * read.
 */
static esp_err_t resolve(NN20ClockSd *pthis, const char *relative_path,
                         NN20ClockMediaPathKind kind, char *out, size_t size)
{
    if (pthis == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const esp_err_t err =
        (kind == NN20CLOCK_MEDIA_PATH_FOLDER)
            ? nn20clock_sd_folder_path(relative_path, out, size)
            : nn20clock_sd_path(relative_path, out, size);
    if (err != ESP_OK) {
        return err;   /* unsafe; refused before the card is consulted */
    }
    if (!pthis->mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

/*
 * What one directory entry is: its size, and whether it is a folder or
 * something this device should not offer at all.
 *
 * The FAT attribute bits are the point. readdir() and stat() both drop
 * every one of them except "is a directory" - ESP-IDF's vfs_fat maps
 * AM_DIR into st_mode and throws AM_HID and AM_SYS away - so the only
 * way to know a directory is the Linux trash can is to ask FatFs
 * itself. f_stat() also carries the size, so this costs no more than
 * the stat() it replaces: stat() on this VFS *is* f_stat plus a lossy
 * conversion.
 *
 * Falls back to stat() when the card's FatFs volume could not be
 * identified - see find_fat_drive(). Then nothing is hidden, which is
 * the old behaviour and the safe way to be wrong: showing a file that
 * should have been hidden is a much smaller problem than hiding one
 * that should have been shown.
 */
static void entry_info(const NN20ClockSd *pthis, const char *relative_path,
                       bool *is_folder, bool *hidden, uint32_t *size_bytes)
{
    *hidden = false;
    *size_bytes = 0u;

    if (pthis->fat_drive[0] != '\0') {
        char path[NN20CLOCK_MEDIA_FULL_PATH_MAX];
        if (snprintf(path, sizeof(path), "%s/%s", pthis->fat_drive,
                     relative_path) < (int)sizeof(path)) {
            FILINFO info;
            if (f_stat(path, &info) == FR_OK) {
                *is_folder = (info.fattrib & AM_DIR) != 0;
                *hidden = (info.fattrib & (AM_HID | AM_SYS)) != 0;
                *size_bytes = (uint32_t)info.fsize;
                return;
            }
        }
    }

    char path[NN20CLOCK_MEDIA_FULL_PATH_MAX];
    if (nn20clock_sd_folder_path(relative_path, path, sizeof(path))
        == ESP_OK) {
        struct stat posix_info;
        if (stat(path, &posix_info) == 0) {
            *is_folder = S_ISDIR(posix_info.st_mode);
            *size_bytes = (*is_folder) ? 0u : (uint32_t)posix_info.st_size;
        }
    }
}

esp_err_t nn20clock_sd_list_media(NN20ClockSd *pthis, const char *folder_path,
                                  NN20ClockMediaList *out_list)
{
    if (pthis == NULL || out_list == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Cleared to a valid empty listing of the requested folder before
     * anything can fail, so a caller that ignores the error still reads
     * a list rather than whatever was in the buffer. */
    memset(out_list, 0, sizeof(*out_list));
    const esp_err_t initialised = nn20clock_media_list_init(out_list,
                                                            folder_path);
    if (initialised != ESP_OK) {
        return initialised;
    }

    char directory[NN20CLOCK_MEDIA_FULL_PATH_MAX];
    const esp_err_t ready = resolve(pthis, folder_path,
                                    NN20CLOCK_MEDIA_PATH_FOLDER, directory,
                                    sizeof(directory));
    if (ready != ESP_OK) {
        return ready;
    }

    DIR *dir = opendir(directory);
    if (dir == NULL) {
        ESP_LOGW(TAG, "cannot open %s", directory);
        return ESP_ERR_NOT_FOUND;
    }

    const struct dirent *entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        bool is_folder = (entry->d_type == DT_DIR);
        bool hidden = false;
        uint32_t size_bytes = 0u;

        char relative[NN20CLOCK_MEDIA_PATH_MAX];
        if (nn20clock_media_path_join(folder_path, entry->d_name, relative,
                                      sizeof(relative)) != ESP_OK) {
            /* A name that makes a path longer than this device carries.
             * Counted, never served - and the add below would refuse it
             * anyway; this just saves asking the card about it. */
            out_list->skipped++;
            continue;
        }
        entry_info(pthis, relative, &is_folder, &hidden, &size_bytes);

        /*
         * Two reasons not to list something that is perfectly readable.
         *
         * FAT's hidden and system bits are the general rule, and the
         * one a user can set for themselves - see SITE HIDE in
         * nn20clock_ftp.h.
         *
         * The name list is for the case the bits do not cover, which
         * turned out to be the common one: Linux's ".Trash-1000"
         * arrives here as "TRASH-~1" with NO hidden bit, because gio
         * never sets one and the leading dot that hides it on a desktop
         * does not survive 8.3. Checked on the card this was written
         * against.
         *
         * Either way they are counted in `skipped`, like anything else
         * that is on the card and not offered - "3 not listed" rather
         * than a silent disappearance.
         *
         * Not listed is not the same as not addressable: the delete and
         * rename calls take a path and do not consult either rule, so a
         * client that knows the name can still remove one. This is
         * about what the device offers, not about what it protects.
         */
        if (hidden || nn20clock_media_name_is_housekeeping(entry->d_name)) {
            out_list->skipped++;
            continue;
        }

        esp_err_t added;
        if (is_folder) {
            /* A folder is listed, not descended into: this is one folder's
             * contents, and the picker enters a child when it is
             * chosen. */
            added = nn20clock_media_list_add_folder(out_list, entry->d_name);
        } else {
            added = nn20clock_media_list_add_file(out_list, entry->d_name,
                                                  size_bytes);
        }

        if (added == ESP_ERR_NO_MEM) {
            /* Full. Everything past this point is invisible, so say so
             * rather than pretend the folder holds only this many. */
            ESP_LOGW(TAG, "more than %d entries; the rest are not listed",
                     NN20CLOCK_MEDIA_MAX);
            out_list->skipped++;
            break;
        }
        if (added != ESP_OK) {
            /* An unsafe name. Counted, never served. */
            out_list->skipped++;
        }
    }
    closedir(dir);

    nn20clock_media_list_sort(out_list);
    ESP_LOGI(TAG, "%u entr%s in %s, %u skipped", (unsigned)out_list->count,
             (out_list->count == 1u) ? "y" : "ies", directory,
             (unsigned)out_list->skipped);
    return ESP_OK;
}

esp_err_t nn20clock_sd_set_hidden(NN20ClockSd *pthis,
                                  const char *relative_path, bool hidden)
{
    /*
     * Asked for as a folder path, which is the permissive one: either end
     * of this can be a file or a folder. "" - the card itself - is refused
     * below, because the root has no directory entry to carry a bit.
     */
    char unused[NN20CLOCK_MEDIA_FULL_PATH_MAX];
    const esp_err_t ready = resolve(pthis, relative_path,
                                    NN20CLOCK_MEDIA_PATH_FOLDER, unused,
                                    sizeof(unused));
    if (ready != ESP_OK) {
        return ready;
    }
    if (relative_path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    if (pthis->fat_drive[0] == '\0') {
        /* Without the volume there is no FatFs path to hand f_chmod,
         * and the bit would not be read back either. */
        return ESP_ERR_NOT_SUPPORTED;
    }

    char path[NN20CLOCK_MEDIA_FULL_PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/%s", pthis->fat_drive,
                 relative_path) >= (int)sizeof(path)) {
        return ESP_ERR_INVALID_SIZE;
    }

    /* f_chmod's third argument is the mask of bits to touch, so this
     * leaves read-only and archive alone. AM_SYS is not set here: the
     * listing hides it too, but it means "the operating system put this
     * here", which is not something this device is entitled to claim. */
    if (f_chmod(path, hidden ? AM_HID : 0, AM_HID) != FR_OK) {
        ESP_LOGW(TAG, "cannot %s %s", hidden ? "hide" : "show",
                 relative_path);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "%s %s", hidden ? "hid" : "unhid", relative_path);
    return ESP_OK;
}

esp_err_t nn20clock_sd_stat(NN20ClockSd *pthis, const char *relative_path,
                            bool *out_is_dir, uint32_t *out_size)
{
    if (out_is_dir == NULL || out_size == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_is_dir = false;
    *out_size = 0u;

    /*
     * Asked as a folder path, which is the more permissive of the two:
     * this answers "what is at this path", and the caller decides
     * whether what it found is what it wanted. "" is the root folder,
     * which is always a directory and never needs the filesystem.
     */
    char path[NN20CLOCK_MEDIA_FULL_PATH_MAX];
    const esp_err_t ready = resolve(pthis, relative_path,
                                    NN20CLOCK_MEDIA_PATH_FOLDER, path,
                                    sizeof(path));
    if (ready != ESP_OK) {
        return ready;
    }

    struct stat info;
    if (stat(path, &info) != 0) {
        return ESP_ERR_NOT_FOUND;
    }

    *out_is_dir = S_ISDIR(info.st_mode);
    *out_size = (*out_is_dir) ? 0u : (uint32_t)info.st_size;
    return ESP_OK;
}

esp_err_t nn20clock_sd_delete_file(NN20ClockSd *pthis,
                                   const char *relative_path)
{
    char path[NN20CLOCK_MEDIA_FULL_PATH_MAX];
    const esp_err_t ready = resolve(pthis, relative_path,
                                    NN20CLOCK_MEDIA_PATH_FILE, path,
                                    sizeof(path));
    if (ready != ESP_OK) {
        return ready;
    }

    /*
     * A directory is not a file to delete, and unlink() on FATFS is not
     * reliably the one that says so. Checked here, so "DELE morning"
     * cannot become an accident with a different name.
     */
    struct stat info;
    if (stat(path, &info) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    if (S_ISDIR(info.st_mode)) {
        ESP_LOGW(TAG, "%s is a folder, not a file", relative_path);
        return ESP_ERR_INVALID_ARG;
    }

    if (unlink(path) != 0) {
        ESP_LOGW(TAG, "cannot delete %s", relative_path);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "deleted %s", relative_path);
    return ESP_OK;
}

esp_err_t nn20clock_sd_make_folder(NN20ClockSd *pthis, const char *folder_path)
{
    if (folder_path != NULL && folder_path[0] == '\0') {
        /* The root folder is the card. It is not something to create. */
        return ESP_ERR_INVALID_ARG;
    }

    char path[NN20CLOCK_MEDIA_FULL_PATH_MAX];
    const esp_err_t ready = resolve(pthis, folder_path,
                                    NN20CLOCK_MEDIA_PATH_FOLDER, path,
                                    sizeof(path));
    if (ready != ESP_OK) {
        return ready;
    }

    if (mkdir(path, 0777) != 0) {
        /* Already there, or the parent is not - one message, because
         * FATFS does not reliably tell the two apart through errno and
         * a wrong reason is worse than a vague one. */
        ESP_LOGW(TAG, "cannot create the folder %s", folder_path);
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "created the folder %s", folder_path);
    return ESP_OK;
}

/* Whether a directory has anything at all in it, "." and ".." aside -
 * FATFS reports neither, but a VFS that did would make an empty folder
 * look full. */
static bool directory_is_empty(const char *path)
{
    DIR *dir = opendir(path);
    if (dir == NULL) {
        return false;
    }

    bool empty = true;
    const struct dirent *entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        empty = false;
        break;
    }

    closedir(dir);
    return empty;
}

esp_err_t nn20clock_sd_delete_folder(NN20ClockSd *pthis,
                                     const char *folder_path)
{
    if (folder_path != NULL && folder_path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;   /* the root folder is the card */
    }

    char path[NN20CLOCK_MEDIA_FULL_PATH_MAX];
    const esp_err_t ready = resolve(pthis, folder_path,
                                    NN20CLOCK_MEDIA_PATH_FOLDER, path,
                                    sizeof(path));
    if (ready != ESP_OK) {
        return ready;
    }

    struct stat info;
    if (stat(path, &info) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    if (!S_ISDIR(info.st_mode)) {
        ESP_LOGW(TAG, "%s is a file, not a folder", folder_path);
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * Checked rather than left to rmdir(). This is the whole safety
     * property of the operation - see the header - and it is not one to
     * hand to a filesystem layer whose errno this project does not
     * otherwise trust.
     */
    if (!directory_is_empty(path)) {
        ESP_LOGW(TAG, "the folder %s is not empty", folder_path);
        return ESP_ERR_INVALID_STATE;
    }

    if (rmdir(path) != 0) {
        ESP_LOGW(TAG, "cannot delete the folder %s", folder_path);
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "deleted the folder %s", folder_path);
    return ESP_OK;
}

esp_err_t nn20clock_sd_rename(NN20ClockSd *pthis, const char *from_path,
                              const char *to_path)
{
    /*
     * Both are asked for as folder paths, which is the check that allows
     * either end to be a file or a folder - the one thing it refuses is
     * "", which would be the card itself at one end of a move.
     */
    if (from_path == NULL || to_path == NULL || from_path[0] == '\0' ||
        to_path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    char from[NN20CLOCK_MEDIA_FULL_PATH_MAX];
    char to[NN20CLOCK_MEDIA_FULL_PATH_MAX];
    const esp_err_t from_ready = resolve(pthis, from_path,
                                         NN20CLOCK_MEDIA_PATH_FOLDER, from,
                                         sizeof(from));
    if (from_ready != ESP_OK) {
        return from_ready;
    }
    const esp_err_t to_ready = resolve(pthis, to_path,
                                       NN20CLOCK_MEDIA_PATH_FOLDER, to,
                                       sizeof(to));
    if (to_ready != ESP_OK) {
        return to_ready;
    }

    struct stat info;
    if (stat(from, &info) != 0) {
        return ESP_ERR_NOT_FOUND;
    }

    /*
     * No overwrite. FTP clients expect one and this refuses anyway: the
     * card holds the only copy of a clip, and a mistyped RNTO that
     * silently replaces one is a worse outcome than a client showing an
     * error. See the header.
     */
    if (stat(to, &info) == 0) {
        ESP_LOGW(TAG, "%s already exists", to_path);
        return ESP_ERR_INVALID_STATE;
    }

    if (rename(from, to) != 0) {
        /* The target's parent folder does not exist, or the card refused.
         * Either way nothing moved. */
        ESP_LOGW(TAG, "cannot rename %s to %s", from_path, to_path);
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "renamed %s to %s", from_path, to_path);
    return ESP_OK;
}

esp_err_t nn20clock_sd_file_size(NN20ClockSd *pthis,
                                 const char *relative_path,
                                 uint32_t *out_size)
{
    if (out_size == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_size = 0u;

    char path[NN20CLOCK_MEDIA_FULL_PATH_MAX];
    const esp_err_t ready = resolve(pthis, relative_path,
                                    NN20CLOCK_MEDIA_PATH_FILE, path,
                                    sizeof(path));
    if (ready != ESP_OK) {
        return ready;
    }

    struct stat info;
    if (stat(path, &info) != 0 || S_ISDIR(info.st_mode)) {
        return ESP_ERR_NOT_FOUND;
    }

    *out_size = (uint32_t)info.st_size;
    return ESP_OK;
}
