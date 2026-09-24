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
 * nn20clock_media_ui.c - see the header.
 *
 * Everything here runs on the UiWorker: show(), destroy(), and every
 * LVGL event handler, which LVGL calls from lv_timer_handler. There is
 * no locking and none is needed.
 *
 * The one thing worth knowing before reading: the media list is over
 * 30 KB, so it lives in the screen's heap-allocated struct and is never
 * a local. The UiWorker's stack is 8 KB.
 *
 * Browsing works the way the alarm editor's picker does - one folder at a
 * time, read on entry rather than cached as a tree.
 *
 * It has <random> too, and it means something slightly different here:
 * an alarm's <random> is "a different film each morning", and this one
 * is "keep drawing from this folder until the sleep timer stops you".
 * Which is why it sits next to the sleep timer rather than among the
 * files - the two are one decision.
 */
#include "nn20clock_media_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "nn20clock_fonts.h"
#include "nn20clock_media.h"

#include <inttypes.h>

static const char *TAG = "NN20CLOCK_MEDIA_UI";

/* Design 11's palette, shared with the other screens. */
#define COLOR_TEXT      lv_color_hex(0xD8E6F2)
#define COLOR_ACCENT    lv_color_hex(0x7FC4FF)
#define COLOR_MUTED     lv_color_hex(0x4A6A85)
#define COLOR_SURFACE   lv_color_hex(0x101820)

/* A 720x720 panel with fingers on it, as on the alarm screen. */
#define HEADER_HEIGHT   96
#define ROW_HEIGHT      104
#define BUTTON_SIZE     72

/*
 * The sleep timer, at the top of the list (design 11).
 *
 * "whole video" first and default: play the media once and stop, which
 * is what somebody choosing a film wants.
 *
 * The rest are durations, not cut-offs. They keep playing for as long
 * as they say - looping the clip, or drawing another under <random> -
 * and cut where they fall rather than waiting for the end. Somebody who
 * asks for half an hour is asking for half an hour of something, not
 * for at most one clip.
 *
 * A short row of fixed choices rather than a number to dial in: this is
 * a bedside clock, and the difference between 45 and 50 minutes is not
 * worth a spinner to anybody.
 */
static const uint32_t SLEEP_MINUTES[] = { 0u, 15u, 30u, 60u, 90u };
#define SLEEP_CHOICES (sizeof(SLEEP_MINUTES) / sizeof(SLEEP_MINUTES[0]))

typedef struct {
    NN20ClockUiBase super;   /* first: a UiBase* casts to this */

    NN20ClockMediaService service;
    /* Borrowed; NULL when there is no card reader. */
    NN20ClockSd *sd;

    /* A container on the display's permanent screen, never a screen of
     * its own - see the same note in nn20clock_time_ui.c. */
    lv_obj_t *root;
    lv_obj_t *body;   /* the scrollable rows */

    /*
     * One folder's contents, read when the screen is shown or a folder is
     * entered, and freed with the screen. Heap, not a member: see the
     * file comment.
     */
    NN20ClockMediaList *media;

    /* The folder being shown - "" is the root folder. Manual playback starts
     * at the root every time: unlike the alarm editor there is no
     * stored choice to return to. */
    char folder_path[NN20CLOCK_MEDIA_PATH_MAX];

    /* The title, which says which folder this is, and is rewritten rather
     * than rebuilt when one is entered. */
    lv_obj_t *title;
    /* The rows alone, so entering a folder can clear them without taking
     * the sleep-timer row above with them. */
    lv_obj_t *rows;

    /* Index into SLEEP_MINUTES. Chosen before a file is picked, and
     * sent with it. */
    size_t sleep_choice;
    lv_obj_t *sleep_label;
} MediaUi;

/* ---------------------------------------------------------- handlers -- */

static void on_back(lv_event_t *event)
{
    MediaUi *ui = lv_event_get_user_data(event);

    /* Design 8 routes this: the manager returns to CLOCK_STATE_TIME and
     * swaps the screen. This screen does not choose what comes next. */
    const NN20ClockUiCommand command = {
        .type = NN20CLOCK_UI_COMMAND_CLOSE_MEDIA_PLAYBACK,
    };
    (void)nn20clock_ui_send_command(&ui->super, &command);
}

/* Row indices that mean something other than an entry in the list. */
#define UP_INDEX     (-1)
#define RANDOM_INDEX (-2)

