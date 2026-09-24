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
 * Component tests for the SD card's folder operations.
 *
 * TARGET ONLY. Every other suite in this directory runs on the host in
 * milliseconds; this one cannot, because nn20clock_sd is SDMMC and
 * FATFS and there is nothing underneath it to fake. It is listed in
 * test/target only - see test/README.md.
 *
 * What it tests is the half of folders that only a real filesystem can
 * answer: that listing one folder does not show another's contents, that a
 * delete refuses a directory, that a folder must be empty before it goes,
 * and that a rename can move a file between folders but will not overwrite
 * anything. The path rules those calls are built on are pure logic and
 * are tested exhaustively on the host, in test_nn20clock_media.c.
 *
 * ------------------------------------------------------------------
 * It writes to the card
 * ------------------------------------------------------------------
 *
 * Everything happens inside one scratch folder with a name no clip would
 * have, created and removed by each case - the same rule
 * test/README.md gives for the storage suite's NVS namespace, for the
 * same reason: a suite must not touch the product's data. If a run is
 * interrupted, the leftover is one directory called NN20TEST and the
 * next run removes it.
 *
 * With no card in the slot every case reports that and passes. A board
 * without a card is not a failing board, and design 15 says so.
 *
 * ------------------------------------------------------------------
 * It mounts the card ONCE
 * ------------------------------------------------------------------
 *
 * Not once per case, which is what a fixture normally does here.
 *
 * The firmware sets CONFIG_FATFS_VOLUME_COUNT=2, and a mount that fails
 * partway does not give its volume slot back. Two cycles of
 * mount/unmount on this board therefore leave the card unmountable for
 * the rest of the run - the third attempt returns ESP_ERR_NO_MEM from
 * mount_prepare and every one after it does the same. That is a
 * limitation of the mount path rather than of folders, and the fix does
 * not belong in a test; what belongs here is not making it worse. So
 * the suite opens the card before its first case and closes it after
 * its last, and uses exactly one cycle.
 *
 * ------------------------------------------------------------------
 * Names on the card are 8.3
 * ------------------------------------------------------------------
 *
 * CONFIG_FATFS_LFN_NONE=y: long file names are off, so FAT accepts at
 * most eight characters plus a three-character extension, and hands
 * every name back in upper case. The names below are chosen to fit, and
 * find() compares case-insensitively, because that is what a real card
 * in this firmware actually returns - which is exactly the kind of
 * thing a host fake would have got wrong.
 */
#include "test_util.h"

#include <stdio.h>
#include <string.h>

#include "ff.h"
#include "nn20clock_sd.h"

/*
 * Not a name any clip would have, and not one the picker will show for
 * long: each case removes it. Eight characters, upper case - see the
 * note on 8.3 names above.
 */
#define SCRATCH "NN20TEST"

/* ---------------------------------------------------------- fixture -- */

/*
 * One card, one mount, for the whole suite - see the note above about
 * FATFS volume slots. Opened by suite_open() before the first case and
 * closed by suite_close() after the last, both called from TEST_MAIN.
 */
static NN20ClockSd *sd;
static bool mounted;

/* Remove everything the suite could have left, deepest first. Called
 * around every case, so an interrupted run does not make the next one
 * fail for a reason that has nothing to do with the code. */
static void scrub(void)
{
    static const char *const FOLDERS[] = {
        SCRATCH "/FROM/INNER", SCRATCH "/FROM", SCRATCH "/TO", SCRATCH
    };
    static const char *const FILES[] = {
        SCRATCH "/A.AVI",      SCRATCH "/B.WAV",      SCRATCH "/C.TXT",
        SCRATCH "/FROM/X.AVI", SCRATCH "/TO/X.AVI",   SCRATCH "/TO/Y.AVI",
        SCRATCH "/TO/RENAMED.AVI", SCRATCH "/FROM/INNER/DEEP.AVI"
    };

    if (!mounted) {
        return;
    }
    for (size_t i = 0; i < sizeof(FILES) / sizeof(FILES[0]); i++) {
        (void)nn20clock_sd_delete_file(sd, FILES[i]);
    }
    for (size_t i = 0; i < sizeof(FOLDERS) / sizeof(FOLDERS[0]); i++) {
        (void)nn20clock_sd_delete_folder(sd, FOLDERS[i]);
    }
}

