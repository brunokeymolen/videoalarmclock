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
 * nn20clock_player.c - see the header.
 *
 * The decode-and-draw path is the one proven in tryout/videoplayback:
 * hardware JPEG decode straight to RGB565, then one full-frame copy to
 * the panel. What is new here is that it has to share a device with a
 * running clock, which is where the two frame buffers and the handover
 * to LVGL come from.
 */
#include "nn20clock_player.h"

#include <fcntl.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "driver/jpeg_decode.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "nn20clock_avi.h"
#include "nn20clock_ringbuf.h"

static const char *TAG = "NN20CLOCK_PLAYER";

/*
 * ------------------------------------------------------------------
 * Playback cost measurement
 * ------------------------------------------------------------------
 *
 * OFF. Set to 1 and rebuild to turn it back on.
 *
 * This is what found the SD throughput bug (design section 19): it
 * reports, once at the end of each playback, where every frame's 66 ms
 * went and how fast the reader actually read. It is kept rather than
 * deleted because that question comes back every time the media format,
 * the card, or the read path changes - and re-deriving the
 * instrumentation is more work than carrying it.
 *
 * It is off by default because it is not free. Enabled, it calls
 * esp_timer_get_time() four times per frame and twice per read - around
 * seventy times a second during playback - on the same threads that are
 * trying to hit a 15 fps deadline. Small, but paid forever for a number
 * nobody is reading.
 *
 * `frames` is deliberately NOT behind this switch: the playback loop
 * uses it to detect a pass that drew nothing, which is a real guard
 * against a busy-wait, not a measurement.
 *
 * Read the output with tools/idf.sh -p <port> monitor.
 */
#define NN20CLOCK_PLAYER_MEASURE 0

#if NN20CLOCK_PLAYER_MEASURE
#define MEASURE_NOW()           esp_timer_get_time()
#define MEASURE_INC(field)      do { pthis->cost.field++; } while (0)
#define MEASURE_ADD(field, v)   do { pthis->cost.field += (uint64_t)(v); } \
                                while (0)
#else
/*
 * The casts to void are what keep the timestamps from becoming
 * unused variables when this is off; the compiler then folds the
 * arithmetic away entirely.
 */
#define MEASURE_NOW()           0
#define MEASURE_INC(field)      ((void)0)
#define MEASURE_ADD(field, v)   ((void)(v))
#endif

/*
 * The largest JPEG frame accepted, from the tryout. A 720x720 frame
 * runs to a few tens of kilobytes; anything approaching a megabyte is
 * not a frame this device was meant to play, and reading it would cost
 * more than skipping it.
 */
#define MAX_JPEG_BYTES (900u * 1024u)

/* One full 720x720 RGB565 frame. */
#define FRAME_BYTES \
    ((size_t)NN20CLOCK_DISPLAY_WIDTH * NN20CLOCK_DISPLAY_HEIGHT * 2u)

/*
 * Two of them, used in turn. The copy to the panel runs on DMA after
 * the draw call returns, so decoding the next frame into the buffer
 * that is still being read would tear the picture.
 */
#define FRAME_BUFFERS 2u

/* Audio is handed to the codec in pieces this size. */
#define AUDIO_CHUNK_BYTES (16u * 1024u)

/* How much tone to generate per pass through the loop. Short enough
 * that stopping feels immediate, long enough not to matter. */
#define TONE_SLICE_MS 200u

/*
 * How long the panel handover waits for a finger to leave the glass
 * before going ahead regardless. Long enough for any real press, short
 * enough that a controller stuck reporting a touch costs a moment
 * rather than the buttons.
 */
#define RELEASE_WAIT_US (2 * 1000 * 1000)

/*
 * The read-ahead window, and the block the reader fills it in.
 *
 * The block is what makes the card fast: FATFS here has a 4096-byte
 * sector, so a read that is large and sector-aligned transfers straight
 * into our buffer, while the small unaligned reads this replaced each
 * cost a whole sector at both ends. 64 KB measured out at roughly one
 * transaction per frame instead of 8.8.
 *
 * The window has to be larger than any single chunk the player will
 * ask for, or it could wait for bytes that cannot all be present at
 * once - hence comfortably above MAX_JPEG_BYTES. At 55 KB a frame, two
 * megabytes is around thirty frames of slack, which is far more than
 * the card ever needs to catch up.
 *
 * The capacity must stay a whole number of blocks: that is what keeps
 * every fill offset aligned. See nn20clock_ringbuf.h.
 */
#define RING_BLOCK_BYTES (64u * 1024u)
#define RING_BLOCKS      32u
#define RING_BYTES       (RING_BLOCK_BYTES * RING_BLOCKS)

/* A path longer than this is not one of ours: the mount point, the
 * and a file name. See NN20CLOCK_MEDIA_PATH_MAX, which
 * this must stay at least as large as. */
#define PATH_MAX_LEN 320u

/*
 * Where a frame's time actually goes (design 17, Milestone 11:
 * "measure playback continuity").
 *
 * Four stages, and the decision each one points at if it dominates:
 *
 *   read     the card. A reader worker filling a ring ahead of the
 *            decoder would overlap this with everything else.
 *   decode   the hardware JPEG unit. Nothing overlaps this but the
 *            blit; if it alone exceeds the frame interval, no amount
 *            of buffering helps and the file is too much for the
 *            device.
 *   blit     handing the frame over. Today this BLOCKS the player for
 *            the whole copy, because the draw is synchronous - so
 *            decode and blit never overlap even though they sit on
 *            different cores.
 *   audio    the codec taking samples. This one is supposed to wait:
 *            it is what paces the stream.
 *
 * `spare` is the pacing sleep - time the player had nothing to do.
 * While it is comfortably positive the device is keeping up and
 * anything else is premature.
 *
 * Microseconds, accumulated per file and reported when playback ends.
 * Reading the clock four times a frame is nothing next to decoding one.
 */
typedef struct {
    uint32_t frames;
    /* Time the player spent with nothing to decode because the window
     * was empty. This is the number the read-ahead exists to drive to
     * zero; `read_us` below is now the reader's own time, spent on its
     * own thread while the player works. */
    uint64_t wait_us;
    /* How the card is actually asked for bytes: FATFS here has a 4096
     * byte sector, so a read that is small or starts mid-sector costs a
     * whole sector either way. Counted to size the reader's block, not
     * for its own sake. */
    uint32_t reads;
    uint32_t reads_under_512;
    uint32_t seeks;
    uint64_t bytes;
    uint64_t read_us;
    uint64_t decode_us;
    uint64_t blit_us;
    uint64_t audio_us;
    uint64_t spare_us;
} PlaybackCost;

struct NN20ClockPlayer {
    nn20_worker_ctx *worker;      /* ours, borrowed: playback runs here */
    nn20_worker_ctx *ui_worker;   /* borrowed */
    NN20ClockDisplay *display;    /* borrowed */
    NN20ClockAudio *audio;        /* borrowed */

    /* ---- owned by the player's worker. ------------------------------ */
    jpeg_decoder_handle_t jpeg;
    uint8_t *jpeg_buffer;                  /* one compressed frame */
    uint8_t *frames[FRAME_BUFFERS];        /* decoded, drawn in turn */
    unsigned next_frame;
    uint8_t *audio_buffer;
    PlaybackCost cost;

