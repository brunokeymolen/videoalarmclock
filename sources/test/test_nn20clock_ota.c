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
 * Component tests for the update version comparison.
 *
 * This is the decision that says whether the clock replaces its own
 * firmware, and it is wrong quietly in both directions: too cautious
 * and a clock never updates, too eager and it installs an older build
 * over a newer one - then does it again at the next check, forever.
 * Neither shows up in a boot log.
 *
 * The awkward input is not the version numbers, it is what `git
 * describe` does between tags. A development board runs
 * "v0.2.0-7-g1a2b3c4-dirty", which is *ahead* of the v0.2.0 the
 * manifest offers, and the release must not be pushed onto it.
 */
#include "test_util.h"

#include "nn20clock_ota_version.h"

/* ------------------------------------------------------------ parsing -- */

TEST(parses_a_plain_tag)
{
    unsigned parts[3] = {9u, 9u, 9u};
    bool extra = true;

    CHECK(nn20clock_ota_parse_version("v1.2.3", parts, &extra));
    CHECK_EQ(1u, parts[0]);
    CHECK_EQ(2u, parts[1]);
    CHECK_EQ(3u, parts[2]);
    CHECK(!extra);
}

TEST(the_v_is_optional)
{
    unsigned parts[3] = {0u, 0u, 0u};

    CHECK(nn20clock_ota_parse_version("0.1.0", parts, NULL));
    CHECK_EQ(0u, parts[0]);
    CHECK_EQ(1u, parts[1]);
    CHECK_EQ(0u, parts[2]);
}

/* What every build between two tags looks like. */
TEST(a_describe_suffix_parses_and_is_flagged)
{
    unsigned parts[3] = {0u, 0u, 0u};
    bool extra = false;

    CHECK(nn20clock_ota_parse_version("v0.1.0-2-gb741df4", parts, &extra));
    CHECK_EQ(0u, parts[0]);
    CHECK_EQ(1u, parts[1]);
    CHECK_EQ(0u, parts[2]);
    CHECK(extra);

    extra = false;
    CHECK(nn20clock_ota_parse_version("v0.1.0-2-gb741df4-dirty", parts,
                                      &extra));
    CHECK(extra);
}

TEST(multi_digit_components)
{
    unsigned parts[3] = {0u, 0u, 0u};

    CHECK(nn20clock_ota_parse_version("v10.20.30", parts, NULL));
    CHECK_EQ(10u, parts[0]);
    CHECK_EQ(20u, parts[1]);
    CHECK_EQ(30u, parts[2]);
}

TEST(rubbish_is_refused)
{
    unsigned parts[3] = {7u, 7u, 7u};

    CHECK(!nn20clock_ota_parse_version(NULL, parts, NULL));
    CHECK(!nn20clock_ota_parse_version("", parts, NULL));
    CHECK(!nn20clock_ota_parse_version("v", parts, NULL));
    CHECK(!nn20clock_ota_parse_version("1.2", parts, NULL));
    CHECK(!nn20clock_ota_parse_version("1.2.", parts, NULL));
    CHECK(!nn20clock_ota_parse_version("latest", parts, NULL));
    CHECK(!nn20clock_ota_parse_version("v1..3", parts, NULL));
    CHECK(!nn20clock_ota_parse_version("v1.2.x", parts, NULL));

    /* A refused parse leaves the caller's buffer alone rather than
     * half-filled, so ignoring the return cannot yield a version that
     * was never in the string. */
    CHECK_EQ(7u, parts[0]);
    CHECK_EQ(7u, parts[1]);
    CHECK_EQ(7u, parts[2]);
}

/* A number long enough to overflow the accumulator is a broken
 * manifest, not a very new release. */
TEST(an_absurd_component_is_refused)
{
    unsigned parts[3] = {0u, 0u, 0u};

    CHECK(!nn20clock_ota_parse_version("v99999999999.0.0", parts, NULL));
}

/* --------------------------------------------------------- comparing -- */