static void suite_open(void)
{
    sd = nn20clock_sd_ctor();
    if (sd == NULL) {
        return;
    }
    mounted = (nn20clock_sd_mount(sd) == ESP_OK);
    scrub();
}

static void suite_close(void)
{
    if (sd != NULL) {
        scrub();
        nn20clock_sd_dtor(sd);
        sd = NULL;
        mounted = false;
    }
}

/*
 * True when there is a card to test against; false, having said so,
 * when there is not. Every case that touches the card starts with this
 * and returns rather than failing - design 15 is explicit that a board
 * with no card is a working board.
 */
static bool have_card(void)
{
    if (sd != NULL && mounted) {
        scrub();   /* whatever the previous case left */
        return true;
    }
    printf("        no SD card; skipped\n");
    return false;
}

/* Write a file through the VFS. The SD component has no create call -
 * uploads arrive through the FTP server's own fopen - so a test that
 * needs a file on the card makes one the same way. */
static bool make_file(const char *relative_path, size_t bytes)
{
    char path[NN20CLOCK_MEDIA_FULL_PATH_MAX];
    if (nn20clock_sd_path(relative_path, path, sizeof(path)) != ESP_OK) {
        return false;
    }

    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        return false;
    }
    for (size_t i = 0; i < bytes; i++) {
        (void)fputc('x', file);
    }
    (void)fclose(file);
    return true;
}

/*
 * Find an entry by name in a listing; NULL when it is not there.
 *
 * Case-insensitively, because that is what a real card in this firmware
 * returns: with long file names off, FAT stores and hands back 8.3
 * names in upper case whatever they were written as. A host fake would
 * have compared exactly and passed.
 */
static bool same_name(const char *a, const char *b)
{
    for (size_t i = 0;; i++) {
        char left = a[i];
        char right = b[i];
        if (left >= 'a' && left <= 'z') {
            left = (char)(left - 'a' + 'A');
        }
        if (right >= 'a' && right <= 'z') {
            right = (char)(right - 'a' + 'A');
        }
        if (left != right) {
            return false;
        }
        if (left == '\0') {
            return true;
        }
    }
}

static const NN20ClockMediaEntry *find(const NN20ClockMediaList *list,
                                       const char *name)
{
    for (size_t i = 0; i < list->count; i++) {
        if (same_name(list->entries[i].name, name)) {
            return &list->entries[i];
        }
    }
    return NULL;
}

/* ---------------------------------------------------------- listing -- */

TEST(a_folder_lists_its_own_files_and_its_child_folders)
{
    if (!have_card()) {
        return;
    }

    REQUIRE(nn20clock_sd_make_folder(sd, SCRATCH) == ESP_OK);
    REQUIRE(nn20clock_sd_make_folder(sd, SCRATCH "/FROM") == ESP_OK);
    REQUIRE(make_file(SCRATCH "/A.AVI", 16u));
    REQUIRE(make_file(SCRATCH "/C.TXT", 4u));
    REQUIRE(make_file(SCRATCH "/FROM/X.AVI", 8u));

    /* Over 30 KB: the heap, never a stack - which is the rule the
     * product code follows and the reason it is worth following here. */
    NN20ClockMediaList *list = calloc(1, sizeof(*list));
    REQUIRE(list != NULL);

    CHECK_EQ(ESP_OK, nn20clock_sd_list_media(sd, SCRATCH, list));
    CHECK_STR_EQ(SCRATCH, list->folder_path);

    const NN20ClockMediaEntry *clip = find(list, "A.AVI");
    REQUIRE(clip != NULL);
    CHECK_EQ(NN20CLOCK_MEDIA_ENTRY_FILE, clip->type);
    CHECK_EQ(NN20CLOCK_MEDIA_KIND_VIDEO, clip->kind);
    CHECK_EQ(16, clip->size_bytes);
    /* The path is what an alarm stores and what the app resolves. */
    CHECK_STR_EQ(SCRATCH "/A.AVI", clip->path);

    /* A file that is not media is still listed - "where did my file
     * go" is worse than a listing that mentions it. */
    const NN20ClockMediaEntry *other = find(list, "C.TXT");
    REQUIRE(other != NULL);
    CHECK_EQ(NN20CLOCK_MEDIA_KIND_UNKNOWN, other->kind);

    const NN20ClockMediaEntry *child = find(list, "FROM");
    REQUIRE(child != NULL);
    CHECK_EQ(NN20CLOCK_MEDIA_ENTRY_FOLDER, child->type);
    CHECK_EQ(0, child->size_bytes);
    CHECK_STR_EQ(SCRATCH "/FROM", child->path);

    /* And NOT what is inside that child: a listing is one folder, never a
     * tree. This is what <random> depends on to mean what it says. */
    CHECK(find(list, "X.AVI") == NULL);

    size_t clips = 0;
    size_t folders = 0;
    size_t others = 0;
    nn20clock_media_list_counts(list, &clips, &folders, &others);
    CHECK_EQ(1, clips);
    CHECK_EQ(1, folders);
    CHECK_EQ(1, others);

    /* The child folder from the other side: its own file and no siblings. */
    CHECK_EQ(ESP_OK,
             nn20clock_sd_list_media(sd, SCRATCH "/FROM", list));
    CHECK(find(list, "X.AVI") != NULL);
    CHECK(find(list, "A.AVI") == NULL);

    free(list);
    scrub();
}

