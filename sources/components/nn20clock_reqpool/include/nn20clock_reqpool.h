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
 * nn20clock_reqpool.h - fixed pool of request slots, claimed lock-free.
 *
 * Posting async work to a worker needs somewhere to put the request:
 * the caller's stack is gone by the time the callback runs, so the
 * arguments have to live somewhere the worker can still read.
 *
 * This is that somewhere. A component embeds an array of request structs
 * plus one NN20ClockReqPool, claims a slot before posting, and releases
 * it in the worker callback. No malloc on the posting path, no
 * fragmentation, and a hard bound on how much work can be in flight - a
 * full pool is backpressure the caller can see and react to.
 *
 * No mutex, and deliberately so. A slot is claimed with a single
 * atomic_exchange: whoever flips the flag from false to true owns it,
 * everyone else moves to the next slot. Nothing ever waits, so nothing
 * can deadlock, and a claim from an ISR or a higher-priority task cannot
 * be blocked by a lower-priority one holding a lock.
 *
 * The slot payload itself needs no atomics: only the claiming thread
 * writes it (before the post) and only the worker thread reads it
 * (after), and the queue's release/acquire pair orders the two.
 *
 * Header-only; there is nothing here worth a translation unit.
 */
#ifndef NN20CLOCK_REQPOOL_H
#define NN20CLOCK_REQPOOL_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Slots are cheap - one atomic_bool each - but the payload they guard is
 * not, so keep pool sizes to what can plausibly be in flight. */
typedef struct {
    atomic_bool *in_use;
    size_t count;
} NN20ClockReqPool;

/*
 * flags must be an array of `count` atomic_bool that outlives the pool,
 * normally a member of the owning struct. calloc() zeroes them, which is
 * a valid "free" for atomic_bool on every platform this project targets;
 * initialize explicitly anyway so it is not luck.
 */
static inline void nn20clock_reqpool_init(NN20ClockReqPool *pool,
                                          atomic_bool *flags, size_t count)
{
    pool->in_use = flags;
    pool->count = count;
    for (size_t i = 0; i < count; i++) {
        atomic_init(&flags[i], false);
    }
}

/*
 * Claim a free slot and return its index, or `count` when the pool is
 * full - the same "== count means none" convention the Timer's
 * subscriber lookup uses.
 *
 * Safe from any thread. Scanning from 0 every time is fine at these
 * sizes and keeps the common case (slot 0 free) at one atomic op.
 */
static inline size_t nn20clock_reqpool_claim(NN20ClockReqPool *pool)
{
    for (size_t i = 0; i < pool->count; i++) {
        if (!atomic_exchange_explicit(&pool->in_use[i], true,
                                      memory_order_acq_rel)) {
            return i;
        }
    }
    return pool->count;
}

/* Release a slot. Call this only from the worker callback that consumed
 * it, and only after the last read of the payload. */
static inline void nn20clock_reqpool_release(NN20ClockReqPool *pool,
                                             size_t index)
{
    if (index < pool->count) {
        atomic_store_explicit(&pool->in_use[index], false,
                              memory_order_release);
    }
}

/* Slots currently claimed. For statistics and tests; by the time the
 * caller reads it, it may already be stale. */
static inline size_t nn20clock_reqpool_in_flight(const NN20ClockReqPool *pool)
{
    size_t used = 0;
    for (size_t i = 0; i < pool->count; i++) {
        if (atomic_load_explicit(&pool->in_use[i], memory_order_relaxed)) {
            used++;
        }
    }
    return used;
}

#ifdef __cplusplus
}
#endif

#endif /* NN20CLOCK_REQPOOL_H */
