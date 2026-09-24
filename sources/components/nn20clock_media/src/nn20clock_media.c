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
 * nn20clock_media.c - see the header.
 *
 * The interesting functions here are the path ones. They are the only
 * thing standing between an FTP client on the network and the rest of
 * the filesystem, so they reject rather than sanitise: a path is either
 * already safe or it is refused. The single exception is
 * nn20clock_media_path_resolve(), which is where a client's "..", its
 * leading slash and its trailing slash are interpreted - once, in one
 * place, into a canonical relative path that every other function here
 * then treats as untrusted anyway.
 */
#include "nn20clock_media.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "NN20CLOCK_MEDIA";

/* ------------------------------------------------------------- kind -- */

/* Case-insensitive suffix test; FAT names come back in any case. */
static bool ends_with(const char *name, const char *suffix)
{
    const size_t name_len = strlen(name);
    const size_t suffix_len = strlen(suffix);
    if (name_len < suffix_len) {
        return false;
    }

    const char *tail = name + (name_len - suffix_len);
    for (size_t i = 0; i < suffix_len; i++) {
        char a = tail[i];
        char b = suffix[i];
        if (a >= 'A' && a <= 'Z') {
            a = (char)(a - 'A' + 'a');
        }
        if (b >= 'A' && b <= 'Z') {
            b = (char)(b - 'A' + 'a');
        }
        if (a != b) {
            return false;
        }
    }
    return true;
}

NN20ClockMediaKind nn20clock_media_kind(const char *name)
{
    if (name == NULL) {
        return NN20CLOCK_MEDIA_KIND_UNKNOWN;
    }

    /* Design 16: AVI with MJPEG video and PCM audio is the proven
     * format for the first version. */
    if (ends_with(name, ".avi")) {
        return NN20CLOCK_MEDIA_KIND_VIDEO;
    }
    if (ends_with(name, ".wav")) {
        return NN20CLOCK_MEDIA_KIND_AUDIO;
    }
    return NN20CLOCK_MEDIA_KIND_UNKNOWN;
}

const char *nn20clock_media_kind_name(NN20ClockMediaKind kind)
{
    switch (kind) {
    case NN20CLOCK_MEDIA_KIND_VIDEO:   return "video";
    case NN20CLOCK_MEDIA_KIND_AUDIO:   return "audio";
    case NN20CLOCK_MEDIA_KIND_UNKNOWN: break;
    }
    return "other";
}

/*
 * The 8.3 names desktop operating systems leave on a card. See the
 * header for why this is a list rather than a pattern.
 */
static const char *const HOUSEKEEPING_NAMES[] = {
    "TRASH-~1",   /* Linux: .Trash-1000, and the rest of .Trash-<uid> */
    "TRASH-~2",
    "TRASH-~3",
    "SYSTEM~1",   /* Windows: System Volume Information */
    "RECYCL~1",   /* Windows: $RECYCLE.BIN */
    "$RECYCLE.BIN",
    "FSEVEN~1",   /* macOS: .fseventsd */
    "SPOTLI~1",   /* macOS: .Spotlight-V100 */
    "TRASHE~1",   /* macOS: .Trashes */
    ".DS_STORE",
};

bool nn20clock_media_name_is_housekeeping(const char *name)
{
    if (name == NULL) {
        return false;
    }

    const size_t count = sizeof(HOUSEKEEPING_NAMES) /
                         sizeof(HOUSEKEEPING_NAMES[0]);
    for (size_t i = 0; i < count; i++) {
        const char *const known = HOUSEKEEPING_NAMES[i];
        size_t j = 0;
        for (;; j++) {
            char left = name[j];
            char right = known[j];
            if (left >= 'a' && left <= 'z') {
                left = (char)(left - 'a' + 'A');
            }
            if (left != right) {
                break;
            }
            if (left == '\0') {
                return true;
            }
        }
    }
    return false;
}

/* ------------------------------------------------------ path safety -- */

/* One component, given as a length rather than a terminator so the path
 * check can use it on a slice without copying. */
