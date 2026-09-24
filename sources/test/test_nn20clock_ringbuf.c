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
 * Component tests for the sliding window (design 17, Milestone 11).
 *
 * All of these run single-threaded, which is deliberate: the concurrency
 * story is one producer and one consumer with no lock, and what actually
 * breaks in that arrangement is the offset arithmetic - a window that
 * wraps mid-read, a read that straddles the join, a consumer that falls
 * far enough behind to be overwritten. Those are all reachable from one
 * thread by calling the two sides in the order a race would produce, and
 * they are unreachable from a test that just runs two threads and hopes.
 */
#include "test_util.h"

#include "nn20clock_ringbuf.h"

#include <stdint.h>
#include <string.h>

/* Small and not a power of two, so every test exercises the wrap and
 * nothing accidentally passes because the modulus was a mask. */
#define CAPACITY 100u

static uint8_t g_storage[CAPACITY];

/* A stand-in file: byte at offset o has value o. Any read can then be
 * checked against its own offsets rather than a golden buffer. */
static void fill_from_file(NN20ClockRingbuf *ring, size_t bytes)
{
    while (bytes > 0u) {
        uint32_t offset = 0;
        uint8_t *dst = NULL;
        const size_t room = nn20clock_ringbuf_writable(ring, &offset, &dst);
        REQUIRE(dst != NULL);
        if (room == 0u) {
            return;   /* full */
        }

        const size_t take = (bytes < room) ? bytes : room;
        for (size_t i = 0; i < take; i++) {
            dst[i] = (uint8_t)((offset + i) & 0xFFu);
        }
        nn20clock_ringbuf_produced(ring, take);
        bytes -= take;
    }
}

static void check_bytes(const uint8_t *got, uint32_t offset, size_t size)
{
    for (size_t i = 0; i < size; i++) {
        CHECK_EQ((int)((offset + i) & 0xFFu), (int)got[i]);
    }
}

/* ------------------------------------------------------------ basics -- */

TEST(init_rejects_nonsense)
{
    NN20ClockRingbuf ring;
    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_ringbuf_init(NULL, g_storage, CAPACITY, 0u));
    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_ringbuf_init(&ring, NULL, CAPACITY, 0u));
    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_ringbuf_init(&ring, g_storage, 0u, 0u));
}

TEST(a_fresh_window_is_empty_at_its_start_offset)
{
    NN20ClockRingbuf ring;
    REQUIRE(nn20clock_ringbuf_init(&ring, g_storage, CAPACITY, 4096u)
            == ESP_OK);

    CHECK_EQ(4096u, nn20clock_ringbuf_start(&ring));
    CHECK_EQ(4096u, nn20clock_ringbuf_end(&ring));
    CHECK_EQ(0u, nn20clock_ringbuf_available(&ring));

    uint32_t offset = 0;
    uint8_t *dst = NULL;
    CHECK_EQ(CAPACITY, nn20clock_ringbuf_writable(&ring, &offset, &dst));
    /* The first fill starts where the window was told to start, which is
     * what keeps every read sector-aligned. */
    CHECK_EQ(4096u, offset);
}

/* ------------------------------------------------------------ reading -- */

TEST(a_read_inside_the_window_returns_the_bytes)
{
    NN20ClockRingbuf ring;
    REQUIRE(nn20clock_ringbuf_init(&ring, g_storage, CAPACITY, 0u) == ESP_OK);
    fill_from_file(&ring, 40u);

    uint8_t got[10];
    CHECK(nn20clock_ringbuf_read(&ring, 5u, got, sizeof(got)));
    check_bytes(got, 5u, sizeof(got));

    /* Reading does not consume: the same bytes are still there. */
    CHECK_EQ(40u, nn20clock_ringbuf_available(&ring));
    CHECK(nn20clock_ringbuf_read(&ring, 5u, got, sizeof(got)));
    check_bytes(got, 5u, sizeof(got));
}

TEST(a_read_past_what_has_arrived_fails_rather_than_returning_rubbish)
{
    NN20ClockRingbuf ring;
    REQUIRE(nn20clock_ringbuf_init(&ring, g_storage, CAPACITY, 0u) == ESP_OK);
    fill_from_file(&ring, 20u);

    uint8_t got[8];
    /* The last byte is at 19, so this asks for one that is not there. */
    CHECK(!nn20clock_ringbuf_read(&ring, 16u, got, sizeof(got)));
    /* Exactly up to the end is fine. */
    CHECK(nn20clock_ringbuf_read(&ring, 12u, got, sizeof(got)));
    check_bytes(got, 12u, sizeof(got));
}

TEST(a_read_behind_the_window_fails_rather_than_returning_rubbish)
{
    NN20ClockRingbuf ring;
    REQUIRE(nn20clock_ringbuf_init(&ring, g_storage, CAPACITY, 0u) == ESP_OK);
    fill_from_file(&ring, 60u);
    nn20clock_ringbuf_consume_to(&ring, 30u);

    uint8_t got[4];
    /* Those bytes are free for the producer to overwrite; they are gone
     * as far as anyone may rely on. */
    CHECK(!nn20clock_ringbuf_read(&ring, 29u, got, sizeof(got)));
    CHECK(nn20clock_ringbuf_read(&ring, 30u, got, sizeof(got)));
    check_bytes(got, 30u, sizeof(got));
}

