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
 * nn20clock_media_mgmt_ui.c - see the header.
 *
 * Everything here runs on the UiWorker: show(), destroy(), the LVGL
 * event handlers, and the refresh timer, which LVGL calls from
 * lv_timer_handler on that same worker. There is no locking and none is
 * needed.
 *
 * The widgets are built once and then only their text changes. A status
 * screen that rebuilt itself every second would throw away the user's
 * scroll position on every tick, which is exactly the moment they are
 * trying to read something.
 */
#include "nn20clock_media_mgmt_ui.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "nn20clock_fonts.h"
#include "nn20clock_media.h"

static const char *TAG = "NN20CLOCK_MEDIA_MGMT_UI";

/* Design 11's palette, shared with the other screens. */
#define COLOR_TEXT      lv_color_hex(0xD8E6F2)
#define COLOR_ACCENT    lv_color_hex(0x7FC4FF)
#define COLOR_MUTED     lv_color_hex(0x4A6A85)
#define COLOR_WARN      lv_color_hex(0xE8A33D)
#define COLOR_SURFACE   lv_color_hex(0x101820)

/* A 720x720 panel with fingers on it, as on the other screens. */
#define HEADER_HEIGHT   96
#define BUTTON_SIZE     72

/*
 * Cards are as tall as what is in them, but never shorter than this.
 *
 * Both used to be a fixed height with the title aligned to the top and
 * the value to the bottom, which is a layout that works right up until
 * the value wraps: at 28 px the server card's "No login..." line runs
 * to three, and the address in the middle ended up underneath it.
 *
 * A flex column cannot do that - the lines are stacked, so a longer one
 * pushes what follows down rather than growing into it. The minimum is
 * only so that a card with a short value still looks like the others.
 */
#define CARD_MIN_HEIGHT        148
#define SERVER_CARD_MIN_HEIGHT 216

/*
 * How long to say "starting" before believing the server is really
 * down. Opening this screen posts the start to the CoreWorker, so the
 * first refresh runs before the listener exists - and an amber "stopped"
 * for one tick, every single time, would train the eye to ignore it.
 */
#define STARTING_POLLS  3

/* The cheap half of the refresh: atomics in the FTP component. Once a
 * second is fast enough to watch a transfer start and slow enough that
 * it costs nothing. */
#define POLL_MS         1000

typedef struct {
    NN20ClockUiBase super;   /* first: a UiBase* casts to this */

    /* All borrowed, all read-only here; any may be NULL. */
    NN20ClockFtp *ftp;
    NN20ClockNet *net;
    NN20ClockSd *sd;

    /* A container on the display's permanent screen, never a screen of
     * its own - see the same note in nn20clock_time_ui.c. */
    lv_obj_t *root;
    lv_obj_t *body;
    lv_timer_t *poll_timer;

    /* The value line of each card; the titles never change. */
    lv_obj_t *server_url;     /* bright: the address to type */
    lv_obj_t *server_value;   /* muted: what that address means */
    lv_obj_t *clients_value;
    lv_obj_t *card_value;
    lv_obj_t *media_value;
    lv_obj_t *transfers_value;

    /*
     * One row per possible session, built up front and hidden when the
     * slot is empty. Fixed at the server's own maximum so a client
     * connecting never has to allocate anything on the UiWorker.
     */
    lv_obj_t *client_rows[NN20CLOCK_FTP_MAX_SESSIONS];

    /*
     * What the last card scan found. The list itself is over 20 KB, so
     * it is never a local and never kept - only these two numbers are.
     */
    size_t media_clips;
    size_t media_folders;
    size_t media_others;
    size_t media_skipped;
    uint64_t bytes_total;
    uint64_t bytes_free;
    bool card_scanned;

    /* What the last tick saw, to decide whether the card is worth
     * reading again. See the header. A mutation is anything a client
     * changed - an upload that finished, a delete, a folder created or
     * removed, a rename - which is a better question than "did somebody
     * disconnect", because a client can change the card and stay. */
    uint32_t mutations_at_scan;
    size_t sessions_last_tick;
    /* Ticks since the screen opened, for STARTING_POLLS above. */
    uint32_t polls;
} MgmtUi;

static void refresh_card(MgmtUi *ui);
static void refresh_status(MgmtUi *ui);

/* ---------------------------------------------------------- helpers -- */

/* Bytes as a human reads them. Two significant places below 10, which
 * is what makes "9.7 GB free" and "12 GB free" both look right. */
