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
 * nn20clock_app.c - build, start, supervise, and tear down the services.
 *
 * The interesting part of this file is the order things happen in. Every
 * component here borrows a worker from NN20ClockWorkers and borrows the
 * services below it, so the set has to be built bottom-up and destroyed
 * top-down; get that wrong and a callback runs against freed memory.
 * The two functions that matter are the ctor and the dtor, and they are
 * mirror images.
 */
#include "nn20clock_app.h"

#include <inttypes.h>
#include <sys/time.h>
#include <time.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#if defined(ESP_PLATFORM)
#include "esp_random.h"

#include "nn20clock_alarm_ui.h"
#include "nn20clock_device_ui.h"
#include "nn20clock_media.h"
#include "nn20clock_media_mgmt_ui.h"
#include "nn20clock_media_ui.h"
#include "nn20clock_ota.h"
#include "nn20clock_time_ui.h"
#include "nn20clock_video_ui.h"

/* Kconfig default, so the file still compiles if the menu is absent. */
#ifndef CONFIG_NN20CLOCK_TIMEZONE
#define CONFIG_NN20CLOCK_TIMEZONE "CET-1CEST,M3.5.0,M10.5.0/3"
#endif
#endif

static const char *TAG = "NN20CLOCK_APP";

struct NN20ClockApp {
    /* Built in this order, destroyed in the reverse. */
    NN20ClockWorkers *workers;
    NN20ClockStorage *storage;
#if defined(ESP_PLATFORM)
    NN20ClockDisplay *display;
    NN20ClockSd *sd;
    /* After the display: the codec borrows the I2C bus the display
     * owns, so it must be gone before the display is (design 2). */
    NN20ClockAudio *audio;
    NN20ClockPlayer *player;
#endif
    NN20ClockTimer *timer;
    NN20ClockManager *manager;
#if defined(ESP_PLATFORM)
    NN20ClockNet *net;
    NN20ClockFtp *ftp;
    NN20ClockOta *ota;

    /*
     * The day/night brightness schedule, and the state that goes with
     * running it. All of this is touched only from the CoreWorker - the
     * minute tick that evaluates it posts there, and so does every
     * screen that hands the panel back - so none of it is atomic and
     * none of it wants a lock.
     *
     * Nothing overrides the schedule. An alarm used to: it pinned the
     * day value from the moment it rang, on the grounds that being woken
     * is the day beginning. It does not any more - the schedule holds
     * through the clock face, a film and a ringing alarm alike, because
     * an alarm at 4am on a clock set to dim at night should ring at the
     * brightness that was asked for at 4am.
     *
     * The one thing a film adds is the schedule's own playback minimum,
     * which can only raise the backlight: see `playing` below.
     */
    NN20ClockTimerSubscriber brightness_subscriber;
    NN20ClockBrightnessSchedule schedule;
    /*
     * The last value the schedule itself asked for - not necessarily
     * what the panel is at. A playback slider can move the backlight
     * underneath this, and the difference is deliberate: it is what
     * stops the tick fighting a finger a second after it lets go.
     */
    uint8_t scheduled_brightness;
    /*
     * Whether a film is on the panel - an alarm ringing, or something
     * chosen from the playback list - and so whether the schedule's
     * playback minimum applies. Set when playback begins and cleared
     * when its state is left, both by posts to this worker; see
     * brightness_playback_private().
     */
    bool playing;

    /*
     * The film a <random> alarm is playing in the ringing it is in.
     *
     * Rolled when the alarm goes off, and rolled again whenever that
     * film runs out - see next_random_media(). <random> means a random
     * film each time, not one random film for the whole morning.
     *
     * Held rather than re-rolled across a snooze, though, because a
     * snooze is the same ringing continued, not a new one: the player
     * remembers where it was in the file and puts the frame on the
     * clock face while it waits, and coming back nine minutes later
     * with a different film would throw both away. So it survives
     * until the alarm is dismissed - see on_state_change(), which is
     * where the difference between a snooze and a dismissal is
     * visible.
     *
     * RAM only, like the snooze it follows: a reboot forgets both, and
     * the alarm rolls again. Nothing here is ever written to a record -
     * the alarm stores "random", never the outcome of one.
     *
     * Written by the ClockManagerWorker, which is where the first roll
     * and the clearing happen, and by the UiWorker, which is where a
     * film ending rolls the next one. The two cannot overlap: the
     * ClockManagerWorker touches this while it builds or tears down the
     * ringing screen, and the UiWorker only while that screen is up and
     * playing. It is the rule nn20clock_player's thumbnail is read
     * under - written while an alarm rings, read when none is. No
     * atomics, no lock.
     */
    uint32_t random_alarm_id;
    /*
     * The full relative path of that film - "wake.avi",
     * "morning/wake.avi" - and not just its name. A name would not say
     * which folder it came from, and two folders may hold clips called the
     * same thing; a path makes "the same film across a snooze" a single
     * string comparison and keeps the player's position key meaningful.
     */
    char random_choice[NN20CLOCK_MEDIA_PATH_MAX];
    /*
     * And the folder it was drawn from - the alarm's media_set_id, copied
     * when the alarm went off.
     *
     * Held rather than looked up again because next_random_media() runs
     * on the UiWorker, where the alarm record is not to hand, and
     * because deriving it from random_choice would be wrong the moment
     * the draw fails: an alarm that fell back to the tone still has a
     * folder, and an alarm that has just been dismissed must not have one.
     */
    char random_folder[NN20CLOCK_MEDIA_PATH_MAX];

    /*
     * The same two, for <random> chosen by hand on the playback screen
     * rather than by an alarm - an evening's films rather than a
     * morning's.
     *
     * Separate fields rather than the pair above, deliberately. Those
     * belong to a ringing alarm and are written by the
     * ClockManagerWorker as well as the UiWorker, under the rule that
     * the two cannot overlap because one builds the ringing screen and
     * the other only runs while it is up. Manual playback is purely the
     * UiWorker's - it is started from a screen and its next-clip
     * callback runs on the same worker - so borrowing the alarm's
     * fields would put a third writer into an argument that was settled
     * by there being exactly two.
     *
     * RAM only. Leaving the screen or a reboot forgets both, which is
     * all "play something at random until I fall asleep" needs.
     */
    char manual_folder[NN20CLOCK_MEDIA_PATH_MAX];
    char manual_choice[NN20CLOCK_MEDIA_PATH_MAX];
#endif

    /* Written only on the CoreWorker, read from any thread. Atomic so
     * that read is a defined value rather than a data race - not a lock,
     * and not how anything here synchronizes. */
    atomic_bool running;
};

static esp_err_t post_result(int rc)
{
    if (rc == 0) {
        return ESP_OK;
    }
    return (rc == 1) ? ESP_ERR_TIMEOUT : ESP_ERR_INVALID_STATE;
}

#if defined(ESP_PLATFORM)
/*
 * Which media a ringing alarm plays (design 13).
 *
 * The chain is short on purpose: the alarm's own file, and otherwise
 * the built-in tone. An empty path is not a failure to report here -
 * the player treats it, and a file that turns out to be missing or
 * unplayable, the same way, so there is exactly one place that decides
 * what silence sounds like.
 *
 * Runs on the ClockManagerWorker, whose stack is 4 KB: one alarm record
 * (a few hundred bytes), never the whole list.
 */
/*
 * The device's own volume setting, from storage.
 *
 * Deliberately NOT nn20clock_audio_volume(), which is the gain the
 * codec is at right now rather than a setting: every alarm calls
 * set_volume() with its own volume and nothing puts the device's back,
 * so after one alarm has rung the codec is sitting at that alarm's
 * volume until the next reboot. Reading it as if it were the
 * preference made manual playback inherit whichever alarm rang last -
 * quiet before any had, and someone else's volume afterwards.
 *
 * Storage is the only thing that knows the setting, so this asks it.
 * The audio component's current gain is the fallback for a card or an
 * NVS that will not answer, which is at least a number that once came
 * from the setting.
 *
 * Blocks on the StorageWorker. Safe from the ClockManagerWorker and
 * from the UiWorker - neither is ever waited on by storage - and the
 * config is a couple of hundred bytes, so it is fine on either stack.
 */
static uint8_t device_volume(NN20ClockApp *pthis)
{
    NN20ClockConfig config;

    if (pthis->storage == NULL ||
        nn20clock_storage_load_config(pthis->storage, &config) != ESP_OK) {
        /*
         * The compiled-in default, not the codec's current gain.
         *
         * The audio component starts at zero and is only lifted off it
         * by the config being applied - so on the one boot where
         * storage will not answer, reading the gain back would return
         * zero and design 13's "never silence" would fail at exactly
         * the moment it matters. A default that is merely wrong is
         * recoverable; a silent alarm is not.
         */
        ESP_LOGW(TAG, "no stored volume; using the default");
        if (nn20clock_storage_default_config(&config) != ESP_OK) {
            return NN20CLOCK_AUDIO_FALLBACK_VOLUME;
        }
    }
    return (config.volume > 100u) ? 100u : config.volume;
}

