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
 * nn20clock_display.c - ST7703 panel bring-up and LVGL on the UiWorker.
 *
 * The panel sequence - backlight GPIO, MIPI PHY LDO, two-lane DSI bus,
 * DBI IO, then the ST7703 with the 720x720 60 Hz DPI timing - is the one
 * proven on this board in tryout/videoplayback. It is repeated rather
 * than shared because the tryout is deliberately independent (design
 * 16); if a third caller ever needs it, that is the moment to extract a
 * panel component, not before.
 *
 * The LVGL half is small on purpose. Draw buffers live in PSRAM, the
 * flush callback hands the buffer to the DPI panel, and lv_timer_handler
 * runs on the UiWorker via a posting esp_timer. No LVGL call happens on
 * any other thread, so there is no lock anywhere in this file.
 */
#include "nn20clock_display.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7703.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_ldo_regulator.h"
#include "driver/i2c_master.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

static const char *TAG = "NN20CLOCK_DISPLAY";

/* Board wiring, from tryout/videoplayback. */
#define PIN_LCD_RST        GPIO_NUM_27
#define PIN_LCD_BACKLIGHT  GPIO_NUM_26
/*
 * ACTIVE LOW. Measured on the board, not assumed: driving this pin high
 * turns the backlight OFF, and the panel is then pitch black while
 * everything else - DSI, LVGL, the flush path - reports success. The
 * value matches tryout/videoplayback, which is the configuration proven
 * on this hardware, and design 3 is explicit that board wiring must be
 * verified rather than copied by analogy.
 */
#define BACKLIGHT_ON_LEVEL 0

/*
 * LEDC drives the backlight so brightness is a real dimmer rather than
 * on/off. 10-bit resolution is finer than the 0-100 scale above, and
 * stays that way at this frequency: LEDC needs freq * 2^bits under its
 * source clock, and 240 * 1024 is a quarter of a megahertz.
 *
 * ------------------------------------------------------------------
 * 240 Hz, and why it is not the 5 kHz you would reach for
 * ------------------------------------------------------------------
 *
 * At 5 kHz this panel stopped dimming and went black somewhere in the
 * low forties - measured, twice, and for a while taken as the panel's
 * floor. It was not. 5 kHz is a 200 us period, so 46% duty is an 86 us
 * pulse, and the backlight's converter does not reliably start inside
 * one that short. The cliff was the drive frequency, not the hardware.
 *
 * At 240 Hz the period is 4.2 ms, so the same 46% is a 1.9 ms pulse and
 * the panel dims smoothly down to 14% - measured on this board. That is
 * what NN20CLOCK_DISPLAY_MIN_ON_BRIGHTNESS is set above.
 *
 * The trade is flicker. 240 Hz is above flicker fusion, so it is
 * invisible when looking straight at it, but PWM this slow can show as
 * a phantom-array smear when the eye moves quickly across the panel,
 * and it will beat with a camera shutter. Raising it trades dimming
 * range back: the useful floor is set by the on-pulse the converter
 * needs, so doubling the frequency roughly doubles the duty needed to
 * reach it. If the flicker turns out to matter more than the last few
 * percent of dimming, that is the dial - and the floor must be
 * re-measured after moving it.
 *
 * Active low matters here too: full brightness is duty 0 and off is
 * full duty. Getting that backwards gives a dimmer that runs in
 * reverse, which is a confusing thing to debug by eye.
 */
#define BACKLIGHT_LEDC_TIMER    LEDC_TIMER_0
#define BACKLIGHT_LEDC_CHANNEL  LEDC_CHANNEL_0
#define BACKLIGHT_LEDC_MODE     LEDC_LOW_SPEED_MODE
#define BACKLIGHT_LEDC_HZ       240
#define BACKLIGHT_LEDC_BITS     LEDC_TIMER_10_BIT
#define BACKLIGHT_DUTY_MAX      ((1u << 10) - 1u)
#define MIPI_PHY_LDO_CHAN  3
#define MIPI_PHY_LDO_MV    2500
#define BACKLIGHT_OFF_LEVEL (!BACKLIGHT_ON_LEVEL)
#define LCD_BITS_PER_PIXEL 16

/*
 * Touch, from the Waveshare BSP for this board
 * (waveshare/esp32_p4_wifi6_touch_lcd_4b). The GT911 sits on the same
 * I2C bus as the audio codec.
 *
 * Neither reset nor interrupt is wired on this board, which is why the
 * panel is polled rather than driven by an interrupt - and that suits
 * LVGL, whose input devices are read from lv_timer_handler on the
 * UiWorker. No extra thread, and no lock.
 */
#define PIN_TOUCH_SCL      GPIO_NUM_8
#define PIN_TOUCH_SDA      GPIO_NUM_7
#define TOUCH_I2C_PORT     I2C_NUM_0
#define TOUCH_I2C_HZ       400000

/*
 * LVGL redraw period. 20 ms is 50 Hz of *opportunity*, not of drawing:
 * a clock that changes once a minute leaves the handler with nothing to
 * do on almost every call, and it returns immediately. It matters at
 * Milestone 4, when a finger is dragging something.
 */
