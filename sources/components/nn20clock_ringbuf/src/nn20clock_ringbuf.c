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
 * nn20clock_ringbuf.c - see the header.
 *
 * The whole file is arithmetic on two monotonically increasing file
 * offsets. Nothing here allocates, blocks, or logs on the hot path: the
 * consumer calls _read() several times per frame and the producer
 * _writable() once per block.
 *
 * A note on the offsets wrapping. They are 32-bit because that is what
 * a RIFF file addresses - the format cannot describe a file larger than
 * 4 GB, which nn20clock_avi.h says out loud - so a stream cannot run
 * long enough to wrap them. Differences are computed as
 * `end - start` on unsigned values, which is correct as long as the
 * window is never larger than the capacity, and it never is.
 */
#include "nn20clock_ringbuf.h"

#include <string.h>

static const char *TAG = "NN20CLOCK_RINGBUF";

static uint32_t load_produced(const NN20ClockRingbuf *pthis)
{
    return (uint32_t)atomic_load_explicit(&pthis->produced,
                                          memory_order_acquire);
}

static uint32_t load_consumed(const NN20ClockRingbuf *pthis)
{
    return (uint32_t)atomic_load_explicit(&pthis->consumed,
                                          memory_order_acquire);
}

esp_err_t nn20clock_ringbuf_init(NN20ClockRingbuf *pthis, uint8_t *data,
                                 size_t capacity, uint32_t start)
{
    if (pthis == NULL || data == NULL || capacity == 0u) {
        ESP_LOGE(TAG, "a window needs a buffer and a size");
        return ESP_ERR_INVALID_ARG;
    }

    pthis->data = data;
    pthis->capacity = capacity;
    pthis->base = start;
    atomic_init(&pthis->produced, start);
    atomic_init(&pthis->consumed, start);
    return ESP_OK;
}

void nn20clock_ringbuf_reset(NN20ClockRingbuf *pthis, uint32_t start)
{
    if (pthis == NULL) {
        return;
    }
    pthis->base = start;
    atomic_store_explicit(&pthis->produced, start, memory_order_release);
    atomic_store_explicit(&pthis->consumed, start, memory_order_release);
}

/* ----------------------------------------------------------- producer -- */

size_t nn20clock_ringbuf_writable(const NN20ClockRingbuf *pthis,
                                  uint32_t *out_offset, uint8_t **out_dst)
{
    if (pthis == NULL || out_offset == NULL || out_dst == NULL) {
        return 0u;
    }

    const uint32_t produced = load_produced(pthis);
    const uint32_t consumed = load_consumed(pthis);
    const size_t held = (size_t)(produced - consumed);
    const size_t free_bytes = pthis->capacity - held;

    *out_offset = produced;
    const size_t index = (size_t)((produced - pthis->base) % pthis->capacity);
    *out_dst = &pthis->data[index];

    /*
     * Contiguous only. Stopping at the end of the buffer means the
     * caller can read straight into it rather than through a bounce
     * buffer, which is the entire point of doing this - the next call
     * picks up at index zero and gets the rest.
     */
    const size_t to_end = pthis->capacity - index;
    return (free_bytes < to_end) ? free_bytes : to_end;
}

void nn20clock_ringbuf_produced(NN20ClockRingbuf *pthis, size_t bytes)
{
    if (pthis == NULL || bytes == 0u) {
        return;
    }
    /* Release: the bytes must be visible before the offset that claims
     * they are there. */
    atomic_store_explicit(&pthis->produced,
                          load_produced(pthis) + (uint32_t)bytes,
                          memory_order_release);
}

/* ----------------------------------------------------------- consumer -- */

bool nn20clock_ringbuf_read(const NN20ClockRingbuf *pthis, uint32_t offset,
                            void *dst, size_t size)
{
    if (pthis == NULL || dst == NULL) {
        return false;
    }
    if (size == 0u) {
        return true;
    }
    if (size > pthis->capacity) {
        return false;   /* larger than the window can ever hold */
    }

    const uint32_t produced = load_produced(pthis);
    const uint32_t consumed = load_consumed(pthis);

    /* Behind the window: those bytes have been overwritten. Ahead of
     * it: they have not arrived. Neither is an error here - the caller
     * waits, or reopens. */
    if (offset < consumed || offset > produced) {
        return false;
    }
    if ((uint32_t)(produced - offset) < (uint32_t)size) {
        return false;
    }

    const size_t index = (size_t)((offset - pthis->base) % pthis->capacity);
    const size_t to_end = pthis->capacity - index;

    if (size <= to_end) {
        memcpy(dst, &pthis->data[index], size);
    } else {
        /* Straddles the join. */
        memcpy(dst, &pthis->data[index], to_end);
        memcpy((uint8_t *)dst + to_end, &pthis->data[0], size - to_end);
    }
    return true;
}

void nn20clock_ringbuf_consume_to(NN20ClockRingbuf *pthis, uint32_t offset)
{
    if (pthis == NULL) {
        return;
    }

    const uint32_t consumed = load_consumed(pthis);
    const uint32_t produced = load_produced(pthis);

    /* Never backwards, and never past what exists. */
    if (offset <= consumed) {
        return;
    }
    if (offset > produced) {
        offset = produced;
    }
    atomic_store_explicit(&pthis->consumed, offset, memory_order_release);
}

uint32_t nn20clock_ringbuf_start(const NN20ClockRingbuf *pthis)
{
    return (pthis != NULL) ? load_consumed(pthis) : 0u;
}

uint32_t nn20clock_ringbuf_end(const NN20ClockRingbuf *pthis)
{
    return (pthis != NULL) ? load_produced(pthis) : 0u;
}

size_t nn20clock_ringbuf_available(const NN20ClockRingbuf *pthis)
{
    if (pthis == NULL) {
        return 0u;
    }
    return (size_t)(load_produced(pthis) - load_consumed(pthis));
}
