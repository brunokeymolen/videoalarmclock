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
 * Component tests for the AVI reader (design 16).
 *
 * These build AVI files a byte at a time and hand them to the parser
 * through its reader callback. That is the whole reason the parser
 * takes a callback: the files that cause trouble are the malformed
 * ones, and those are far easier to write here than to produce on an SD
 * card and carry to the board.
 */
#include "test_util.h"

#include "nn20clock_avi.h"

#include <stdint.h>

/* ------------------------------------------------------ the builder -- */

typedef struct {
    uint8_t data[4096];
    uint32_t len;
} Avi;

static void put(Avi *a, const void *bytes, uint32_t size)
{
    REQUIRE(a->len + size <= sizeof(a->data));
    memcpy(&a->data[a->len], bytes, size);
    a->len += size;
}

static void put_fourcc(Avi *a, const char *id)
{
    put(a, id, 4);
}

static void put_le16(Avi *a, uint16_t value)
{
    const uint8_t bytes[2] = {(uint8_t)(value & 0xFFu),
                              (uint8_t)((value >> 8) & 0xFFu)};
    put(a, bytes, sizeof(bytes));
}

static void put_le32(Avi *a, uint32_t value)
{
    const uint8_t bytes[4] = {(uint8_t)(value & 0xFFu),
                              (uint8_t)((value >> 8) & 0xFFu),
                              (uint8_t)((value >> 16) & 0xFFu),
                              (uint8_t)((value >> 24) & 0xFFu)};
    put(a, bytes, sizeof(bytes));
}

static void put_filler(Avi *a, uint32_t size)
{
    for (uint32_t i = 0; i < size; i++) {
        const uint8_t byte = (uint8_t)(0x40u + (i & 0x0Fu));
        put(a, &byte, 1);
    }
}

/* Writes the header with a placeholder size and returns where that
 * size lives, for end_chunk() to fill in. */
static uint32_t begin_chunk(Avi *a, const char *id)
{
    put_fourcc(a, id);
    const uint32_t patch = a->len;
    put_le32(a, 0);
    return patch;
}

static void end_chunk(Avi *a, uint32_t patch)
{
    const uint32_t size = a->len - (patch + 4u);
    a->data[patch + 0] = (uint8_t)(size & 0xFFu);
    a->data[patch + 1] = (uint8_t)((size >> 8) & 0xFFu);
    a->data[patch + 2] = (uint8_t)((size >> 16) & 0xFFu);
    a->data[patch + 3] = (uint8_t)((size >> 24) & 0xFFu);
    if (size & 1u) {
        const uint8_t pad = 0;
        put(a, &pad, 1);   /* chunks are padded to an even length */
    }
}

static uint32_t begin_list(Avi *a, const char *type)
{
    const uint32_t patch = begin_chunk(a, "LIST");
    put_fourcc(a, type);
    return patch;
}

/*
 * One buffer, shared by every test.
 *
 * Not a local: four kilobytes is more than the target test app's task
 * stacks have. Not one per test either - eight separate statics is
 * thirty-two kilobytes of .bss, which is enough to stop the on-target
 * build linking. Each test builds into it from scratch, so sharing it
 * costs nothing.
 */
static Avi g_avi;

static size_t avi_read(void *ctx, uint32_t offset, void *buffer, size_t size)
{
    const Avi *a = (const Avi *)ctx;
    if (offset >= a->len) {
        return 0;
    }
    const size_t available = a->len - offset;
    if (size > available) {
        size = available;
    }
    memcpy(buffer, &a->data[offset], size);
    return size;
}

/* ------------------------------------------------- the file it reads -- */

static void put_video_stream(Avi *a, const char *codec,
                             uint32_t width, uint32_t height,
                             uint32_t scale, uint32_t rate)
{
    const uint32_t strl = begin_list(a, "strl");

    const uint32_t strh = begin_chunk(a, "strh");
    put_fourcc(a, "vids");
    put_fourcc(a, codec);         /* fccHandler */
    put_le32(a, 0);               /* flags */
    put_le16(a, 0);               /* priority */
    put_le16(a, 0);               /* language */
    put_le32(a, 0);               /* initial frames */
    put_le32(a, scale);
    put_le32(a, rate);
    put_le32(a, 0);               /* start */
    put_le32(a, 0);               /* length */
    end_chunk(a, strh);

    const uint32_t strf = begin_chunk(a, "strf");
    put_le32(a, 40);              /* biSize */
    put_le32(a, width);
    put_le32(a, height);
    put_le16(a, 1);               /* biPlanes */
    put_le16(a, 24);              /* biBitCount */
    put_fourcc(a, codec);         /* biCompression */
    put_le32(a, width * height);  /* biSizeImage */
    put_le32(a, 0);
    put_le32(a, 0);
    put_le32(a, 0);
    put_le32(a, 0);
    end_chunk(a, strf);

    end_chunk(a, strl);
}

