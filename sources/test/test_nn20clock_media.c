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
 * Component tests for the media rules (design 13, folders).
 *
 * The important half of this suite is path safety. These functions are
 * the only thing between an FTP client on the network and the rest of
 * the filesystem, so the traversal attempts are spelled out rather than
 * assumed - once for a single component, once for a whole relative
 * path, and once for the resolver, which is the only place ".." means
 * anything at all.
 */
#include "test_util.h"

#include <string.h>

#include "nn20clock_media.h"

/* ------------------------------------------------------------- kind -- */

TEST(the_proven_formats_are_recognised)
{
    /* Design 16 settled AVI with MJPEG video and PCM audio. */
    CHECK_EQ(NN20CLOCK_MEDIA_KIND_VIDEO,
             nn20clock_media_kind("steam720.avi"));
    CHECK_EQ(NN20CLOCK_MEDIA_KIND_AUDIO, nn20clock_media_kind("beep.wav"));

    /* Case does not matter: FAT hands names back in whatever case it
     * feels like. */
    CHECK_EQ(NN20CLOCK_MEDIA_KIND_VIDEO, nn20clock_media_kind("LOUD.AVI"));
    CHECK_EQ(NN20CLOCK_MEDIA_KIND_AUDIO, nn20clock_media_kind("Beep.Wav"));

    CHECK_EQ(NN20CLOCK_MEDIA_KIND_UNKNOWN, nn20clock_media_kind("notes.txt"));
    CHECK_EQ(NN20CLOCK_MEDIA_KIND_UNKNOWN, nn20clock_media_kind("movie.mp4"));
    CHECK_EQ(NN20CLOCK_MEDIA_KIND_UNKNOWN, nn20clock_media_kind(""));
    CHECK_EQ(NN20CLOCK_MEDIA_KIND_UNKNOWN, nn20clock_media_kind(NULL));

    /* An extension is a suffix, not a substring. */
    CHECK_EQ(NN20CLOCK_MEDIA_KIND_UNKNOWN,
             nn20clock_media_kind("avi.something"));
    CHECK_EQ(NN20CLOCK_MEDIA_KIND_UNKNOWN, nn20clock_media_kind("avi"));

    CHECK_STR_EQ("video",
                 nn20clock_media_kind_name(NN20CLOCK_MEDIA_KIND_VIDEO));
    CHECK_STR_EQ("audio",
                 nn20clock_media_kind_name(NN20CLOCK_MEDIA_KIND_AUDIO));
    CHECK_STR_EQ("other",
                 nn20clock_media_kind_name(NN20CLOCK_MEDIA_KIND_UNKNOWN));
}

/* ------------------------------------------------- component safety -- */

TEST(ordinary_names_are_accepted_as_components)
{
    CHECK(nn20clock_media_name_is_safe_component("steam720.avi"));
    CHECK(nn20clock_media_name_is_safe_component("My Alarm 2.wav"));
    CHECK(nn20clock_media_name_is_safe_component("a"));

    /* A folder is a name too, and has no extension. */
    CHECK(nn20clock_media_name_is_safe_component("morning"));
    CHECK(nn20clock_media_name_is_safe_component("Weekend Films"));

    /* Not media, but a safe name: the listing counts it rather than
     * refusing it, and the FTP server will serve it. */
    CHECK(nn20clock_media_name_is_safe_component("readme.txt"));
}

TEST(traversal_attempts_are_refused_as_components)
{
    CHECK(!nn20clock_media_name_is_safe_component(".."));
    CHECK(!nn20clock_media_name_is_safe_component("."));
    CHECK(!nn20clock_media_name_is_safe_component("../secrets"));
    CHECK(!nn20clock_media_name_is_safe_component("../../nvs"));

    /* A path is not a name, however harmless it looks. */
    CHECK(!nn20clock_media_name_is_safe_component("sub/dir.avi"));
    CHECK(!nn20clock_media_name_is_safe_component("/absolute.avi"));
    CHECK(!nn20clock_media_name_is_safe_component("dir/../escape.avi"));

    /* Backslash too: a Windows client may send one, and a filesystem
     * that ignores it is not a reason to let it through. */
    CHECK(!nn20clock_media_name_is_safe_component("sub\\dir.avi"));
    CHECK(!nn20clock_media_name_is_safe_component("..\\escape.avi"));

    CHECK(!nn20clock_media_name_is_safe_component(""));
    CHECK(!nn20clock_media_name_is_safe_component(NULL));

    /* Control characters make a mess of a line-based FTP listing. */
    CHECK(!nn20clock_media_name_is_safe_component("evil\nname.avi"));
    CHECK(!nn20clock_media_name_is_safe_component("tab\there.avi"));

    char oversized[NN20CLOCK_MEDIA_NAME_MAX + 8];
    memset(oversized, 'a', sizeof(oversized) - 1);
    oversized[sizeof(oversized) - 1] = '\0';
    CHECK(!nn20clock_media_name_is_safe_component(oversized));
}

