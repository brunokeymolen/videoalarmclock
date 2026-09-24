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
 * nn20clock_time_ui.c - the clock face (design 11, TimeUi).
 *
 * Three labels on a black screen: the time, the date, and a status line
 * that says whether the clock has been set. Everything here runs on the
 * UiWorker, because everything here is reached through the UiBase
 * dispatcher.
 *
 * The one rule worth stating twice: the constructor does not touch
 * LVGL. ClockManager calls it from its own worker when a state is
 * entered, and the widgets are built in show(), which the dispatcher
 * runs on the UiWorker.
 */
#include "nn20clock_time_ui.h"

#include <stdlib.h>
#include <string.h>

#include "nn20clock_fonts.h"
#include "nn20clock_player.h"
#include "nn20clock_timefmt.h"
#include "lvgl.h"

static const char *TAG = "NN20CLOCK_TIME_UI";

/*
 * Design 11: light blue digits on black.
 *
 * The digits use a generated 220 px font (fonts/, and tools/gen-fonts.sh
 * to rebuild it) - about 640 px of the 720 px panel. The first version of this screen scaled LVGL's
 * built-in 48 px Montserrat up with transform_scale instead, which on a
 * 720x720 panel produced small digits - the transform does not scale a
 * label's glyphs - and would have looked like a stretched bitmap even
 * if it had. The date and status lines are small enough that the
 * built-in faces are right for them.
 */
#define COLOR_DIGITS  lv_color_hex(0x7FC4FF)
#define COLOR_DATE    lv_color_hex(0x4A6A85)
#define COLOR_STATUS  lv_color_hex(0x3A5468)

/* Neither true nor false: nothing has been rendered yet. */
#define SYNC_UNKNOWN (-1)

/*
 * Below this year the clock has clearly never been set - a board with no
 * RTC battery comes up at the epoch, and 1 January 1970 is not a time
 * anyone wants to read on their bedside clock while NTP is still
 * connecting. Design 14 allows a valid RTC as a time source, so a
 * plausible date is shown even before NTP confirms it; an implausible
 * one is shown as placeholders instead.
 *
 * The threshold only has to be later than the epoch and earlier than
 * any real use of the device.
 */
#define PLAUSIBLE_YEAR 2025u

typedef struct {
    NN20ClockUiBase super;   /* first: a UiBase* casts to this */

    /*
     * A container on the display's permanent screen, not a screen of its
     * own. LVGL always has exactly one active screen, and deleting that
     * screen leaves it holding a dangling pointer - which faults on the
     * UI thread a moment later, somewhere unrelated.
     *
     * Design 8 has ClockManager destroy the outgoing screen before
     * building the next one, so a screen that owns its lv_screen would
     * hit that on every switch. Owning a container instead means
     * destroying a screen never removes the active one: the container
     * goes, the screen stays.
     */
    lv_obj_t *root;
    lv_obj_t *time_label;
    lv_obj_t *date_label;
    lv_obj_t *status_label;
    lv_obj_t *alarm_button;
    lv_obj_t *play_button;
    lv_obj_t *gear_button;

    /* The snoozed-alarm reminder, and the last frame it draws. The
     * descriptor points into the player's buffer and is filled in once
     * the player says there is a picture. */
    NN20ClockPlayer *player;      /* borrowed; may be NULL */
    lv_obj_t *snooze_box;
    lv_obj_t *snooze_image;
    lv_obj_t *snooze_glyph;
    lv_image_dsc_t snooze_dsc;
    bool shown_snooze;
    /* One-shot, restarted by every touch. Owned by this screen and
     * deleted with it - an LVGL timer outliving the object it points at
     * would fire into freed memory. */
    lv_timer_t *icon_timer;

    /* What is on screen now, so a redraw that would change nothing is
     * skipped. At one event a second and a 720x720 panel, not
     * re-rendering is worth the two comparisons. */
    char shown_time[NN20CLOCK_HHMM_SIZE];
    char shown_date[NN20CLOCK_DATE_SIZE];
    /* Tri-state: SYNC_UNKNOWN until the first event, so the status line
     * is written even when the first reading is "not synced". */
    int shown_synced;
} TimeUi;

