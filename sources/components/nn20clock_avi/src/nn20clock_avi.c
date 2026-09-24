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
 * nn20clock_avi.c - see the header.
 *
 * This is the port of the parsing proven in tryout/videoplayback, with
 * one change that matters: the tryout walked the file with a FILE* and
 * fseek, and this walks it with explicit offsets through a callback.
 * That is what lets the whole container format be tested on the host
 * against buffers built in the test, which is worth the small extra
 * bookkeeping - a malformed AVI is otherwise only ever discovered on
 * the board, at the moment the alarm goes off.
 *
 * The parsing is deliberately suspicious of the file. Sizes come
 * straight off the card and are clamped against the enclosing chunk
 * before they are used, list nesting is depth-limited, and every loop
 * advances by at least one header even when a chunk claims a size of
 * zero. A truncated or hostile file must end playback, not hang the
 * player task or walk off the end of a buffer.
 */
#include "nn20clock_avi.h"

#include <inttypes.h>
#include <string.h>

static const char *TAG = "NN20CLOCK_AVI";

/* A RIFF chunk header: four-character id, then a 32-bit size. */
#define CHUNK_HEADER_BYTES 8u

/* "LIST" adds a four-character type before its contents. */
#define LIST_TYPE_BYTES 4u

/*
 * How deep the lists may nest before the file is assumed to be
 * malformed. A real AVI needs two (RIFF > hdrl > strl); the limit is
 * here because this recurses, and the player task's stack is not
 * generous.
 */
#define MAX_LIST_DEPTH 8u

/*
 * One "idx1" record: a four-character chunk id, flags, the offset of
 * the chunk, and its size. Fixed width, which is what makes seeking a
 * matter of arithmetic rather than a scan.
 */
#define INDEX_RECORD_BYTES 16u
#define INDEX_ID_AT        0u
#define INDEX_OFFSET_AT    8u

/*
 * How far past a chosen record to look for a video chunk.
 *
 * The records are interleaved in the same order as the frames, so a
 * video one is never far away - a handful of audio records at most.
 * Bounded so that a malformed index costs a few reads rather than a
 * walk to the end of the file.
 */
#define INDEX_SEARCH_SPAN 32u

/* Design 16's rate, used when the file's own numbers are unusable. */
#define FALLBACK_FPS 15u

/* ------------------------------------------------------------ bytes -- */

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool fourcc_is(const char id[4], const char *expected)
{
    return memcmp(id, expected, 4) == 0;
}

static bool read_exact(NN20ClockAviReadFn read, void *ctx, uint32_t offset,
                       void *buffer, size_t size)
{
    return read(ctx, offset, buffer, size) == size;
}

/*
 * Read the chunk header at `offset`. False at end of file, which is a
 * normal way for a walk to finish rather than an error.
 */
static bool read_chunk_header(NN20ClockAviReadFn read, void *ctx,
                              uint32_t offset, char id[4], uint32_t *out_size)
{
    uint8_t header[CHUNK_HEADER_BYTES];
    if (!read_exact(read, ctx, offset, header, sizeof(header))) {
        return false;
    }
    memcpy(id, header, 4);
    *out_size = le32(&header[4]);
    return true;
}

/* Whether `offset` still has room for a chunk header before `end`,
 * written so it cannot overflow on a wild offset. */
static bool room_for_header(uint32_t offset, uint32_t end)
{
    return offset < end && (end - offset) >= CHUNK_HEADER_BYTES;
}

/* A codec id of zeroes or spaces tells us nothing. */
static bool fourcc_is_blank(const char id[4])
{
    for (size_t i = 0; i < 4; i++) {
        if (id[i] != '\0' && id[i] != ' ') {
            return false;
        }
    }
    return true;
}

static void set_codec(NN20ClockAviInfo *info, const char id[4])
{
    if (fourcc_is_blank(id)) {
        return;
    }
    memcpy(info->video_codec, id, 4);
    info->video_codec[4] = '\0';
}

static bool codec_is(const char *codec, const char *expected)
{
    for (size_t i = 0; i < 4; i++) {
        char a = codec[i];
        char b = expected[i];
        if (a >= 'a' && a <= 'z') {
            a = (char)(a - 'a' + 'A');
        }
        if (a != b) {
            return false;
        }
    }
    return true;
}

/* ---------------------------------------------------------- headers -- */