/* ------------------------------------------------- desktop leftovers -- */

TEST(desktop_housekeeping_names_are_recognised)
{
    /*
     * As they actually arrive: 8.3-mangled, because the card is FAT
     * with long names off. ".Trash-1000" is "TRASH-~1" by the time this
     * device sees it, which is also why its leading dot cannot be used
     * to recognise it.
     */
    CHECK(nn20clock_media_name_is_housekeeping("TRASH-~1"));
    CHECK(nn20clock_media_name_is_housekeeping("SYSTEM~1"));
    CHECK(nn20clock_media_name_is_housekeeping("RECYCL~1"));
    CHECK(nn20clock_media_name_is_housekeeping("FSEVEN~1"));
    CHECK(nn20clock_media_name_is_housekeeping("SPOTLI~1"));

    /* Case-insensitively: FAT returns names however it stored them. */
    CHECK(nn20clock_media_name_is_housekeeping("trash-~1"));
    CHECK(nn20clock_media_name_is_housekeeping(".ds_store"));
}

TEST(a_users_own_names_are_never_housekeeping)
{
    CHECK(!nn20clock_media_name_is_housekeeping("MORNING"));
    CHECK(!nn20clock_media_name_is_housekeeping("WAKE.AVI"));
    CHECK(!nn20clock_media_name_is_housekeeping(""));
    CHECK(!nn20clock_media_name_is_housekeeping(NULL));

    /*
     * THE case this list exists to not break.
     *
     * "~1" is what FAT does to any long name, so a folder somebody
     * created from their PC as "Morning Films" arrives as "MORNIN~1" -
     * indistinguishable by shape from "TRASH-~1". That is why this is a
     * list of specific names and not a pattern: a pattern would hide
     * the user's own work, and they would have no way to know why.
     */
    CHECK(!nn20clock_media_name_is_housekeeping("MORNIN~1"));
    CHECK(!nn20clock_media_name_is_housekeeping("HOLIDA~2"));

    /* A prefix of a known name is not that name. */
    CHECK(!nn20clock_media_name_is_housekeeping("TRASH"));
    CHECK(!nn20clock_media_name_is_housekeeping("TRASH-~11"));
}

/* ------------------------------------------------------ path safety -- */

TEST(relative_paths_are_accepted)
{
    CHECK(nn20clock_media_path_is_safe("wake.avi",
                                       NN20CLOCK_MEDIA_PATH_FILE));
    CHECK(nn20clock_media_path_is_safe("morning/wake.avi",
                                       NN20CLOCK_MEDIA_PATH_FILE));
    CHECK(nn20clock_media_path_is_safe("weekend/kids/song.avi",
                                       NN20CLOCK_MEDIA_PATH_FILE));

    CHECK(nn20clock_media_path_is_safe("morning",
                                       NN20CLOCK_MEDIA_PATH_FOLDER));
    CHECK(nn20clock_media_path_is_safe("weekend/kids",
                                       NN20CLOCK_MEDIA_PATH_FOLDER));
}

TEST(the_empty_path_is_the_root_folder_and_never_a_file)
{
    /* The one difference between the two kinds, and the whole reason
     * there are two. */
    CHECK(nn20clock_media_path_is_safe("", NN20CLOCK_MEDIA_PATH_FOLDER));
    CHECK(!nn20clock_media_path_is_safe("", NN20CLOCK_MEDIA_PATH_FILE));

    CHECK(!nn20clock_media_path_is_safe(NULL, NN20CLOCK_MEDIA_PATH_FOLDER));
    CHECK(!nn20clock_media_path_is_safe(NULL, NN20CLOCK_MEDIA_PATH_FILE));
}

