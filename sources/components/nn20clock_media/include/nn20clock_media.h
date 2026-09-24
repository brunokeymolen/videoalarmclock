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
 * nn20clock_media.h - what counts as media, where it lives, and the
 * list of it.
 *
 * Dual-mode and pure: no filesystem, no state. The SD component fills
 * these structures, the FTP server canonicalises client paths with
 * them, and the alarm record is validated against them - so the
 * decisions about names, paths and extensions live in one place that
 * can be tested in milliseconds.
 *
 * ------------------------------------------------------------------
 * Folders
 * ------------------------------------------------------------------
 *
 * A folder is a directory on the card, and the card's root is the root
 * folder. One word for one thing, everywhere - the UI, this header, the
 * SD layer and the FTP server all say "folder", and it is what a person
 * would call it looking at the card on their PC.
 *
 * A folder path is a relative POSIX path from the root of the card, with
 * no leading and no trailing slash:
 *
 *     ""                the root folder
 *     "morning"         a folder
 *     "weekend/kids"    a nested folder
 *
 * A file path is the same thing with a file name on the end:
 * "wake.avi", "morning/wake.avi". Nothing here ever holds an absolute
 * path: the mount point is added at the last moment, in exactly one
 * place, by nn20clock_media_path().
 */
#ifndef NN20CLOCK_MEDIA_H
#define NN20CLOCK_MEDIA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nn20clock_platform.h"