/*
 * One "strl" list: a "strh" saying what kind of stream this is, and a
 * "strf" describing its format. The two are read in order, so the strh
 * seen first is what tells us how to read the strf that follows.
 */
static esp_err_t parse_stream_list(NN20ClockAviReadFn read, void *ctx,
                                   uint32_t offset, uint32_t end,
                                   NN20ClockAviInfo *info)
{
    char stream_type[4] = {0};
    char handler[4] = {0};

    while (room_for_header(offset, end)) {
        char id[4];
        uint32_t size;
        if (!read_chunk_header(read, ctx, offset, id, &size)) {
            return ESP_OK;   /* truncated; keep what we have */
        }

        const uint32_t payload = offset + CHUNK_HEADER_BYTES;
        if (size > end - payload) {
            size = end - payload;
        }

        if (fourcc_is(id, "strh")) {
            /* AVISTREAMHEADER: type at 0, handler at 4, scale at 20,
             * rate at 24. */
            uint8_t header[56] = {0};
            const size_t want = size < sizeof(header) ? size : sizeof(header);
            if (!read_exact(read, ctx, payload, header, want)) {
                return ESP_FAIL;
            }
            memcpy(stream_type, header, 4);
            memcpy(handler, &header[4], 4);
            if (fourcc_is(stream_type, "vids") && want >= 28) {
                info->video_scale = le32(&header[20]);
                info->video_rate = le32(&header[24]);
            }
        } else if (fourcc_is(id, "strf")) {
            uint8_t fmt[40] = {0};
            const size_t want = size < sizeof(fmt) ? size : sizeof(fmt);
            if (!read_exact(read, ctx, payload, fmt, want)) {
                return ESP_FAIL;
            }
            if (fourcc_is(stream_type, "vids") && want >= 12) {
                /* BITMAPINFOHEADER: width at 4, height at 8,
                 * compression at 16. */
                info->width = le32(&fmt[4]);
                info->height = le32(&fmt[8]);
                if (want >= 20) {
                    set_codec(info, (const char *)&fmt[16]);
                }
                /* Some encoders leave biCompression empty and name the
                 * codec only in the stream header. */
                if (info->video_codec[0] == '\0') {
                    set_codec(info, handler);
                }
            } else if (fourcc_is(stream_type, "auds") && want >= 16) {
                /* WAVEFORMATEX: tag at 0, channels at 2, rate at 4,
                 * bits at 14. */
                info->audio_format = le16(&fmt[0]);
                info->audio_channels = le16(&fmt[2]);
                info->audio_sample_rate = le32(&fmt[4]);
                info->audio_bits = le16(&fmt[14]);
            }
        }

        offset = payload + size + (size & 1u);
    }

    return ESP_OK;
}

/*
 * Walk the chunks in [offset, end), descending into lists. Stops as
 * soon as the "movi" list is found: everything the player needs comes
 * before it, and the frames themselves are walked separately.
 */
static esp_err_t parse_chunks(NN20ClockAviReadFn read, void *ctx,
                              uint32_t offset, uint32_t end, unsigned depth,
                              NN20ClockAviInfo *info)
{
    if (depth > MAX_LIST_DEPTH) {
        ESP_LOGW(TAG, "AVI lists nested deeper than %u; giving up",
                 (unsigned)MAX_LIST_DEPTH);
        return ESP_OK;
    }

    while (room_for_header(offset, end)) {
        char id[4];
        uint32_t size;
        if (!read_chunk_header(read, ctx, offset, id, &size)) {
            return ESP_OK;   /* truncated; the caller decides if that is fatal */
        }

        const uint32_t payload = offset + CHUNK_HEADER_BYTES;
        if (size > end - payload) {
            size = end - payload;
        }
        const uint32_t chunk_end = payload + size;

        if (fourcc_is(id, "LIST") && size >= LIST_TYPE_BYTES) {
            char list_type[4];
            if (!read_exact(read, ctx, payload, list_type, sizeof(list_type))) {
                return ESP_FAIL;
            }
            const uint32_t inner = payload + LIST_TYPE_BYTES;

            if (fourcc_is(list_type, "movi")) {
                /*
                 * Recorded, and then the walk CONTINUES. The index sits
                 * after the frames, so returning here - which is what
                 * this did until seeking was needed - meant the parser
                 * never saw it. Stepping over the movi list is an
                 * offset jump, not a read, so the frames are not
                 * touched on the way past.
                 */
                info->movi_start = inner;
                info->movi_end = chunk_end;
                offset = chunk_end + (size & 1u);
                continue;
            }
            if (fourcc_is(list_type, "strl")) {
                const esp_err_t err =
                    parse_stream_list(read, ctx, inner, chunk_end, info);
                if (err != ESP_OK) {
                    return err;
                }
            } else if (info->movi_start != 0) {
                /* Past the frames now; only the index is still of
                 * interest, and it is not inside another list. */
            } else {
                const esp_err_t err =
                    parse_chunks(read, ctx, inner, chunk_end, depth + 1, info);
                if (err != ESP_OK) {
                    return err;
                }
            }
        } else if (fourcc_is(id, "idx1") && size >= INDEX_RECORD_BYTES) {
            info->index_offset = payload;
            info->index_count = size / INDEX_RECORD_BYTES;
        }

        offset = chunk_end + (size & 1u);
    }

    return ESP_OK;
}

