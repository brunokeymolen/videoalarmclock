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
 * nn20clock_audio.c - see the header.
 *
 * The pin numbers, the MCLK multiple, and the codec init order are from
 * tryout/videoplayback, where they were established on this board. They
 * are not guesses and should not be tidied.
 */
#include "nn20clock_audio.h"

#include <inttypes.h>
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "es8311.h"

static const char *TAG = "NN20CLOCK_AUDIO";

/*
 * Audio pins for this board, from the Waveshare BSP by way of
 * tryout/videoplayback. PA_ENABLE drives the little onboard amplifier
 * and is active high.
 */
#define PIN_PA_ENABLE  GPIO_NUM_53
#define PIN_I2S_MCK    GPIO_NUM_13
#define PIN_I2S_BCK    GPIO_NUM_12
#define PIN_I2S_WS     GPIO_NUM_10
#define PIN_I2S_DO     GPIO_NUM_9
#define PIN_I2S_DI     GPIO_NUM_11
#define I2S_PORT       I2S_NUM_0

/*
 * MCLK at 256x the sample rate. The codec's clock coefficient table is
 * indexed by the (MCLK, rate) pair, and 256x is the multiple that has
 * an entry for every rate we accept.
 */
#define MCLK_MULTIPLE 256u

/* The codec's I2C address on this board. */
#define ES8311_ADDR 0x18

/* The rates the coefficient table covers and a card is likely to hold. */
#define MIN_SAMPLE_RATE 8000u
#define MAX_SAMPLE_RATE 96000u

/* ------------------------------------------------------------- tone -- */

/*
 * The built-in alarm (design 13): a beep, a gap, repeat. 880 Hz is high
 * enough to carry across a room and low enough not to be shrill on a
 * small speaker.
 */
#define TONE_HZ         880.0f
#define TONE_ON_MS      400u
#define TONE_OFF_MS     300u
#define TONE_PERIOD_MS  (TONE_ON_MS + TONE_OFF_MS)

/*
 * Peak amplitude, a third of full scale. The onboard amplifier distorts
 * well before the sample format does, and a clipped square is a worse
 * alarm than a clean sine.
 */
#define TONE_AMPLITUDE 10000.0f

/* Milliseconds of tone generated per buffer. Small enough that the
 * buffer is a few kilobytes, large enough not to churn. */
#define TONE_CHUNK_MS 20u

struct NN20ClockAudio {
    NN20ClockDisplay *display;   /* borrowed; it owns the I2C bus */

    es8311_handle_t codec;
    i2s_chan_handle_t tx;

    /* The format currently configured on the codec and the I2S port. */
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits;

    /* Tone state, owned by whoever is playing. Kept across calls so a
     * tone asked for in short pieces sounds continuous. */
    int16_t *tone_buffer;
    size_t tone_buffer_bytes;
    uint32_t tone_sample;     /* samples generated since the last rewind */

    bool initialized;
    atomic_bool running;
    atomic_uint volume;       /* percent, the setting - not a ramp step */

    /*
     * A ramp in progress (see _fade_volume()). The player task starts
     * one and the player task advances it, so the numbers are plain
     * fields; `fading` is atomic because the settings screen cancels a
     * ramp from the UiWorker.
     */
    atomic_bool fading;
    uint8_t fade_from;
    uint8_t fade_to;
    uint32_t fade_ms;
    int64_t fade_began_us;
    uint8_t fade_applied;     /* the last level written to the codec */
};

/* ------------------------------------------------------------ codec -- */

static esp_err_t pa_enable(bool on)
{
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << PIN_PA_ENABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&config), TAG, "PA gpio");
    return gpio_set_level(PIN_PA_ENABLE, on ? 1 : 0);
}

/*
 * Point the codec at a sample rate. Separate from the I2S side because
 * the two have to be told the same thing and the codec is the one that
 * can refuse: its clock dividers come from a table, and a rate with no
 * entry cannot be produced at all.
 */