TEST(unsafe_relative_paths_are_refused)
{
    /* Absolute is somebody else's namespace. */
    CHECK(!nn20clock_media_path_is_safe("/wake.avi",
                                        NN20CLOCK_MEDIA_PATH_FILE));
    CHECK(!nn20clock_media_path_is_safe("/", NN20CLOCK_MEDIA_PATH_FOLDER));

    /* A trailing slash is a canonicalisation this function does not do:
     * it refuses rather than trims. */
    CHECK(!nn20clock_media_path_is_safe("morning/",
                                        NN20CLOCK_MEDIA_PATH_FOLDER));

    /* An empty component. */
    CHECK(!nn20clock_media_path_is_safe("morning//wake.avi",
                                        NN20CLOCK_MEDIA_PATH_FILE));

    /* "." and ".." are never components of a stored path, at any
     * depth - resolving them is somebody else's job. */
    CHECK(!nn20clock_media_path_is_safe("..", NN20CLOCK_MEDIA_PATH_FOLDER));
    CHECK(!nn20clock_media_path_is_safe(".", NN20CLOCK_MEDIA_PATH_FOLDER));
    CHECK(!nn20clock_media_path_is_safe("morning/../..",
                                        NN20CLOCK_MEDIA_PATH_FOLDER));
    CHECK(!nn20clock_media_path_is_safe("morning/./wake.avi",
                                        NN20CLOCK_MEDIA_PATH_FILE));
    CHECK(!nn20clock_media_path_is_safe("../../nvs",
                                        NN20CLOCK_MEDIA_PATH_FILE));

    CHECK(!nn20clock_media_path_is_safe("morning\\wake.avi",
                                        NN20CLOCK_MEDIA_PATH_FILE));
    CHECK(!nn20clock_media_path_is_safe("morning/wake\n.avi",
                                        NN20CLOCK_MEDIA_PATH_FILE));

    /* Longer than this device carries, however safe each component is. */
    char oversized[NN20CLOCK_MEDIA_PATH_MAX + 8];
    memset(oversized, 'a', sizeof(oversized) - 1);
    oversized[sizeof(oversized) - 1] = '\0';
    CHECK(!nn20clock_media_path_is_safe(oversized,
                                        NN20CLOCK_MEDIA_PATH_FILE));
}

/* ------------------------------------------------------- full paths -- */

TEST(a_path_is_the_mount_point_plus_the_relative_path)
{
    char path[NN20CLOCK_MEDIA_FULL_PATH_MAX];

    CHECK_EQ(ESP_OK, nn20clock_media_path("/sdcard", "steam720.avi", path,
                                          sizeof(path)));
    CHECK_STR_EQ("/sdcard/steam720.avi", path);

    CHECK_EQ(ESP_OK, nn20clock_media_path("/sdcard", "morning/wake.avi", path,
                                          sizeof(path)));
    CHECK_STR_EQ("/sdcard/morning/wake.avi", path);
}

TEST(the_root_folder_resolves_to_the_mount_point_itself)
{
    char path[NN20CLOCK_MEDIA_FULL_PATH_MAX];

    /* The one case a file path cannot express, and the reason there is
     * a second function. */
    CHECK_EQ(ESP_OK, nn20clock_media_folder_path("/sdcard", "", path,
                                              sizeof(path)));
    CHECK_STR_EQ("/sdcard", path);

    CHECK_EQ(ESP_OK,
             nn20clock_media_folder_path("/sdcard", "weekend/kids", path,
                                         sizeof(path)));
    CHECK_STR_EQ("/sdcard/weekend/kids", path);
}

TEST(an_unsafe_path_produces_no_full_path)
{
    char path[NN20CLOCK_MEDIA_FULL_PATH_MAX];

    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_media_path("/sdcard", "../escape", path,
                                  sizeof(path)));
    CHECK_STR_EQ("", path);   /* and nothing half-built left behind */

    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_media_path("/sdcard", "", path, sizeof(path)));
    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_media_path("/sdcard", NULL, path, sizeof(path)));
    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_media_path(NULL, "a.avi", path, sizeof(path)));
    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_media_folder_path("/sdcard", "morning/../..", path,
                                      sizeof(path)));
}

TEST(a_path_that_does_not_fit_is_refused)
{
    char tiny[8];
    CHECK_EQ(ESP_ERR_INVALID_SIZE,
             nn20clock_media_path("/sdcard", "steam720.avi", tiny,
                                  sizeof(tiny)));
    CHECK_STR_EQ("", tiny);
}

/* ------------------------------------------------ joining and parts -- */