static void put_audio_stream(Avi *a, uint16_t format, uint16_t channels,
                             uint32_t sample_rate, uint16_t bits)
{
    const uint32_t strl = begin_list(a, "strl");

    const uint32_t strh = begin_chunk(a, "strh");
    put_fourcc(a, "auds");
    put_le32(a, 0);
    put_le32(a, 0);
    put_le16(a, 0);
    put_le16(a, 0);
    put_le32(a, 0);
    put_le32(a, 1);               /* scale */
    put_le32(a, sample_rate);     /* rate */
    put_le32(a, 0);
    put_le32(a, 0);
    end_chunk(a, strh);

    const uint32_t strf = begin_chunk(a, "strf");
    put_le16(a, format);
    put_le16(a, channels);
    put_le32(a, sample_rate);
    put_le32(a, sample_rate * channels * (bits / 8u));   /* byte rate */
    put_le16(a, (uint16_t)(channels * (bits / 8u)));     /* block align */
    put_le16(a, bits);
    end_chunk(a, strf);

    end_chunk(a, strl);
}

/*
 * An ordinary AVI: a header list with one video and one audio stream,
 * then a movi list with two frames and a bit of audio.
 */
static void build_ordinary(Avi *a)
{
    memset(a, 0, sizeof(*a));

    const uint32_t riff = begin_chunk(a, "RIFF");
    put_fourcc(a, "AVI ");

    const uint32_t hdrl = begin_list(a, "hdrl");
    const uint32_t avih = begin_chunk(a, "avih");
    put_filler(a, 56);            /* the main header, which we skip */
    end_chunk(a, avih);
    put_video_stream(a, "MJPG", 720, 720, 1, 15);
    put_audio_stream(a, NN20CLOCK_AVI_FORMAT_PCM, 2, 44100, 16);
    end_chunk(a, hdrl);

    const uint32_t movi = begin_list(a, "movi");

    uint32_t chunk = begin_chunk(a, "00dc");
    put_filler(a, 100);
    end_chunk(a, chunk);

    chunk = begin_chunk(a, "01wb");
    put_filler(a, 40);
    end_chunk(a, chunk);

    chunk = begin_chunk(a, "00dc");
    put_filler(a, 60);
    end_chunk(a, chunk);

    end_chunk(a, movi);
    end_chunk(a, riff);
}

/*
 * The same file, plus an "idx1" index after the frames.
 *
 * `absolute` picks which of the two conventions the records use: an
 * offset from the "movi" four-character code, or a straight file
 * offset. Nothing in a real AVI says which it is, so both have to work
 * and the parser has to tell them apart by itself.
 */
static void build_with_index(Avi *a, bool absolute)
{
    memset(a, 0, sizeof(*a));

    const uint32_t riff = begin_chunk(a, "RIFF");
    put_fourcc(a, "AVI ");

    const uint32_t hdrl = begin_list(a, "hdrl");
    const uint32_t avih = begin_chunk(a, "avih");
    put_filler(a, 56);
    end_chunk(a, avih);
    put_video_stream(a, "MJPG", 720, 720, 1, 15);
    put_audio_stream(a, NN20CLOCK_AVI_FORMAT_PCM, 2, 44100, 16);
    end_chunk(a, hdrl);

    const uint32_t movi = begin_list(a, "movi");
    /* Where offsets are measured from in the usual convention: the
     * "movi" fourcc itself, four bytes before the list contents. */
    const uint32_t movi_fourcc = a->len - 4u;

    /* Four chunks, video and audio alternating, each remembered so the
     * index below can point at it. */
    uint32_t at[4];
    const char *ids[4] = {"00dc", "01wb", "00dc", "01wb"};
    const uint32_t sizes[4] = {100u, 40u, 60u, 40u};
    for (unsigned i = 0; i < 4; i++) {
        at[i] = a->len;
        const uint32_t chunk = begin_chunk(a, ids[i]);
        put_filler(a, sizes[i]);
        end_chunk(a, chunk);
    }
    end_chunk(a, movi);

    const uint32_t idx = begin_chunk(a, "idx1");
    for (unsigned i = 0; i < 4; i++) {
        put_fourcc(a, ids[i]);
        put_le32(a, 0x10u);                    /* flags: a keyframe */
        put_le32(a, absolute ? at[i] : at[i] - movi_fourcc);
        put_le32(a, sizes[i]);
    }
    end_chunk(a, idx);

    end_chunk(a, riff);
}

/* ----------------------------------------------------------- header -- */

TEST(an_ordinary_avi_is_read)
{
    Avi *const a_ptr = &g_avi;
    build_ordinary(a_ptr);

    NN20ClockAviInfo info;
    REQUIRE(nn20clock_avi_parse(avi_read, a_ptr, a_ptr->len, &info) == ESP_OK);

    CHECK_EQ(720, info.width);
    CHECK_EQ(720, info.height);
    CHECK_STR_EQ("MJPG", info.video_codec);
    CHECK_EQ(1, info.video_scale);
    CHECK_EQ(15, info.video_rate);

    CHECK_EQ(NN20CLOCK_AVI_FORMAT_PCM, info.audio_format);
    CHECK_EQ(2, info.audio_channels);
    CHECK_EQ(44100, info.audio_sample_rate);
    CHECK_EQ(16, info.audio_bits);

    /* The movi list holds the three chunks and nothing else. */
    CHECK(info.movi_start > 0);
    CHECK(info.movi_end > info.movi_start);
    CHECK(info.movi_end <= a_ptr->len);
    CHECK_EQ(3u * 8u + 100u + 40u + 60u, info.movi_end - info.movi_start);

    CHECK(nn20clock_avi_is_playable(&info));
}

