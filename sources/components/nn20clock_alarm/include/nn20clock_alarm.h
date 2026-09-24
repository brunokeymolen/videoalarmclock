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
 * nn20clock_alarm.h - the alarm model and when it fires (design 10).
 *
 * Pure logic, no state, no threads: an alarm record, and functions that
 * answer "does this alarm go off in this span of time". That is what
 * makes the firing rules - the part of this clock that is actually hard
 * to get right - testable exhaustively on a desktop in milliseconds
 * rather than by waiting for 7am.
 *
 * ------------------------------------------------------------------
 * Why occurrences are matched over an interval, not against an instant
 * ------------------------------------------------------------------
 *
 * The obvious implementation - fire when now == alarm_time - is wrong.
 * A poll can be late: a busy worker, a slow SD read, or a scheduler
 * hiccup can take the clock from 01:02:02 to 01:02:04, and an alarm at
 * 01:02:03 would never be seen at all.
 *
 * So the Timer asks a different question on every tick: which alarms
 * occur in (after, until] - the span since the previous reading? The
 * range is half-open deliberately. `after` was covered by the previous
 * tick and `until` is covered by this one, so every instant is examined
 * exactly once: no gaps, and no alarm can fire twice from the tick
 * itself.
 *
 * That leaves two cases the interval cannot decide on its own, and the
 * Timer handles both:
 *
 *   Lateness. An interval can be long - the device was busy, or the
 *   clock was stepped. An alarm found deep in the past should not ring
 *   at the wrong time of day, so the Timer applies a grace window and
 *   reports anything older as missed.
 *
 *   Clock steps. When SNTP corrects the clock, the span between the old
 *   and new readings is not elapsed time and contains no real
 *   occurrences. The Timer re-baselines instead of scanning it.
 */
#ifndef NN20CLOCK_ALARM_H
#define NN20CLOCK_ALARM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "nn20clock_media.h"
#include "nn20clock_platform.h"
#include "nn20clock_time.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------ model -- */

typedef enum {
    NN20CLOCK_ALARM_KIND_ONE_OFF,
    NN20CLOCK_ALARM_KIND_RECURRENT
} NN20ClockAlarmKind;

/*
 * Which of the record's media fields means anything.
 *
 * VIDEO and AUDIO both mean "the file at media_path", or the built-in
 * tone when that is empty; nothing distinguishes them today, because
 * what a file turns out to be is settled by opening it and not by what
 * an alarm claimed when it was saved.
 *
 * RANDOM_SET means "not this file, one chosen when the alarm goes off"
 * - design 13's rule that the selection happens at firing time rather
 * than at saving time. media_set_id is the FOLDER to choose from, and
 * media_path is ignored. An empty media_set_id is the root folder, which
 * is also what an alarm saved before folders existed says, so those keep
 * meaning exactly what they meant: choose from the root of the card.
 *
 * Random never recurses. "morning" means the clips directly in
 * "morning", not the ones in "morning/kids" - a folder is a choice the
 * user made, and quietly widening it is not.
 *
 * The built-in tone is never in the pool. It is design 13's fallback
 * for when media cannot be played, and an alarm set to choose between
 * films should not sometimes beep instead.
 */
typedef enum {
    NN20CLOCK_ALARM_MEDIA_VIDEO,
    NN20CLOCK_ALARM_MEDIA_AUDIO,
    NN20CLOCK_ALARM_MEDIA_RANDOM_SET
} NN20ClockAlarmMediaMode;

/* Design 10's bit layout: bit 0 is Monday, matching
 * NN20ClockDateTime::weekday so a weekday indexes the mask directly. */
#define NN20CLOCK_ALARM_MONDAY    (1u << 0)
#define NN20CLOCK_ALARM_TUESDAY   (1u << 1)
#define NN20CLOCK_ALARM_WEDNESDAY (1u << 2)
#define NN20CLOCK_ALARM_THURSDAY  (1u << 3)
#define NN20CLOCK_ALARM_FRIDAY    (1u << 4)
#define NN20CLOCK_ALARM_SATURDAY  (1u << 5)
#define NN20CLOCK_ALARM_SUNDAY    (1u << 6)
#define NN20CLOCK_ALARM_WEEKDAYS  0x1Fu   /* Mon-Fri */
#define NN20CLOCK_ALARM_EVERY_DAY 0x7Fu

/*
 * A relative file path from the card's root: "wake.avi",
 * "morning/wake.avi". Equal to NN20CLOCK_MEDIA_PATH_MAX on purpose, so
 * every path the picker can browse to is one an alarm can remember;
 * the static assert below is what keeps the two from drifting.
 */
#define NN20CLOCK_ALARM_MEDIA_PATH_MAX 256

/*
 * A relative FOLDER path, for RANDOM_SET. Smaller than the file path, and
 * deliberately not grown: this is a stored blob whose size is its
 * schema (see nn20clock_storage.c), and 63 characters of folder path is
 * already more nesting than a bedside clock has any use for. The alarm
 * editor checks a folder against this before offering <random> in it,
 * rather than saving something that cannot be read back.
 */
#define NN20CLOCK_ALARM_MEDIA_SET_MAX  64

/* An id of 0 is "unassigned"; storage allocates from 1 up, so a zeroed
 * record is never mistaken for a real alarm. */
#define NN20CLOCK_ALARM_ID_NONE 0u