    /* The read-ahead window. Filled by the reader worker, drained by
     * this one; see nn20clock_ringbuf.h for why there is no lock. */
    nn20_worker_ctx *reader;      /* borrowed */
    uint8_t *ring_storage;
    NN20ClockRingbuf ring;
    /*
     * One past the last byte the reader will fetch: the end of the
     * frame data, as the parser found it.
     *
     * Deliberately NOT the size of the file. file_size() reports zero
     * when ftell() cannot answer, and the parser already tolerates that
     * by falling back to the size the RIFF header claims - so a reader
     * bounded by the file size can believe the file ended before it
     * began while parsing works perfectly. movi_end is the authority
     * for where the frames stop, so the reader uses it.
     */
    uint32_t stream_end;
    atomic_bool reader_halt;
    atomic_bool reader_eof;

    /*
     * Owned by the reader worker while it runs, and by this worker
     * otherwise. The handover is: parse the headers here with the
     * reader stopped, start the reader, and never touch the file again
     * until it has been stopped.
     */
    /*
     * A file descriptor, not a FILE*. See file_read(): stdio's buffer
     * was cutting every read into 128-byte pieces.
     */
    int fd;
    uint32_t file_position;   /* where `file` is, to skip needless seeks */
    /* When to stop waiting for a finger to lift; see
     * sync_video_visibility(). Zero when not waiting. */
    int64_t release_deadline_us;

    /*
     * Where the last play stopped, so a snooze resumes rather than
     * restarts. Only meaningful when resume_path matches what is being
     * played; cleared by forget_private(), which _forget_position()
     * posts here precisely so that the clearing cannot race the write
     * at the end of playback_private().
     */
    char resume_path[PATH_MAX_LEN];
    uint32_t resume_cursor;

    /* The last frame shown, shrunk. See the header. */
    uint16_t *thumbnail;
    atomic_bool thumbnail_valid;

    /* ---- owned by the UiWorker. ------------------------------------- */
    NN20ClockPlayerTouchFn touch_handler;
    void *touch_ctx;
    NN20ClockPlayerDoneFn done_handler;
    void *done_ctx;

    /* ---- read and written across threads. --------------------------- */
    atomic_bool running;
    atomic_bool playing;
    atomic_bool halt;
    /*
     * Who the panel is for, in two parts that must not be confused.
     *
     * `video_wanted` is the screen's standing wish, and it belongs to
     * the UiWorker alone - the player's task only ever reads it. It
     * used to write it as well, true at the start of every file and
     * false at the end, which threw the wish away at every film
     * boundary: buttons that were up when a film ended were still up,
     * invisibly, underneath the next film. And since LVGL does not run
     * while the player owns the panel, nothing was left that could take
     * them down again - not even their own five-second timeout. The
     * whole of the next film played with a dead screen.
     *
     * `has_video` is the player's own: a decodable file is open, so
     * there is a picture to put on the panel at all.
     *
     * The picture goes up only when both are true - see
     * video_on_panel().
     */
    atomic_bool video_wanted;
    atomic_bool has_video;

    /* Written by _play() before playback is posted, read by the worker
     * afterwards; the post is the handover. */
    char path[PATH_MAX_LEN];
    uint8_t volume;
    uint32_t fade_in_ms;
    uint8_t fade_from_volume;
    bool loop;
    uint32_t stop_after_ms;
    /* When to give up, from esp_timer_get_time(). Zero: no deadline. */
    int64_t stop_at_us;
    bool tone_fallback;
    bool remember;

    bool initialized;
};

/* ------------------------------------------------------------ touch -- */

/*
 * Runs on the UiWorker, so it reads the handler on the same thread that
 * sets it - which is the whole reason this is posted rather than called
 * from the player's task.
 */
static int notify_touch_private(nn20_worker_ctx *worker, void *user_data)
{
    (void)worker;
    NN20ClockPlayer *pthis = user_data;

    if (pthis->touch_handler != NULL) {
        pthis->touch_handler(pthis->touch_ctx);
    }
    return 0;
}

void nn20clock_player_set_touch_handler(NN20ClockPlayer *pthis,
                                        NN20ClockPlayerTouchFn handler,
                                        void *ctx)
{
    if (pthis == NULL) {
        return;
    }
    /* Called from a screen's show()/hide(), which are already on the
     * UiWorker - the same thread notify_touch_private() runs on. */
    pthis->touch_handler = handler;
    pthis->touch_ctx = ctx;
}

/*
 * Runs on the UiWorker, for the same reason notify_touch_private() does:
 * the handler is read on the thread that sets it, so a screen clearing
 * it in destroy() cannot race with playback ending.
 */
static int notify_done_private(nn20_worker_ctx *worker, void *user_data)
{
    (void)worker;
    NN20ClockPlayer *pthis = user_data;

    if (pthis->done_handler != NULL) {
        pthis->done_handler(pthis->done_ctx);
    }
    return 0;
}

void nn20clock_player_set_done_handler(NN20ClockPlayer *pthis,
                                       NN20ClockPlayerDoneFn handler,
                                       void *ctx)
{
    if (pthis == NULL) {
        return;
    }
    /* On the UiWorker, like the touch handler above. */
    pthis->done_handler = handler;
    pthis->done_ctx = ctx;
}

/* ------------------------------------------------------- file access -- */

/*
 * The AVI reader's callback: one read() per request, straight to the
 * filesystem.
 *
 * ------------------------------------------------------------------
 * Why this is not fopen/fread
 * ------------------------------------------------------------------
 *
 * It was, and that was the bottleneck. Newlib's fread does not pass a
 * large request through to the filesystem; it fills its own FILE buffer
 * and copies out of it, one bufferful at a time. ESP-IDF sizes that
 * buffer from CONFIG_FATFS_VFS_FSTAT_BLKSIZE, whose default of 0 means
 * newlib's own BLKSIZ - 128 bytes. So a 64 KB read here became five
 * hundred 128-byte reads underneath, and a 24 MB clip became something
 * like 190,000 of them.
 *
 * That also explains a measurement that had been sitting in design
 * section 19 looking mysterious: changing the size and alignment of the
 * reads *here* made no difference to throughput, because the reads that
 * actually reached the card were 128 bytes either way. The experiment
 * could not vary the thing it was trying to vary.
 *
 * Reading from the descriptor goes through the VFS to FATFS with no
 * intermediate buffer and no copy, so a 64 KB request is a 64 KB
 * request. Measured on the same 24 MB clip, same card, same 401 reads:
 * 1244 KB/s through fread, 7274 KB/s without it.
 *
 * ------------------------------------------------------------------
 * Why this is not pread() either
 * ------------------------------------------------------------------
 *
 * It was, and that is what made a long film slow down the further into
 * it you got - steadily, with no single moment to point at.
 *
 * ESP-IDF's vfs_fat_pread() is not a positional read down at the
 * filesystem. It is f_lseek(offset), f_read(), f_lseek(back to where
 * we were). The descriptor's own position therefore never moves: it
 * sits at zero for the whole file. And FatFs only walks forward from
 * the cluster it is already on when the position is ahead of zero -
 * from zero it takes the other branch and starts at the first cluster
 * of the file. So every read restarted the cluster walk from the top.
 *
 * The cost of that is linear in how far into the file the read is.
 * With 16 KB clusters and a 4096-byte sector, four minutes into a
 * 20 fps film - about 260 MB - each 64 KB block first follows some
 * 16,600 cluster links and drags roughly 68 KB of FAT sectors over the
 * bus to do it. The payload is the smaller half of the transfer, and
 * none of the walk is reusable, because the next block starts again
 * from the beginning. At the start of the file it costs nothing, which
 * is why every measurement taken on a short clip looked fine.
 *
 * read() leaves the position where the last read ended, so the walk is
 * one cluster rather than sixteen thousand, and lseek() is called only
 * when the offset really jumps - the parser reading headers, or a loop
 * going back to the top of the movi list. Seeking forward is cheap for
 * the same reason: FatFs starts from where the file already is.
 */