/*
 * The file size we are given wins over the one the header claims. A
 * RIFF size that is too large is common in files that were cut short,
 * and believing it would walk off the end of the card's file.
 */
TEST(the_real_file_size_wins_over_the_header)
{
    Avi *const a_ptr = &g_avi;
    build_ordinary(a_ptr);

    /* Claim the file is far larger than it is. */
    a_ptr->data[4] = 0xFF;
    a_ptr->data[5] = 0xFF;
    a_ptr->data[6] = 0xFF;
    a_ptr->data[7] = 0x0F;

    NN20ClockAviInfo info;
    REQUIRE(nn20clock_avi_parse(avi_read, a_ptr, a_ptr->len, &info) == ESP_OK);
    CHECK(info.movi_end <= a_ptr->len);
}

TEST(a_file_that_is_not_an_avi_is_refused)
{
    Avi *const a_ptr = &g_avi;

    memset(a_ptr, 0, sizeof(*a_ptr));
    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_avi_parse(avi_read, a_ptr, a_ptr->len, &(NN20ClockAviInfo){0}));

    /* A RIFF file, but a sound one. */
    memset(a_ptr, 0, sizeof(*a_ptr));
    put_fourcc(a_ptr, "RIFF");
    put_le32(a_ptr, 100);
    put_fourcc(a_ptr, "WAVE");
    put_filler(a_ptr, 100);
    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_avi_parse(avi_read, a_ptr, a_ptr->len, &(NN20ClockAviInfo){0}));

    /* Shorter than the twelve bytes a RIFF header needs. */
    memset(a_ptr, 0, sizeof(*a_ptr));
    put_fourcc(a_ptr, "RIFF");
    put_le32(a_ptr, 4);
    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_avi_parse(avi_read, a_ptr, a_ptr->len, &(NN20ClockAviInfo){0}));

    NN20ClockAviInfo info;
    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_avi_parse(NULL, a_ptr, a_ptr->len, &info));
    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_avi_parse(avi_read, a_ptr, a_ptr->len, NULL));
}

TEST(an_avi_without_frames_is_refused)
{
    Avi *const a_ptr = &g_avi;
    memset(a_ptr, 0, sizeof(*a_ptr));

    const uint32_t riff = begin_chunk(a_ptr, "RIFF");
    put_fourcc(a_ptr, "AVI ");
    const uint32_t hdrl = begin_list(a_ptr, "hdrl");
    put_video_stream(a_ptr, "MJPG", 720, 720, 1, 15);
    end_chunk(a_ptr, hdrl);
    end_chunk(a_ptr, riff);

    NN20ClockAviInfo info;
    CHECK_EQ(ESP_ERR_NOT_FOUND,
             nn20clock_avi_parse(avi_read, a_ptr, a_ptr->len, &info));
}

/*
 * "Not a video" and "a video we cannot play" are different errors
 * because they want different words on screen.
 */
TEST(streams_this_device_cannot_decode_are_named)
{
    Avi *const a_ptr = &g_avi;
    NN20ClockAviInfo info;

    memset(a_ptr, 0, sizeof(*a_ptr));
    uint32_t riff = begin_chunk(a_ptr, "RIFF");
    put_fourcc(a_ptr, "AVI ");
    uint32_t hdrl = begin_list(a_ptr, "hdrl");
    put_video_stream(a_ptr, "DIVX", 720, 720, 1, 15);
    end_chunk(a_ptr, hdrl);
    uint32_t movi = begin_list(a_ptr, "movi");
    uint32_t chunk = begin_chunk(a_ptr, "00dc");
    put_filler(a_ptr, 16);
    end_chunk(a_ptr, chunk);
    end_chunk(a_ptr, movi);
    end_chunk(a_ptr, riff);

    CHECK_EQ(ESP_ERR_NOT_SUPPORTED,
             nn20clock_avi_parse(avi_read, a_ptr, a_ptr->len, &info));
    /* Filled in anyway, so the failure can say which codec it was. */
    CHECK_STR_EQ("DIVX", info.video_codec);
    CHECK_EQ(720, info.width);

    /* MJPEG video, but MP3 audio. */
    memset(a_ptr, 0, sizeof(*a_ptr));
    riff = begin_chunk(a_ptr, "RIFF");
    put_fourcc(a_ptr, "AVI ");
    hdrl = begin_list(a_ptr, "hdrl");
    put_video_stream(a_ptr, "MJPG", 720, 720, 1, 15);
    put_audio_stream(a_ptr, 0x0055, 2, 44100, 16);
    end_chunk(a_ptr, hdrl);
    movi = begin_list(a_ptr, "movi");
    chunk = begin_chunk(a_ptr, "00dc");
    put_filler(a_ptr, 16);
    end_chunk(a_ptr, chunk);
    end_chunk(a_ptr, movi);
    end_chunk(a_ptr, riff);

    CHECK_EQ(ESP_ERR_NOT_SUPPORTED,
             nn20clock_avi_parse(avi_read, a_ptr, a_ptr->len, &info));
    CHECK_EQ(0x0055, info.audio_format);
}