static bool component_is_safe(const char *name, size_t length)
{
    if (name == NULL || length == 0u) {
        return false;   /* "" and "morning//wake.avi" alike */
    }
    if (length >= NN20CLOCK_MEDIA_NAME_MAX) {
        return false;   /* longer than FAT allows */
    }

    /* "." and ".." are directory entries, not places to go. Resolving
     * them is nn20clock_media_path_resolve()'s job and nothing else's. */
    if (length == 1u && name[0] == '.') {
        return false;
    }
    if (length == 2u && name[0] == '.' && name[1] == '.') {
        return false;
    }

    for (size_t i = 0; i < length; i++) {
        const char c = name[i];
        /*
         * A separator means this is a path, not a name - refused
         * outright rather than trimmed. Backslash counts too: an FTP
         * client on Windows may send one, and a filesystem that ignores
         * it is not a reason to let it through.
         */
        if (c == '/' || c == '\\') {
            return false;
        }
        /* Control characters have no business in a file name and make a
         * mess of an FTP listing, which is line-based. */
        if ((unsigned char)c < 0x20 || (unsigned char)c == 0x7F) {
            return false;
        }
    }
    return true;
}

bool nn20clock_media_name_is_safe_component(const char *name)
{
    if (name == NULL) {
        return false;
    }
    const size_t length = strnlen(name, NN20CLOCK_MEDIA_NAME_MAX);
    if (length >= NN20CLOCK_MEDIA_NAME_MAX) {
        return false;   /* unterminated inside the field */
    }
    return component_is_safe(name, length);
}

bool nn20clock_media_path_is_safe(const char *path,
                                  NN20ClockMediaPathKind kind)
{
    if (path == NULL) {
        return false;
    }

    const size_t length = strnlen(path, NN20CLOCK_MEDIA_PATH_MAX);
    if (length >= NN20CLOCK_MEDIA_PATH_MAX) {
        return false;   /* unterminated, or longer than this device caps */
    }

    if (length == 0u) {
        /* The root folder is a place; a file with no name is not. */
        return kind == NN20CLOCK_MEDIA_PATH_FOLDER;
    }

    /* A leading slash is an absolute path and a trailing one is a
     * canonicalisation this function does not do - both are somebody
     * else's string, not this one. */
    const char *cursor = path;
    for (;;) {
        const char *slash = strchr(cursor, '/');
        const size_t part = (slash != NULL) ? (size_t)(slash - cursor)
                                            : strlen(cursor);
        if (!component_is_safe(cursor, part)) {
            return false;
        }
        if (slash == NULL) {
            return true;
        }
        cursor = slash + 1;   /* a trailing slash lands on "", refused */
    }
}