TEST(listing_refuses_an_unsafe_folder_and_reports_a_missing_one)
{
    if (!have_card()) {
        return;
    }

    NN20ClockMediaList *list = calloc(1, sizeof(*list));
    REQUIRE(list != NULL);

    /* Refused before the filesystem is asked anything. */
    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_sd_list_media(sd, "../..", list));
    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_sd_list_media(sd, "/etc", list));

    /* Safe, but not there. */
    CHECK_EQ(ESP_ERR_NOT_FOUND,
             nn20clock_sd_list_media(sd, SCRATCH "/ABSENT", list));

    /* The root folder always lists. */
    CHECK_EQ(ESP_OK, nn20clock_sd_list_media(sd, "", list));
    CHECK_STR_EQ("", list->folder_path);

    free(list);
    scrub();
}

/* ------------------------------------------------ creating and removing -- */

TEST(a_folder_is_created_once_and_removed_only_when_empty)
{
    if (!have_card()) {
        return;
    }

    CHECK_EQ(ESP_OK, nn20clock_sd_make_folder(sd, SCRATCH));

    /* Twice is not an error worth inventing a code for, but it is an
     * error: something is already there. */
    CHECK_EQ(ESP_ERR_INVALID_STATE,
             nn20clock_sd_make_folder(sd, SCRATCH));

    /* The root folder is the card. It is not something to create or
     * remove. */
    CHECK_EQ(ESP_ERR_INVALID_ARG, nn20clock_sd_make_folder(sd, ""));
    CHECK_EQ(ESP_ERR_INVALID_ARG, nn20clock_sd_delete_folder(sd, ""));

    /* Nor is anything outside it. */
    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_sd_make_folder(sd, "../escape"));

    /* One directory, not a chain: a mistyped path is an error rather
     * than a tree. */
    CHECK_EQ(ESP_ERR_INVALID_STATE,
             nn20clock_sd_make_folder(sd, SCRATCH "/A/B"));

    REQUIRE(make_file(SCRATCH "/A.AVI", 4u));

    /* THE rule of delete_folder. Recursive delete is one mistyped path
     * away from an empty card. */
    CHECK_EQ(ESP_ERR_INVALID_STATE,
             nn20clock_sd_delete_folder(sd, SCRATCH));

    CHECK_EQ(ESP_OK, nn20clock_sd_delete_file(sd, SCRATCH "/A.AVI"));
    CHECK_EQ(ESP_OK, nn20clock_sd_delete_folder(sd, SCRATCH));
    CHECK_EQ(ESP_ERR_NOT_FOUND,
             nn20clock_sd_delete_folder(sd, SCRATCH));

    scrub();
}