TEST(a_silent_video_is_still_playable)
{
    Avi *const a_ptr = &g_avi;
    memset(a_ptr, 0, sizeof(*a_ptr));

    const uint32_t riff = begin_chunk(a_ptr, "RIFF");
    put_fourcc(a_ptr, "AVI ");
    const uint32_t hdrl = begin_list(a_ptr, "hdrl");
    put_video_stream(a_ptr, "MJPG", 720, 720, 1, 15);
    end_chunk(a_ptr, hdrl);
    const uint32_t movi = begin_list(a_ptr, "movi");
    const uint32_t chunk = begin_chunk(a_ptr, "00dc");
    put_filler(a_ptr, 16);
    end_chunk(a_ptr, chunk);
    end_chunk(a_ptr, movi);
    end_chunk(a_ptr, riff);

    NN20ClockAviInfo info;
    REQUIRE(nn20clock_avi_parse(avi_read, a_ptr, a_ptr->len, &info) == ESP_OK);
    CHECK_EQ(0, info.audio_sample_rate);
    CHECK(nn20clock_avi_is_playable(&info));
}

TEST(playability_is_decided_field_by_field)
{
    NN20ClockAviInfo info = {
        .width = 720, .height = 720,
        .video_codec = "MJPG",
        .video_rate = 15, .video_scale = 1,
        .audio_format = NN20CLOCK_AVI_FORMAT_PCM,
        .audio_channels = 2, .audio_sample_rate = 44100, .audio_bits = 16,
    };
    CHECK(nn20clock_avi_is_playable(&info));

    /* The same frames under the other common name. */
    memcpy(info.video_codec, "jpeg", 5);
    CHECK(nn20clock_avi_is_playable(&info));
    memcpy(info.video_codec, "MJPG", 5);

    info.audio_bits = 8;
    CHECK(!nn20clock_avi_is_playable(&info));
    info.audio_bits = 16;

    info.audio_channels = 6;
    CHECK(!nn20clock_avi_is_playable(&info));
    info.audio_channels = 2;

    info.width = 0;
    CHECK(!nn20clock_avi_is_playable(&info));
    info.width = 720;

    CHECK(!nn20clock_avi_is_playable(NULL));
}

TEST(the_frame_interval_comes_from_the_rate_pair)
{
    NN20ClockAviInfo info = {.video_scale = 1, .video_rate = 15};
    CHECK_EQ(66666, nn20clock_avi_frame_interval_us(&info));

    info.video_scale = 1;
    info.video_rate = 30;
    CHECK_EQ(33333, nn20clock_avi_frame_interval_us(&info));

    /* 29.97 fps is 30000/1001, and not 30. This is why the pair is kept
     * rather than reduced to a number of frames per second. */
    info.video_scale = 1001;
    info.video_rate = 30000;
    CHECK_EQ(33366, nn20clock_avi_frame_interval_us(&info));

    /* Unusable numbers fall back to design 16's proven rate. */
    info.video_scale = 0;
    info.video_rate = 0;
    CHECK_EQ(66666, nn20clock_avi_frame_interval_us(&info));

    info.video_scale = 1;
    info.video_rate = 100000;   /* implausibly fast */
    CHECK_EQ(66666, nn20clock_avi_frame_interval_us(&info));

    info.video_scale = 60;
    info.video_rate = 1;        /* one frame a minute */
    CHECK_EQ(66666, nn20clock_avi_frame_interval_us(&info));

    CHECK_EQ(66666, nn20clock_avi_frame_interval_us(NULL));
}

/* ----------------------------------------------------------- chunks -- */