#define LVGL_TICK_PERIOD_MS 20

/*
 * Draw buffer height in lines. A full 720x720 RGB565 frame is 1 MB;
 * two 80-line buffers are 115 KB each in PSRAM, which is ample for text
 * and leaves the big allocations for the video path at Milestone 7.
 */
#define DRAW_BUFFER_LINES 80

struct NN20ClockDisplay {
    nn20_worker_ctx *ui_worker;   /* borrowed; owned by NN20ClockWorkers */

    /* ---- UiWorker-owned. -------------------------------------------- */
    esp_lcd_panel_handle_t panel;
    esp_lcd_dsi_bus_handle_t dsi_bus;
    esp_lcd_panel_io_handle_t dbi_io;
    esp_ldo_channel_handle_t mipi_phy_ldo;

    lv_display_t *lv_display;
    void *draw_buffer_a;
    void *draw_buffer_b;

    esp_timer_handle_t lvgl_timer;   /* posts to the UiWorker */

    /* The board's I2C bus. Shared with the audio codec, which
     * borrows it - see nn20clock_display_i2c_bus(). */
    i2c_master_bus_handle_t i2c_bus;
    esp_lcd_panel_io_handle_t touch_io;
    esp_lcd_touch_handle_t touch;
    lv_indev_t *lv_indev;

    /* The panel and LVGL are brought up once and released in the dtor.
     * Stopping and starting again controls refreshing and the backlight,
     * not the hardware - see start_private(). */
    bool initialized;

    /* ---- read from other threads. ----------------------------------- */
    atomic_bool running;
    /* Set while the video player owns the panel: LVGL stops redrawing
     * and touch is reduced to a flag. See _video_begin(). */
    atomic_bool video_mode;
    /* Latched: a touch happened. Cleared by whoever reads it. */
    atomic_bool video_touched;
    /* Level: a finger is on the glass right now. */
    atomic_bool video_touch_down;
    atomic_uint video_frames;
    atomic_uint video_frames_dropped;
    atomic_uint flush_count;
    atomic_uint brightness;   /* percent, as applied */
};

static esp_err_t post_result(int rc)
{
    if (rc == 0) {
        return ESP_OK;
    }
    return (rc == 1) ? ESP_ERR_TIMEOUT : ESP_ERR_INVALID_STATE;
}

/* IRAM_ATTR belongs on the definition only: repeating it here makes the
 * compiler assign the function two different sections and drop one. */
static bool on_color_trans_done(esp_lcd_panel_handle_t panel,
                                esp_lcd_dpi_panel_event_data_t *edata,
                                void *user_ctx);

/* ------------------------------------------------------------ panel -- */

/* Runs on the UiWorker. */
static esp_err_t panel_init(NN20ClockDisplay *pthis)
{
    const ledc_timer_config_t timer_config = {
        .speed_mode = BACKLIGHT_LEDC_MODE,
        .duty_resolution = BACKLIGHT_LEDC_BITS,
        .timer_num = BACKLIGHT_LEDC_TIMER,
        .freq_hz = BACKLIGHT_LEDC_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer_config), TAG,
                        "backlight timer");

    const ledc_channel_config_t channel_config = {
        .gpio_num = PIN_LCD_BACKLIGHT,
        .speed_mode = BACKLIGHT_LEDC_MODE,
        .channel = BACKLIGHT_LEDC_CHANNEL,
        .timer_sel = BACKLIGHT_LEDC_TIMER,
        .intr_type = LEDC_INTR_DISABLE,
        /* Off to start with - active low, so full duty. The panel must
         * not show a bright rectangle of noise while LVGL is still
         * starting. */
        .duty = BACKLIGHT_DUTY_MAX,
        .hpoint = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&channel_config), TAG,
                        "backlight channel");

    const esp_ldo_channel_config_t ldo_config = {
        .chan_id = MIPI_PHY_LDO_CHAN,
        .voltage_mv = MIPI_PHY_LDO_MV,
    };
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo_config,
                                                &pthis->mipi_phy_ldo),
                        TAG, "MIPI PHY LDO");

    esp_lcd_dsi_bus_config_t bus_config = ST7703_PANEL_BUS_DSI_2CH_CONFIG();
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_config, &pthis->dsi_bus), TAG,
                        "DSI bus");

    esp_lcd_dbi_io_config_t dbi_config = ST7703_PANEL_IO_DBI_CONFIG();
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(pthis->dsi_bus, &dbi_config,
                                                 &pthis->dbi_io),
                        TAG, "DBI IO");

    esp_lcd_dpi_panel_config_t dpi_config =
        ST7703_720_720_PANEL_60HZ_DPI_CONFIG(LCD_COLOR_PIXEL_FORMAT_RGB565);
    st7703_vendor_config_t vendor_config = {
        .flags = { .use_mipi_interface = 1 },
        .mipi_config = {
            .dsi_bus = pthis->dsi_bus,
            .dpi_config = &dpi_config,
        },
    };
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = LCD_BITS_PER_PIXEL,
        .vendor_config = &vendor_config,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7703(pthis->dbi_io, &panel_config,
                                                 &pthis->panel),
                        TAG, "ST7703 panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(pthis->panel), TAG, "panel reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(pthis->panel), TAG, "panel init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(pthis->panel, true), TAG,
                        "panel on");

    /* Registered before LVGL exists, but it only fires once a draw is in
     * flight, and the first of those comes from lvgl_init() below. */
    const esp_lcd_dpi_panel_event_callbacks_t callbacks = {
        .on_color_trans_done = on_color_trans_done,
    };
    ESP_RETURN_ON_ERROR(
        esp_lcd_dpi_panel_register_event_callbacks(pthis->panel, &callbacks,
                                                   pthis),
        TAG, "panel callbacks");

    ESP_LOGI(TAG, "ST7703 %dx%d RGB565 up", NN20CLOCK_DISPLAY_WIDTH,
             NN20CLOCK_DISPLAY_HEIGHT);
    return ESP_OK;
}