TEST(joining_builds_a_relative_path)
{
    char path[NN20CLOCK_MEDIA_PATH_MAX];

    CHECK_EQ(ESP_OK, nn20clock_media_path_join("", "wake.avi", path,
                                               sizeof(path)));
    CHECK_STR_EQ("wake.avi", path);

    CHECK_EQ(ESP_OK, nn20clock_media_path_join("morning", "wake.avi", path,
                                               sizeof(path)));
    CHECK_STR_EQ("morning/wake.avi", path);

    CHECK_EQ(ESP_OK, nn20clock_media_path_join("weekend/kids", "song.avi",
                                               path, sizeof(path)));
    CHECK_STR_EQ("weekend/kids/song.avi", path);

    /* Both halves are checked, so a joined path is a safe path. */
    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_media_path_join("morning", "..", path, sizeof(path)));
    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_media_path_join("morning", "a/b.avi", path,
                                       sizeof(path)));
    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_media_path_join("../escape", "a.avi", path,
                                       sizeof(path)));
}

TEST(a_join_too_long_for_the_device_is_refused)
{
    /* Not just too long for the buffer: too long to store anywhere,
     * which is what the device cap means. */
    char deep[NN20CLOCK_MEDIA_PATH_MAX];
    memset(deep, 'm', sizeof(deep) - 1);
    deep[sizeof(deep) - 1] = '\0';
    REQUIRE(nn20clock_media_path_is_safe(deep, NN20CLOCK_MEDIA_PATH_FOLDER));

    char path[NN20CLOCK_MEDIA_PATH_MAX * 2];
    CHECK_EQ(ESP_ERR_INVALID_SIZE,
             nn20clock_media_path_join(deep, "wake.avi", path,
                                       sizeof(path)));
    CHECK_STR_EQ("", path);
}

TEST(a_path_knows_its_folder_and_its_name)
{
    char parent[NN20CLOCK_MEDIA_PATH_MAX];

    CHECK_EQ(ESP_OK, nn20clock_media_path_parent("wake.avi", parent,
                                                 sizeof(parent)));
    CHECK_STR_EQ("", parent);   /* the root folder */

    CHECK_EQ(ESP_OK, nn20clock_media_path_parent("morning/wake.avi", parent,
                                                 sizeof(parent)));
    CHECK_STR_EQ("morning", parent);

    CHECK_EQ(ESP_OK,
             nn20clock_media_path_parent("weekend/kids/song.avi", parent,
                                         sizeof(parent)));
    CHECK_STR_EQ("weekend/kids", parent);

    /* The root folder's parent is the root folder: there is nowhere above. */
    CHECK_EQ(ESP_OK, nn20clock_media_path_parent("", parent,
                                                 sizeof(parent)));
    CHECK_STR_EQ("", parent);

    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_media_path_parent("../escape", parent,
                                         sizeof(parent)));

    CHECK_STR_EQ("wake.avi", nn20clock_media_path_name("wake.avi"));
    CHECK_STR_EQ("wake.avi", nn20clock_media_path_name("morning/wake.avi"));
    CHECK_STR_EQ("song.avi",
                 nn20clock_media_path_name("weekend/kids/song.avi"));
    CHECK_STR_EQ("", nn20clock_media_path_name(""));
    CHECK_STR_EQ("", nn20clock_media_path_name(NULL));
}

/* -------------------------------------------------- path resolution -- */

TEST(a_relative_path_resolves_against_the_current_folder)
{
    char out[NN20CLOCK_MEDIA_PATH_MAX];

    CHECK_EQ(ESP_OK, nn20clock_media_path_resolve("", "wake.avi", out,
                                                  sizeof(out)));
    CHECK_STR_EQ("wake.avi", out);

    CHECK_EQ(ESP_OK, nn20clock_media_path_resolve("morning", "wake.avi", out,
                                                  sizeof(out)));
    CHECK_STR_EQ("morning/wake.avi", out);

    CHECK_EQ(ESP_OK, nn20clock_media_path_resolve("morning", "kids/x.avi",
                                                  out, sizeof(out)));
    CHECK_STR_EQ("morning/kids/x.avi", out);

    /* An empty argument is "where I already am" - LIST with no path. */
    CHECK_EQ(ESP_OK, nn20clock_media_path_resolve("morning", "", out,
                                                  sizeof(out)));
    CHECK_STR_EQ("morning", out);

    CHECK_EQ(ESP_OK, nn20clock_media_path_resolve("", "", out, sizeof(out)));
    CHECK_STR_EQ("", out);
}