esp_err_t nn20clock_media_path(const char *mount_point,
                               const char *relative_path,
                               char *out, size_t size)
{
    if (mount_point == NULL || out == NULL || size == 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';

    if (!nn20clock_media_path_is_safe(relative_path,
                                      NN20CLOCK_MEDIA_PATH_FILE)) {
        ESP_LOGW(TAG, "refusing an unsafe media path");
        return ESP_ERR_INVALID_ARG;
    }

    const int written = snprintf(out, size, "%s/%s", mount_point,
                                 relative_path);
    if (written < 0 || (size_t)written >= size) {
        out[0] = '\0';
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

esp_err_t nn20clock_media_folder_path(const char *mount_point,
                                   const char *folder_path,
                                   char *out, size_t size)
{
    if (mount_point == NULL || out == NULL || size == 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';

    if (!nn20clock_media_path_is_safe(folder_path,
                                      NN20CLOCK_MEDIA_PATH_FOLDER)) {
        ESP_LOGW(TAG, "refusing an unsafe folder path");
        return ESP_ERR_INVALID_ARG;
    }

    /* The root folder is the mount point itself - "/sdcard/" would work on
     * FATFS and is still not what this device means by a path. */
    const int written = (folder_path[0] == '\0')
                            ? snprintf(out, size, "%s", mount_point)
                            : snprintf(out, size, "%s/%s", mount_point,
                                       folder_path);
    if (written < 0 || (size_t)written >= size) {
        out[0] = '\0';
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

esp_err_t nn20clock_media_path_join(const char *folder_path, const char *name,
                                    char *out, size_t size)
{
    if (out == NULL || size == 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';

    if (!nn20clock_media_path_is_safe(folder_path,
                                      NN20CLOCK_MEDIA_PATH_FOLDER) ||
        !nn20clock_media_name_is_safe_component(name)) {
        return ESP_ERR_INVALID_ARG;
    }

    const int written = (folder_path[0] == '\0')
                            ? snprintf(out, size, "%s", name)
                            : snprintf(out, size, "%s/%s", folder_path, name);
    if (written < 0 || (size_t)written >= size ||
        (size_t)written >= NN20CLOCK_MEDIA_PATH_MAX) {
        /* Too long for the buffer, or too long for this device even if
         * the buffer would take it - a path nothing else could store. */
        out[0] = '\0';
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

esp_err_t nn20clock_media_path_parent(const char *path, char *out,
                                      size_t size)
{
    if (out == NULL || size == 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';

    if (!nn20clock_media_path_is_safe(path, NN20CLOCK_MEDIA_PATH_FOLDER)) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *const slash = strrchr(path, '/');
    if (slash == NULL) {
        return ESP_OK;   /* top level: the parent is the root folder */
    }

    const size_t length = (size_t)(slash - path);
    if (length >= size) {
        out[0] = '\0';
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(out, path, length);
    out[length] = '\0';
    return ESP_OK;
}

const char *nn20clock_media_path_name(const char *path)
{
    if (path == NULL) {
        return "";
    }
    const char *const slash = strrchr(path, '/');
    return (slash != NULL) ? (slash + 1) : path;
}

/* ------------------------------------------------- path resolution -- */

/* Append one component to a path being built, or fail if it will not
 * fit. `used` is the length so far, terminator excluded. */
static bool append_component(char *out, size_t size, size_t *used,
                             const char *name, size_t length)
{
    const size_t separator = (*used > 0u) ? 1u : 0u;
    if (*used + separator + length + 1u > size) {
        return false;
    }
    if (separator != 0u) {
        out[*used] = '/';
        (*used)++;
    }
    memcpy(out + *used, name, length);
    *used += length;
    out[*used] = '\0';
    return true;
}

/* Drop the last component. False when there is none - which is a client
 * asking for the parent of the root, and is refused rather than
 * silently clamped: "/.." and "morning/../.." are different mistakes
 * and only one of them is navigation. */
static bool drop_component(char *out, size_t *used)
{
    if (*used == 0u) {
        return false;
    }
    char *const slash = strrchr(out, '/');
    *used = (slash != NULL) ? (size_t)(slash - out) : 0u;
    out[*used] = '\0';
    return true;
}

esp_err_t nn20clock_media_path_resolve(const char *base_folder,
                                       const char *input,
                                       char *out, size_t size)
{
    if (out == NULL || size == 0u || input == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';

    if (base_folder == NULL) {
        base_folder = "";
    }
    if (!nn20clock_media_path_is_safe(base_folder,
                                      NN20CLOCK_MEDIA_PATH_FOLDER)) {
        /* The session's own current directory is not something a client
         * can set to an unsafe value - but if it ever were, this is
         * where it stops. */
        return ESP_ERR_INVALID_ARG;
    }

    const size_t input_length = strnlen(input, NN20CLOCK_MEDIA_PATH_MAX * 2u);
    if (input_length >= NN20CLOCK_MEDIA_PATH_MAX * 2u) {
        return ESP_ERR_INVALID_SIZE;
    }

    /*
     * An absolute path starts from the root; anything else starts from
     * where the session already is. That is the whole of FTP's path
     * model, and it is two lines because "..", the only other thing a
     * client can say, is handled below.
     */
    size_t used = 0u;
    if (input[0] != '/') {
        const size_t base_length = strlen(base_folder);
        if (base_length + 1u > size) {
            return ESP_ERR_INVALID_SIZE;
        }
        memcpy(out, base_folder, base_length + 1u);
        used = base_length;
    }

    const char *cursor = input;
    while (*cursor != '\0') {
        while (*cursor == '/') {
            cursor++;   /* leading, doubled and trailing slashes alike */
        }
        if (*cursor == '\0') {
            break;
        }

        const char *const slash = strchr(cursor, '/');
        const size_t length = (slash != NULL) ? (size_t)(slash - cursor)
                                              : strlen(cursor);

        if (length == 1u && cursor[0] == '.') {
            /* "." is where we already are. */
        } else if (length == 2u && cursor[0] == '.' && cursor[1] == '.') {
            if (!drop_component(out, &used)) {
                /* Above the root. Refused, not clamped - see the header:
                 * a path that tries to leave the card is an answer, not
                 * an input to repair. */
                out[0] = '\0';
                ESP_LOGW(TAG, "refusing a path that leaves the card");
                return ESP_ERR_INVALID_ARG;
            }
        } else if (!component_is_safe(cursor, length)) {
            out[0] = '\0';
            return ESP_ERR_INVALID_ARG;
        } else if (!append_component(out, size, &used, cursor, length)) {
            out[0] = '\0';
            return ESP_ERR_INVALID_SIZE;
        }

        cursor = (slash != NULL) ? (slash + 1) : (cursor + length);
    }

    if (used >= NN20CLOCK_MEDIA_PATH_MAX) {
        out[0] = '\0';
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

/* ------------------------------------------------------------- list -- */

esp_err_t nn20clock_media_list_init(NN20ClockMediaList *list,
                                    const char *folder_path)
{
    if (list == NULL ||
        !nn20clock_media_path_is_safe(folder_path,
                                      NN20CLOCK_MEDIA_PATH_FOLDER)) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(list, 0, sizeof(*list));
    snprintf(list->folder_path, sizeof(list->folder_path), "%s", folder_path);
    return ESP_OK;
}

/* Both add_ functions differ only in what they fill in afterwards. */
static esp_err_t list_add(NN20ClockMediaList *list, const char *name,
                          NN20ClockMediaEntry **out_entry)
{
    if (list == NULL || !nn20clock_media_name_is_safe_component(name)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (list->count >= NN20CLOCK_MEDIA_MAX) {
        return ESP_ERR_NO_MEM;
    }

    NN20ClockMediaEntry *const entry = &list->entries[list->count];
    /* Built before anything is committed: a name that is fine on its
     * own can still make a path this device will not carry, and that is
     * an entry to skip rather than one to hand out half-formed. */
    const esp_err_t err = nn20clock_media_path_join(list->folder_path, name,
                                                    entry->path,
                                                    sizeof(entry->path));
    if (err != ESP_OK) {
        entry->path[0] = '\0';
        return ESP_ERR_INVALID_ARG;
    }

    snprintf(entry->name, sizeof(entry->name), "%s", name);
    entry->size_bytes = 0u;
    entry->kind = NN20CLOCK_MEDIA_KIND_UNKNOWN;
    list->count++;
    *out_entry = entry;
    return ESP_OK;
}

esp_err_t nn20clock_media_list_add_file(NN20ClockMediaList *list,
                                        const char *name,
                                        uint32_t size_bytes)
{
    NN20ClockMediaEntry *entry = NULL;
    const esp_err_t err = list_add(list, name, &entry);
    if (err != ESP_OK) {
        return err;
    }

    entry->type = NN20CLOCK_MEDIA_ENTRY_FILE;
    entry->size_bytes = size_bytes;
    entry->kind = nn20clock_media_kind(name);
    return ESP_OK;
}

esp_err_t nn20clock_media_list_add_folder(NN20ClockMediaList *list,
                                       const char *name)
{
    NN20ClockMediaEntry *entry = NULL;
    const esp_err_t err = list_add(list, name, &entry);
    if (err != ESP_OK) {
        return err;
    }

    entry->type = NN20CLOCK_MEDIA_ENTRY_FOLDER;
    return ESP_OK;
}

static int compare_names(const char *a, const char *b)
{
    /* Case-insensitive, so "Alarm.avi" and "alarm.avi" sort together
     * rather than in ASCII order with all the capitals first. */
    for (size_t i = 0;; i++) {
        char left = a[i];
        char right = b[i];
        if (left >= 'A' && left <= 'Z') {
            left = (char)(left - 'A' + 'a');
        }
        if (right >= 'A' && right <= 'Z') {
            right = (char)(right - 'A' + 'a');
        }
        if (left != right) {
            return (left < right) ? -1 : 1;
        }
        if (left == '\0') {
            return 0;
        }
    }
}

/* Folders before files, then by name. One comparison, so the sort below
 * stays the three lines it was. */
static int compare_entries(const NN20ClockMediaEntry *a,
                           const NN20ClockMediaEntry *b)
{
    if (a->type != b->type) {
        return (a->type == NN20CLOCK_MEDIA_ENTRY_FOLDER) ? -1 : 1;
    }
    return compare_names(a->name, b->name);
}

void nn20clock_media_list_sort(NN20ClockMediaList *list)
{
    if (list == NULL || list->count < 2u) {
        return;
    }

    /*
     * Insertion sort: the list is capped at 64 entries and usually far
     * smaller, and this needs no scratch space beyond the one entry it
     * holds while it shuffles.
     *
     * That entry is over half a kilobyte now that it carries a path as
     * well as a name, and this can run on the StorageWorker, whose
     * stack is 4 KB. Worth knowing before anyone adds a second local
     * here - the list itself is on the heap for exactly this reason,
     * and a static would be worse than either: the FTP tasks and the
     * UiWorker sort their own lists, and this project has no lock.
     */
    for (size_t i = 1; i < list->count; i++) {
        const NN20ClockMediaEntry key = list->entries[i];
        size_t j = i;
        while (j > 0u && compare_entries(&list->entries[j - 1], &key) > 0) {
            list->entries[j] = list->entries[j - 1];
            j--;
        }
        list->entries[j] = key;
    }
}

/* ---------------------------------------------------------- openers -- */

bool nn20clock_media_name_number(const char *name, uint32_t *out_number)
{
    if (name == NULL || name[0] < '0' || name[0] > '9') {
        return false;
    }

    uint32_t number = 0u;
    for (size_t i = 0; name[i] >= '0' && name[i] <= '9'; i++) {
        const uint32_t digit = (uint32_t)(name[i] - '0');
        if (number > (UINT32_MAX - digit) / 10u) {
            /* Saturate rather than wrap: a name with twelve digits on
             * the front is not a running order, and wrapping would sort
             * it in front of "1". */
            number = UINT32_MAX;
            break;
        }
        number = number * 10u + digit;
    }

    if (out_number != NULL) {
        *out_number = number;
    }
    return true;
}

/* The running order: the number first, then the name for two openers
 * that lead with the same one. Total, so nothing ties and nothing
 * plays twice. */
static int compare_openers(uint32_t a_number, const char *a_name,
                           uint32_t b_number, const char *b_name)
{
    if (a_number != b_number) {
        return (a_number < b_number) ? -1 : 1;
    }
    return compare_names(a_name, b_name);
}

const NN20ClockMediaEntry *nn20clock_media_list_next_opener(
        const NN20ClockMediaList *list, const char *after_path)
{
    if (list == NULL) {
        return NULL;
    }

    const char *after_name = NULL;
    uint32_t after_number = 0u;
    if (after_path != NULL && after_path[0] != '\0') {
        after_name = nn20clock_media_path_name(after_path);
        if (after_name == NULL ||
            !nn20clock_media_name_number(after_name, &after_number)) {
            /* The last clip was not an opener, so the run is behind us
             * and what is left is the draw. */
            return NULL;
        }
    }

    /* A linear scan for the smallest one still ahead. The list is 64
     * entries at most and this runs once per clip, so sorting it first
     * would buy nothing and would reorder a listing the caller may be
     * drawing on screen. */
    const NN20ClockMediaEntry *best = NULL;
    uint32_t best_number = 0u;
    for (size_t i = 0; i < list->count; i++) {
        const NN20ClockMediaEntry *const entry = &list->entries[i];
        uint32_t number = 0u;
        if (entry->type != NN20CLOCK_MEDIA_ENTRY_FILE ||
            entry->kind == NN20CLOCK_MEDIA_KIND_UNKNOWN ||
            !nn20clock_media_name_number(entry->name, &number)) {
            continue;
        }
        if (after_name != NULL &&
            compare_openers(number, entry->name, after_number,
                            after_name) <= 0) {
            continue;   /* played already, or is the clip itself */
        }
        if (best == NULL ||
            compare_openers(number, entry->name, best_number,
                            best->name) < 0) {
            best = entry;
            best_number = number;
        }
    }
    return best;
}

void nn20clock_media_list_counts(const NN20ClockMediaList *list,
                                 size_t *out_clips, size_t *out_folders,
                                 size_t *out_others)
{
    size_t clips = 0u;
    size_t folders = 0u;
    size_t others = 0u;

    if (list != NULL) {
        for (size_t i = 0; i < list->count; i++) {
            const NN20ClockMediaEntry *const entry = &list->entries[i];
            if (entry->type == NN20CLOCK_MEDIA_ENTRY_FOLDER) {
                folders++;
            } else if (entry->kind != NN20CLOCK_MEDIA_KIND_UNKNOWN) {
                clips++;
            } else {
                others++;
            }
        }
    }

    if (out_clips != NULL) {
        *out_clips = clips;
    }
    if (out_folders != NULL) {
        *out_folders = folders;
    }
    if (out_others != NULL) {
        *out_others = others;
    }
}
