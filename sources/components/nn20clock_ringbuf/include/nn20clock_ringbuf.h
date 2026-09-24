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
 * nn20clock_ringbuf.h - a sliding window over a byte stream (design 17,
 * Milestone 11).
 *
 * Dual-mode. The arithmetic is pure - offsets, capacity, and wrapping -
 * so it is tested on the host with no threads and no card, which is
 * where this kind of code goes wrong.
 *
 * ------------------------------------------------------------------
 * Why this exists
 * ------------------------------------------------------------------
 *
 * Measured on the board: playing a 720x720 MJPEG cost 8.8 SD
 * transactions per frame, 4.4 of them fetching eight bytes of chunk
 * header, at about 4.5 ms per transaction. Reading took 40 ms of a
 * 66 ms frame, and because the player read and fed the codec on one
 * thread, that 40 ms landed on top of the 35 ms the codec spends
 * accepting samples rather than beside it.
 *
 * This fixes both halves at once. A reader fills the window ahead in
 * large aligned blocks while the player is busy elsewhere, and the
 * player's reads become memcpy out of PSRAM.
 *
 * ------------------------------------------------------------------
 * Absolute offsets, not indices
 * ------------------------------------------------------------------
 *
 * Positions here are offsets into the FILE, not into the buffer. The
 * byte at file offset `o` lives at `data[(o - base) % capacity]`, where
 * `base` is where the window was started, and "do I have offset o yet"
 * is a comparison rather than a bookkeeping exercise.
 *
 * Indexing from `base` rather than from zero is what keeps the fills
 * aligned. A window starting at file offset 4096 with a 100-byte buffer
 * would otherwise begin at index 96 and offer four contiguous bytes,
 * and the producer's first read would be four bytes long - the exact
 * thing this is here to stop. From `base`, a fresh window always begins
 * at index 0, so with a capacity that is a whole number of blocks every
 * fill boundary is a block boundary.
 *
 * That is what lets nn20clock_avi_next_chunk() stay exactly as it is:
 * it already reads through a callback taking a file offset, so the
 * callback is pointed here instead of at fread() and the parser cannot
 * tell the difference. Its host tests keep covering the format.
 *
 * ------------------------------------------------------------------
 * Threading
 * ------------------------------------------------------------------
 *
 * One producer and one consumer, and no lock - which is the same answer
 * the rest of this project gives. `produced` is written only by the
 * reader and `consumed` only by the player; each reads the other's with
 * acquire/release, and neither ever waits for the other inside this
 * file. A caller that needs to wait does so outside, where it can
 * decide what waiting means.
 *
 * Every function is safe to call from the side that owns it while the
 * other side is running. The exceptions are _init() and _reset(), which
 * move both ends and so must not run while the other side is touching
 * the window.
 */
#ifndef NN20CLOCK_RINGBUF_H
#define NN20CLOCK_RINGBUF_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nn20clock_platform.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t *data;      /* borrowed; capacity bytes */
    size_t capacity;

    /* File offset one past the last byte filled. Written by the
     * producer only. */
    atomic_uint_least32_t produced;
    /* File offset of the first byte the consumer still wants. Written
     * by the consumer only; everything before it is free to overwrite. */
    atomic_uint_least32_t consumed;

    /* Where the window was started. Only _init() and _reset() move it,
     * and neither may run while either side is working. */
    uint32_t base;
} NN20ClockRingbuf;

/*
 * `data` is borrowed and must outlive this; `capacity` must be non-zero.
 *
 * `start` is the file offset the window begins at, and the caller
 * should align it down to whatever the filesystem likes - on this
 * device FATFS has a 4096-byte sector, and a fill that starts
 * mid-sector costs a whole extra sector at each end. Aligning the start
 * and filling in whole blocks keeps every read aligned by construction.
 */
esp_err_t nn20clock_ringbuf_init(NN20ClockRingbuf *pthis, uint8_t *data,
                                 size_t capacity, uint32_t start);

/* Throw the window away and begin again at `start`. Neither side may be
 * running. */
void nn20clock_ringbuf_reset(NN20ClockRingbuf *pthis, uint32_t start);

/* ------------------------------------------------------- producer -- */

/*
 * Where to put the next bytes, and how many may go there in one piece.
 *
 * `out_offset` is the file offset to read from - already aligned if the
 * window started aligned and every fill has been a whole block.
 * `out_dst` points into the buffer. The count is contiguous: it stops
 * at the end of the buffer rather than wrapping, so the caller can read
 * straight into it.
 *
 * Zero means the window is full and the consumer has to catch up.
 */
size_t nn20clock_ringbuf_writable(const NN20ClockRingbuf *pthis,
                                  uint32_t *out_offset, uint8_t **out_dst);

/* Publish `bytes` written at the position _writable() reported. */
void nn20clock_ringbuf_produced(NN20ClockRingbuf *pthis, size_t bytes);

/* ------------------------------------------------------- consumer -- */

/*
 * Copy `size` bytes at file offset `offset` into `dst`.
 *
 * All or nothing: false when the window does not hold the whole range,
 * either because the producer has not got there yet or because the
 * offset is behind the window and the bytes are gone. The caller
 * decides which by asking _available() and _start().
 */
bool nn20clock_ringbuf_read(const NN20ClockRingbuf *pthis, uint32_t offset,
                            void *dst, size_t size);

/*
 * Release everything before `offset`. Idempotent, and ignores an offset
 * that would move the window backwards.
 */
void nn20clock_ringbuf_consume_to(NN20ClockRingbuf *pthis, uint32_t offset);

/* The oldest offset still held, and one past the newest. */
uint32_t nn20clock_ringbuf_start(const NN20ClockRingbuf *pthis);
uint32_t nn20clock_ringbuf_end(const NN20ClockRingbuf *pthis);

/* Bytes held: _end() - _start(). */
size_t nn20clock_ringbuf_available(const NN20ClockRingbuf *pthis);

#ifdef __cplusplus
}
#endif

#endif /* NN20CLOCK_RINGBUF_H */