/*
 * Pick one film out of one folder at random (design 13's random set).
 *
 * Uniform over what is playable in `folder_path`, and over that only:
 * files the media layer does not recognise are not something to wake up
 * to, and the built-in tone is not in the pool at all - it is the
 * fallback for media that cannot be played, and an alarm set to choose
 * between films should not sometimes beep instead. Reaching the tone
 * from here means the folder had nothing, which is the same fallback a
 * named file that has gone missing takes.
 *
 * It does NOT recurse. A folder is a choice the user made in the picker,
 * and "morning" means the clips in "morning" - quietly including
 * "morning/kids" would make the one control the feature has mean
 * something the user did not ask for. Child folders in the listing are
 * simply not in the pool.
 *
 * `exclude` is a film not to pick, or NULL for the whole folder: it is the
 * one that just finished, and leaving it in the pool would let a fair
 * draw hand back the same film twice in a row - which is the one
 * outcome somebody who asked for <random> reads as a bug rather than as
 * chance. It is dropped only while something else remains, so a folder
 * with one film in it still plays that film. A relative path, like what
 * this writes out, so two folders holding a clip of the same name do not
 * exclude each other's.
 *
 * Before any of that, the openers. A film whose name starts with a
 * number - "1INTRO.AVI", "15SUMMER.AVI" - is not in the draw at all
 * until the numbered ones have played, in their numbers' order, from
 * the top. That is the whole of the feature: a folder can open with a
 * title card, or with three parts that only mean anything in sequence,
 * and shuffle everything after them.
 *
 * `exclude` carries the position. The run is a pure function of the
 * film that just played - see nn20clock_media_list_next_opener() - so a
 * <random> that starts fresh starts at the first opener, and one that
 * has drawn an unnumbered film has left the run behind for good. That
 * is deliberate: the openers are an opening, and a folder that got back
 * to them an hour later would be playing its title card in the middle
 * of the evening.
 *
 * A folder of nothing but openers is a playlist: the run starts over at
 * the first one rather than falling through to a draw, because a folder
 * that says "1, 2, 3" has already said what order it wants and a
 * shuffle of it would be the feature answering its own question.
 *
 * Everything else draws, over the films with NO number - "randomize the
 * others" - except when the film that just played is the only one of
 * those. Then the draw is over the whole folder, so a folder of
 * "1INTRO.AVI" and one other film alternates rather than playing that
 * other film for the rest of the night.
 *
 * esp_random() rather than rand(): it needs no seeding, which matters
 * on a device whose first alarm can be its first minute of uptime. A
 * seeded PRNG would hand out the same film every morning after a power
 * cut. The modulo bias over at most 64 files is far below anything a
 * person could notice in a lifetime of mornings.
 *
 * The list is over 30 KB and this runs on the ClockManagerWorker, whose
 * stack is 4 - so it goes on the heap and is freed before returning,
 * exactly like the alarm editor's copy.
 */
static bool choose_random_media(NN20ClockApp *pthis, const char *folder_path,
                                char *out_relative_path, size_t out_size,
                                const char *exclude_relative_path)
{
    if (pthis->sd == NULL || !nn20clock_sd_is_mounted(pthis->sd)) {
        return false;
    }

    NN20ClockMediaList *const list = calloc(1, sizeof(*list));
    if (list == NULL) {
        ESP_LOGE(TAG, "out of memory for the media list");
        return false;
    }

    bool chosen = false;
    if (nn20clock_sd_list_media(pthis->sd, folder_path, list) != ESP_OK) {
        ESP_LOGW(TAG, "could not read the folder to choose a film");
    } else {
        /*
         * Counted once, because the list holds folders and files that
         * cannot be played - an index into it is never an index into a
         * pool - and because the two decisions below both need the
         * counts.
         *
         * `unnumbered` is the pool the feature asks for: the films with
         * no number, which is what "the others" means. `films` is the
         * fallback for a folder that leaves that pool empty.
         */
        size_t films = 0;
        size_t unnumbered = 0;
        size_t excluded = 0;
        size_t unnumbered_excluded = 0;
        for (size_t i = 0; i < list->count; i++) {
            const NN20ClockMediaEntry *const entry = &list->entries[i];
            if (entry->type != NN20CLOCK_MEDIA_ENTRY_FILE ||
                entry->kind == NN20CLOCK_MEDIA_KIND_UNKNOWN) {
                continue;
            }
            const bool is_opener =
                nn20clock_media_name_number(entry->name, NULL);
            const bool is_excluded =
                (exclude_relative_path != NULL) &&
                (strcmp(entry->path, exclude_relative_path) == 0);

            films++;
            if (is_excluded) {
                excluded++;
            }
            if (!is_opener) {
                unnumbered++;
                if (is_excluded) {
                    unnumbered_excluded++;
                }
            }
        }

        /* The openers, while the run lasts. No draw and no exclusion:
         * the order is the point. */
        const NN20ClockMediaEntry *opener =
            nn20clock_media_list_next_opener(list, exclude_relative_path);
        if (opener == NULL && films > 0u && unnumbered == 0u) {
            /* Every film in the folder is an opener, so the folder is a
             * playlist and the run starts over rather than falling
             * through to a draw that would play it out of order. */
            opener = nn20clock_media_list_next_opener(list, NULL);
        }
        if (opener != NULL) {
            (void)snprintf(out_relative_path, out_size, "%s", opener->path);
            chosen = true;
        }

        if (!chosen) {
            /* The draw, over the films with no number - except when the
             * one that just played is the only one of those, which
             * would leave nothing to draw from. Then it is the whole
             * folder, so a folder of one opener and one other film
             * alternates instead of playing that other film all
             * night. */
            const bool others_only = (unnumbered > unnumbered_excluded);
            const size_t candidates = others_only ? unnumbered : films;
            const size_t leave_out =
                others_only ? unnumbered_excluded : excluded;

            /* The last film in the folder is not excluded from a pool
             * of one: an alarm that has run out of other things to play
             * plays the thing it has. */
            const bool skip = (leave_out > 0u) && (candidates > leave_out);
            const size_t pool = skip ? (candidates - leave_out) : candidates;

            if (pool > 0u) {
                size_t pick = (size_t)(esp_random() % (uint32_t)pool);
                for (size_t i = 0; i < list->count; i++) {
                    const NN20ClockMediaEntry *const entry = &list->entries[i];
                    if (entry->type != NN20CLOCK_MEDIA_ENTRY_FILE ||
                        entry->kind == NN20CLOCK_MEDIA_KIND_UNKNOWN) {
                        continue;
                    }
                    if (others_only &&
                        nn20clock_media_name_number(entry->name, NULL)) {
                        continue;
                    }
                    if (skip &&
                        strcmp(entry->path, exclude_relative_path) == 0) {
                        continue;
                    }
                    if (pick == 0u) {
                        (void)snprintf(out_relative_path, out_size, "%s",
                                       entry->path);
                        chosen = true;
                        break;
                    }
                    pick--;
                }
            }
        }
    }
    free(list);
    return chosen;
}

static void resolve_media(NN20ClockApp *pthis, char *out_path,
                          size_t out_size, uint8_t *out_volume,
                          bool *out_random)
{
    out_path[0] = '\0';
    *out_random = false;

    /*
     * One volume, device-wide. NN20ClockAlarmConfig still carries a
     * volume of its own - design 9's schema has it and design 11 lists
     * "Set volume" under the alarm editor - but nothing has ever been
     * able to change it, so every alarm holds the same invisible
     * default and overriding the device setting with it only ever meant
     * "ignore what the user chose".
     *
     * One speaker in one room wants one number. The alarm's field is
     * left in the record rather than migrated out of it; see the note
     * on NN20ClockAlarmConfig::volume.
     */
    *out_volume = device_volume(pthis);

    const uint32_t alarm_id = nn20clock_manager_ringing_alarm(pthis->manager);
    if (alarm_id == NN20CLOCK_ALARM_ID_NONE || pthis->storage == NULL) {
        return;
    }

    NN20ClockAlarmConfig alarm;
    if (nn20clock_storage_get_alarm(pthis->storage, alarm_id, &alarm)
        != ESP_OK) {
        ESP_LOGW(TAG, "alarm %u is ringing but cannot be read; using the "
                      "built-in tone", (unsigned)alarm_id);
        return;
    }

    if (alarm.media_mode == NN20CLOCK_ALARM_MEDIA_RANDOM_SET) {
        /*
         * Chosen here, when the alarm goes off, and not when it was
         * saved - design 13. The record says "random in this folder" and
         * never which film, so nothing about this outlives the ringing.
         * This is the first film only; the screen asks for the next one
         * when this runs out - see next_random_media().
         *
         * media_set_id is the folder, "" being the root folder - which is
         * also what an alarm saved before folders existed carries, so
         * those keep choosing from the root of the card exactly as they
         * did.
         *
         * Kept across a snooze: see NN20ClockApp::random_choice. The id
         * is what makes that safe - a held choice belongs to one alarm,
         * so a different alarm ringing rolls its own rather than
         * inheriting it.
         */
        if (pthis->random_alarm_id == alarm_id &&
            pthis->random_choice[0] != '\0') {
            ESP_LOGI(TAG, "alarm %u: still %s", (unsigned)alarm_id,
                     pthis->random_choice);
        } else if (choose_random_media(pthis, alarm.media_set_id,
                                       pthis->random_choice,
                                       sizeof(pthis->random_choice), NULL)) {
            pthis->random_alarm_id = alarm_id;
            (void)snprintf(pthis->random_folder,
                           sizeof(pthis->random_folder), "%s",
                           alarm.media_set_id);
            ESP_LOGI(TAG, "alarm %u: chose %s at random", (unsigned)alarm_id,
                     pthis->random_choice);
        } else {
            pthis->random_alarm_id = NN20CLOCK_ALARM_ID_NONE;
            pthis->random_choice[0] = '\0';
            pthis->random_folder[0] = '\0';
            ESP_LOGW(TAG, "alarm %u: nothing in that folder to choose from; "
                          "using the built-in tone", (unsigned)alarm_id);
            return;
        }

        /* Only once a film has actually been rolled: an alarm that fell
         * back to the tone has nothing to move on from, and the tone
         * rings until it is stopped. */
        *out_random = true;

        /* Into the record's own field, so the one path below resolves
         * and checks it - a path off the card's own listing is already
         * a safe relative path, and running it through the same gate
         * anyway costs nothing and leaves one path to audit. */
        (void)snprintf(alarm.media_path, sizeof(alarm.media_path), "%s",
                       pthis->random_choice);
    }

    if (alarm.media_path[0] == '\0') {
        return;
    }

    /*
     * The stored value is a relative path from the card's root -
     * "wake.avi", "morning/wake.avi" - and never an absolute one. The
     * same rule the FTP server enforces. Resolving it here rather than
     * storing a full path means the mount point is in one place, and a
     * path that tries to escape the card is refused rather than opened.
     */
    if (nn20clock_media_path(NN20CLOCK_SD_MOUNT_POINT, alarm.media_path,
                             out_path, out_size) != ESP_OK) {
        ESP_LOGW(TAG, "alarm %u names media that cannot be served; using "
                      "the built-in tone", (unsigned)alarm_id);
        out_path[0] = '\0';
    }
}

