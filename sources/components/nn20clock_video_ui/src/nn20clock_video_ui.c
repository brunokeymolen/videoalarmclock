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
 * nn20clock_video_ui.c - see the header.
 *
 * Everything here runs on the UiWorker, including the callback the
 * player makes when the screen is touched during video - the player
 * posts it there rather than calling it from its own task, so this file
 * never has to think about two threads.
 */
#include "nn20clock_video_ui.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "nn20clock_display.h"
#include "nn20clock_fonts.h"
#include "nn20clock_media.h"
#include "nn20clock_timefmt.h"

static const char *TAG = "NN20CLOCK_VIDEO_UI";

/*
 * Design 11's palette, pushed a little warmer than the clock face: this
 * screen is meant to be noticed, not read at 3am.
 */
#define COLOR_TEXT    lv_color_hex(0xFFFFFF)
/* The clock face's blue, so the time reads as the same thing on both
 * screens rather than as a label on this one. */
#define COLOR_DIGITS  lv_color_hex(0x7FC4FF)
#define COLOR_ACCENT  lv_color_hex(0xFF9640)
#define COLOR_DIM     lv_color_hex(0x7A6250)
#define COLOR_SNOOZE  lv_color_hex(0x2E6FB7)
#define COLOR_OFF     lv_color_hex(0xB03A2E)
/* Stopping a film is not turning off an alarm: the same shape, without
 * the colour that means "you are about to end something that woke
 * you". */
#define COLOR_STOP    lv_color_hex(0x2E6FB7)
#define COLOR_SCRIM   lv_color_hex(0x000000)

/*
 * How much of the panel each button takes. Two buttons, stacked, each
 * most of the width: a target this size does not need to be aimed at.
 *
 * 26% leaves 48% of the panel between them, which is what the time
 * needs at 140 px. Bigger buttons would be no easier to hit and would
 * squeeze the one thing on this screen worth reading from across a
 * room.
 */
#define BUTTON_WIDTH_PCT  80
/*
 * Fixed heights now that the window is a flex column. The panel is 720
 * tall: 140 for the time, two 150-pixel buttons for an alarm, two
 * 70-pixel slider rows, and the padding between them.
 */
#define BUTTON_HEIGHT      150
#define SLIDER_ROW_HEIGHT  70
#define SLIDER_HEIGHT      40
#define ACTIONS_PAD        16

/* How often to check whether there is a picture. Fast enough that the
 * face never lingers over a film, slow enough to be free. */
#define FACE_POLL_MS 200

/* The actions view sits over the video, so it is dimmed rather than
 * opaque - it should be obvious that the alarm is still playing behind
 * it. */
#define SCRIM_OPACITY LV_OPA_70

/*
 * How long the buttons stay up before the alarm goes back to being the
 * whole screen. Long enough to read two words and reach for one of
 * them; short enough that a knock on the bedside table does not leave
 * the video hidden behind a menu for the rest of the alarm.
 */
#define ACTIONS_VISIBLE_MS 5000

/*
 * The buttons ignore presses for this long after appearing.
 *
 * They arrive under a finger that was already moving - the touch that
 * asked for them - and snoozing an alarm because of a press aimed at
 * bare video would be infuriating. A quarter of a second is below what
 * anyone can aim in, and well under a deliberate second press.
 */
#define ACTIONS_GUARD_MS 250


/*
 * How often to ask the player again when it was busy, and how long to
 * keep asking.
 *
 * Design 5 lets an alarm interrupt manual playback, and the outgoing
 * screen's halt is asynchronous - it has to be, because the player
 * blocks on the UiWorker for every frame and a screen that waited for
 * it would be waiting for itself. So the alarm's screen can be built
 * while the film it replaced is still winding down, and the player
 * refuses to start a second thing.
 *
 * Asking again costs nothing and needs no locking. It also lands at
 * exactly the right moment: LVGL timers do not run while the player
 * owns the panel, so the first tick after the old playback lets go is
 * the first tick this fires on.
 *
 * An alarm keeps asking for as long as its screen is up - design 13
 * has no acceptable silent outcome. Manual playback gives up: the user
 * is watching, and a film that takes seconds to start is a fault worth
 * showing rather than hiding.
 */
#define START_RETRY_MS    50
#define START_GIVE_UP_MS  3000

/*
 * How long the ringing face stays out of the way while a <random>
 * alarm changes films.
 *
 * The player hands the panel back the moment a film ends and takes it
 * again when the next one has opened, and in between there is no
 * picture - which is exactly what the bell-and-time face is for. Left
 * to itself it would flash up for the fraction of a second the card
 * takes, every film, all morning.
 *
 * Bounded rather than held until the next picture arrives, because the
 * next film may be one this device cannot decode: then there is no
 * picture coming, the player is ringing the tone, and the face is the
 * right thing on the screen. Two seconds is far longer than opening a
 * file takes and far shorter than anyone would stare at a black panel
 * wondering what went wrong.
 */
#define NEXT_FILM_GRACE_MS 2000

/*
 * There is no ring bound here. It used to be an lv_timer on this
 * screen, and that timer never fired: LVGL's timers do not run while
 * the player owns the panel (see nn20clock_display_video_begin), so a
 * video alarm was unbounded, and the hour's worth of overdue timer went
 * off the instant a touch handed the panel back - stopping the alarm
 * with the buttons the user had just asked for still going up.
 *
 * The bound is the ClockManager's now, on a worker that keeps running
 * whoever owns the panel, and measured from the alarm's own time so a
 * snooze does not extend it. See NN20CLOCK_MANAGER_MAX_RING_SECONDS.
 */