static void format_bytes(uint64_t bytes, char *out, size_t size)
{
    static const char *const UNITS[] = { "B", "KB", "MB", "GB", "TB" };
    size_t unit = 0;
    /* Kept in whole units plus a remainder rather than a double: this
     * runs on a core with no need to drag in soft-float formatting for
     * a status line. */
    uint64_t whole = bytes;
    uint64_t remainder = 0;

    while (whole >= 1024u && unit + 1 < (sizeof(UNITS) / sizeof(UNITS[0]))) {
        remainder = whole % 1024u;
        whole /= 1024u;
        unit++;
    }

    if (unit > 0 && whole < 10u) {
        snprintf(out, size, "%" PRIu64 ".%" PRIu64 " %s", whole,
                 (remainder * 10u) / 1024u, UNITS[unit]);
    } else {
        snprintf(out, size, "%" PRIu64 " %s", whole, UNITS[unit]);
    }
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
 * The shell every card on this screen is built in: a surface with
 * rounded corners whose children stack down the page.
 *
 * A column and not absolute alignment, for the reason CARD_MIN_HEIGHT
 * gives - a wrapped line has to push what follows down rather than
 * grow into it.
 */
static lv_obj_t *make_card(MgmtUi *ui, int32_t min_height)
{
    lv_obj_t *card = lv_obj_create(ui->body);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(card, min_height, LV_PART_MAIN);
    lv_obj_set_style_flex_grow(card, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(card, COLOR_SURFACE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_row(card, 8, LV_PART_MAIN);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

/*
 * A card: a title, a value beneath it, and nothing else. `handler` is
 * optional - most of these are read-only, and a chevron on a row that
 * does nothing when tapped is a lie.
 *
 * Returns the value label, which is the part that changes.
 */
static lv_obj_t *add_card(MgmtUi *ui, const char *title,
                          lv_event_cb_t handler)
{
    lv_obj_t *row = make_card(ui, CARD_MIN_HEIGHT);

    lv_obj_t *title_label = lv_label_create(row);
    lv_label_set_text(title_label, title);
    lv_obj_set_style_text_font(title_label, &nn20clock_font_ui_bold_28,
                               LV_PART_MAIN);
    lv_obj_set_style_text_color(title_label, COLOR_TEXT, LV_PART_MAIN);

    /* What the card is actually reporting, so it is drawn as something
     * to read rather than in the muted colour used for decoration.
     * Long lines wrap inside the card rather than running off it, and
     * the card grows to hold them. */
    lv_obj_t *value_label = lv_label_create(row);
    lv_label_set_text(value_label, "");
    lv_obj_set_style_text_font(value_label, &lv_font_montserrat_28,
                               LV_PART_MAIN);
    lv_obj_set_style_text_color(value_label, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_set_width(value_label, LV_PCT(88));
    lv_label_set_long_mode(value_label, LV_LABEL_LONG_WRAP);

    if (handler != NULL) {
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, handler, LV_EVENT_CLICKED, ui);

        /* Out of the column and pinned to the right edge: it belongs
         * beside the card, not after its last line. */
        lv_obj_t *chevron = lv_label_create(row);
        lv_label_set_text(chevron, LV_SYMBOL_REFRESH);
        lv_obj_add_flag(chevron, LV_OBJ_FLAG_IGNORE_LAYOUT);
        lv_obj_set_style_text_font(chevron, &lv_font_montserrat_28,
                                   LV_PART_MAIN);
        lv_obj_set_style_text_color(chevron, COLOR_ACCENT, LV_PART_MAIN);
        lv_obj_align(chevron, LV_ALIGN_RIGHT_MID, 0, 0);
    }

    return value_label;
}

/*
 * The server card, which is the odd one out: three lines instead of
 * two, with the address in the accent colour at the size the rest of
 * the UI uses for headings.
 *
 * It gets its own builder rather than a flag on add_card() because the
 * address is the only thing on this screen anybody has to read off the
 * panel and type somewhere else. Muted grey at 20 px, from across a
 * bedroom, is the wrong treatment for that one string.
 */
static void add_server_card(MgmtUi *ui)
{
    lv_obj_t *row = make_card(ui, SERVER_CARD_MIN_HEIGHT);

    lv_obj_t *title = lv_label_create(row);
    lv_label_set_text(title, "FTP server");
    lv_obj_set_style_text_font(title, &nn20clock_font_ui_bold_28,
                               LV_PART_MAIN);
    lv_obj_set_style_text_color(title, COLOR_TEXT, LV_PART_MAIN);

    /*
     * The address, at the largest face this screen has. It is copied by
     * eye onto a phone or a laptop across the room, which is a job no
     * other string here has, and every other line just moved up to
     * 28 px - leaving it there would have flattened it into them.
     *
     * Elided rather than wrapped: half an address on a second line is
     * worse than one that says it has been cut.
     */
    ui->server_url = lv_label_create(row);
    lv_label_set_text(ui->server_url, "");
    lv_obj_set_style_text_font(ui->server_url, &lv_font_montserrat_48,
                               LV_PART_MAIN);
    lv_obj_set_style_text_color(ui->server_url, COLOR_ACCENT, LV_PART_MAIN);
    lv_label_set_long_mode(ui->server_url, LV_LABEL_LONG_DOT);
    lv_obj_set_width(ui->server_url, LV_PCT(96));

    ui->server_value = lv_label_create(row);
    lv_label_set_text(ui->server_value, "");
    lv_obj_set_style_text_font(ui->server_value, &lv_font_montserrat_28,
                               LV_PART_MAIN);
    lv_obj_set_style_text_color(ui->server_value, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_set_width(ui->server_value, LV_PCT(96));
    lv_label_set_long_mode(ui->server_value, LV_LABEL_LONG_WRAP);
}

/* An indented line under the connections card, one per session. */
static lv_obj_t *add_client_row(MgmtUi *ui)
{
    lv_obj_t *label = lv_label_create(ui->body);
    lv_label_set_text(label, "");
    lv_obj_set_style_text_font(label, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_set_style_pad_left(label, 16, LV_PART_MAIN);
    lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
    return label;
}

/* ----------------------------------------------------------- events -- */

static void on_leave(lv_event_t *event)
{
    MgmtUi *ui = lv_event_get_user_data(event);

    /* Design 5 sends this back into the settings menu it was opened
     * from. The screen does not choose that; the manager does. */
    const NN20ClockUiCommand command = {
        .type = NN20CLOCK_UI_COMMAND_CLOSE_MEDIA_MANAGEMENT,
    };
    (void)nn20clock_ui_send_command(&ui->super, &command);
}

/* The one control on the screen: read the card again now. For the case
 * the automatic triggers cannot see - a client that deleted a file and
 * stayed connected. */
static void on_refresh(lv_event_t *event)
{
    MgmtUi *ui = lv_event_get_user_data(event);
    refresh_card(ui);
    refresh_status(ui);
}

static void on_poll(lv_timer_t *timer)
{
    MgmtUi *ui = lv_timer_get_user_data(timer);
    const size_t sessions = nn20clock_ftp_session_count(ui->ftp);

    ui->polls++;
    const uint32_t mutations = nn20clock_ftp_mutations(ui->ftp);

    /*
     * Read the card again only when something could have changed what
     * is on it. One counter answers that for every operation there is -
     * an upload that finished, a delete, a folder created or removed, a
     * rename - which is what this used to infer from a client hanging
     * up, and got wrong for a client that changed something and stayed
     * connected.
     *
     * The disconnect check stays anyway: it costs nothing, and it
     * catches a client that hung up mid-upload, which changes the card
     * without completing anything.
     */
    if (mutations != ui->mutations_at_scan ||
        (sessions == 0u && ui->sessions_last_tick > 0u)) {
        refresh_card(ui);
    }
    ui->sessions_last_tick = sessions;

    refresh_status(ui);
}

/* ---------------------------------------------------------- refresh -- */

/* The expensive half: reads the card. See the header for when. */
static void refresh_card(MgmtUi *ui)
{
    ui->card_scanned = false;
    ui->media_clips = 0;
    ui->media_folders = 0;
    ui->media_others = 0;
    ui->media_skipped = 0;
    ui->bytes_total = 0;
    ui->bytes_free = 0;
    ui->mutations_at_scan = nn20clock_ftp_mutations(ui->ftp);

    if (ui->sd == NULL || !nn20clock_sd_is_mounted(ui->sd)) {
        return;
    }

    (void)nn20clock_sd_usage(ui->sd, &ui->bytes_total, &ui->bytes_free);

    /*
     * Over 20 KB, and the UiWorker's stack is 8 KB - so it is allocated
     * for the length of the scan and given back. Holding one in the
     * screen for the whole time it is open would cost the same memory
     * to show two numbers.
     */
    NN20ClockMediaList *list = malloc(sizeof(*list));
    if (list == NULL) {
        ESP_LOGW(TAG, "no memory to count the media");
        return;
    }
    /*
     * The root folder only, and not the tree below it. A recursive scan
     * can take seconds on a slow card, and this runs on the UiWorker
     * from a tick - the count is a status line, not an inventory.
     */
    if (nn20clock_sd_list_media(ui->sd, "", list) == ESP_OK) {
        nn20clock_media_list_counts(list, &ui->media_clips, &ui->media_folders,
                                    &ui->media_others);
        ui->media_skipped = list->skipped;
        ui->card_scanned = true;
    }
    free(list);
}

/*
 * Add to a string being built, tracking how much of it is used.
 *
 * snprintf() returns what it WOULD have written, so adding its return
 * value to an offset walks off the end of the buffer the moment the
 * text does not fit - and then the next call gets a negative size as a
 * huge size_t. This clamps instead, and a truncated status line is the
 * worst it can do.
 */
static void append(char *out, size_t size, size_t *used, const char *format,
                   ...)
{
    if (*used >= size) {
        return;
    }

    va_list args;
    va_start(args, format);
    const int written = vsnprintf(out + *used, size - *used, format, args);
    va_end(args);

    if (written < 0) {
        return;
    }
    *used += (size_t)written;
    if (*used >= size) {
        *used = size - 1u;
    }
}

/* The cheap half: atomics, and the two numbers the last card scan
 * left behind. */
static void refresh_status(MgmtUi *ui)
{
    char text[192];

    /* ------------------------------------------------------ server -- */
    const bool running =
        (ui->ftp != NULL) && nn20clock_ftp_is_running(ui->ftp);

    if (ui->ftp == NULL) {
        lv_label_set_text(ui->server_url, "unavailable");
        lv_label_set_text(ui->server_value, "not built into this firmware");
    } else if (running) {
        char ip[NN20CLOCK_NET_IP_STRING_MAX] = "";
        const bool has_ip =
            (ui->net != NULL) &&
            (nn20clock_net_ip_string(ui->net, ip, sizeof(ip)) == ESP_OK);

        if (has_ip) {
            snprintf(text, sizeof(text), "ftp://%s:%u/", ip,
                     (unsigned)nn20clock_ftp_port(ui->ftp));
            lv_label_set_text(ui->server_url, text);
        } else {
            lv_label_set_text(ui->server_url, "no address yet");
        }

        /*
         * What that address means, under it. Design 11 asks for a
         * username and a password here; there are none, and the header
         * says why. Two empty fields would read as a bug, so the screen
         * says the true thing instead - including that this window is
         * open only while somebody is standing here looking at it.
         */
        lv_label_set_text(ui->server_value,
                          "No login. Any device on this network can add or "
                          "delete media while this screen is open.");
    } else if (ui->polls < STARTING_POLLS) {
        lv_label_set_text(ui->server_url, "starting...");
        lv_label_set_text(ui->server_value, "");
    } else {
        /*
         * It should be up: opening this screen is what starts it. So
         * the interesting part is which precondition is missing, which
         * turns "it does not work" into something the user can act on.
         */
        lv_label_set_text(ui->server_url, "not running");
        if (ui->sd == NULL || !nn20clock_sd_is_mounted(ui->sd)) {
            lv_label_set_text(ui->server_value,
                              "There is no card to serve.");
        } else if (ui->net == NULL || !nn20clock_net_is_connected(ui->net)) {
            lv_label_set_text(ui->server_value,
                              "The clock is not on the network.");
        } else {
            lv_label_set_text(ui->server_value,
                              "The server would not start; the log says "
                              "why.");
        }
    }
    lv_obj_set_style_text_color(ui->server_url,
                                running ? COLOR_ACCENT : COLOR_WARN,
                                LV_PART_MAIN);

    /* ----------------------------------------------------- clients -- */
    const size_t sessions = nn20clock_ftp_session_count(ui->ftp);
    if (sessions == 0u) {
        lv_label_set_text(ui->clients_value, "nobody connected");
    } else {
        snprintf(text, sizeof(text), "%u connected", (unsigned)sessions);
        lv_label_set_text(ui->clients_value, text);
    }

    for (size_t i = 0; i < NN20CLOCK_FTP_MAX_SESSIONS; i++) {
        char address[NN20CLOCK_NET_IP_STRING_MAX + 32] = "";
        NN20ClockFtpActivity activity = NN20CLOCK_FTP_ACTIVITY_IDLE;

        if (nn20clock_ftp_session_address(ui->ftp, i, address,
                                          sizeof(address)) != ESP_OK ||
            nn20clock_ftp_session_activity(ui->ftp, i, &activity) != ESP_OK) {
            lv_obj_add_flag(ui->client_rows[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }

        snprintf(text, sizeof(text), LV_SYMBOL_UPLOAD "  %s  -  %s", address,
                 nn20clock_ftp_activity_name(activity));
        lv_label_set_text(ui->client_rows[i], text);
        lv_obj_remove_flag(ui->client_rows[i], LV_OBJ_FLAG_HIDDEN);
    }

    /* -------------------------------------------------------- card -- */
    if (ui->sd == NULL) {
        lv_label_set_text(ui->card_value, "no card reader");
    } else if (!nn20clock_sd_is_mounted(ui->sd)) {
        lv_label_set_text(ui->card_value, "no card");
    } else {
        char free_text[24];
        char total_text[24];
        format_bytes(ui->bytes_free, free_text, sizeof(free_text));
        format_bytes(ui->bytes_total, total_text, sizeof(total_text));
        snprintf(text, sizeof(text), "mounted - %s free of %s", free_text,
                 total_text);
        lv_label_set_text(ui->card_value, text);
    }
    lv_obj_set_style_text_color(
        ui->card_value,
        (ui->sd != NULL && nn20clock_sd_is_mounted(ui->sd)) ? COLOR_MUTED
                                                            : COLOR_WARN,
        LV_PART_MAIN);

    /* ------------------------------------------------------- media -- */
    if (!ui->card_scanned) {
        lv_label_set_text(ui->media_value, "not read - tap to try again");
    } else {
        /*
         * "12 clips, 4 folders, 3 other files" - the root folder only, which
         * is what refresh_card() reads. Written out one clause at a
         * time so a card with no folders and no strays says "12 clips"
         * rather than "12 clips, 0 folders, 0 other files".
         *
         * Files that are on the card but not playable are named rather
         * than hidden, for the reason nn20clock_sd.h gives: "where did
         * my file go" is a worse experience than a count that mentions
         * it. So are entries this device refused outright, which is
         * what `skipped` holds.
         */
        size_t used = 0u;
        append(text, sizeof(text), &used, "%u clip%s",
               (unsigned)ui->media_clips,
               (ui->media_clips == 1u) ? "" : "s");
        if (ui->media_folders > 0u) {
            append(text, sizeof(text), &used, ", %u folder%s",
                   (unsigned)ui->media_folders,
                   (ui->media_folders == 1u) ? "" : "s");
        }
        if (ui->media_others > 0u) {
            append(text, sizeof(text), &used, ", %u other file%s",
                   (unsigned)ui->media_others,
                   (ui->media_others == 1u) ? "" : "s");
        }
        if (ui->media_skipped > 0u) {
            append(text, sizeof(text), &used, ", %u not listed",
                   (unsigned)ui->media_skipped);
        }
        append(text, sizeof(text), &used, " in the root - tap to read again");
        lv_label_set_text(ui->media_value, text);
    }

    /* --------------------------------------------------- transfers -- */
    snprintf(text, sizeof(text),
             "%u received, %u sent since the server started",
             (unsigned)nn20clock_ftp_uploads(ui->ftp),
             (unsigned)nn20clock_ftp_downloads(ui->ftp));
    lv_label_set_text(ui->transfers_value, text);
}

/* ----------------------------------------------------------- vtable -- */

static esp_err_t mgmt_ui_show(NN20ClockUiBase *base)
{
    MgmtUi *ui = (MgmtUi *)base;

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
    lv_obj_set_flex_flow(ui->root, LV_FLEX_FLOW_COLUMN);

    /* A header of fixed height plus a body that grows into the rest,
     * expressed with flex - see the note in nn20clock_device_ui.c about
     * why this is not "100% minus the header". */
    lv_obj_t *header = lv_obj_create(ui->root);
    lv_obj_set_size(header, LV_PCT(100), HEADER_HEIGHT);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(header, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(header, 12, LV_PART_MAIN);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back = make_icon_button(header, LV_SYMBOL_LEFT, on_leave, ui);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, "Media");
    lv_obj_set_style_text_font(title, &nn20clock_font_ui_bold_28,
                               LV_PART_MAIN);
    lv_obj_set_style_text_color(title, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);

    ui->body = lv_obj_create(ui->root);
    lv_obj_set_width(ui->body, LV_PCT(100));
    lv_obj_set_flex_grow(ui->body, 1);
    lv_obj_set_style_bg_opa(ui->body, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(ui->body, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ui->body, 20, LV_PART_MAIN);
    lv_obj_set_style_pad_row(ui->body, 12, LV_PART_MAIN);
    lv_obj_set_flex_flow(ui->body, LV_FLEX_FLOW_COLUMN);

    add_server_card(ui);
    ui->clients_value = add_card(ui, "Connections", NULL);
    for (size_t i = 0; i < NN20CLOCK_FTP_MAX_SESSIONS; i++) {
        ui->client_rows[i] = add_client_row(ui);
    }
    ui->card_value = add_card(ui, "SD card", NULL);
    ui->media_value = add_card(ui, "Media", on_refresh);
    ui->transfers_value = add_card(ui, "Transfers", NULL);

    refresh_card(ui);
    refresh_status(ui);

    ui->sessions_last_tick = nn20clock_ftp_session_count(ui->ftp);
    ui->poll_timer = lv_timer_create(on_poll, POLL_MS, ui);

    ESP_LOGI(TAG, "media management shown");
    return ESP_OK;
}

static esp_err_t mgmt_ui_hide(NN20ClockUiBase *base)
{
    MgmtUi *ui = (MgmtUi *)base;

    /* Stop polling the moment the screen is out of sight: an alarm can
     * take the display at any time, and a timer still reading the card
     * behind a playing video is the one thing this screen must not
     * do. */
    if (ui->poll_timer != NULL) {
        lv_timer_pause(ui->poll_timer);
    }
    if (ui->root != NULL) {
        lv_obj_add_flag(ui->root, LV_OBJ_FLAG_HIDDEN);
    }
    return ESP_OK;
}

static esp_err_t mgmt_ui_timer_event(NN20ClockUiBase *base,
                                     const NN20ClockTimerEvent *event)
{
    (void)base;
    (void)event;
    return ESP_OK;   /* this screen shows no time */
}

static void mgmt_ui_destroy(NN20ClockUiBase *base)
{
    MgmtUi *ui = (MgmtUi *)base;

    /* Before the widgets: the timer's callback dereferences them. */
    if (ui->poll_timer != NULL) {
        lv_timer_delete(ui->poll_timer);
        ui->poll_timer = NULL;
    }
    if (ui->root != NULL) {
        lv_obj_delete(ui->root);
        ui->root = NULL;
        ui->body = NULL;
    }

    nn20clock_ui_base_deinit(base);
    free(ui);
}

static const NN20ClockUiVTable MGMT_UI_VTABLE = {
    .show = mgmt_ui_show,
    .hide = mgmt_ui_hide,
    .handle_touch = NULL,   /* LVGL routes touch to the widgets */
    .handle_timer_event = mgmt_ui_timer_event,
    .destroy = mgmt_ui_destroy,
};

/* ------------------------------------------------------------- ctor -- */

NN20ClockUiBase *nn20clock_media_mgmt_ui_ctor(nn20_worker_ctx *ui_worker,
                                              NN20ClockManager *manager,
                                              NN20ClockUiCommandFn on_command,
                                              NN20ClockFtp *ftp,
                                              NN20ClockNet *net,
                                              NN20ClockSd *sd)
{
    if (ui_worker == NULL) {
        ESP_LOGE(TAG, "no UiWorker");
        return NULL;
    }

    MgmtUi *ui = calloc(1, sizeof(*ui));
    if (ui == NULL) {
        ESP_LOGE(TAG, "out of memory");
        return NULL;
    }

    const NN20ClockUiBaseConfig config = {
        .vtable = &MGMT_UI_VTABLE,
        .name = "MediaManagementUi",
        .worker = ui_worker,
        .manager = manager,
        .on_command = on_command,
    };
    if (nn20clock_ui_base_init(&ui->super, &config) != ESP_OK) {
        free(ui);
        return NULL;
    }

    ui->ftp = ftp;
    ui->net = net;
    ui->sd = sd;
    /* No LVGL call has happened here, and none may: this runs on
     * whichever worker asked for the screen. */
    return &ui->super;
}