/* ------------------------------------------------------------ touch -- */

/*
 * Runs on the UiWorker. A touch panel that cannot be brought up is not
 * fatal - the clock still shows the time - so this reports and the
 * caller carries on.
 */
/*
 * The board's I2C bus, which the touch controller and the audio codec
 * share. It is brought up separately from either of them because both
 * need it and only one may create it: see nn20clock_display_i2c_bus().
 */
static esp_err_t i2c_bus_init(NN20ClockDisplay *pthis)
{
    const i2c_master_bus_config_t bus_config = {
        .i2c_port = TOUCH_I2C_PORT,
        .sda_io_num = PIN_TOUCH_SDA,
        .scl_io_num = PIN_TOUCH_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_config, &pthis->i2c_bus),
                        TAG, "i2c bus");
    ESP_LOGI(TAG, "I2C bus up on SCL %d / SDA %d, shared with the codec",
             (int)PIN_TOUCH_SCL, (int)PIN_TOUCH_SDA);
    return ESP_OK;
}

static esp_err_t touch_init(NN20ClockDisplay *pthis)
{
    ESP_RETURN_ON_FALSE(pthis->i2c_bus != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "no i2c bus");

    esp_lcd_panel_io_i2c_config_t io_config =
        ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    io_config.scl_speed_hz = TOUCH_I2C_HZ;
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_i2c(pthis->i2c_bus, &io_config,
                                 &pthis->touch_io),
        TAG, "touch panel io");

    const esp_lcd_touch_config_t touch_config = {
        .x_max = NN20CLOCK_DISPLAY_WIDTH,
        .y_max = NN20CLOCK_DISPLAY_HEIGHT,
        /* Not wired on this board - see the pin block above. */
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = GPIO_NUM_NC,
        .flags = {
            /* The BSP uses no swap or mirror for this panel; the touch
             * coordinates already match the display orientation. */
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };
    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_gt911(pthis->touch_io,
                                                    &touch_config,
                                                    &pthis->touch),
                        TAG, "GT911");

    ESP_LOGI(TAG, "GT911 touch up");
    return ESP_OK;
}

i2c_master_bus_handle_t nn20clock_display_i2c_bus(const NN20ClockDisplay *pthis)
{
    return pthis != NULL ? pthis->i2c_bus : NULL;
}

/*
 * Called by LVGL from lv_timer_handler, so already on the UiWorker. It
 * reads the panel over I2C, which takes a millisecond or so - short
 * enough to sit in the UI loop, and the reason the read happens here
 * rather than on a thread of its own.
 */