#ifdef __cplusplus
extern "C" {
#endif

/* FAT's own limit for a long file name, plus a terminator. One
 * component, not a path. */
#define NN20CLOCK_MEDIA_NAME_MAX 256

/*
 * The longest relative path this device will handle, terminator
 * included.
 *
 * FAT allows 255 bytes per component and no meaningful limit on the
 * depth, which is more filesystem than a bedside clock has any use for.
 * The cap is the size of NN20ClockAlarmConfig::media_path, so that
 * every path this device can browse to is a path an alarm can remember
 * - the two numbers must stay equal, and the alarm header says so from
 * its side.
 */
#define NN20CLOCK_MEDIA_PATH_MAX 256

/*
 * The longest full VFS path: a mount point, a separator and a relative
 * path. Anything asking for a path buffer to open should use this
 * rather than guessing - a buffer one byte short turns a playable file
 * into a silent alarm.
 */
#define NN20CLOCK_MEDIA_FULL_PATH_MAX (NN20CLOCK_MEDIA_PATH_MAX + 32)

/* Entries in one listing. A single folder much larger than this is
 * unusable from a 720x720 screen anyway, and folders are how a card
 * with more than this on it stays navigable. */
#define NN20CLOCK_MEDIA_MAX 64

typedef enum {
    NN20CLOCK_MEDIA_KIND_UNKNOWN,
    NN20CLOCK_MEDIA_KIND_VIDEO,
    NN20CLOCK_MEDIA_KIND_AUDIO
} NN20ClockMediaKind;

/* A listing holds both, and the UI draws them differently: a folder is
 * somewhere to go, a file is something to play. */
typedef enum {
    NN20CLOCK_MEDIA_ENTRY_FILE,
    NN20CLOCK_MEDIA_ENTRY_FOLDER
} NN20ClockMediaEntryType;

typedef struct {
    char name[NN20CLOCK_MEDIA_NAME_MAX];   /* basename, no directory */
    char path[NN20CLOCK_MEDIA_PATH_MAX];   /* relative to the card root */
    uint32_t size_bytes;                   /* files only; 0 for a folder */
    NN20ClockMediaKind kind;               /* files only */
    NN20ClockMediaEntryType type;
} NN20ClockMediaEntry;

/*
 * One folder's direct contents. Not a tree: listing "morning" gives
 * what is in "morning" and nothing from "morning/kids" - see
 * nn20clock_media_list_init().
 *
 * Over 30 KB. It belongs on the heap, never on a stack.
 */
typedef struct {
    char folder_path[NN20CLOCK_MEDIA_PATH_MAX];  /* "" is the root folder */
    NN20ClockMediaEntry entries[NN20CLOCK_MEDIA_MAX];
    size_t count;
    /* Entries present but not listed: names this device refuses, and
     * anything past NN20CLOCK_MEDIA_MAX. A listing can then say "and
     * three more" rather than silently hiding them. */
    size_t skipped;
} NN20ClockMediaList;

/*
 * What this file is, by extension.
 *
 * Extension alone, deliberately: opening every file on the card to
 * sniff its contents would make listing slow, and the playback path
 * rejects what it cannot decode anyway. Design 16 settled AVI with
 * MJPEG video and PCM audio as the proven format; .wav is here because
 * a fallback alarm sound is the obvious next thing to need.
 */
NN20ClockMediaKind nn20clock_media_kind(const char *name);

const char *nn20clock_media_kind_name(NN20ClockMediaKind kind);

/*
 * Whether a name is desktop housekeeping rather than something a person
 * put on the card.
 *
 * A short, explicit list: the trash cans and index folders Linux,
 * Windows and macOS leave behind when a card is plugged into them.
 * Every one of them is 8.3-mangled by the time this device sees it -
 * ".Trash-1000" arrives as "TRASH-~1" - so the leading dot that makes
 * them hidden on a desktop is already gone, and most of them do not
 * carry FAT's hidden bit either.
 *
 * Deliberately a list and not a pattern. The obvious pattern - anything
 * with "~1" in it - is wrong: that suffix is what FAT does to ANY long
 * name, so it would also hide a folder somebody created from their PC
 * as "Morning Films". A list misses the next desktop's junk folder; a
 * pattern hides the user's own work, and only one of those is
 * recoverable by the user noticing.
 *
 * Case-insensitive, because FAT returns names in whatever case it
 * stored them.
 */
bool nn20clock_media_name_is_housekeeping(const char *name);

/*
 * The number a name leads with, for the openers - see
 * nn20clock_media_list_next_opener().
 *
 * "1MYSTUFF.AVI" is 1, "15SUMMER.AVI" is 15, "SUMMER.AVI" is not a
 * number at all. Leading digits only: a number somewhere in the middle
 * of a name is part of the name, because "PART2.AVI" is what a clip is
 * called rather than a position in a running order.
 *
 * Leading zeros are worth nothing here, which is what lets someone pad
 * their names to sort tidily on a PC - "01", "02", "10" - without that
 * padding meaning anything to this device. Twelve digits and more
 * saturate at UINT32_MAX rather than wrapping; they then sort by name,
 * which is as much order as a name like that deserves.
 *
 * `out_number` may be NULL when only the yes or no is wanted.
 */
bool nn20clock_media_name_number(const char *name, uint32_t *out_number);

/* ------------------------------------------------------ path safety -- */

/*
 * Whether one name - a file or a folder, never a path - may be created
 * or served.
 *
 * Rejects an empty name, "." or "..", any name containing a path
 * separator, control characters, and anything too long. This is the
 * grain everything else is built from: a relative path is safe exactly
 * when every component of it passes this.
 */
bool nn20clock_media_name_is_safe_component(const char *name);

/*
 * Which of the two things a path is allowed to be.
 *
 * The only difference is the empty string: "" is the root folder, which
 * is a real place to list and to choose <random> from, and never a
 * file.
 */
typedef enum {
    NN20CLOCK_MEDIA_PATH_FILE,     /* "" is refused */
    NN20CLOCK_MEDIA_PATH_FOLDER    /* "" is the root folder */
} NN20ClockMediaPathKind;

/*
 * Whether a relative path may be used.
 *
 * Safe means: made of safe components separated by single forward
 * slashes, no leading slash, no trailing slash, no "." or ".." in it,
 * and short enough for NN20CLOCK_MEDIA_PATH_MAX.
 *
 * REFUSES rather than sanitises. Trying to clean up a hostile path is
 * how directory traversal bugs get written: "morning//../.." has no
 * correct repair, only a correct refusal. Anything that wants to turn
 * client text into a path calls nn20clock_media_path_resolve() first,
 * which is the one place ".." means anything at all.
 */
bool nn20clock_media_path_is_safe(const char *path,
                                  NN20ClockMediaPathKind kind);

/*
 * Full VFS path for a relative FILE path, e.g. "/sdcard/morning/wake.avi".
 * ESP_ERR_INVALID_ARG for an unsafe path, ESP_ERR_INVALID_SIZE when it
 * does not fit; `out` is left an empty string either way.
 */
esp_err_t nn20clock_media_path(const char *mount_point,
                               const char *relative_path,
                               char *out, size_t size);

/*
 * Full VFS path for a FOLDER path. The root folder is the mount point
 * itself, which is the one case nn20clock_media_path() cannot express.
 */
esp_err_t nn20clock_media_folder_path(const char *mount_point,
                                      const char *folder_path,
                                      char *out, size_t size);

/*
 * folder_path + "/" + name, or just name in the root folder. Both
 * halves are checked, so a joined path is a safe path.
 */
esp_err_t nn20clock_media_path_join(const char *folder_path, const char *name,
                                    char *out, size_t size);

/*
 * The folder a path lives in: "morning/wake.avi" -> "morning",
 * "wake.avi" -> "". This is how the alarm editor reopens its picker
 * where the stored clip is.
 */
esp_err_t nn20clock_media_path_parent(const char *path, char *out,
                                      size_t size);

/* The last component of a path. Points into `path`; never NULL for a
 * safe path. */
const char *nn20clock_media_path_name(const char *path);

/*
 * Turn what a client typed into a safe relative path, or refuse it.
 *
 * This is the only function in the project where "..", a leading slash
 * and a trailing slash mean anything, and it exists because FTP clients
 * send all three as a matter of course. `input` is resolved against
 * `base_folder` - the session's current directory - exactly as a shell
 * would, and the result is a canonical relative path with no "..", no
 * leading slash and no trailing slash left in it.
 *
 * ".." may walk towards the root and never past it: "/../etc" is not
 * repaired into "etc", it is refused. So is a backslash, a control
 * character, an empty component ("morning//wake.avi"), and a result too
 * long for NN20CLOCK_MEDIA_PATH_MAX.
 *
 * An empty `input` resolves to `base_folder` - "LIST" with no argument
 * means the current directory. The result can therefore be "", the root
 * folder, so callers that need a file check the result with
 * nn20clock_media_path_is_safe(..., NN20CLOCK_MEDIA_PATH_FILE).
 */
esp_err_t nn20clock_media_path_resolve(const char *base_folder,
                                       const char *input,
                                       char *out, size_t size);

/* ------------------------------------------------------------- list -- */

/*
 * Empty the list and set the folder it describes. Every entry added
 * afterwards gets its `path` built from this, which is what stops a
 * listing and the paths it hands out from disagreeing.
 *
 * ESP_ERR_INVALID_ARG for a folder path this device will not accept.
 */
esp_err_t nn20clock_media_list_init(NN20ClockMediaList *list,
                                    const char *folder_path);

/* ESP_ERR_NO_MEM when the list is full; the entry is not added.
 * ESP_ERR_INVALID_ARG for an unsafe name or a path that does not fit,
 * which the caller should count as skipped. */
esp_err_t nn20clock_media_list_add_file(NN20ClockMediaList *list,
                                        const char *name,
                                        uint32_t size_bytes);
esp_err_t nn20clock_media_list_add_folder(NN20ClockMediaList *list,
                                          const char *name);

/*
 * Folders first, then files, each group by name case-insensitively.
 *
 * Folders first because they are where the rest of the card is: a row
 * that takes you somewhere belongs above the rows that do not, and a
 * user scrolling for a clip should not have to pass through folders
 * interleaved with it.
 */
void nn20clock_media_list_sort(NN20ClockMediaList *list);

/*
 * The opener that follows `after_path`, or NULL when the run is over.
 *
 * An opener is a playable clip whose name starts with a number:
 * "1INTRO.AVI", "15SUMMER.AVI". They are how a folder says "these
 * first, in this order" to a feature whose whole point is not having an
 * order - <random> plays the openers in numeric order before it starts
 * drawing, so a folder can have a title card, or three parts of one
 * evening that only make sense in sequence, without giving up the
 * shuffle for everything else in it.
 *
 * Ordered by the number, then by name for two clips that lead with the
 * same one ("1A.AVI" before "1B.AVI"), so the order is total and a
 * folder never plays the same opener twice.
 *
 * `after_path` is the clip that just played, relative like everything
 * else here, or NULL to start the run. Only its NAME is read - the
 * caller's last clip may have been deleted from the card since, and a
 * run that stops dead because somebody tidied up over FTP would be a
 * worse answer than carrying on from where it says it was.
 *
 * NULL comes back for a folder with no openers left to play, and for
 * an `after_path` that is not an opener at all: once <random> has drawn
 * its first unnumbered clip, the run is behind it and drawing is all
 * that is left. Which makes this a pure function of the last clip and
 * the folder, with no "am I still in the run" flag anywhere - the last
 * clip's own name is that flag.
 *
 * The returned entry points into `list` and lives as long as it does.
 */
const NN20ClockMediaEntry *nn20clock_media_list_next_opener(
        const NN20ClockMediaList *list, const char *after_path);

/*
 * What a listing adds up to, for a status line: playable clips, child
 * folders, and files that are neither. Any out pointer may be NULL.
 */
void nn20clock_media_list_counts(const NN20ClockMediaList *list,
                                 size_t *out_clips, size_t *out_folders,
                                 size_t *out_others);

#ifdef __cplusplus
}
#endif

#endif /* NN20CLOCK_MEDIA_H */