TEST(deleting_a_file_refuses_a_folder_and_the_other_way_round)
{
    if (!have_card()) {
        return;
    }

    REQUIRE(nn20clock_sd_make_folder(sd, SCRATCH) == ESP_OK);
    REQUIRE(nn20clock_sd_make_folder(sd, SCRATCH "/TO") == ESP_OK);
    REQUIRE(make_file(SCRATCH "/A.AVI", 4u));

    /* "DELE morning" must not become an accident with a different
     * name. */
    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_sd_delete_file(sd, SCRATCH "/TO"));
    /* And RMD on a file is the same mistake from the other side. */
    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_sd_delete_folder(sd, SCRATCH "/A.AVI"));

    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_sd_delete_file(sd, "../escape"));
    CHECK_EQ(ESP_ERR_INVALID_ARG, nn20clock_sd_delete_file(sd, ""));
    CHECK_EQ(ESP_ERR_NOT_FOUND,
             nn20clock_sd_delete_file(sd, SCRATCH "/ABSENT.AVI"));

    CHECK_EQ(ESP_OK, nn20clock_sd_delete_file(sd, SCRATCH "/A.AVI"));

    scrub();
}

/* ----------------------------------------------------------- rename -- */

TEST(rename_moves_a_file_between_folders_and_never_overwrites)
{
    if (!have_card()) {
        return;
    }

    REQUIRE(nn20clock_sd_make_folder(sd, SCRATCH) == ESP_OK);
    REQUIRE(nn20clock_sd_make_folder(sd, SCRATCH "/FROM") == ESP_OK);
    REQUIRE(nn20clock_sd_make_folder(sd, SCRATCH "/TO") == ESP_OK);
    REQUIRE(make_file(SCRATCH "/FROM/X.AVI", 12u));

    /* The move FTP clients send as RNFR/RNTO, crossing folders. */
    CHECK_EQ(ESP_OK, nn20clock_sd_rename(sd, SCRATCH "/FROM/X.AVI",
                                         SCRATCH "/TO/X.AVI"));

    uint32_t size = 0;
    CHECK_EQ(ESP_OK, nn20clock_sd_file_size(sd, SCRATCH "/TO/X.AVI",
                                            &size));
    CHECK_EQ(12, size);
    CHECK_EQ(ESP_ERR_NOT_FOUND,
             nn20clock_sd_file_size(sd, SCRATCH "/FROM/X.AVI",
                                    &size));

    /* No overwrite: the card holds the only copy of a clip, and a
     * mistyped RNTO that silently replaces one is worse than an error. */
    REQUIRE(make_file(SCRATCH "/TO/Y.AVI", 3u));
    CHECK_EQ(ESP_ERR_INVALID_STATE,
             nn20clock_sd_rename(sd, SCRATCH "/TO/Y.AVI",
                                 SCRATCH "/TO/X.AVI"));
    /* And nothing moved. */
    CHECK_EQ(ESP_OK, nn20clock_sd_file_size(sd, SCRATCH "/TO/Y.AVI",
                                            &size));
    CHECK_EQ(3, size);

    /* A rename in place is the other half of what RNFR/RNTO is for. */
    CHECK_EQ(ESP_OK, nn20clock_sd_rename(sd, SCRATCH "/TO/Y.AVI",
                                         SCRATCH "/TO/RENAMED.AVI"));
    CHECK_EQ(ESP_OK, nn20clock_sd_delete_file(sd,
                                              SCRATCH "/TO/RENAMED.AVI"));

    scrub();
}

TEST(rename_refuses_what_would_leave_the_card)
{
    if (!have_card()) {
        return;
    }

    REQUIRE(nn20clock_sd_make_folder(sd, SCRATCH) == ESP_OK);
    REQUIRE(make_file(SCRATCH "/A.AVI", 4u));

    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_sd_rename(sd, SCRATCH "/A.AVI",
                                 "../escaped.avi"));
    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_sd_rename(sd, "../../nvs",
                                 SCRATCH "/A.AVI"));
    /* "" is the card itself at one end of a move. */
    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_sd_rename(sd, SCRATCH "/A.AVI", ""));
    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_sd_rename(sd, "", SCRATCH "/A.AVI"));

    CHECK_EQ(ESP_ERR_NOT_FOUND,
             nn20clock_sd_rename(sd, SCRATCH "/ABSENT.AVI",
                                 SCRATCH "/B.AVI"));

    /* A target whose parent folder does not exist: nothing is created on
     * the way. */
    CHECK_EQ(ESP_ERR_INVALID_STATE,
             nn20clock_sd_rename(sd, SCRATCH "/A.AVI",
                                 SCRATCH "/ABSENT/A.AVI"));

    CHECK_EQ(ESP_OK, nn20clock_sd_delete_file(sd, SCRATCH "/A.AVI"));

    scrub();
}