/* ------------------------------------------------------------ index -- */

/* Defined with the chunk walk below; the index needs to tell a video
 * record from an audio one. */
static NN20ClockAviChunkKind chunk_kind(const char id[4]);

/* One record's chunk id and offset field. */
static bool read_index_record(NN20ClockAviReadFn read, void *ctx,
                              const NN20ClockAviInfo *info, uint32_t n,
                              char out_id[4], uint32_t *out_offset)
{
    if (n >= info->index_count) {
        return false;
    }

    uint8_t record[INDEX_RECORD_BYTES];
    const uint32_t at = info->index_offset + (n * INDEX_RECORD_BYTES);
    if (!read_exact(read, ctx, at, record, sizeof(record))) {
        return false;
    }

    memcpy(out_id, &record[INDEX_ID_AT], 4);
    *out_offset = le32(&record[INDEX_OFFSET_AT]);
    return true;
}

/* Does `base` make the first record point at the chunk it describes? */
static bool base_agrees(NN20ClockAviReadFn read, void *ctx,
                        const NN20ClockAviInfo *info, uint32_t base,
                        const char want_id[4], uint32_t record_offset)
{
    const uint32_t at = base + record_offset;
    if (at < info->movi_start || at >= info->movi_end) {
        return false;
    }

    char id[4];
    uint32_t size;
    if (!read_chunk_header(read, ctx, at, id, &size)) {
        return false;
    }
    return memcmp(id, want_id, 4) == 0;
}

/*
 * Work out what the index's offsets are measured from.
 *
 * Nothing in the file says, and both conventions are common, so this
 * tries each against the first record and keeps whichever lands on the
 * chunk that record names. Guessing wrong would seek into the middle of
 * a frame, so a file where neither works is treated as having no index
 * at all - refusing to seek beats seeking to rubbish.
 */
static void resolve_index_base(NN20ClockAviReadFn read, void *ctx,
                               NN20ClockAviInfo *info)
{
    if (info->index_count == 0u || info->movi_start == 0u) {
        return;
    }

    char id[4];
    uint32_t offset = 0;
    if (!read_index_record(read, ctx, info, 0u, id, &offset)) {
        info->index_count = 0u;
        return;
    }

    /* The usual convention: from the "movi" four-character code, which
     * sits just before the list's contents. */
    const uint32_t from_movi = info->movi_start - LIST_TYPE_BYTES;
    if (base_agrees(read, ctx, info, from_movi, id, offset)) {
        info->index_base = from_movi;
        return;
    }
    /* The other one: straight file offsets. */
    if (base_agrees(read, ctx, info, 0u, id, offset)) {
        info->index_base = 0u;
        return;
    }

    ESP_LOGW(TAG, "AVI index does not line up with the frames; "
                  "seeking is unavailable");
    info->index_count = 0u;
}

bool nn20clock_avi_has_index(const NN20ClockAviInfo *info)
{
    return info != NULL && info->index_count > 0u;
}

/*
 * If record `n` names a video chunk that lands inside the frames, give
 * back where it starts.
 */
static bool video_record_at(NN20ClockAviReadFn read, void *ctx,
                            const NN20ClockAviInfo *info, uint32_t n,
                            uint32_t *out_cursor)
{
    char id[4];
    uint32_t offset = 0;

    if (!read_index_record(read, ctx, info, n, id, &offset)) {
        return false;
    }
    if (chunk_kind(id) != NN20CLOCK_AVI_CHUNK_VIDEO) {
        return false;
    }

    const uint32_t at = info->index_base + offset;
    if (at < info->movi_start || at >= info->movi_end) {
        return false;   /* the index disagrees with the frames */
    }
    *out_cursor = at;
    return true;
}