TEST(a_read_larger_than_the_window_can_never_succeed)
{
    NN20ClockRingbuf ring;
    REQUIRE(nn20clock_ringbuf_init(&ring, g_storage, CAPACITY, 0u) == ESP_OK);
    fill_from_file(&ring, CAPACITY);

    uint8_t got[CAPACITY + 1u];
    /* Saying so rather than waiting forever: a caller that blocks until
     * this returns true would never come back. */
    CHECK(!nn20clock_ringbuf_read(&ring, 0u, got, sizeof(got)));
}

/* -------------------------------------------------------------- wrap -- */

TEST(a_read_straddling_the_join_is_stitched_from_both_ends)
{
    NN20ClockRingbuf ring;
    REQUIRE(nn20clock_ringbuf_init(&ring, g_storage, CAPACITY, 0u) == ESP_OK);

    /* Get the window most of the way round, then keep going so the live
     * bytes span the end of the buffer. */
    fill_from_file(&ring, CAPACITY);
    nn20clock_ringbuf_consume_to(&ring, 90u);
    fill_from_file(&ring, 50u);

    /* 95..104 sits across index 99 -> 0. */
    uint8_t got[10];
    CHECK(nn20clock_ringbuf_read(&ring, 95u, got, sizeof(got)));
    check_bytes(got, 95u, sizeof(got));
}

TEST(filling_reports_contiguous_room_so_reads_go_straight_into_the_buffer)
{
    NN20ClockRingbuf ring;
    REQUIRE(nn20clock_ringbuf_init(&ring, g_storage, CAPACITY, 0u) == ESP_OK);
    fill_from_file(&ring, 90u);
    nn20clock_ringbuf_consume_to(&ring, 90u);

    uint32_t offset = 0;
    uint8_t *dst = NULL;
    /* 10 bytes to the end of the buffer, even though 100 are free: the
     * producer reads into this pointer directly, so it must not be told
     * about room that wraps. */
    CHECK_EQ(10u, nn20clock_ringbuf_writable(&ring, &offset, &dst));
    CHECK_EQ(90u, offset);
    CHECK(dst == &g_storage[90]);

    nn20clock_ringbuf_produced(&ring, 10u);

    /* Now the rest, from the top of the buffer. */
    CHECK_EQ(90u, nn20clock_ringbuf_writable(&ring, &offset, &dst));
    CHECK_EQ(100u, offset);
    CHECK(dst == &g_storage[0]);
}

TEST(a_full_window_offers_no_room_until_the_consumer_moves)
{
    NN20ClockRingbuf ring;
    REQUIRE(nn20clock_ringbuf_init(&ring, g_storage, CAPACITY, 0u) == ESP_OK);
    fill_from_file(&ring, CAPACITY);

    uint32_t offset = 0;
    uint8_t *dst = NULL;
    CHECK_EQ(0u, nn20clock_ringbuf_writable(&ring, &offset, &dst));

    nn20clock_ringbuf_consume_to(&ring, 25u);
    CHECK_EQ(25u, nn20clock_ringbuf_writable(&ring, &offset, &dst));
    CHECK_EQ(CAPACITY, offset);
}

/* ---------------------------------------------------------- consuming -- */

TEST(consuming_never_moves_the_window_backwards_or_past_the_end)
{
    NN20ClockRingbuf ring;
    REQUIRE(nn20clock_ringbuf_init(&ring, g_storage, CAPACITY, 0u) == ESP_OK);
    fill_from_file(&ring, 50u);

    nn20clock_ringbuf_consume_to(&ring, 20u);
    CHECK_EQ(20u, nn20clock_ringbuf_start(&ring));

    /* Backwards is ignored rather than believed: the parser walks
     * forward, and a stale offset must not resurrect freed bytes. */
    nn20clock_ringbuf_consume_to(&ring, 10u);
    CHECK_EQ(20u, nn20clock_ringbuf_start(&ring));

    /* Past the end clamps: there is nothing there to release. */
    nn20clock_ringbuf_consume_to(&ring, 900u);
    CHECK_EQ(50u, nn20clock_ringbuf_start(&ring));
    CHECK_EQ(0u, nn20clock_ringbuf_available(&ring));
}

/*
 * The access pattern the player actually produces: an eight-byte header,
 * then a payload, then a skip over the pad byte - round and round, for
 * far more bytes than the window holds.
 */