/* Runs on the UiWorker. */
static void render(TimeUi *ui, const NN20ClockDateTime *now, bool synced)
{
    /*
     * A time from an unset clock is not worth drawing. Showing
     * "1970-01-01" for the half minute before NTP answers looks like a
     * fault; placeholders plus "clock not set" say exactly what is
     * happening.
     */
    const bool believable = (now->year >= PLAUSIBLE_YEAR);

    char text[NN20CLOCK_HHMM_SIZE];
    if (believable) {
        (void)nn20clock_timefmt_hhmm(now, text, sizeof(text));
    } else {
        snprintf(text, sizeof(text), "--:--");
    }
    if (strcmp(text, ui->shown_time) != 0) {
        lv_label_set_text(ui->time_label, text);
        snprintf(ui->shown_time, sizeof(ui->shown_time), "%s", text);
    }

    char date[NN20CLOCK_DATE_SIZE];
    if (believable) {
        (void)nn20clock_timefmt_date(now, date, sizeof(date));
    } else {
        /* Blank rather than dashes: one placeholder on screen is a
         * clock waiting for the time, two looks broken. */
        date[0] = '\0';
    }
    if (strcmp(date, ui->shown_date) != 0) {
        if (date[0] == '\0') {
            lv_label_set_text(ui->date_label, "");
        } else {
            lv_label_set_text_fmt(ui->date_label, "%s %s",
                                  nn20clock_timefmt_weekday(now->weekday),
                                  date);
        }
        snprintf(ui->shown_date, sizeof(ui->shown_date), "%s", date);
    }

    if ((int)synced != ui->shown_synced) {
        /*
         * Saying so matters: until NTP lands the displayed time is
         * whatever the RTC believed, and an alarm clock that is
         * silently hours out is worse than one that admits it.
         */
        lv_label_set_text(ui->status_label, synced ? "" : "clock not set");
        ui->shown_synced = (int)synced;
    }
}

/* ------------------------------------------------------------ icons -- */

/*
 * Design 11: the touch entry points are icons, not the whole screen.
 * Tapping anywhere used to open the settings, which meant brushing the
 * display changed what it was showing.
 *
 * The glyphs are LVGL's built-in symbols, so no icon assets are needed -
 * they are part of the Montserrat fonts already compiled in.
 */
#define ICON_SIZE     96
#define ICON_MARGIN   24

/*
 * The icons are hidden until the screen is touched, and hide again after
 * this long without a touch. The normal state of a bedside clock is the
 * time and nothing else - which matters more at night than it does in a
 * screenshot.
 */
#define ICON_VISIBLE_MS 10000

/*
 * The snoozed alarm, top right.
 *
 * A snooze is the one piece of scheduler state a bedside clock must not
 * keep to itself - an alarm you think you turned off and only rested is
 * how you end up woken at the wrong moment. So it is shown, and it is
 * shown as the frame the alarm was interrupted on rather than as an
 * icon: that says which alarm at a glance, without a word of text.
 *
 * Unlike the settings icons this does not hide with the others. It is
 * information about what the clock is about to do, not a control, and
 * the whole point is that it is there when nobody is looking.
 */
#define SNOOZE_MARGIN 24

