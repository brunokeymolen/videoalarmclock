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
 * nn20clock_device_ui.c - the system settings screen (design 11).
 *
 * Everything here runs on the UiWorker: show(), destroy(), and every
 * LVGL event handler. The one exception is the scan result, which
 * arrives on the CoreWorker and is posted across - see on_scan_done().
 */
#include "nn20clock_device_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* For NN20CLOCK_DISPLAY_MIN_ON_BRIGHTNESS: the slider must not offer a
 * range the display will silently clamp. */
#include "esp_app_desc.h"
#include "nn20clock_ota.h"
#include "nn20clock_brightness.h"
#include "nn20clock_display.h"
#include "lvgl.h"

static const char *TAG = "NN20CLOCK_DEVICE_UI";

/* Generated; shared look with the alarm screen. The big digits are the
 * clock face's, reused for the slider readout below. */
LV_FONT_DECLARE(nn20clock_font_ui_bold_28);
LV_FONT_DECLARE(nn20clock_font_clock_140);

#define COLOR_TEXT      lv_color_hex(0xD8E6F2)
#define COLOR_ACCENT    lv_color_hex(0x7FC4FF)
#define COLOR_MUTED     lv_color_hex(0x4A6A85)
#define COLOR_SURFACE   lv_color_hex(0x101820)

#define HEADER_HEIGHT   96
#define ROW_HEIGHT      104
/*
 * A menu row holds a title over the value it currently has, both meant
 * to be read standing up and not leaning in: 48 px over 28 px, which is
 * what the extra height is for. The menu scrolls, and a list you scroll
 * but can read beats one that fits and you cannot.
 *
 * Its own constant rather than a taller ROW_HEIGHT: the Time screen's
 * one row is built to that and is not this shape.
 */
#define MENU_ROW_HEIGHT 124
#define BUTTON_SIZE     72

/* How far the slider readout sits below the slider itself. */
#define READOUT_DROP    120

typedef enum {
    VIEW_MENU,
    VIEW_NETWORKS,
    VIEW_PASSWORD,
    VIEW_BRIGHTNESS,
    VIEW_VOLUME,
    VIEW_TIME,
    VIEW_ABOUT
} View;

typedef struct {
    NN20ClockUiBase super;   /* first: a UiBase* casts to this */

    NN20ClockDeviceService service;
    NN20ClockNet *net;
    NN20ClockOta *ota;

    lv_obj_t *root;
    lv_obj_t *view;          /* whichever view is up */
    lv_obj_t *body;          /* its scrollable part, when it has one */

    /*
     * The big number under whichever slider is up. One field, not two:
     * only ever one of the two slider views exists at a time.
     */
    lv_obj_t *readout;

    /* The brightness screen's four value labels, and which half of the
     * schedule the clock is in - which decides whether moving a slider
     * previews on the panel or only changes the number. */
    lv_obj_t *day_value;
    lv_obj_t *night_value;
    lv_obj_t *playback_value;
    lv_obj_t *schedule_value;
    bool editing_is_day;

    /* The time screen: the switch, and the five rollers behind it that
     * only exist while the sync is off. */
    lv_obj_t *ntp_switch;
    lv_obj_t *clock_rollers;   /* container; NULL when NTP is on */
    lv_obj_t *roller_day;
    lv_obj_t *roller_month;
    lv_obj_t *roller_year;
    lv_obj_t *roller_hour;
    lv_obj_t *roller_minute;

    /* Password entry. */
    lv_obj_t *password_input;
    char pending_ssid[NN20CLOCK_WIFI_SSID_MAX];

    /* Scan results, copied from the callback. Sized like the net
     * component's list rather than assumed. */
    NN20ClockNetApList aps;
    bool scanning;

    /*
     * The About view's update controls, and whether that view is the
     * one on screen.
     *
     * The flag is what the second tick consults: an update is polled
     * rather than pushed (see nn20clock_ota.h on why), and polling a
     * view that is not there would be redrawing widgets that were
     * deleted with it.
     */
    bool about_open;
    lv_obj_t *ota_status;
    lv_obj_t *ota_bar;
    lv_obj_t *ota_button;
    lv_obj_t *ota_button_label;
    NN20ClockOtaState ota_shown;   /* what the controls last drew */

    /*
     * The config being edited. Loaded on show, written back on save -
     * so a slider drag does not hit flash on every pixel of travel.
     */
    NN20ClockConfig config;
} DeviceUi;

static void show_menu(DeviceUi *ui);
static void show_networks(DeviceUi *ui);
static void show_time(DeviceUi *ui);
static void show_about(DeviceUi *ui);

/* ---------------------------------------------------------- helpers -- */