static void lvgl_touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    NN20ClockDisplay *pthis = lv_indev_get_user_data(indev);

    uint16_t x[1] = {0};
    uint16_t y[1] = {0};
    uint16_t strength[1] = {0};
    uint8_t count = 0;

    (void)esp_lcd_touch_read_data(pthis->touch);
    /*
     * esp_lcd_touch_get_coordinates() is marked deprecated by this
     * driver version in favour of esp_lcd_touch_get_data(), which takes
     * an array of esp_lcd_touch_data_t rather than parallel coordinate
     * arrays. It is not a drop-in, the old call still works, and the
     * warning is not an error - so the migration is left for whenever
     * the driver actually removes it, rather than done blind here.
     */
    const bool pressed = esp_lcd_touch_get_coordinates(pthis->touch, x, y,
                                                       strength, &count, 1);

    if (pressed && count > 0u) {
        data->point.x = (int32_t)x[0];
        data->point.y = (int32_t)y[0];
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        /* Only the state changes on release; LVGL keeps the last point,
         * which is what it needs to decide whether a press became a
         * click on the object it started on. */
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

/*
 * Runs on the UiWorker in place of lv_timer_handler while the player
 * owns the panel. All it answers is "did someone touch the screen" -
 * which is all the ringing screen needs to decide to show its buttons.
 */
static void video_touch_poll(NN20ClockDisplay *pthis)
{
    if (pthis->touch == NULL) {
        return;
    }

    uint16_t x[1] = {0};
    uint16_t y[1] = {0};
    uint16_t strength[1] = {0};
    uint8_t count = 0;

    (void)esp_lcd_touch_read_data(pthis->touch);
    const bool pressed = esp_lcd_touch_get_coordinates(pthis->touch, x, y,
                                                       strength, &count, 1);
    const bool down = pressed && count > 0u;

    /*
     * Two answers, not one. "Was it touched" is what wakes the screen
     * up; "is a finger on it right now" is what says when it is safe to
     * give the panel back - see nn20clock_display_touch_is_down().
     */
    if (down) {
        atomic_store_explicit(&pthis->video_touched, true,
                              memory_order_release);
    }
    atomic_store_explicit(&pthis->video_touch_down, down,
                          memory_order_release);
}

/* ------------------------------------------------------------- LVGL -- */

/*
 * The copy into the frame buffer has finished and the draw buffer can be
 * reused. Runs in the driver's interrupt context, so it does the one
 * thing it must and nothing else; lv_display_flush_ready() only sets a
 * flag.
 *
 * IRAM_ATTR because the driver requires it whenever the callback may run
 * with the flash cache disabled.
 */
static bool IRAM_ATTR on_color_trans_done(esp_lcd_panel_handle_t panel,
                                          esp_lcd_dpi_panel_event_data_t *edata,
                                          void *user_ctx)
{
    (void)panel;
    (void)edata;
    NN20ClockDisplay *pthis = user_ctx;

    /*
     * A copy can still be in flight while the display is being torn
     * down. The teardown unregisters this callback before deleting
     * anything, so reaching here with a NULL display should be
     * impossible - but this runs in interrupt context, where being
     * wrong costs a reset rather than an error, so it is checked.
     */
    if (pthis->lv_display == NULL) {
        return false;
    }

    lv_display_flush_ready(pthis->lv_display);
    return false;   /* no higher-priority task woken */
}

/*
 * Called by LVGL from lv_timer_handler, so already on the UiWorker.
 *
 * The copy is asynchronous: on this panel the driver moves the draw
 * buffer into the frame buffer with DMA2D and returns immediately, so
 * the flush is NOT finished when this returns. Telling LVGL otherwise
 * lets it start rendering into the buffer the DMA is still reading, and
 * the driver rejects the next draw with ESP_ERR_INVALID_STATE while its
 * copy semaphore is held - which shows up as every second band of the
 * screen silently failing to appear. So flush_ready() is called from
 * on_color_trans_done() instead, and only here when the draw never
 * started at all.
 */
static void lvgl_flush_cb(lv_display_t *display, const lv_area_t *area,
                          uint8_t *pixels)
{
    NN20ClockDisplay *pthis = lv_display_get_user_data(display);

    /* draw_bitmap's end coordinates are exclusive, LVGL's area is
     * inclusive - the +1s are not slack, they are the conversion. */
    const esp_err_t err = esp_lcd_panel_draw_bitmap(
        pthis->panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, pixels);

    if (err != ESP_OK) {
        /*
         * Logged rather than swallowed. A blank panel with a happily
         * rising flush count is a genuinely confusing way to spend an
         * afternoon, and the first question - "did the pixels even get
         * there?" - should be answerable from the log.
         *
         * The completion callback will not run for a draw that never
         * started, so release LVGL here or it waits for this flush
         * forever and the display freezes.
         */
        ESP_LOGE(TAG, "flush failed (0x%x) for %d,%d..%d,%d", (unsigned)err,
                 (int)area->x1, (int)area->y1, (int)area->x2, (int)area->y2);
        lv_display_flush_ready(display);
        return;
    }

    atomic_fetch_add_explicit(&pthis->flush_count, 1u, memory_order_relaxed);
}

/* LVGL's millisecond source. esp_timer_get_time is monotonic and cheap,
 * so LVGL needs no tick task of its own. */
static uint32_t lvgl_tick_cb(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* Runs on the UiWorker: the only place lv_timer_handler is ever called. */
static int lvgl_handler_private(nn20_worker_ctx *worker, void *user_data)
{
    (void)worker;
    NN20ClockDisplay *pthis = user_data;

    if (!atomic_load_explicit(&pthis->running, memory_order_acquire)) {
        return 0;
    }

    /*
     * While the player owns the panel, LVGL must not draw on it - two
     * writers to one panel is the bug that produced the missing bands
     * at Milestone 2. But touch still has to work, or the ringing
     * screen could not be dismissed, so the controller is read here
     * directly instead. Same thread, same I2C bus, no lock.
     */
    if (atomic_load_explicit(&pthis->video_mode, memory_order_acquire)) {
        video_touch_poll(pthis);
        return 0;
    }

    lv_timer_handler();
    return 0;
}

/*
 * esp_timer context. It must not touch LVGL - it only posts, and a full
 * queue means the UiWorker is still busy with the previous redraw, in
 * which case dropping this one is exactly right.
 */
static void lvgl_timer_cb(void *arg)
{
    NN20ClockDisplay *pthis = arg;
    (void)nn20_worker_post(pthis->ui_worker, lvgl_handler_private, pthis);
}

/*
 * LVGL's core state is global, not per-display. A second NN20ClockDisplay
 * - which only happens in the tests, since the device has one panel -
 * must not re-initialize it underneath the first. The lv_display object
 * and its buffers are per-instance and are released in the dtor.
 */
static bool g_lvgl_initialized;

/* Runs on the UiWorker. */
static esp_err_t lvgl_init(NN20ClockDisplay *pthis)
{
    if (!g_lvgl_initialized) {
        lv_init();
        lv_tick_set_cb(lvgl_tick_cb);
        g_lvgl_initialized = true;
    }

    const size_t buffer_bytes =
        NN20CLOCK_DISPLAY_WIDTH * DRAW_BUFFER_LINES * sizeof(uint16_t);

    /* PSRAM: 32 MB of it, and the internal heap is worth more elsewhere.
     * DMA-capable because the DSI path reads these buffers directly. */
    pthis->draw_buffer_a = heap_caps_malloc(buffer_bytes,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    pthis->draw_buffer_b = heap_caps_malloc(buffer_bytes,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    ESP_RETURN_ON_FALSE(pthis->draw_buffer_a != NULL &&
                            pthis->draw_buffer_b != NULL,
                        ESP_ERR_NO_MEM, TAG, "draw buffers (%u B each)",
                        (unsigned)buffer_bytes);

    pthis->lv_display = lv_display_create(NN20CLOCK_DISPLAY_WIDTH,
                                          NN20CLOCK_DISPLAY_HEIGHT);
    ESP_RETURN_ON_FALSE(pthis->lv_display != NULL, ESP_ERR_NO_MEM, TAG,
                        "lv_display_create");

    lv_display_set_user_data(pthis->lv_display, pthis);
    lv_display_set_color_format(pthis->lv_display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(pthis->lv_display, lvgl_flush_cb);
    /* Two partial buffers: LVGL renders into one while the panel reads
     * the other. */
    lv_display_set_buffers(pthis->lv_display, pthis->draw_buffer_a,
                           pthis->draw_buffer_b, buffer_bytes,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    /* Black, so an empty screen looks like an off screen rather than a
     * white slab in a dark bedroom (design 11's TimeUi). */
    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_black(),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(lv_screen_active(), LV_OPA_COVER, LV_PART_MAIN);

    if (pthis->touch != NULL) {
        pthis->lv_indev = lv_indev_create();
        if (pthis->lv_indev == NULL) {
            return ESP_ERR_NO_MEM;
        }
        lv_indev_set_type(pthis->lv_indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_display(pthis->lv_indev, pthis->lv_display);
        lv_indev_set_user_data(pthis->lv_indev, pthis);
        lv_indev_set_read_cb(pthis->lv_indev, lvgl_touch_read_cb);
    }

    ESP_LOGI(TAG, "LVGL %d.%d up, %u B x2 draw buffers in PSRAM, touch %s",
             LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, (unsigned)buffer_bytes,
             (pthis->lv_indev != NULL) ? "attached" : "unavailable");
    return ESP_OK;
}

/* --------------------------------------------------------- lifecycle -- */

NN20ClockDisplay *nn20clock_display_ctor(nn20_worker_ctx *ui_worker)
{
    if (ui_worker == NULL) {
        ESP_LOGE(TAG, "no UiWorker (design 4, 11)");
        return NULL;
    }

    NN20ClockDisplay *pthis = calloc(1, sizeof(*pthis));
    if (pthis == NULL) {
        ESP_LOGE(TAG, "out of memory");
        return NULL;
    }

    pthis->ui_worker = ui_worker;
    atomic_init(&pthis->running, false);
    atomic_init(&pthis->video_mode, false);
    atomic_init(&pthis->video_touched, false);
    atomic_init(&pthis->video_touch_down, false);
    atomic_init(&pthis->video_frames, 0u);
    atomic_init(&pthis->video_frames_dropped, 0u);
    atomic_init(&pthis->flush_count, 0u);
    atomic_init(&pthis->brightness, 0u);
    return pthis;
}

typedef struct {
    NN20ClockDisplay *display;
    esp_err_t result;
} StartRequest;

static int start_private(nn20_worker_ctx *worker, void *user_data)
{
    (void)worker;
    StartRequest *request = user_data;
    NN20ClockDisplay *pthis = request->display;

    /*
     * Hardware setup happens once. A stop blanks the panel and halts the
     * refresh; a start after that has nothing to re-acquire, and trying
     * would claim a second DSI bus and PHY regulator on top of the ones
     * this object already holds - which fails, and took an on-target
     * test run to notice.
     */
    if (!pthis->initialized) {
        request->result = panel_init(pthis);
        if (request->result != ESP_OK) {
            return -1;
        }

        /* The bus first, and separately: the codec needs it even if
         * the touch controller will not answer. */
        const esp_err_t bus_err = i2c_bus_init(pthis);
        if (bus_err != ESP_OK) {
            ESP_LOGE(TAG, "no I2C bus (0x%x); no touch and no sound",
                     (unsigned)bus_err);
        }

        /* Before LVGL, which attaches the input device to the display.
         * A touch panel that will not start costs the settings screens,
         * not the clock, so it is reported and not fatal. */
        const esp_err_t touch_err =
            (bus_err == ESP_OK) ? touch_init(pthis) : bus_err;
        if (touch_err != ESP_OK) {
            ESP_LOGE(TAG, "touch unavailable (0x%x); display is read-only",
                     (unsigned)touch_err);
        }

        request->result = lvgl_init(pthis);
        if (request->result != ESP_OK) {
            return -1;
        }
        pthis->initialized = true;
    } else {
        request->result = esp_lcd_panel_disp_on_off(pthis->panel, true);
        if (request->result != ESP_OK) {
            return -1;
        }
    }

    /* Set before the first handler runs, since the handler checks it. */
    atomic_store_explicit(&pthis->running, true, memory_order_release);

    /* Draw the empty black screen once before the backlight comes on,
     * so the first thing visible is the clock's background and not
     * whatever the panel powered up holding. */
    lv_timer_handler();

    return 0;
}

esp_err_t nn20clock_display_start(NN20ClockDisplay *pthis)
{
    if (pthis == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (nn20clock_display_is_running(pthis)) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Synchronous: callers may show a screen the moment this returns. */
    StartRequest request = { .display = pthis, .result = ESP_FAIL };
    const esp_err_t posted = post_result(
        nn20_worker_post_sync(pthis->ui_worker, start_private, &request));
    if (posted != ESP_OK) {
        return posted;
    }
    if (request.result != ESP_OK) {
        return request.result;
    }

    /* Started only after LVGL is up, so the first tick has something to
     * drive. */
    const esp_timer_create_args_t timer_args = {
        .callback = lvgl_timer_cb,
        .arg = pthis,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "nn20clock-lvgl",
    };
    esp_err_t err = esp_timer_create(&timer_args, &pthis->lvgl_timer);
    if (err == ESP_OK) {
        err = esp_timer_start_periodic(pthis->lvgl_timer,
                                       LVGL_TICK_PERIOD_MS * 1000);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LVGL timer would not start (0x%x)", (unsigned)err);
        atomic_store_explicit(&pthis->running, false, memory_order_release);
        return err;
    }

    /* Only now, with a frame drawn: the first thing visible is the
     * clock's background rather than whatever the panel powered up
     * holding. The caller replaces this with the stored default. */
    (void)nn20clock_display_set_brightness(pthis, 100u);

    ESP_LOGI(TAG, "display running, redraw every %d ms", LVGL_TICK_PERIOD_MS);
    return ESP_OK;
}

static int stop_private(nn20_worker_ctx *worker, void *user_data)
{
    (void)worker;
    NN20ClockDisplay *pthis = user_data;

    (void)nn20clock_display_set_brightness(pthis, 0u);
    if (pthis->panel != NULL) {
        (void)esp_lcd_panel_disp_on_off(pthis->panel, false);
    }
    return 0;
}

esp_err_t nn20clock_display_stop(NN20ClockDisplay *pthis)
{
    if (pthis == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!nn20clock_display_is_running(pthis)) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Timer first, then the flag: no handler can be posted afterwards. */
    if (pthis->lvgl_timer != NULL) {
        (void)esp_timer_stop(pthis->lvgl_timer);
        (void)esp_timer_delete(pthis->lvgl_timer);
        pthis->lvgl_timer = NULL;
    }
    atomic_store_explicit(&pthis->running, false, memory_order_release);

    const esp_err_t result = post_result(
        nn20_worker_post_sync(pthis->ui_worker, stop_private, pthis));
    ESP_LOGI(TAG, "display stopped");
    return result;
}

/*
 * Runs on the UiWorker. Releases in the reverse of the order panel_init
 * and lvgl_init acquired: LVGL's view of the display first, then the
 * panel, its IO, the bus, and finally the regulator that powers the PHY.
 */
static int teardown_private(nn20_worker_ctx *worker, void *user_data)
{
    (void)worker;
    NN20ClockDisplay *pthis = user_data;

    /*
     * The panel goes first, and its callbacks go before that.
     *
     * The DPI panel copies draw buffers asynchronously and reports
     * completion through on_color_trans_done, which dereferences the
     * LVGL display. Deleting the display while a copy is still in
     * flight leaves that callback holding freed memory - a store access
     * fault on the UI core, moments after everything logged a clean
     * shutdown. Unregistering first, then deleting the panel, means no
     * callback can arrive after the display is gone.
     */
    if (pthis->panel != NULL) {
        const esp_lcd_dpi_panel_event_callbacks_t none = {0};
        (void)esp_lcd_dpi_panel_register_event_callbacks(pthis->panel, &none,
                                                         NULL);
        (void)esp_lcd_panel_del(pthis->panel);
        pthis->panel = NULL;
    }

    if (pthis->lv_indev != NULL) {
        /* Before the display it is attached to, and before the touch
         * handle its read callback uses. */
        lv_indev_delete(pthis->lv_indev);
        pthis->lv_indev = NULL;
    }
    if (pthis->touch != NULL) {
        (void)esp_lcd_touch_del(pthis->touch);
        pthis->touch = NULL;
    }
    if (pthis->touch_io != NULL) {
        (void)esp_lcd_panel_io_del(pthis->touch_io);
        pthis->touch_io = NULL;
    }
    /* Last of the I2C users here, and the audio codec - which borrows
     * this bus - is destroyed before the display, so nothing is left
     * holding it. See VENDORED.md in nn20clock_es8311. */
    if (pthis->i2c_bus != NULL) {
        (void)i2c_del_master_bus(pthis->i2c_bus);
        pthis->i2c_bus = NULL;
    }

    if (pthis->lv_display != NULL) {
        /* Only this display goes; LVGL's core stays initialized, since
         * it is global and another instance may still be using it. */
        lv_display_delete(pthis->lv_display);
        pthis->lv_display = NULL;
    }

    /* After the display, which may still reference them. */
    free(pthis->draw_buffer_a);
    free(pthis->draw_buffer_b);
    pthis->draw_buffer_a = NULL;
    pthis->draw_buffer_b = NULL;
    if (pthis->dbi_io != NULL) {
        (void)esp_lcd_panel_io_del(pthis->dbi_io);
        pthis->dbi_io = NULL;
    }
    if (pthis->dsi_bus != NULL) {
        (void)esp_lcd_del_dsi_bus(pthis->dsi_bus);
        pthis->dsi_bus = NULL;
    }
    if (pthis->mipi_phy_ldo != NULL) {
        (void)esp_ldo_release_channel(pthis->mipi_phy_ldo);
        pthis->mipi_phy_ldo = NULL;
    }

    return 0;
}

void nn20clock_display_dtor(NN20ClockDisplay *pthis)
{
    if (pthis == NULL) {
        return;
    }
    if (nn20clock_display_is_running(pthis)) {
        (void)nn20clock_display_stop(pthis);
    }

    /*
     * The device never destroys its display - app_main holds it for the
     * life of the board - so it is tempting not to write this at all.
     * It is here because without it the DSI bus, the panel, and the PHY
     * regulator stay claimed, and the *next* display cannot start. That
     * is not hypothetical: it is what the on-target test suite does when
     * it builds a second application, and it failed exactly that way.
     */
    (void)nn20_worker_post_sync(pthis->ui_worker, teardown_private, pthis);
    free(pthis);
}

bool nn20clock_display_is_running(const NN20ClockDisplay *pthis)
{
    return pthis != NULL &&
           atomic_load_explicit(&pthis->running, memory_order_acquire);
}

esp_err_t nn20clock_display_set_brightness(NN20ClockDisplay *pthis,
                                           uint8_t percent)
{
    if (pthis == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (percent > 100u) {
        percent = 100u;
    }
    if (percent > 0u && percent < NN20CLOCK_DISPLAY_MIN_ON_BRIGHTNESS) {
        /* A panel at 1% reads as broken rather than dim, and someone who
         * wanted it dark would have chosen off. */
        percent = NN20CLOCK_DISPLAY_MIN_ON_BRIGHTNESS;
    }

    /* Active low: full brightness is zero duty. */
    const uint32_t on_duty = (BACKLIGHT_DUTY_MAX * percent) / 100u;
    const uint32_t duty = BACKLIGHT_DUTY_MAX - on_duty;

    esp_err_t err = ledc_set_duty(BACKLIGHT_LEDC_MODE, BACKLIGHT_LEDC_CHANNEL,
                                  duty);
    if (err == ESP_OK) {
        err = ledc_update_duty(BACKLIGHT_LEDC_MODE, BACKLIGHT_LEDC_CHANNEL);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "brightness %u%% failed (0x%x)", (unsigned)percent,
                 (unsigned)err);
        return err;
    }

    atomic_store_explicit(&pthis->brightness, percent, memory_order_relaxed);
    return ESP_OK;
}

uint8_t nn20clock_display_brightness(const NN20ClockDisplay *pthis)
{
    if (pthis == NULL) {
        return 0u;
    }
    return (uint8_t)atomic_load_explicit(&pthis->brightness,
                                         memory_order_relaxed);
}

esp_err_t nn20clock_display_set_backlight(NN20ClockDisplay *pthis, bool on)
{
    return nn20clock_display_set_brightness(pthis, on ? 100u : 0u);
}

/* ------------------------------------------------------------ video -- */

/*
 * Milliseconds to let the panel settle when handing it over.
 *
 * A copy into the frame buffer runs on DMA and finishes after
 * draw_bitmap has returned. Both sides of a handover start by drawing
 * the whole screen, so waiting out the last copy is what keeps the
 * first frame of the new owner from being refused - and 40 ms is
 * invisible next to switching between a video and a screen of buttons.
 */
#define VIDEO_HANDOVER_SETTLE_MS 40u

typedef struct {
    NN20ClockDisplay *display;
    const void *pixels;
    esp_err_t result;
} DrawFrameRequest;

/* Runs on the UiWorker, which owns the panel. */
static int draw_frame_private(nn20_worker_ctx *worker, void *user_data)
{
    (void)worker;
    DrawFrameRequest *request = user_data;
    NN20ClockDisplay *pthis = request->display;

    request->result = esp_lcd_panel_draw_bitmap(pthis->panel, 0, 0,
                                                NN20CLOCK_DISPLAY_WIDTH,
                                                NN20CLOCK_DISPLAY_HEIGHT,
                                                request->pixels);
    return 0;
}

/* Runs on the UiWorker: makes LVGL repaint everything it thinks is
 * already on the panel, since the video overwrote all of it. */
static int repaint_private(nn20_worker_ctx *worker, void *user_data)
{
    (void)worker;
    (void)user_data;
    lv_obj_invalidate(lv_screen_active());
    return 0;
}

esp_err_t nn20clock_display_video_begin(NN20ClockDisplay *pthis)
{
    if (pthis == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!nn20clock_display_is_running(pthis)) {
        return ESP_ERR_INVALID_STATE;
    }

    atomic_store_explicit(&pthis->video_touched, false, memory_order_release);
    atomic_store_explicit(&pthis->video_touch_down, false,
                          memory_order_release);
    atomic_store_explicit(&pthis->video_frames, 0u, memory_order_release);
    atomic_store_explicit(&pthis->video_frames_dropped, 0u,
                          memory_order_release);
    atomic_store_explicit(&pthis->video_mode, true, memory_order_release);

    /* LVGL may have a redraw in flight from the tick just before this. */
    vTaskDelay(pdMS_TO_TICKS(VIDEO_HANDOVER_SETTLE_MS));

    ESP_LOGI(TAG, "panel handed to the player");
    return ESP_OK;
}

esp_err_t nn20clock_display_video_end(NN20ClockDisplay *pthis)
{
    if (pthis == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!atomic_load_explicit(&pthis->video_mode, memory_order_acquire)) {
        return ESP_OK;
    }

    atomic_store_explicit(&pthis->video_mode, false, memory_order_release);

    /* The last frame's copy, before LVGL starts drawing over it. */
    vTaskDelay(pdMS_TO_TICKS(VIDEO_HANDOVER_SETTLE_MS));

    /* LVGL believes the panel still holds what it last drew. It does
     * not - the video replaced every pixel - so everything is marked
     * dirty and drawn again. */
    (void)nn20_worker_post_sync(pthis->ui_worker, repaint_private, pthis);

    ESP_LOGI(TAG, "panel back to LVGL after %u frames (%u dropped)",
             (unsigned)atomic_load_explicit(&pthis->video_frames,
                                            memory_order_acquire),
             (unsigned)atomic_load_explicit(&pthis->video_frames_dropped,
                                            memory_order_acquire));
    return ESP_OK;
}

bool nn20clock_display_video_active(const NN20ClockDisplay *pthis)
{
    return pthis != NULL &&
           atomic_load_explicit(&pthis->video_mode, memory_order_acquire);
}

esp_err_t nn20clock_display_video_draw(NN20ClockDisplay *pthis,
                                       const void *pixels)
{
    if (pthis == NULL || pixels == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!nn20clock_display_video_active(pthis)) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Synchronous, and on the UiWorker: the panel has one owner, and
     * the player must not draw a frame while LVGL is mid-flush. */
    DrawFrameRequest request = {
        .display = pthis,
        .pixels = pixels,
        .result = ESP_FAIL,
    };
    const esp_err_t posted = post_result(
        nn20_worker_post_sync(pthis->ui_worker, draw_frame_private, &request));
    if (posted != ESP_OK) {
        return posted;
    }

    if (request.result == ESP_ERR_INVALID_STATE) {
        /*
         * The previous frame's copy is still running. Dropping this one
         * is right: the next frame is 60-odd milliseconds away and will
         * replace it anyway, and waiting here would only push the whole
         * stream later.
         */
        atomic_fetch_add_explicit(&pthis->video_frames_dropped, 1u,
                                  memory_order_relaxed);
        return ESP_OK;
    }
    if (request.result != ESP_OK) {
        return request.result;
    }

    atomic_fetch_add_explicit(&pthis->video_frames, 1u, memory_order_relaxed);
    return ESP_OK;
}

bool nn20clock_display_touch_is_down(const NN20ClockDisplay *pthis)
{
    if (pthis == NULL) {
        return false;
    }
    return atomic_load_explicit(&pthis->video_touch_down,
                                memory_order_acquire);
}

bool nn20clock_display_video_take_touch(NN20ClockDisplay *pthis)
{
    if (pthis == NULL) {
        return false;
    }
    return atomic_exchange_explicit(&pthis->video_touched, false,
                                    memory_order_acq_rel);
}

uint32_t nn20clock_display_flush_count(const NN20ClockDisplay *pthis)
{
    if (pthis == NULL) {
        return 0u;
    }
    return atomic_load_explicit(&pthis->flush_count, memory_order_relaxed);
}