/*
 * The next film for a <random> alarm, asked for when the last one ran
 * out - see NN20ClockVideoUiNextMediaFn.
 *
 * A fresh draw over the SAME folder, minus the film that just played: the
 * point of <random> is a different film, and a fair draw that can
 * repeat reads as the alarm being stuck. The folder is the one the alarm
 * named, held in random_folder - drawing from the whole card here would
 * quietly widen a choice the user made in the picker, and would do it
 * only after the first film ended, which is the worst kind of
 * inconsistency to notice.
 *
 * Returning false leaves the screen to play what it has again, which is
 * what happens when the folder has one film in it or has gone away
 * entirely - an alarm has no silent option.
 *
 * Runs on the UiWorker, which is the one that hears the end of a film.
 * Reading the card there is what the media screens already do, and
 * random_choice is safe to write from here: see the note on the field.
 */
static bool next_random_media(void *ctx, char *out_path, size_t out_size)
{
    NN20ClockApp *pthis = ctx;

    char relative[NN20CLOCK_MEDIA_PATH_MAX];
    if (!choose_random_media(pthis, pthis->random_folder, relative,
                             sizeof(relative), pthis->random_choice)) {
        return false;
    }
    if (strcmp(relative, pthis->random_choice) == 0) {
        /* The only film in the folder. The screen replays what it has,
         * which costs it nothing - and saying so here keeps the player
         * from being stopped and started for the same file. */
        return false;
    }

    if (nn20clock_media_path(NN20CLOCK_SD_MOUNT_POINT, relative, out_path,
                             out_size) != ESP_OK) {
        ESP_LOGW(TAG, "%s cannot be served; playing the last film again",
                 relative);
        return false;
    }

    (void)snprintf(pthis->random_choice, sizeof(pthis->random_choice), "%s",
                   relative);
    return true;
}

/*
 * ------------------------------------------------------------------
 * The day/night brightness schedule
 * ------------------------------------------------------------------
 *
 * nn20clock_brightness owns the decision and is tested on the host;
 * everything here is wiring. The rule is one line: every minute, ask
 * the schedule what the brightness should be and apply it if it has
 * changed.
 *
 * All of this state is owned by the CoreWorker. The minute tick arrives
 * on the Timer's worker and the alarm transition on the manager's, and
 * both post here rather than touching it where they land - so the
 * schedule, the override and the applied value have exactly one owner
 * and need no atomics and no lock. Once a minute is not a rate that
 * makes a post worth avoiding.
 *
 * The CoreWorker is also where this is allowed to talk to the backlight
 * at all: a Timer subscriber may not touch LVGL, and while LEDC is not
 * LVGL, keeping the hardware call on the same worker as everything else
 * the app drives is one less rule to remember.
 */

/* Local minutes since midnight, from the wall clock the rest of the
 * device shows. */
static uint16_t local_minutes_now(void)
{
    const time_t now = time(NULL);
    struct tm local = {0};
    if (localtime_r(&now, &local) == NULL) {
        return 0u;
    }
    return (uint16_t)(local.tm_hour * 60 + local.tm_min);
}

/*
 * Re-read the schedule from storage.
 *
 * Every tick, rather than caching it at boot and refreshing on save.
 * The cached version is what the first attempt did and it was wrong in
 * a way that looked like the feature simply not working: the settings
 * screen wrote the new times to storage, nothing told this copy, and
 * the tick went on evaluating the boot schedule forever. Dragging a
 * slider still changed the panel - that path previews directly - so the
 * screen looked right while the clock never switched.
 *
 * It is cheap enough not to be worth the risk of the same bug again.
 * Storage keeps the config in RAM and only touches NVS on save, so this
 * is a worker round-trip and a struct copy, once a minute.
 */
static void refresh_schedule(NN20ClockApp *pthis)
{
    NN20ClockConfig config;
    if (nn20clock_storage_load_config(pthis->storage, &config) != ESP_OK) {
        return;   /* keep whatever was last known good */
    }

    pthis->schedule.day_percent = config.brightness_day;
    pthis->schedule.night_percent = config.brightness_night;
    pthis->schedule.day_start_minutes = config.day_start_minutes;
    pthis->schedule.night_start_minutes = config.night_start_minutes;
    pthis->schedule.playback_min_percent = config.brightness_playback_min;
    nn20clock_brightness_normalise(&pthis->schedule);
}

/*
 * Evaluate the schedule and drive the panel.
 *
 * Only writes when the answer changes, so the ordinary minute costs one
 * comparison rather than an LEDC reconfigure - and, more usefully, the
 * log line below marks the two moments a day the brightness actually
 * moves instead of appearing 1440 times.
 */
static void apply_scheduled_brightness(NN20ClockApp *pthis)
{
    const uint16_t now = local_minutes_now();

    refresh_schedule(pthis);

    const uint8_t percent =
        pthis->playing
            ? nn20clock_brightness_playback_percent(&pthis->schedule, now)
            : nn20clock_brightness_percent(&pthis->schedule, now);
    /*
     * Only when the schedule's own answer moves.
     *
     * The alternative - correcting the panel every minute to whatever
     * the schedule says - would make the brightness slider during a
     * film useless, snatching the panel back within sixty seconds of
     * letting go. So a preview stands, and the schedule takes the panel
     * at the next thing that genuinely changes its mind: a boundary, a
     * settings change, or the screen that borrowed it closing.
     */
    if (percent == pthis->scheduled_brightness) {
        return;
    }

    if (nn20clock_display_set_brightness(pthis->display, percent) == ESP_OK) {
        pthis->scheduled_brightness = percent;
        ESP_LOGI(TAG, "brightness %u%% (%02u:%02u, %s%s)", (unsigned)percent,
                 (unsigned)(now / 60u), (unsigned)(now % 60u),
                 nn20clock_brightness_is_day(&pthis->schedule, now) ? "day"
                                                                   : "night",
                 pthis->playing ? ", playing" : "");
    }
}

static int brightness_tick_private(nn20_worker_ctx *worker, void *user_data)
{
    (void)worker;
    apply_scheduled_brightness(user_data);
    return 0;
}

/* Runs on the Timer's worker, for every event; design 6 leaves the
 * filtering to the subscriber. It only posts. */
static void on_brightness_tick(const NN20ClockTimerEvent *event,
                               void *user_data)
{
    NN20ClockApp *pthis = user_data;

    /*
     * The minute tick is the schedule's clock. TIME_SYNCED is here too
     * because NTP can move the wall clock hours in one step, and the
     * brightness should follow it immediately rather than at the next
     * minute - which on a clock that has just learned it is 3am is the
     * difference between a dim face and a floodlight.
     */
    if (event->type == NN20CLOCK_TIMER_EVENT_MINUTE ||
        event->type == NN20CLOCK_TIMER_EVENT_TIME_SYNCED) {
        (void)nn20_worker_post(nn20clock_workers_core(pthis->workers),
                               brightness_tick_private, pthis);
    }
}

/*
 * Take the panel back after something borrowed it.
 *
 * Posted to the CoreWorker from the state callback, so the schedule's
 * state is read and written by the worker that owns it.
 *
 * The playback screen's brightness slider is a live preview that stores
 * nothing, so when that screen goes away the schedule should have the
 * backlight again immediately rather than at the next boundary. Zeroing
 * the tracking value is what gets past the "has it changed" test above -
 * zero is not a brightness the schedule can ask for, since the display
 * floor is well above it.
 */
static int brightness_reclaim_private(nn20_worker_ctx *worker,
                                      void *user_data)
{
    (void)worker;
    NN20ClockApp *pthis = user_data;

    pthis->scheduled_brightness = 0u;
    apply_scheduled_brightness(pthis);
    return 0;
}

/*
 * A film has started or stopped, so the playback minimum starts or
 * stops counting. Posted rather than set where it is heard, like
 * everything else here: the edges arrive on the ClockManagerWorker and
 * the UiWorker, and `playing` belongs to the CoreWorker.
 *
 * Retakes the panel the way the reclaim does, for the same reason - a
 * slider on the screen that is closing may have moved it, and "has the
 * schedule changed its mind" is not the question when what changed is
 * which rule applies.
 */
static int brightness_playback_begun_private(nn20_worker_ctx *worker,
                                             void *user_data)
{
    NN20ClockApp *pthis = user_data;
    pthis->playing = true;
    return brightness_reclaim_private(worker, pthis);
}

static int brightness_playback_ended_private(nn20_worker_ctx *worker,
                                             void *user_data)
{
    NN20ClockApp *pthis = user_data;
    pthis->playing = false;
    return brightness_reclaim_private(worker, pthis);
}

/*
 * ------------------------------------------------------------------
 * The FTP server runs only while the media screen is open
 * ------------------------------------------------------------------
 *
 * It has no authentication - nn20clock_ftp.h argues why - so any device
 * on the network can add or delete media for as long as it is
 * listening. Leaving it listening forever means that window is the
 * clock's whole uptime. Tying it to the one screen that exists to
 * manage media makes the window a deliberate act: somebody has to walk
 * up to the clock, open Settings, and tap Media.
 *
 * Both of these run on the CoreWorker rather than on the manager's.
 * Stopping blocks until the acceptor and every session task have
 * actually gone - up to two seconds if a client is mid-transfer - and
 * the ClockManagerWorker is what delivers alarm events. An alarm two
 * seconds late because somebody closed a settings screen would be a
 * bad trade.
 *
 * They are posted, so they serialise behind each other on that one
 * worker: opening and closing the screen quickly cannot interleave a
 * start with a stop.
 */