TEST(an_absolute_path_resolves_from_the_root)
{
    char out[NN20CLOCK_MEDIA_PATH_MAX];

    CHECK_EQ(ESP_OK, nn20clock_media_path_resolve("weekend/kids",
                                                  "/morning/wake.avi", out,
                                                  sizeof(out)));
    CHECK_STR_EQ("morning/wake.avi", out);

    /* "/" is the root folder, which is where a client lands with CWD /. */
    CHECK_EQ(ESP_OK, nn20clock_media_path_resolve("morning", "/", out,
                                                  sizeof(out)));
    CHECK_STR_EQ("", out);
}

TEST(dot_and_dot_dot_are_navigation_and_are_canonicalised_away)
{
    char out[NN20CLOCK_MEDIA_PATH_MAX];

    CHECK_EQ(ESP_OK, nn20clock_media_path_resolve("morning", ".", out,
                                                  sizeof(out)));
    CHECK_STR_EQ("morning", out);

    CHECK_EQ(ESP_OK, nn20clock_media_path_resolve("weekend/kids", "..", out,
                                                  sizeof(out)));
    CHECK_STR_EQ("weekend", out);

    CHECK_EQ(ESP_OK, nn20clock_media_path_resolve("weekend/kids", "../..",
                                                  out, sizeof(out)));
    CHECK_STR_EQ("", out);

    /* Down and back up again, which is what a client browsing does. */
    CHECK_EQ(ESP_OK, nn20clock_media_path_resolve("", "morning/../weekend",
                                                  out, sizeof(out)));
    CHECK_STR_EQ("weekend", out);

    /* Doubled and trailing slashes are noise, not components. */
    CHECK_EQ(ESP_OK, nn20clock_media_path_resolve("", "morning//kids/", out,
                                                  sizeof(out)));
    CHECK_STR_EQ("morning/kids", out);

    /* Whatever comes out has none of it left in. */
    CHECK(nn20clock_media_path_is_safe(out, NN20CLOCK_MEDIA_PATH_FOLDER));
}

TEST(resolving_cannot_climb_above_the_root)
{
    char out[NN20CLOCK_MEDIA_PATH_MAX];

    /* Refused, not clamped to the root: a path that tries to leave the
     * card is an answer, not an input to repair. */
    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_media_path_resolve("", "..", out, sizeof(out)));
    CHECK_STR_EQ("", out);

    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_media_path_resolve("", "/../etc/passwd", out,
                                          sizeof(out)));
    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_media_path_resolve("morning", "../../nvs", out,
                                          sizeof(out)));
    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_media_path_resolve("weekend/kids", "../../../x", out,
                                          sizeof(out)));

    /* Down one and up two is still up one. */
    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_media_path_resolve("", "morning/../..", out,
                                          sizeof(out)));
}

TEST(resolving_refuses_what_no_component_may_contain)
{
    char out[NN20CLOCK_MEDIA_PATH_MAX];

    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_media_path_resolve("", "morning\\wake.avi", out,
                                          sizeof(out)));
    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_media_path_resolve("", "wake\n.avi", out,
                                          sizeof(out)));

    /* A result too long for the device, built out of legal pieces. */
    char deep[NN20CLOCK_MEDIA_PATH_MAX];
    memset(deep, 'm', sizeof(deep) - 1);
    deep[sizeof(deep) - 1] = '\0';
    char big[NN20CLOCK_MEDIA_PATH_MAX * 2];
    CHECK_EQ(ESP_ERR_INVALID_SIZE,
             nn20clock_media_path_resolve(deep, "wake.avi", big,
                                          sizeof(big)));

    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_media_path_resolve("", NULL, out, sizeof(out)));
}

/* ------------------------------------------------------------- list -- */

/*
 * One list for the whole suite, at file scope.
 *
 * It is over 30 KB, and this suite also runs on the target, where the
 * test binary links every suite's fixtures at once and runs out of
 * internal RAM before it runs out of anything else. Six of these as
 * locals is 200 KB of .bss and a link that fails with pages of
 * "discards section". Every case starts by initialising it, which
 * clears it, so sharing costs nothing.
 */
static NN20ClockMediaList list;

TEST(a_listing_knows_which_folder_it_describes)
{
    CHECK_EQ(ESP_OK, nn20clock_media_list_init(&list, "morning"));
    CHECK_STR_EQ("morning", list.folder_path);
    CHECK_EQ(0, list.count);
    CHECK_EQ(0, list.skipped);

    CHECK_EQ(ESP_OK, nn20clock_media_list_init(&list, ""));
    CHECK_STR_EQ("", list.folder_path);

    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_media_list_init(&list, "../escape"));
    CHECK_EQ(ESP_ERR_INVALID_ARG, nn20clock_media_list_init(NULL, ""));
}