static size_t file_read(void *ctx, uint32_t offset, void *buffer, size_t size)
{
    NN20ClockPlayer *pthis = ctx;

    if (pthis->fd < 0) {
        return 0;
    }

    /*
     * Only when the offset is not where the last read ended, which is
     * the whole point: the reader streams forward and so almost never
     * pays for this, while the parser and a loop back to the start do.
     */
    if (offset != pthis->file_position) {
        MEASURE_INC(seeks);
        if (lseek(pthis->fd, (off_t)offset, SEEK_SET) < 0) {
            /*
             * Where the descriptor is now is anyone's guess, so make
             * the next call seek again rather than read from wherever
             * this left it. No real offset can be this, so it cannot
             * match one and skip the seek.
             */
            pthis->file_position = UINT32_MAX;
            return 0;
        }
        pthis->file_position = offset;
    }

    const ssize_t got = read(pthis->fd, buffer, size);
    if (got <= 0) {
        return 0;
    }
    pthis->file_position = offset + (uint32_t)got;

    MEASURE_INC(reads);
    if (size < 512u) {
        MEASURE_INC(reads_under_512);
    }
    MEASURE_ADD(bytes, got);
    return (size_t)got;
}

static uint32_t file_size(int fd)
{
    struct stat info;
    if (fstat(fd, &info) != 0 || info.st_size <= 0) {
        return 0;
    }
    return (uint32_t)info.st_size;
}

/* ------------------------------------------------------- read ahead -- */

/* Defined with the playback loop below; the reader needs to know when
 * the whole thing has been stopped. */
static bool halted(const NN20ClockPlayer *pthis);

/* Runs on the reader worker, for as long as a file is being streamed.
 * The only thread that touches `file` while playback is running. */
static int fill_private(nn20_worker_ctx *worker, void *user_data)
{
    (void)worker;
    NN20ClockPlayer *pthis = user_data;

    while (!atomic_load_explicit(&pthis->reader_halt, memory_order_acquire)) {
        uint32_t offset = 0;
        uint8_t *dst = NULL;
        const size_t room = nn20clock_ringbuf_writable(&pthis->ring, &offset,
                                                       &dst);

        if (offset >= pthis->stream_end) {
            atomic_store_explicit(&pthis->reader_eof, true,
                                  memory_order_release);
            break;
        }

        /*
         * Less than a block free means the player has not caught up.
         * Waiting for a whole block rather than filling the gap is what
         * keeps every read aligned - a short read here would put the
         * window's offset off a sector boundary and leave it there.
         */
        if (room < RING_BLOCK_BYTES) {
            vTaskDelay(1);
            continue;
        }

        size_t want = RING_BLOCK_BYTES;
        const uint32_t left = pthis->stream_end - offset;
        if (left < want) {
            want = left;   /* the last block of the file */
        }

        const int64_t began = MEASURE_NOW();
        const size_t got = file_read(pthis, offset, dst, want);
        MEASURE_ADD(read_us, MEASURE_NOW() - began);

        if (got == 0u) {
            atomic_store_explicit(&pthis->reader_eof, true,
                                  memory_order_release);
            break;
        }
        nn20clock_ringbuf_produced(&pthis->ring, got);
    }

    return 0;
}

/* A no-op whose only job is to run after fill_private() has returned. */
static int reader_idle_private(nn20_worker_ctx *worker, void *user_data)
{
    (void)worker;
    (void)user_data;
    return 0;
}

/*
 * Point the window at `from` and set the reader going.
 *
 * The start is aligned down to a block so that every fill after it is
 * aligned too; the player simply reads from its real offset, which the
 * window happens to hold a little before.
 */
static void reader_start(NN20ClockPlayer *pthis, uint32_t from, uint32_t to)
{
    const uint32_t aligned = from - (from % RING_BLOCK_BYTES);

    pthis->stream_end = to;

    nn20clock_ringbuf_reset(&pthis->ring, aligned);
    atomic_store_explicit(&pthis->reader_eof, false, memory_order_release);
    atomic_store_explicit(&pthis->reader_halt, false, memory_order_release);

    if (nn20_worker_post(pthis->reader, fill_private, pthis) != 0) {
        /* Nothing will fill the window, so nothing would ever be
         * playable. Say so and let the caller fall back. */
        ESP_LOGE(TAG, "the reader would not take the job");
        atomic_store_explicit(&pthis->reader_eof, true, memory_order_release);
    }
}

/*
 * Stop the reader and wait for it to let go of the file.
 *
 * The wait is a synchronous post that queues behind the fill job, so it
 * returns only once that job has returned. This worker waiting on the
 * reader is safe: the reader never waits on anything but the card, and
 * in particular never on the UiWorker or on this one.
 */
static void reader_stop(NN20ClockPlayer *pthis)
{
    atomic_store_explicit(&pthis->reader_halt, true, memory_order_release);
    (void)nn20_worker_post_sync(pthis->reader, reader_idle_private, pthis);
}

/*
 * The AVI reader callback used while streaming: served from the window,
 * never from the card.
 *
 * Waits for bytes that have not arrived yet, and gives up when the
 * reader has reached the end of the file or playback has been stopped -
 * either way the caller sees a short read, which is what it already
 * treats as the end.
 */
static size_t stream_read(void *ctx, uint32_t offset, void *buffer,
                          size_t size)
{
    NN20ClockPlayer *pthis = ctx;

    if (size > RING_BYTES) {
        /* Larger than the window can ever hold: waiting would never
         * end. The parser skips oversized chunks, so this is a
         * malformed file rather than a normal one. */
        ESP_LOGW(TAG, "a %u-byte read is larger than the window",
                 (unsigned)size);
        return 0u;
    }

    /* Everything before this read is finished with, and releasing it is
     * what gives the reader room to run ahead. */
    nn20clock_ringbuf_consume_to(&pthis->ring, offset);

    /*
     * The clock is only read once the window has actually run dry,
     * which it now almost never does. Timing every call instead would
     * be the most frequent measurement in the player - eight or nine a
     * frame - for a number that is nearly always zero.
     */
    int64_t wait_began = 0;

    for (;;) {
        if (nn20clock_ringbuf_read(&pthis->ring, offset, buffer, size)) {
            if (wait_began != 0) {
                MEASURE_ADD(wait_us, MEASURE_NOW() - wait_began);
            }
            nn20clock_ringbuf_consume_to(&pthis->ring,
                                         offset + (uint32_t)size);
            return size;
        }

        if (halted(pthis)) {
            return 0u;
        }

        if (atomic_load_explicit(&pthis->reader_eof, memory_order_acquire)) {
            /* The file ended inside this read. Hand back what there is;
             * the caller treats a short read as the end. */
            const uint32_t end = nn20clock_ringbuf_end(&pthis->ring);
            if (offset >= end) {
                return 0u;
            }
            const size_t have = (size_t)(end - offset);
            const size_t take = (have < size) ? have : size;
            if (!nn20clock_ringbuf_read(&pthis->ring, offset, buffer, take)) {
                return 0u;
            }
            nn20clock_ringbuf_consume_to(&pthis->ring,
                                         offset + (uint32_t)take);
            return take;
        }

        if (wait_began == 0) {
            wait_began = MEASURE_NOW();
        }
        vTaskDelay(1);
    }
}