static int ftp_start_private(nn20_worker_ctx *worker, void *user_data)
{
    (void)worker;
    NN20ClockApp *pthis = user_data;

    /*
     * Without a card there is nothing to serve, so it is not started -
     * an FTP server that answers and then fails every command is worse
     * than one that is not there. The screen says which of the two is
     * missing.
     */
    if (!nn20clock_sd_is_mounted(pthis->sd)) {
        ESP_LOGW(TAG, "no SD card; FTP not started");
    } else if (nn20clock_net_state(pthis->net) == NN20CLOCK_NET_DISABLED) {
        ESP_LOGW(TAG, "no network; FTP not started");
    } else if (nn20clock_ftp_start(pthis->ftp) != ESP_OK) {
        ESP_LOGE(TAG, "FTP server would not start");
    }
    return 0;
}

static int ftp_stop_private(nn20_worker_ctx *worker, void *user_data)
{
    (void)worker;
    NN20ClockApp *pthis = user_data;

    /*
     * A transfer in flight is cut off here. That is the honest
     * consequence of the rule above and not an oversight: the server is
     * reachable while the screen is open, and closing the screen is how
     * you take it away. A half-received file is removed by the server's
     * own upload path, so the card is not left with a truncated clip.
     */
    (void)nn20clock_ftp_stop(pthis->ftp);
    return 0;
}

/*
 * Runs on the ClockManagerWorker after every accepted transition, so it
 * must stay short - see NN20ClockManagerStateFn. It only posts.
 */
static void on_state_change(NN20ClockManagerState from,
                            NN20ClockManagerState to, void *user_data)
{
    NN20ClockApp *pthis = user_data;
    nn20_worker_ctx *const core = nn20clock_workers_core(pthis->workers);

    if (to == NN20CLOCK_STATE_MEDIA_MANAGEMENT) {
        (void)nn20_worker_post(core, ftp_start_private, pthis);
    } else if (from == NN20CLOCK_STATE_MEDIA_MANAGEMENT) {
        (void)nn20_worker_post(core, ftp_stop_private, pthis);
    }

    /*
     * Both of these screens carry a brightness slider, and what a finger
     * did with it stands only while the screen is up. Leaving either
     * ends the film too, so the playback minimum stops with it.
     *
     * The settings screen previews on the panel as well - the playback
     * minimum slider most of all, which is dragged at night to a value
     * the clock face will not be shown at - so leaving it hands the
     * panel back the same way.
     *
     * The end is posted before any beginning: an alarm interrupting a
     * film is both at once, and the CoreWorker runs them in this order.
     */
    if (from == NN20CLOCK_STATE_ALARM_RINGING ||
        from == NN20CLOCK_STATE_MEDIA_PLAYBACK) {
        (void)nn20_worker_post(core, brightness_playback_ended_private, pthis);
    } else if (from == NN20CLOCK_STATE_DEVICE_SETTINGS) {
        (void)nn20_worker_post(core, brightness_reclaim_private, pthis);
    }

    /*
     * An alarm is a film from the moment it rings. Entering
     * MEDIA_PLAYBACK is not - that state opens on the list, and the film
     * starts in play_media() once something is chosen.
     */
    if (to == NN20CLOCK_STATE_ALARM_RINGING) {
        (void)nn20_worker_post(core, brightness_playback_begun_private, pthis);
    }
}

/*
 * Everything this app keeps for one ringing, thrown away together.
 *
 * Runs on the ClockManagerWorker - see
 * NN20ClockManagerRingingEndedFn - which is the worker that owns the
 * rolled film, rolls it in resolve_media(), and is therefore allowed to
 * clear it here without posting.
 *
 * A snooze never reaches this: it is the same occurrence continued, and
 * both of the things below are what it is coming back for. Asking the
 * manager rather than reading the Timer's pending snooze is what makes
 * that reliable - a snooze that returns past its hour ends without any
 * state change at all, and a rule written against the ALARM_RINGING
 * edge missed it.
 */
static void on_ringing_ended(uint32_t alarm_id, void *user_data)
{
    NN20ClockApp *pthis = user_data;

    /* The film a <random> alarm rolled, so the next morning is a fresh
     * draw. */
    if (pthis->random_alarm_id == alarm_id) {
        ESP_LOGI(TAG, "alarm %u is over; the next one draws again",
                 (unsigned)alarm_id);
        pthis->random_alarm_id = NN20CLOCK_ALARM_ID_NONE;
        pthis->random_choice[0] = '\0';
        pthis->random_folder[0] = '\0';
    }

    /*
     * And the player's memory of where in the film it had got to -
     * otherwise tomorrow's alarm starts halfway through, on the frame
     * this one ended on. VideoPlayerUi's Off button does this too, at
     * the press rather than here; this is the one that catches the
     * dismissals nobody pressed anything for.
     */
    nn20clock_player_forget_position(pthis->player);
}

/*
 * Design 8's "select which UI is active". Runs on the
 * ClockManagerWorker, so it must not touch LVGL - the screen
 * constructor only allocates, and the widgets are built in show() on
 * the UiWorker.
 */
static NN20ClockUiBase *screen_factory(NN20ClockManagerState state,
                                       void *user_data)
{
    NN20ClockApp *pthis = user_data;
    nn20_worker_ctx *const ui_worker = nn20clock_workers_ui(pthis->workers);

    switch (state) {
    case NN20CLOCK_STATE_TIME:
        /* The player is passed for one thing: the frame a snoozed alarm
         * was interrupted on, shown in the corner while it waits. */
        return nn20clock_time_ui_ctor(ui_worker, pthis->manager,
                                      nn20clock_manager_handle_ui_command,
                                      pthis->player);

    case NN20CLOCK_STATE_ALARM_SETTINGS:
        /* The screen edits alarms through the manager's service rather
         * than through Storage, which design 11 keeps out of screens. */
        return nn20clock_alarm_ui_ctor(
            ui_worker, pthis->manager, nn20clock_manager_handle_ui_command,
            nn20clock_manager_alarm_service(pthis->manager), pthis->sd);

    case NN20CLOCK_STATE_DEVICE_SETTINGS:
        /* Same rule for device settings; the radio is passed separately
         * because scanning is a query, not a setting. */
        return nn20clock_device_ui_ctor(
            ui_worker, pthis->manager, nn20clock_manager_handle_ui_command,
            nn20clock_manager_device_service(pthis->manager), pthis->net,
            pthis->ota);

    case NN20CLOCK_STATE_ALARM_RINGING: {
        /* The screen starts the playback in show() and stops it when it
         * goes away, so the alarm's sound and its screen have exactly
         * the same lifetime. */
        char path[NN20CLOCK_MEDIA_PATH_MAX];
        uint8_t volume = 0;
        bool random_set = false;
        resolve_media(pthis, path, sizeof(path), &volume, &random_set);

        return nn20clock_video_ui_ctor(
            ui_worker, pthis->manager, nn20clock_manager_handle_ui_command,
            pthis->player, NN20CLOCK_VIDEO_UI_ALARM, path, volume,
            0u /* an alarm has no sleep timer */,
            /* Only a <random> alarm is given somewhere to ask for the
             * next film; any other one loops the one it named. */
            random_set ? next_random_media : NULL, pthis,
            nn20clock_manager_device_service(pthis->manager));
    }

    case NN20CLOCK_STATE_MEDIA_PLAYBACK:
        /*
         * The list, not the player: this is the state entered by the
         * play icon, and nothing is playing yet. Choosing a file
         * replaces this screen with the playback one through
         * play_media() below - the same state, a different screen, and
         * no transition, because none of what design 5 records about
         * the device has changed.
         */
        return nn20clock_media_ui_ctor(
            ui_worker, pthis->manager, nn20clock_manager_handle_ui_command,
            nn20clock_manager_media_service(pthis->manager), pthis->sd);

    case NN20CLOCK_STATE_MEDIA_MANAGEMENT:
        /*
         * All three are borrowed and only queried. They are passed
         * directly rather than behind a service for the reason
         * DeviceSettingsUi passes the radio directly: a service is for
         * settings a screen may change, and asking a component how it
         * is doing is not that. Any of them may be NULL here - a build
         * with no card or no radio still gets a screen, and it says
         * which part is missing.
         */
        return nn20clock_media_mgmt_ui_ctor(
            ui_worker, pthis->manager, nn20clock_manager_handle_ui_command,
            pthis->ftp, pthis->net, pthis->sd);

    default:
        break;
    }

    /*
     * The remaining state has no screen yet: an error screen is
     * Milestone 12. Returning NULL leaves the display empty, which is
     * honest - better than showing a screen that does not match the
     * state.
     */
    return NULL;
}

/*
 * Start playing something the user chose (design 17, Milestone 8).
 *
 * Runs on the UiWorker, called by MediaPlaybackUi through the manager's
 * media service. It builds the playback screen and hands it over; the
 * manager destroys the list and shows this one, in that order, on its
 * own worker. Nothing here waits on anything, which is what keeps the
 * UiWorker out of a cycle.
 *
 * `relative_path` is a file path from the card's root - "wake.avi",
 * "morning/wake.avi" - straight from the list the screen was showing.
 * It is resolved here for the same reason an alarm's is: the mount
 * point belongs in one place, and a path that tries to leave the card
 * is refused rather than opened.
 */