/*
 * A different clip every time one ends, drawn from the folder being shown.
 *
 * In angle brackets because it is not a file name and must not read
 * like one: it sits in a list of them, and FAT will not let a real file
 * on the card be called this.
 *
 * Paired with the sleep timer above it, which is the point of having it
 * here at all - "play something until I fall asleep" needs both halves,
 * and neither is much use alone.
 */
#define RANDOM_LABEL "<random>"

/* How many clips it takes before choosing between them means anything.
 * With one, <random> would be an elaborate way of naming that file. */
#define RANDOM_MINIMUM 2u

/* What the root folder is called. The card's root has no name of its own,
 * and "/" reads as a path rather than a place. */
#define ROOT_LABEL "Root"

static void fill(MediaUi *ui);

/* Rebuild the rows for whichever folder ui->folder_path now names. */
static void reopen(MediaUi *ui)
{
    lv_obj_clean(ui->rows);
    free(ui->media);
    ui->media = NULL;
    fill(ui);

    lv_label_set_text(ui->title, (ui->folder_path[0] == '\0')
                                     ? ROOT_LABEL
                                     : nn20clock_media_path_name(
                                           ui->folder_path));
}

/*
 * A row was chosen: enter it, or play it.
 *
 * For a file, the service starts playback and puts the playback screen
 * up in this screen's place, so there is nothing to do here afterwards
 * - not even hiding the list. If it refuses, the list stays up, which
 * is the right place to be when nothing started.
 */
static void on_pick(lv_event_t *event)
{
    MediaUi *ui = lv_event_get_user_data(event);
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
        reopen(ui);
        return;
    }

    const uint32_t sleep_ms = SLEEP_MINUTES[ui->sleep_choice] * 60u * 1000u;

    if (index == RANDOM_INDEX) {
        ESP_LOGI(TAG, "playing %s at random (sleep timer %" PRIu32 " min)",
                 (ui->folder_path[0] == '\0') ? "the root folder"
                                               : ui->folder_path,
                 SLEEP_MINUTES[ui->sleep_choice]);
        const esp_err_t err =
            ui->service.play_random(ui->service.ctx, ui->folder_path,
                                    sleep_ms);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "could not start random playback (0x%x)",
                     (unsigned)err);
        }
        return;
    }

    if (ui->media == NULL || index < 0 ||
        (size_t)index >= ui->media->count) {
        return;
    }

    const NN20ClockMediaEntry *const entry = &ui->media->entries[index];

    if (entry->type == NN20CLOCK_MEDIA_ENTRY_FOLDER) {
        (void)snprintf(ui->folder_path, sizeof(ui->folder_path), "%s",
                       entry->path);
        reopen(ui);
        return;
    }

    /* The entry's path, not its name: the service takes a path relative
     * to the card's root, and this screen never builds one itself. */
    ESP_LOGI(TAG, "playing %s (sleep timer %" PRIu32 " min)", entry->path,
             SLEEP_MINUTES[ui->sleep_choice]);

    const esp_err_t err = ui->service.play(ui->service.ctx, entry->path,
                                           sleep_ms);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not play %s (0x%x)", entry->path, (unsigned)err);
    }
}

/* What the sleep row reads: "all", or a number of minutes. */
static void sleep_text(const MediaUi *ui, char *out, size_t size)
{
    const uint32_t minutes = SLEEP_MINUTES[ui->sleep_choice];

    if (minutes == 0u) {
        (void)snprintf(out, size, "Sleep timer: whole video");
    } else {
        (void)snprintf(out, size, "Sleep timer: %" PRIu32 " min", minutes);
    }
}

/*
 * The row cycles rather than opening a second screen. Five choices is
 * not worth a screen of its own, and cycling keeps the whole decision -
 * timer and file - on one page.
 */
static void on_sleep(lv_event_t *event)
{
    MediaUi *ui = lv_event_get_user_data(event);

    ui->sleep_choice = (ui->sleep_choice + 1u) % SLEEP_CHOICES;

    char text[48];
    sleep_text(ui, text, sizeof(text));
    lv_label_set_text(ui->sleep_label, text);
}

/* ----------------------------------------------------------- widgets -- */