TEST(entries_carry_their_kind_and_their_path)
{
    REQUIRE(nn20clock_media_list_init(&list, "morning") == ESP_OK);

    CHECK_EQ(ESP_OK,
             nn20clock_media_list_add_file(&list, "steam720.avi", 1234u));
    CHECK_EQ(ESP_OK, nn20clock_media_list_add_file(&list, "beep.wav", 99u));
    CHECK_EQ(ESP_OK, nn20clock_media_list_add_file(&list, "notes.txt", 7u));
    CHECK_EQ(ESP_OK, nn20clock_media_list_add_folder(&list, "kids"));

    CHECK_EQ(4, list.count);

    /* The path is built from the list's folder, which is what stops a
     * listing and the paths it hands out from disagreeing. */
    CHECK_STR_EQ("steam720.avi", list.entries[0].name);
    CHECK_STR_EQ("morning/steam720.avi", list.entries[0].path);
    CHECK_EQ(NN20CLOCK_MEDIA_ENTRY_FILE, list.entries[0].type);
    CHECK_EQ(NN20CLOCK_MEDIA_KIND_VIDEO, list.entries[0].kind);
    CHECK_EQ(1234, list.entries[0].size_bytes);

    CHECK_EQ(NN20CLOCK_MEDIA_KIND_AUDIO, list.entries[1].kind);
    CHECK_EQ(NN20CLOCK_MEDIA_KIND_UNKNOWN, list.entries[2].kind);

    CHECK_STR_EQ("kids", list.entries[3].name);
    CHECK_STR_EQ("morning/kids", list.entries[3].path);
    CHECK_EQ(NN20CLOCK_MEDIA_ENTRY_FOLDER, list.entries[3].type);
    CHECK_EQ(0, list.entries[3].size_bytes);

    size_t clips = 0;
    size_t folders = 0;
    size_t others = 0;
    nn20clock_media_list_counts(&list, &clips, &folders, &others);
    CHECK_EQ(2, clips);
    CHECK_EQ(1, folders);
    CHECK_EQ(1, others);
}

TEST(entries_in_the_root_have_bare_paths)
{
    REQUIRE(nn20clock_media_list_init(&list, "") == ESP_OK);

    REQUIRE(nn20clock_media_list_add_file(&list, "wake.avi", 1u) == ESP_OK);
    CHECK_STR_EQ("wake.avi", list.entries[0].name);
    CHECK_STR_EQ("wake.avi", list.entries[0].path);
}

TEST(unsafe_entries_are_refused_rather_than_added)
{
    REQUIRE(nn20clock_media_list_init(&list, "") == ESP_OK);

    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_media_list_add_file(&list, "../escape", 1u));
    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_media_list_add_file(&list, "a/b.avi", 1u));
    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_media_list_add_folder(&list, ".."));
    CHECK_EQ(0, list.count);
}

TEST(the_list_fills_up_and_says_so)
{
    REQUIRE(nn20clock_media_list_init(&list, "") == ESP_OK);

    for (size_t i = 0; i < NN20CLOCK_MEDIA_MAX; i++) {
        char name[32];
        snprintf(name, sizeof(name), "clip%02u.avi", (unsigned)i);
        REQUIRE(nn20clock_media_list_add_file(&list, name, 1u) == ESP_OK);
    }
    CHECK_EQ(NN20CLOCK_MEDIA_MAX, list.count);

    /* ESP_ERR_NO_MEM specifically: the SD layer tells "full" apart from
     * "refused" to decide whether to stop reading the directory. */
    CHECK_EQ(ESP_ERR_NO_MEM,
             nn20clock_media_list_add_file(&list, "one-too-many.avi", 1u));
    CHECK_EQ(ESP_ERR_NO_MEM, nn20clock_media_list_add_folder(&list, "late"));
    CHECK_EQ(NN20CLOCK_MEDIA_MAX, list.count);
}