static esp_err_t play_media(void *ctx, const char *relative_path,
                            uint32_t sleep_ms)
{
    NN20ClockApp *pthis = ctx;

    char path[NN20CLOCK_MEDIA_FULL_PATH_MAX];
    if (nn20clock_media_path(NN20CLOCK_SD_MOUNT_POINT, relative_path, path,
                             sizeof(path)) != ESP_OK) {
        ESP_LOGW(TAG, "%s is not a path this device will play",
                 relative_path);
        return ESP_ERR_INVALID_ARG;
    }

    NN20ClockUiBase *const screen = nn20clock_video_ui_ctor(
        nn20clock_workers_ui(pthis->workers), pthis->manager,
        nn20clock_manager_handle_ui_command, pthis->player,
        NN20CLOCK_VIDEO_UI_MEDIA, path, device_volume(pthis), sleep_ms,
        NULL, NULL /* one file, chosen by hand, played once */,
        nn20clock_manager_device_service(pthis->manager));
    if (screen == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const esp_err_t err = nn20clock_manager_set_screen(pthis->manager,
                                                       screen);
    if (err != ESP_OK) {
        /* Ownership only passes on success, so this is ours to free -
         * and destroying it here, on the UiWorker, is where its own
         * destroy hook expects to run anyway. */
        nn20clock_ui_destroy(screen);
        return err;
    }

    /* The film has the panel: the playback minimum applies. */
    (void)nn20_worker_post(nn20clock_workers_core(pthis->workers),
                           brightness_playback_begun_private, pthis);
    return ESP_OK;
}

/*
 * The next clip for manual <random>, asked for when the last one ended.
 *
 * The mirror of next_random_media(), and separate for the reason the
 * fields are: see NN20ClockApp::manual_folder. Same rules - the same folder,
 * never recursing, and the clip that just played left out of the draw
 * whenever the folder holds anything else.
 *
 * One difference. An alarm that cannot draw must still make a noise, so
 * it falls back to the tone and the screen replays what it has.
 * Returning false here just stops the evening's films, which is the
 * honest outcome when somebody has deleted the folder over FTP while it
 * was playing.
 *
 * Runs on the UiWorker, which is the one that hears the end of a clip.
 */
static bool next_manual_media(void *ctx, char *out_path, size_t out_size)
{
    NN20ClockApp *pthis = ctx;

    char relative[NN20CLOCK_MEDIA_PATH_MAX];
    if (!choose_random_media(pthis, pthis->manual_folder, relative,
                             sizeof(relative), pthis->manual_choice)) {
        return false;
    }
    if (strcmp(relative, pthis->manual_choice) == 0) {
        /* The only clip in the folder. The screen replays what it has,
         * which costs it nothing - and saying so here keeps the player
         * from being stopped and started for the same file. */
        return false;
    }

    if (nn20clock_media_path(NN20CLOCK_SD_MOUNT_POINT, relative, out_path,
                             out_size) != ESP_OK) {
        return false;
    }

    (void)snprintf(pthis->manual_choice, sizeof(pthis->manual_choice), "%s",
                   relative);
    return true;
}

/*
 * Start playing a folder at random (the manual half of design 13's
 * <random>).
 *
 * The same screen as a fixed clip, given somewhere to ask for the next
 * one - which is the whole difference between the two. A fixed clip
 * plays once and stops; this keeps drawing until the sleep timer cuts
 * it or somebody leaves.
 *
 * Runs on the UiWorker, like play_media().
 */
static esp_err_t play_random_media(void *ctx, const char *folder_path,
                                   uint32_t sleep_ms)
{
    NN20ClockApp *pthis = ctx;

    /* The first draw. Nothing to exclude yet, and nothing to play if it
     * fails - unlike an alarm, silence is an acceptable answer here. */
    char relative[NN20CLOCK_MEDIA_PATH_MAX];
    if (!choose_random_media(pthis, folder_path, relative, sizeof(relative),
                             NULL)) {
        ESP_LOGW(TAG, "nothing to play at random in %s",
                 (folder_path[0] == '\0') ? "the root folder" : folder_path);
        return ESP_ERR_NOT_FOUND;
    }

    char path[NN20CLOCK_MEDIA_FULL_PATH_MAX];
    if (nn20clock_media_path(NN20CLOCK_SD_MOUNT_POINT, relative, path,
                             sizeof(path)) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Held for next_manual_media(), which has no other way to know
     * which folder the user picked. */
    (void)snprintf(pthis->manual_folder, sizeof(pthis->manual_folder), "%s",
                   folder_path);
    (void)snprintf(pthis->manual_choice, sizeof(pthis->manual_choice), "%s",
                   relative);

    ESP_LOGI(TAG, "playing %s at random, starting with %s",
             (folder_path[0] == '\0') ? "the root folder" : folder_path,
             relative);

    NN20ClockUiBase *const screen = nn20clock_video_ui_ctor(
        nn20clock_workers_ui(pthis->workers), pthis->manager,
        nn20clock_manager_handle_ui_command, pthis->player,
        NN20CLOCK_VIDEO_UI_MEDIA, path, device_volume(pthis), sleep_ms,
        next_manual_media, pthis,
        nn20clock_manager_device_service(pthis->manager));
    if (screen == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const esp_err_t err = nn20clock_manager_set_screen(pthis->manager,
                                                       screen);
    if (err != ESP_OK) {
        /* Ownership only passes on success - see play_media(). */
        nn20clock_ui_destroy(screen);
        return err;
    }

    (void)nn20_worker_post(nn20clock_workers_core(pthis->workers),
                           brightness_playback_begun_private, pthis);
    return ESP_OK;
}

/*
 * The FTP server's view of the SD card.
 *
 * These live here rather than in either component on purpose: design 12
 * wants an FTP server reusable in other projects, so it knows nothing
 * about SD cards, and the SD component knows nothing about networks.
 * This is the seam between them, and it is the only place that has to
 * know both exist.
 *
 * They run on the FTP server's own tasks, not on any worker. They touch
 * only the SD component, which is safe to call from any thread as long
 * as the calls do not overlap - the server serves at most two clients
 * and the card is fast enough that this has not been worth serialising
 * further. If a third caller ever appears, this is where a worker hop
 * belongs.
 *
 * Every path arrives from the network. None of it is trusted twice
 * over: the FTP server canonicalises it before calling any of these,
 * and the SD component refuses anything that is not a safe relative
 * path under the card's root. Two checks of the same rule, which is
 * what keeps a client inside the card.
 */
static size_t ftp_list(void *ctx, const char *dir, NN20ClockFtpEntry *entries,
                       size_t max)
{
    NN20ClockApp *pthis = ctx;

    /* Over 30 KB - the heap, never a task stack. */
    NN20ClockMediaList *list = malloc(sizeof(*list));
    if (list == NULL) {
        return 0;
    }

    size_t count = 0;
    if (nn20clock_sd_list_media(pthis->sd, dir, list) == ESP_OK) {
        count = (list->count < max) ? list->count : max;
        for (size_t i = 0; i < count; i++) {
            const NN20ClockMediaEntry *const entry = &list->entries[i];
            /* The name, not the path: FTP listings are relative to the
             * directory being listed, and a client shown "morning/x.avi"
             * inside "morning" asks for "morning/morning/x.avi" next. */
            snprintf(entries[i].name, sizeof(entries[i].name), "%s",
                     entry->name);
            entries[i].size_bytes = entry->size_bytes;
            entries[i].is_dir = (entry->type == NN20CLOCK_MEDIA_ENTRY_FOLDER);
        }
    }

    free(list);
    return count;
}

static esp_err_t ftp_stat(void *ctx, const char *path, bool *out_is_dir,
                          uint32_t *out_size)
{
    NN20ClockApp *pthis = ctx;
    return nn20clock_sd_stat(pthis->sd, path, out_is_dir, out_size);
}

static void *ftp_open(void *ctx, const char *path, const char *mode)
{
    NN20ClockApp *pthis = ctx;
    (void)pthis;

    char full[NN20CLOCK_MEDIA_FULL_PATH_MAX];
    if (nn20clock_sd_path(path, full, sizeof(full)) != ESP_OK) {
        /* Not a safe path: refused before the filesystem sees it. */
        return NULL;
    }
    return fopen(full, mode);
}

static void *ftp_open_read(void *ctx, const char *path)
{
    return ftp_open(ctx, path, "rb");
}

static void *ftp_open_write(void *ctx, const char *path)
{
    return ftp_open(ctx, path, "wb");
}

static size_t ftp_read(void *ctx, void *handle, void *buffer, size_t size)
{
    (void)ctx;
    return fread(buffer, 1, size, (FILE *)handle);
}

static size_t ftp_write(void *ctx, void *handle, const void *buffer,
                        size_t size)
{
    (void)ctx;
    return fwrite(buffer, 1, size, (FILE *)handle);
}

static void ftp_close(void *ctx, void *handle)
{
    (void)ctx;
    (void)fclose((FILE *)handle);
}

static esp_err_t ftp_remove_file(void *ctx, const char *path)
{
    NN20ClockApp *pthis = ctx;
    return nn20clock_sd_delete_file(pthis->sd, path);
}

static esp_err_t ftp_make_dir(void *ctx, const char *path)
{
    NN20ClockApp *pthis = ctx;
    return nn20clock_sd_make_folder(pthis->sd, path);
}

static esp_err_t ftp_remove_dir(void *ctx, const char *path)
{
    NN20ClockApp *pthis = ctx;
    return nn20clock_sd_delete_folder(pthis->sd, path);
}

static esp_err_t ftp_rename(void *ctx, const char *from, const char *to)
{
    NN20ClockApp *pthis = ctx;
    return nn20clock_sd_rename(pthis->sd, from, to);
}

static esp_err_t ftp_set_hidden(void *ctx, const char *path, bool hidden)
{
    NN20ClockApp *pthis = ctx;
    return nn20clock_sd_set_hidden(pthis->sd, path, hidden);
}

/* The two device hooks. Called on the UiWorker, from the settings
 * screen, through the manager's device service. */
static esp_err_t apply_volume_hook(void *ctx, uint8_t percent)
{
    NN20ClockApp *pthis = ctx;
    return nn20clock_audio_set_volume(pthis->audio, percent);
}

static esp_err_t apply_brightness_hook(void *ctx, uint8_t percent)
{
    NN20ClockApp *pthis = ctx;
    /*
     * A live preview from a slider. It is deliberately not recorded in
     * applied_brightness: that field is what the schedule believes it
     * last set, and leaving it alone is what makes the next minute tick
     * notice the difference and put the schedule back.
     */
    return nn20clock_display_set_brightness(pthis->display, percent);
}

static uint8_t current_brightness_hook(void *ctx)
{
    NN20ClockApp *pthis = ctx;
    return nn20clock_display_brightness(pthis->display);
}

static esp_err_t set_ntp_hook(void *ctx, bool enabled)
{
    NN20ClockApp *pthis = ctx;
    return nn20clock_net_set_ntp(pthis->net, enabled);
}

/*
 * Set the wall clock by hand, for a device with no internet.
 *
 * Refused while the sync is on, rather than allowed and then quietly
 * undone: SNTP would move the clock back within minutes and the user
 * would have no way to tell that is what happened.
 *
 * The value is stored as well as applied. Nothing else writes
 * last_known_time, and without it a power cut takes a hand-set clock
 * back to 1970 - which makes the whole feature pointless on exactly the
 * device it exists for.
 */
static esp_err_t set_time_hook(void *ctx, time_t when)
{
    NN20ClockApp *pthis = ctx;

    if (nn20clock_net_ntp_enabled(pthis->net)) {
        ESP_LOGW(TAG, "refusing to set the clock while the time sync is on");
        return ESP_ERR_INVALID_STATE;
    }

    const struct timeval tv = { .tv_sec = when, .tv_usec = 0 };
    if (settimeofday(&tv, NULL) != 0) {
        ESP_LOGE(TAG, "could not set the system clock");
        return ESP_FAIL;
    }

    NN20ClockConfig config;
    if (nn20clock_storage_load_config(pthis->storage, &config) == ESP_OK) {
        config.last_known_time = when;
        (void)nn20clock_storage_save_config(pthis->storage, &config);
    }

    ESP_LOGI(TAG, "clock set by hand");

    /* The schedule and every screen work from the wall clock, so both
     * want to hear that it just moved. */
    (void)nn20_worker_post(nn20clock_workers_core(pthis->workers),
                           brightness_reclaim_private, pthis);

    /*
     * notify_time_set, not refresh: the user setting the clock is the
     * only authoritative source this device has, so it must both clear
     * "clock not set" on the face - SNTP is off and will never clear it
     * - and re-baseline the schedule, so the span the clock just jumped
     * over is not mistaken for elapsed time.
     */
    (void)nn20clock_timer_notify_time_set(pthis->timer);
    return ESP_OK;
}

static esp_err_t connect_wifi_hook(void *ctx, const char *ssid,
                                   const char *password)
{
    NN20ClockApp *pthis = ctx;
    return nn20clock_net_connect_to(pthis->net, ssid, password);
}

/* Runs on the SNTP task: post and return, nothing more. */
static void on_time_synced(void *user_data)
{
    NN20ClockApp *pthis = user_data;
    (void)nn20clock_timer_notify_time_synced(pthis->timer);
}
#endif

/*
 * Where the Timer's schedule comes from (design 7: the Timer loads
 * configured alarm schedules from storage). The Timer does not know
 * about Storage - it asks through this - which is what keeps the two
 * components independent and the Timer testable without a filesystem.
 *
 * Runs on the Timer's worker and reads Storage, which waits on the
 * StorageWorker. One-way: nothing on the StorageWorker ever waits on
 * the Timer.
 */
static esp_err_t load_alarms(NN20ClockAlarmList *out_alarms, void *user_data)
{
    NN20ClockApp *pthis = user_data;
    return nn20clock_storage_list_alarms(pthis->storage, out_alarms);
}

/* --------------------------------------------------------- lifecycle -- */

NN20ClockApp *nn20clock_app_ctor(void)
{
    NN20ClockApp *pthis = calloc(1, sizeof(*pthis));
    if (pthis == NULL) {
        ESP_LOGE(TAG, "out of memory");
        return NULL;
    }

    atomic_init(&pthis->running, false);

    /* 1. The threads first: everything below takes one. */
    pthis->workers = nn20clock_workers_ctor();
    if (pthis->workers == NULL) {
        ESP_LOGE(TAG, "worker set failed");
        goto fail;
    }

    /* 2. Storage, on the StorageWorker. The Timer takes it, so it has to
     *    exist first even though nothing loads from it yet. */
    pthis->storage = nn20clock_storage_ctor(
        nn20clock_workers_storage(pthis->workers));
    if (pthis->storage == NULL) {
        ESP_LOGE(TAG, "Storage failed");
        goto fail;
    }

#if defined(ESP_PLATFORM)
    /* 2b. The SD card. Nothing depends on it being present - design 15
     *     treats a missing card as recoverable - but the FTP server
     *     needs the object to exist. */
    pthis->sd = nn20clock_sd_ctor();
    if (pthis->sd == NULL) {
        ESP_LOGE(TAG, "SD failed");
        goto fail;
    }

    /* 3. The panel and LVGL, on the UiWorker (design 11). Built before
     *    the manager because the manager's first transition shows a
     *    screen on it. */
    pthis->display = nn20clock_display_ctor(
        nn20clock_workers_ui(pthis->workers));
    if (pthis->display == NULL) {
        ESP_LOGE(TAG, "Display failed");
        goto fail;
    }

    /* 3b. Sound, and the player that drives it. After the display
     *     because the codec shares the I2C bus the display brings up,
     *     which means - by design 4's reverse teardown - that they are
     *     also destroyed before it. */
    pthis->audio = nn20clock_audio_ctor(pthis->display);
    if (pthis->audio == NULL) {
        ESP_LOGE(TAG, "Audio failed");
        goto fail;
    }

    pthis->player = nn20clock_player_ctor(
        nn20clock_workers_player(pthis->workers),
        nn20clock_workers_reader(pthis->workers),
        nn20clock_workers_ui(pthis->workers),
        pthis->display, pthis->audio);
    if (pthis->player == NULL) {
        ESP_LOGE(TAG, "Player failed");
        goto fail;
    }
#endif

    /* 4. The Timer, which brings its own worker (design 6, 7): a
     *    one-second tick must not queue behind SD card I/O or a screen
     *    redraw, so it is deliberately not one of design 4's four. */
    pthis->timer = nn20clock_timer_ctor(pthis->storage);
    if (pthis->timer == NULL) {
        ESP_LOGE(TAG, "Timer failed");
        goto fail;
    }

    (void)nn20clock_timer_set_alarm_source(pthis->timer, load_alarms, pthis);

    /* 5. The manager: it borrows the services above. */
    pthis->manager = nn20clock_manager_ctor(
        nn20clock_workers_clock_manager(pthis->workers),
        pthis->storage, pthis->timer);
    if (pthis->manager == NULL) {
        ESP_LOGE(TAG, "ClockManager failed");
        goto fail;
    }

#if defined(ESP_PLATFORM)
    /* What this app keeps for a ringing, and when to let go of it. */
    (void)nn20clock_manager_set_ringing_ended_callback(pthis->manager,
                                                       on_ringing_ended, pthis);

    /* The manager builds screens from this on every transition. On the
     * host there is no factory and the manager runs headless. */
    (void)nn20clock_manager_set_screen_factory(pthis->manager, screen_factory,
                                               pthis);

    /* What opens and closes the FTP server - see on_state_change(). */
    (void)nn20clock_manager_set_state_callback(pthis->manager,
                                               on_state_change, pthis);

    /* How the manager reaches the panel and the radio for device
     * settings. It owns neither, and the components do not exist off
     * the target - hence hooks rather than pointers. */
    const NN20ClockDeviceHooks hooks = {
        .apply_brightness = apply_brightness_hook,
        .current_brightness = current_brightness_hook,
        .apply_volume = apply_volume_hook,
        .connect_wifi = connect_wifi_hook,
        .set_ntp = set_ntp_hook,
        .set_time = set_time_hook,
        .ctx = pthis,
    };
    (void)nn20clock_manager_set_device_hooks(pthis->manager, hooks);

    /* And how it reaches the player, for media the user picked rather
     * than an alarm. Same arrangement, same reason. */
    const NN20ClockMediaHooks media_hooks = {
        .play_media = play_media,
        .play_random_media = play_random_media,
        .ctx = pthis,
    };
    (void)nn20clock_manager_set_media_hooks(pthis->manager, media_hooks);

    /* 6. The network last: nothing below it depends on it, and design
     *    14 requires the clock to work when it never connects. */
    pthis->net = nn20clock_net_ctor(nn20clock_workers_core(pthis->workers));
    if (pthis->net == NULL) {
        ESP_LOGE(TAG, "Net failed");
        goto fail;
    }

    /* SNTP setting the clock is what the Timer needs to hear about: it
     * marks the time trustworthy and forces the next tick to report,
     * which is what moves a wrong time off the display. */
    (void)nn20clock_net_set_synced_callback(pthis->net, on_time_synced,
                                            pthis);

    /*
     * The updater. Constructed beside the network because that is what
     * it needs, but not started with it: nothing here polls, and both
     * the check and the install are a button on the About screen.
     *
     * A failure to construct one costs the update button and nothing
     * else - the screen says the build cannot update itself and the
     * clock carries on, which is the same bargain design 14 strikes for
     * a clock with no network.
     */
    pthis->ota = nn20clock_ota_ctor();
    if (pthis->ota == NULL) {
        ESP_LOGW(TAG, "no updater; the About screen will say so");
    }

    /* 7. The FTP server (design 12), serving the card through the
     *    adapter above. Built last because it borrows both. */
    const NN20ClockFtpOps ftp_ops = {
        .list = ftp_list,
        .stat = ftp_stat,
        .open_read = ftp_open_read,
        .open_write = ftp_open_write,
        .read = ftp_read,
        .write = ftp_write,
        .close = ftp_close,
        .remove_file = ftp_remove_file,
        .make_dir = ftp_make_dir,
        .remove_dir = ftp_remove_dir,
        .rename = ftp_rename,
        .set_hidden = ftp_set_hidden,
        .ctx = pthis,
    };
    pthis->ftp = nn20clock_ftp_ctor(ftp_ops, NN20CLOCK_FTP_DEFAULT_PORT);
    if (pthis->ftp == NULL) {
        ESP_LOGE(TAG, "FTP server failed");
        goto fail;
    }
#endif

    ESP_LOGI(TAG, "services constructed");
    return pthis;

fail:
    nn20clock_app_dtor(pthis);
    return NULL;
}

void nn20clock_app_dtor(NN20ClockApp *pthis)
{
    if (pthis == NULL) {
        return;
    }

    if (nn20clock_app_is_running(pthis)) {
        (void)nn20clock_app_stop(pthis);
    }

    /* Reverse of the ctor, and it has to stay that way: the manager
     * holds the timer and storage, and all three hold workers. The
     * worker set goes last so a component's dtor can still post to its
     * worker to drain what is queued. */
#if defined(ESP_PLATFORM)
    /* The server first: its tasks call into the card. The updater
     * before the network it downloads over; its dtor waits for a
     * transfer in flight to be abandoned. */
    nn20clock_ftp_dtor(pthis->ftp);
    nn20clock_ota_dtor(pthis->ota);
    nn20clock_net_dtor(pthis->net);
#endif
    nn20clock_manager_dtor(pthis->manager);
    nn20clock_timer_dtor(pthis->timer);
#if defined(ESP_PLATFORM)
    nn20clock_player_dtor(pthis->player);
    nn20clock_audio_dtor(pthis->audio);
    nn20clock_display_dtor(pthis->display);
    nn20clock_sd_dtor(pthis->sd);
#endif
    nn20clock_storage_dtor(pthis->storage);
    nn20clock_workers_dtor(pthis->workers);

    free(pthis);
}

/*
 * Runs on the CoreWorker. Each step is waited on by the step's own
 * component, so by the time one returns the next may depend on it.
 */
static int start_private(nn20_worker_ctx *worker, void *user_data)
{
    (void)worker;
    NN20ClockApp *pthis = user_data;

    /*
     * Storage stays up across a stop: it owns no thread of its own and,
     * unlike the Timer and the manager, has nothing running to shut
     * down. So starting an app that was stopped and restarted must skip
     * it rather than trip its already-started guard.
     *
     * Revisit at Milestone 3: once _start() opens an NVS handle, a stop
     * probably ought to close it, and this becomes a real restart.
     */
    esp_err_t err = ESP_OK;
    if (!nn20clock_storage_is_ready(pthis->storage)) {
        err = nn20clock_storage_start(pthis->storage);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Storage start failed (0x%x)", (unsigned)err);
            goto fail;
        }
    }

#if defined(ESP_PLATFORM)
    /* Before the manager: its first transition shows a screen, and a
     * screen cannot be shown on a panel that is not up yet. */
    err = nn20clock_display_start(pthis->display);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Display start failed (0x%x)", (unsigned)err);
        goto fail;
    }

    /*
     * Sound, then the player, both after the display: the codec takes
     * the I2C bus the display created when it started, and the player
     * takes about three megabytes of PSRAM for its frame buffers.
     *
     * Neither is fatal. Design 15 makes audio failure recoverable, and
     * a clock that shows the time without being able to ring is still a
     * clock - it just says so, loudly, in the log.
     */
    err = nn20clock_audio_start(pthis->audio);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "no audio (0x%x); alarms will be silent",
                 (unsigned)err);
    } else {
        err = nn20clock_player_start(pthis->player);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "no player (0x%x); alarms will be silent",
                     (unsigned)err);
        }
    }
    err = ESP_OK;

    /* Design 14: one timezone, applied before any time is displayed.
     * Milestone 3 takes this from storage instead of Kconfig. */
    (void)nn20clock_timer_set_timezone(pthis->timer,
                                       CONFIG_NN20CLOCK_TIMEZONE);