static lv_obj_t *make_panel(lv_obj_t *parent)
{
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_set_size(panel, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(panel, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(panel, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(panel, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(panel, 0, LV_PART_MAIN);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    return panel;
}

static lv_obj_t *make_icon_button(lv_obj_t *parent, const char *symbol,
                                  lv_event_cb_t handler, void *user_data)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_size(button, BUTTON_SIZE, BUTTON_SIZE);
    lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(button, COLOR_SURFACE, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(button, 0, LV_PART_MAIN);

    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, symbol);
    lv_obj_set_style_text_color(label, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_center(label);

    if (handler != NULL) {
        lv_obj_add_event_cb(button, handler, LV_EVENT_CLICKED, user_data);
    }
    return button;
}

/*
 * A header of fixed height plus a body that grows into the rest.
 *
 * Expressed with flex rather than "100% minus the header": LV_PCT()
 * encodes a percentage as a special coordinate value, so subtracting a
 * pixel count from it is arithmetic on an encoded number - which once
 * produced a body one pixel high and an apparently blank screen.
 */
static lv_obj_t *make_view(DeviceUi *ui, const char *title,
                           lv_event_cb_t on_back)
{
    lv_obj_t *view = make_panel(ui->root);
    lv_obj_set_flex_flow(view, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *header = lv_obj_create(view);
    lv_obj_set_size(header, LV_PCT(100), HEADER_HEIGHT);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(header, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(header, 12, LV_PART_MAIN);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back = make_icon_button(header, LV_SYMBOL_LEFT, on_back, ui);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *label = lv_label_create(header);
    lv_label_set_text(label, title);
    lv_obj_set_style_text_font(label, &nn20clock_font_ui_bold_28,
                               LV_PART_MAIN);
    lv_obj_set_style_text_color(label, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);

    ui->body = lv_obj_create(view);
    lv_obj_set_width(ui->body, LV_PCT(100));
    lv_obj_set_flex_grow(ui->body, 1);
    lv_obj_set_style_bg_opa(ui->body, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(ui->body, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ui->body, 20, LV_PART_MAIN);
    lv_obj_set_style_pad_row(ui->body, 16, LV_PART_MAIN);
    lv_obj_set_flex_flow(ui->body, LV_FLEX_FLOW_COLUMN);
    /* Children narrower than the body sit in the middle of it. Rows are
     * full width and do not notice; sliders are inset (see below) and
     * would otherwise hug the left edge. */
    lv_obj_set_flex_align(ui->body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    return view;
}

static void clear_view(DeviceUi *ui)
{
    if (ui->view != NULL) {
        lv_obj_delete(ui->view);
        ui->view = NULL;
        ui->body = NULL;
        ui->password_input = NULL;
        ui->readout = NULL;
        ui->day_value = NULL;
        ui->night_value = NULL;
        ui->playback_value = NULL;
        ui->schedule_value = NULL;
        ui->ntp_switch = NULL;
        ui->clock_rollers = NULL;
        ui->about_open = false;
        ui->ota_status = NULL;
        ui->ota_bar = NULL;
        ui->ota_button = NULL;
        ui->ota_button_label = NULL;
    }
}

/*
 * Whether the clock is in the day half of the schedule being edited.
 *
 * Asked of the values in hand rather than of the app, because the user
 * may have just dragged a boundary past the current time and the answer
 * has to follow the screen, not the last saved setting.
 */
static bool schedule_says_day(const DeviceUi *ui)
{
    const time_t now = time(NULL);
    struct tm local = {0};
    if (localtime_r(&now, &local) == NULL) {
        return true;
    }

    const NN20ClockBrightnessSchedule schedule = {
        .day_percent = ui->config.brightness_day,
        .night_percent = ui->config.brightness_night,
        .day_start_minutes = ui->config.day_start_minutes,
        .night_start_minutes = ui->config.night_start_minutes,
    };
    return nn20clock_brightness_is_day(
        &schedule, (uint16_t)(local.tm_hour * 60 + local.tm_min));
}

/*
 * The slider's value, big enough to read while the thing you are
 * adjusting is disappearing.
 *
 * That is not a figure of speech: the brightness slider now reaches
 * down to 25, and somewhere in the low forties this panel's backlight
 * stops dimming and goes dark. Finding where means watching the number
 * as it goes - which a 20 px grey hint at the bottom of the screen
 * cannot do.
 *
 * White rather than the accent blue, and the clock face's 140 px
 * digits: on a panel fading to nothing, the brightest pixels available
 * and the largest glyphs already compiled in. The per-cent sign is a
 * separate label because those digits are the only glyphs that font
 * has - it carries the ten numerals, the colon and the hyphen, and
 * nothing else.
 */
static void add_readout(DeviceUi *ui, uint8_t percent)
{
    lv_obj_t *row = lv_obj_create(ui->body);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    /* Clear of the slider, so a hand on the knob is not covering the
     * number it is setting. */
    lv_obj_set_style_pad_top(row, READOUT_DROP, LV_PART_MAIN);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    /* Centred across, and bottom-aligned to each other so the small
     * per-cent sign sits on the digits' baseline rather than floating
     * beside their middle. Flex handles 25 and 100 without arithmetic. */
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_CENTER);

    ui->readout = lv_label_create(row);
    lv_obj_set_style_text_font(ui->readout, &nn20clock_font_clock_140,
                               LV_PART_MAIN);
    lv_obj_set_style_text_color(ui->readout, lv_color_white(), LV_PART_MAIN);
    lv_label_set_text_fmt(ui->readout, "%u", (unsigned)percent);

    lv_obj_t *sign = lv_label_create(row);
    lv_label_set_text(sign, "%");
    lv_obj_set_style_text_font(sign, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_set_style_text_color(sign, lv_color_white(), LV_PART_MAIN);
    /* Lifted off the very bottom: the big digits have descender room
     * under them that the per-cent sign does not. */
    lv_obj_set_style_pad_bottom(sign, 24, LV_PART_MAIN);
}

static void set_readout(DeviceUi *ui, uint8_t percent)
{
    if (ui->readout != NULL) {
        lv_label_set_text_fmt(ui->readout, "%u", (unsigned)percent);
    }
}

/* A tappable row: a title, a value beneath it, and a chevron. */
static lv_obj_t *add_menu_row(DeviceUi *ui, const char *title,
                              const char *value, lv_event_cb_t handler)
{
    lv_obj_t *row = lv_obj_create(ui->body);
    lv_obj_set_size(row, LV_PCT(100), MENU_ROW_HEIGHT);
    lv_obj_set_style_flex_grow(row, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(row, COLOR_SURFACE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 16, LV_PART_MAIN);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, handler, LV_EVENT_CLICKED, ui);

    lv_obj_t *title_label = lv_label_create(row);
    lv_label_set_text(title_label, title);
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_48,
                               LV_PART_MAIN);
    lv_obj_set_style_text_color(title_label, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_align(title_label, LV_ALIGN_TOP_LEFT, 0, 0);

    /*
     * The setting's current value, and it is half the reason to open
     * this screen at all - which network, how bright, what time. It was
     * 20 px in the muted blue used for things that are only decoration,
     * so the one line on each row actually worth reading was the one
     * drawn to be ignored.
     *
     * Bounded and elided rather than left to run: at this size a long
     * value would otherwise reach the chevron.
     */
    lv_obj_t *value_label = lv_label_create(row);
    lv_label_set_text(value_label, value);
    lv_label_set_long_mode(value_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(value_label, LV_PCT(90));
    lv_obj_set_style_text_font(value_label, &lv_font_montserrat_28,
                               LV_PART_MAIN);
    lv_obj_set_style_text_color(value_label, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_align(value_label, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    /* Sized with the rest: it inherited the theme's 14 px default,
     * which was a speck beside 28 px text and is a speck beside 48. */
    lv_obj_t *chevron = lv_label_create(row);
    lv_label_set_text(chevron, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_font(chevron, &lv_font_montserrat_28,
                               LV_PART_MAIN);
    lv_obj_set_style_text_color(chevron, COLOR_MUTED, LV_PART_MAIN);
    lv_obj_align(chevron, LV_ALIGN_RIGHT_MID, 0, 0);

    return row;
}

/* ---------------------------------------------------------- the menu -- */

static void on_leave(lv_event_t *event)
{
    DeviceUi *ui = lv_event_get_user_data(event);

    /* Design 8 routes this: the manager returns to CLOCK_STATE_TIME and
     * swaps the screen. This screen does not choose what comes next. */
    const NN20ClockUiCommand command = {
        .type = NN20CLOCK_UI_COMMAND_CLOSE_SETTINGS,
    };
    (void)nn20clock_ui_send_command(&ui->super, &command);
}

/*
 * Opening Wi-Fi always scans afresh.
 *
 * The results are kept while the screen is up - going in to type a
 * passphrase and coming back out should not cost another sweep of the
 * channels - but they are a snapshot of where the clock was standing
 * when it was taken, and a list from the last time the menu was opened
 * is worth nothing. Dropping them is all it takes: show_networks()
 * scans whenever it has none.
 */
static void on_open_networks(lv_event_t *event)
{
    DeviceUi *ui = lv_event_get_user_data(event);

    ui->aps.count = 0;
    show_networks(ui);
}

static void show_brightness(DeviceUi *ui);
static void show_volume(DeviceUi *ui);

static void on_open_brightness(lv_event_t *event)
{
    show_brightness((DeviceUi *)lv_event_get_user_data(event));
}

static void on_open_volume(lv_event_t *event)
{
    show_volume((DeviceUi *)lv_event_get_user_data(event));
}

/*
 * Media management is a screen of its own, not a view of this one: it
 * is a state in design 5 and the manager owns which screen is up. So
 * this raises a command and lets the manager swap us out, exactly as
 * the back button does.
 */
static void on_open_about(lv_event_t *event)
{
    show_about((DeviceUi *)lv_event_get_user_data(event));
}

static void on_open_time(lv_event_t *event)
{
    show_time((DeviceUi *)lv_event_get_user_data(event));
}

static void on_open_media(lv_event_t *event)
{
    DeviceUi *ui = lv_event_get_user_data(event);

    /* The config is written back before leaving, the same as every
     * other way out of this screen: a brightness or volume change made
     * just before tapping this must not be lost. */
    if (ui->service.save_config(ui->service.ctx, &ui->config) != ESP_OK) {
        ESP_LOGE(TAG, "could not store the settings");
    }

    const NN20ClockUiCommand command = {
        .type = NN20CLOCK_UI_COMMAND_OPEN_MEDIA_MANAGEMENT,
    };
    (void)nn20clock_ui_send_command(&ui->super, &command);
}

static void show_menu(DeviceUi *ui)
{
    clear_view(ui);
    ui->view = make_view(ui, "Settings", on_leave);
    if (ui->view == NULL) {
        return;
    }

    /*
     * The network and the address it got. The SSID alone does not say
     * whether the join actually worked - an address does, and it is the
     * thing you want when the clock is on the network and you are
     * looking for it from a laptop.
     *
     * The SSID shown is the one in use, from the radio, rather than the
     * stored one: after joining a new network the two agree, but before
     * a restart the stored value can be ahead of reality.
     */
    const char *ssid = (ui->net != NULL) ? nn20clock_net_ssid(ui->net)
                                         : ui->config.wifi_ssid;
    if (ssid == NULL || ssid[0] == '\0') {
        ssid = ui->config.wifi_ssid;
    }

    char ip[NN20CLOCK_NET_IP_STRING_MAX] = "";
    const bool has_ip =
        (ui->net != NULL) &&
        (nn20clock_net_ip_string(ui->net, ip, sizeof(ip)) == ESP_OK);

    char wifi_value[NN20CLOCK_WIFI_SSID_MAX + NN20CLOCK_NET_IP_STRING_MAX + 8];
    if (ssid[0] == '\0') {
        snprintf(wifi_value, sizeof(wifi_value), "not configured");
    } else if (has_ip) {
        snprintf(wifi_value, sizeof(wifi_value), "%s  %s", ssid, ip);
    } else {
        /* Configured but with no address: connecting, or it failed. */
        snprintf(wifi_value, sizeof(wifi_value), "%s  not connected", ssid);
    }
    (void)add_menu_row(ui, "Wi-Fi", wifi_value, on_open_networks);

    char brightness_value[64];
    snprintf(brightness_value, sizeof(brightness_value),
             "%u%% day, %u%% night from %02u:%02u",
             (unsigned)ui->config.brightness_day,
             (unsigned)ui->config.brightness_night,
             (unsigned)(ui->config.night_start_minutes / 60u),
             (unsigned)(ui->config.night_start_minutes % 60u));
    (void)add_menu_row(ui, "Brightness", brightness_value,
                       on_open_brightness);

    char volume_value[48];
    /* Say plainly that nothing consumes this yet, rather than offering a
     * control that appears to do something. */
    snprintf(volume_value, sizeof(volume_value), "%u%%",
             (unsigned)ui->config.volume);
    (void)add_menu_row(ui, "Volume", volume_value, on_open_volume);

    /* Design 5 puts the door to media management here and nowhere else
     * - it is a maintenance screen, not something to reach from the
     * clock face by accident. */
    (void)add_menu_row(ui, "Media", "card, FTP server, and who is "
                                    "connected", on_open_media);

    /*
     * Below Media. The clock sets itself, so this is the row somebody
     * only opens when it has not - which is why it is last rather than
     * first.
     */
    char time_value[64];
    if (ui->config.ntp_enabled) {
        snprintf(time_value, sizeof(time_value), "set from the internet");
    } else {
        const time_t now = time(NULL);
        struct tm local = {0};
        if (localtime_r(&now, &local) != NULL) {
            snprintf(time_value, sizeof(time_value),
                     "set by hand - %02u:%02u, %02u/%02u/%04u",
                     (unsigned)local.tm_hour, (unsigned)local.tm_min,
                     (unsigned)local.tm_mday, (unsigned)local.tm_mon + 1u,
                     (unsigned)local.tm_year + 1900u);
        } else {
            snprintf(time_value, sizeof(time_value), "set by hand");
        }
    }
    (void)add_menu_row(ui, "Time", time_value, on_open_time);

    /* Last, because it is the row nobody needs twice. */
    const esp_app_desc_t *app = esp_app_get_description();
    (void)add_menu_row(ui, "About",
                       (app != NULL) ? app->version : "Video Alarm Clock",
                       on_open_about);
}

/* ------------------------------------------------------------- Wi-Fi -- */

static void on_back_to_menu(lv_event_t *event)
{
    show_menu((DeviceUi *)lv_event_get_user_data(event));
}

/* Runs on the UiWorker, posted from the scan callback. */
static int scan_done_private(nn20_worker_ctx *worker, void *user_data)
{
    (void)worker;
    DeviceUi *ui = user_data;

    ui->scanning = false;
    /* Only redraw if the networks view is still the one on screen: the
     * user may have gone back while the scan was running. */
    show_networks(ui);
    return 0;
}

/*
 * Runs on the CoreWorker, where the scan happened. It copies the result
 * and posts; it must not touch LVGL from here.
 */
static void on_scan_done(esp_err_t status, const NN20ClockNetApList *aps,
                         void *user_data)
{
    DeviceUi *ui = user_data;

    if (status == ESP_OK && aps != NULL) {
        ui->aps = *aps;
    } else {
        ui->aps.count = 0;
    }

    (void)nn20_worker_post(nn20clock_ui_worker(&ui->super), scan_done_private,
                           ui);
}

static void on_password_ready(lv_event_t *event);

/*
 * Back from the passphrase prompt is back to the list it was reached
 * from, not out to the settings menu. Choosing the wrong network off a
 * list of them is easy, and undoing it should cost one press rather
 * than a press and a fresh scan.
 */
static void on_back_to_networks(lv_event_t *event)
{
    show_networks((DeviceUi *)lv_event_get_user_data(event));
}

/*
 * Show or hide what is being typed.
 *
 * A passphrase entered on a touch keyboard and never shown is a
 * passphrase typed twice, so there is the usual eye - and it starts
 * hidden, because the screen faces the room.
 */
static void on_password_reveal(lv_event_t *event)
{
    DeviceUi *ui = lv_event_get_user_data(event);
    lv_obj_t *button = lv_event_get_target(event);

    if (ui->password_input == NULL) {
        return;
    }

    const bool hidden = !lv_textarea_get_password_mode(ui->password_input);
    lv_textarea_set_password_mode(ui->password_input, hidden);

    lv_obj_t *icon = lv_obj_get_child(button, 0);
    if (icon != NULL) {
        lv_label_set_text(icon, hidden ? LV_SYMBOL_EYE_OPEN
                                       : LV_SYMBOL_EYE_CLOSE);
    }
}

/* A network was chosen: ask for the passphrase, or join straight away if
 * it is open. */
static void on_network_chosen(lv_event_t *event)
{
    DeviceUi *ui = lv_event_get_user_data(event);
    lv_obj_t *row = lv_event_get_target(event);
    const size_t index = (size_t)(uintptr_t)lv_obj_get_user_data(row);

    if (index >= ui->aps.count) {
        return;
    }
    snprintf(ui->pending_ssid, sizeof(ui->pending_ssid), "%s",
             ui->aps.aps[index].ssid);

    if (!ui->aps.aps[index].needs_password) {
        snprintf(ui->config.wifi_ssid, sizeof(ui->config.wifi_ssid), "%s",
                 ui->pending_ssid);
        ui->config.wifi_password[0] = '\0';
        (void)ui->service.save_config(ui->service.ctx, &ui->config);
        (void)ui->service.connect_wifi(ui->service.ctx, ui->config.wifi_ssid,
                                       "");
        show_networks(ui);
        return;
    }

    clear_view(ui);
    ui->view = make_view(ui, ui->pending_ssid, on_back_to_networks);
    if (ui->view == NULL) {
        return;
    }

    /* The field and its eye on one line, the field taking whatever the
     * button leaves. */
    lv_obj_t *entry = lv_obj_create(ui->body);
    lv_obj_set_width(entry, LV_PCT(100));
    lv_obj_set_height(entry, LV_SIZE_CONTENT);
    lv_obj_set_style_flex_grow(entry, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(entry, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(entry, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(entry, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_column(entry, 12, LV_PART_MAIN);
    lv_obj_set_flex_flow(entry, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(entry, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(entry, LV_OBJ_FLAG_SCROLLABLE);

    ui->password_input = lv_textarea_create(entry);
    lv_obj_set_flex_grow(ui->password_input, 1);
    lv_textarea_set_one_line(ui->password_input, true);
    /* Hides the characters as they are typed. The passphrase is not
     * shown, logged, or read back anywhere. */
    lv_textarea_set_password_mode(ui->password_input, true);
    lv_textarea_set_placeholder_text(ui->password_input, "Password");
    lv_textarea_set_max_length(ui->password_input,
                               NN20CLOCK_WIFI_PASSWORD_MAX - 1);
    lv_obj_set_style_text_font(ui->password_input, &lv_font_montserrat_28,
                               LV_PART_MAIN);

    /* Closed to start with: hidden is the state the field is in. */
    (void)make_icon_button(entry, LV_SYMBOL_EYE_CLOSE, on_password_reveal,
                           ui);

    lv_obj_t *keyboard = lv_keyboard_create(ui->view);
    lv_keyboard_set_textarea(keyboard, ui->password_input);
    lv_obj_set_size(keyboard, LV_PCT(100), LV_PCT(50));
    lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    /*
     * The keys are drawn by LV_PART_ITEMS, which was left on the
     * theme's 14 px default - a glyph a fifth the height of the button
     * holding it, on a panel meant to be typed on with a fingertip.
     *
     * 28 px is as far as it goes: the top row is twelve keys wide, so
     * each is 60 px, and the widest label on it ("ABC") has to fit
     * inside that. A larger face would push the multi-character keys
     * over their buttons while the letters still had room.
     */
    lv_obj_set_style_text_font(keyboard, &lv_font_montserrat_28,
                               LV_PART_ITEMS);
    /* LVGL raises READY when the keyboard's tick is pressed. */
    lv_obj_add_event_cb(keyboard, on_password_ready, LV_EVENT_READY, ui);

    lv_obj_add_state(ui->password_input, LV_STATE_FOCUSED);
}

static void on_password_ready(lv_event_t *event)
{
    DeviceUi *ui = lv_event_get_user_data(event);
    if (ui->password_input == NULL) {
        return;
    }

    const char *password = lv_textarea_get_text(ui->password_input);
    snprintf(ui->config.wifi_ssid, sizeof(ui->config.wifi_ssid), "%s",
             ui->pending_ssid);
    snprintf(ui->config.wifi_password, sizeof(ui->config.wifi_password), "%s",
             (password != NULL) ? password : "");

    /* Stored first, then applied: a reboot mid-connect should still come
     * up on the network the user chose. */
    if (ui->service.save_config(ui->service.ctx, &ui->config) != ESP_OK) {
        ESP_LOGE(TAG, "could not store the network");
    }
    if (ui->service.connect_wifi(ui->service.ctx, ui->config.wifi_ssid,
                                 ui->config.wifi_password) != ESP_OK) {
        /* Most likely the radio never came up, because no network was
         * configured at boot. Stored either way, so a restart uses it. */
        ESP_LOGW(TAG, "could not join now; stored for the next restart");
    }

    /* Never logged: the SSID is fine, the passphrase is not. */
    ESP_LOGI(TAG, "network set to '%s'", ui->config.wifi_ssid);

    /*
     * Back to the list, not out to the settings menu - and without
     * scanning again. The scan results are still the ones this network
     * was picked from, and the card at the top of that screen is where
     * the join can be watched: it names the network just chosen and
     * says whether the radio has got there yet.
     */
    show_networks(ui);
}

/* A plain caption over a group of rows. */
static void add_section_label(DeviceUi *ui, const char *text)
{
    lv_obj_t *label = lv_label_create(ui->body);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, COLOR_MUTED, LV_PART_MAIN);
    lv_obj_set_width(label, LV_PCT(100));
}

/*
 * Which network this clock is set to, as far as anything on screen is
 * concerned.
 *
 * The stored one first, the radio's only as a fallback. They normally
 * agree, and the two cases where they do not both come out right this
 * way round:
 *
 *   nothing stored     the network was configured at build time and
 *                      never written to storage, so the radio is the
 *                      only one who knows. Reading the stored value
 *                      alone is what had a device sitting happily on
 *                      Wi-Fi reporting that it had no network.
 *   just chosen        a network picked on this screen is saved before
 *                      the join is even posted, and the radio still
 *                      names the old one for as long as it takes to
 *                      drop it. The one the user just chose is the one
 *                      to show.
 *
 * Which leaves the stored value being ahead of reality, and that is
 * what the status line under it is for: it says "Connecting..." or
 * "Could not connect" rather than letting the name imply a join that
 * has not happened.
 *
 * Never NULL, and "" when there is genuinely none.
 */
static const char *current_ssid(const DeviceUi *ui)
{
    if (ui->config.wifi_ssid[0] != '\0') {
        return ui->config.wifi_ssid;
    }
    const char *ssid = (ui->net != NULL) ? nn20clock_net_ssid(ui->net) : NULL;
    return (ssid != NULL) ? ssid : "";
}

/*
 * What this clock is on now, at the top of the Wi-Fi screen and above
 * whatever the scan turns up.
 *
 * It is the first question the screen is opened to answer - "which
 * network am I on, and am I actually on it" - and it used to be
 * answerable only by reading down a list of everything in range looking
 * for the one drawn in the accent colour.
 *
 * Not a row: nothing happens when it is touched. Rejoining is choosing
 * the network from the list below, which is where every other network
 * is chosen too.
 */
static void add_current_network(DeviceUi *ui)
{
    add_section_label(ui, "Current network");

    lv_obj_t *card = lv_obj_create(ui->body);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_flex_grow(card, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(card, COLOR_SURFACE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_row(card, 6, LV_PART_MAIN);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    const char *ssid = current_ssid(ui);
    const bool configured = (ssid[0] != '\0');

    lv_obj_t *name = lv_label_create(card);
    lv_label_set_text(name, configured ? ssid : "No network configured");
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(name, LV_PCT(100));
    lv_obj_set_style_text_font(name, &nn20clock_font_ui_bold_28,
                               LV_PART_MAIN);
    lv_obj_set_style_text_color(name, configured ? COLOR_ACCENT : COLOR_MUTED,
                                LV_PART_MAIN);

    /*
     * The stored network and the radio's state are two different
     * things, and the gap between them is the whole reason this is
     * worth drawing: a passphrase saved a moment ago is configured and
     * not yet connected, and that is what somebody standing here needs
     * to see.
     */
    char status[64] = "Not connected";
    if (!configured) {
        status[0] = '\0';
    } else if (ui->net == NULL) {
        snprintf(status, sizeof(status), "No radio available");
    } else {
        switch (nn20clock_net_state(ui->net)) {
        case NN20CLOCK_NET_CONNECTED: {
            char ip[NN20CLOCK_NET_IP_STRING_MAX] = "";
            if (nn20clock_net_ip_string(ui->net, ip, sizeof(ip)) == ESP_OK) {
                snprintf(status, sizeof(status), "Connected  %s", ip);
            } else {
                snprintf(status, sizeof(status), "Connected");
            }
            break;
        }
        case NN20CLOCK_NET_CONNECTING:
            snprintf(status, sizeof(status), "Connecting...");
            break;
        case NN20CLOCK_NET_FAILED:
            snprintf(status, sizeof(status), "Could not connect");
            break;
        case NN20CLOCK_NET_DISABLED:
            snprintf(status, sizeof(status), "Radio off");
            break;
        }
    }

    if (status[0] != '\0') {
        lv_obj_t *line = lv_label_create(card);
        lv_label_set_text(line, status);
        lv_obj_set_style_text_font(line, &lv_font_montserrat_28,
                                   LV_PART_MAIN);
        lv_obj_set_style_text_color(line, COLOR_TEXT, LV_PART_MAIN);
    }
}

static void show_networks(DeviceUi *ui)
{
    clear_view(ui);
    ui->view = make_view(ui, "Wi-Fi", on_back_to_menu);
    if (ui->view == NULL) {
        return;
    }

    /* First, and in every branch below: what the clock is on now does
     * not depend on how the scan for everything else is going. */
    add_current_network(ui);

    if (ui->net == NULL) {
        lv_obj_t *label = lv_label_create(ui->body);
        lv_label_set_text(label, "No radio available.");
        lv_obj_set_style_text_font(label, &lv_font_montserrat_28,
                                   LV_PART_MAIN);
        lv_obj_set_style_text_color(label, COLOR_TEXT, LV_PART_MAIN);
        return;
    }

    if (ui->scanning) {
        lv_obj_t *spinner = lv_spinner_create(ui->body);
        lv_obj_set_size(spinner, 96, 96);

        lv_obj_t *label = lv_label_create(ui->body);
        lv_label_set_text(label, "Scanning...");
        lv_obj_set_style_text_font(label, &lv_font_montserrat_28,
                                   LV_PART_MAIN);
        lv_obj_set_style_text_color(label, COLOR_TEXT, LV_PART_MAIN);
        return;
    }

    if (ui->aps.count == 0u) {
        /* Nothing yet: start a scan, and this view is rebuilt when it
         * finishes. The scan runs on the CoreWorker, so the screen stays
         * responsive while it sweeps the channels. */
        ui->scanning = true;
        if (nn20clock_net_scan(ui->net, on_scan_done, ui) != ESP_OK) {
            ui->scanning = false;
            lv_obj_t *label = lv_label_create(ui->body);
            lv_label_set_text(label, "Scan unavailable.\nIs Wi-Fi enabled?");
            lv_obj_set_style_text_font(label, &lv_font_montserrat_28,
                                       LV_PART_MAIN);
            lv_obj_set_style_text_color(label, COLOR_TEXT, LV_PART_MAIN);
            return;
        }
        show_networks(ui);   /* redraw as the spinner */
        return;
    }

    add_section_label(ui, "Networks in range");

    const char *const joined = current_ssid(ui);

    for (size_t i = 0; i < ui->aps.count; i++) {
        lv_obj_t *row = lv_obj_create(ui->body);
        lv_obj_set_size(row, LV_PCT(100), 88);
        lv_obj_set_style_flex_grow(row, 0, LV_PART_MAIN);
        lv_obj_set_style_bg_color(row, COLOR_SURFACE, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(row, 16, LV_PART_MAIN);
        lv_obj_set_style_pad_all(row, 16, LV_PART_MAIN);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(row, (void *)(uintptr_t)i);
        lv_obj_add_event_cb(row, on_network_chosen, LV_EVENT_CLICKED, ui);

        lv_obj_t *name = lv_label_create(row);
        lv_label_set_text(name, ui->aps.aps[i].ssid);
        lv_obj_set_style_text_font(name, &lv_font_montserrat_28,
                                   LV_PART_MAIN);
        /* The one already joined is drawn in the accent, so the list
         * agrees with the card above it. */
        lv_obj_set_style_text_color(name,
                                    (strcmp(ui->aps.aps[i].ssid, joined) == 0)
                                        ? COLOR_ACCENT
                                        : COLOR_TEXT,
                                    LV_PART_MAIN);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_t *lock = lv_label_create(row);
        lv_label_set_text(lock, ui->aps.aps[i].needs_password
                                    ? LV_SYMBOL_WIFI "  " LV_SYMBOL_CLOSE
                                    : LV_SYMBOL_WIFI);
        lv_obj_set_style_text_color(lock, COLOR_MUTED, LV_PART_MAIN);
        lv_obj_align(lock, LV_ALIGN_RIGHT_MID, 0, 0);
    }
}

/* --------------------------------------------------------- brightness -- */

/*
 * One screen holding four settings: how bright by day, how bright by
 * night, and the two times it changes at. It has to fit without
 * scrolling - a control you have to scroll to reach is one you set once
 * and never touch again - so the arithmetic is written down rather than
 * guessed at.
 *
 *   720 panel - 96 header - 40 body padding      = 584 usable
 *   four sections of (52 label + 48 slider)      = 400
 *   seven SECTION_GAP gaps between the eight     = 182
 *                                                -----
 *                                                  582
 *
 * The times sit in the last section's own value label, so they cost
 * nothing extra. The gap is everything that is left, and it was 44 px
 * when there were three sections: four sliders packed together are four
 * chances to grab the wrong one in the dark, so it stays as wide as the
 * panel allows rather than a round number.
 */
#define SECTION_LABEL_HEIGHT 52
#define SECTION_SLIDER_HEIGHT 48
#define SECTION_GAP 26

/*
 * Slider track width, as a percentage of the body.
 *
 * The body is 680 px wide inside its padding, so 90% is a 612 px track
 * and the knob takes it to 660 - clear of both edges with room for the
 * knob's shadow. Anything above about 93% puts the knob off the panel.
 */
#define SLIDER_WIDTH_PCT 90

/* Live preview only. Which of the two values is being edited decides
 * whether the panel follows the finger: dragging the night slider at
 * noon must not black out the screen you are looking at. */
static void preview_brightness(DeviceUi *ui, uint8_t percent, bool live)
{
    if (live) {
        (void)ui->service.apply_brightness(ui->service.ctx, percent);
    }
}

static void on_day_brightness_changed(lv_event_t *event)
{
    DeviceUi *ui = lv_event_get_user_data(event);
    const uint8_t percent =
        (uint8_t)lv_slider_get_value(lv_event_get_target(event));

    ui->config.brightness_day = percent;
    if (ui->day_value != NULL) {
        lv_label_set_text_fmt(ui->day_value, "%u%%", (unsigned)percent);
    }
    /* Preview it only if this is the half the clock is actually in. */
    preview_brightness(ui, percent, ui->editing_is_day);
}

static void on_night_brightness_changed(lv_event_t *event)
{
    DeviceUi *ui = lv_event_get_user_data(event);
    const uint8_t percent =
        (uint8_t)lv_slider_get_value(lv_event_get_target(event));

    ui->config.brightness_night = percent;
    if (ui->night_value != NULL) {
        lv_label_set_text_fmt(ui->night_value, "%u%%", (unsigned)percent);
    }
    preview_brightness(ui, percent, !ui->editing_is_day);
}

/*
 * The least a film is shown at. Always previewed: the value only ever
 * appears on the panel during a film, so the one chance to see what it
 * means is while it is being dragged - and that is usually at night,
 * which is exactly when it matters. The panel goes back to the schedule
 * when this screen closes.
 */
static void on_playback_brightness_changed(lv_event_t *event)
{
    DeviceUi *ui = lv_event_get_user_data(event);
    const uint8_t percent =
        (uint8_t)lv_slider_get_value(lv_event_get_target(event));

    ui->config.brightness_playback_min = percent;
    if (ui->playback_value != NULL) {
        lv_label_set_text_fmt(ui->playback_value, "%u%%", (unsigned)percent);
    }
    preview_brightness(ui, percent, true);
}

/* The two knobs of the range slider are the two boundary times. LVGL
 * calls the lower one "left"; here that is when the day starts. */
static void on_schedule_changed(lv_event_t *event)
{
    DeviceUi *ui = lv_event_get_user_data(event);
    lv_obj_t *slider = lv_event_get_target(event);

    const uint16_t step = NN20CLOCK_BRIGHTNESS_STEP_MINUTES;
    ui->config.day_start_minutes =
        (uint16_t)(lv_slider_get_left_value(slider) * step);
    ui->config.night_start_minutes =
        (uint16_t)(lv_slider_get_value(slider) * step);

    if (ui->schedule_value != NULL) {
        lv_label_set_text_fmt(
            ui->schedule_value, "%02u:%02u  -  %02u:%02u",
            (unsigned)(ui->config.day_start_minutes / 60u),
            (unsigned)(ui->config.day_start_minutes % 60u),
            (unsigned)(ui->config.night_start_minutes / 60u),
            (unsigned)(ui->config.night_start_minutes % 60u));
    }

    /* Moving a boundary can move the clock from one half to the other,
     * so re-decide which value the panel should be showing. */
    ui->editing_is_day = schedule_says_day(ui);
    preview_brightness(ui, ui->editing_is_day ? ui->config.brightness_day
                                              : ui->config.brightness_night,
                       true);
}

static void on_brightness_done(lv_event_t *event)
{
    DeviceUi *ui = lv_event_get_user_data(event);

    if (ui->service.save_config(ui->service.ctx, &ui->config) != ESP_OK) {
        ESP_LOGE(TAG, "could not store the brightness schedule");
    }
    show_menu(ui);
}

/*
 * A section: a heading on the left, its value on the right in the big
 * face, and a full-width slider under both.
 *
 * Returns the value label, which is the part that changes. The slider
 * comes back through `out_slider` because two of the three need setting
 * up further.
 */
static lv_obj_t *add_section(DeviceUi *ui, const char *title,
                             lv_obj_t **out_slider)
{
    lv_obj_t *head = lv_obj_create(ui->body);
    lv_obj_set_size(head, LV_PCT(100), SECTION_LABEL_HEIGHT);
    lv_obj_set_style_flex_grow(head, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(head, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(head, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(head, 0, LV_PART_MAIN);
    lv_obj_remove_flag(head, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(head);
    lv_label_set_text(label, title);
    lv_obj_set_style_text_font(label, &nn20clock_font_ui_bold_28,
                               LV_PART_MAIN);
    lv_obj_set_style_text_color(label, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 0, 0);

    /* White and large, for the reason add_readout() gives: this is read
     * while the thing it describes is going dark. */
    lv_obj_t *value = lv_label_create(head);
    lv_label_set_text(value, "");
    lv_obj_set_style_text_font(value, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_set_style_text_color(value, lv_color_white(), LV_PART_MAIN);
    lv_obj_align(value, LV_ALIGN_RIGHT_MID, 0, 0);

    lv_obj_t *slider = lv_slider_create(ui->body);
    /*
     * Not the full width. LVGL centres the knob on the end of the
     * track, so a slider this tall draws half a knob - 24 px - past
     * each end of whatever width it is given. At 100% that is 24 px off
     * the panel on both sides.
     */
    lv_obj_set_width(slider, LV_PCT(SLIDER_WIDTH_PCT));
    lv_obj_set_height(slider, SECTION_SLIDER_HEIGHT);
    lv_obj_set_style_flex_grow(slider, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, COLOR_ACCENT, LV_PART_KNOB);

    *out_slider = slider;
    return value;
}

static void show_brightness(DeviceUi *ui)
{
    clear_view(ui);
    ui->view = make_view(ui, "Brightness", on_brightness_done);
    if (ui->view == NULL) {
        return;
    }

    /* Wider than the 16 px every other view uses: see SECTION_GAP. */
    lv_obj_set_style_pad_row(ui->body, SECTION_GAP, LV_PART_MAIN);

    /* Which half the clock is in right now, so the sliders preview the
     * one that is actually on the panel. */
    ui->editing_is_day = schedule_says_day(ui);

    lv_obj_t *slider = NULL;

    /* ---- day ---- */
    ui->day_value = add_section(ui, "Day", &slider);
    /*
     * Not from zero, and not below what this panel can show: under its
     * floor the backlight goes dark rather than dim, and a screen you
     * cannot read is a screen you cannot use to turn the brightness back
     * up. See NN20CLOCK_DISPLAY_MIN_ON_BRIGHTNESS.
     */
    lv_slider_set_range(slider, NN20CLOCK_DISPLAY_MIN_ON_BRIGHTNESS, 100);
    lv_slider_set_value(slider, ui->config.brightness_day, LV_ANIM_OFF);
    lv_obj_add_event_cb(slider, on_day_brightness_changed,
                        LV_EVENT_VALUE_CHANGED, ui);
    lv_label_set_text_fmt(ui->day_value, "%u%%",
                          (unsigned)ui->config.brightness_day);

    /* ---- night ---- */
    ui->night_value = add_section(ui, "Night", &slider);
    lv_slider_set_range(slider, NN20CLOCK_DISPLAY_MIN_ON_BRIGHTNESS, 100);
    lv_slider_set_value(slider, ui->config.brightness_night, LV_ANIM_OFF);
    lv_obj_add_event_cb(slider, on_night_brightness_changed,
                        LV_EVENT_VALUE_CHANGED, ui);
    lv_label_set_text_fmt(ui->night_value, "%u%%",
                          (unsigned)ui->config.brightness_night);

    /* ---- the playback minimum ---- */
    ui->playback_value = add_section(ui, "Playback minimum", &slider);
    /*
     * The same floor as the other two. Below it the minimum could never
     * matter anyway - the schedule is never under it - so the bottom of
     * the track is "no minimum", and a stored 0 simply shows there. The
     * label reads the slider rather than the config for that reason:
     * it says what the knob says.
     */
    lv_slider_set_range(slider, NN20CLOCK_DISPLAY_MIN_ON_BRIGHTNESS, 100);
    lv_slider_set_value(slider, ui->config.brightness_playback_min,
                        LV_ANIM_OFF);
    lv_obj_add_event_cb(slider, on_playback_brightness_changed,
                        LV_EVENT_VALUE_CHANGED, ui);
    lv_label_set_text_fmt(ui->playback_value, "%u%%",
                          (unsigned)lv_slider_get_value(slider));

    /* ---- the two times ---- */
    ui->schedule_value = add_section(ui, "Day starts / ends", &slider);
    /*
     * A range slider: two knobs on one track, which is the shape of the
     * thing being set - a day with a beginning and an end. Counted in
     * quarter hours rather than minutes, because 1440 steps across 680
     * pixels is a control nobody can place, and nobody has an opinion
     * about 21:53 versus 22:00.
     */
    lv_slider_set_mode(slider, LV_SLIDER_MODE_RANGE);
    lv_slider_set_range(slider, 0,
                        (int32_t)(NN20CLOCK_BRIGHTNESS_DAY_MINUTES /
                                  NN20CLOCK_BRIGHTNESS_STEP_MINUTES) - 1);
    /*
     * The right knob first, then the left, and the order is not a
     * matter of taste: LVGL clamps the left value against the right
     * one. A fresh slider's right value is 0, so setting the left knob
     * to 06:30 before the right knob has moved off zero silently
     * clamps it back to 00:00 - which reads as "the schedule did not
     * survive a reload", because the label beside it comes from the
     * stored config and is right while the knob is wrong.
     */
    lv_slider_set_value(slider,
                        ui->config.night_start_minutes /
                            NN20CLOCK_BRIGHTNESS_STEP_MINUTES,
                        LV_ANIM_OFF);
    lv_slider_set_left_value(slider,
                             ui->config.day_start_minutes /
                                 NN20CLOCK_BRIGHTNESS_STEP_MINUTES,
                             LV_ANIM_OFF);
    lv_obj_add_event_cb(slider, on_schedule_changed, LV_EVENT_VALUE_CHANGED,
                        ui);

    /* The times are two numbers, so they go under the slider at the
     * size the rest of the screen uses rather than in the big face. */
    lv_obj_set_style_text_font(ui->schedule_value, &lv_font_montserrat_28,
                               LV_PART_MAIN);
    lv_label_set_text_fmt(ui->schedule_value, "%02u:%02u  -  %02u:%02u",
                          (unsigned)(ui->config.day_start_minutes / 60u),
                          (unsigned)(ui->config.day_start_minutes % 60u),
                          (unsigned)(ui->config.night_start_minutes / 60u),
                          (unsigned)(ui->config.night_start_minutes % 60u));
}

/* ------------------------------------------------------------- about -- */

/*
 * Who made it, and - the part that earns its place - exactly which
 * build is on this board.
 *
 * The version is not a string kept up to date by hand. ESP-IDF derives
 * PROJECT_VER from `git describe`, so it reads "v0.1.0" on the tag and
 * "v0.1.0-2-gb741df4-dirty" two commits later, and the trailing
 * "-dirty" is the build telling you it came from a tree with
 * uncommitted changes. On a device somebody else is holding, being able
 * to ask it what it is running is worth more than a tidy number.
 */
static void add_about_line(DeviceUi *ui, const char *text,
                           const lv_font_t *font, lv_color_t colour)
{
    lv_obj_t *label = lv_label_create(ui->body);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, colour, LV_PART_MAIN);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
}

/* A blank line, for the spacing the design of this screen is entirely
 * made of. */
static void add_about_gap(DeviceUi *ui, int32_t height)
{
    lv_obj_t *gap = lv_obj_create(ui->body);
    lv_obj_set_size(gap, 1, height);
    lv_obj_set_style_flex_grow(gap, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(gap, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(gap, 0, LV_PART_MAIN);
    lv_obj_remove_flag(gap, LV_OBJ_FLAG_SCROLLABLE);
}

/* --------------------------------------------------------- updates -- */

/*
 * The update controls at the foot of the About view.
 *
 * Two buttons in one, because they are two steps of one errand and a
 * screen with a permanently greyed-out "Update" beside "Check" is a
 * screen that spends most of its life showing a control nobody can use.
 * The single button says what it will do next.
 *
 * Nothing here pushes: nn20clock_ota publishes a snapshot and this
 * reads it from the one-second timer event the screen already gets. A
 * completion callback would have to carry this screen's address across
 * a download that outlives it, and the only ways to make that safe are
 * a lock - which design 4 forbids - or a handshake this does not need.
 */

/* What the button offers, given where the update got to. */
static const char *ota_action_text(const NN20ClockOtaStatus *status)
{
    switch (status->state) {
    case NN20CLOCK_OTA_AVAILABLE:
        return "Install";
    case NN20CLOCK_OTA_CHECKING:
        return "Checking...";
    case NN20CLOCK_OTA_DOWNLOADING:
        return "Installing...";
    case NN20CLOCK_OTA_INSTALLED:
        return "Restart now";
    case NN20CLOCK_OTA_FAILED:
        return "Try again";
    case NN20CLOCK_OTA_IDLE:
    case NN20CLOCK_OTA_UP_TO_DATE:
        break;
    }
    return "Check for updates";
}

static bool ota_action_enabled(const NN20ClockOtaStatus *status)
{
    return status->state != NN20CLOCK_OTA_CHECKING &&
           status->state != NN20CLOCK_OTA_DOWNLOADING;
}

/*
 * Why it failed, in words rather than in an ESP_ERR_ name.
 *
 * The person reading this is holding an alarm clock, not a debugger,
 * and the useful distinction is whose problem it is: the network, the
 * release, or the device. The exact code is in the log for the case
 * where somebody does want it.
 */
static const char *ota_failure_text(esp_err_t err)
{
    switch (err) {
    case ESP_ERR_INVALID_STATE:
        return "This build has nowhere to check.";
    case ESP_ERR_INVALID_RESPONSE:
        return "The update server answered with nonsense.";
    case ESP_ERR_INVALID_CRC:
        return "The download did not match its checksum.\nNothing was installed.";
    case ESP_ERR_INVALID_SIZE:
        return "The download stopped before the end.";
    case ESP_ERR_NO_MEM:
        return "Not enough memory to update right now.";
    case ESP_ERR_NOT_FOUND:
        return "This board has no second firmware slot.";
    default:
        break;
    }
    return "Could not reach the update server.";
}

/* The line above the button. */
static void ota_status_text(const DeviceUi *ui,
                            const NN20ClockOtaStatus *status, char *out,
                            size_t size)
{
    switch (status->state) {
    case NN20CLOCK_OTA_IDLE:
        snprintf(out, size, "%s",
                 (ui->net != NULL && nn20clock_net_is_connected(ui->net))
                     ? ""
                     : "No network. Updates need one.");
        return;
    case NN20CLOCK_OTA_CHECKING:
        snprintf(out, size, "Looking for a newer version...");
        return;
    case NN20CLOCK_OTA_UP_TO_DATE:
        snprintf(out, size, "This is the latest version.");
        return;
    case NN20CLOCK_OTA_AVAILABLE:
        if (status->notes[0] != '\0') {
            snprintf(out, size, "%s is available\n%s", status->version,
                     status->notes);
        } else {
            snprintf(out, size, "%s is available", status->version);
        }
        return;
    case NN20CLOCK_OTA_DOWNLOADING:
        snprintf(out, size, "Installing %s - %u%%\nKeep the power on.",
                 status->version, (unsigned)status->percent);
        return;
    case NN20CLOCK_OTA_INSTALLED:
        snprintf(out, size, "%s is installed.\nRestart to run it.",
                 status->version);
        return;
    case NN20CLOCK_OTA_FAILED:
        snprintf(out, size, "%s", ota_failure_text(status->error));
        return;
    }
    snprintf(out, size, " ");
}

/*
 * Draw the controls to match a status. Called on every second tick
 * while the About view is up, so it changes what it must and leaves the
 * rest alone - rebuilding widgets once a second is how a screen
 * flickers.
 */
static void refresh_ota(DeviceUi *ui, const NN20ClockOtaStatus *status)
{
    if (ui->ota_status == NULL) {
        return;
    }

    char text[192];
    ota_status_text(ui, status, text, sizeof(text));
    lv_label_set_text(ui->ota_status, text);

    if (status->state == NN20CLOCK_OTA_DOWNLOADING) {
        lv_obj_remove_flag(ui->ota_bar, LV_OBJ_FLAG_HIDDEN);
        lv_bar_set_value(ui->ota_bar, (int32_t)status->percent, LV_ANIM_OFF);
    } else {
        lv_obj_add_flag(ui->ota_bar, LV_OBJ_FLAG_HIDDEN);
    }

    if (status->state == ui->ota_shown) {
        return;   /* only the percentage moved */
    }
    ui->ota_shown = status->state;

    lv_label_set_text(ui->ota_button_label, ota_action_text(status));
    if (ota_action_enabled(status)) {
        lv_obj_remove_state(ui->ota_button, LV_STATE_DISABLED);
        lv_obj_set_style_bg_color(ui->ota_button, COLOR_ACCENT, LV_PART_MAIN);
    } else {
        lv_obj_add_state(ui->ota_button, LV_STATE_DISABLED);
        lv_obj_set_style_bg_color(ui->ota_button, COLOR_MUTED, LV_PART_MAIN);
    }
}

static void on_ota_action(lv_event_t *event)
{
    DeviceUi *ui = lv_event_get_user_data(event);
    if (ui->ota == NULL) {
        return;
    }

    NN20ClockOtaStatus status;
    nn20clock_ota_status(ui->ota, &status);

    switch (status.state) {
    case NN20CLOCK_OTA_INSTALLED:
        /* Does not return. Everything worth keeping is already in NVS
         * or on the card; there is nothing to flush here. */
        nn20clock_ota_restart();
        return;
    case NN20CLOCK_OTA_AVAILABLE:
        (void)nn20clock_ota_install(ui->ota);
        break;
    default:
        (void)nn20clock_ota_check(ui->ota);
        break;
    }

    /* Redraw at once rather than waiting up to a second for the tick:
     * a button that does nothing visible for a second reads as broken
     * and gets pressed again. */
    nn20clock_ota_status(ui->ota, &status);
    refresh_ota(ui, &status);
}

/*
 * Build the section. Returns with the controls either live or, when
 * this build has no updater, replaced by a line saying so - rather than
 * a button that cannot work.
 */
static void add_about_updates(DeviceUi *ui)
{
    add_about_gap(ui, 24);

    if (ui->ota == NULL) {
        add_about_line(ui, "This build cannot update itself.",
                       &lv_font_montserrat_28, COLOR_TEXT);
        return;
    }

    ui->ota_status = lv_label_create(ui->body);
    lv_label_set_long_mode(ui->ota_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(ui->ota_status, LV_PCT(90));
    lv_obj_set_style_text_font(ui->ota_status, &lv_font_montserrat_28,
                               LV_PART_MAIN);
    lv_obj_set_style_text_color(ui->ota_status, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_align(ui->ota_status, LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN);

    ui->ota_bar = lv_bar_create(ui->body);
    lv_obj_set_size(ui->ota_bar, LV_PCT(80), 12);
    lv_obj_set_style_flex_grow(ui->ota_bar, 0, LV_PART_MAIN);
    lv_bar_set_range(ui->ota_bar, 0, 100);
    lv_obj_set_style_bg_color(ui->ota_bar, COLOR_SURFACE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ui->ota_bar, COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_add_flag(ui->ota_bar, LV_OBJ_FLAG_HIDDEN);

    ui->ota_button = lv_button_create(ui->body);
    lv_obj_set_size(ui->ota_button, LV_PCT(70), 80);
    lv_obj_set_style_flex_grow(ui->ota_button, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(ui->ota_button, 16, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ui->ota_button, COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_add_event_cb(ui->ota_button, on_ota_action, LV_EVENT_CLICKED, ui);

    ui->ota_button_label = lv_label_create(ui->ota_button);
    lv_obj_set_style_text_font(ui->ota_button_label,
                               &nn20clock_font_ui_bold_28, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui->ota_button_label, lv_color_black(),
                                LV_PART_MAIN);
    lv_obj_center(ui->ota_button_label);

    /*
     * Draw from wherever a previous visit to this screen left the
     * update: a download started here and still running has to be
     * reported, not restarted.
     *
     * ota_shown is deliberately set to something no status can be, so
     * the first refresh always draws the button rather than deciding it
     * already matches.
     */
    ui->ota_shown = (NN20ClockOtaState)-1;

    NN20ClockOtaStatus status;
    nn20clock_ota_status(ui->ota, &status);
    refresh_ota(ui, &status);
}

static void show_about(DeviceUi *ui)
{
    clear_view(ui);
    ui->view = make_view(ui, "About", on_back_to_menu);
    if (ui->view == NULL) {
        return;
    }

    const esp_app_desc_t *app = esp_app_get_description();

    add_about_gap(ui, 20);
    add_about_line(ui, "Video Alarm Clock", &nn20clock_font_ui_bold_28,
                   COLOR_TEXT);
    add_about_line(ui, (app != NULL) ? app->version : "unknown version",
                   &lv_font_montserrat_28, COLOR_ACCENT);

    add_about_gap(ui, 40);
    /* Everything on this screen is something somebody came here to
     * read, so none of it is drawn in the muted colour used for
     * decoration. */
    add_about_line(ui, "2026 (C) Bruno Keymolen", &lv_font_montserrat_28,
                   COLOR_TEXT);
    add_about_line(ui, "bruno.keymolen@gmail.com", &lv_font_montserrat_28,
                   COLOR_TEXT);

    /*
     * The build date, because "which version" and "which build" are
     * different questions when the version ends in -dirty - and that is
     * exactly when somebody is trying to work out what they flashed.
     */
    if (app != NULL) {
        char built[64];
        snprintf(built, sizeof(built), "built %s %s", app->date, app->time);
        add_about_gap(ui, 24);
        add_about_line(ui, built, &lv_font_montserrat_28, COLOR_TEXT);
    }

    add_about_updates(ui);

    /* Last, so nothing is polled before it is built. */
    ui->about_open = true;
}

/* -------------------------------------------------------------- time -- */

/*
 * Time settings, for a clock with no internet.
 *
 * The switch is the whole story: with the sync on there is nothing to
 * set, because anything typed in would be overwritten within minutes,
 * so the rollers are not drawn at all rather than drawn and ignored.
 * Turn it off and they appear.
 *
 * Five rollers and no seconds. Setting a clock by hand to the second is
 * a thing nobody can do anyway, and the sixth roller is what would push
 * this off the screen.
 */
#define ROLLER_WIDTH 118
#define ROLLER_ROWS  3
#define YEAR_FIRST   2025u
#define YEAR_COUNT   26u      /* 2025..2050, which outlives the hardware */

static void show_time(DeviceUi *ui);

/* "01\n02\n...\nN" - the roller wants one string with newlines. */
static void number_options(char *out, size_t size, unsigned first,
                           unsigned count, unsigned width)
{
    size_t used = 0;
    for (unsigned i = 0; i < count && used < size; i++) {
        const int wrote = snprintf(out + used, size - used, "%0*u%s",
                                   (int)width, first + i,
                                   (i + 1 < count) ? "\n" : "");
        if (wrote <= 0) {
            break;
        }
        used += (size_t)wrote;
    }
}

static lv_obj_t *make_time_roller(lv_obj_t *parent, const char *options,
                                  uint32_t selected)
{
    lv_obj_t *roller = lv_roller_create(parent);
    lv_roller_set_options(roller, options, LV_ROLLER_MODE_NORMAL);

    /*
     * The styles BEFORE the row count, and the order matters:
     * lv_roller_set_visible_row_count() turns rows into pixels there
     * and then, from whatever font the roller has at that moment.
     * Setting it first sized these five wheels to three rows of the
     * 14 px default while they drew at 28 - about one row's worth of
     * height for three rows of digits. The same mistake was in the
     * alarm editor's wheels.
     */
    lv_obj_set_style_bg_color(roller, COLOR_SURFACE, LV_PART_MAIN);
    lv_obj_set_style_text_color(roller, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(roller, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_set_style_text_line_space(roller, 4, LV_PART_MAIN);
    lv_obj_set_style_border_width(roller, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(roller, COLOR_ACCENT, LV_PART_SELECTED);
    lv_obj_set_style_text_color(roller, lv_color_black(), LV_PART_SELECTED);

    lv_roller_set_visible_row_count(roller, ROLLER_ROWS);
    lv_obj_set_width(roller, ROLLER_WIDTH);

    lv_roller_set_selected(roller, selected, LV_ANIM_OFF);
    return roller;
}

static void on_ntp_toggled(lv_event_t *event)
{
    DeviceUi *ui = lv_event_get_user_data(event);
    const bool enabled =
        lv_obj_has_state(lv_event_get_target(event), LV_STATE_CHECKED);

    ui->config.ntp_enabled = enabled;

    /*
     * Applied and stored together. Unlike a slider this fires once, on
     * a deliberate tap, so there is no reason to defer the write - and
     * a clock that forgets this across a restart would sync itself
     * again behind the user's back.
     */
    if (ui->service.set_ntp != NULL) {
        (void)ui->service.set_ntp(ui->service.ctx, enabled);
    }
    if (ui->service.save_config(ui->service.ctx, &ui->config) != ESP_OK) {
        ESP_LOGE(TAG, "could not store the time sync setting");
    }

    show_time(ui);   /* the rollers appear or go away */
}

/* Collect the five rollers into the wall clock. */
static void on_set_time(lv_event_t *event)
{
    DeviceUi *ui = lv_event_get_user_data(event);
    if (ui->roller_day == NULL || ui->service.set_time == NULL) {
        return;
    }

    struct tm wanted = {0};
    wanted.tm_mday = (int)lv_roller_get_selected(ui->roller_day) + 1;
    wanted.tm_mon = (int)lv_roller_get_selected(ui->roller_month);
    wanted.tm_year =
        (int)(YEAR_FIRST + lv_roller_get_selected(ui->roller_year)) - 1900;
    wanted.tm_hour = (int)lv_roller_get_selected(ui->roller_hour);
    wanted.tm_min = (int)lv_roller_get_selected(ui->roller_minute);
    wanted.tm_sec = 0;
    /*
     * Let mktime work out whether this local time is in daylight saving
     * rather than asserting it. -1 means "you decide"; saying 0 here is
     * how a hand-set clock ends up an hour out for half the year.
     */
    wanted.tm_isdst = -1;

    const time_t when = mktime(&wanted);
    if (when == (time_t)-1) {
        ESP_LOGW(TAG, "that is not a date");
        return;
    }

    if (ui->service.set_time(ui->service.ctx, when) != ESP_OK) {
        ESP_LOGE(TAG, "could not set the clock");
        return;
    }
    show_menu(ui);
}

static void show_time(DeviceUi *ui)
{
    clear_view(ui);
    ui->view = make_view(ui, "Time", on_back_to_menu);
    if (ui->view == NULL) {
        return;
    }

    /* ---- the switch ---- */
    lv_obj_t *row = lv_obj_create(ui->body);
    lv_obj_set_size(row, LV_PCT(100), ROW_HEIGHT);
    lv_obj_set_style_flex_grow(row, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(row, COLOR_SURFACE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 16, LV_PART_MAIN);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(row);
    lv_label_set_text(title, "Set from the internet");
    lv_obj_set_style_text_font(title, &nn20clock_font_ui_bold_28,
                               LV_PART_MAIN);
    lv_obj_set_style_text_color(title, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *hint = lv_label_create(row);
    lv_label_set_text(hint, ui->config.ntp_enabled
                                ? "On. The clock keeps itself right."
                                : "Off. Set it by hand below.");
    /* Bounded so it cannot reach the switch on the right of the row. */
    lv_label_set_long_mode(hint, LV_LABEL_LONG_DOT);
    lv_obj_set_width(hint, LV_PCT(78));
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_set_style_text_color(hint, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    ui->ntp_switch = lv_switch_create(row);
    lv_obj_set_size(ui->ntp_switch, 90, 48);
    lv_obj_align(ui->ntp_switch, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(ui->ntp_switch, COLOR_ACCENT,
                              LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (ui->config.ntp_enabled) {
        lv_obj_add_state(ui->ntp_switch, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(ui->ntp_switch, on_ntp_toggled, LV_EVENT_VALUE_CHANGED,
                        ui);

    if (ui->config.ntp_enabled) {
        /* Nothing else to offer: the rollers would be a control that
         * does nothing, since the next sync would undo it. */
        return;
    }

    /* ---- the rollers, seeded with the clock as it stands ---- */
    const time_t now = time(NULL);
    struct tm local = {0};
    if (localtime_r(&now, &local) == NULL) {
        return;
    }

    ui->clock_rollers = lv_obj_create(ui->body);
    lv_obj_set_width(ui->clock_rollers, LV_PCT(100));
    lv_obj_set_height(ui->clock_rollers, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(ui->clock_rollers, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(ui->clock_rollers, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ui->clock_rollers, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_column(ui->clock_rollers, 6, LV_PART_MAIN);
    lv_obj_remove_flag(ui->clock_rollers, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(ui->clock_rollers, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ui->clock_rollers, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* 31 days always. A short month is caught by mktime() normalising
     * the date, which is a better answer than a roller that reshuffles
     * itself under a finger. */
    char options[YEAR_COUNT * 8];
    number_options(options, sizeof(options), 1u, 31u, 2u);
    ui->roller_day = make_time_roller(ui->clock_rollers, options,
                                      (uint32_t)local.tm_mday - 1u);
    number_options(options, sizeof(options), 1u, 12u, 2u);
    ui->roller_month = make_time_roller(ui->clock_rollers, options,
                                        (uint32_t)local.tm_mon);
    number_options(options, sizeof(options), YEAR_FIRST, YEAR_COUNT, 4u);
    ui->roller_year = make_time_roller(
        ui->clock_rollers, options,
        (uint32_t)((local.tm_year + 1900) - (int)YEAR_FIRST));
    number_options(options, sizeof(options), 0u, 24u, 2u);
    ui->roller_hour = make_time_roller(ui->clock_rollers, options,
                                       (uint32_t)local.tm_hour);
    number_options(options, sizeof(options), 0u, 60u, 2u);
    ui->roller_minute = make_time_roller(ui->clock_rollers, options,
                                         (uint32_t)local.tm_min);

    /*
     * One caption per wheel, in a row laid out exactly like the wheels
     * above it. It used to be a single string with the gaps typed into
     * it, which lined up at one font size and one wheel width and at no
     * other - and both have now changed once.
     *
     * Muted and not brightened with the rest: these name the controls,
     * they are not the setting. The numbers above them are.
     */
    static const char *const ROLLER_CAPTIONS[5] = {
        "day", "month", "year", "hour", "min"
    };

    lv_obj_t *captions = lv_obj_create(ui->body);
    lv_obj_set_width(captions, LV_PCT(100));
    lv_obj_set_height(captions, LV_SIZE_CONTENT);
    lv_obj_set_style_flex_grow(captions, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(captions, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(captions, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(captions, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_column(captions, 6, LV_PART_MAIN);
    lv_obj_remove_flag(captions, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(captions, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(captions, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    for (size_t i = 0; i < 5u; i++) {
        lv_obj_t *caption = lv_label_create(captions);
        lv_label_set_text(caption, ROLLER_CAPTIONS[i]);
        lv_obj_set_width(caption, ROLLER_WIDTH);
        lv_obj_set_style_text_align(caption, LV_TEXT_ALIGN_CENTER,
                                    LV_PART_MAIN);
        lv_obj_set_style_text_font(caption, &lv_font_montserrat_28,
                                   LV_PART_MAIN);
        lv_obj_set_style_text_color(caption, COLOR_MUTED, LV_PART_MAIN);
    }

    lv_obj_t *apply = lv_button_create(ui->body);
    lv_obj_set_size(apply, LV_PCT(60), 80);
    lv_obj_set_style_flex_grow(apply, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(apply, 16, LV_PART_MAIN);
    lv_obj_set_style_bg_color(apply, COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_add_event_cb(apply, on_set_time, LV_EVENT_CLICKED, ui);

    lv_obj_t *apply_label = lv_label_create(apply);
    lv_label_set_text(apply_label, "Set the clock");
    lv_obj_set_style_text_font(apply_label, &nn20clock_font_ui_bold_28,
                               LV_PART_MAIN);
    lv_obj_set_style_text_color(apply_label, lv_color_black(), LV_PART_MAIN);
    lv_obj_center(apply_label);
}

/* ------------------------------------------------------------ volume -- */

static void on_volume_changed(lv_event_t *event)
{
    DeviceUi *ui = lv_event_get_user_data(event);
    ui->config.volume = (uint8_t)lv_slider_get_value(
        lv_event_get_target(event));
    set_readout(ui, ui->config.volume);
}

static void on_volume_done(lv_event_t *event)
{
    DeviceUi *ui = lv_event_get_user_data(event);

    if (ui->service.save_config(ui->service.ctx, &ui->config) != ESP_OK) {
        ESP_LOGE(TAG, "could not store volume");
    }
    show_menu(ui);
}

static void show_volume(DeviceUi *ui)
{
    clear_view(ui);
    ui->view = make_view(ui, "Volume", on_volume_done);
    if (ui->view == NULL) {
        return;
    }

    lv_obj_t *slider = lv_slider_create(ui->body);
    lv_obj_set_width(slider, LV_PCT(SLIDER_WIDTH_PCT));
    lv_obj_set_height(slider, 48);
    lv_slider_set_range(slider, 0, 100);
    lv_slider_set_value(slider, ui->config.volume, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, COLOR_ACCENT, LV_PART_KNOB);
    lv_obj_add_event_cb(slider, on_volume_changed, LV_EVENT_VALUE_CHANGED,
                        ui);

    add_readout(ui, ui->config.volume);

    /*
     * The one volume on the device: alarms and the media the user picks
     * both ring at it. Said on the screen because an alarm that turns
     * out to be quieter than expected is not a thing to discover at
     * 7am.
     *
     * The number is stored on the way out and read when playback
     * starts. Dragging it changes nothing you can hear right now - not
     * an oversight, there is simply nothing playing to change; design
     * 17's Milestone 9 puts a volume slider inside playback, which is
     * where it can be heard.
     */
    lv_obj_t *hint = lv_label_create(ui->body);
    lv_label_set_text(hint,
                      "Used by alarms and by media you play.\n"
                      "Takes effect the next time something plays.");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_set_style_text_color(hint, COLOR_TEXT, LV_PART_MAIN);
}

/* ----------------------------------------------------------- vtable -- */

static esp_err_t device_ui_show(NN20ClockUiBase *base)
{
    DeviceUi *ui = (DeviceUi *)base;

    /* A container on the display's permanent screen, never a screen of
     * its own - deleting the active screen leaves LVGL holding a
     * dangling pointer. Same reason as every other screen here. */
    ui->root = lv_obj_create(lv_screen_active());
    if (ui->root == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_set_size(ui->root, LV_PCT(100), LV_PCT(100));
    lv_obj_center(ui->root);
    lv_obj_set_style_bg_color(ui->root, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui->root, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(ui->root, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(ui->root, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ui->root, 0, LV_PART_MAIN);
    lv_obj_remove_flag(ui->root, LV_OBJ_FLAG_SCROLLABLE);

    if (ui->service.load_config(ui->service.ctx, &ui->config) != ESP_OK) {
        ESP_LOGE(TAG, "cannot read the configuration");
        (void)nn20clock_storage_default_config(&ui->config);
    }

    show_menu(ui);
    ESP_LOGI(TAG, "settings shown");
    return ESP_OK;
}

static esp_err_t device_ui_hide(NN20ClockUiBase *base)
{
    DeviceUi *ui = (DeviceUi *)base;
    if (ui->root != NULL) {
        lv_obj_add_flag(ui->root, LV_OBJ_FLAG_HIDDEN);
    }
    return ESP_OK;
}

static esp_err_t device_ui_timer_event(NN20ClockUiBase *base,
                                       const NN20ClockTimerEvent *event)
{
    DeviceUi *ui = (DeviceUi *)base;

    /*
     * This screen shows no time. It uses the second tick as the poll
     * the About view's update controls need - the one place in this
     * project that already delivers something regular on the UiWorker,
     * so no timer of its own is created for it.
     */
    if (event != NULL && event->type == NN20CLOCK_TIMER_EVENT_SECOND &&
        ui->about_open && ui->ota != NULL) {
        NN20ClockOtaStatus status;
        nn20clock_ota_status(ui->ota, &status);
        refresh_ota(ui, &status);
    }
    return ESP_OK;
}

static void device_ui_destroy(NN20ClockUiBase *base)
{
    DeviceUi *ui = (DeviceUi *)base;

    if (ui->root != NULL) {
        lv_obj_delete(ui->root);
        ui->root = NULL;
        ui->view = NULL;
        ui->body = NULL;
        ui->password_input = NULL;
        ui->about_open = false;
        ui->ota_status = NULL;
        ui->ota_bar = NULL;
        ui->ota_button = NULL;
        ui->ota_button_label = NULL;
    }

    nn20clock_ui_base_deinit(base);
    free(ui);
}

static const NN20ClockUiVTable DEVICE_UI_VTABLE = {
    .show = device_ui_show,
    .hide = device_ui_hide,
    .handle_touch = NULL,   /* LVGL routes touch to the widgets */
    .handle_timer_event = device_ui_timer_event,
    .destroy = device_ui_destroy,
};

/* ------------------------------------------------------------- ctor -- */

NN20ClockUiBase *nn20clock_device_ui_ctor(nn20_worker_ctx *ui_worker,
                                          NN20ClockManager *manager,
                                          NN20ClockUiCommandFn on_command,
                                          NN20ClockDeviceService service,
                                          NN20ClockNet *net,
                                          NN20ClockOta *ota)
{
    if (ui_worker == NULL) {
        ESP_LOGE(TAG, "no UiWorker");
        return NULL;
    }
    if (service.load_config == NULL || service.save_config == NULL ||
        service.apply_brightness == NULL || service.connect_wifi == NULL) {
        ESP_LOGE(TAG, "incomplete device service");
        return NULL;
    }

    DeviceUi *ui = calloc(1, sizeof(*ui));
    if (ui == NULL) {
        ESP_LOGE(TAG, "out of memory");
        return NULL;
    }

    const NN20ClockUiBaseConfig config = {
        .vtable = &DEVICE_UI_VTABLE,
        .name = "DeviceSettingsUi",
        .worker = ui_worker,
        .manager = manager,
        .on_command = on_command,
    };
    if (nn20clock_ui_base_init(&ui->super, &config) != ESP_OK) {
        free(ui);
        return NULL;
    }

    ui->service = service;
    ui->net = net;
    ui->ota = ota;
    /* No LVGL call has happened here, and none may: this runs on
     * whichever worker asked for the screen. */
    return &ui->super;
}