/* ------------------------------------------------------------- stat -- */

TEST(stat_tells_a_folder_from_a_file)
{
    if (!have_card()) {
        return;
    }

    REQUIRE(nn20clock_sd_make_folder(sd, SCRATCH) == ESP_OK);
    REQUIRE(make_file(SCRATCH "/A.AVI", 9u));

    bool is_dir = false;
    uint32_t size = 0;

    /* The root folder, without the filesystem having to be asked. */
    CHECK_EQ(ESP_OK, nn20clock_sd_stat(sd, "", &is_dir, &size));
    CHECK(is_dir);

    CHECK_EQ(ESP_OK, nn20clock_sd_stat(sd, SCRATCH, &is_dir, &size));
    CHECK(is_dir);
    CHECK_EQ(0, size);   /* a directory reports no size, not a fake one */

    CHECK_EQ(ESP_OK,
             nn20clock_sd_stat(sd, SCRATCH "/A.AVI", &is_dir, &size));
    CHECK(!is_dir);
    CHECK_EQ(9, size);

    CHECK_EQ(ESP_ERR_NOT_FOUND,
             nn20clock_sd_stat(sd, SCRATCH "/ABSENT", &is_dir,
                               &size));
    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_sd_stat(sd, "../..", &is_dir, &size));

    /* SIZE is defined for files: a number for a directory is a number a
     * client would use as a byte count. */
    CHECK_EQ(ESP_ERR_NOT_FOUND,
             nn20clock_sd_file_size(sd, SCRATCH, &size));

    CHECK_EQ(ESP_OK, nn20clock_sd_delete_file(sd, SCRATCH "/A.AVI"));

    scrub();
}

/* ----------------------------------------------------------- hidden -- */

/*
 * Set the FAT hidden bit on something, through FatFs.
 *
 * There is no POSIX way to do this - the VFS does not carry the
 * attribute in either direction - which is the same reason the listing
 * has to read it through FatFs. "0:" because that is the volume the
 * card mounts on in this application; the product code discovers it
 * rather than assuming it, and a test with one card can assume.
 */
static bool set_hidden(const char *relative_path)
{
    char path[NN20CLOCK_MEDIA_FULL_PATH_MAX];
    if (snprintf(path, sizeof(path), "0:/%s", relative_path)
        >= (int)sizeof(path)) {
        return false;
    }
    return f_chmod(path, AM_HID, AM_HID) == FR_OK;
}

TEST(hidden_and_system_entries_are_counted_but_not_listed)
{
    if (!have_card()) {
        return;
    }

    REQUIRE(nn20clock_sd_make_folder(sd, SCRATCH) == ESP_OK);
    REQUIRE(nn20clock_sd_make_folder(sd, SCRATCH "/TO") == ESP_OK);
    REQUIRE(make_file(SCRATCH "/A.AVI", 8u));
    REQUIRE(make_file(SCRATCH "/B.WAV", 8u));

    NN20ClockMediaList *list = calloc(1, sizeof(*list));
    REQUIRE(list != NULL);

    /* Everything visible to begin with, so the difference below is the
     * hidden bit and not something else. */
    REQUIRE(nn20clock_sd_list_media(sd, SCRATCH, list) == ESP_OK);
    CHECK_EQ(3, list->count);
    CHECK_EQ(0, list->skipped);
    CHECK(find(list, "A.AVI") != NULL);
    CHECK(find(list, "TO") != NULL);

    /*
     * Now hide one file and one folder.
     *
     * This is the assertion the feature exists for, and it cannot be
     * made any other way: neither readdir() nor stat() carries the
     * hidden bit - ESP-IDF's vfs_fat keeps AM_DIR and throws AM_HID
     * away - so a listing that still showed these would mean the FatFs
     * read had silently fallen back to stat(), which looks exactly like
     * "nothing was hidden".
     */
    REQUIRE(set_hidden(SCRATCH "/A.AVI"));
    REQUIRE(set_hidden(SCRATCH "/TO"));

    REQUIRE(nn20clock_sd_list_media(sd, SCRATCH, list) == ESP_OK);
    CHECK_EQ(1, list->count);
    CHECK(find(list, "A.AVI") == NULL);
    CHECK(find(list, "TO") == NULL);
    /* Counted, not silently dropped - the screen says "2 not listed"
     * rather than pretending the folder holds one thing. */
    CHECK_EQ(2, list->skipped);
    /* And what was not hidden is untouched. */
    CHECK(find(list, "B.WAV") != NULL);

    /*
     * Hidden is not the same as unreachable. Delete and rename take a
     * path and do not consult the bit, so a client that knows the name
     * can still clean one up - which is what makes this a decision
     * about what the device OFFERS rather than a permission.
     */
    uint32_t size = 0;
    CHECK_EQ(ESP_OK, nn20clock_sd_file_size(sd, SCRATCH "/A.AVI", &size));
    CHECK_EQ(8, size);
    CHECK_EQ(ESP_OK, nn20clock_sd_delete_file(sd, SCRATCH "/A.AVI"));
    CHECK_EQ(ESP_OK, nn20clock_sd_delete_folder(sd, SCRATCH "/TO"));

    free(list);
    scrub();
}