#endif

    /*
     * The schedule is loaded before the Timer starts ticking, so the
     * first tick already has the real alarms. Storage is up by now,
     * which is why this cannot happen any earlier.
     *
     * A failure here is not fatal: the Timer keeps whatever schedule it
     * has (none, at boot) and the clock still tells the time. It is
     * logged loudly because a clock silently running with no alarms is
     * the failure a user discovers by oversleeping.
     */
    err = nn20clock_timer_reload_schedule(pthis->timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "alarm schedule unavailable (0x%x); no alarms will "
                      "fire until it reloads", (unsigned)err);
    }

    err = nn20clock_timer_start(pthis->timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Timer start failed (0x%x)", (unsigned)err);
        goto fail;
    }

#if defined(ESP_PLATFORM)
    /*
     * The minute tick that runs the brightness schedule. Not fatal if
     * it fails: the panel keeps whatever it was set to at boot, which
     * is a clock with a fixed brightness rather than no clock.
     */
    pthis->brightness_subscriber.on_event = on_brightness_tick;
    pthis->brightness_subscriber.user_data = pthis;
    if (nn20clock_timer_subscribe(pthis->timer,
                                  &pthis->brightness_subscriber) != ESP_OK) {
        ESP_LOGE(TAG, "no minute tick for the brightness schedule; "
                      "brightness will not follow the clock");
    }