esp_err_t nn20clock_avi_seek(NN20ClockAviReadFn read, void *ctx,
                             const NN20ClockAviInfo *info, uint32_t permille,
                             uint32_t *out_cursor)
{
    if (read == NULL || info == NULL || out_cursor == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!nn20clock_avi_has_index(info)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (permille > 1000u) {
        permille = 1000u;
    }

    uint32_t first = (uint32_t)(((uint64_t)info->index_count * permille) /
                                1000u);
    if (first >= info->index_count) {
        first = info->index_count - 1u;
    }

    /*
     * To the nearest video chunk. Landing on an audio record and
     * resuming there would start playback with a fragment of sound and
     * no picture until the next frame arrived.
     *
     * Forwards first, because a seek should not go back in time - but
     * then backwards, because the last records in a file are often
     * audio and a slider dragged to the end would otherwise find
     * nothing at all.
     */
    for (uint32_t i = 0; i < INDEX_SEARCH_SPAN; i++) {
        const uint32_t n = first + i;
        if (n >= info->index_count) {
            break;
        }
        if (video_record_at(read, ctx, info, n, out_cursor)) {
            return ESP_OK;
        }
    }
    for (uint32_t i = 1; i <= first && i <= INDEX_SEARCH_SPAN; i++) {
        if (video_record_at(read, ctx, info, first - i, out_cursor)) {
            return ESP_OK;
        }
    }

    return ESP_ERR_NOT_FOUND;
}

uint32_t nn20clock_avi_position_permille(const NN20ClockAviInfo *info,
                                         uint32_t cursor)
{
    if (info == NULL || info->movi_end <= info->movi_start) {
        return 0u;
    }
    if (cursor <= info->movi_start) {
        return 0u;
    }
    if (cursor >= info->movi_end) {
        return 1000u;
    }

    const uint32_t span = info->movi_end - info->movi_start;
    return (uint32_t)(((uint64_t)(cursor - info->movi_start) * 1000u) / span);
}

esp_err_t nn20clock_avi_parse(NN20ClockAviReadFn read, void *ctx,
                              uint32_t file_size, NN20ClockAviInfo *out_info)
{
    if (read == NULL || out_info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out_info, 0, sizeof(*out_info));

    /* "RIFF", a size, then the form type - "AVI " for a video. */
    uint8_t header[12];
    if (!read_exact(read, ctx, 0, header, sizeof(header))) {
        return ESP_ERR_INVALID_ARG;
    }
    if (memcmp(header, "RIFF", 4) != 0 || memcmp(&header[8], "AVI ", 4) != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Trust the real file size over the header's when we have it: a
     * RIFF size that is wrong is common, and a size that is too large
     * would let the walk run past the end of the card's file. */
    uint32_t end = file_size;
    if (end == 0) {
        const uint32_t riff_size = le32(&header[4]);
        end = (riff_size > UINT32_MAX - CHUNK_HEADER_BYTES)
                  ? UINT32_MAX
                  : riff_size + CHUNK_HEADER_BYTES;
    }
    if (end <= sizeof(header)) {
        return ESP_ERR_NOT_FOUND;
    }

    const esp_err_t err =
        parse_chunks(read, ctx, (uint32_t)sizeof(header), end, 0, out_info);
    if (err != ESP_OK) {
        return err;
    }
    if (out_info->movi_start == 0) {
        ESP_LOGE(TAG, "AVI has no movi list");
        return ESP_ERR_NOT_FOUND;
    }

    if (out_info->video_rate == 0 || out_info->video_scale == 0) {
        out_info->video_scale = 1;
        out_info->video_rate = FALLBACK_FPS;
    }
    if (out_info->movi_end <= out_info->movi_start || out_info->movi_end > end) {
        out_info->movi_end = end;
    }

    ESP_LOGI(TAG, "AVI %" PRIu32 "x%" PRIu32 " %s %" PRIu32 "/%" PRIu32 " fps",
             out_info->width, out_info->height,
             out_info->video_codec[0] != '\0' ? out_info->video_codec : "?",
             out_info->video_rate, out_info->video_scale);
    if (out_info->audio_sample_rate != 0) {
        ESP_LOGI(TAG, "AVI audio format=%u %uch %" PRIu32 "Hz %u-bit",
                 (unsigned)out_info->audio_format,
                 (unsigned)out_info->audio_channels,
                 out_info->audio_sample_rate,
                 (unsigned)out_info->audio_bits);
    }

    resolve_index_base(read, ctx, out_info);

    ESP_LOGI(TAG, "AVI frames at %" PRIu32 "..%" PRIu32 " (%" PRIu32
                  " B) of a %" PRIu32 " B file",
             out_info->movi_start, out_info->movi_end,
             out_info->movi_end - out_info->movi_start, end);

    if (nn20clock_avi_has_index(out_info)) {
        ESP_LOGI(TAG, "AVI index: %" PRIu32 " records, base %" PRIu32,
                 out_info->index_count, out_info->index_base);
    } else {
        ESP_LOGI(TAG, "AVI has no usable index; seeking is unavailable");
    }

    if (!nn20clock_avi_is_playable(out_info)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return ESP_OK;
}

uint32_t nn20clock_avi_frame_interval_us(const NN20ClockAviInfo *info)
{
    const uint32_t fallback = 1000000u / FALLBACK_FPS;

    if (info == NULL || info->video_rate == 0 || info->video_scale == 0) {
        return fallback;
    }

    const uint64_t us =
        ((uint64_t)info->video_scale * 1000000u) / (uint64_t)info->video_rate;

    /* Faster than a thousand frames a second, or slower than one, means
     * the file's numbers are nonsense rather than unusual. */
    if (us < 1000u || us > 1000000u) {
        return fallback;
    }
    return (uint32_t)us;
}

bool nn20clock_avi_is_playable(const NN20ClockAviInfo *info)
{
    if (info == NULL) {
        return false;
    }
    if (info->width == 0 || info->height == 0) {
        return false;
    }

    /* Design 16: MJPEG. "JPEG" is the same frames under a different
     * name and the hardware decoder does not care which. */
    if (!codec_is(info->video_codec, "MJPG") &&
        !codec_is(info->video_codec, "JPEG")) {
        return false;
    }

    /* A silent video is fine - the alarm is still a picture. */
    if (info->audio_sample_rate == 0) {
        return true;
    }

    return info->audio_format == NN20CLOCK_AVI_FORMAT_PCM &&
           info->audio_bits == 16 &&
           info->audio_channels >= 1 && info->audio_channels <= 2;
}

/* ----------------------------------------------------------- chunks -- */

static NN20ClockAviChunkKind chunk_kind(const char id[4])
{
    /* Chunks in movi are named "<stream><stream><kind><kind>": "00dc"
     * is compressed video on stream 0, "01wb" is audio bytes on stream
     * 1. Only the last two characters say which is which. */
    if (id[2] == 'd' && (id[3] == 'c' || id[3] == 'b')) {
        return NN20CLOCK_AVI_CHUNK_VIDEO;
    }
    if (id[2] == 'w' && id[3] == 'b') {
        return NN20CLOCK_AVI_CHUNK_AUDIO;
    }
    return NN20CLOCK_AVI_CHUNK_OTHER;
}

esp_err_t nn20clock_avi_next_chunk(NN20ClockAviReadFn read, void *ctx,
                                   const NN20ClockAviInfo *info,
                                   uint32_t *cursor,
                                   NN20ClockAviChunk *out_chunk)
{
    if (read == NULL || info == NULL || cursor == NULL || out_chunk == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t offset = *cursor;
    if (offset < info->movi_start) {
        offset = info->movi_start;
    }

    while (room_for_header(offset, info->movi_end)) {
        char id[4];
        uint32_t size;
        if (!read_chunk_header(read, ctx, offset, id, &size)) {
            break;   /* the file is shorter than movi_end claimed */
        }

        const uint32_t payload = offset + CHUNK_HEADER_BYTES;

        /* Some writers group frames into "rec " lists. Step into them
         * rather than over them: the frames inside are the point. */
        if (fourcc_is(id, "LIST")) {
            offset = payload + LIST_TYPE_BYTES;
            continue;
        }

        if (size > info->movi_end - payload) {
            size = info->movi_end - payload;
        }

        out_chunk->kind = chunk_kind(id);
        out_chunk->offset = payload;
        out_chunk->size = size;

        /* Chunks are padded to an even length. */
        *cursor = payload + size + (size & 1u);
        return ESP_OK;
    }

    *cursor = info->movi_end;
    return ESP_ERR_NOT_FOUND;
}