/*
 * Where a ringing alarm's sound starts, and how long it takes to reach
 * the volume setting.
 *
 * An alarm that arrives at full volume in the first instant wakes you
 * by fright. Coming up out of the quiet over a few seconds is enough
 * time to surface in and not nearly enough to sleep through - and the
 * floor is a floor, not silence: 15 percent is audible in a quiet
 * room, so even somebody who stops the alarm in its first second has
 * heard it. Design 13's rule is that an alarm is never silent, and a
 * ramp that started at nothing would break it for as long as it took
 * to climb.
 *
 * Per ringing session, unlike the hour bound above: a snooze destroys
 * this screen, so the alarm fades in again nine minutes later. That is
 * right for the same reason it is right the first time - what comes
 * back is a room that has been quiet since.
 *
 * The setting is never touched by any of this. The volume slider on
 * this screen shows the setting throughout, the ramp lives only in the
 * codec, and moving the slider ends the ramp - see
 * nn20clock_audio_fade_volume().
 */
#define ALARM_FADE_FROM_VOLUME 15u
#define ALARM_FADE_MS          7000u

typedef struct {
    NN20ClockUiBase super;   /* first: a UiBase* casts to this */

    NN20ClockPlayer *player;   /* borrowed */
    NN20ClockDeviceService device;
    NN20ClockVideoUiOrigin origin;
    char media_path[NN20CLOCK_MEDIA_PATH_MAX];
    /*
     * Where a <random> playback gets its next clip, and NULL for a
     * fixed one - see NN20ClockVideoUiNextMediaFn. When it is set,
     * media_path above is this screen's own copy of what is playing
     * now rather than the one thing it was built to play: it is
     * replaced at every end.
     */
    NN20ClockVideoUiNextMediaFn next_media;
    void *next_media_ctx;
    uint8_t volume;
    /*
     * The sleep timer the picker was showing, in milliseconds, and how
     * long ago playback first started.
     *
     * Zero is design 11's `all`: play the media once and stop. Anything
     * else is a DURATION, not a cut-off for one clip - see
     * remaining_ms(). Ignored for an alarm, which has no such end.
     */
    uint32_t sleep_ms;
    uint32_t started_at;

    /* A container on the display's permanent screen, never a screen of
     * its own - see the same note in nn20clock_time_ui.c. */
    lv_obj_t *root;

    /* The two views. Exactly one is visible at a time. */
    lv_obj_t *playback;
    lv_obj_t *actions;

    /*
     * Playback view. `tone_face` is the bell, the time and the hint -
     * everything that only makes sense when there is no picture. It
     * stays hidden while the player has video, because otherwise it is
     * what shows through every moment LVGL holds the panel: while a
     * finger is still down and the player is waiting for it to lift,
     * and for a frame or two on the way in and out of the screen.
     */
    lv_obj_t *tone_face;
    lv_obj_t *time_label;
    /* The same time, large, on the actions view. */
    lv_obj_t *actions_time_label;

    /*
     * Puts the alarm back on the whole screen if the buttons go
     * untouched. Repeating and paused by its own callback, never a
     * repeat count of one - LVGL deletes a timer whose count runs out,
     * and this screen keeps the pointer. See nn20clock_time_ui.c, where
     * that cost a heap corruption.
     */
    lv_timer_t *actions_timer;
    uint32_t actions_shown_at;   /* lv_tick_get(), for ACTIONS_GUARD_MS */

    /* Only exists while the player is busy with what came before. See
     * START_RETRY_MS. Deleted the moment playback starts. */
    lv_timer_t *start_timer;
    uint32_t start_asked_at;
    /* Keeps tone_face in step with whether there is a picture. */
    lv_timer_t *face_timer;

    /* The two sliders, and the settings they move. Read back on
     * release to persist, so a drag writes flash once rather than on
     * every pixel of travel. */
    lv_obj_t *volume_slider;
    lv_obj_t *brightness_slider;

    char shown_time[NN20CLOCK_HHMM_SIZE];
    bool showing_actions;
    /*
     * Set while a <random> alarm is between films - see
     * NEXT_FILM_GRACE_MS. The tick is when the next one was asked for,
     * which is what bounds the wait.
     */
    bool changing_film;
    uint32_t changing_film_at;

    /*
     * Whether anything has actually started playing yet.
     *
     * The fade is per ringing, not per film: a <random> alarm moving on
     * to its next film is the same alarm still ringing, and dropping it
     * back to ALARM_FADE_FROM_VOLUME every few minutes would be an
     * alarm that gets quieter the longer it is ignored. A start that
     * was retried still fades - it is the first sound that counts, not
     * the first attempt.
     */
    bool began;
} VideoUi;

/* ------------------------------------------------------------ views -- */

/* Defined below; show_actions() needs it before its definition. */
static void sync_tone_face(VideoUi *ui);