#endif

    /* Starting the manager subscribes it to the Timer and takes design
     * 5's BOOT -> TIME edge. */
    err = nn20clock_manager_start(pthis->manager);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ClockManager start failed (0x%x)", (unsigned)err);
        goto fail;
    }

#if defined(ESP_PLATFORM)
    /*
     * Last, and its failure is not the app's failure: design 14 has
     * three more time sources behind NTP, and a clock that refuses to
     * start because the Wi-Fi is down would be a worse clock.
     */
    /*
     * Credentials and brightness come from storage, which is up by now.
     * An empty stored SSID means no network has been chosen yet: the
     * radio still comes up so the settings screen can scan, and nothing
     * is joined until it does.
     */
    NN20ClockConfig config;
    if (nn20clock_storage_load_config(pthis->storage, &config) == ESP_OK) {
        /*
         * The schedule decides the brightness from here on, including
         * right now: a clock restarted at three in the morning should
         * come back dim, not at whatever it was in daylight.
         *
         * There is no separate startup floor any more, and there does
         * not need to be. It existed because one stored number, set too
         * low, left a screen nobody could read and no way to reach the
         * settings that would fix it. NN20CLOCK_DISPLAY_MIN_ON_BRIGHTNESS
         * now guarantees readability on *every* call rather than only at
         * boot, which is the stronger promise - and a schedule brightens
         * the clock again by itself every morning regardless.
         */
        apply_scheduled_brightness(pthis);   /* reads the schedule itself */

        /* The default an alarm without its own volume rings at. Unlike
         * brightness there is no floor: someone who set the volume to
         * zero can still see the screen and change it back. */
        (void)nn20clock_audio_set_volume(pthis->audio, config.volume);

        /*
         * The stored NTP choice, before the network starts, so a clock
         * the user took off the sync does not get one anyway during the
         * few seconds between the two calls.
         */
        (void)nn20clock_net_set_ntp(pthis->net, config.ntp_enabled);

        /*
         * With no sync there is nothing to set the clock, so it comes
         * back to the last time it was known to be right.
         *
         * Only when the system clock has not been set at all - the RTC
         * survives a reset, so after a plain reboot it is already
         * better than this stored value. The test is the year: the
         * epoch is 1970 and no clock this project cares about is
         * legitimately there.
         */
        if (!config.ntp_enabled && config.last_known_time > 0) {
            const time_t now = time(NULL);
            struct tm local = {0};
            if (localtime_r(&now, &local) != NULL && local.tm_year < 120) {
                const struct timeval tv = {
                    .tv_sec = config.last_known_time, .tv_usec = 0,
                };
                (void)settimeofday(&tv, NULL);
                ESP_LOGW(TAG, "no time sync; restored the last known time, "
                              "which is behind by however long the power "
                              "was off");
            }
        }

        err = nn20clock_net_start(pthis->net, config.wifi_ssid,
                                  config.wifi_password);
    } else {
        ESP_LOGE(TAG, "no stored config; no network until one is chosen "
                      "in the settings screen");
        /* Including a volume: the codec starts at zero, and leaving it
         * there would make an unreadable config a silent alarm. */
        (void)nn20clock_audio_set_volume(pthis->audio,
                                         device_volume(pthis));
        err = nn20clock_net_start(pthis->net, NULL, NULL);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Net start failed (0x%x); running without NTP",
                 (unsigned)err);
    }
#endif

#if defined(ESP_PLATFORM)
    /*
     * The SD card goes up AFTER the radio, and the order is not a
     * preference.
     *
     * On this board the ESP32-C6 co-processor is reached over SDIO, and
     * it shares the SDMMC peripheral with the card slot. Mounting the
     * card while esp_hosted is still bringing up its link leaves the
     * co-processor unable to initialise - "sdmmc_card_init failed",
     * repeatedly, and then no Wi-Fi at all - while the card itself
     * mounts perfectly and reports nothing wrong. Bringing the radio up
     * first and the card second keeps both.
     *
     * Design 15 makes a missing card recoverable, so a failure here
     * costs media and nothing else.
     */
    if (nn20clock_sd_mount(pthis->sd) != ESP_OK) {
        ESP_LOGW(TAG, "no SD card; alarm media will be unavailable");
    }

    /*
     * The FTP server is deliberately NOT started here. It listens only
     * while the media management screen is open - see
     * on_state_change(). Said out loud at boot because "the FTP server
     * is not answering" is otherwise a puzzle rather than a setting.
     */
    ESP_LOGI(TAG, "FTP listens only while the media screen is open");

    /*
     * Everything is up, so an image installed over the air has passed
     * the only test worth setting it: it reached a running clock. Until
     * this call the bootloader holds it as "pending verify" and reverts
     * to the previous slot at the next reset.
     *
     * Here rather than in app_main because this is the point where the
     * screen, the card, the radio and the alarm schedule have all
     * started - a build that panics on any of them un-installs itself.
     */
    (void)nn20clock_ota_mark_valid();
#endif

    atomic_store_explicit(&pthis->running, true, memory_order_release);
    ESP_LOGI(TAG, "started; state %s",
             nn20clock_manager_state_name(
                 nn20clock_manager_state(pthis->manager)));
    return 0;