/* ------------------------------------------------------------ video -- */

/*
 * Whether the picture belongs on the panel: there is one, and the
 * screen has not taken the panel for its buttons. Both halves, every
 * time - see where the two flags are declared for what happens when
 * one of them is asked on its own.
 */
static bool video_on_panel(const NN20ClockPlayer *pthis)
{
    return atomic_load_explicit(&pthis->has_video, memory_order_acquire) &&
           atomic_load_explicit(&pthis->video_wanted, memory_order_acquire);
}

/*
 * Bring the panel under the player, or give it back, to match what the
 * screen last asked for. Only the player's task calls this, so the two
 * sides of the handover cannot interleave.
 */
static void sync_video_visibility(NN20ClockPlayer *pthis)
{
    const bool wanted = video_on_panel(pthis);
    const bool active = nn20clock_display_video_active(pthis->display);

    if (wanted && !active) {
        (void)nn20clock_display_video_begin(pthis->display);
        pthis->release_deadline_us = 0;
        return;
    }
    if (wanted || !active) {
        return;
    }

    /*
     * Giving the panel back, but not while a finger is still on it.
     *
     * The screen asks for the panel because it was touched, so at this
     * moment that touch is usually still in progress. Handing over now
     * means LVGL resumes in the middle of a press and delivers a click
     * nobody made - which is how the snooze screen came to appear only
     * while the finger was down: its own opening touch closed it again
     * on release. Worse, a finger that happened to be where a button
     * lands would have snoozed the alarm by itself.
     *
     * So the handover waits for the release. The deadline is there
     * because a controller that reports a stuck press must not be able
     * to keep the buttons off the screen for good.
     */
    if (nn20clock_display_touch_is_down(pthis->display)) {
        const int64_t now = esp_timer_get_time();
        if (pthis->release_deadline_us == 0) {
            pthis->release_deadline_us = now + RELEASE_WAIT_US;
        }
        if (now < pthis->release_deadline_us) {
            return;
        }
        ESP_LOGW(TAG, "touch still down; handing the panel over anyway");
    }

    pthis->release_deadline_us = 0;
    (void)nn20clock_display_video_end(pthis->display);
}

/*
 * A touch while the player owns the panel. LVGL is not running, so this
 * is the only way the screen hears about it.
 */
static void poll_touch(NN20ClockPlayer *pthis)
{
    if (!nn20clock_display_video_active(pthis->display)) {
        return;   /* LVGL has the panel and reports touch itself */
    }
    if (nn20clock_display_video_take_touch(pthis->display)) {
        (void)nn20_worker_post(pthis->ui_worker, notify_touch_private, pthis);
    }
}

/*
 * Keep a small copy of the frame just decoded.
 *
 * Every sixth pixel in each direction, which is all a 120-pixel square
 * of a 720-pixel frame needs. Done per frame rather than at the end
 * because the frame buffers are used in turn and the one holding the
 * last picture is not worth tracking for this - the copy is 14,400
 * reads, nothing next to decoding the frame it came from.
 */
static void capture_thumbnail(NN20ClockPlayer *pthis, const uint8_t *frame)
{
    if (pthis->thumbnail == NULL || !pthis->remember) {
        /* The picture exists to remind the clock face of a snoozed
         * alarm. Media the user sat and watched has nothing to remind
         * anyone of, and must not overwrite an alarm's frame. */
        return;
    }

    const uint16_t *source = (const uint16_t *)(const void *)frame;
    const unsigned step =
        NN20CLOCK_DISPLAY_WIDTH / NN20CLOCK_PLAYER_THUMBNAIL_SIZE;

    for (unsigned y = 0; y < NN20CLOCK_PLAYER_THUMBNAIL_SIZE; y++) {
        const uint16_t *row = &source[(size_t)(y * step) *
                                      NN20CLOCK_DISPLAY_WIDTH];
        uint16_t *out = &pthis->thumbnail[(size_t)y *
                                          NN20CLOCK_PLAYER_THUMBNAIL_SIZE];
        for (unsigned x = 0; x < NN20CLOCK_PLAYER_THUMBNAIL_SIZE; x++) {
            out[x] = row[x * step];
        }
    }

    /* Published only once the pixels are all there. */
    atomic_store_explicit(&pthis->thumbnail_valid, true,
                          memory_order_release);
}

static esp_err_t draw_video_chunk(NN20ClockPlayer *pthis,
                                  const NN20ClockAviChunk *chunk)
{
    if (chunk->size > MAX_JPEG_BYTES) {
        ESP_LOGW(TAG, "skipping a %" PRIu32 "-byte frame", chunk->size);
        return ESP_OK;
    }

    const size_t got = stream_read(pthis, chunk->offset, pthis->jpeg_buffer,
                                   chunk->size);
    if (got != chunk->size) {
        return ESP_FAIL;   /* the card, or the end of a truncated file */
    }

    uint8_t *frame = pthis->frames[pthis->next_frame];

    const jpeg_decode_cfg_t decode = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        /* BGR element order: the panel is wired that way, and getting
         * this wrong swaps red and blue rather than failing. */
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
    };
    uint32_t decoded = 0;
    const int64_t decode_began = MEASURE_NOW();
    const esp_err_t err = jpeg_decoder_process(pthis->jpeg, &decode,
                                               pthis->jpeg_buffer, chunk->size,
                                               frame, FRAME_BYTES, &decoded);
    MEASURE_ADD(decode_us, MEASURE_NOW() - decode_began);
    if (err != ESP_OK) {
        /* One unreadable frame is not a reason to stop an alarm. */
        ESP_LOGW(TAG, "frame would not decode (0x%x)", (unsigned)err);
        return ESP_OK;
    }

    capture_thumbnail(pthis, frame);

    const int64_t blit_began = MEASURE_NOW();
    const esp_err_t drawn = nn20clock_display_video_draw(pthis->display, frame);
    MEASURE_ADD(blit_us, MEASURE_NOW() - blit_began);
    /* Not measurement: the playback loop uses this to notice a pass that
     * drew nothing. See the note by NN20CLOCK_PLAYER_MEASURE. */
    pthis->cost.frames++;
    if (drawn != ESP_OK) {
        /*
         * A frame that did not reach the panel is a frame missing from
         * the picture, not a reason to stop an alarm. The usual cause
         * is the UiWorker being busy for longer than a frame, and the
         * next one will land.
         */
        ESP_LOGW(TAG, "frame not drawn (0x%x)", (unsigned)drawn);
    }
    pthis->next_frame = (pthis->next_frame + 1u) % FRAME_BUFFERS;
    return ESP_OK;
}