TEST(the_frames_are_walked_in_order)
{
    Avi *const a_ptr = &g_avi;
    build_ordinary(a_ptr);

    NN20ClockAviInfo info;
    REQUIRE(nn20clock_avi_parse(avi_read, a_ptr, a_ptr->len, &info) == ESP_OK);

    uint32_t cursor = info.movi_start;
    NN20ClockAviChunk chunk;

    REQUIRE(nn20clock_avi_next_chunk(avi_read, a_ptr, &info, &cursor, &chunk) == ESP_OK);
    CHECK_EQ(NN20CLOCK_AVI_CHUNK_VIDEO, chunk.kind);
    CHECK_EQ(100, chunk.size);
    CHECK_EQ(info.movi_start + 8u, chunk.offset);

    REQUIRE(nn20clock_avi_next_chunk(avi_read, a_ptr, &info, &cursor, &chunk) == ESP_OK);
    CHECK_EQ(NN20CLOCK_AVI_CHUNK_AUDIO, chunk.kind);
    CHECK_EQ(40, chunk.size);

    REQUIRE(nn20clock_avi_next_chunk(avi_read, a_ptr, &info, &cursor, &chunk) == ESP_OK);
    CHECK_EQ(NN20CLOCK_AVI_CHUNK_VIDEO, chunk.kind);
    CHECK_EQ(60, chunk.size);

    /* Running out of chunks is how playback learns the file is over. */
    CHECK_EQ(ESP_ERR_NOT_FOUND,
             nn20clock_avi_next_chunk(avi_read, a_ptr, &info, &cursor, &chunk));
    /* And asking again keeps saying so. */
    CHECK_EQ(ESP_ERR_NOT_FOUND,
             nn20clock_avi_next_chunk(avi_read, a_ptr, &info, &cursor, &chunk));
}

/*
 * An odd-sized chunk is followed by a pad byte that is not part of it.
 * Miss that and every chunk after the first odd one is read one byte
 * out of step - which looks like a corrupt file rather than a bug.
 */
TEST(odd_sized_chunks_are_stepped_over_with_their_padding)
{
    Avi *const a_ptr = &g_avi;
    memset(a_ptr, 0, sizeof(*a_ptr));

    const uint32_t riff = begin_chunk(a_ptr, "RIFF");
    put_fourcc(a_ptr, "AVI ");
    const uint32_t hdrl = begin_list(a_ptr, "hdrl");
    put_video_stream(a_ptr, "MJPG", 720, 720, 1, 15);
    end_chunk(a_ptr, hdrl);

    const uint32_t movi = begin_list(a_ptr, "movi");
    uint32_t chunk_at = begin_chunk(a_ptr, "00dc");
    put_filler(a_ptr, 33);           /* odd */
    end_chunk(a_ptr, chunk_at);
    chunk_at = begin_chunk(a_ptr, "00dc");
    put_filler(a_ptr, 7);            /* odd again */
    end_chunk(a_ptr, chunk_at);
    end_chunk(a_ptr, movi);
    end_chunk(a_ptr, riff);

    NN20ClockAviInfo info;
    REQUIRE(nn20clock_avi_parse(avi_read, a_ptr, a_ptr->len, &info) == ESP_OK);

    uint32_t cursor = info.movi_start;
    NN20ClockAviChunk chunk;
    REQUIRE(nn20clock_avi_next_chunk(avi_read, a_ptr, &info, &cursor, &chunk) == ESP_OK);
    CHECK_EQ(33, chunk.size);
    REQUIRE(nn20clock_avi_next_chunk(avi_read, a_ptr, &info, &cursor, &chunk) == ESP_OK);
    CHECK_EQ(NN20CLOCK_AVI_CHUNK_VIDEO, chunk.kind);
    CHECK_EQ(7, chunk.size);
    CHECK_EQ(ESP_ERR_NOT_FOUND,
             nn20clock_avi_next_chunk(avi_read, a_ptr, &info, &cursor, &chunk));
}

/* Some writers group frames into "rec " lists. The frames inside are
 * the point, so the walk steps into them. */
TEST(rec_lists_are_stepped_into)
{
    Avi *const a_ptr = &g_avi;
    memset(a_ptr, 0, sizeof(*a_ptr));

    const uint32_t riff = begin_chunk(a_ptr, "RIFF");
    put_fourcc(a_ptr, "AVI ");
    const uint32_t hdrl = begin_list(a_ptr, "hdrl");
    put_video_stream(a_ptr, "MJPG", 720, 720, 1, 15);
    end_chunk(a_ptr, hdrl);

    const uint32_t movi = begin_list(a_ptr, "movi");
    const uint32_t rec = begin_list(a_ptr, "rec ");
    uint32_t chunk_at = begin_chunk(a_ptr, "00dc");
    put_filler(a_ptr, 24);
    end_chunk(a_ptr, chunk_at);
    chunk_at = begin_chunk(a_ptr, "01wb");
    put_filler(a_ptr, 12);
    end_chunk(a_ptr, chunk_at);
    end_chunk(a_ptr, rec);
    chunk_at = begin_chunk(a_ptr, "00dc");
    put_filler(a_ptr, 20);
    end_chunk(a_ptr, chunk_at);
    end_chunk(a_ptr, movi);
    end_chunk(a_ptr, riff);

    NN20ClockAviInfo info;
    REQUIRE(nn20clock_avi_parse(avi_read, a_ptr, a_ptr->len, &info) == ESP_OK);

    uint32_t cursor = info.movi_start;
    NN20ClockAviChunk chunk;
    REQUIRE(nn20clock_avi_next_chunk(avi_read, a_ptr, &info, &cursor, &chunk) == ESP_OK);
    CHECK_EQ(NN20CLOCK_AVI_CHUNK_VIDEO, chunk.kind);
    CHECK_EQ(24, chunk.size);
    REQUIRE(nn20clock_avi_next_chunk(avi_read, a_ptr, &info, &cursor, &chunk) == ESP_OK);
    CHECK_EQ(NN20CLOCK_AVI_CHUNK_AUDIO, chunk.kind);
    CHECK_EQ(12, chunk.size);
    REQUIRE(nn20clock_avi_next_chunk(avi_read, a_ptr, &info, &cursor, &chunk) == ESP_OK);
    CHECK_EQ(NN20CLOCK_AVI_CHUNK_VIDEO, chunk.kind);
    CHECK_EQ(20, chunk.size);
    CHECK_EQ(ESP_ERR_NOT_FOUND,
             nn20clock_avi_next_chunk(avi_read, a_ptr, &info, &cursor, &chunk));
}