/* ------------------------------------------------------- no card yet -- */

TEST(every_call_refuses_an_unmounted_card)
{
    /*
     * Constructed but never mounted - which is also the state the
     * product is in when there is no card in the slot, and design 15
     * says that is recoverable rather than fatal.
     *
     * Its own instance, not the suite's: this case is about what an
     * unmounted card answers, and it never touches the filesystem, so
     * it costs no volume slot.
     */
    NN20ClockSd *fresh = nn20clock_sd_ctor();
    REQUIRE(fresh != NULL);

    NN20ClockMediaList *list = calloc(1, sizeof(*list));
    REQUIRE(list != NULL);

    CHECK_EQ(ESP_ERR_INVALID_STATE, nn20clock_sd_list_media(fresh, "", list));
    CHECK_EQ(ESP_ERR_INVALID_STATE,
             nn20clock_sd_delete_file(fresh, "a.avi"));
    CHECK_EQ(ESP_ERR_INVALID_STATE, nn20clock_sd_make_folder(fresh, "m"));
    CHECK_EQ(ESP_ERR_INVALID_STATE, nn20clock_sd_delete_folder(fresh, "m"));
    CHECK_EQ(ESP_ERR_INVALID_STATE, nn20clock_sd_rename(fresh, "a.avi",
                                                        "b.avi"));

    bool is_dir = false;
    uint32_t size = 0;
    CHECK_EQ(ESP_ERR_INVALID_STATE, nn20clock_sd_stat(fresh, "", &is_dir,
                                                      &size));
    CHECK_EQ(ESP_ERR_INVALID_STATE, nn20clock_sd_file_size(fresh, "a.avi",
                                                           &size));

    /* An unsafe path is refused the same way whether or not there is a
     * card: a difference in behaviour is a difference an attacker can
     * read. */
    CHECK_EQ(ESP_ERR_INVALID_ARG, nn20clock_sd_list_media(fresh, "../..", list));
    CHECK_EQ(ESP_ERR_INVALID_ARG, nn20clock_sd_delete_file(fresh, "../escape"));

    free(list);
    nn20clock_sd_dtor(fresh);
}

TEST_MAIN("nn20clock_sd")
{
    /* One mount for the whole suite - see the header comment. */
    suite_open();

    RUN(a_folder_lists_its_own_files_and_its_child_folders);
    RUN(listing_refuses_an_unsafe_folder_and_reports_a_missing_one);
    RUN(a_folder_is_created_once_and_removed_only_when_empty);
    RUN(deleting_a_file_refuses_a_folder_and_the_other_way_round);
    RUN(rename_moves_a_file_between_folders_and_never_overwrites);
    RUN(rename_refuses_what_would_leave_the_card);
    RUN(stat_tells_a_folder_from_a_file);
    RUN(hidden_and_system_entries_are_counted_but_not_listed);
    RUN(every_call_refuses_an_unmounted_card);

    suite_close();
}
