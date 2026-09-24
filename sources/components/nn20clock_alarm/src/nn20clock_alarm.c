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
 * nn20clock_alarm.c - the alarm model and its occurrence arithmetic.
 *
 * All of it is pure: same inputs, same answer, no clock read, no state.
 * The Timer supplies the interval and this decides what fires in it.
 */
#include "nn20clock_alarm.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "NN20CLOCK_ALARM";

/*
 * How far ahead nn20clock_alarm_next_occurrence() will look for a
 * recurrent alarm. A weekly schedule repeats within 7 days, so 8 covers
 * every case with a day to spare for a DST shift; without a bound, an
 * alarm with no weekdays set would loop forever.
 */
#define SEARCH_DAYS 8

/* Deliberately no SECONDS_PER_DAY constant: days are not all 86400
 * seconds long, and every occurrence here is built through local
 * broken-down time precisely so that never has to be assumed. */

/* ------------------------------------------------------------ model -- */

esp_err_t nn20clock_alarm_defaults(NN20ClockAlarmConfig *out_alarm)
{
    if (out_alarm == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out_alarm, 0, sizeof(*out_alarm));
    out_alarm->id = NN20CLOCK_ALARM_ID_NONE;
    out_alarm->enabled = true;
    out_alarm->kind = NN20CLOCK_ALARM_KIND_RECURRENT;
    out_alarm->hour = 7u;
    out_alarm->minute = 0u;
    out_alarm->weekdays_mask = NN20CLOCK_ALARM_WEEKDAYS;
    out_alarm->media_mode = NN20CLOCK_ALARM_MEDIA_AUDIO;
    out_alarm->snooze_minutes = NN20CLOCK_ALARM_DEFAULT_SNOOZE_MINUTES;
    return ESP_OK;
}

const char *nn20clock_alarm_kind_name(NN20ClockAlarmKind kind)
{
    switch (kind) {
    case NN20CLOCK_ALARM_KIND_ONE_OFF:   return "one-off";
    case NN20CLOCK_ALARM_KIND_RECURRENT: return "recurrent";
    }
    return "unknown";
}

static bool date_is_plausible(const NN20ClockDate *date)
{
    /* Not a full calendar check - mktime normalizes the rest, and a
     * 31st of February would simply resolve to March. The point is to
     * reject values that are obviously not a date at all. */
    return date->year >= 1970u && date->year <= 2200u &&
           date->month >= 1u && date->month <= 12u &&
           date->day >= 1u && date->day <= 31u;
}