TEST(the_players_walk_streams_a_file_much_larger_than_the_window)
{
    NN20ClockRingbuf ring;
    REQUIRE(nn20clock_ringbuf_init(&ring, g_storage, CAPACITY, 0u) == ESP_OK);

    uint32_t cursor = 0u;
    for (unsigned chunk = 0; chunk < 200u; chunk++) {
        const size_t payload = 7u + (chunk % 13u);

        /* The reader keeps it topped up; the player waits for what it
         * needs rather than assuming. */
        fill_from_file(&ring, CAPACITY);

        uint8_t header[8];
        REQUIRE(nn20clock_ringbuf_read(&ring, cursor, header, sizeof(header)));
        check_bytes(header, cursor, sizeof(header));
        cursor += 8u;

        uint8_t body[20];
        REQUIRE(nn20clock_ringbuf_read(&ring, cursor, body, payload));
        check_bytes(body, cursor, payload);
        cursor += (uint32_t)payload + (payload & 1u);   /* the pad byte */

        nn20clock_ringbuf_consume_to(&ring, cursor);
    }

    /* Well past the window's size, so this only passes if wrapping,
     * consuming, and refilling all agree. */
    CHECK(cursor > 10u * CAPACITY);
}

TEST(reset_throws_the_window_away_and_starts_again)
{
    NN20ClockRingbuf ring;
    REQUIRE(nn20clock_ringbuf_init(&ring, g_storage, CAPACITY, 0u) == ESP_OK);
    fill_from_file(&ring, 80u);
    nn20clock_ringbuf_consume_to(&ring, 40u);

    nn20clock_ringbuf_reset(&ring, 8192u);
    CHECK_EQ(8192u, nn20clock_ringbuf_start(&ring));
    CHECK_EQ(8192u, nn20clock_ringbuf_end(&ring));
    CHECK_EQ(0u, nn20clock_ringbuf_available(&ring));

    uint8_t got[4];
    CHECK(!nn20clock_ringbuf_read(&ring, 40u, got, sizeof(got)));
}

/*
 * The property the whole exercise depends on: with a capacity that is a
 * whole number of blocks and a start aligned to one, every fill the
 * producer is offered begins at a block boundary and is a whole block.
 *
 * That is what turns 8.8 unaligned card transactions per frame into
 * under one aligned transaction - so if this stops holding, the reason
 * for having any of this is gone.
 */
TEST(every_fill_lands_on_a_block_boundary)
{
    enum { BLOCK = 25u };   /* CAPACITY is four of these */
    NN20ClockRingbuf ring;
    /* A start that is a multiple of the block but NOT of the capacity -
     * the case that indexing from zero got wrong. */
    REQUIRE(nn20clock_ringbuf_init(&ring, g_storage, CAPACITY, 175u)
            == ESP_OK);

    uint32_t cursor = 175u;
    for (unsigned round = 0; round < 40u; round++) {
        uint32_t offset = 0;
        uint8_t *dst = NULL;
        const size_t room = nn20clock_ringbuf_writable(&ring, &offset, &dst);

        REQUIRE(room >= BLOCK);
        CHECK_EQ(0u, offset % BLOCK);        /* aligned on the card */
        CHECK_EQ(0u, room % BLOCK);          /* and a whole number of them */

        for (size_t i = 0; i < BLOCK; i++) {
            dst[i] = (uint8_t)((offset + i) & 0xFFu);
        }
        nn20clock_ringbuf_produced(&ring, BLOCK);

        /* The player keeps up, one block behind. */
        uint8_t got[BLOCK];
        REQUIRE(nn20clock_ringbuf_read(&ring, cursor, got, sizeof(got)));
        check_bytes(got, cursor, sizeof(got));
        cursor += BLOCK;
        nn20clock_ringbuf_consume_to(&ring, cursor);
    }
}

TEST(accessors_tolerate_null)
{
    uint8_t got[4];
    CHECK_EQ(0u, nn20clock_ringbuf_start(NULL));
    CHECK_EQ(0u, nn20clock_ringbuf_end(NULL));
    CHECK_EQ(0u, nn20clock_ringbuf_available(NULL));
    CHECK(!nn20clock_ringbuf_read(NULL, 0u, got, sizeof(got)));
    nn20clock_ringbuf_consume_to(NULL, 10u);
    nn20clock_ringbuf_reset(NULL, 0u);
    nn20clock_ringbuf_produced(NULL, 10u);
}

TEST_MAIN("nn20clock_ringbuf")
{
    RUN(init_rejects_nonsense);
    RUN(a_fresh_window_is_empty_at_its_start_offset);

    RUN(a_read_inside_the_window_returns_the_bytes);
    RUN(a_read_past_what_has_arrived_fails_rather_than_returning_rubbish);
    RUN(a_read_behind_the_window_fails_rather_than_returning_rubbish);
    RUN(a_read_larger_than_the_window_can_never_succeed);

    RUN(a_read_straddling_the_join_is_stitched_from_both_ends);
    RUN(filling_reports_contiguous_room_so_reads_go_straight_into_the_buffer);
    RUN(a_full_window_offers_no_room_until_the_consumer_moves);

    RUN(consuming_never_moves_the_window_backwards_or_past_the_end);
    RUN(the_players_walk_streams_a_file_much_larger_than_the_window);
    RUN(reset_throws_the_window_away_and_starts_again);
    RUN(every_fill_lands_on_a_block_boundary);
    RUN(accessors_tolerate_null);
}