/* Chunks that are not frames - an index, padding - are handed back
 * rather than hidden, so the player decides what to do with them. */
TEST(chunks_that_are_neither_are_reported_as_other)
{
    Avi *const a_ptr = &g_avi;
    memset(a_ptr, 0, sizeof(*a_ptr));

    const uint32_t riff = begin_chunk(a_ptr, "RIFF");
    put_fourcc(a_ptr, "AVI ");
    const uint32_t hdrl = begin_list(a_ptr, "hdrl");
    put_video_stream(a_ptr, "MJPG", 720, 720, 1, 15);
    end_chunk(a_ptr, hdrl);

    const uint32_t movi = begin_list(a_ptr, "movi");
    uint32_t chunk_at = begin_chunk(a_ptr, "JUNK");
    put_filler(a_ptr, 16);
    end_chunk(a_ptr, chunk_at);
    chunk_at = begin_chunk(a_ptr, "00dc");
    put_filler(a_ptr, 16);
    end_chunk(a_ptr, chunk_at);
    end_chunk(a_ptr, movi);
    end_chunk(a_ptr, riff);

    NN20ClockAviInfo info;
    REQUIRE(nn20clock_avi_parse(avi_read, a_ptr, a_ptr->len, &info) == ESP_OK);

    uint32_t cursor = info.movi_start;
    NN20ClockAviChunk chunk;
    REQUIRE(nn20clock_avi_next_chunk(avi_read, a_ptr, &info, &cursor, &chunk) == ESP_OK);
    CHECK_EQ(NN20CLOCK_AVI_CHUNK_OTHER, chunk.kind);
    REQUIRE(nn20clock_avi_next_chunk(avi_read, a_ptr, &info, &cursor, &chunk) == ESP_OK);
    CHECK_EQ(NN20CLOCK_AVI_CHUNK_VIDEO, chunk.kind);
}

/*
 * A file cut short mid-frame. The last chunk claims more bytes than
 * exist; playback must end rather than read past the end.
 */
TEST(a_truncated_file_ends_the_walk)
{
    Avi *const a_ptr = &g_avi;
    build_ordinary(a_ptr);

    NN20ClockAviInfo info;
    REQUIRE(nn20clock_avi_parse(avi_read, a_ptr, a_ptr->len, &info) == ESP_OK);

    /* Chop the file in half, after the parse, the way a card pulled
     * mid-write would. */
    a_ptr->len = info.movi_start + 20u;

    uint32_t cursor = info.movi_start;
    NN20ClockAviChunk chunk;
    unsigned walked = 0;
    while (nn20clock_avi_next_chunk(avi_read, a_ptr, &info, &cursor, &chunk) == ESP_OK) {
        walked++;
        REQUIRE(walked < 100);   /* it must terminate */
    }
    CHECK(walked <= 3);
}

/* A chunk claiming a gigabyte inside a small file must not send the
 * reader past the end of the movi list. */
TEST(an_oversized_chunk_is_clamped_to_the_movi_list)
{
    Avi *const a_ptr = &g_avi;
    memset(a_ptr, 0, sizeof(*a_ptr));

    const uint32_t riff = begin_chunk(a_ptr, "RIFF");
    put_fourcc(a_ptr, "AVI ");
    const uint32_t hdrl = begin_list(a_ptr, "hdrl");
    put_video_stream(a_ptr, "MJPG", 720, 720, 1, 15);
    end_chunk(a_ptr, hdrl);

    const uint32_t movi = begin_list(a_ptr, "movi");
    const uint32_t chunk_at = begin_chunk(a_ptr, "00dc");
    put_filler(a_ptr, 32);
    end_chunk(a_ptr, chunk_at);
    end_chunk(a_ptr, movi);
    end_chunk(a_ptr, riff);

    /* Rewrite the frame's size as a gigabyte. */
    a_ptr->data[chunk_at + 0] = 0x00;
    a_ptr->data[chunk_at + 1] = 0x00;
    a_ptr->data[chunk_at + 2] = 0x00;
    a_ptr->data[chunk_at + 3] = 0x40;

    NN20ClockAviInfo info;
    REQUIRE(nn20clock_avi_parse(avi_read, a_ptr, a_ptr->len, &info) == ESP_OK);

    uint32_t cursor = info.movi_start;
    NN20ClockAviChunk chunk;
    REQUIRE(nn20clock_avi_next_chunk(avi_read, a_ptr, &info, &cursor, &chunk) == ESP_OK);
    CHECK(chunk.offset + chunk.size <= info.movi_end);
    CHECK_EQ(ESP_ERR_NOT_FOUND,
             nn20clock_avi_next_chunk(avi_read, a_ptr, &info, &cursor, &chunk));
}