fail:
    /* Design 5's BOOT -> ERROR edge. Put the failure where the state
     * machine can see it, not only in the log - Milestone 12 turns this
     * into a recovery path and an error screen. */
    (void)nn20clock_manager_request_state(pthis->manager,
                                          NN20CLOCK_STATE_ERROR);
    return -1;
}

esp_err_t nn20clock_app_start(NN20ClockApp *pthis)
{
    if (pthis == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (nn20clock_app_is_running(pthis)) {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t posted = post_result(nn20_worker_post_sync(
        nn20clock_workers_core(pthis->workers), start_private, pthis));
    if (posted != ESP_OK) {
        return posted;
    }
    /* post_sync reports whether the callback ran, not what it returned,
     * so the result is read from the flag the callback sets. */
    return nn20clock_app_is_running(pthis) ? ESP_OK : ESP_FAIL;
}

static int stop_private(nn20_worker_ctx *worker, void *user_data)
{
    (void)worker;
    NN20ClockApp *pthis = user_data;

    /* Reverse of start: the manager stops subscribing before the Timer
     * that feeds it goes quiet, and storage stays up until both are
     * done with it. Each is best-effort - a failure to stop is logged,
     * never a reason to leave the rest running. */
#if defined(ESP_PLATFORM)
    /* The server before the network it runs on, and before the card its
     * transfers touch. */
    if (nn20clock_ftp_is_running(pthis->ftp)) {
        (void)nn20clock_ftp_stop(pthis->ftp);
    }
    if (nn20clock_net_state(pthis->net) != NN20CLOCK_NET_DISABLED) {
        (void)nn20clock_net_stop(pthis->net);
    }
#endif
    if (nn20clock_manager_is_running(pthis->manager)) {
        (void)nn20clock_manager_stop(pthis->manager);
    }
#if defined(ESP_PLATFORM)
    /*
     * Before the Timer stops, and before anything this callback reaches
     * is torn down. unsubscribe() waits for any callback already in
     * flight, which is the guarantee that matters here.
     */
    (void)nn20clock_timer_unsubscribe(pthis->timer,
                                      &pthis->brightness_subscriber);
#endif
    if (nn20clock_timer_is_running(pthis->timer)) {
        (void)nn20clock_timer_stop(pthis->timer);
    }
#if defined(ESP_PLATFORM)
    /* Before the display: the player draws on it and the codec sits on
     * its I2C bus. */
    if (nn20clock_player_is_running(pthis->player)) {
        (void)nn20clock_player_stop(pthis->player);
    }
    if (nn20clock_audio_is_running(pthis->audio)) {
        (void)nn20clock_audio_stop(pthis->audio);
    }
    if (nn20clock_display_is_running(pthis->display)) {
        (void)nn20clock_display_stop(pthis->display);
    }
    if (nn20clock_sd_is_mounted(pthis->sd)) {
        (void)nn20clock_sd_unmount(pthis->sd);
    }
#endif

    atomic_store_explicit(&pthis->running, false, memory_order_release);
    ESP_LOGI(TAG, "stopped");
    return 0;
}

esp_err_t nn20clock_app_stop(NN20ClockApp *pthis)
{
    if (pthis == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!nn20clock_app_is_running(pthis)) {
        return ESP_ERR_INVALID_STATE;
    }
    return post_result(nn20_worker_post_sync(
        nn20clock_workers_core(pthis->workers), stop_private, pthis));
}

bool nn20clock_app_is_running(const NN20ClockApp *pthis)
{
    return pthis != NULL &&
           atomic_load_explicit(&pthis->running, memory_order_acquire);
}

/* --------------------------------------------------------- services -- */

NN20ClockWorkers *nn20clock_app_workers(const NN20ClockApp *pthis)
{
    return (pthis != NULL) ? pthis->workers : NULL;
}

NN20ClockStorage *nn20clock_app_storage(const NN20ClockApp *pthis)
{
    return (pthis != NULL) ? pthis->storage : NULL;
}

NN20ClockTimer *nn20clock_app_timer(const NN20ClockApp *pthis)
{
    return (pthis != NULL) ? pthis->timer : NULL;
}

NN20ClockManager *nn20clock_app_manager(const NN20ClockApp *pthis)
{
    return (pthis != NULL) ? pthis->manager : NULL;
}

#if defined(ESP_PLATFORM)
NN20ClockDisplay *nn20clock_app_display(const NN20ClockApp *pthis)
{
    return (pthis != NULL) ? pthis->display : NULL;
}

NN20ClockNet *nn20clock_app_net(const NN20ClockApp *pthis)
{
    return (pthis != NULL) ? pthis->net : NULL;
}

NN20ClockSd *nn20clock_app_sd(const NN20ClockApp *pthis)
{
    return (pthis != NULL) ? pthis->sd : NULL;
}

NN20ClockFtp *nn20clock_app_ftp(const NN20ClockApp *pthis)
{
    return (pthis != NULL) ? pthis->ftp : NULL;
}
#endif

/* ------------------------------------------------------- supervision -- */

/*
 * The verdict travels in the request rather than in the return value:
 * post_sync reports whether the callback ran, not what it returned. The
 * caller blocks until this is done, so a plain struct on its stack is
 * all the storage it needs.
 */
typedef struct {
    NN20ClockApp *app;
    int problems;
} SuperviseRequest;

static int supervise_private(nn20_worker_ctx *worker, void *user_data)
{
    (void)worker;
    SuperviseRequest *request = user_data;
    NN20ClockApp *pthis = request->app;

    if (!nn20clock_workers_are_running(pthis->workers)) {
        ESP_LOGE(TAG, "a worker thread has stopped");
        request->problems++;
    }
    if (!nn20clock_timer_is_running(pthis->timer)) {
        ESP_LOGE(TAG, "Timer is not running");
        request->problems++;
    }
    if (!nn20clock_manager_is_running(pthis->manager)) {
        ESP_LOGE(TAG, "ClockManager is not running");
        request->problems++;
    }
    if (nn20clock_manager_state(pthis->manager) == NN20CLOCK_STATE_ERROR) {
        ESP_LOGW(TAG, "ClockManager is in ERROR");
        request->problems++;
    }
#if defined(ESP_PLATFORM)
    if (!nn20clock_display_is_running(pthis->display)) {
        ESP_LOGE(TAG, "Display is not running");
        request->problems++;
    }
    /*
     * A network that never connected is NOT a problem: design 14 has
     * fallbacks behind NTP and the clock is expected to run without a
     * radio. It is reported in the status line, not counted here.
     */
#endif

    /* Milestone 12 decides what to do about any of this. Reporting is
     * the whole job today, and a supervisor that quietly restarted
     * things now would hide the bugs this skeleton exists to find. */
    return (request->problems == 0) ? 0 : -1;
}

esp_err_t nn20clock_app_supervise(NN20ClockApp *pthis)
{
    if (pthis == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!nn20clock_app_is_running(pthis)) {
        return ESP_ERR_INVALID_STATE;
    }

    SuperviseRequest request = { .app = pthis, .problems = 0 };
    const esp_err_t posted = post_result(nn20_worker_post_sync(
        nn20clock_workers_core(pthis->workers), supervise_private, &request));
    if (posted != ESP_OK) {
        return posted;
    }
    return (request.problems == 0) ? ESP_OK : ESP_FAIL;
}

bool nn20clock_app_is_playing(const NN20ClockApp *pthis)
{
    if (pthis == NULL) {
        return false;
    }
#if defined(ESP_PLATFORM)
    return nn20clock_player_is_playing(pthis->player);
#else
    return false;
#endif
}

void nn20clock_app_log_status(const NN20ClockApp *pthis)
{
    if (pthis == NULL) {
        return;
    }

    ESP_LOGI(TAG, "state %s, %u rejected transition(s), %u timer event(s)",
             nn20clock_manager_state_name(
                 nn20clock_manager_state(pthis->manager)),
             (unsigned)nn20clock_manager_rejected_transitions(pthis->manager),
             (unsigned)nn20clock_manager_timer_event_count(pthis->manager));
    ESP_LOGI(TAG, "%u alarm(s) scheduled, %u fired, %u missed",
             (unsigned)nn20clock_timer_alarm_count(pthis->timer),
             (unsigned)nn20clock_manager_alarm_count(pthis->manager),
             (unsigned)nn20clock_timer_missed_alarm_count(pthis->timer));
    ESP_LOGI(TAG, "storage %s, timer %s, manager %s",
             nn20clock_storage_is_ready(pthis->storage) ? "ready" : "down",
             nn20clock_timer_is_running(pthis->timer) ? "running" : "stopped",
             nn20clock_manager_is_running(pthis->manager) ? "running"
                                                          : "stopped");
#if defined(ESP_PLATFORM)
    if (nn20clock_sd_is_mounted(pthis->sd)) {
        uint64_t total = 0;
        uint64_t free_bytes = 0;
        (void)nn20clock_sd_usage(pthis->sd, &total, &free_bytes);
        ESP_LOGI(TAG, "SD %llu/%llu MB free; FTP %s, %u client(s), "
                      "%u up / %u down",
                 free_bytes / (1024ULL * 1024ULL),
                 total / (1024ULL * 1024ULL),
                 nn20clock_ftp_is_running(pthis->ftp) ? "running" : "stopped",
                 (unsigned)nn20clock_ftp_session_count(pthis->ftp),
                 (unsigned)nn20clock_ftp_uploads(pthis->ftp),
                 (unsigned)nn20clock_ftp_downloads(pthis->ftp));
    } else {
        ESP_LOGI(TAG, "no SD card");
    }
    ESP_LOGI(TAG, "display %s, %" PRIu32 " flush(es); net %s, clock %s",
             nn20clock_display_is_running(pthis->display) ? "running"
                                                          : "stopped",
             nn20clock_display_flush_count(pthis->display),
             nn20clock_net_state_name(nn20clock_net_state(pthis->net)),
             nn20clock_timer_is_time_synced(pthis->timer) ? "synced"
                                                          : "NOT SET");
#endif
    nn20clock_workers_log_stats(pthis->workers);
}