/* Runs on the UiWorker. */
static void show_actions(VideoUi *ui, bool actions)
{
    if (ui->actions == NULL || ui->playback == NULL) {
        return;
    }
    if (ui->showing_actions == actions) {
        return;
    }
    ui->showing_actions = actions;

    /*
     * The panel first, then the widgets. Asking the player to give the
     * panel back before the buttons exist would show a frozen video
     * frame for an instant; asking for it back afterwards means LVGL
     * draws the buttons the moment it has somewhere to draw them.
     *
     * This only records the wish - the player hands the panel over
     * between frames, and waits for the finger to leave the glass
     * first. Nothing here waits for it.
     */
    (void)nn20clock_player_set_video_visible(ui->player, !actions);

    if (actions) {
        ui->actions_shown_at = lv_tick_get();
        /* One time on screen, not two: the ringing face behind would
         * otherwise show its own small clock through the scrim. */
        lv_obj_add_flag(ui->playback, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(ui->actions, LV_OBJ_FLAG_HIDDEN);
        if (ui->actions_timer != NULL) {
            lv_timer_reset(ui->actions_timer);
            lv_timer_resume(ui->actions_timer);
        }
    } else {
        lv_obj_add_flag(ui->actions, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(ui->playback, LV_OBJ_FLAG_HIDDEN);
        /*
         * Before the view is on screen, not after. The player only
         * takes the panel back once the finger has left the glass, so
         * on a long press this view is what is showing for as long as
         * the press lasts - and it must not be showing a bell over a
         * film that is still playing.
         */
        sync_tone_face(ui);
        if (ui->actions_timer != NULL) {
            lv_timer_pause(ui->actions_timer);
        }
    }
}

/*
 * Show the bell-and-time face only when there is no picture.
 *
 * Polled rather than driven by an event, because the thing it tracks -
 * whether the player has a decodable file open - becomes true a moment
 * after playback starts and can become false at any point if the file
 * fails. A flag this screen set once would be wrong in both
 * directions.
 *
 * Cheap: LVGL timers do not run at all while the player owns the panel,
 * so this ticks only in the moments when it has something to decide.
 */
static void sync_tone_face(VideoUi *ui)
{
    if (ui->tone_face == NULL) {
        return;
    }

    if (nn20clock_player_has_video(ui->player)) {
        ui->changing_film = false;
        lv_obj_add_flag(ui->tone_face, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    /* No picture yet, but one is on its way: black for a moment beats
     * the face flashing up between every pair of films. */
    if (ui->changing_film) {
        if (lv_tick_elaps(ui->changing_film_at) < (uint32_t)NEXT_FILM_GRACE_MS) {
            lv_obj_add_flag(ui->tone_face, LV_OBJ_FLAG_HIDDEN);
            return;
        }
        ui->changing_film = false;
    }

    lv_obj_remove_flag(ui->tone_face, LV_OBJ_FLAG_HIDDEN);
}

static void on_face_tick(lv_timer_t *timer)
{
    sync_tone_face(lv_timer_get_user_data(timer));
}

/*
 * The window is being used: start its countdown again.
 *
 * ACTIONS_VISIBLE_MS is an IDLE timeout, and until this existed it was
 * not counting idleness - it started when the window opened and ran to
 * the end regardless. Dragging a slider for five seconds made the whole
 * window disappear from under the finger doing the dragging, the player
 * took the panel back for a moment, and the same finger reopened it: a
 * black flash and a lost adjustment.
 */
static void actions_keep_alive(VideoUi *ui)
{
    if (ui->actions_timer != NULL && ui->showing_actions) {
        lv_timer_reset(ui->actions_timer);
    }
}

/* Any press inside the window counts, including one on a button or a
 * slider that has not finished being used yet. */
static void on_actions_pressing(lv_event_t *event)
{
    actions_keep_alive(lv_event_get_user_data(event));
}

/* Nobody chose anything: back to the alarm. */
static void on_actions_timeout(lv_timer_t *timer)
{
    VideoUi *ui = lv_timer_get_user_data(timer);

    show_actions(ui, false);
    /* Paused by show_actions() above, and again here so the timer is
     * left stopped even if it was already hidden. */
    lv_timer_pause(timer);
}

/*
 * Whether the buttons have been up long enough to mean it. See
 * ACTIONS_GUARD_MS.
 */
static bool actions_are_settled(const VideoUi *ui)
{
    return lv_tick_elaps(ui->actions_shown_at) >= (uint32_t)ACTIONS_GUARD_MS;
}

/* The player saw a touch while it owned the panel. Already posted to
 * the UiWorker by the player, so this is safe LVGL ground. */
static void on_player_touch(void *ctx)
{
    VideoUi *ui = ctx;
    show_actions(ui, true);
}

/* A touch on the playback view, when LVGL is the one seeing it - the
 * built-in tone case, where there is no video and LVGL keeps the panel. */
static void on_playback_clicked(lv_event_t *event)
{
    VideoUi *ui = lv_event_get_user_data(event);
    show_actions(ui, true);
}

/* A touch on the actions view but not on a button: back to the alarm. */
static void on_scrim_clicked(lv_event_t *event)
{
    VideoUi *ui = lv_event_get_user_data(event);

    if (!actions_are_settled(ui)) {
        return;
    }
    /* Back to the alarm, whether that is the video or the ringing face
     * over the built-in tone. Either is better than a menu. */
    show_actions(ui, false);
}

/* ---------------------------------------------------------- buttons -- */

static void send(VideoUi *ui, NN20ClockUiCommandType type)
{
    const NN20ClockUiCommand command = { .type = type, .value = 0 };
    (void)nn20clock_ui_send_command(&ui->super, &command);
}

static void on_snooze_clicked(lv_event_t *event)
{
    VideoUi *ui = lv_event_get_user_data(event);

    if (!actions_are_settled(ui)) {
        return;
    }

    ESP_LOGI(TAG, "snooze");
    /*
     * The position is kept, not cleared: the player remembers where it
     * stopped and picks the same file up there when the snooze fires.
     */
    /*
     * Silence first. The manager will change state and destroy this
     * screen, but that is a round trip through two workers and the
     * alarm should stop the moment the button is pressed.
     */
    (void)nn20clock_player_halt(ui->player);
    send(ui, NN20CLOCK_UI_COMMAND_SNOOZE_ALARM);
}

/*
 * Manual playback, stopped by hand.
 *
 * No snooze and no dismissal: nothing is scheduled, so there is nothing
 * to put off or to delete. The position is not kept either - the
 * player was told not to remember it, and the snooze that owns that
 * memory has nothing to do with this.
 */
static void on_stop_clicked(lv_event_t *event)
{
    VideoUi *ui = lv_event_get_user_data(event);

    if (!actions_are_settled(ui)) {
        return;
    }

    ESP_LOGI(TAG, "playback stopped");
    /* Silence first, as with the alarm: the manager changing state and
     * destroying this screen is a round trip through two workers. */
    (void)nn20clock_player_halt(ui->player);
    send(ui, NN20CLOCK_UI_COMMAND_CLOSE_MEDIA_PLAYBACK);
}

/* Defined with the starting code below; on_player_done() needs it
 * before its definition. */
static void play_next(VideoUi *ui);
static uint32_t remaining_ms(const VideoUi *ui);

/*
 * The media ended by itself, with its hour behind it.
 *
 * Posted here by the player and run on the UiWorker, so this is safe
 * LVGL ground. Registered by manual playback, and by the one kind of
 * alarm that has an end to hear about: a <random> one, which asked the
 * player for a single pass so that it could choose again here. Every
 * other alarm loops inside the player and never reaches this.
 */
static void on_player_done(void *ctx)
{
    VideoUi *ui = ctx;

    if (ui->origin == NN20CLOCK_VIDEO_UI_ALARM) {
        /* Not a moment to close anything: the alarm is still ringing
         * and is owed the next film. */
        play_next(ui);
        return;
    }

    /*
     * Manual playback. Three ways to arrive here, and only one of them
     * means "keep going".
     *
     * The player posts this whenever it finishes without having been
     * halted - which includes finishing because the sleep timer ran
     * out, not only because the clip ended. So the timer is checked
     * first: with none set, one clip was the whole request; with one
     * set and expired, the evening is over either way.
     */
    if (ui->sleep_ms == 0u) {
        ESP_LOGI(TAG, "the media ended");
        send(ui, NN20CLOCK_UI_COMMAND_CLOSE_MEDIA_PLAYBACK);
        return;
    }
    if (remaining_ms(ui) == 0u) {
        ESP_LOGI(TAG, "the sleep timer ran out");
        send(ui, NN20CLOCK_UI_COMMAND_CLOSE_MEDIA_PLAYBACK);
        return;
    }

    if (ui->next_media == NULL) {
        /*
         * A fixed clip with time still on the timer. The player was
         * asked to loop, so it should not have ended - reaching here
         * means it gave up on the file. Nothing to be gained by
         * restarting it into the same failure.
         */
        ESP_LOGW(TAG, "playback ended early with %" PRIu32 " ms left",
                 remaining_ms(ui));
        send(ui, NN20CLOCK_UI_COMMAND_CLOSE_MEDIA_PLAYBACK);
        return;
    }

    play_next(ui);
}

static void on_off_clicked(lv_event_t *event)
{
    VideoUi *ui = lv_event_get_user_data(event);

    if (!actions_are_settled(ui)) {
        return;
    }

    ESP_LOGI(TAG, "alarm off");
    (void)nn20clock_player_halt(ui->player);
    /*
     * Off is not snooze: nothing is coming back, so the position and
     * the last frame go with it.
     *
     * Kept here even though the app forgets on every dismissal - see
     * its on_state_change(). This one runs at the press rather than
     * after the screen swap, so nothing can read the frame of an alarm
     * that has just been dismissed.
     */
    nn20clock_player_forget_position(ui->player);
    send(ui, NN20CLOCK_UI_COMMAND_STOP_ALARM);
}

/* ---------------------------------------------------------- sliders -- */

/*
 * Live while dragging, stored when the finger comes off.
 *
 * Both halves matter. Applying on every change is what makes a slider
 * usable at all - you set the volume by hearing it, not by reading a
 * number - and storing only on release is what keeps a drag from
 * writing flash a hundred times on its way across the screen.
 */
static void on_volume_changed(lv_event_t *event)
{
    VideoUi *ui = lv_event_get_user_data(event);
    const int32_t value = lv_slider_get_value(lv_event_get_target(event));

    actions_keep_alive(ui);
    ui->volume = (uint8_t)value;
    if (ui->device.apply_volume != NULL) {
        (void)ui->device.apply_volume(ui->device.ctx, ui->volume);
    }
}

static void on_brightness_changed(lv_event_t *event)
{
    VideoUi *ui = lv_event_get_user_data(event);
    const int32_t value = lv_slider_get_value(lv_event_get_target(event));

    actions_keep_alive(ui);
    if (ui->device.apply_brightness != NULL) {
        (void)ui->device.apply_brightness(ui->device.ctx, (uint8_t)value);
    }
}

/*
 * Store both, once, on release.
 *
 * Read back from the widgets rather than tracked, so whichever slider
 * was let go the other one's value is still right. There is one volume
 * and one brightness on this device (design 11); these are the same
 * settings DeviceSettingsUi shows, not a per-session copy of them.
 */
static void on_slider_released(lv_event_t *event)
{
    VideoUi *ui = lv_event_get_user_data(event);

    /* Letting go is the start of the idle time, not part of it. */
    actions_keep_alive(ui);

    if (ui->device.load_config == NULL || ui->device.save_config == NULL) {
        return;
    }

    NN20ClockConfig config;
    if (ui->device.load_config(ui->device.ctx, &config) != ESP_OK) {
        return;
    }

    /*
     * Volume is stored here; brightness deliberately is not.
     *
     * Brightness follows a day/night schedule, and there is no single
     * field this slider could write that would still be true an hour
     * later - writing the day value at midnight would quietly change
     * what the clock looks like tomorrow afternoon. So dimming during a
     * film dims the film, and the next minute tick hands the panel back
     * to the schedule.
     */
    if (ui->volume_slider != NULL) {
        config.volume = (uint8_t)lv_slider_get_value(ui->volume_slider);
    }

    if (ui->device.save_config(ui->device.ctx, &config) != ESP_OK) {
        ESP_LOGW(TAG, "could not store the slider settings");
    }
}

/*
 * A labelled slider. The glyph rather than a word: this window is read
 * in the dark by somebody half awake, and a speaker and a sun are
 * quicker than "Volume" and "Brightness".
 */
static lv_obj_t *make_slider(lv_obj_t *parent, const char *glyph,
                             int32_t minimum, int32_t value,
                             lv_event_cb_t on_change, VideoUi *ui)
{
    lv_obj_t *row = lv_obj_create(parent);
    if (row == NULL) {
        return NULL;
    }
    lv_obj_set_size(row, LV_PCT(90), SLIDER_ROW_HEIGHT);
    lv_obj_set_style_flex_grow(row, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *icon = lv_label_create(row);
    lv_label_set_text(icon, glyph);
    lv_obj_set_style_text_color(icon, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *slider = lv_slider_create(row);
    if (slider == NULL) {
        return NULL;
    }
    /* Tall enough to hit without aiming, which is the whole point of
     * this screen. */
    lv_obj_set_size(slider, LV_PCT(80), SLIDER_HEIGHT);
    lv_obj_align(slider, LV_ALIGN_RIGHT_MID, 0, 0);
    /*
     * A floor, because the two sliders are not the same shape. Volume
     * starts at zero - silence is a thing somebody wants. Brightness
     * does not: the display clamps anything below its minimum up to it,
     * so a slider that went lower would sit at 5% while the panel sat
     * at 25%, and the control would be lying about what it did.
     */
    lv_slider_set_range(slider, minimum, 100);
    lv_slider_set_value(slider, value, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, COLOR_DIGITS, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, COLOR_TEXT, LV_PART_KNOB);

    lv_obj_add_event_cb(slider, on_change, LV_EVENT_VALUE_CHANGED, ui);
    lv_obj_add_event_cb(slider, on_slider_released, LV_EVENT_RELEASED, ui);
    return slider;
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *text,
                             lv_color_t colour, lv_event_cb_t handler,
                             VideoUi *ui)
{
    lv_obj_t *button = lv_button_create(parent);
    if (button == NULL) {
        return NULL;
    }

    lv_obj_set_size(button, LV_PCT(BUTTON_WIDTH_PCT), BUTTON_HEIGHT);
    /* Fixed height in a flex column: a percentage would be measured
     * against the column and squeezed by everything else in it. */
    lv_obj_set_style_flex_grow(button, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(button, colour, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(button, 24, LV_PART_MAIN);
    lv_obj_add_event_cb(button, handler, LV_EVENT_CLICKED, ui);

    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_center(label);

    return button;
}

/* ------------------------------------------------------------ views -- */

static esp_err_t build_playback(VideoUi *ui)
{
    ui->playback = lv_obj_create(ui->root);
    if (ui->playback == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_set_size(ui->playback, LV_PCT(100), LV_PCT(100));
    lv_obj_center(ui->playback);
    lv_obj_set_style_bg_color(ui->playback, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui->playback, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(ui->playback, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(ui->playback, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ui->playback, 0, LV_PART_MAIN);
    lv_obj_remove_flag(ui->playback, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ui->playback, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ui->playback, on_playback_clicked, LV_EVENT_CLICKED,
                        ui);

    /*
     * What is drawn here is only ever seen when there is no video: with
     * a file playing, the player owns the panel and covers all of it.
     * It is built either way, because whether the file is playable is
     * not known until the player has opened it.
     */
    const bool alarm = (ui->origin == NN20CLOCK_VIDEO_UI_ALARM);

    /*
     * Everything below goes in one container so it can be hidden as a
     * group the moment there is a picture to show instead.
     */
    ui->tone_face = lv_obj_create(ui->playback);
    if (ui->tone_face == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_set_size(ui->tone_face, LV_PCT(100), LV_PCT(100));
    lv_obj_center(ui->tone_face);
    lv_obj_set_style_bg_color(ui->tone_face, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui->tone_face, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(ui->tone_face, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(ui->tone_face, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ui->tone_face, 0, LV_PART_MAIN);
    lv_obj_remove_flag(ui->tone_face, LV_OBJ_FLAG_SCROLLABLE);
    /* Touches belong to the playback view underneath it. */
    lv_obj_add_flag(ui->tone_face, LV_OBJ_FLAG_EVENT_BUBBLE);
    /* Hidden until something says there is no picture - see
     * sync_tone_face(). Starting hidden is what stops it flashing up
     * for the moment before the first frame arrives. */
    lv_obj_add_flag(ui->tone_face, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *glyph = lv_label_create(ui->tone_face);
    lv_label_set_text(glyph, alarm ? LV_SYMBOL_BELL : LV_SYMBOL_PLAY);
    lv_obj_set_style_text_color(glyph, COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_set_style_text_font(glyph, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_align(glyph, LV_ALIGN_CENTER, 0, -180);

    ui->time_label = lv_label_create(ui->tone_face);
    lv_label_set_text(ui->time_label, "--:--");
    lv_obj_set_style_text_color(ui->time_label, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(ui->time_label, &lv_font_montserrat_48,
                               LV_PART_MAIN);
    lv_obj_align(ui->time_label, LV_ALIGN_CENTER, 0, -60);

    lv_obj_t *hint = lv_label_create(ui->tone_face);
    /* An alarm with no video keeps ringing and the hint is a way out.
     * Manual playback with no video is a file that would not play, and
     * the screen is about to close by itself - saying "touch the
     * screen" would be an instruction to do nothing. */
    lv_label_set_text(hint, alarm ? "touch the screen" : "");
    lv_obj_set_style_text_color(hint, COLOR_DIM, LV_PART_MAIN);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -60);

    return ESP_OK;
}

static esp_err_t build_actions(VideoUi *ui)
{
    ui->actions = lv_obj_create(ui->root);
    if (ui->actions == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_set_size(ui->actions, LV_PCT(100), LV_PCT(100));
    lv_obj_center(ui->actions);
    /* Dimmed, not opaque: the alarm is still playing behind this. */
    lv_obj_set_style_bg_color(ui->actions, COLOR_SCRIM, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui->actions, SCRIM_OPACITY, LV_PART_MAIN);
    lv_obj_set_style_border_width(ui->actions, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(ui->actions, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ui->actions, 0, LV_PART_MAIN);
    lv_obj_remove_flag(ui->actions, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ui->actions, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ui->actions, on_scrim_clicked, LV_EVENT_CLICKED, ui);
    /* A press anywhere in the window, held or not, keeps it up. */
    lv_obj_add_event_cb(ui->actions, on_actions_pressing, LV_EVENT_PRESSING,
                        ui);

    /*
     * A flex column, top to bottom: the time, then the origin's
     * buttons, then the sliders.
     *
     * Laid out rather than aligned because the two origins have a
     * different number of buttons - one for media, two for an alarm -
     * and absolute alignment would leave a hole in one of them.
     */
    lv_obj_set_flex_flow(ui->actions, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(ui->actions, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(ui->actions, ACTIONS_PAD, LV_PART_MAIN);
    lv_obj_set_style_pad_row(ui->actions, ACTIONS_PAD, LV_PART_MAIN);

    /*
     * The time first, in the clock face's blue.
     *
     * Reaching for a bedside clock at 3am, the question is usually not
     * "snooze or off" but "what time is it" - so it is the largest
     * thing here that is not a button. The ringing face behind is
     * hidden while this is up, so there is exactly one time on screen.
     */
    ui->actions_time_label = lv_label_create(ui->actions);
    lv_label_set_text(ui->actions_time_label, ui->shown_time[0] != '\0'
                                                  ? ui->shown_time
                                                  : "--:--");
    lv_obj_set_style_text_color(ui->actions_time_label, COLOR_DIGITS,
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(ui->actions_time_label,
                               &nn20clock_font_clock_140, LV_PART_MAIN);

    if (ui->origin == NN20CLOCK_VIDEO_UI_ALARM) {
        lv_obj_t *snooze = make_button(ui->actions, "SNOOZE", COLOR_SNOOZE,
                                       on_snooze_clicked, ui);
        lv_obj_t *off = make_button(ui->actions, "ALARM OFF", COLOR_OFF,
                                    on_off_clicked, ui);
        if (snooze == NULL || off == NULL) {
            return ESP_ERR_NO_MEM;
        }
    } else {
        lv_obj_t *stop = make_button(ui->actions, "STOP", COLOR_STOP,
                                     on_stop_clicked, ui);
        if (stop == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    /*
     * The sliders, below the buttons, on both origins (design 11).
     *
     * Brightness starts from what is stored rather than from the panel:
     * the player owns the panel while video is up, but the backlight is
     * a separate thing and dimming it works either way.
     */
    /*
     * Where the panel actually is, not what is stored: with a schedule
     * the stored day value is wrong at 3am and the night value wrong at
     * noon, and a slider that jumps the moment you touch it is worse
     * than no slider.
     */
    int32_t brightness = 50;
    if (ui->device.current_brightness != NULL) {
        const uint8_t now = ui->device.current_brightness(ui->device.ctx);
        if (now > 0u) {
            brightness = (int32_t)now;
        }
    }

    ui->volume_slider = make_slider(ui->actions, LV_SYMBOL_VOLUME_MAX, 0,
                                    (int32_t)ui->volume, on_volume_changed,
                                    ui);
    ui->brightness_slider = make_slider(
        ui->actions, LV_SYMBOL_EYE_OPEN,
        NN20CLOCK_DISPLAY_MIN_ON_BRIGHTNESS, brightness,
        on_brightness_changed, ui);
    if (ui->volume_slider == NULL || ui->brightness_slider == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* A press on a button is a press on a button, not on the backdrop
     * behind it - LVGL bubbles only when asked, and it is not asked. */
    lv_obj_add_flag(ui->actions, LV_OBJ_FLAG_HIDDEN);
    return ESP_OK;
}

/* ---------------------------------------------------------- starting -- */

/*
 * Ask the player to start. Returns whether it did.
 *
 * The request is where the two origins differ, and it is built here
 * rather than in the constructor so that a retry re-states it - the
 * player copies what it is given and forgets it afterwards.
 */
/*
 * How much of the sleep timer is left, in milliseconds.
 *
 * The timer is a DURATION for the whole session, not a cut-off for one
 * clip. That distinction only started to matter when <random> reached
 * manual playback: each clip is a fresh request to the player, and
 * handing it the full `sleep_ms` every time would restart the half hour
 * at every clip and never stop.
 *
 * Zero out means the timer has run out; zero `sleep_ms` means there
 * never was one, and the two are told apart by the caller rather than
 * here. lv_tick_elaps() because the tick wraps, at which point
 * subtracting two of them by hand gives a very long evening.
 */
static uint32_t remaining_ms(const VideoUi *ui)
{
    if (ui->sleep_ms == 0u) {
        return 0u;
    }

    const uint32_t elapsed = lv_tick_elaps(ui->started_at);
    return (elapsed >= ui->sleep_ms) ? 0u : (ui->sleep_ms - elapsed);
}

static bool start_playback(VideoUi *ui)
{
    const bool alarm = (ui->origin == NN20CLOCK_VIDEO_UI_ALARM);

    /* The session's clock starts with its first clip, not with each
     * one. */
    if (!ui->began) {
        ui->started_at = lv_tick_get();
    }

    const NN20ClockPlayerRequest request = {
        .path = ui->media_path,
        .volume = ui->volume,
        /* Only an alarm fades in, and only into its first film; media
         * the user chose to watch starts at the volume they set. */
        .fade_in_ms = (alarm && !ui->began) ? ALARM_FADE_MS : 0u,
        .fade_from_volume = ALARM_FADE_FROM_VOLUME,
        /*
         * An alarm goes round again until somebody stops it.
         *
         * Manual playback is the sleep timer's: at `all` it plays the
         * clip once and stops, and with a timer set it keeps playing
         * until the time is up - looping the clip, or drawing the next
         * one when <random> gave us somewhere to ask. Somebody who sets
         * half an hour wants half an hour, not "half an hour or until
         * this ten-minute clip ends, whichever comes first".
         *
         * Which of the two is the `next_media` test: looping in the
         * player would never give this screen the end it needs to hear
         * in order to draw the next clip.
         */
        .loop = (ui->next_media == NULL) &&
                (alarm || ui->sleep_ms != 0u),
        .stop_after_ms = alarm ? 0u : remaining_ms(ui),
        .tone_fallback = alarm,
        .remember = alarm,
    };

    const esp_err_t err = nn20clock_player_play(ui->player, &request);
    if (err == ESP_OK) {
        ui->began = true;
        return true;
    }

    ESP_LOGW(TAG, "the player would not start (0x%x)", (unsigned)err);
    return false;
}

/* Runs on the UiWorker, like every LVGL timer callback. */
static void on_start_retry(lv_timer_t *timer)
{
    VideoUi *ui = lv_timer_get_user_data(timer);

    if (start_playback(ui)) {
        /*
         * Deleted from inside its own callback, which LVGL supports and
         * which is the whole job of this timer. Deliberately not a
         * repeat count - see the note on actions_timer.
         */
        lv_timer_delete(timer);
        ui->start_timer = NULL;
        return;
    }

    if (ui->origin == NN20CLOCK_VIDEO_UI_ALARM) {
        return;   /* design 13: keep asking, there is no silent option */
    }
    if (lv_tick_elaps(ui->start_asked_at) < (uint32_t)START_GIVE_UP_MS) {
        return;
    }

    /*
     * Nothing is going to happen, and the player will not report an end
     * it never began - so this screen asks to close rather than sit
     * black until it is touched.
     */
    ESP_LOGE(TAG, "giving up on starting playback");
    lv_timer_delete(timer);
    ui->start_timer = NULL;
    send(ui, NN20CLOCK_UI_COMMAND_CLOSE_MEDIA_PLAYBACK);
}

/* Ask for a timer to keep trying with, if there is not one already. */
static void retry_start_later(VideoUi *ui)
{
    if (ui->start_timer != NULL) {
        return;
    }
    ui->start_asked_at = lv_tick_get();
    ui->start_timer = lv_timer_create(on_start_retry, START_RETRY_MS, ui);
    if (ui->start_timer == NULL) {
        ESP_LOGE(TAG, "no timer to retry the start with");
    }
}

/*
 * A <random> playback's clip has ended: draw another and play it.
 *
 * The draw is the caller's - all this screen knows is that it was given
 * somewhere to ask. A refusal is not a failure: it means the card has
 * nothing else to offer, and the film that just ended goes round again
 * rather than the alarm falling silent, which is design 13's rule and
 * the same thing an ordinary alarm does by looping in the player.
 *
 * Starting can still be refused - the player may not have finished
 * winding the last pass down - so a failure goes to the same retry
 * timer the first start uses, which for an alarm never gives up.
 */
static void play_next(VideoUi *ui)
{
    char next[NN20CLOCK_MEDIA_PATH_MAX];

    ui->changing_film = true;
    ui->changing_film_at = lv_tick_get();

    if (ui->next_media != NULL &&
        ui->next_media(ui->next_media_ctx, next, sizeof(next)) &&
        next[0] != '\0') {
        (void)snprintf(ui->media_path, sizeof(ui->media_path), "%s", next);
        ESP_LOGI(TAG, "the film ended; next is %s", ui->media_path);
    } else {
        ESP_LOGW(TAG, "the film ended and there is nothing else; again");
    }

    if (!start_playback(ui)) {
        retry_start_later(ui);
    }
}

/* ------------------------------------------------------------ hooks -- */

static esp_err_t video_ui_show(NN20ClockUiBase *base)
{
    VideoUi *ui = (VideoUi *)base;

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

    esp_err_t err = build_playback(ui);
    if (err == ESP_OK) {
        err = build_actions(ui);
    }
    if (err != ESP_OK) {
        return err;
    }
    ui->showing_actions = false;
    /*
     * The panel is this screen's to lend out, so it says so up front
     * rather than leaving the player to assume it every time it opens
     * a file. That assumption was the bug: a <random> run moving on to
     * its next film took the panel back from under the buttons, which
     * then could not be dismissed or used, and the film played out over
     * a screen that answered nothing. From here the wish changes in
     * exactly one place - show_actions().
     */
    (void)nn20clock_player_set_video_visible(ui->player, true);

    ui->actions_timer = lv_timer_create(on_actions_timeout,
                                        ACTIONS_VISIBLE_MS, ui);
    if (ui->actions_timer != NULL) {
        lv_timer_pause(ui->actions_timer);
    }

    ui->face_timer = lv_timer_create(on_face_tick, FACE_POLL_MS, ui);

    /*
     * Registered before playback starts, so a touch during the first
     * frame is not lost. Cleared in destroy(), on this same thread,
     * which is what makes the player's callback safe.
     */
    nn20clock_player_set_touch_handler(ui->player, on_player_touch, ui);

    /*
     * An end to hear about is manual playback's, and a <random> alarm's
     * between films - the one alarm that asked the player for a single
     * pass. Registering it for any other alarm would be dead code with
     * a live side effect: such an alarm never finishes on its own, and
     * if one somehow did, on_player_done() would take the media branch
     * and returning to the clock face would dismiss it - including
     * deleting a one-off alarm that had not actually rung out.
     *
     * Manual playback always wants it, whatever the sleep timer says:
     * the player reports the timer running out through this same
     * handler, not only the end of a clip.
     */
    if (ui->origin != NN20CLOCK_VIDEO_UI_ALARM || ui->next_media != NULL) {
        nn20clock_player_set_done_handler(ui->player, on_player_done, ui);
    }

    if (!start_playback(ui)) {
        /* Busy with what this screen replaced. Ask again shortly - see
         * START_RETRY_MS. */
        retry_start_later(ui);
    }
    return ESP_OK;
}

static esp_err_t video_ui_hide(NN20ClockUiBase *base)
{
    VideoUi *ui = (VideoUi *)base;

    /* Hidden means the alarm is over as far as this screen is
     * concerned; whatever is playing stops here rather than at
     * destroy(), so a screen kept around does not keep ringing. */
    (void)nn20clock_player_halt(ui->player);

    if (ui->root != NULL) {
        lv_obj_add_flag(ui->root, LV_OBJ_FLAG_HIDDEN);
    }
    return ESP_OK;
}

/*
 * The clock behind the alarm. Only visible in the no-video case, but
 * kept up to date regardless: the file may fail at any point and leave
 * this showing.
 */
static esp_err_t video_ui_timer_event(NN20ClockUiBase *base,
                                      const NN20ClockTimerEvent *event)
{
    VideoUi *ui = (VideoUi *)base;

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

    char text[NN20CLOCK_HHMM_SIZE];
    (void)nn20clock_timefmt_hhmm(&now, text, sizeof(text));
    if (strcmp(text, ui->shown_time) != 0) {
        /* Both views carry the time; whichever is up shows the same
         * minute. */
        lv_label_set_text(ui->time_label, text);
        if (ui->actions_time_label != NULL) {
            lv_label_set_text(ui->actions_time_label, text);
        }
        snprintf(ui->shown_time, sizeof(ui->shown_time), "%s", text);
    }
    return ESP_OK;
}

static void video_ui_destroy(NN20ClockUiBase *base)
{
    VideoUi *ui = (VideoUi *)base;

    /*
     * The handler before the widgets: after this line the player has no
     * way back into a screen that is about to stop existing. Both the
     * clearing and the calling happen on the UiWorker, so there is no
     * window between them.
     */
    nn20clock_player_set_touch_handler(ui->player, NULL, NULL);
    nn20clock_player_set_done_handler(ui->player, NULL, NULL);
    (void)nn20clock_player_halt(ui->player);

    /* Before the widgets it hides: a timer firing after this screen is
     * freed would work through a released pointer. */
    if (ui->actions_timer != NULL) {
        lv_timer_delete(ui->actions_timer);
        ui->actions_timer = NULL;
    }
    if (ui->start_timer != NULL) {
        lv_timer_delete(ui->start_timer);
        ui->start_timer = NULL;
    }
    if (ui->face_timer != NULL) {
        lv_timer_delete(ui->face_timer);
        ui->face_timer = NULL;
    }
    if (ui->root != NULL) {
        /* Deletes the two views and their buttons with it. */
        lv_obj_delete(ui->root);
        ui->root = NULL;
        ui->playback = NULL;
        ui->actions = NULL;
        ui->tone_face = NULL;
        ui->time_label = NULL;
        ui->actions_time_label = NULL;
    }

    nn20clock_ui_base_deinit(&ui->super);
    free(ui);
}

static const NN20ClockUiVTable VIDEO_UI_VTABLE = {
    .show = video_ui_show,
    .hide = video_ui_hide,
    .handle_touch = NULL,   /* LVGL and the player both report directly */
    .handle_timer_event = video_ui_timer_event,
    .destroy = video_ui_destroy,
};

/* ------------------------------------------------------------- ctor -- */

NN20ClockUiBase *nn20clock_video_ui_ctor(nn20_worker_ctx *ui_worker,
                                         NN20ClockManager *manager,
                                         NN20ClockUiCommandFn on_command,
                                         NN20ClockPlayer *player,
                                         NN20ClockVideoUiOrigin origin,
                                         const char *media_path,
                                         uint8_t volume,
                                         uint32_t sleep_ms,
                                         NN20ClockVideoUiNextMediaFn next_media,
                                         void *next_media_ctx,
                                         NN20ClockDeviceService device)
{
    if (ui_worker == NULL || player == NULL) {
        ESP_LOGE(TAG, "a playback screen needs the UiWorker and a player");
        return NULL;
    }

    VideoUi *ui = calloc(1, sizeof(*ui));
    if (ui == NULL) {
        ESP_LOGE(TAG, "out of memory");
        return NULL;
    }

    const NN20ClockUiBaseConfig config = {
        .vtable = &VIDEO_UI_VTABLE,
        .name = "VideoPlayerUi",
        .worker = ui_worker,
        .manager = manager,
        .on_command = on_command,
    };
    if (nn20clock_ui_base_init(&ui->super, &config) != ESP_OK) {
        free(ui);
        return NULL;
    }

    ui->player = player;
    ui->origin = origin;
    /* Only an alarm has an endless run to fill, so only an alarm is
     * ever asked for another film - see start_playback(). */
    /*
     * Kept for both origins. It used to be dropped for anything that
     * was not an alarm, from when <random> was an alarm idea only - so
     * manual <random> was handed a draw function that this screen threw
     * away, and played exactly one clip.
     */
    ui->next_media = next_media;
    ui->next_media_ctx = next_media_ctx;
    ui->volume = (volume > 100u) ? 100u : volume;
    ui->sleep_ms = sleep_ms;
    ui->device = device;
    if (media_path != NULL) {
        (void)snprintf(ui->media_path, sizeof(ui->media_path), "%s",
                       media_path);
    }

    /* No LVGL call has happened here, and none may: this runs on the
     * manager's worker. */
    return &ui->super;
}