TEST(the_list_puts_folders_first_and_sorts_each_group)
{
    REQUIRE(nn20clock_media_list_init(&list, "") == ESP_OK);

    REQUIRE(nn20clock_media_list_add_file(&list, "zebra.avi", 1u) == ESP_OK);
    REQUIRE(nn20clock_media_list_add_folder(&list, "zoo") == ESP_OK);
    REQUIRE(nn20clock_media_list_add_file(&list, "Apple.avi", 2u) == ESP_OK);
    REQUIRE(nn20clock_media_list_add_folder(&list, "Attic") == ESP_OK);
    REQUIRE(nn20clock_media_list_add_file(&list, "monkey.wav", 3u) == ESP_OK);
    REQUIRE(nn20clock_media_list_add_file(&list, "banana.avi", 4u) == ESP_OK);

    nn20clock_media_list_sort(&list);

    /* Folders first: a row that takes you somewhere belongs above the rows
     * that do not. */
    CHECK_EQ(NN20CLOCK_MEDIA_ENTRY_FOLDER, list.entries[0].type);
    CHECK_STR_EQ("Attic", list.entries[0].name);
    CHECK_EQ(NN20CLOCK_MEDIA_ENTRY_FOLDER, list.entries[1].type);
    CHECK_STR_EQ("zoo", list.entries[1].name);

    /* Then files, case-insensitively, so "Alarm.avi" and "alarm.avi"
     * sort together rather than in ASCII order. */
    CHECK_STR_EQ("Apple.avi", list.entries[2].name);
    CHECK_STR_EQ("banana.avi", list.entries[3].name);
    CHECK_STR_EQ("monkey.wav", list.entries[4].name);
    CHECK_STR_EQ("zebra.avi", list.entries[5].name);

    /* Sorting moves whole entries, not just names. */
    CHECK_EQ(2, list.entries[2].size_bytes);
    CHECK_EQ(4, list.entries[3].size_bytes);
    CHECK_STR_EQ("banana.avi", list.entries[3].path);

    nn20clock_media_list_sort(NULL);   /* must not crash */

    nn20clock_media_list_counts(NULL, NULL, NULL, NULL);
}

/* -------------------------------------------------------- openers -- */

TEST(a_name_that_starts_with_a_number_is_an_opener)
{
    uint32_t number = 99u;

    CHECK(nn20clock_media_name_number("1MYSTUFF.AVI", &number));
    CHECK_EQ(1, number);
    CHECK(nn20clock_media_name_number("15SUMMER.AVI", &number));
    CHECK_EQ(15, number);

    /* A number is all a name needs to be; it does not need anything
     * after it. */
    CHECK(nn20clock_media_name_number("7.avi", &number));
    CHECK_EQ(7, number);

    /* Padding is for sorting tidily on a PC and means nothing here. */
    CHECK(nn20clock_media_name_number("007bond.avi", &number));
    CHECK_EQ(7, number);

    /* Leading digits only: a number in the middle of a name is part of
     * the name. */
    CHECK(!nn20clock_media_name_number("PART2.AVI", NULL));
    CHECK(!nn20clock_media_name_number("SUMMER.AVI", NULL));
    CHECK(!nn20clock_media_name_number(" 1.avi", NULL));
    CHECK(!nn20clock_media_name_number("", NULL));
    CHECK(!nn20clock_media_name_number(NULL, NULL));

    /* More digits than a number can hold saturates rather than
     * wrapping, which would sort it in front of "1". */
    CHECK(nn20clock_media_name_number("99999999999999clip.avi", &number));
    CHECK_EQ(UINT32_MAX, number);
}

TEST(the_openers_play_in_their_numbers_order_before_the_draw)
{
    REQUIRE(nn20clock_media_list_init(&list, "morning") == ESP_OK);
    REQUIRE(nn20clock_media_list_add_file(&list, "SUMMER.AVI", 1u) == ESP_OK);
    REQUIRE(nn20clock_media_list_add_file(&list, "15LAST.AVI", 1u) == ESP_OK);
    REQUIRE(nn20clock_media_list_add_file(&list, "1FIRST.AVI", 1u) == ESP_OK);
    REQUIRE(nn20clock_media_list_add_folder(&list, "2KIDS") == ESP_OK);
    REQUIRE(nn20clock_media_list_add_file(&list, "3NOTES.TXT", 1u) == ESP_OK);
    REQUIRE(nn20clock_media_list_add_file(&list, "9MID.AVI", 1u) == ESP_OK);

    /* Nothing played yet: the first opener, by number and not by the
     * order the card handed the folder over in. */
    const NN20ClockMediaEntry *entry =
        nn20clock_media_list_next_opener(&list, NULL);
    REQUIRE(entry != NULL);
    CHECK_STR_EQ("morning/1FIRST.AVI", entry->path);

    /* 9 before 15: the number is a number, not the first character of
     * one. This is the whole reason the run is not just the sort. */
    entry = nn20clock_media_list_next_opener(&list, "morning/1FIRST.AVI");
    REQUIRE(entry != NULL);
    CHECK_STR_EQ("morning/9MID.AVI", entry->path);

    entry = nn20clock_media_list_next_opener(&list, "morning/9MID.AVI");
    REQUIRE(entry != NULL);
    CHECK_STR_EQ("morning/15LAST.AVI", entry->path);

    /* A numbered folder is somewhere to go and a numbered text file is
     * not playable, so neither is ever an opener - the run ends at the
     * last numbered CLIP. */
    CHECK(nn20clock_media_list_next_opener(&list, "morning/15LAST.AVI") ==
          NULL);

    /* And once an unnumbered clip has played, the run is behind us for
     * good: the openers are an opening. */
    CHECK(nn20clock_media_list_next_opener(&list, "morning/SUMMER.AVI") ==
          NULL);

    CHECK(nn20clock_media_list_next_opener(NULL, NULL) == NULL);
}