static esp_err_t codec_configure(NN20ClockAudio *pthis, uint32_t sample_rate)
{
    const es8311_clock_config_t clock = {
        .mclk_inverted = false,
        .sclk_inverted = false,
        .mclk_from_mclk_pin = true,
        .mclk_frequency = (int)(sample_rate * MCLK_MULTIPLE),
        .sample_frequency = (int)sample_rate,
    };

    ESP_RETURN_ON_ERROR(es8311_init(pthis->codec, &clock,
                                    ES8311_RESOLUTION_16,
                                    ES8311_RESOLUTION_16),
                        TAG, "es8311 init");
    ESP_RETURN_ON_ERROR(es8311_sample_frequency_config(
                            pthis->codec,
                            (int)(sample_rate * MCLK_MULTIPLE),
                            (int)sample_rate),
                        TAG, "es8311 rate");
    /* Nothing on this device records; leaving the microphone path up
     * would only add noise to the output. */
    ESP_RETURN_ON_ERROR(es8311_microphone_config(pthis->codec, false),
                        TAG, "es8311 mic off");
    return ESP_OK;
}

static esp_err_t apply_volume(NN20ClockAudio *pthis, uint8_t percent)
{
    if (pthis->codec == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return es8311_voice_volume_set(pthis->codec, (int)percent, NULL);
}

/* ------------------------------------------------------------ fade -- */

/*
 * Where a running ramp has got to, quantised to a whole step.
 *
 * The division is by whole steps rather than by milliseconds so that
 * the last step lands exactly on the target: a ramp of seven steps
 * reaches `fade_to` on the seventh and not a fraction below it. Only
 * the player task calls this, which is why it may read the fields
 * without ceremony.
 */
static uint8_t fade_level(const NN20ClockAudio *pthis, int64_t now_us)
{
    const int64_t elapsed_us = now_us - pthis->fade_began_us;
    if (elapsed_us <= 0) {
        return pthis->fade_from;
    }
    const uint32_t elapsed_ms = (uint32_t)(elapsed_us / 1000);
    if (elapsed_ms >= pthis->fade_ms) {
        return pthis->fade_to;
    }

    const uint32_t steps = pthis->fade_ms / NN20CLOCK_AUDIO_FADE_STEP_MS;
    const uint32_t done = elapsed_ms / NN20CLOCK_AUDIO_FADE_STEP_MS;
    if (steps == 0u) {
        return pthis->fade_to;   /* refused in _fade_volume(); belt and braces */
    }

    const int32_t span = (int32_t)pthis->fade_to - (int32_t)pthis->fade_from;
    return (uint8_t)((int32_t)pthis->fade_from +
                     ((span * (int32_t)done) / (int32_t)steps));
}

/*
 * What the codec should be playing at right now.
 *
 * Used everywhere the volume register has to be put back - starting
 * the output, and every reconfiguration, because es8311_init() resets
 * it. During a ramp the setting is not what the codec was at, and
 * restoring the setting there would undo the ramp by jumping to the
 * end of it in the middle of the first second of an alarm.
 */
static uint8_t current_gain(const NN20ClockAudio *pthis)
{
    if (atomic_load_explicit(&pthis->fading, memory_order_acquire)) {
        return fade_level(pthis, esp_timer_get_time());
    }
    return (uint8_t)atomic_load_explicit(&pthis->volume, memory_order_acquire);
}

/*
 * Move a ramp on, if it has reached its next step.
 *
 * Called from the write path rather than from a timer, for three
 * reasons: a ramp only means anything while sound is going out; both
 * things that make sound - a file's PCM and the built-in tone - come
 * through there; and it keeps the whole ramp on the player's own task,
 * so the only thing to synchronise is somebody cancelling it.
 *
 * Cheap enough to sit in that path. It is a subtraction per buffer and
 * an I2C write per step - once a second, against a frame budget of 33
 * milliseconds.
 */
static void advance_fade(NN20ClockAudio *pthis)
{
    if (!atomic_load_explicit(&pthis->fading, memory_order_acquire)) {
        return;
    }

    const int64_t now = esp_timer_get_time();
    const bool done =
        (now - pthis->fade_began_us) >= ((int64_t)pthis->fade_ms * 1000);
    const uint8_t level = fade_level(pthis, now);

    if (level != pthis->fade_applied) {
        pthis->fade_applied = level;
        (void)apply_volume(pthis, level);
    }

    if (done) {
        atomic_store_explicit(&pthis->fading, false, memory_order_release);
        ESP_LOGI(TAG, "fade-in finished at %u%%", (unsigned)level);
    } else if (!atomic_load_explicit(&pthis->fading, memory_order_acquire)) {
        /*
         * Cancelled between the check at the top and the write above -
         * a slider moved in the first seconds of an alarm. Whoever
         * cancelled it had already written the volume they wanted and
         * we have just overwritten it, so put theirs back: the last
         * word belongs to the person touching the screen, not to a
         * step of a ramp that is no longer running.
         */
        (void)apply_volume(pthis,
                           (uint8_t)atomic_load_explicit(&pthis->volume,
                                                         memory_order_acquire));
    }
}

/* -------------------------------------------------------------- i2s -- */

static i2s_std_config_t std_config(uint32_t sample_rate, uint16_t channels)
{
    const i2s_slot_mode_t slots =
        (channels == 1) ? I2S_SLOT_MODE_MONO : I2S_SLOT_MODE_STEREO;

    i2s_std_config_t config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, slots),
        .gpio_cfg = {
            .mclk = PIN_I2S_MCK,
            .bclk = PIN_I2S_BCK,
            .ws = PIN_I2S_WS,
            .dout = PIN_I2S_DO,
            .din = PIN_I2S_DI,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    config.clk_cfg.mclk_multiple = MCLK_MULTIPLE;
    return config;
}

/* ------------------------------------------------------- lifecycle -- */

NN20ClockAudio *nn20clock_audio_ctor(NN20ClockDisplay *display)
{
    if (display == NULL) {
        /* The display owns the board's I2C bus, and without it there is
         * no way to reach the codec. A silent alarm clock is worth
         * saying out loud. */
        ESP_LOGE(TAG, "no display (design 2); there will be no sound");
        return NULL;
    }

    NN20ClockAudio *pthis = calloc(1, sizeof(*pthis));
    if (pthis == NULL) {
        ESP_LOGE(TAG, "out of memory");
        return NULL;
    }

    pthis->display = display;
    pthis->sample_rate = NN20CLOCK_AUDIO_DEFAULT_SAMPLE_RATE;
    pthis->channels = NN20CLOCK_AUDIO_DEFAULT_CHANNELS;
    pthis->bits = NN20CLOCK_AUDIO_DEFAULT_BITS;
    atomic_init(&pthis->running, false);
    atomic_init(&pthis->volume, 0u);
    atomic_init(&pthis->fading, false);
    return pthis;
}

static void release(NN20ClockAudio *pthis)
{
    if (pthis->tx != NULL) {
        (void)i2s_channel_disable(pthis->tx);
        (void)i2s_del_channel(pthis->tx);
        pthis->tx = NULL;
    }
    if (pthis->codec != NULL) {
        /* Removes the device from the display's bus; the bus itself is
         * not ours to delete. */
        es8311_delete(pthis->codec);
        pthis->codec = NULL;
    }
    free(pthis->tone_buffer);
    pthis->tone_buffer = NULL;
    pthis->tone_buffer_bytes = 0;
    pthis->initialized = false;
}

void nn20clock_audio_dtor(NN20ClockAudio *pthis)
{
    if (pthis == NULL) {
        return;
    }
    (void)nn20clock_audio_stop(pthis);
    release(pthis);
    free(pthis);
}

esp_err_t nn20clock_audio_start(NN20ClockAudio *pthis)
{
    if (pthis == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (nn20clock_audio_is_running(pthis)) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = pa_enable(true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "amplifier would not enable (0x%x)", (unsigned)err);
        return err;
    }

    if (!pthis->initialized) {
        /* One chunk of tone, allocated once. Generating the fallback
         * alarm is not the moment to discover the heap is full. */
        pthis->tone_buffer_bytes =
            (size_t)((NN20CLOCK_AUDIO_DEFAULT_SAMPLE_RATE * TONE_CHUNK_MS) / 1000u)
            * NN20CLOCK_AUDIO_DEFAULT_CHANNELS * sizeof(int16_t);
        pthis->tone_buffer = malloc(pthis->tone_buffer_bytes);
        if (pthis->tone_buffer == NULL) {
            ESP_LOGE(TAG, "out of memory for the tone buffer");
            release(pthis);
            return ESP_ERR_NO_MEM;
        }

        /* Fetched now rather than at construction: the bus is created
         * when the display starts, which is after this object exists. */
        i2c_master_bus_handle_t bus =
            nn20clock_display_i2c_bus(pthis->display);
        if (bus == NULL) {
            ESP_LOGE(TAG, "the display has no I2C bus; no sound");
            release(pthis);
            return ESP_ERR_INVALID_STATE;
        }

        pthis->codec = es8311_create(bus, ES8311_ADDR);
        if (pthis->codec == NULL) {
            ESP_LOGE(TAG, "ES8311 not found at 0x%02x", ES8311_ADDR);
            release(pthis);
            return ESP_ERR_NOT_FOUND;
        }

        err = codec_configure(pthis, pthis->sample_rate);
        if (err != ESP_OK) {
            release(pthis);
            return err;
        }

        /* auto_clear writes silence when the buffer runs dry, rather
         * than repeating whatever was last in it - which is a buzz. */
        i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT,
                                                               I2S_ROLE_MASTER);
        channel.auto_clear = true;
        err = i2s_new_channel(&channel, &pthis->tx, NULL);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "no I2S channel (0x%x)", (unsigned)err);
            release(pthis);
            return err;
        }

        const i2s_std_config_t std = std_config(pthis->sample_rate,
                                                pthis->channels);
        err = i2s_channel_init_std_mode(pthis->tx, &std);
        if (err == ESP_OK) {
            err = i2s_channel_enable(pthis->tx);
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "I2S would not start (0x%x)", (unsigned)err);
            release(pthis);
            return err;
        }

        pthis->initialized = true;
    } else {
        err = i2s_channel_enable(pthis->tx);
        if (err != ESP_OK) {
            return err;
        }
    }

    atomic_store_explicit(&pthis->running, true, memory_order_release);

    /* The stored default arrives from the settings; until then the
     * codec is where a previous run left it. */
    (void)apply_volume(pthis, current_gain(pthis));

    ESP_LOGI(TAG, "audio up: %" PRIu32 " Hz, %u-bit, %u channel(s)",
             pthis->sample_rate, (unsigned)pthis->bits,
             (unsigned)pthis->channels);
    return ESP_OK;
}

