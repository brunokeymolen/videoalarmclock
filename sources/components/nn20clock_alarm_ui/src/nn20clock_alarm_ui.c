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
 * nn20clock_alarm_ui.c - the alarm settings screen (design 11).
 *
 * Everything here runs on the UiWorker: show(), hide(), destroy(), and
 * every LVGL event handler, which LVGL calls from lv_timer_handler.
 * There is no locking, and none is needed.
 *
 * The one thing worth knowing before reading: the alarm list is over
 * 5 KB, so it lives in the screen's heap-allocated struct and is never
 * a local. A copy on a worker stack reset the board once already.
 */
#include "nn20clock_alarm_ui.h"

#include "nn20clock_fonts.h"
#include "nn20clock_media.h"
#include "nn20clock_sd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nn20clock_timefmt.h"
#include "lvgl.h"

static const char *TAG = "NN20CLOCK_ALARM_UI";

/* Design 11's palette, shared with TimeUi. */
#define COLOR_TEXT      lv_color_hex(0xD8E6F2)
#define COLOR_ACCENT    lv_color_hex(0x7FC4FF)
#define COLOR_MUTED     lv_color_hex(0x4A6A85)
#define COLOR_SURFACE   lv_color_hex(0x101820)
#define COLOR_DANGER    lv_color_hex(0xFF7F7F)

/* A 720x720 panel with fingers on it: rows and controls are sized to be
 * hit without aiming. */
#define HEADER_HEIGHT   96
#define ROW_HEIGHT      104
#define BUTTON_SIZE     72

/*
 * The time wheels.
 *
 * Three rows: the value selected, and the one either side of it. That
 * is what tells the user which way to drag, and it is all it takes -
 * five rows showed two neighbours each way and only made the wheel
 * taller. Odd, because the selected row is the middle one; an even
 * count would leave the selection off-centre.
 *
 * The two wheels sit either side of the panel's centre line with
 * ROLLER_GAP between them, and the colon is a label exactly that wide
 * in the gap. Aligning it to the right of the hour wheel by a guessed
 * offset is what left it sitting off-centre and half over the minutes.
 *
 * ROLLER_SPACE is the air above the wheels and below them, before the
 * weekday toggles. With three rows the block is short enough that even
 * spacing matters: packed against the header it left a hole in the
 * middle of the screen.
 */
#define ROLLER_ROWS     3
#define ROLLER_WIDTH    150
#define ROLLER_GAP      30
#define ROLLER_SPACE    50
#define ROLLER_TOP      (HEADER_HEIGHT + ROLLER_SPACE)

static const char *WEEKDAY_LABELS[7] = {
    "Mo", "Tu", "We", "Th", "Fr", "Sa", "Su"
};

typedef struct {
    NN20ClockUiBase super;   /* first: a UiBase* casts to this */

    NN20ClockAlarmService service;
    /* Borrowed; NULL when there is no card reader, in which case the
     * only sound on offer is the built-in tone. */
    NN20ClockSd *sd;

    lv_obj_t *root;
    lv_obj_t *list_view;
    lv_obj_t *list_body;      /* the scrollable rows */
    lv_obj_t *editor_view;
    lv_obj_t *media_view;
    lv_obj_t *media_body;

    /*
     * One folder's contents, read when that folder is entered and freed when
     * the picker closes. Heap, not a member: it is over 30 KB, and the
     * screen spends almost all its life not showing it.
     */
    NN20ClockMediaList *media;

    /*
     * The folder the picker is showing - "" for the root folder. Not the
     * alarm's: browsing moves this, and only choosing a row writes
     * anything into the record. Set from the alarm when the picker
     * opens, so it starts where the current choice lives.
     */
    char folder_path[NN20CLOCK_MEDIA_PATH_MAX];

    /* Editor widgets, valid only while the editor is built. */
    lv_obj_t *hour_roller;
    lv_obj_t *minute_roller;
    lv_obj_t *weekday_buttons[7];
    lv_obj_t *enable_switch;
    lv_obj_t *media_button;
    /* Hidden until a save is refused - see on_editor_back(). */
    lv_obj_t *editor_warning;

    /* The alarm being edited. id == NONE means a new one. */
    NN20ClockAlarmConfig editing;

    /*
     * Over 5 KB. Here rather than on any stack - see the file comment.
     * Refreshed from the service whenever the list is shown.
     */
    NN20ClockAlarmList alarms;
} AlarmUi;

static void show_list(AlarmUi *ui);
static void show_editor(AlarmUi *ui, const NN20ClockAlarmConfig *alarm);
static void show_media(AlarmUi *ui);
static void on_editor_sound(lv_event_t *event);
static void media_label(const NN20ClockAlarmConfig *alarm, char *out,
                        size_t size);

/* ---------------------------------------------------------- helpers -- */

/* "Mon-Fri", "Every day", "Mo We Fr" - what the row shows under the
 * time. Design 10 numbers weekdays from Monday, which is also how they
 * read. */
static void format_weekdays(const NN20ClockAlarmConfig *alarm, char *out,
                            size_t size)
{
    if (alarm->kind == NN20CLOCK_ALARM_KIND_ONE_OFF) {
        snprintf(out, size, "%04u-%02u-%02u",
                 (unsigned)alarm->one_off_date.year,
                 (unsigned)alarm->one_off_date.month,
                 (unsigned)alarm->one_off_date.day);
        return;
    }

    const uint8_t mask = alarm->weekdays_mask & NN20CLOCK_ALARM_EVERY_DAY;
    if (mask == NN20CLOCK_ALARM_EVERY_DAY) {
        snprintf(out, size, "Every day");
        return;
    }
    if (mask == NN20CLOCK_ALARM_WEEKDAYS) {
        snprintf(out, size, "Mon-Fri");
        return;
    }
    if (mask == (NN20CLOCK_ALARM_SATURDAY | NN20CLOCK_ALARM_SUNDAY)) {
        snprintf(out, size, "Weekends");
        return;
    }

    out[0] = '\0';
    size_t used = 0;
    for (int day = 0; day < 7; day++) {
        if ((mask & (1u << day)) == 0u) {
            continue;
        }
        const int written = snprintf(out + used, size - used, "%s%s",
                                     (used > 0) ? " " : "",
                                     WEEKDAY_LABELS[day]);
        if (written <= 0 || (size_t)written >= size - used) {
            break;   /* truncated; the row is only so wide anyway */
        }
        used += (size_t)written;
    }
    if (used == 0) {
        /* Validation refuses to store this, but a row must still render
         * something rather than an empty gap. */
        snprintf(out, size, "never");
    }
}

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