TEST(openers_that_lead_with_the_same_number_go_by_name)
{
    REQUIRE(nn20clock_media_list_init(&list, "") == ESP_OK);
    REQUIRE(nn20clock_media_list_add_file(&list, "1b.avi", 1u) == ESP_OK);
    REQUIRE(nn20clock_media_list_add_file(&list, "01A.avi", 1u) == ESP_OK);

    /* Padded or not, both are 1; the name breaks the tie, so the order
     * is total and neither plays twice. */
    const NN20ClockMediaEntry *entry =
        nn20clock_media_list_next_opener(&list, NULL);
    REQUIRE(entry != NULL);
    CHECK_STR_EQ("01A.avi", entry->path);

    entry = nn20clock_media_list_next_opener(&list, "01A.avi");
    REQUIRE(entry != NULL);
    CHECK_STR_EQ("1b.avi", entry->path);

    CHECK(nn20clock_media_list_next_opener(&list, "1b.avi") == NULL);
}

TEST(the_run_carries_on_when_the_last_opener_has_gone_missing)
{
    REQUIRE(nn20clock_media_list_init(&list, "") == ESP_OK);
    REQUIRE(nn20clock_media_list_add_file(&list, "3THIRD.AVI", 1u) == ESP_OK);

    /* "2SECOND.AVI" was deleted over FTP while it was playing. The run
     * is a function of the name, not of finding it in the folder, so it
     * carries on rather than stopping dead. */
    const NN20ClockMediaEntry *const entry =
        nn20clock_media_list_next_opener(&list, "2SECOND.AVI");
    REQUIRE(entry != NULL);
    CHECK_STR_EQ("3THIRD.AVI", entry->path);
}

TEST_MAIN("nn20clock_media")
{
    RUN(the_proven_formats_are_recognised);
    RUN(ordinary_names_are_accepted_as_components);
    RUN(traversal_attempts_are_refused_as_components);
    RUN(desktop_housekeeping_names_are_recognised);
    RUN(a_users_own_names_are_never_housekeeping);
    RUN(relative_paths_are_accepted);
    RUN(the_empty_path_is_the_root_folder_and_never_a_file);
    RUN(unsafe_relative_paths_are_refused);
    RUN(a_path_is_the_mount_point_plus_the_relative_path);
    RUN(the_root_folder_resolves_to_the_mount_point_itself);
    RUN(an_unsafe_path_produces_no_full_path);
    RUN(a_path_that_does_not_fit_is_refused);
    RUN(joining_builds_a_relative_path);
    RUN(a_join_too_long_for_the_device_is_refused);
    RUN(a_path_knows_its_folder_and_its_name);
    RUN(a_relative_path_resolves_against_the_current_folder);
    RUN(an_absolute_path_resolves_from_the_root);
    RUN(dot_and_dot_dot_are_navigation_and_are_canonicalised_away);
    RUN(resolving_cannot_climb_above_the_root);
    RUN(resolving_refuses_what_no_component_may_contain);
    RUN(a_listing_knows_which_folder_it_describes);
    RUN(entries_carry_their_kind_and_their_path);
    RUN(entries_in_the_root_have_bare_paths);
    RUN(unsafe_entries_are_refused_rather_than_added);
    RUN(the_list_fills_up_and_says_so);
    RUN(the_list_puts_folders_first_and_sorts_each_group);
    RUN(a_name_that_starts_with_a_number_is_an_opener);
    RUN(the_openers_play_in_their_numbers_order_before_the_draw);
    RUN(openers_that_lead_with_the_same_number_go_by_name);
    RUN(the_run_carries_on_when_the_last_opener_has_gone_missing);
}