/* Nine minutes, which is what bedside clocks have offered since
 * mechanical ones did it because of gear ratios. Per-alarm, in
 * snooze_minutes; this is only the default and the fallback. */
#define NN20CLOCK_ALARM_DEFAULT_SNOOZE_MINUTES 9u

typedef struct {
    uint32_t id;
    bool enabled;
    NN20ClockAlarmKind kind;
    uint8_t hour;              /* 0-23 */
    uint8_t minute;            /* 0-59 */
    uint8_t weekdays_mask;     /* recurrent only; 0 for one-off */
    NN20ClockDate one_off_date;/* one-off only */
    NN20ClockAlarmMediaMode media_mode;
    /* A safe relative file path, or "" for the built-in tone. */
    char media_path[NN20CLOCK_ALARM_MEDIA_PATH_MAX];
    /* A safe relative folder path for RANDOM_SET; "" is the root folder. */
    char media_set_id[NN20CLOCK_ALARM_MEDIA_SET_MAX];
    /*
     * No per-alarm volume. Design 9's schema had one and design 11
     * listed "Set volume" under the alarm editor, but there is one
     * speaker in one room, so there is one volume: the device's, in
     * nn20clock_storage.h's NN20ClockConfig::volume. An alarm rings at
     * whatever the user last set there.
     */
    uint16_t snooze_minutes;
} NN20ClockAlarmConfig;

/* Design 17 caps nothing explicitly; 16 is well past what a bedside
 * clock needs and keeps the whole list a fixed, flash-friendly size. */
#define NN20CLOCK_ALARM_MAX 16

typedef struct {
    NN20ClockAlarmConfig alarms[NN20CLOCK_ALARM_MAX];
    size_t count;
} NN20ClockAlarmList;

/* Sensible blank alarm: enabled, 07:00, weekdays, audio. Callers edit
 * from here rather than from a zeroed struct, which would be a disabled
 * one-off at midnight with no date. */
esp_err_t nn20clock_alarm_defaults(NN20ClockAlarmConfig *out_alarm);

/*
 * ESP_ERR_INVALID_ARG with a specific complaint logged for: an hour or
 * minute out of range, a recurrent alarm with no weekdays selected, a
 * one-off with an impossible date, an unterminated media string, or a
 * media path that is not a safe relative path under the card's root.
 * Called before anything is stored, so a record that reaches flash is
 * one the scheduler can evaluate.
 *
 * Note what this is NOT: it does not ask the card whether the file is
 * there. Media comes and goes over FTP, and an alarm whose clip was
 * deleted must still ring - it falls back to the built-in tone at
 * firing time. Validation is about what the record MEANS, not about
 * what happens to be on the card this minute.
 */
esp_err_t nn20clock_alarm_validate(const NN20ClockAlarmConfig *alarm);

const char *nn20clock_alarm_kind_name(NN20ClockAlarmKind kind);

/* ------------------------------------------------------- occurrences -- */

/*
 * The first instant at or after `from` when this alarm goes off, or 0
 * if it never will - a disabled alarm, or a one-off whose date has
 * passed.
 *
 * Local time, via the timezone currently in force, so an alarm set for
 * 07:00 stays at 07:00 across a DST change rather than drifting an hour.
 */
esp_err_t nn20clock_alarm_next_occurrence(const NN20ClockAlarmConfig *alarm,
                                          time_t from, time_t *out_when);

/*
 * Does this alarm occur in the half-open interval (after, until]?
 *
 * This is the question the tick asks. See the header comment for why it
 * is an interval: matching an instant loses any alarm whose second the
 * clock skipped over.
 *
 * Returns true and sets *out_when to the occurrence. When more than one
 * occurrence falls inside - possible only for a very long interval -
 * the EARLIEST is reported, because that is the one whose lateness the
 * caller must judge.
 *
 * `until` before `after` is not an error and not an occurrence: the
 * clock went backwards, which means it was corrected, not that time
 * passed.
 */
bool nn20clock_alarm_occurs_in(const NN20ClockAlarmConfig *alarm,
                               time_t after, time_t until, time_t *out_when);

/* ------------------------------------------------------------- list -- */

/* ESP_ERR_NOT_FOUND if no alarm has this id. */
esp_err_t nn20clock_alarm_list_find(const NN20ClockAlarmList *list,
                                    uint32_t id, size_t *out_index);

/*
 * Add or replace by id. An id of NN20CLOCK_ALARM_ID_NONE is assigned the
 * lowest free id, so a caller adding an alarm does not have to know what
 * is already stored. ESP_ERR_NO_MEM when the list is full.
 */
esp_err_t nn20clock_alarm_list_put(NN20ClockAlarmList *list,
                                   const NN20ClockAlarmConfig *alarm,
                                   uint32_t *out_id);

/* ESP_ERR_NOT_FOUND if the id is not present. */
esp_err_t nn20clock_alarm_list_remove(NN20ClockAlarmList *list, uint32_t id);

/*
 * The earliest alarm occurring in (after, until], across the whole list,
 * skipping disabled ones. Returns false when none do.
 *
 * This is what the Timer calls on every tick.
 */
bool nn20clock_alarm_list_first_in(const NN20ClockAlarmList *list,
                                   time_t after, time_t until,
                                   uint32_t *out_id, time_t *out_when);

#ifdef __cplusplus
}
#endif

#endif /* NN20CLOCK_ALARM_H */