static lv_obj_t *make_text_button(lv_obj_t *parent, const char *text,
                                  lv_color_t color, lv_event_cb_t handler,
                                  void *user_data)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_size(button, BUTTON_SIZE, BUTTON_SIZE);
    lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(button, COLOR_SURFACE, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(button, 0, LV_PART_MAIN);

    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, color, LV_PART_MAIN);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_center(label);

    if (handler != NULL) {
        lv_obj_add_event_cb(button, handler, LV_EVENT_CLICKED, user_data);
    }
    return button;
}

/* Replaces whichever view is up. LVGL deletes the widgets with it, so
 * every pointer into the old view must be forgotten here. */
static void clear_views(AlarmUi *ui)
{
    if (ui->list_view != NULL) {
        lv_obj_delete(ui->list_view);
        ui->list_view = NULL;
        ui->list_body = NULL;
    }
    if (ui->editor_view != NULL) {
        lv_obj_delete(ui->editor_view);
        ui->editor_view = NULL;
        ui->hour_roller = NULL;
        ui->minute_roller = NULL;
        ui->enable_switch = NULL;
        ui->media_button = NULL;
        ui->editor_warning = NULL;
        memset(ui->weekday_buttons, 0, sizeof(ui->weekday_buttons));
    }
    if (ui->media_view != NULL) {
        lv_obj_delete(ui->media_view);
        ui->media_view = NULL;
        ui->media_body = NULL;
    }
    /* The rows are gone, and they were the only things pointing into
     * this. Freed here rather than kept: it is over 30 KB. */
    free(ui->media);
    ui->media = NULL;
}

/* --------------------------------------------------------- the list -- */

static void on_back(lv_event_t *event)
{
    AlarmUi *ui = lv_event_get_user_data(event);

    /* Design 8 routes this: the manager returns to CLOCK_STATE_TIME and
     * swaps the screen. This screen does not choose what comes next. */
    const NN20ClockUiCommand command = {
        .type = NN20CLOCK_UI_COMMAND_CLOSE_SETTINGS,
    };
    (void)nn20clock_ui_send_command(&ui->super, &command);
}

static void on_add(lv_event_t *event)
{
    AlarmUi *ui = lv_event_get_user_data(event);

    NN20ClockAlarmConfig fresh;
    (void)nn20clock_alarm_defaults(&fresh);
    fresh.id = NN20CLOCK_ALARM_ID_NONE;
    show_editor(ui, &fresh);
}

/* The row's switch: enable or disable without opening the editor, which
 * is the common case for a recurring alarm. */
static void on_row_toggle(lv_event_t *event)
{
    AlarmUi *ui = lv_event_get_user_data(event);
    lv_obj_t *toggle = lv_event_get_target(event);
    const uint32_t id = (uint32_t)(uintptr_t)lv_obj_get_user_data(toggle);

    size_t index = 0;
    if (nn20clock_alarm_list_find(&ui->alarms, id, &index) != ESP_OK) {
        return;
    }

    NN20ClockAlarmConfig updated = ui->alarms.alarms[index];
    updated.enabled = lv_obj_has_state(toggle, LV_STATE_CHECKED);

    if (ui->service.save(ui->service.ctx, &updated, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "could not %s alarm %u",
                 updated.enabled ? "enable" : "disable", (unsigned)id);
        /* Put the switch back where the stored state actually is. */
        show_list(ui);
        return;
    }
    ui->alarms.alarms[index] = updated;
}

static void on_row_edit(lv_event_t *event)
{
    AlarmUi *ui = lv_event_get_user_data(event);
    lv_obj_t *row = lv_event_get_target(event);
    const uint32_t id = (uint32_t)(uintptr_t)lv_obj_get_user_data(row);

    size_t index = 0;
    if (nn20clock_alarm_list_find(&ui->alarms, id, &index) != ESP_OK) {
        return;
    }
    show_editor(ui, &ui->alarms.alarms[index]);
}