static void set_icons_visible(TimeUi *ui, bool visible)
{
    if (ui->alarm_button == NULL || ui->play_button == NULL ||
        ui->gear_button == NULL) {
        return;
    }
    if (visible) {
        lv_obj_remove_flag(ui->alarm_button, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(ui->play_button, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(ui->gear_button, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(ui->alarm_button, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui->play_button, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui->gear_button, LV_OBJ_FLAG_HIDDEN);
    }
}

/* The idle timeout expired: back to just the time. Runs on the UiWorker,
 * like every LVGL timer callback. */
static void on_icon_timeout(lv_timer_t *timer)
{
    TimeUi *ui = lv_timer_get_user_data(timer);

    set_icons_visible(ui, false);

    /*
     * Pausing is what makes this a one-shot, and it has to be done here
     * rather than with lv_timer_set_repeat_count(..., 1).
     *
     * A repeat count that reaches zero makes LVGL DELETE the timer
     * (lv_timer.c: "The repeat count is over, delete the timer"), and
     * this screen keeps the pointer to restart it on the next touch.
     * Writing through it afterwards - lv_timer_reset() stores a tick
     * into the freed block - corrupted the heap, and the device then
     * died in an unrelated allocation minutes later, inside LVGL's
     * glyph rendering. A timer this screen owns must outlive its own
     * firing.
     */
    lv_timer_pause(timer);
}

/*
 * Any touch on the screen reveals the icons and restarts the countdown -
 * including a touch on an icon, so using one does not make the other
 * disappear mid-reach.
 */
static void on_screen_touch(lv_event_t *event)
{
    TimeUi *ui = lv_event_get_user_data(event);

    set_icons_visible(ui, true);
    if (ui->icon_timer != NULL) {
        lv_timer_reset(ui->icon_timer);
        lv_timer_resume(ui->icon_timer);
    }
}

/* Runs on the UiWorker: LVGL calls event handlers from
 * lv_timer_handler, which is the same thread everything else here runs
 * on. */
static void on_alarm_icon(lv_event_t *event)
{
    NN20ClockUiBase *base = lv_event_get_user_data(event);

    const NN20ClockUiCommand command = {
        .type = NN20CLOCK_UI_COMMAND_OPEN_SETTINGS,
    };
    (void)nn20clock_ui_send_command(base, &command);
}

/* Design 17, Milestone 8: something to watch, chosen rather than rung. */
static void on_play_icon(lv_event_t *event)
{
    NN20ClockUiBase *base = lv_event_get_user_data(event);

    const NN20ClockUiCommand command = {
        .type = NN20CLOCK_UI_COMMAND_OPEN_MEDIA_PLAYBACK,
    };
    (void)nn20clock_ui_send_command(base, &command);
}

static void on_gear_icon(lv_event_t *event)
{
    NN20ClockUiBase *base = lv_event_get_user_data(event);

    const NN20ClockUiCommand command = {
        .type = NN20CLOCK_UI_COMMAND_OPEN_DEVICE_SETTINGS,
    };
    (void)nn20clock_ui_send_command(base, &command);
}

/*
 * The reminder was pressed: the alarm is over rather than resting.
 *
 * The manager cancels the pending occurrence; this screen does not hide
 * the box itself, because the Timer is the thing that knows whether the
 * snooze is really gone. It disappears on the next event, which the
 * manager asks for immediately.
 */
static void on_snooze_box(lv_event_t *event)
{
    NN20ClockUiBase *base = lv_event_get_user_data(event);

    const NN20ClockUiCommand command = {
        .type = NN20CLOCK_UI_COMMAND_CANCEL_SNOOZE,
    };
    (void)nn20clock_ui_send_command(base, &command);
}

/*
 * Show or hide the snoozed-alarm reminder.
 *
 * The picture is only attached once, the first time the player has one:
 * an lv_image_dsc_t pointing into the player's thumbnail buffer, which
 * stays put until the next alarm plays. When there is no picture - an
 * alarm that rang on the built-in tone - a bell is drawn instead, so
 * the reminder is always there to be pressed.
 */
static void set_snooze_visible(TimeUi *ui, bool visible)
{
    if (ui->snooze_box == NULL) {
        return;
    }

    if (!visible) {
        lv_obj_add_flag(ui->snooze_box, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    const void *pixels = nn20clock_player_thumbnail(ui->player);
    if (pixels != NULL && ui->snooze_image != NULL) {
        ui->snooze_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        ui->snooze_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        ui->snooze_dsc.header.w = NN20CLOCK_PLAYER_THUMBNAIL_SIZE;
        ui->snooze_dsc.header.h = NN20CLOCK_PLAYER_THUMBNAIL_SIZE;
        ui->snooze_dsc.header.stride =
            NN20CLOCK_PLAYER_THUMBNAIL_SIZE * sizeof(uint16_t);
        ui->snooze_dsc.data_size = (uint32_t)NN20CLOCK_PLAYER_THUMBNAIL_SIZE *
                                   NN20CLOCK_PLAYER_THUMBNAIL_SIZE *
                                   sizeof(uint16_t);
        ui->snooze_dsc.data = pixels;

        lv_image_set_src(ui->snooze_image, &ui->snooze_dsc);
        lv_obj_remove_flag(ui->snooze_image, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui->snooze_glyph, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(ui->snooze_image, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(ui->snooze_glyph, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_remove_flag(ui->snooze_box, LV_OBJ_FLAG_HIDDEN);
}

/* A round, flat icon button. Returns NULL only if LVGL is out of
 * memory, which the caller treats as a failed show. */
static lv_obj_t *make_icon(lv_obj_t *parent, const char *symbol,
                           lv_color_t color, lv_align_t align, int32_t x,
                           lv_event_cb_t handler, void *user_data)
{
    lv_obj_t *button = lv_button_create(parent);
    if (button == NULL) {
        return NULL;
    }

    lv_obj_set_size(button, ICON_SIZE, ICON_SIZE);
    lv_obj_align(button, align, x, -ICON_MARGIN);
    lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    /* Flat on black: the icon is the affordance, not a raised button. */
    lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(button, 0, LV_PART_MAIN);
    /* Visible feedback on press, since there is no other cue that a
     * touch registered. */
    lv_obj_set_style_bg_opa(button, LV_OPA_20, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(button, color,
                              LV_PART_MAIN | LV_STATE_PRESSED);

    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, symbol);
    lv_obj_set_style_text_color(label, color, LV_PART_MAIN);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_center(label);

    if (handler != NULL) {
        lv_obj_add_event_cb(button, handler, LV_EVENT_CLICKED, user_data);
    }
    return button;
}

/* ----------------------------------------------------------- vtable -- */

static esp_err_t time_ui_show(NN20ClockUiBase *base)
{
    TimeUi *ui = (TimeUi *)base;

    /* Built here, not in the constructor: this is the UiWorker. */
    ui->root = lv_obj_create(lv_screen_active());
    if (ui->root == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_set_size(ui->root, LV_PCT(100), LV_PCT(100));
    lv_obj_center(ui->root);
    lv_obj_set_style_bg_color(ui->root, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui->root, LV_OPA_COVER, LV_PART_MAIN);
    /* No border, padding, or scrolling: this is a backdrop, and LVGL's
     * default object style is a rounded grey card. */
    lv_obj_set_style_border_width(ui->root, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(ui->root, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ui->root, 0, LV_PART_MAIN);
    lv_obj_remove_flag(ui->root, LV_OBJ_FLAG_SCROLLABLE);

    ui->time_label = lv_label_create(ui->root);
    lv_label_set_text(ui->time_label, "--:--");
    lv_obj_set_style_text_color(ui->time_label, COLOR_DIGITS, LV_PART_MAIN);
    lv_obj_set_style_text_font(ui->time_label, &nn20clock_font_clock_220,
                               LV_PART_MAIN);
    /* Slightly above centre: the date sits below it, and the pair reads
     * better a little high than mathematically centred. */
    lv_obj_align(ui->time_label, LV_ALIGN_CENTER, 0, -60);

    ui->date_label = lv_label_create(ui->root);
    lv_label_set_text(ui->date_label, "");
    lv_obj_set_style_text_color(ui->date_label, COLOR_DATE, LV_PART_MAIN);
    /* 48 is the largest built-in Montserrat. The date is a supporting
     * line, not the headline, so it does not warrant a generated font of
     * its own - it needs letters, which would cost far more flash than
     * the twelve glyphs the digits use. */
    lv_obj_set_style_text_font(ui->date_label, &lv_font_montserrat_48,
                               LV_PART_MAIN);
    lv_obj_align(ui->date_label, LV_ALIGN_CENTER, 0, 150);

    ui->status_label = lv_label_create(ui->root);
    lv_label_set_text(ui->status_label, "clock not set");
    lv_obj_set_style_text_color(ui->status_label, COLOR_STATUS, LV_PART_MAIN);
    lv_obj_set_style_text_font(ui->status_label, &lv_font_montserrat_28,
                               LV_PART_MAIN);
    lv_obj_align(ui->status_label, LV_ALIGN_BOTTOM_MID, 0, -40);

    /*
     * The snoozed-alarm reminder, top right, out of the digits' way.
     * Built here and hidden; a timer event with a pending snooze
     * reveals it.
     */
    ui->snooze_box = lv_obj_create(ui->root);
    if (ui->snooze_box == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_set_size(ui->snooze_box, NN20CLOCK_PLAYER_THUMBNAIL_SIZE,
                    NN20CLOCK_PLAYER_THUMBNAIL_SIZE);
    lv_obj_align(ui->snooze_box, LV_ALIGN_TOP_RIGHT, -SNOOZE_MARGIN,
                 SNOOZE_MARGIN);
    lv_obj_set_style_bg_color(ui->snooze_box, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui->snooze_box, LV_OPA_COVER, LV_PART_MAIN);
    /* A thin border so a dark frame still reads as a picture and not as
     * a hole in the screen. */
    lv_obj_set_style_border_color(ui->snooze_box, COLOR_DATE, LV_PART_MAIN);
    lv_obj_set_style_border_width(ui->snooze_box, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(ui->snooze_box, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ui->snooze_box, 0, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(ui->snooze_box, true, LV_PART_MAIN);
    lv_obj_remove_flag(ui->snooze_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ui->snooze_box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ui->snooze_box, on_snooze_box, LV_EVENT_CLICKED,
                        base);

    ui->snooze_image = lv_image_create(ui->snooze_box);
    lv_obj_center(ui->snooze_image);
    lv_obj_add_flag(ui->snooze_image, LV_OBJ_FLAG_HIDDEN);

    /* Shown when the snoozed alarm had no picture - it rang on the
     * built-in tone - so there is always something to press. */
    ui->snooze_glyph = lv_label_create(ui->snooze_box);
    lv_label_set_text(ui->snooze_glyph, LV_SYMBOL_BELL);
    lv_obj_set_style_text_color(ui->snooze_glyph, COLOR_DIGITS, LV_PART_MAIN);
    lv_obj_set_style_text_font(ui->snooze_glyph, &lv_font_montserrat_48,
                               LV_PART_MAIN);
    lv_obj_center(ui->snooze_glyph);

    lv_obj_add_flag(ui->snooze_box, LV_OBJ_FLAG_HIDDEN);
    ui->shown_snooze = false;

    /*
     * Bottom corners, well clear of the digits. Always visible for now;
     * the design has them appearing for ~30 s after a touch later, so
     * that a bedside clock at night is the time and nothing else.
     */
    ui->alarm_button = make_icon(ui->root, LV_SYMBOL_BELL, COLOR_DIGITS,
                                 LV_ALIGN_BOTTOM_LEFT, ICON_MARGIN,
                                 on_alarm_icon, base);
    /* Between the two, as design 11 puts it - and centred, so the three
     * read as one row of controls rather than two corners and an
     * afterthought. */
    ui->play_button = make_icon(ui->root, LV_SYMBOL_PLAY, COLOR_DIGITS,
                                LV_ALIGN_BOTTOM_MID, 0, on_play_icon, base);
    ui->gear_button = make_icon(ui->root, LV_SYMBOL_SETTINGS, COLOR_DIGITS,
                                LV_ALIGN_BOTTOM_RIGHT, -ICON_MARGIN,
                                on_gear_icon, base);
    if (ui->alarm_button == NULL || ui->play_button == NULL ||
        ui->gear_button == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /*
     * Hidden to start with. A press anywhere - including on the icons
     * themselves, which is what EVENT_BUBBLE is for - reveals them and
     * restarts the countdown.
     */
    set_icons_visible(ui, false);
    lv_obj_add_flag(ui->alarm_button, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(ui->play_button, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(ui->gear_button, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_add_flag(ui->root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ui->root, on_screen_touch, LV_EVENT_PRESSED, ui);

    ui->icon_timer = lv_timer_create(on_icon_timeout, ICON_VISIBLE_MS, ui);
    if (ui->icon_timer != NULL) {
        /* One shot per touch: created paused, restarted by each press,
         * and paused again by its own callback. Deliberately NOT a
         * repeat count of one - see on_icon_timeout(). */
        lv_timer_pause(ui->icon_timer);
    }

    /* A screen shown after being hidden keeps its objects, so clear the
     * flag rather than assume it was never set. */
    lv_obj_remove_flag(ui->root, LV_OBJ_FLAG_HIDDEN);

    /* Nothing drawn yet is not the same as a time of 00:00 - leave the
     * placeholders until a Timer event says otherwise. The manager asks
     * the Timer to report as soon as a screen is shown, so that is a
     * fraction of a second, not a minute.
     *
     * shown_synced starts at a value the first event cannot match, so
     * the status line is always written once rather than assumed. */
    ui->shown_time[0] = '\0';
    ui->shown_date[0] = '\0';
    ui->shown_synced = SYNC_UNKNOWN;

    ESP_LOGI(TAG, "clock face shown");
    return ESP_OK;
}

static esp_err_t time_ui_hide(NN20ClockUiBase *base)
{
    TimeUi *ui = (TimeUi *)base;

    /* Hidden, not destroyed: the manager may show this screen again
     * without rebuilding it. destroy() is what frees the objects. */
    if (ui->root != NULL) {
        lv_obj_add_flag(ui->root, LV_OBJ_FLAG_HIDDEN);
    }
    return ESP_OK;
}

/*
 * Design 6 leaves filtering to the subscriber, and this screen is the
 * clearest case of it: a MINUTE event redraws, a TIME_SYNCED event
 * redraws because the whole time may have changed, and everything else
 * - SECOND ticks, alarm events - is ignored. Ignoring is a normal
 * outcome here, not an error.
 */
static esp_err_t time_ui_timer_event(NN20ClockUiBase *base,
                                     const NN20ClockTimerEvent *event)
{
    TimeUi *ui = (TimeUi *)base;

    if (ui->time_label == NULL) {
        return ESP_OK;   /* not shown yet */
    }
    if (event->type != NN20CLOCK_TIMER_EVENT_MINUTE &&
        event->type != NN20CLOCK_TIMER_EVENT_TIME_SYNCED) {
        return ESP_OK;
    }

    NN20ClockDateTime now;
    if (nn20clock_timer_to_local(event->payload->displayed_time, &now)
        != ESP_OK) {
        return ESP_FAIL;
    }

    /*
     * Straight from the payload. An earlier version inferred it from
     * having seen a TIME_SYNCED event, which meant a screen built after
     * the sync - opening the alarm settings and coming back does
     * exactly that - said "clock not set" for the rest of its life.
     */
    render(ui, &now, event->payload->time_synced);

    /*
     * Straight from the payload, like time_synced and for the same
     * reason: a screen built after the snooze was set has no other way
     * to learn about it. Only touched when it changes - re-attaching
     * the image every second would redraw a corner of the panel for
     * nothing.
     */
    if (event->payload->snooze_pending != ui->shown_snooze) {
        ui->shown_snooze = event->payload->snooze_pending;
        set_snooze_visible(ui, ui->shown_snooze);
    }
    return ESP_OK;
}

/*
 * Touch reaches this screen through LVGL's input device and its own
 * event handlers, not through this hook: LVGL knows which object was
 * pressed, and the icons are objects. The hook stays because UiBase
 * defines it and a screen without LVGL widgets - a future error screen,
 * say - may still want raw touches.
 *
 * It deliberately does nothing. An earlier version opened the settings
 * on any release anywhere, which meant brushing the display changed
 * what it was showing.
 */
static esp_err_t time_ui_touch(NN20ClockUiBase *base,
                               const NN20ClockTouchEvent *event)
{
    (void)base;
    (void)event;
    return ESP_OK;
}

static void time_ui_destroy(NN20ClockUiBase *base)
{
    TimeUi *ui = (TimeUi *)base;

    if (ui->icon_timer != NULL) {
        /* Before the widgets it touches: a timer that fires after this
         * screen is freed would write into released memory. */
        lv_timer_delete(ui->icon_timer);
        ui->icon_timer = NULL;
    }

    if (ui->root != NULL) {
        /* Deletes the labels with it; they are its children. The
         * display's screen is left alone, which is the point. */
        lv_obj_delete(ui->root);
        ui->root = NULL;
        ui->alarm_button = NULL;
        ui->play_button = NULL;
        ui->gear_button = NULL;
    }

    nn20clock_ui_base_deinit(base);
    free(ui);
}

static const NN20ClockUiVTable TIME_UI_VTABLE = {
    .show = time_ui_show,
    .hide = time_ui_hide,
    .handle_touch = time_ui_touch,
    .handle_timer_event = time_ui_timer_event,
    .destroy = time_ui_destroy,
};

/* ------------------------------------------------------------- ctor -- */

NN20ClockUiBase *nn20clock_time_ui_ctor(nn20_worker_ctx *ui_worker,
                                        NN20ClockManager *manager,
                                        NN20ClockUiCommandFn on_command,
                                        NN20ClockPlayer *player)
{
    if (ui_worker == NULL) {
        ESP_LOGE(TAG, "no UiWorker");
        return NULL;
    }

    TimeUi *ui = calloc(1, sizeof(*ui));
    if (ui == NULL) {
        ESP_LOGE(TAG, "out of memory");
        return NULL;
    }

    const NN20ClockUiBaseConfig config = {
        .vtable = &TIME_UI_VTABLE,
        .name = "TimeUi",
        .worker = ui_worker,
        .manager = manager,
        .on_command = on_command,
    };
    if (nn20clock_ui_base_init(&ui->super, &config) != ESP_OK) {
        free(ui);
        return NULL;
    }

    ui->player = player;

    /* No LVGL call has happened here, and none may: this runs on
     * whichever worker asked for the screen. */
    return &ui->super;
}