/* Lists nested without end are a malformed file, not a reason to run
 * the player task's stack out. */
TEST(endlessly_nested_lists_do_not_recurse_forever)
{
    Avi *const a_ptr = &g_avi;
    memset(a_ptr, 0, sizeof(*a_ptr));

    const uint32_t riff = begin_chunk(a_ptr, "RIFF");
    put_fourcc(a_ptr, "AVI ");

    uint32_t lists[64];
    const unsigned depth = 40;
    for (unsigned i = 0; i < depth; i++) {
        lists[i] = begin_list(a_ptr, "odml");
    }
    put_filler(a_ptr, 8);
    for (unsigned i = depth; i > 0; i--) {
        end_chunk(a_ptr, lists[i - 1]);
    }
    end_chunk(a_ptr, riff);

    NN20ClockAviInfo info;
    CHECK_EQ(ESP_ERR_NOT_FOUND,
             nn20clock_avi_parse(avi_read, a_ptr, a_ptr->len, &info));
}

TEST(the_walk_refuses_missing_arguments)
{
    Avi *const a_ptr = &g_avi;
    build_ordinary(a_ptr);

    NN20ClockAviInfo info;
    REQUIRE(nn20clock_avi_parse(avi_read, a_ptr, a_ptr->len, &info) == ESP_OK);

    uint32_t cursor = info.movi_start;
    NN20ClockAviChunk chunk;
    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_avi_next_chunk(NULL, a_ptr, &info, &cursor, &chunk));
    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_avi_next_chunk(avi_read, a_ptr, NULL, &cursor, &chunk));
    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_avi_next_chunk(avi_read, a_ptr, &info, NULL, &chunk));
    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_avi_next_chunk(avi_read, a_ptr, &info, &cursor, NULL));
}

/* ------------------------------------------------------------ index -- */

TEST(a_file_with_no_index_says_so_rather_than_guessing)
{
    Avi *const a_ptr = &g_avi;
    build_ordinary(a_ptr);

    NN20ClockAviInfo info;
    REQUIRE(nn20clock_avi_parse(avi_read, a_ptr, a_ptr->len, &info) == ESP_OK);

    /* Legal and common. Such a file plays; it just cannot be seeked,
     * and the caller is told that rather than being given a guess. */
    CHECK(!nn20clock_avi_has_index(&info));
    CHECK_EQ(0u, info.index_count);

    uint32_t cursor = 0;
    CHECK_EQ(ESP_ERR_NOT_SUPPORTED,
             nn20clock_avi_seek(avi_read, a_ptr, &info, 500u, &cursor));
}

TEST(an_index_relative_to_the_movi_fourcc_is_found)
{
    Avi *const a_ptr = &g_avi;
    build_with_index(a_ptr, false);

    NN20ClockAviInfo info;
    REQUIRE(nn20clock_avi_parse(avi_read, a_ptr, a_ptr->len, &info) == ESP_OK);

    CHECK(nn20clock_avi_has_index(&info));
    CHECK_EQ(4u, info.index_count);
    /* Finding it at all means the parser kept walking past the movi
     * list rather than stopping at it. */
    CHECK(info.index_offset > info.movi_end);
}

TEST(an_index_of_absolute_offsets_is_found_too)
{
    Avi *const a_ptr = &g_avi;
    build_with_index(a_ptr, true);

    NN20ClockAviInfo info;
    REQUIRE(nn20clock_avi_parse(avi_read, a_ptr, a_ptr->len, &info) == ESP_OK);

    CHECK(nn20clock_avi_has_index(&info));
    CHECK_EQ(0u, info.index_base);   /* offsets are from the file start */
}

/*
 * The point of the whole exercise: a seek lands on a video chunk
 * header, so playback resumes on a frame rather than inside one.
 */
TEST(a_seek_lands_on_a_video_chunk_under_either_convention)
{
    for (unsigned pass = 0; pass < 2u; pass++) {
        Avi *const a_ptr = &g_avi;
        build_with_index(a_ptr, pass == 1u);

        NN20ClockAviInfo info;
        REQUIRE(nn20clock_avi_parse(avi_read, a_ptr, a_ptr->len, &info)
                == ESP_OK);

        for (uint32_t permille = 0; permille <= 1000u; permille += 100u) {
            uint32_t cursor = 0;
            REQUIRE(nn20clock_avi_seek(avi_read, a_ptr, &info, permille,
                                       &cursor) == ESP_OK);

            /* Inside the frames, and reading a chunk from there gives a
             * video one - which is only true if the cursor is on a
             * header boundary. */
            CHECK(cursor >= info.movi_start);
            CHECK(cursor < info.movi_end);

            uint32_t walk = cursor;
            NN20ClockAviChunk chunk;
            REQUIRE(nn20clock_avi_next_chunk(avi_read, a_ptr, &info, &walk,
                                             &chunk) == ESP_OK);
            CHECK_EQ(NN20CLOCK_AVI_CHUNK_VIDEO, chunk.kind);
        }
    }
}