static esp_err_t play_audio_chunk(NN20ClockPlayer *pthis,
                                  const NN20ClockAviChunk *chunk)
{
    uint32_t offset = chunk->offset;
    uint32_t remaining = chunk->size;

    while (remaining > 0u) {
        const size_t want = (remaining < AUDIO_CHUNK_BYTES)
                                ? remaining
                                : AUDIO_CHUNK_BYTES;
        const size_t got = stream_read(pthis, offset, pthis->audio_buffer,
                                       want);
        if (got == 0u) {
            return ESP_FAIL;
        }

        const int64_t write_began = MEASURE_NOW();
        const esp_err_t err = nn20clock_audio_write(pthis->audio,
                                                    pthis->audio_buffer, got);
        MEASURE_ADD(audio_us, MEASURE_NOW() - write_began);
        if (err != ESP_OK) {
            return err;
        }
        offset += (uint32_t)got;
        remaining -= (uint32_t)got;
    }
    return ESP_OK;
}

/* ------------------------------------------------------- the loop --- */

static bool halted(const NN20ClockPlayer *pthis)
{
    return atomic_load_explicit(&pthis->halt, memory_order_acquire);
}

/*
 * The sleep timer has run out.
 *
 * Separate from halted(): being stopped by hand is the user, and
 * running out of time is not, so only one of them is news to the screen
 * (see the done handler). Read on the player's own thread only, so a
 * plain field is enough.
 */
static bool expired(const NN20ClockPlayer *pthis)
{
    return pthis->stop_at_us != 0 &&
           esp_timer_get_time() >= pthis->stop_at_us;
}

/*
 * Play the movi list once. Returns ESP_OK at the end of the file, which
 * is the caller's cue to start it again.
 */
static esp_err_t play_once(NN20ClockPlayer *pthis,
                           const NN20ClockAviInfo *info,
                           uint32_t *cursor)
{
    const uint32_t interval_us = nn20clock_avi_frame_interval_us(info);

    int64_t started_us = 0;
    uint32_t frames = 0;

    while (!halted(pthis) && !expired(pthis)) {
        sync_video_visibility(pthis);
        poll_touch(pthis);

        NN20ClockAviChunk chunk;
        const esp_err_t err = nn20clock_avi_next_chunk(stream_read, pthis,
                                                       info, cursor, &chunk);
        if (err == ESP_ERR_NOT_FOUND) {
            return ESP_OK;   /* end of the file */
        }
        if (err != ESP_OK) {
            return err;
        }

        switch (chunk.kind) {
        case NN20CLOCK_AVI_CHUNK_VIDEO:
            /*
             * Frames are paced against the wall clock rather than by
             * sleeping a fixed interval, so a slow decode is absorbed
             * by the next frame instead of accumulating into drift.
             *
             * With sound, the I2S buffer draining in
             * play_audio_chunk() paces things too - it blocks at
             * exactly the sample rate - but only to within the depth of
             * that buffer. This is what the tryout did and it is kept:
             * the frame clock is the picture's, and the sound stays
             * with it rather than the other way round.
             */
            if (started_us == 0) {
                started_us = esp_timer_get_time();
            } else {
                const int64_t due =
                    started_us + ((int64_t)frames * (int64_t)interval_us);
                const int64_t now = esp_timer_get_time();
                if (due > now) {
                    MEASURE_ADD(spare_us, due - now);
                    vTaskDelay(pdMS_TO_TICKS((uint32_t)((due - now) / 1000)));
                }
            }
            frames++;

            if (video_on_panel(pthis)) {
                const esp_err_t drawn = draw_video_chunk(pthis, &chunk);
                if (drawn != ESP_OK) {
                    return drawn;
                }
            }
            /* When the screen has taken the panel for its buttons the
             * frame is skipped entirely rather than decoded and thrown
             * away: the sound is what matters then. */
            break;

        case NN20CLOCK_AVI_CHUNK_AUDIO: {
            const esp_err_t written = play_audio_chunk(pthis, &chunk);
            if (written != ESP_OK) {
                return written;
            }
            break;
        }

        case NN20CLOCK_AVI_CHUNK_OTHER:
        default:
            break;   /* an index, padding: nothing to play */
        }
    }

    return ESP_OK;
}

/* The fallback of design 13: no file, or no file this device can play. */
static void play_tone(NN20ClockPlayer *pthis)
{
    ESP_LOGI(TAG, "ringing with the built-in tone");

    /* No picture, so nothing to put on the panel whatever the screen
     * wishes: the screen draws, not us. */
    atomic_store_explicit(&pthis->has_video, false, memory_order_release);
    sync_video_visibility(pthis);

    (void)nn20clock_audio_configure(pthis->audio,
                                    NN20CLOCK_AUDIO_DEFAULT_SAMPLE_RATE,
                                    NN20CLOCK_AUDIO_DEFAULT_CHANNELS,
                                    NN20CLOCK_AUDIO_DEFAULT_BITS);

    while (!halted(pthis) && !expired(pthis)) {
        if (nn20clock_audio_tone(pthis->audio, TONE_SLICE_MS) != ESP_OK) {
            /*
             * Nothing left to try. Keep looping anyway rather than
             * returning: the alarm is still ringing as far as the rest
             * of the device is concerned, and the screen is still up.
             */
            vTaskDelay(pdMS_TO_TICKS(TONE_SLICE_MS));
        }
    }
}

/*
 * Open the file and set both outputs up for it. Any reason this cannot
 * be played is reported so the caller can fall back to the tone; none
 * of them is an error the user needs to see.
 */