TEST(a_higher_release_is_offered)
{
    CHECK_EQ(NN20CLOCK_OTA_VERSION_NEWER,
             nn20clock_ota_compare_versions("v0.2.0", "v0.1.0"));
    CHECK_EQ(NN20CLOCK_OTA_VERSION_NEWER,
             nn20clock_ota_compare_versions("v0.1.1", "v0.1.0"));
    CHECK_EQ(NN20CLOCK_OTA_VERSION_NEWER,
             nn20clock_ota_compare_versions("v1.0.0", "v0.9.9"));
}

TEST(the_same_release_is_not_offered)
{
    CHECK_EQ(NN20CLOCK_OTA_VERSION_NOT_NEWER,
             nn20clock_ota_compare_versions("v0.1.0", "v0.1.0"));
}

TEST(an_older_release_is_never_offered)
{
    CHECK_EQ(NN20CLOCK_OTA_VERSION_NOT_NEWER,
             nn20clock_ota_compare_versions("v0.1.0", "v0.2.0"));
    CHECK_EQ(NN20CLOCK_OTA_VERSION_NOT_NEWER,
             nn20clock_ota_compare_versions("v0.9.9", "v1.0.0"));
}

/*
 * The one that matters on this desk. The board is running two commits
 * past v0.1.0; a manifest still offering v0.1.0 must not push it back.
 */
TEST(a_development_build_is_not_pushed_backwards)
{
    CHECK_EQ(NN20CLOCK_OTA_VERSION_NOT_NEWER,
             nn20clock_ota_compare_versions("v0.1.0",
                                            "v0.1.0-2-gb741df4"));
    CHECK_EQ(NN20CLOCK_OTA_VERSION_NOT_NEWER,
             nn20clock_ota_compare_versions("v0.1.0",
                                            "v0.1.0-2-gb741df4-dirty"));
}

/* But a genuinely newer release still reaches it. */
TEST(a_development_build_still_gets_the_next_release)
{
    CHECK_EQ(NN20CLOCK_OTA_VERSION_NEWER,
             nn20clock_ota_compare_versions("v0.2.0",
                                            "v0.1.0-2-gb741df4-dirty"));
}

TEST(a_manifest_that_cannot_name_itself_offers_nothing)
{
    CHECK_EQ(NN20CLOCK_OTA_VERSION_OFFER_UNREADABLE,
             nn20clock_ota_compare_versions("latest", "v0.1.0"));
    CHECK_EQ(NN20CLOCK_OTA_VERSION_OFFER_UNREADABLE,
             nn20clock_ota_compare_versions(NULL, "v0.1.0"));
    /* Checked before the running version, so an unreadable pair is
     * still refused rather than installed. */
    CHECK_EQ(NN20CLOCK_OTA_VERSION_OFFER_UNREADABLE,
             nn20clock_ota_compare_versions("latest", "unknown"));
}

/*
 * A build with no tag in its history - a fresh clone, a shallow CI
 * checkout - has nothing to compare against, so the release is the only
 * version either side can name and it is offered.
 */
TEST(an_untagged_build_is_offered_the_release)
{
    CHECK_EQ(NN20CLOCK_OTA_VERSION_RUNNING_UNREADABLE,
             nn20clock_ota_compare_versions("v0.2.0", "unknown"));
    CHECK_EQ(NN20CLOCK_OTA_VERSION_RUNNING_UNREADABLE,
             nn20clock_ota_compare_versions("v0.2.0", NULL));
}

TEST_MAIN("nn20clock_ota")
{
    RUN(parses_a_plain_tag);
    RUN(the_v_is_optional);
    RUN(a_describe_suffix_parses_and_is_flagged);
    RUN(multi_digit_components);
    RUN(rubbish_is_refused);
    RUN(an_absurd_component_is_refused);

    RUN(a_higher_release_is_offered);
    RUN(the_same_release_is_not_offered);
    RUN(an_older_release_is_never_offered);
    RUN(a_development_build_is_not_pushed_backwards);
    RUN(a_development_build_still_gets_the_next_release);
    RUN(a_manifest_that_cannot_name_itself_offers_nothing);
    RUN(an_untagged_build_is_offered_the_release);
}