esp_err_t nn20clock_audio_stop(NN20ClockAudio *pthis)
{
    if (pthis == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!nn20clock_audio_is_running(pthis)) {
        return ESP_OK;
    }

    atomic_store_explicit(&pthis->running, false, memory_order_release);

    if (pthis->tx != NULL) {
        (void)i2s_channel_disable(pthis->tx);
    }
    /* After the output, so the amplifier is not switched off with a
     * half-written buffer still draining into it. */
    (void)pa_enable(false);

    ESP_LOGI(TAG, "audio stopped");
    return ESP_OK;
}

bool nn20clock_audio_is_running(const NN20ClockAudio *pthis)
{
    return pthis != NULL &&
           atomic_load_explicit(&pthis->running, memory_order_acquire);
}

/* -------------------------------------------------------- playback -- */

esp_err_t nn20clock_audio_configure(NN20ClockAudio *pthis,
                                    uint32_t sample_rate,
                                    uint16_t channels,
                                    uint16_t bits)
{
    if (pthis == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!nn20clock_audio_is_running(pthis)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (bits != 16 || channels < 1 || channels > 2 ||
        sample_rate < MIN_SAMPLE_RATE || sample_rate > MAX_SAMPLE_RATE) {
        ESP_LOGW(TAG, "cannot play %" PRIu32 " Hz %u-bit %u-channel audio",
                 sample_rate, (unsigned)bits, (unsigned)channels);
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* Rewind the tone whether or not the format changes: a caller
     * asking for a format is starting something new. */
    pthis->tone_sample = 0;

    if (sample_rate == pthis->sample_rate && channels == pthis->channels &&
        bits == pthis->bits) {
        return ESP_OK;
    }

    /* The channel has to be idle to be reconfigured. Anything still in
     * its buffer is dropped, which is right: what follows is a
     * different stream. */
    ESP_RETURN_ON_ERROR(i2s_channel_disable(pthis->tx), TAG, "i2s disable");

    const i2s_std_config_t std = std_config(sample_rate, channels);
    esp_err_t err = i2s_channel_reconfig_std_clock(pthis->tx, &std.clk_cfg);
    if (err == ESP_OK) {
        err = i2s_channel_reconfig_std_slot(pthis->tx, &std.slot_cfg);
    }
    if (err == ESP_OK) {
        err = codec_configure(pthis, sample_rate);
    }

    if (err != ESP_OK) {
        /* Put back what was working, so a file this device cannot play
         * costs that file and not the rest of the alarm. */
        ESP_LOGW(TAG, "%" PRIu32 " Hz refused (0x%x); staying at %" PRIu32 " Hz",
                 sample_rate, (unsigned)err, pthis->sample_rate);
        const i2s_std_config_t previous = std_config(pthis->sample_rate,
                                                     pthis->channels);
        (void)i2s_channel_reconfig_std_clock(pthis->tx, &previous.clk_cfg);
        (void)i2s_channel_reconfig_std_slot(pthis->tx, &previous.slot_cfg);
        (void)codec_configure(pthis, pthis->sample_rate);
        (void)i2s_channel_enable(pthis->tx);
        (void)apply_volume(pthis, current_gain(pthis));
        return ESP_ERR_NOT_SUPPORTED;
    }

    pthis->sample_rate = sample_rate;
    pthis->channels = channels;
    pthis->bits = bits;

    ESP_RETURN_ON_ERROR(i2s_channel_enable(pthis->tx), TAG, "i2s enable");

    /* es8311_init() resets the volume register along with everything
     * else, so it has to be put back after every reconfiguration -
     * where a ramp has got to, not the setting it is heading for. */
    (void)apply_volume(pthis, current_gain(pthis));

    ESP_LOGI(TAG, "audio format: %" PRIu32 " Hz, %u-bit, %u channel(s)",
             sample_rate, (unsigned)bits, (unsigned)channels);
    return ESP_OK;
}

esp_err_t nn20clock_audio_write(NN20ClockAudio *pthis,
                                const void *samples, size_t bytes)
{
    if (pthis == NULL || samples == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!nn20clock_audio_is_running(pthis)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (bytes == 0) {
        return ESP_OK;
    }

    /* Before the buffer goes out, so a step lands on sound that has not
     * been written yet rather than a buffer's worth after it. */
    advance_fade(pthis);

    size_t written = 0;
    const esp_err_t err = i2s_channel_write(pthis->tx, samples, bytes,
                                            &written, portMAX_DELAY);
    if (err != ESP_OK) {
        return err;
    }
    if (written != bytes) {
        /* With portMAX_DELAY this should not happen; if it does, the
         * sound is already wrong and saying so beats guessing. */
        ESP_LOGW(TAG, "short I2S write: %u of %u bytes",
                 (unsigned)written, (unsigned)bytes);
    }
    return ESP_OK;
}

esp_err_t nn20clock_audio_tone(NN20ClockAudio *pthis, uint32_t duration_ms)
{
    if (pthis == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!nn20clock_audio_is_running(pthis) || pthis->tone_buffer == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const uint32_t rate = pthis->sample_rate;
    const uint16_t channels = pthis->channels;
    const size_t frames_per_chunk =
        pthis->tone_buffer_bytes / (channels * sizeof(int16_t));
    if (frames_per_chunk == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t remaining_frames = (uint32_t)(((uint64_t)duration_ms * rate) / 1000u);

    while (remaining_frames > 0) {
        const size_t frames = (remaining_frames < frames_per_chunk)
                                  ? remaining_frames
                                  : frames_per_chunk;

        for (size_t i = 0; i < frames; i++) {
            const uint32_t position = pthis->tone_sample + (uint32_t)i;
            /* Where we are in the beep-gap cycle, in milliseconds. */
            const uint32_t ms = (uint32_t)(((uint64_t)position * 1000u) / rate);
            const bool sounding = (ms % TONE_PERIOD_MS) < TONE_ON_MS;

            int16_t sample = 0;
            if (sounding) {
                const float phase = 2.0f * (float)M_PI * TONE_HZ *
                                    ((float)position / (float)rate);
                sample = (int16_t)(TONE_AMPLITUDE * sinf(phase));
            }
            for (uint16_t c = 0; c < channels; c++) {
                pthis->tone_buffer[(i * channels) + c] = sample;
            }
        }

        pthis->tone_sample += (uint32_t)frames;
        remaining_frames -= (uint32_t)frames;

        const esp_err_t err = nn20clock_audio_write(
            pthis, pthis->tone_buffer, frames * channels * sizeof(int16_t));
        if (err != ESP_OK) {
            return err;
        }
    }

    return ESP_OK;
}

/* ---------------------------------------------------------- volume -- */

esp_err_t nn20clock_audio_set_volume(NN20ClockAudio *pthis, uint8_t percent)
{
    if (pthis == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (percent > 100u) {
        percent = 100u;
    }

    /*
     * Setting the volume by hand ends any ramp: somebody has just said
     * what they want it to be, and a fade-in that carried on would
     * take it away from them a second later. This is what makes the
     * slider work during the first seconds of an alarm.
     */
    atomic_store_explicit(&pthis->fading, false, memory_order_release);
    atomic_store_explicit(&pthis->volume, percent, memory_order_release);

    if (pthis->codec == NULL) {
        /* Remembered, and applied when the codec comes up. */
        return ESP_OK;
    }
    return apply_volume(pthis, percent);
}

esp_err_t nn20clock_audio_fade_volume(NN20ClockAudio *pthis, uint8_t from,
                                      uint8_t to, uint32_t ms)
{
    if (pthis == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (from > 100u) {
        from = 100u;
    }
    if (to > 100u) {
        to = 100u;
    }

    /*
     * The target is the volume from this moment on, ramp or no ramp.
     * The levels on the way up are the codec's register and nothing
     * else: _volume() must not report one, because the app reads it
     * back as a setting when storage will not answer, and a fade-in
     * caught at its first step would be recorded as somebody's
     * preference for 15 percent.
     */
    atomic_store_explicit(&pthis->volume, to, memory_order_release);

    if (from >= to || ms < NN20CLOCK_AUDIO_FADE_STEP_MS) {
        /* Not a fade-in. See the header: the volume simply goes there. */
        atomic_store_explicit(&pthis->fading, false, memory_order_release);
        if (pthis->codec == NULL) {
            return ESP_OK;
        }
        return apply_volume(pthis, to);
    }

    pthis->fade_from = from;
    pthis->fade_to = to;
    pthis->fade_ms = ms;
    pthis->fade_began_us = esp_timer_get_time();
    pthis->fade_applied = from;
    atomic_store_explicit(&pthis->fading, true, memory_order_release);

    ESP_LOGI(TAG, "fading in from %u%% to %u%% over %" PRIu32 " ms",
             (unsigned)from, (unsigned)to, ms);

    if (pthis->codec == NULL) {
        /* Remembered, and applied when the codec comes up - by which
         * time the ramp will have moved on, which current_gain() knows. */
        return ESP_OK;
    }
    return apply_volume(pthis, from);
}

uint8_t nn20clock_audio_volume(const NN20ClockAudio *pthis)
{
    if (pthis == NULL) {
        return 0;
    }
    return (uint8_t)atomic_load_explicit(&pthis->volume, memory_order_acquire);
}