static lv_obj_t *make_panel(lv_obj_t *parent)
{
    lv_obj_t *panel = lv_obj_create(parent);
    if (panel == NULL) {
        return NULL;
    }
    lv_obj_set_size(panel, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(panel, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(panel, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(panel, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(panel, 0, LV_PART_MAIN);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    return panel;
}

static lv_obj_t *make_back_button(lv_obj_t *parent, MediaUi *ui)
{
    lv_obj_t *button = lv_button_create(parent);
    if (button == NULL) {
        return NULL;
    }
    lv_obj_set_size(button, BUTTON_SIZE, BUTTON_SIZE);
    lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(button, COLOR_SURFACE, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(button, 0, LV_PART_MAIN);

    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(label, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_center(label);

    lv_obj_add_event_cb(button, on_back, LV_EVENT_CLICKED, ui);
    return button;
}

/*
 * What a row is, which decides how it is drawn.
 *
 * A folder is somewhere to go, a file is something to play, and <random>
 * is neither - they sit in one scrolling column, and the difference has
 * to be visible without reading, because tapping the wrong one is the
 * difference between navigating and starting a film.
 */
typedef enum {
    ROW_FILE,
    ROW_FOLDER,      /* inverted: the accent as the fill */
    ROW_RANDOM
} RowKind;

/* One row. The index travels in the widget, so the handler needs no
 * lookup table; it is a small integer and never dereferenced. */
static void add_row(MediaUi *ui, const char *name, intptr_t index,
                    RowKind kind)
{
    lv_obj_t *row = lv_obj_create(ui->rows);
    if (row == NULL) {
        return;
    }
    lv_obj_set_size(row, LV_PCT(100), ROW_HEIGHT);
    /* Fixed height in a scrolling column: without this the flex layout
     * squeezes the rows to fit rather than scrolling them. */
    lv_obj_set_style_flex_grow(row, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(row,
                              (kind == ROW_FOLDER) ? COLOR_ACCENT
                                                : COLOR_SURFACE,
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 16, LV_PART_MAIN);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_user_data(row, (void *)index);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, on_pick, LV_EVENT_CLICKED, ui);

    /* On the accent fill, the surface colour is the readable one. */
    const lv_color_t colour = (kind == ROW_FOLDER) ? COLOR_SURFACE
                                                   : COLOR_TEXT;

    const char *glyph = LV_SYMBOL_PLAY;
    if (kind == ROW_FOLDER) {
        glyph = LV_SYMBOL_DIRECTORY;
    } else if (kind == ROW_RANDOM) {
        glyph = LV_SYMBOL_SHUFFLE;
    }

    lv_obj_t *icon = lv_label_create(row);
    lv_label_set_text(icon, glyph);
    lv_obj_set_style_text_color(icon,
                                (kind == ROW_FOLDER) ? colour : COLOR_ACCENT,
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, name);
    /* A long file name gets cut rather than pushing the row wider; the
     * whole name is on the card, not on the screen. */
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(label, LV_PCT(80));
    lv_obj_set_style_text_font(label, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, colour, LV_PART_MAIN);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 60, 0);
}

static void say(MediaUi *ui, const char *text)
{
    lv_obj_t *note = lv_label_create(ui->rows);
    if (note == NULL) {
        return;
    }
    lv_label_set_text(note, text);
    lv_obj_set_style_text_color(note, COLOR_MUTED, LV_PART_MAIN);
    lv_obj_set_style_text_font(note, &lv_font_montserrat_20, LV_PART_MAIN);
}

/*
 * Fill the rows with what is in the folder being shown.
 *
 * Every outcome that is not a list of rows says why in the same place
 * the rows would have been. An empty screen would be indistinguishable
 * from a broken one.
 */
static void fill(MediaUi *ui)
{
    if (ui->sd == NULL || !nn20clock_sd_is_mounted(ui->sd)) {
        say(ui, "no card");
        return;
    }

    /* Heap: over 30 KB, and the UiWorker's stack is 8. */
    ui->media = calloc(1, sizeof(*ui->media));
    if (ui->media == NULL) {
        ESP_LOGE(TAG, "out of memory for the media list");
        say(ui, "out of memory");
        return;
    }
    if (nn20clock_sd_list_media(ui->sd, ui->folder_path, ui->media)
        != ESP_OK) {
        ESP_LOGW(TAG, "could not read the folder %s", ui->folder_path);
        free(ui->media);
        ui->media = NULL;
        /* Back to the root, which is the one folder that is there whenever
         * the card is - so the next tap is not into the same hole. */
        ui->folder_path[0] = '\0';
        say(ui, "that folder could not be read");
        return;
    }

    /* Up first, above everything: it is where the eye starts, and it is
     * the row somebody who entered the wrong folder wants. */
    if (ui->folder_path[0] != '\0') {
        add_row(ui, "..", UP_INDEX, ROW_FOLDER);
    }

    size_t clips = 0;
    nn20clock_media_list_counts(ui->media, &clips, NULL, NULL);

    /*
     * <random> above the card's own rows, with the sleep timer, because
     * the two are one decision: "play something until I fall asleep".
     * It draws from THIS folder and never from the folders below it, which is
     * the same promise the alarm editor makes.
     *
     * Offered only once there are two clips to choose between - with
     * one, it is an elaborate way of naming that file.
     */
    if (clips >= RANDOM_MINIMUM) {
        add_row(ui, RANDOM_LABEL, RANDOM_INDEX, ROW_RANDOM);
    }

    /* Sorted folders-first by the media layer, so the two groups come out
     * in order without the screen knowing anything about it. */
    size_t shown = 0;
    for (size_t i = 0; i < ui->media->count; i++) {
        const NN20ClockMediaEntry *const entry = &ui->media->entries[i];
        if (entry->type == NN20CLOCK_MEDIA_ENTRY_FOLDER) {
            add_row(ui, entry->name, (intptr_t)i, ROW_FOLDER);
            shown++;
        } else if (entry->kind != NN20CLOCK_MEDIA_KIND_UNKNOWN) {
            add_row(ui, entry->name, (intptr_t)i, ROW_FILE);
            shown++;
        }
        /* Anything else is on the card but not something this plays. */
    }

    if (shown == 0) {
        say(ui, (ui->folder_path[0] == '\0') ? "nothing to play on the card"
                                          : "nothing to play in this folder");
    }
}

/* ------------------------------------------------------------ hooks -- */

static esp_err_t media_ui_show(NN20ClockUiBase *base)
{
    MediaUi *ui = (MediaUi *)base;

    /* Built here, not in the constructor: this is the UiWorker. */
    ui->root = make_panel(lv_screen_active());
    if (ui->root == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /*
     * Header then body, the same flex layout as the alarm screen - and
     * for the same reason: LV_PCT(100) minus a header height is
     * arithmetic on an encoded value, not a size, and produced a body
     * one pixel high.
     */
    lv_obj_set_flex_flow(ui->root, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *header = lv_obj_create(ui->root);
    if (header == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_set_size(header, LV_PCT(100), HEADER_HEIGHT);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(header, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(header, 12, LV_PART_MAIN);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back = make_back_button(header, ui);
    if (back == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 0, 0);

    /* The folder being shown rather than "Play": the screen's job is
     * already obvious from how it was reached, and the one thing that
     * changes as you browse is where you are. */
    ui->title = lv_label_create(header);
    lv_label_set_text(ui->title, (ui->folder_path[0] == '\0')
                                     ? ROOT_LABEL
                                     : nn20clock_media_path_name(
                                           ui->folder_path));
    lv_label_set_long_mode(ui->title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(ui->title, 420);
    lv_obj_set_style_text_align(ui->title, LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(ui->title, &nn20clock_font_ui_bold_28,
                               LV_PART_MAIN);
    lv_obj_set_style_text_color(ui->title, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_align(ui->title, LV_ALIGN_CENTER, 0, 0);

    ui->body = lv_obj_create(ui->root);
    if (ui->body == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_set_width(ui->body, LV_PCT(100));
    lv_obj_set_flex_grow(ui->body, 1);   /* the rest of the panel */
    lv_obj_set_style_bg_opa(ui->body, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(ui->body, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ui->body, 20, LV_PART_MAIN);
    lv_obj_set_style_pad_row(ui->body, 16, LV_PART_MAIN);
    lv_obj_set_flex_flow(ui->body, LV_FLEX_FLOW_COLUMN);

    /*
     * The sleep timer sits above the files, as design 11 places it: it
     * applies to whichever one is chosen next, so it has to be decided
     * before rather than after.
     */
    lv_obj_t *sleep_row = lv_obj_create(ui->body);
    if (sleep_row == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_set_size(sleep_row, LV_PCT(100), ROW_HEIGHT);
    lv_obj_set_style_flex_grow(sleep_row, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sleep_row, COLOR_SURFACE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sleep_row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(sleep_row, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(sleep_row, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_all(sleep_row, 16, LV_PART_MAIN);
    lv_obj_remove_flag(sleep_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(sleep_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(sleep_row, on_sleep, LV_EVENT_CLICKED, ui);

    lv_obj_t *moon = lv_label_create(sleep_row);
    lv_label_set_text(moon, LV_SYMBOL_POWER);
    lv_obj_set_style_text_color(moon, COLOR_MUTED, LV_PART_MAIN);
    lv_obj_set_style_text_font(moon, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_align(moon, LV_ALIGN_LEFT_MID, 0, 0);

    ui->sleep_label = lv_label_create(sleep_row);
    char text[48];
    sleep_text(ui, text, sizeof(text));
    lv_label_set_text(ui->sleep_label, text);
    lv_obj_set_style_text_font(ui->sleep_label, &lv_font_montserrat_28,
                               LV_PART_MAIN);
    lv_obj_set_style_text_color(ui->sleep_label, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_align(ui->sleep_label, LV_ALIGN_LEFT_MID, 60, 0);

    /* The rows in a container of their own, under the sleep row:
     * entering a folder clears this and nothing else, so the sleep timer
     * the user already set survives the navigation. */
    ui->rows = lv_obj_create(ui->body);
    if (ui->rows == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_set_width(ui->rows, LV_PCT(100));
    lv_obj_set_flex_grow(ui->rows, 1);
    lv_obj_set_style_bg_opa(ui->rows, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(ui->rows, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ui->rows, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(ui->rows, 16, LV_PART_MAIN);
    lv_obj_set_flex_flow(ui->rows, LV_FLEX_FLOW_COLUMN);

    fill(ui);

    /* A screen shown after being hidden keeps its objects, so clear the
     * flag rather than assume it was never set. */
    lv_obj_remove_flag(ui->root, LV_OBJ_FLAG_HIDDEN);

    ESP_LOGI(TAG, "media list shown");
    return ESP_OK;
}

static esp_err_t media_ui_hide(NN20ClockUiBase *base)
{
    MediaUi *ui = (MediaUi *)base;

    /* Hidden, not destroyed: the manager may show this screen again
     * without rebuilding it. destroy() is what frees the objects. */
    if (ui->root != NULL) {
        lv_obj_add_flag(ui->root, LV_OBJ_FLAG_HIDDEN);
    }
    return ESP_OK;
}

static void media_ui_destroy(NN20ClockUiBase *base)
{
    MediaUi *ui = (MediaUi *)base;

    if (ui->root != NULL) {
        /* Deletes the header, the body, and the rows with it; they are
         * its children. The display's screen is left alone. */
        lv_obj_delete(ui->root);
        ui->root = NULL;
        ui->body = NULL;
        ui->rows = NULL;
        ui->title = NULL;
    }

    free(ui->media);
    ui->media = NULL;

    nn20clock_ui_base_deinit(base);
    free(ui);
}

static const NN20ClockUiVTable MEDIA_UI_VTABLE = {
    .show = media_ui_show,
    .hide = media_ui_hide,
    /* Touch arrives through LVGL's own event handlers - the rows are
     * objects, and LVGL knows which one was pressed. */
    .handle_touch = NULL,
    /* Nothing here shows the time, so a tick has nothing to change. */
    .handle_timer_event = NULL,
    .destroy = media_ui_destroy,
};

/* ------------------------------------------------------------- ctor -- */

NN20ClockUiBase *nn20clock_media_ui_ctor(nn20_worker_ctx *ui_worker,
                                         NN20ClockManager *manager,
                                         NN20ClockUiCommandFn on_command,
                                         NN20ClockMediaService service,
                                         NN20ClockSd *sd)
{
    if (ui_worker == NULL) {
        ESP_LOGE(TAG, "no UiWorker");
        return NULL;
    }
    if (service.play == NULL || service.play_random == NULL) {
        /* A list of files nothing can start is a screen that lies about
         * what it does. */
        ESP_LOGE(TAG, "no way to start playback");
        return NULL;
    }

    MediaUi *ui = calloc(1, sizeof(*ui));
    if (ui == NULL) {
        ESP_LOGE(TAG, "out of memory");
        return NULL;
    }

    const NN20ClockUiBaseConfig config = {
        .vtable = &MEDIA_UI_VTABLE,
        .name = "MediaPlaybackUi",
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