TEST(seeking_to_the_start_returns_the_first_frame)
{
    Avi *const a_ptr = &g_avi;
    build_with_index(a_ptr, false);

    NN20ClockAviInfo info;
    REQUIRE(nn20clock_avi_parse(avi_read, a_ptr, a_ptr->len, &info) == ESP_OK);

    uint32_t cursor = 0;
    REQUIRE(nn20clock_avi_seek(avi_read, a_ptr, &info, 0u, &cursor) == ESP_OK);
    CHECK_EQ(info.movi_start, cursor);
}

/*
 * An index whose offsets match neither convention is thrown away.
 *
 * Guessing would seek into the middle of a frame and decode rubbish,
 * which is worse than a slider that does not move: the file still
 * plays perfectly from the beginning.
 */
TEST(an_index_that_matches_neither_convention_is_discarded)
{
    Avi *const a_ptr = &g_avi;
    build_with_index(a_ptr, false);

    /* Corrupt the first record's offset so neither base lands on the
     * chunk it names. The record's offset field is 8 bytes in. */
    NN20ClockAviInfo probe;
    REQUIRE(nn20clock_avi_parse(avi_read, a_ptr, a_ptr->len, &probe)
            == ESP_OK);
    a_ptr->data[probe.index_offset + 8u] ^= 0x55u;

    NN20ClockAviInfo info;
    REQUIRE(nn20clock_avi_parse(avi_read, a_ptr, a_ptr->len, &info) == ESP_OK);
    CHECK(!nn20clock_avi_has_index(&info));
}

TEST(a_position_reads_back_as_a_fraction_of_the_frames)
{
    Avi *const a_ptr = &g_avi;
    build_ordinary(a_ptr);

    NN20ClockAviInfo info;
    REQUIRE(nn20clock_avi_parse(avi_read, a_ptr, a_ptr->len, &info) == ESP_OK);

    CHECK_EQ(0u, nn20clock_avi_position_permille(&info, info.movi_start));
    CHECK_EQ(1000u, nn20clock_avi_position_permille(&info, info.movi_end));
    /* Before and after the frames clamp rather than wrapping. */
    CHECK_EQ(0u, nn20clock_avi_position_permille(&info, 0u));
    CHECK_EQ(1000u, nn20clock_avi_position_permille(&info, 0xFFFFFFFFu));

    const uint32_t middle =
        info.movi_start + ((info.movi_end - info.movi_start) / 2u);
    const uint32_t at = nn20clock_avi_position_permille(&info, middle);
    CHECK(at >= 490u && at <= 510u);
}

TEST(index_accessors_tolerate_null)
{
    uint32_t cursor = 0;
    CHECK(!nn20clock_avi_has_index(NULL));
    CHECK_EQ(0u, nn20clock_avi_position_permille(NULL, 0u));
    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_avi_seek(NULL, NULL, NULL, 0u, &cursor));
}

TEST_MAIN("nn20clock_avi")
{
    RUN(an_ordinary_avi_is_read);
    RUN(a_file_with_no_index_says_so_rather_than_guessing);
    RUN(an_index_relative_to_the_movi_fourcc_is_found);
    RUN(an_index_of_absolute_offsets_is_found_too);
    RUN(a_seek_lands_on_a_video_chunk_under_either_convention);
    RUN(seeking_to_the_start_returns_the_first_frame);
    RUN(an_index_that_matches_neither_convention_is_discarded);
    RUN(a_position_reads_back_as_a_fraction_of_the_frames);
    RUN(index_accessors_tolerate_null);
    RUN(the_real_file_size_wins_over_the_header);
    RUN(a_file_that_is_not_an_avi_is_refused);
    RUN(an_avi_without_frames_is_refused);
    RUN(streams_this_device_cannot_decode_are_named);
    RUN(a_silent_video_is_still_playable);
    RUN(playability_is_decided_field_by_field);
    RUN(the_frame_interval_comes_from_the_rate_pair);
    RUN(the_frames_are_walked_in_order);
    RUN(odd_sized_chunks_are_stepped_over_with_their_padding);
    RUN(rec_lists_are_stepped_into);
    RUN(chunks_that_are_neither_are_reported_as_other);
    RUN(a_truncated_file_ends_the_walk);
    RUN(an_oversized_chunk_is_clamped_to_the_movi_list);
    RUN(endlessly_nested_lists_do_not_recurse_forever);
    RUN(the_walk_refuses_missing_arguments);
}