static esp_err_t begin_file(NN20ClockPlayer *pthis, NN20ClockAviInfo *out_info)
{
    pthis->fd = open(pthis->path, O_RDONLY);
    if (pthis->fd < 0) {
        ESP_LOGW(TAG, "cannot open %s", pthis->path);
        return ESP_ERR_NOT_FOUND;
    }
    pthis->file_position = 0;

    const uint32_t size = file_size(pthis->fd);

    const esp_err_t parsed = nn20clock_avi_parse(file_read, pthis, size,
                                                 out_info);
    if (parsed != ESP_OK) {
        ESP_LOGW(TAG, "%s is not playable (0x%x)", pthis->path,
                 (unsigned)parsed);
        return parsed;
    }

    if (out_info->width != NN20CLOCK_DISPLAY_WIDTH ||
        out_info->height != NN20CLOCK_DISPLAY_HEIGHT) {
        /*
         * The frame is copied to the panel whole, so it has to be the
         * panel's size. Scaling is a Milestone 11 question; refusing is
         * honest, and the tone still rings.
         */
        ESP_LOGW(TAG, "%s is %" PRIu32 "x%" PRIu32 ", not %dx%d",
                 pthis->path, out_info->width, out_info->height,
                 NN20CLOCK_DISPLAY_WIDTH, NN20CLOCK_DISPLAY_HEIGHT);
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (out_info->audio_sample_rate != 0u) {
        const esp_err_t configured =
            nn20clock_audio_configure(pthis->audio,
                                      out_info->audio_sample_rate,
                                      out_info->audio_channels,
                                      out_info->audio_bits);
        if (configured != ESP_OK) {
            /* A picture with no sound is not an alarm. */
            ESP_LOGW(TAG, "%s has audio this device cannot play",
                     pthis->path);
            return configured;
        }
    }

    return ESP_OK;
}

/*
 * What the last file cost, per frame, against what it had to spend.
 *
 * Printed once at the end rather than per frame: logging inside the
 * loop would change the thing being measured.
 */
#if !NN20CLOCK_PLAYER_MEASURE

/* Measurement is off; the call site stays put so turning it back on is
 * one #define and nothing else. */
static void report_cost(const NN20ClockPlayer *pthis, uint32_t interval_us)
{
    (void)pthis;
    (void)interval_us;
}

#else

static void report_cost(const NN20ClockPlayer *pthis, uint32_t interval_us)
{
    const uint32_t frames = pthis->cost.frames;
    if (frames == 0u) {
        return;
    }

    const uint64_t wait = pthis->cost.wait_us / frames;
    const uint64_t decode = pthis->cost.decode_us / frames;
    const uint64_t blit = pthis->cost.blit_us / frames;
    const uint64_t audio = pthis->cost.audio_us / frames;
    const uint64_t spare = pthis->cost.spare_us / frames;
    /*
     * Reading is no longer in here: it happens on the reader's thread
     * while this one works, and only shows up as `wait` on the
     * occasions the window ran dry. That is the whole point of the
     * arrangement, so the two are reported apart.
     */
    const uint64_t busy = wait + decode + blit + audio;

    ESP_LOGI(TAG, "%" PRIu32 " frames at %" PRIu32 " us: wait %llu, "
                  "decode %llu, blit %llu, audio %llu = %llu us busy "
                  "(%llu us spare)",
             frames, interval_us, (unsigned long long)wait,
             (unsigned long long)decode, (unsigned long long)blit,
             (unsigned long long)audio, (unsigned long long)busy,
             (unsigned long long)spare);

    ESP_LOGI(TAG, "reader: %" PRIu32 " reads (%" PRIu32 " under 512 B), "
                  "%" PRIu32 " seeks, %llu B = %llu B/frame, %llu KB/s "
                  "while reading",
             pthis->cost.reads, pthis->cost.reads_under_512,
             pthis->cost.seeks, (unsigned long long)pthis->cost.bytes,
             (unsigned long long)(pthis->cost.bytes / frames),
             (unsigned long long)(pthis->cost.read_us > 0u
                 ? (pthis->cost.bytes * 1000u) / pthis->cost.read_us
                 : 0u));

    if (busy > interval_us) {
        ESP_LOGW(TAG, "over budget by %llu us per frame: the file asks for "
                      "more than this path delivers",
                 (unsigned long long)(busy - interval_us));
    }
}

#endif /* NN20CLOCK_PLAYER_MEASURE */

/* Runs on the player's worker, for as long as the alarm rings. */
static int playback_private(nn20_worker_ctx *worker, void *user_data)
{
    (void)worker;
    NN20ClockPlayer *pthis = user_data;

    /*
     * Here rather than in _play(): the ramp is timed from the first
     * sound, and _play() returns before the worker has picked the job
     * up. Starting it there would spend part of the fade waiting for
     * the card to open the file - which is exactly the fraction of a
     * second an alarm is quietest, and exactly what somebody asleep
     * would not hear.
     */
    if (pthis->fade_in_ms != 0u) {
        (void)nn20clock_audio_fade_volume(pthis->audio,
                                          pthis->fade_from_volume,
                                          pthis->volume, pthis->fade_in_ms);
    } else {
        (void)nn20clock_audio_set_volume(pthis->audio, pthis->volume);
    }

    memset(&pthis->cost, 0, sizeof(pthis->cost));

    NN20ClockAviInfo info;
    const bool wanted_file = (pthis->path[0] != '\0');
    esp_err_t err = wanted_file ? begin_file(pthis, &info) : ESP_ERR_NOT_FOUND;

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "playing %s", pthis->path);
        /*
         * That there is a picture, and nothing about whose the panel
         * is: that is the screen's to say. Saying it here is what used
         * to put a film over the buttons somebody was still using.
         */
        atomic_store_explicit(&pthis->has_video, true, memory_order_release);

        /*
         * Where to start: the beginning, or where a snooze interrupted
         * this same file. The position is only trusted when it names
         * the file being played and lands inside its frames - a card
         * whose contents changed under us must not send the reader off
         * into the middle of a chunk.
         */
        uint32_t cursor = info.movi_start;
        if (pthis->remember &&
            strcmp(pthis->resume_path, pthis->path) == 0 &&
            pthis->resume_cursor > info.movi_start &&
            pthis->resume_cursor < info.movi_end) {
            cursor = pthis->resume_cursor;
            ESP_LOGI(TAG, "resuming at %" PRIu32 " of %" PRIu32,
                     cursor - info.movi_start,
                     info.movi_end - info.movi_start);
        }

        /*
         * Design 13: an alarm ends when someone ends it, not when the
         * file does. Design 17's manual playback ends by itself, but
         * not before its floor - so both are this loop, and the only
         * question asked at the end of the file is whether enough time
         * has passed.
         */
        /*
         * The headers are parsed and nothing else is touching the file,
         * so the reader takes it from here. Everything below reads
         * through the window rather than from the card.
         */
        reader_start(pthis, cursor, info.movi_end);

        const int64_t began_us = esp_timer_get_time();
        uint32_t frames_before = pthis->cost.frames;

        /* The sleep timer, armed from the moment the media starts. */
        pthis->stop_at_us =
            (pthis->stop_after_ms == 0u)
                ? 0
                : began_us + ((int64_t)pthis->stop_after_ms * 1000);
        if (pthis->stop_at_us != 0) {
            ESP_LOGI(TAG, "sleep timer: %" PRIu32 " ms",
                     pthis->stop_after_ms);
        }

        while (!halted(pthis) && !expired(pthis)) {
            err = play_once(pthis, &info, &cursor);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "playback stopped early (0x%x)", (unsigned)err);
                break;
            }

            /*
             * play_once() returns ESP_OK for two different things: the
             * file ended, or someone stopped the alarm. Only the first
             * of them should rewind.
             *
             * Getting this wrong is silent and total: the rewind below
             * ran before the loop condition noticed the halt, so a
             * snooze always saved the start of the file and always
             * resumed from the beginning.
             */
            if (halted(pthis)) {
                break;
            }

            /*
             * A pass that drew nothing will draw nothing next time
             * either, and going round again as fast as the loop allows
             * is a busy-wait that starves the idle task - which is
             * exactly what a reader bounded by a zero file size
             * produced. Whatever is wrong with the file, stop.
             */
            if (pthis->cost.frames == frames_before) {
                ESP_LOGW(TAG, "a whole pass produced no frames; stopping");
                err = ESP_ERR_INVALID_STATE;
                break;
            }
            frames_before = pthis->cost.frames;

            /*
             * The media ended. An alarm goes round again; anything the
             * user chose to watch is over (design 11's sleep timer at
             * `all`, which is its default).
             */
            if (!pthis->loop) {
                ESP_LOGI(TAG, "the media ended");
                break;
            }
            if (expired(pthis)) {
                break;
            }

            /*
             * Round again from the top - a jump backwards, which the
             * window cannot serve because it only ever runs forwards.
             * Point the reader at the beginning instead. It costs one
             * stop and start per pass through the file, which at a
             * minute or more a pass is nothing.
             */
            cursor = info.movi_start;
            reader_stop(pthis);
            reader_start(pthis, cursor, info.movi_end);
        }

        /* Before the file is closed below: the reader must have let go
         * of it first. */
        reader_stop(pthis);

        /*
         * Remember where this stopped, in case it was a snooze. It
         * costs nothing when it was not: a dismissal calls
         * _forget_position(), and anything else that plays a different
         * file ignores this.
         *
         * Manual playback skips it entirely. Writing this path here
         * would evict the file a snoozed alarm is waiting to resume,
         * and the snooze would then start again from the beginning
         * with somebody else's film in the corner of the clock face.
         */
        if (pthis->remember) {
            (void)snprintf(pthis->resume_path, sizeof(pthis->resume_path),
                           "%s", pthis->path);
            pthis->resume_cursor = cursor;
            ESP_LOGI(TAG, "stopped at %" PRIu32 " of %" PRIu32,
                     cursor - info.movi_start, info.movi_end - info.movi_start);
        }

        /*
         * The file is over, so there is no picture left to hold the
         * panel with and it goes back to LVGL. The screen's wish is
         * left exactly as it was: the next film belongs to the same
         * screen, and that screen still wants what it wanted.
         */
        atomic_store_explicit(&pthis->has_video, false, memory_order_release);
        sync_video_visibility(pthis);
    }

    if (pthis->fd >= 0) {
        (void)close(pthis->fd);
        pthis->fd = -1;
    }

    /* Whatever went wrong with the file, the alarm still has to ring.
     * Manual playback asks for no tone and gets none: it stops, and the
     * screen that started it hears about it below. */
    if (err != ESP_OK && pthis->tone_fallback && !halted(pthis)) {
        play_tone(pthis);
    }

    atomic_store_explicit(&pthis->has_video, false, memory_order_release);
    atomic_store_explicit(&pthis->playing, false, memory_order_release);
    report_cost(pthis, nn20clock_avi_frame_interval_us(&info));
    ESP_LOGI(TAG, "playback finished");

    /*
     * Ending by itself is news; being stopped is not. Posted rather
     * than called, so the handler runs on the UiWorker where it was
     * set - see notify_done_private().
     */
    if (!halted(pthis)) {
        (void)nn20_worker_post(pthis->ui_worker, notify_done_private, pthis);
    }
    return 0;
}