esp_err_t nn20clock_alarm_validate(const NN20ClockAlarmConfig *alarm)
{
    if (alarm == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (alarm->hour > 23u || alarm->minute > 59u) {
        ESP_LOGE(TAG, "alarm %u: %02u:%02u is not a time",
                 (unsigned)alarm->id, (unsigned)alarm->hour,
                 (unsigned)alarm->minute);
        return ESP_ERR_INVALID_ARG;
    }

    switch (alarm->kind) {
    case NN20CLOCK_ALARM_KIND_RECURRENT:
        /* A recurrent alarm with no days never fires. Storing one is a
         * silent way to have no alarm at all, so it is refused. */
        if ((alarm->weekdays_mask & NN20CLOCK_ALARM_EVERY_DAY) == 0u) {
            ESP_LOGE(TAG, "alarm %u: recurrent with no weekdays",
                     (unsigned)alarm->id);
            return ESP_ERR_INVALID_ARG;
        }
        break;
    case NN20CLOCK_ALARM_KIND_ONE_OFF:
        if (!date_is_plausible(&alarm->one_off_date)) {
            ESP_LOGE(TAG, "alarm %u: one-off date %04u-%02u-%02u",
                     (unsigned)alarm->id, (unsigned)alarm->one_off_date.year,
                     (unsigned)alarm->one_off_date.month,
                     (unsigned)alarm->one_off_date.day);
            return ESP_ERR_INVALID_ARG;
        }
        /* Design 10: weekdays either ignored or required to be zero for
         * a one-off. Required, so the record has one meaning. */
        if (alarm->weekdays_mask != 0u) {
            ESP_LOGE(TAG, "alarm %u: one-off with weekdays set",
                     (unsigned)alarm->id);
            return ESP_ERR_INVALID_ARG;
        }
        break;
    default:
        ESP_LOGE(TAG, "alarm %u: unknown kind %d", (unsigned)alarm->id,
                 (int)alarm->kind);
        return ESP_ERR_INVALID_ARG;
    }

    switch (alarm->media_mode) {
    case NN20CLOCK_ALARM_MEDIA_VIDEO:
    case NN20CLOCK_ALARM_MEDIA_AUDIO:
    case NN20CLOCK_ALARM_MEDIA_RANDOM_SET:
        break;
    default:
        /* Not cosmetic: the firing path switches on this to decide
         * whether media_path names the file or is to be ignored, and a
         * value from neither branch would silently take one of them. */
        ESP_LOGE(TAG, "alarm %u: unknown media mode %d", (unsigned)alarm->id,
                 (int)alarm->media_mode);
        return ESP_ERR_INVALID_ARG;
    }

    /* Must be NUL-terminated inside the field or every later use is a
     * buffer overrun waiting to happen. Checked before the path rules
     * below, which all take a C string. */
    if (memchr(alarm->media_path, '\0', sizeof(alarm->media_path)) == NULL ||
        memchr(alarm->media_set_id, '\0', sizeof(alarm->media_set_id))
            == NULL) {
        ESP_LOGE(TAG, "alarm %u: unterminated media string",
                 (unsigned)alarm->id);
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * The paths, against the same rules the card and the FTP server
     * enforce - nn20clock_media owns them, and an alarm that stored
     * something those two would refuse is an alarm that rings the tone
     * for a reason nobody can see.
     *
     * Which field means anything follows the mode, and the other is
     * left alone rather than required to be empty: the editor clears
     * it, but a record that carries a stale name in an ignored field is
     * still a record the scheduler can evaluate.
     */
    if (alarm->media_mode == NN20CLOCK_ALARM_MEDIA_RANDOM_SET) {
        /* "" is the root folder, and is the only empty path that means a
         * place rather than nothing. */
        if (!nn20clock_media_path_is_safe(alarm->media_set_id,
                                          NN20CLOCK_MEDIA_PATH_FOLDER)) {
            ESP_LOGE(TAG, "alarm %u: media set is not a folder on this card",
                     (unsigned)alarm->id);
            return ESP_ERR_INVALID_ARG;
        }
    } else if (alarm->media_path[0] != '\0' &&
               !nn20clock_media_path_is_safe(alarm->media_path,
                                             NN20CLOCK_MEDIA_PATH_FILE)) {
        /* Empty is the built-in tone, which is always available and is
         * not a path at all. */
        ESP_LOGE(TAG, "alarm %u: media path is not a clip on this card",
                 (unsigned)alarm->id);
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

/* ------------------------------------------------------- occurrences -- */

/*
 * The instant of `hour:minute` on the local calendar day containing
 * `day_anchor`, offset by `day_offset` days.
 *
 * Built through localtime_r/mktime rather than by adding 86400-second
 * multiples, because days are not all 86400 seconds long: across a DST
 * change one is 23 hours and another 25. Going via broken-down local
 * time keeps an alarm at the wall-clock time the user set.
 *
 * tm_isdst = -1 asks libc to work out whether DST applies on the
 * resulting day, which is the whole point.
 */
static time_t local_instant(time_t day_anchor, int day_offset, uint8_t hour,
                            uint8_t minute)
{
    struct tm local = {0};
    if (localtime_r(&day_anchor, &local) == NULL) {
        return (time_t)0;
    }

    local.tm_mday += day_offset;
    local.tm_hour = (int)hour;
    local.tm_min = (int)minute;
    local.tm_sec = 0;
    local.tm_isdst = -1;   /* let libc decide for that day */

    /* mktime normalizes an out-of-range tm_mday, which is how the day
     * offset crosses month and year boundaries for free. */
    return mktime(&local);
}

/* 0 = Monday, matching design 10's mask and NN20ClockDateTime. */
static int weekday_of(time_t when)
{
    struct tm local = {0};
    if (localtime_r(&when, &local) == NULL) {
        return -1;
    }
    return (local.tm_wday + 6) % 7;
}

static bool fires_on_weekday(const NN20ClockAlarmConfig *alarm, int weekday)
{
    if (weekday < 0 || weekday > 6) {
        return false;
    }
    return (alarm->weekdays_mask & (1u << weekday)) != 0u;
}

static time_t one_off_instant(const NN20ClockAlarmConfig *alarm)
{
    struct tm local = {0};
    local.tm_year = (int)alarm->one_off_date.year - 1900;
    local.tm_mon = (int)alarm->one_off_date.month - 1;
    local.tm_mday = (int)alarm->one_off_date.day;
    local.tm_hour = (int)alarm->hour;
    local.tm_min = (int)alarm->minute;
    local.tm_sec = 0;
    local.tm_isdst = -1;
    return mktime(&local);
}

esp_err_t nn20clock_alarm_next_occurrence(const NN20ClockAlarmConfig *alarm,
                                          time_t from, time_t *out_when)
{
    if (alarm == NULL || out_when == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_when = (time_t)0;
    if (!alarm->enabled) {
        return ESP_OK;   /* never; not an error */
    }

    if (alarm->kind == NN20CLOCK_ALARM_KIND_ONE_OFF) {
        const time_t when = one_off_instant(alarm);
        if (when != (time_t)-1 && when >= from) {
            *out_when = when;
        }
        /* A one-off in the past has no next occurrence. Design 10 has
         * the ClockManager delete it once it has fired. */
        return ESP_OK;
    }

    for (int offset = 0; offset < SEARCH_DAYS; offset++) {
        const time_t candidate = local_instant(from, offset, alarm->hour,
                                               alarm->minute);
        if (candidate == (time_t)-1) {
            continue;
        }
        if (candidate < from) {
            continue;   /* today's time already passed */
        }
        /* The weekday of the candidate, not of `from`: a day offset can
         * cross midnight, and after a DST shift the two can differ. */
        if (fires_on_weekday(alarm, weekday_of(candidate))) {
            *out_when = candidate;
            return ESP_OK;
        }
    }

    /* Only reachable for a recurrent alarm with no weekdays set, which
     * validation refuses to store. */
    return ESP_OK;
}

bool nn20clock_alarm_occurs_in(const NN20ClockAlarmConfig *alarm,
                               time_t after, time_t until, time_t *out_when)
{
    if (alarm == NULL || out_when == NULL) {
        return false;
    }
    if (until <= after) {
        /* Nothing elapsed, or the clock moved backwards - a correction,
         * not the passage of time. Either way, nothing occurred. */
        return false;
    }
    if (!alarm->enabled) {
        return false;
    }

    /*
     * The first occurrence strictly after `after`. next_occurrence()
     * answers "at or after", so ask from the following second: the
     * interval is half-open at the bottom, because `after` was already
     * covered by the previous tick and must not fire twice.
     */
    time_t when = (time_t)0;
    if (nn20clock_alarm_next_occurrence(alarm, after + 1, &when) != ESP_OK) {
        return false;
    }
    if (when == (time_t)0 || when > until) {
        return false;
    }

    *out_when = when;
    return true;
}

/* ------------------------------------------------------------- list -- */

esp_err_t nn20clock_alarm_list_find(const NN20ClockAlarmList *list,
                                    uint32_t id, size_t *out_index)
{
    if (list == NULL || id == NN20CLOCK_ALARM_ID_NONE) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < list->count; i++) {
        if (list->alarms[i].id == id) {
            if (out_index != NULL) {
                *out_index = i;
            }
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

/* Lowest unused id from 1 up, so ids stay small and readable rather
 * than climbing forever. */
static uint32_t next_free_id(const NN20ClockAlarmList *list)
{
    for (uint32_t candidate = 1u; candidate <= NN20CLOCK_ALARM_MAX + 1u;
         candidate++) {
        if (nn20clock_alarm_list_find(list, candidate, NULL)
            == ESP_ERR_NOT_FOUND) {
            return candidate;
        }
    }
    return NN20CLOCK_ALARM_ID_NONE;   /* unreachable while the list fits */
}

esp_err_t nn20clock_alarm_list_put(NN20ClockAlarmList *list,
                                   const NN20ClockAlarmConfig *alarm,
                                   uint32_t *out_id)
{
    if (list == NULL || alarm == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const esp_err_t valid = nn20clock_alarm_validate(alarm);
    if (valid != ESP_OK) {
        return valid;
    }

    size_t index = 0;
    if (alarm->id != NN20CLOCK_ALARM_ID_NONE &&
        nn20clock_alarm_list_find(list, alarm->id, &index) == ESP_OK) {
        list->alarms[index] = *alarm;   /* replace in place */
        if (out_id != NULL) {
            *out_id = alarm->id;
        }
        return ESP_OK;
    }

    if (list->count >= NN20CLOCK_ALARM_MAX) {
        ESP_LOGE(TAG, "alarm list full (%d)", NN20CLOCK_ALARM_MAX);
        return ESP_ERR_NO_MEM;
    }

    NN20ClockAlarmConfig *slot = &list->alarms[list->count];
    *slot = *alarm;
    if (slot->id == NN20CLOCK_ALARM_ID_NONE) {
        slot->id = next_free_id(list);
    }
    list->count++;

    if (out_id != NULL) {
        *out_id = slot->id;
    }
    return ESP_OK;
}

esp_err_t nn20clock_alarm_list_remove(NN20ClockAlarmList *list, uint32_t id)
{
    size_t index = 0;
    const esp_err_t found = nn20clock_alarm_list_find(list, id, &index);
    if (found != ESP_OK) {
        return found;
    }

    /* Order is not part of the contract, so fill the hole with the last
     * entry rather than shifting the tail down. */
    list->alarms[index] = list->alarms[list->count - 1];
    memset(&list->alarms[list->count - 1], 0, sizeof(list->alarms[0]));
    list->count--;
    return ESP_OK;
}

bool nn20clock_alarm_list_first_in(const NN20ClockAlarmList *list,
                                   time_t after, time_t until,
                                   uint32_t *out_id, time_t *out_when)
{
    if (list == NULL || out_id == NULL || out_when == NULL) {
        return false;
    }

    bool found = false;
    time_t earliest = (time_t)0;
    uint32_t earliest_id = NN20CLOCK_ALARM_ID_NONE;

    for (size_t i = 0; i < list->count; i++) {
        time_t when = (time_t)0;
        if (!nn20clock_alarm_occurs_in(&list->alarms[i], after, until, &when)) {
            continue;
        }
        /* Earliest wins. Two alarms at the same instant is a tie broken
         * by list order, which is arbitrary but stable - and ringing one
         * of two simultaneous alarms is all a single speaker can do. */
        if (!found || when < earliest) {
            found = true;
            earliest = when;
            earliest_id = list->alarms[i].id;
        }
    }

    if (found) {
        *out_id = earliest_id;
        *out_when = earliest;
    }
    return found;
}
