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
#ifndef NN20_MPSC_QUEUE_H
#define NN20_MPSC_QUEUE_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#ifndef MPSC_CACHELINE_BYTES
#define MPSC_CACHELINE_BYTES 64
#endif

#if !defined(MPSC_ALIGNAS)
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define MPSC_ALIGNAS(n) _Alignas(n)
#else
#define MPSC_ALIGNAS(n)
#endif
#endif

typedef struct mpscq_slot {
    _Atomic size_t seq;
    void *ctx;
    void *value;
} mpscq_slot;

typedef struct mpscq_index_atomic {
    MPSC_ALIGNAS(MPSC_CACHELINE_BYTES) _Atomic size_t value;
} mpscq_index_atomic;

/* Only the single consumer writes head, but observers (queue depth for
 * statistics) read it from other threads, so it is atomic with relaxed
 * ordering rather than a plain size_t. Costs nothing, and keeps the
 * thread sanitiser quiet. */
typedef struct mpscq_index_plain {
    MPSC_ALIGNAS(MPSC_CACHELINE_BYTES) _Atomic size_t value;
} mpscq_index_plain;

typedef struct nn20_mpsc_queue {
    size_t capacity;
    size_t mask;
    mpscq_index_atomic tail; /* written by producers */
    mpscq_index_plain head;  /* written by the single consumer */
    mpscq_slot *slots;
} nn20_mpsc_queue;

static inline bool nn20_mpsc_queue_is_pow2(size_t v) {
    return v && ((v & (v - 1)) == 0);
}

static inline bool nn20_mpsc_queue_init(nn20_mpsc_queue *q, size_t capacity) {
    if (!q || capacity < 2 || !nn20_mpsc_queue_is_pow2(capacity)) {
        return false;
    }

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
    const size_t alloc_raw = capacity * sizeof(mpscq_slot);
    const size_t align_mask = (size_t)MPSC_CACHELINE_BYTES - 1u;
    const size_t alloc_size = (alloc_raw + align_mask) & ~align_mask;
    q->slots = aligned_alloc(MPSC_CACHELINE_BYTES, alloc_size);
#else
    q->slots = malloc(capacity * sizeof(mpscq_slot));
#endif
    if (!q->slots) {
        return false;
    }

    q->capacity = capacity;
    q->mask = capacity - 1;
    atomic_store_explicit(&q->tail.value, 0u, memory_order_relaxed);
    atomic_store_explicit(&q->head.value, 0u, memory_order_relaxed);

    for (size_t i = 0; i < capacity; ++i) {
        atomic_store_explicit(&q->slots[i].seq, i, memory_order_relaxed);
        q->slots[i].value = NULL;
    }

    return true;
}

static inline void nn20_mpsc_queue_deinit(nn20_mpsc_queue *q) {
    if (!q) {
        return;
    }
    free(q->slots);
    q->slots = NULL;
    q->capacity = 0;
    q->mask = 0;
    atomic_store_explicit(&q->tail.value, 0u, memory_order_relaxed);
    atomic_store_explicit(&q->head.value, 0u, memory_order_relaxed);
}

/* Multi-producer enqueue. Returns false if the queue is full. */
static inline bool nn20_mpsc_queue_push(nn20_mpsc_queue *q, void *ctx, void *value) {
    if (!q) {
        return false;
    }

    size_t pos = atomic_load_explicit(&q->tail.value, memory_order_relaxed);

    for (;;) {
        mpscq_slot *slot = &q->slots[pos & q->mask];
        size_t seq = atomic_load_explicit(&slot->seq, memory_order_acquire);
        intptr_t diff = (intptr_t)seq - (intptr_t)pos;

        if (diff == 0) {
            if (atomic_compare_exchange_weak_explicit(&q->tail.value,
                                                      &pos,
                                                      pos + 1,
                                                      memory_order_acq_rel,
                                                      memory_order_relaxed)) {
                slot->ctx = ctx;
                slot->value = value;
                atomic_store_explicit(&slot->seq, pos + 1, memory_order_release);
                return true;
            }
            continue;
        }

        if (diff < 0) {
            return false; /* queue is full */
        }

        pos = atomic_load_explicit(&q->tail.value, memory_order_relaxed);
    }
}

/* Single-consumer dequeue. Returns false if the queue is empty. */
static inline bool nn20_mpsc_queue_pop(nn20_mpsc_queue *q, void **out_ctx, void **out_value) {
    if (!q || !out_ctx || !out_value) {
        return false;
    }

    size_t pos = atomic_load_explicit(&q->head.value, memory_order_relaxed);
    /* Each slot's sequence counter encodes which turn owns it. */
    mpscq_slot *slot = &q->slots[pos & q->mask];
    size_t seq = atomic_load_explicit(&slot->seq, memory_order_acquire);
    intptr_t diff = (intptr_t)seq - (intptr_t)(pos + 1);

    if (diff == 0) {
        void *ctx = slot->ctx;
        void *val = slot->value;
        atomic_store_explicit(&slot->seq, pos + q->mask + 1, memory_order_release);
        atomic_store_explicit(&q->head.value, pos + 1, memory_order_relaxed);
        *out_ctx = ctx;
        *out_value = val;
        return true;
    }

    return false; /* empty */
}

#endif /* NN20_MPSC_QUEUE_H */