/* --------------------------------------------------------- lifecycle -- */

NN20ClockPlayer *nn20clock_player_ctor(nn20_worker_ctx *worker,
                                       nn20_worker_ctx *reader,
                                       nn20_worker_ctx *ui_worker,
                                       NN20ClockDisplay *display,
                                       NN20ClockAudio *audio)
{
    if (worker == NULL || reader == NULL || ui_worker == NULL ||
        display == NULL) {
        ESP_LOGE(TAG, "a player needs a worker, the UiWorker and a display");
        return NULL;
    }
    if (audio == NULL) {
        /* Not fatal to construct, but worth saying: without the codec
         * there is no tone either, and design 13 has nothing left. */
        ESP_LOGE(TAG, "no audio; alarms will be silent");
        return NULL;
    }

    NN20ClockPlayer *pthis = calloc(1, sizeof(*pthis));
    if (pthis == NULL) {
        ESP_LOGE(TAG, "out of memory");
        return NULL;
    }

    /* calloc leaves this at zero, which is a perfectly valid file
     * descriptor - stdin. Every "is a file open" test here is `fd >= 0`,
     * so it has to start negative. */
    pthis->fd = -1;

    pthis->worker = worker;
    pthis->reader = reader;
    pthis->ui_worker = ui_worker;
    pthis->display = display;
    pthis->audio = audio;
    atomic_init(&pthis->running, false);
    atomic_init(&pthis->playing, false);
    atomic_init(&pthis->halt, false);
    atomic_init(&pthis->video_wanted, false);
    atomic_init(&pthis->has_video, false);
    atomic_init(&pthis->thumbnail_valid, false);
    return pthis;
}

static void release(NN20ClockPlayer *pthis)
{
    for (unsigned i = 0; i < FRAME_BUFFERS; i++) {
        free(pthis->frames[i]);
        pthis->frames[i] = NULL;
    }
    free(pthis->jpeg_buffer);
    pthis->jpeg_buffer = NULL;
    free(pthis->audio_buffer);
    pthis->audio_buffer = NULL;
    free(pthis->ring_storage);
    pthis->ring_storage = NULL;
    atomic_store_explicit(&pthis->thumbnail_valid, false,
                          memory_order_release);
    free(pthis->thumbnail);
    pthis->thumbnail = NULL;

    if (pthis->jpeg != NULL) {
        (void)jpeg_del_decoder_engine(pthis->jpeg);
        pthis->jpeg = NULL;
    }
    pthis->initialized = false;
}

esp_err_t nn20clock_player_start(NN20ClockPlayer *pthis)
{
    if (pthis == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (nn20clock_player_is_running(pthis)) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!pthis->initialized) {
        const jpeg_decode_engine_cfg_t engine = {
            .intr_priority = 1,
            .timeout_ms = 100,
        };
        esp_err_t err = jpeg_new_decoder_engine(&engine, &pthis->jpeg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "no JPEG decoder (0x%x)", (unsigned)err);
            return err;
        }

        /*
         * The decoder's own allocator: these buffers are read and
         * written by hardware, so they need the alignment and the
         * cacheable-but-syncable memory it knows about.
         */
        size_t allocated = 0;
        const jpeg_decode_memory_alloc_cfg_t input = {
            .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER,
        };
        pthis->jpeg_buffer = jpeg_alloc_decoder_mem(MAX_JPEG_BYTES, &input,
                                                    &allocated);

        const jpeg_decode_memory_alloc_cfg_t output = {
            .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
        };
        for (unsigned i = 0; i < FRAME_BUFFERS; i++) {
            pthis->frames[i] = jpeg_alloc_decoder_mem(FRAME_BYTES, &output,
                                                      &allocated);
        }

        pthis->audio_buffer = malloc(AUDIO_CHUNK_BYTES);

        /* PSRAM, and the largest thing the player holds. The card
         * reads straight into it, so it is the one buffer whose size
         * and alignment decide how fast the card goes. */
        pthis->ring_storage = heap_caps_malloc(RING_BYTES, MALLOC_CAP_SPIRAM);
        if (pthis->ring_storage != NULL) {
            (void)nn20clock_ringbuf_init(&pthis->ring, pthis->ring_storage,
                                         RING_BYTES, 0u);
        }

        /* PSRAM: the clock face reads it once a second at most, and
         * internal RAM is worth more elsewhere. */
        pthis->thumbnail = heap_caps_malloc(
            (size_t)NN20CLOCK_PLAYER_THUMBNAIL_SIZE *
                NN20CLOCK_PLAYER_THUMBNAIL_SIZE * sizeof(uint16_t),
            MALLOC_CAP_SPIRAM);

        bool complete = pthis->jpeg_buffer != NULL &&
                        pthis->audio_buffer != NULL &&
                        pthis->ring_storage != NULL &&
                        pthis->thumbnail != NULL;
        for (unsigned i = 0; i < FRAME_BUFFERS; i++) {
            complete = complete && pthis->frames[i] != NULL;
        }
        if (!complete) {
            ESP_LOGE(TAG, "out of memory for the video buffers");
            release(pthis);
            return ESP_ERR_NO_MEM;
        }

        pthis->initialized = true;
    }

    atomic_store_explicit(&pthis->running, true, memory_order_release);
    ESP_LOGI(TAG, "player ready: %u frame buffers of %u KB",
             (unsigned)FRAME_BUFFERS, (unsigned)(FRAME_BYTES / 1024u));
    return ESP_OK;
}

