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
 * nn20clock_avi.h - reading an AVI file (design 16's proven format).
 *
 * Dual-mode. The parsing is pure byte-wrangling over a reader callback,
 * so the whole container format - which is where malformed files cause
 * trouble - is tested on the host against buffers built in the test,
 * with no filesystem and no card.
 *
 * Design 16 settled the format for the first version: an AVI container,
 * MJPEG video at 720x720, and PCM audio. This reads exactly that and
 * says so plainly when handed anything else, rather than half-playing
 * it.
 *
 * ------------------------------------------------------------------
 * The shape of an AVI, briefly
 * ------------------------------------------------------------------
 *
 * A RIFF file is nested chunks: a four-character id, a 32-bit
 * little-endian size, then that many bytes, padded to an even length.
 * "LIST" chunks contain a further four-character type and more chunks.
 *
 * What matters here is the "hdrl" list, which holds one "strl" per
 * stream describing it, and the "movi" list, which holds the actual
 * frames as chunks named "00dc" (video data) and "01wb" (audio bytes) -
 * the leading digits being the stream number.
 */
#ifndef NN20CLOCK_AVI_H
#define NN20CLOCK_AVI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nn20clock_platform.h"

#ifdef __cplusplus
extern "C" {
#endif

/* WAVE_FORMAT_PCM. Anything else is compressed audio this device does
 * not decode. */
#define NN20CLOCK_AVI_FORMAT_PCM 1u

typedef struct {
    uint32_t width;
    uint32_t height;
    /* Frame rate as the file states it: rate/scale. Kept as the pair
     * rather than a float, because the player needs exact microseconds
     * per frame and 30000/1001 is not 29.97. */
    uint32_t video_rate;
    uint32_t video_scale;
    /* The four-character codec id, NUL-terminated so it can be logged.
     * "MJPG" is the one this device decodes; anything else is worth
     * saying out loud, which is why it is kept rather than reduced to a
     * boolean. */
    char video_codec[5];

    /* A sample rate of zero means the file has no audio stream at all,
     * which is a normal thing for an AVI to be and not an error. */
    uint16_t audio_format;      /* NN20CLOCK_AVI_FORMAT_PCM, or not */
    uint16_t audio_channels;
    uint32_t audio_sample_rate;
    uint16_t audio_bits;

    /* Byte range of the "movi" list: where the frames live. */
    uint32_t movi_start;
    uint32_t movi_end;

    /*
     * The "idx1" index, if the file has one: a flat array of 16-byte
     * records, one per chunk, sitting after the frames.
     *
     * This is what makes seeking possible. Without it a byte offset
     * lands in the middle of a chunk and there is no reliable way back
     * to a boundary - a JPEG payload can contain any four bytes,
     * including something that reads exactly like a chunk header, so
     * scanning for one can resynchronise onto rubbish.
     *
     * `index_count` is zero when the file has no usable index, which is
     * legal; such a file plays perfectly and simply cannot be seeked.
     */
    uint32_t index_offset;   /* first record */
    uint32_t index_count;    /* records, not bytes */
    /*
     * What the offsets inside the records are measured from.
     *
     * The format is ambiguous in practice: most writers store an offset
     * from the "movi" four-character code, some store an absolute file
     * offset, and nothing in the file says which. It is resolved by
     * trying both against the first record and seeing which one lands
     * on the chunk that record describes. See nn20clock_avi_parse().
     */
    uint32_t index_base;
} NN20ClockAviInfo;

/*
 * Read `size` bytes at `offset` into `buffer`. Returns the number read,
 * which is short only at end of file.
 *
 * A callback rather than a FILE*, so the parser has no opinion about
 * where the bytes come from: the firmware passes an SD file, the tests
 * pass a buffer in memory.
 */
typedef size_t (*NN20ClockAviReadFn)(void *ctx, uint32_t offset,
                                     void *buffer, size_t size);

/*
 * Parse the headers.
 *
 * ESP_ERR_INVALID_ARG when this is not a RIFF/AVI file at all,
 * ESP_ERR_NOT_FOUND when it has no "movi" list, ESP_ERR_NOT_SUPPORTED
 * when it is an AVI whose streams this device cannot play. The distinct
 * codes matter: "not a video" and "a video we cannot decode" want
 * different words on screen.
 *
 * On ESP_ERR_NOT_SUPPORTED `out_info` is still filled in, so the caller
 * can log which codec it was actually handed.
 *
 * `file_size` may be zero when it is not known, in which case the size
 * the RIFF header claims is used instead.
 */
esp_err_t nn20clock_avi_parse(NN20ClockAviReadFn read, void *ctx,
                              uint32_t file_size, NN20ClockAviInfo *out_info);

/* Microseconds per frame, from the rate/scale pair. Falls back to 15 fps
 * - design 16's proven rate - when the file's numbers are unusable,
 * because a plausible frame rate beats refusing to play. */
uint32_t nn20clock_avi_frame_interval_us(const NN20ClockAviInfo *info);

/* Whether the streams are ones this device can actually play: MJPEG
 * video and PCM audio, per design 16. Audio is optional; a silent video
 * is still playable. */
bool nn20clock_avi_is_playable(const NN20ClockAviInfo *info);

/* ----------------------------------------------------------- chunks -- */

typedef enum {
    NN20CLOCK_AVI_CHUNK_VIDEO,
    NN20CLOCK_AVI_CHUNK_AUDIO,
    NN20CLOCK_AVI_CHUNK_OTHER
} NN20ClockAviChunkKind;

typedef struct {
    NN20ClockAviChunkKind kind;
    uint32_t offset;   /* of the payload, not the header */
    uint32_t size;
} NN20ClockAviChunk;

/*
 * The next frame chunk at or after `cursor`, which must start inside the
 * movi list. On success `cursor` is advanced past it.
 *
 * ESP_ERR_NOT_FOUND at the end of the movi list, which is how playback
 * knows the file is over.
 */
esp_err_t nn20clock_avi_next_chunk(NN20ClockAviReadFn read, void *ctx,
                                   const NN20ClockAviInfo *info,
                                   uint32_t *cursor,
                                   NN20ClockAviChunk *out_chunk);

/* ------------------------------------------------------------ index -- */

/* Whether the file carries an index this device could seek with. */
bool nn20clock_avi_has_index(const NN20ClockAviInfo *info);

/*
 * Where to resume to land `permille` (0-1000) of the way through the
 * frames.
 *
 * The cursor that comes back is the offset of a VIDEO chunk's header,
 * so playback picks up on a frame rather than inside one - and because
 * the chunks are interleaved, the audio that belongs with it follows
 * immediately. Hand it straight to nn20clock_avi_next_chunk().
 *
 * Positions by index record rather than by time, which for MJPEG is the
 * same thing to within a frame: every frame stands alone, so there is
 * no keyframe to hunt backwards for.
 *
 * ESP_ERR_NOT_SUPPORTED when the file has no index, which is the
 * caller's cue to leave the position control alone rather than to
 * report a failure.
 */
esp_err_t nn20clock_avi_seek(NN20ClockAviReadFn read, void *ctx,
                             const NN20ClockAviInfo *info, uint32_t permille,
                             uint32_t *out_cursor);

/*
 * How far through the frames `cursor` is, in permille (0-1000), for a
 * position control to show. Measured by byte offset within the movi
 * list, not by time - close enough for a slider, and it needs no index.
 */
uint32_t nn20clock_avi_position_permille(const NN20ClockAviInfo *info,
                                         uint32_t cursor);

#ifdef __cplusplus
}
#endif

#endif /* NN20CLOCK_AVI_H */