static void add_row(AlarmUi *ui, const NN20ClockAlarmConfig *alarm)
{
    lv_obj_t *row = lv_obj_create(ui->list_body);
    lv_obj_set_size(row, LV_PCT(100), ROW_HEIGHT);
    /* Fixed height in a scrolling column: without this the flex layout
     * squeezes rows to fit rather than scrolling them. */
    lv_obj_set_style_flex_grow(row, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(row, COLOR_SURFACE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 16, LV_PART_MAIN);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    /* The id travels in the widget, so a handler needs no lookup table.
     * Ids are small integers, so this pointer is never dereferenced. */
    lv_obj_set_user_data(row, (void *)(uintptr_t)alarm->id);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, on_row_edit, LV_EVENT_CLICKED, ui);

    NN20ClockDateTime when = {
        .hour = alarm->hour,
        .minute = alarm->minute,
    };
    char time_text[NN20CLOCK_HHMM_SIZE];
    (void)nn20clock_timefmt_hhmm(&when, time_text, sizeof(time_text));

    lv_obj_t *time_label = lv_label_create(row);
    lv_label_set_text(time_label, time_text);
    lv_obj_set_style_text_font(time_label, &lv_font_montserrat_48,
                               LV_PART_MAIN);
    lv_obj_set_style_text_color(time_label,
                                alarm->enabled ? COLOR_ACCENT : COLOR_MUTED,
                                LV_PART_MAIN);
    lv_obj_align(time_label, LV_ALIGN_LEFT_MID, 0, 0);

    char days[48];
    format_weekdays(alarm, days, sizeof(days));
    lv_obj_t *days_label = lv_label_create(row);
    lv_label_set_text(days_label, days);
    lv_obj_set_style_text_font(days_label, &lv_font_montserrat_20,
                               LV_PART_MAIN);
    lv_obj_set_style_text_color(days_label, COLOR_MUTED, LV_PART_MAIN);
    lv_obj_align(days_label, LV_ALIGN_LEFT_MID, 170, 0);

    lv_obj_t *toggle = lv_switch_create(row);
    lv_obj_set_size(toggle, 90, 46);
    lv_obj_align(toggle, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(toggle, COLOR_ACCENT,
                              LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_user_data(toggle, (void *)(uintptr_t)alarm->id);
    if (alarm->enabled) {
        lv_obj_add_state(toggle, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(toggle, on_row_toggle, LV_EVENT_VALUE_CHANGED, ui);
}

static void show_list(AlarmUi *ui)
{
    clear_views(ui);

    ui->list_view = make_panel(ui->root);
    if (ui->list_view == NULL) {
        return;
    }

    /*
     * The panel is a flex column: a fixed-height header, then a body
     * that grows into whatever is left.
     *
     * Not "100% minus the header": LV_PCT() encodes a percentage as a
     * special coordinate value, so LV_PCT(100) - HEADER_HEIGHT is
     * arithmetic on an encoded number, not a size. It produced a body
     * one pixel high and an apparently empty screen.
     */
    lv_obj_set_flex_flow(ui->list_view, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(ui->list_view, 0, LV_PART_MAIN);

    lv_obj_t *header = lv_obj_create(ui->list_view);
    lv_obj_set_size(header, LV_PCT(100), HEADER_HEIGHT);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(header, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(header, 12, LV_PART_MAIN);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back = make_text_button(header, LV_SYMBOL_LEFT, COLOR_TEXT,
                                      on_back, ui);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, "Alarms");
    lv_obj_set_style_text_font(title, &nn20clock_font_ui_bold_28,
                               LV_PART_MAIN);
    lv_obj_set_style_text_color(title, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *add = make_text_button(header, LV_SYMBOL_PLUS, COLOR_ACCENT,
                                     on_add, ui);
    lv_obj_align(add, LV_ALIGN_RIGHT_MID, 0, 0);

    /* The rows, scrollable: sixteen alarms do not fit on one screen. */
    ui->list_body = lv_obj_create(ui->list_view);
    lv_obj_set_width(ui->list_body, LV_PCT(100));
    lv_obj_set_flex_grow(ui->list_body, 1);   /* the rest of the panel */
    lv_obj_set_style_bg_opa(ui->list_body, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(ui->list_body, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ui->list_body, 20, LV_PART_MAIN);
    lv_obj_set_style_pad_row(ui->list_body, 16, LV_PART_MAIN);
    lv_obj_set_flex_flow(ui->list_body, LV_FLEX_FLOW_COLUMN);

    if (ui->service.list(ui->service.ctx, &ui->alarms) != ESP_OK) {
        ESP_LOGE(TAG, "cannot read the alarm list");
        memset(&ui->alarms, 0, sizeof(ui->alarms));
    }

    if (ui->alarms.count == 0u) {
        lv_obj_t *empty = lv_label_create(ui->list_body);
        lv_label_set_text(empty, "No alarms yet.\nTap + to add one.");
        lv_obj_set_style_text_font(empty, &lv_font_montserrat_20,
                                   LV_PART_MAIN);
        lv_obj_set_style_text_color(empty, COLOR_MUTED, LV_PART_MAIN);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER,
                                    LV_PART_MAIN);
        lv_obj_align(empty, LV_ALIGN_TOP_MID, 0, 60);
        return;
    }

    for (size_t i = 0; i < ui->alarms.count; i++) {
        add_row(ui, &ui->alarms.alarms[i]);
    }
}

/* ------------------------------------------------------- the editor -- */

/*
 * Leaving the editor without keeping anything.
 *
 * The editor saves on the way out (see on_editor_back), so this is the
 * only way to change your mind - which is why it is a button of its own
 * and not just the back arrow doing nothing.
 */
static void on_editor_cancel(lv_event_t *event)
{
    show_list((AlarmUi *)lv_event_get_user_data(event));
}

/*
 * Dark text on the accent when selected, light on dark when not. Done in
 * code rather than with a state-bound style because the text lives in a
 * child label, and LVGL does not propagate a parent's state to it.
 */
static void set_weekday_label_colour(lv_obj_t *button)
{
    lv_obj_t *label = lv_obj_get_child(button, 0);
    if (label == NULL) {
        return;
    }
    const bool selected = lv_obj_has_state(button, LV_STATE_CHECKED);
    lv_obj_set_style_text_color(label,
                                selected ? lv_color_black() : COLOR_TEXT,
                                LV_PART_MAIN);
}

static void on_weekday_toggle(lv_event_t *event)
{
    AlarmUi *ui = lv_event_get_user_data(event);
    lv_obj_t *button = lv_event_get_target(event);
    const int day = (int)(intptr_t)lv_obj_get_user_data(button);

    if (lv_obj_has_state(button, LV_STATE_CHECKED)) {
        ui->editing.weekdays_mask |= (uint8_t)(1u << day);
    } else {
        ui->editing.weekdays_mask &= (uint8_t)~(1u << day);
    }
    set_weekday_label_colour(button);
}

/*
 * Read the editor's widgets back into the record being edited.
 *
 * Its own function because saving is not the only thing that needs it:
 * opening the sound list rebuilds the editor afterwards, and a time set
 * on the rollers but not yet collected would be lost on the way there.
 */
static void collect_editor(AlarmUi *ui)
{
    if (ui->hour_roller == NULL) {
        return;
    }
    ui->editing.hour = (uint8_t)lv_roller_get_selected(ui->hour_roller);
    ui->editing.minute = (uint8_t)lv_roller_get_selected(ui->minute_roller);
    ui->editing.enabled = lv_obj_has_state(ui->enable_switch,
                                           LV_STATE_CHECKED);
}

/* Show the editor's one complaint, or clear it. */
static void set_editor_warning(AlarmUi *ui, const char *text)
{
    if (ui->editor_warning == NULL) {
        return;
    }
    if (text == NULL) {
        lv_obj_add_flag(ui->editor_warning, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_label_set_text(ui->editor_warning, text);
    lv_obj_remove_flag(ui->editor_warning, LV_OBJ_FLAG_HIDDEN);
}

/*
 * Leaving the editor by the back arrow, which is what keeps the edits.
 *
 * The editor has no separate save step: a screen whose changes are only
 * kept if you find the right button is a screen that loses changes. So
 * back is save, and the cancel button beside it is the way out for
 * somebody who did not mean any of it.
 *
 * The one thing that can stop it is an alarm that could never fire -
 * recurrent with no weekday. That is refused rather than stored, and
 * the editor stays up saying so, because silently dropping the edit on
 * the way out is exactly what this design is meant to avoid. Cancel is
 * still there for anyone who would rather leave it.
 */
static void on_editor_back(lv_event_t *event)
{
    AlarmUi *ui = lv_event_get_user_data(event);

    collect_editor(ui);

    if (ui->editing.kind == NN20CLOCK_ALARM_KIND_RECURRENT &&
        (ui->editing.weekdays_mask & NN20CLOCK_ALARM_EVERY_DAY) == 0u) {
        ESP_LOGW(TAG, "alarm needs at least one day");
        set_editor_warning(ui, "Pick at least one day");
        return;
    }

    if (ui->service.save(ui->service.ctx, &ui->editing, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "could not save the alarm");
        set_editor_warning(ui, "Could not save this alarm");
        return;
    }
    show_list(ui);
}

static void on_editor_delete(lv_event_t *event)
{
    AlarmUi *ui = lv_event_get_user_data(event);

    if (ui->editing.id != NN20CLOCK_ALARM_ID_NONE &&
        ui->service.remove(ui->service.ctx, ui->editing.id) != ESP_OK) {
        ESP_LOGE(TAG, "could not delete alarm %u",
                 (unsigned)ui->editing.id);
    }
    show_list(ui);
}

/* "00\n01\n...\n23" - the roller wants one string with newlines. */
static void build_number_options(char *out, size_t size, int count)
{
    size_t used = 0;
    for (int i = 0; i < count && used < size; i++) {
        const int written = snprintf(out + used, size - used, "%s%02d",
                                     (i > 0) ? "\n" : "", i);
        if (written <= 0) {
            break;
        }
        used += (size_t)written;
    }
}

static lv_obj_t *make_roller(lv_obj_t *parent, const char *options,
                             uint32_t selected, int32_t x)
{
    lv_obj_t *roller = lv_roller_create(parent);
    lv_roller_set_options(roller, options, LV_ROLLER_MODE_NORMAL);

    /*
     * The styles BEFORE the row count, and that order is the whole
     * point. lv_roller_set_visible_row_count() turns rows into pixels
     * there and then, using whatever font the roller has at that
     * moment. Setting it first meant the height was five rows of the
     * 14 px default - about one row of the 48 px face actually drawn -
     * so the wheel showed the selected value and nothing either side of
     * it, which is the one thing a roller is for.
     */
    lv_obj_set_style_bg_color(roller, COLOR_SURFACE, LV_PART_MAIN);
    lv_obj_set_style_text_color(roller, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(roller, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_set_style_text_line_space(roller, 4, LV_PART_MAIN);
    lv_obj_set_style_border_width(roller, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(roller, COLOR_ACCENT, LV_PART_SELECTED);
    lv_obj_set_style_text_color(roller, lv_color_black(), LV_PART_SELECTED);

    lv_roller_set_visible_row_count(roller, ROLLER_ROWS);
    lv_obj_set_width(roller, ROLLER_WIDTH);
    lv_obj_align(roller, LV_ALIGN_TOP_MID, x, ROLLER_TOP);

    lv_roller_set_selected(roller, selected, LV_ANIM_OFF);
    return roller;
}

static void show_editor(AlarmUi *ui, const NN20ClockAlarmConfig *alarm)
{
    clear_views(ui);
    ui->editing = *alarm;

    ui->editor_view = make_panel(ui->root);
    if (ui->editor_view == NULL) {
        return;
    }

    lv_obj_t *title = lv_label_create(ui->editor_view);
    lv_label_set_text(title, (alarm->id == NN20CLOCK_ALARM_ID_NONE)
                                 ? "New alarm"
                                 : "Edit alarm");
    lv_obj_set_style_text_font(title, &nn20clock_font_ui_bold_28,
                               LV_PART_MAIN);
    lv_obj_set_style_text_color(title, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 28);

    /* Back keeps the edits - see on_editor_back(). */
    lv_obj_t *back = make_text_button(ui->editor_view, LV_SYMBOL_LEFT,
                                      COLOR_TEXT, on_editor_back, ui);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 12, 12);

    /* Hours and minutes. Rollers rather than +/- buttons: setting 06:45
     * takes two drags instead of fifty taps. */
    static char hours[24 * 3 + 1];
    static char minutes[60 * 3 + 1];
    build_number_options(hours, sizeof(hours), 24);
    build_number_options(minutes, sizeof(minutes), 60);

    /* Half the gap either side of the centre line, so the colon's column
     * is the panel's middle and the two wheels are symmetric about it. */
    const int32_t roller_x = (ROLLER_WIDTH + ROLLER_GAP) / 2;
    ui->hour_roller = make_roller(ui->editor_view, hours, alarm->hour,
                                  -roller_x);
    ui->minute_roller = make_roller(ui->editor_view, minutes, alarm->minute,
                                    roller_x);

    lv_obj_t *colon = lv_label_create(ui->editor_view);
    lv_label_set_text(colon, ":");
    lv_obj_set_style_text_font(colon, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_set_style_text_color(colon, COLOR_TEXT, LV_PART_MAIN);
    /*
     * The colon is given the gap as its width and centres its own glyph
     * in it, so it lands on the panel's centre line whatever the glyph
     * is worth - rather than being pushed off the hour wheel by an
     * offset chosen to look right once.
     *
     * Vertically it hangs off the roller and not off the top of the
     * screen: a roller's height depends on its font and row count, and
     * those are numbers that would otherwise have to be kept in sync by
     * hand. The middle of the wheel is the selected row, which is the
     * digit the colon belongs to.
     */
    lv_obj_set_width(colon, ROLLER_GAP);
    lv_obj_set_style_text_align(colon, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align_to(colon, ui->hour_roller, LV_ALIGN_OUT_RIGHT_MID, 0, 0);

    /*
     * Everything below the wheels hangs off their real height, for the
     * same reason: five rows of a 48 px face is a number LVGL works out
     * and this file should not repeat.
     */
    lv_obj_update_layout(ui->editor_view);
    const int32_t rollers_bottom =
        ROLLER_TOP + lv_obj_get_height(ui->hour_roller);

    /* Seven day toggles in a row, sized for a fingertip. */
    lv_obj_t *days = lv_obj_create(ui->editor_view);
    lv_obj_set_size(days, LV_PCT(96), 100);
    lv_obj_align(days, LV_ALIGN_TOP_MID, 0, rollers_bottom + ROLLER_SPACE);
    lv_obj_set_style_bg_opa(days, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(days, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(days, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(days, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(days, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(days, LV_OBJ_FLAG_SCROLLABLE);

    for (int day = 0; day < 7; day++) {
        lv_obj_t *button = lv_button_create(days);
        lv_obj_set_size(button, 84, 84);
        lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(button, COLOR_SURFACE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(button, COLOR_ACCENT,
                                  LV_PART_MAIN | LV_STATE_CHECKED);
        lv_obj_set_style_shadow_width(button, 0, LV_PART_MAIN);
        lv_obj_add_flag(button, LV_OBJ_FLAG_CHECKABLE);
        lv_obj_set_user_data(button, (void *)(intptr_t)day);

        lv_obj_t *label = lv_label_create(button);
        lv_label_set_text(label, WEEKDAY_LABELS[day]);
        lv_obj_set_style_text_font(label, &nn20clock_font_ui_bold_28,
                                   LV_PART_MAIN);
        lv_obj_center(label);

        if ((alarm->weekdays_mask & (1u << day)) != 0u) {
            lv_obj_add_state(button, LV_STATE_CHECKED);
        }
        /*
         * The label is a child of the button, and LVGL states do not
         * propagate to children - a style bound to LV_STATE_CHECKED on
         * the label would never apply, which is why the first version
         * stayed thin white on light blue whatever the state. The colour
         * is set explicitly here and again whenever the button is
         * toggled.
         */
        set_weekday_label_colour(button);
        lv_obj_add_event_cb(button, on_weekday_toggle, LV_EVENT_VALUE_CHANGED,
                            ui);
        ui->weekday_buttons[day] = button;
    }

    /*
     * "Enabled" and the sound below read as the two settings they are,
     * at the size the rest of this screen is drawn at. They were both
     * 20 px muted text, which on a 720 panel at arm's length looked
     * like a caption on the buttons rather than something to touch.
     */
    lv_obj_t *enabled_label = lv_label_create(ui->editor_view);
    lv_label_set_text(enabled_label, "Enabled");
    lv_obj_set_style_text_font(enabled_label, &nn20clock_font_ui_bold_28,
                               LV_PART_MAIN);
    lv_obj_set_style_text_color(enabled_label, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_align(enabled_label, LV_ALIGN_BOTTOM_LEFT, 40, -138);

    ui->enable_switch = lv_switch_create(ui->editor_view);
    lv_obj_set_size(ui->enable_switch, 100, 50);
    lv_obj_align(ui->enable_switch, LV_ALIGN_BOTTOM_LEFT, 180, -132);
    lv_obj_set_style_bg_color(ui->enable_switch, COLOR_ACCENT,
                              LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (alarm->enabled) {
        lv_obj_add_state(ui->enable_switch, LV_STATE_CHECKED);
    }

    /*
     * What this alarm plays (design 13). A row rather than an icon: the
     * point is to see which sound is chosen without having to open
     * anything.
     */
    lv_obj_t *sound = lv_obj_create(ui->editor_view);
    lv_obj_set_size(sound, 380, 76);
    lv_obj_align(sound, LV_ALIGN_BOTTOM_RIGHT, -40, -126);
    lv_obj_set_style_bg_color(sound, COLOR_SURFACE, LV_PART_MAIN);
    lv_obj_set_style_border_width(sound, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(sound, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_all(sound, 12, LV_PART_MAIN);
    lv_obj_remove_flag(sound, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(sound, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(sound, on_editor_sound, LV_EVENT_CLICKED, ui);
    ui->media_button = sound;

    /* LVGL's own face, not the bold one: the bold is printable ASCII
     * only and a symbol would draw as nothing. */
    lv_obj_t *note = lv_label_create(sound);
    lv_label_set_text(note, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_color(note, COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_set_style_text_font(note, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_align(note, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *sound_name = lv_label_create(sound);
    char sound_text[NN20CLOCK_MEDIA_PATH_MAX + 32];
    media_label(alarm, sound_text, sizeof(sound_text));
    lv_label_set_text(sound_name, sound_text);
    lv_label_set_long_mode(sound_name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(sound_name, 290);
    lv_obj_set_style_text_color(sound_name, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(sound_name, &nn20clock_font_ui_bold_28,
                               LV_PART_MAIN);
    lv_obj_align(sound_name, LV_ALIGN_LEFT_MID, 46, 0);

    /*
     * Cancel, where the tick used to be. The tick had to be found to
     * keep an edit; this one has to be found to throw one away, which
     * is the rarer thing to want and the one worth an explicit press.
     */
    lv_obj_t *cancel = make_text_button(ui->editor_view, LV_SYMBOL_CLOSE,
                                        COLOR_DANGER, on_editor_cancel, ui);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_RIGHT, -40, -40);

    /*
     * Between the two bottom buttons, and empty until there is
     * something to say. Only a save refused on the way out fills it in.
     */
    ui->editor_warning = lv_label_create(ui->editor_view);
    lv_label_set_text(ui->editor_warning, "");
    lv_obj_set_style_text_font(ui->editor_warning, &lv_font_montserrat_20,
                               LV_PART_MAIN);
    lv_obj_set_style_text_color(ui->editor_warning, COLOR_DANGER,
                                LV_PART_MAIN);
    lv_obj_align(ui->editor_warning, LV_ALIGN_BOTTOM_MID, 0, -66);
    lv_obj_add_flag(ui->editor_warning, LV_OBJ_FLAG_HIDDEN);

    /* Only for an alarm that exists; there is nothing to delete when
     * adding one. */
    if (alarm->id != NN20CLOCK_ALARM_ID_NONE) {
        lv_obj_t *remove = make_text_button(ui->editor_view, LV_SYMBOL_TRASH,
                                            COLOR_DANGER, on_editor_delete,
                                            ui);
        lv_obj_align(remove, LV_ALIGN_BOTTOM_LEFT, 40, -40);
    }
}

/* ------------------------------------------------------ the sounds -- */

/*
 * What an alarm with no media of its own plays. Design 13's fallback,
 * named here so the list has something to select rather than a blank
 * row meaning "none".
 */
#define TONE_LABEL "Built-in tone"

/*
 * A different film every time the alarm goes off, chosen when it fires,
 * from the folder the picker is showing.
 *
 * In angle brackets because it is not a file name and must not read
 * like one: it sits in a list of them, and FAT will not let a real file
 * on the card be called this, so there is nothing it can be confused
 * with.
 */
#define RANDOM_LABEL "<random>"

/* What the root folder is called where a name is wanted. The card's root
 * has no name of its own, and "/" reads as a path rather than a place. */
#define ROOT_LABEL "Root"

/* How many films it takes before choosing between them means anything.
 * With one, <random> would be an elaborate way of naming that file. */
#define RANDOM_MINIMUM 2u

/* Why the row is there but cannot be picked. Short: it shares the line
 * with the label, where the tick goes on the rows that can. */
#define RANDOM_UNAVAILABLE "2+ films"

/* And why a folder too deep to remember cannot offer one - see
 * NN20CLOCK_ALARM_MEDIA_SET_MAX. */
#define RANDOM_TOO_DEEP "folder too deep"

/* Row indices that mean something other than an entry in the list. */
#define TONE_INDEX   (-1)
#define RANDOM_INDEX (-2)
#define UP_INDEX     (-3)

static bool is_random(const NN20ClockAlarmConfig *alarm)
{
    return alarm->media_mode == NN20CLOCK_ALARM_MEDIA_RANDOM_SET;
}

/*
 * What the editor's sound button says: the folder and the choice in it.
 *
 *   Tone                  the built-in tone
 *   wake.avi              a clip in the root folder
 *   morning / wake.avi    a clip in a folder
 *   Root / <random>       random from the root folder
 *   morning / <random>    random from a folder
 *
 * A buffer rather than a pointer into the record, because three of
 * those five are built rather than stored.
 */
static void media_label(const NN20ClockAlarmConfig *alarm, char *out,
                        size_t size)
{
    if (is_random(alarm)) {
        const char *const folder = alarm->media_set_id;
        (void)snprintf(out, size, "%s / " RANDOM_LABEL,
                       (folder[0] == '\0') ? ROOT_LABEL : folder);
        return;
    }

    if (alarm->media_path[0] == '\0') {
        (void)snprintf(out, size, "%s", TONE_LABEL);
        return;
    }

    /* A clip in the root folder is just its name; anywhere else names the
     * folder it is in, because two folders may hold "wake.avi". The spaces
     * around the slash are what stop it reading as a path the user
     * could type. */
    char parent[NN20CLOCK_MEDIA_PATH_MAX];
    if (nn20clock_media_path_parent(alarm->media_path, parent,
                                    sizeof(parent)) == ESP_OK &&
        parent[0] != '\0') {
        (void)snprintf(out, size, "%s / %s", parent,
                       nn20clock_media_path_name(alarm->media_path));
    } else {
        (void)snprintf(out, size, "%s",
                       nn20clock_media_path_name(alarm->media_path));
    }
}

/* Films directly in the folder being shown, as opposed to everything in
 * it. Child folders and stray files are not something to wake up to. */
static size_t playable_count(const NN20ClockMediaList *media)
{
    size_t playable = 0u;
    if (media != NULL) {
        nn20clock_media_list_counts(media, &playable, NULL, NULL);
    }
    return playable;
}

static void on_media_back(lv_event_t *event)
{
    AlarmUi *ui = lv_event_get_user_data(event);
    show_editor(ui, &ui->editing);
}

/*
 * A row was chosen. Three kinds of thing can be behind it: a folder to
 * enter, one of the two rows that are not files, or a file.
 *
 * Entering a folder re-reads the card rather than caching a tree - see
 * show_media(). Everything else settles the alarm's media and goes back
 * to the editor, where the tick is still what commits it.
 */
static void on_media_pick(lv_event_t *event)
{
    AlarmUi *ui = lv_event_get_user_data(event);
    lv_obj_t *row = lv_event_get_target(event);
    const intptr_t index = (intptr_t)lv_obj_get_user_data(row);

    if (index == UP_INDEX) {
        char parent[NN20CLOCK_MEDIA_PATH_MAX];
        if (nn20clock_media_path_parent(ui->folder_path, parent,
                                        sizeof(parent)) == ESP_OK) {
            (void)snprintf(ui->folder_path, sizeof(ui->folder_path), "%s",
                           parent);
        } else {
            ui->folder_path[0] = '\0';   /* lost: the root is always there */
        }
        show_media(ui);
        return;
    }

    if (index >= 0 && ui->media != NULL &&
        (size_t)index < ui->media->count &&
        ui->media->entries[index].type == NN20CLOCK_MEDIA_ENTRY_FOLDER) {
        (void)snprintf(ui->folder_path, sizeof(ui->folder_path), "%s",
                       ui->media->entries[index].path);
        show_media(ui);
        return;
    }

    if (index == RANDOM_INDEX) {
        /*
         * The mode and the folder are the whole choice: which film is not
         * decided here and not stored. media_path is cleared so the
         * record has one meaning - a stale path left behind would be a
         * file this alarm does not play, waiting to be read by anything
         * that forgets to check the mode first.
         */
        const size_t length = strnlen(ui->folder_path,
                                      sizeof(ui->folder_path));
        if (length >= sizeof(ui->editing.media_set_id)) {
            /* The row is not offered for a folder this long - see
             * RANDOM_TOO_DEEP - so this cannot normally be reached.
             * Refusing here anyway is what makes the copy below
             * obviously whole rather than obviously truncated. */
            ESP_LOGW(TAG, "folder path too long to store; ignoring <random>");
            return;
        }
        ui->editing.media_mode = NN20CLOCK_ALARM_MEDIA_RANDOM_SET;
        ui->editing.media_path[0] = '\0';
        memcpy(ui->editing.media_set_id, ui->folder_path, length + 1u);
    } else if (index == TONE_INDEX || ui->media == NULL ||
               (size_t)index >= ui->media->count) {
        ui->editing.media_mode = NN20CLOCK_ALARM_MEDIA_AUDIO;
        ui->editing.media_path[0] = '\0';
        ui->editing.media_set_id[0] = '\0';
    } else {
        /* The entry's path, not its name: it is what the alarm stores
         * and what the app resolves, and the screen never builds one. */
        ui->editing.media_mode = NN20CLOCK_ALARM_MEDIA_AUDIO;
        (void)snprintf(ui->editing.media_path,
                       sizeof(ui->editing.media_path), "%s",
                       ui->media->entries[index].path);
        ui->editing.media_set_id[0] = '\0';
    }

    char label[NN20CLOCK_MEDIA_PATH_MAX + 32];
    media_label(&ui->editing, label, sizeof(label));
    ESP_LOGI(TAG, "alarm sound: %s", label);

    /* Not saved yet - the editor's tick is still what commits it. */
    show_editor(ui, &ui->editing);
}

/*
 * One row of the sound list.
 *
 * `enabled` false is a row that is shown but cannot be chosen, with
 * `note` in place of the tick saying why. Shown rather than hidden
 * because a row that is simply absent teaches nobody that the option
 * exists: somebody with one film in a folder should be able to see that
 * putting a second one there would give them <random>.
 *
 * `is_folder` inverts the row - the accent as the fill rather than as the
 * text. A folder is somewhere to go and a file is something to play, and
 * they sit in one scrolling column: the difference has to be visible
 * without reading, because tapping the wrong one is the difference
 * between navigating and setting your alarm.
 */
static lv_obj_t *add_media_row(AlarmUi *ui, const char *name, intptr_t index,
                               bool selected, bool enabled, const char *note,
                               bool is_folder)
{
    lv_obj_t *row = lv_obj_create(ui->media_body);
    if (row == NULL) {
        return NULL;
    }
    lv_obj_set_size(row, LV_PCT(100), ROW_HEIGHT);
    /* Fixed height in a scrolling column: without this the flex layout
     * squeezes the rows to fit rather than scrolling them. */
    lv_obj_set_style_flex_grow(row, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(row, is_folder ? COLOR_ACCENT : COLOR_SURFACE,
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 16, LV_PART_MAIN);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_user_data(row, (void *)index);
    if (enabled) {
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, on_media_pick, LV_EVENT_CLICKED, ui);
    } else {
        /* No handler at all rather than a handler that declines: a row
         * that lights up under the finger and then does nothing reads
         * as a bug, and half the point of showing it is that it looks
         * unavailable. */
        lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_opa(row, LV_OPA_50, LV_PART_MAIN);
    }

    lv_color_t colour = COLOR_TEXT;
    if (is_folder) {
        /* On the accent fill, the surface colour is the readable one. */
        colour = COLOR_SURFACE;
    } else if (!enabled) {
        colour = COLOR_MUTED;
    } else if (selected) {
        colour = COLOR_ACCENT;
    }

    /* A folder says so with a glyph as well as with the inversion: colour
     * alone is not something everybody can read. */
    lv_coord_t text_left = 0;
    if (is_folder) {
        lv_obj_t *glyph = lv_label_create(row);
        lv_label_set_text(glyph, LV_SYMBOL_DIRECTORY);
        lv_obj_set_style_text_color(glyph, colour, LV_PART_MAIN);
        lv_obj_set_style_text_font(glyph, &lv_font_montserrat_28,
                                   LV_PART_MAIN);
        lv_obj_align(glyph, LV_ALIGN_LEFT_MID, 0, 0);
        text_left = 54;
    }

    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, name);
    /* A long file name gets cut rather than pushing the tick off the
     * row; the whole name is on the card, not on the screen. */
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(label, LV_PCT(75));
    lv_obj_set_style_text_font(label, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, colour, LV_PART_MAIN);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, text_left, 0);

    if (!enabled && note != NULL) {
        lv_obj_t *why = lv_label_create(row);
        lv_label_set_text(why, note);
        lv_obj_set_style_text_color(why, COLOR_MUTED, LV_PART_MAIN);
        lv_obj_set_style_text_font(why, &lv_font_montserrat_20, LV_PART_MAIN);
        lv_obj_align(why, LV_ALIGN_RIGHT_MID, 0, 0);
    } else if (selected) {
        lv_obj_t *tick = lv_label_create(row);
        lv_label_set_text(tick, LV_SYMBOL_OK);
        lv_obj_set_style_text_color(tick, colour, LV_PART_MAIN);
        lv_obj_set_style_text_font(tick, &lv_font_montserrat_28,
                                   LV_PART_MAIN);
        lv_obj_align(tick, LV_ALIGN_RIGHT_MID, 0, 0);
    }
    return row;
}

/*
 * Where the picker should open: the folder the alarm's media already lives
 * in, so somebody changing a choice starts where they made it rather
 * than at the root every time.
 *
 * Anything that does not resolve falls back to the root folder, which
 * always exists. A card whose folders have been rearranged over FTP should
 * open somewhere, not nowhere.
 */
static void media_start_folder(const NN20ClockAlarmConfig *alarm, char *out,
                            size_t size)
{
    out[0] = '\0';

    if (is_random(alarm)) {
        if (nn20clock_media_path_is_safe(alarm->media_set_id,
                                         NN20CLOCK_MEDIA_PATH_FOLDER)) {
            (void)snprintf(out, size, "%s", alarm->media_set_id);
        }
        return;
    }

    if (alarm->media_path[0] != '\0' &&
        nn20clock_media_path_is_safe(alarm->media_path,
                                     NN20CLOCK_MEDIA_PATH_FILE)) {
        char parent[NN20CLOCK_MEDIA_PATH_MAX];
        if (nn20clock_media_path_parent(alarm->media_path, parent,
                                        sizeof(parent)) == ESP_OK) {
            (void)snprintf(out, size, "%s", parent);
        }
    }
}

/*
 * Design 13: an alarm may name a file on the card. This is where it is
 * named - one folder at a time, with the built-in tone at the top, which
 * is what an alarm falls back to anyway.
 *
 * Reads the card every time a folder is entered rather than caching the
 * tree. A tree would be a second copy of the card in RAM that goes
 * stale the moment an FTP client touches it, to save a directory read
 * a user waits for once per tap.
 */
static void show_media(AlarmUi *ui)
{
    clear_views(ui);

    ui->media_view = make_panel(ui->root);
    if (ui->media_view == NULL) {
        return;
    }

    /* Header then body, the same flex layout as the alarm list - and
     * for the same reason: LV_PCT(100) minus a header height is
     * arithmetic on an encoded value, not a size. */
    lv_obj_set_flex_flow(ui->media_view, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(ui->media_view, 0, LV_PART_MAIN);

    lv_obj_t *header = lv_obj_create(ui->media_view);
    lv_obj_set_size(header, LV_PCT(100), HEADER_HEIGHT);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(header, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(header, 12, LV_PART_MAIN);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    /* Back leaves the picker for the editor at every depth. Going UP a
     * folder is a row in the list instead - two controls that both look
     * like "back" in a corner is how people leave a screen by accident.
     */
    lv_obj_t *back = make_text_button(header, LV_SYMBOL_LEFT, COLOR_TEXT,
                                      on_media_back, ui);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 0, 0);

    /* The folder, not the screen's job: "Alarm sound" is what the editor
     * row already said, and the one thing that changes as you browse is
     * where you are. */
    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, (ui->folder_path[0] == '\0')
                                 ? ROOT_LABEL
                                 : nn20clock_media_path_name(ui->folder_path));
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(title, 420);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &nn20clock_font_ui_bold_28,
                               LV_PART_MAIN);
    lv_obj_set_style_text_color(title, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);

    ui->media_body = lv_obj_create(ui->media_view);
    lv_obj_set_width(ui->media_body, LV_PCT(100));
    lv_obj_set_flex_grow(ui->media_body, 1);   /* the rest of the panel */
    lv_obj_set_style_bg_opa(ui->media_body, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(ui->media_body, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ui->media_body, 20, LV_PART_MAIN);
    lv_obj_set_style_pad_row(ui->media_body, 16, LV_PART_MAIN);
    lv_obj_set_flex_flow(ui->media_body, LV_FLEX_FLOW_COLUMN);

    const bool have_card = (ui->sd != NULL && nn20clock_sd_is_mounted(ui->sd));

    /*
     * The card is read before the first row is built, not after the
     * tone row: whether <random> can be offered depends on how many
     * films are in this folder, and a row cannot be added above one that
     * already exists.
     */
    if (have_card) {
        /* Heap: this is over 30 KB and the UiWorker's stack is 8. */
        ui->media = calloc(1, sizeof(*ui->media));
        if (ui->media == NULL) {
            ESP_LOGE(TAG, "out of memory for the media list");
        } else if (nn20clock_sd_list_media(ui->sd, ui->folder_path, ui->media)
                   != ESP_OK) {
            ESP_LOGW(TAG, "could not read the folder %s", ui->folder_path);
            free(ui->media);
            ui->media = NULL;
            /* The folder is gone or unreadable. Back to the root, which is
             * the one folder that is there whenever the card is. */
            ui->folder_path[0] = '\0';
        }
    }

    /* Up first, above everything: it is where the eye starts, and it is
     * the row somebody who entered the wrong folder wants. */
    if (ui->folder_path[0] != '\0') {
        (void)add_media_row(ui, "..", UP_INDEX, false, true, NULL, true);
    }

    const size_t films = playable_count(ui->media);

    /* The tone belongs to the alarm, not to a folder, so it is offered in
     * the root and nowhere else - repeating it in every folder would read
     * as "the tone for this folder", which is not a thing. */
    if (ui->folder_path[0] == '\0') {
        (void)add_media_row(ui, TONE_LABEL, TONE_INDEX,
                            !is_random(&ui->editing) &&
                                ui->editing.media_path[0] == '\0',
                            true, NULL, false);
    }

    /*
     * <random> for the folder being shown, and for that folder only: it means
     * "choose from here when the alarm rings", never "choose from
     * anywhere below here".
     *
     * Offered whenever there is a folder to choose from, and refused until
     * there are two films to choose between - see RANDOM_MINIMUM. An
     * alarm already set to <random> here keeps its tick either way:
     * films come and go over FTP, and a setting must not be silently
     * dropped because somebody deleted a file. It still rings - see
     * resolve_media() in nn20clock_app.c, which falls back the same way
     * a named file that has gone missing does.
     */
    if (ui->media != NULL) {
        /* A folder path too long to store is one the alarm could not read
         * back - see NN20CLOCK_ALARM_MEDIA_SET_MAX. Said out loud
         * rather than hidden, for the reason every other refused row is
         * shown. */
        const bool storable =
            strlen(ui->folder_path) < NN20CLOCK_ALARM_MEDIA_SET_MAX;
        const bool usable = storable && (films >= RANDOM_MINIMUM);
        const char *const why = storable ? RANDOM_UNAVAILABLE
                                         : RANDOM_TOO_DEEP;
        (void)add_media_row(ui, RANDOM_LABEL, RANDOM_INDEX,
                            is_random(&ui->editing) &&
                                strcmp(ui->folder_path,
                                       ui->editing.media_set_id) == 0,
                            usable, usable ? NULL : why, false);
    }

    if (!have_card) {
        lv_obj_t *note = lv_label_create(ui->media_body);
        lv_label_set_text(note, "no card - only the built-in tone");
        lv_obj_set_style_text_color(note, COLOR_MUTED, LV_PART_MAIN);
        lv_obj_set_style_text_font(note, &lv_font_montserrat_20,
                                   LV_PART_MAIN);
        return;
    }
    if (ui->media == NULL) {
        return;   /* already logged; the tone is still selectable */
    }

    /* Sorted folders-first by the media layer, so the two groups come out
     * in order without the screen knowing anything about it. */
    for (size_t i = 0; i < ui->media->count; i++) {
        const NN20ClockMediaEntry *const entry = &ui->media->entries[i];
        if (entry->type == NN20CLOCK_MEDIA_ENTRY_FOLDER) {
            (void)add_media_row(ui, entry->name, (intptr_t)i, false, true,
                                NULL, true);
        } else if (entry->kind != NN20CLOCK_MEDIA_KIND_UNKNOWN) {
            (void)add_media_row(ui, entry->name, (intptr_t)i,
                                !is_random(&ui->editing) &&
                                    strcmp(entry->path,
                                           ui->editing.media_path) == 0,
                                true, NULL, false);
        }
        /* Anything else is on the card but not something to wake up
         * to, and is not a row. */
    }
}

static void on_editor_sound(lv_event_t *event)
{
    AlarmUi *ui = lv_event_get_user_data(event);

    /* The rollers hold changes the record does not have yet, and
     * choosing a sound rebuilds the editor from the record. */
    collect_editor(ui);

    /* Open where the alarm's media already is, not at the root. */
    media_start_folder(&ui->editing, ui->folder_path, sizeof(ui->folder_path));
    show_media(ui);
}

/* ----------------------------------------------------------- vtable -- */

static esp_err_t alarm_ui_show(NN20ClockUiBase *base)
{
    AlarmUi *ui = (AlarmUi *)base;

    /* A container on the display's permanent screen, never a screen of
     * its own: deleting the active screen leaves LVGL holding a
     * dangling pointer. Same reason as TimeUi. */
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

    show_list(ui);
    ESP_LOGI(TAG, "alarm settings shown, %u alarm(s)",
             (unsigned)ui->alarms.count);
    return ESP_OK;
}

static esp_err_t alarm_ui_hide(NN20ClockUiBase *base)
{
    AlarmUi *ui = (AlarmUi *)base;
    if (ui->root != NULL) {
        lv_obj_add_flag(ui->root, LV_OBJ_FLAG_HIDDEN);
    }
    return ESP_OK;
}

/* This screen shows no time, so timer events are nothing to it. Design
 * 6 leaves filtering to the subscriber, and ignoring everything is a
 * legitimate outcome. */
static esp_err_t alarm_ui_timer_event(NN20ClockUiBase *base,
                                      const NN20ClockTimerEvent *event)
{
    (void)base;
    (void)event;
    return ESP_OK;
}

static void alarm_ui_destroy(NN20ClockUiBase *base)
{
    AlarmUi *ui = (AlarmUi *)base;

    if (ui->root != NULL) {
        /* Deletes both views and every widget with them. */
        lv_obj_delete(ui->root);
        ui->root = NULL;
        ui->list_view = NULL;
        ui->editor_view = NULL;
    }

    nn20clock_ui_base_deinit(base);
    free(ui);
}

static const NN20ClockUiVTable ALARM_UI_VTABLE = {
    .show = alarm_ui_show,
    .hide = alarm_ui_hide,
    .handle_touch = NULL,   /* LVGL routes touch to the widgets */
    .handle_timer_event = alarm_ui_timer_event,
    .destroy = alarm_ui_destroy,
};

/* ------------------------------------------------------------- ctor -- */

NN20ClockUiBase *nn20clock_alarm_ui_ctor(nn20_worker_ctx *ui_worker,
                                         NN20ClockManager *manager,
                                         NN20ClockUiCommandFn on_command,
                                         NN20ClockAlarmService service,
                                         NN20ClockSd *sd)
{
    if (ui_worker == NULL) {
        ESP_LOGE(TAG, "no UiWorker");
        return NULL;
    }
    if (service.list == NULL || service.save == NULL ||
        service.remove == NULL) {
        ESP_LOGE(TAG, "incomplete alarm service");
        return NULL;
    }

    /* calloc: the struct carries the 5 KB alarm list, which is exactly
     * why it is not on anyone's stack. */
    AlarmUi *ui = calloc(1, sizeof(*ui));
    if (ui == NULL) {
        ESP_LOGE(TAG, "out of memory");
        return NULL;
    }

    const NN20ClockUiBaseConfig config = {
        .vtable = &ALARM_UI_VTABLE,
        .name = "AlarmSettingsUi",
        .worker = ui_worker,
        .manager = manager,
        .on_command = on_command,
    };
    if (nn20clock_ui_base_init(&ui->super, &config) != ESP_OK) {
        free(ui);
        return NULL;
    }

    ui->service = service;
    ui->sd = sd;
    /* No LVGL call has happened here, and none may: this runs on
     * whichever worker asked for the screen. */
    return &ui->super;
}