/* Does nothing, on purpose: see its one caller. */
static int drain_private(nn20_worker_ctx *worker, void *user_data)
{
    (void)worker;
    (void)user_data;
    return 0;
}

esp_err_t nn20clock_player_stop(NN20ClockPlayer *pthis)
{
    if (pthis == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!nn20clock_player_is_running(pthis)) {
        return ESP_OK;
    }

    (void)nn20clock_player_halt(pthis);

    /*
     * Safe here, unlike in _halt(): stopping happens at shutdown from
     * the application's thread, never from the UiWorker, so waiting for
     * the player's worker to drain cannot deadlock against the frames
     * it is posting there. The empty callback is the wait - it runs
     * once playback has returned.
     */
    (void)nn20_worker_post_sync(pthis->worker, drain_private, NULL);

    atomic_store_explicit(&pthis->running, false, memory_order_release);
    release(pthis);
    ESP_LOGI(TAG, "player stopped");
    return ESP_OK;
}

void nn20clock_player_dtor(NN20ClockPlayer *pthis)
{
    if (pthis == NULL) {
        return;
    }
    (void)nn20clock_player_stop(pthis);
    release(pthis);
    free(pthis);
}

bool nn20clock_player_is_running(const NN20ClockPlayer *pthis)
{
    return pthis != NULL &&
           atomic_load_explicit(&pthis->running, memory_order_acquire);
}

/* --------------------------------------------------------- playback -- */

esp_err_t nn20clock_player_play(NN20ClockPlayer *pthis,
                                const NN20ClockPlayerRequest *request)
{
    if (pthis == NULL || request == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!nn20clock_player_is_running(pthis)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (atomic_load_explicit(&pthis->playing, memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }

    const char *const path = request->path;
    if (path != NULL && path[0] != '\0') {
        if (strlen(path) >= sizeof(pthis->path)) {
            ESP_LOGW(TAG, "path too long; using the built-in tone");
            pthis->path[0] = '\0';
        } else {
            (void)snprintf(pthis->path, sizeof(pthis->path), "%s", path);
        }
    } else {
        pthis->path[0] = '\0';
    }
    pthis->volume = (request->volume > 100u) ? 100u : request->volume;
    pthis->fade_in_ms = request->fade_in_ms;
    pthis->fade_from_volume = (request->fade_from_volume > 100u)
                                  ? 100u
                                  : request->fade_from_volume;
    pthis->loop = request->loop;
    pthis->stop_after_ms = request->stop_after_ms;
    pthis->tone_fallback = request->tone_fallback;
    pthis->remember = request->remember;

    /* Whatever is in there belongs to the previous alarm. The first
     * frame of this one replaces it - unless this playback is not the
     * sort that keeps a frame, in which case the alarm's stays where it
     * is and untouched. */
    if (pthis->remember) {
        atomic_store_explicit(&pthis->thumbnail_valid, false,
                              memory_order_release);
    }

    atomic_store_explicit(&pthis->halt, false, memory_order_release);
    atomic_store_explicit(&pthis->playing, true, memory_order_release);

    const int posted = nn20_worker_post(pthis->worker, playback_private, pthis);
    if (posted != 0) {
        atomic_store_explicit(&pthis->playing, false, memory_order_release);
        ESP_LOGE(TAG, "the player's worker would not take the job");
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

esp_err_t nn20clock_player_halt(NN20ClockPlayer *pthis)
{
    if (pthis == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    atomic_store_explicit(&pthis->halt, true, memory_order_release);
    return ESP_OK;
}

bool nn20clock_player_is_playing(const NN20ClockPlayer *pthis)
{
    return pthis != NULL &&
           atomic_load_explicit(&pthis->playing, memory_order_acquire);
}

esp_err_t nn20clock_player_set_video_visible(NN20ClockPlayer *pthis,
                                             bool visible)
{
    if (pthis == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    /*
     * Recorded whether or not a file is open. Asking for the picture
     * used to be refused when there was none, which made the wish
     * unsettable in the one moment a screen most needs to set it -
     * before its first film has opened - and left the player to assume
     * it instead. A wish with no picture behind it simply does nothing:
     * see video_on_panel().
     */
    atomic_store_explicit(&pthis->video_wanted, visible, memory_order_release);
    return ESP_OK;
}

bool nn20clock_player_video_visible(const NN20ClockPlayer *pthis)
{
    return pthis != NULL &&
           atomic_load_explicit(&pthis->video_wanted, memory_order_acquire);
}

bool nn20clock_player_has_video(const NN20ClockPlayer *pthis)
{
    return pthis != NULL &&
           atomic_load_explicit(&pthis->has_video, memory_order_acquire);
}

/* ------------------------------------------------- across a snooze -- */

/*
 * Runs on the player's worker, which owns these two fields - see the
 * struct. Posted rather than called, for the reason in
 * _forget_position() below.
 */
static int forget_private(nn20_worker_ctx *worker, void *user_data)
{
    (void)worker;
    NN20ClockPlayer *pthis = user_data;

    pthis->resume_path[0] = '\0';
    pthis->resume_cursor = 0;

    /* Again, behind any frame captured between the public call and
     * this one. */
    atomic_store_explicit(&pthis->thumbnail_valid, false,
                          memory_order_release);
    return 0;
}

void nn20clock_player_forget_position(NN20ClockPlayer *pthis)
{
    if (pthis == NULL) {
        return;
    }

    /*
     * The frame goes here, on the caller's thread. It is one atomic
     * flag - the snapshot case the no-locking rule allows, not a lock -
     * and clearing it at once is what stops _thumbnail() handing a
     * dismissed alarm's frame to a screen built before this reaches the
     * player's worker: apply_screen() shows the new screen and only then
     * reports the state change. forget_private() clears it again,
     * behind any frame captured in between.
     */
    atomic_store_explicit(&pthis->thumbnail_valid, false,
                          memory_order_release);

    /*
     * The position cannot go the same way, and this is why it is a
     * post.
     *
     * _halt() only asks playback to stop. The worker that is stopping
     * still has to come out of play_once(), wait for the reader to let
     * go of the card, and *then* write where it got to - milliseconds
     * later, from the thread that owns these fields. Clearing them from
     * here was therefore undone a moment afterwards, every time: a
     * dismissed alarm kept its position, and the next alarm on the same
     * file resumed halfway through instead of starting at the
     * beginning.
     *
     * Posting puts the clearing behind the playback job in the same
     * queue, so it always lands after the write it has to beat.
     */
    if (nn20_worker_post(pthis->worker, forget_private, pthis) != 0) {
        /* Not written from this thread instead: that is the bug above.
         * The position stays, and says so. */
        ESP_LOGE(TAG, "the player's worker would not take the forget");
    }
}

const void *nn20clock_player_thumbnail(const NN20ClockPlayer *pthis)
{
    if (pthis == NULL) {
        return NULL;
    }
    if (!atomic_load_explicit(&pthis->thumbnail_valid, memory_order_acquire)) {
        return NULL;
    }
    return pthis->thumbnail;
}
