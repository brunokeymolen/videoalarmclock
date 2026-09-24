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
 * Component tests for the Storage facade (design 9).
 *
 * Milestone 1 scope: the worker seam and the config record. Nothing here
 * asserts that anything is persisted, because nothing is yet - that is
 * what nn20clock_storage_is_persistent() reports, and there is a test
 * for the report rather than for a promise the component does not keep.
 */
#include "test_util.h"


/*
 * A namespace of the suite's own.
 *
 * On the board these tests write to real flash, which is the point of
 * running them there - but writing to the product's namespace means a
 * test run reconfigures the actual clock. One did: a test stored a fake
 * network, and the device came up afterwards trying to join it, with no
 * Wi-Fi and no NTP.
 */
#define TEST_NAMESPACE "nn20clock_t"

#include "nn20clock_storage.h"

/*
 * Wait for everything already posted to a worker to have run. post_sync
 * returns only after its own callback has, and the worker is FIFO, so
 * this is a barrier - no sleeps, no polling, no flakiness.
 */
static int barrier_cb(nn20_worker_ctx *worker, void *user_data)
{
    (void)worker;
    (void)user_data;
    return 0;
}

static void flush(nn20_worker_ctx *worker)
{
    (void)nn20_worker_post_sync(worker, barrier_cb, NULL);
}

typedef struct {
    NN20ClockStorage *storage;
    nn20_worker_ctx *worker;
} Fixture;

/*
 * Bring the storage back to a known state.
 *
 * This matters only on the target, and it is the difference between the
 * two builds rather than a quirk of the tests: on the host the records
 * live in RAM and every fixture starts blank, while on the board they
 * are in flash and outlive the instance that wrote them - which is the
 * entire point of persistence. A test that assumed an empty store
 * passed on a desktop and failed on hardware.
 *
 * Uses only the public API, so it exercises the same paths everything
 * else does.
 */
static void reset_storage(Fixture *fixture)
{
    static NN20ClockAlarmList list;
    if (nn20clock_storage_list_alarms(fixture->storage, &list) == ESP_OK) {
        /* Remove by id read from a snapshot: deleting rearranges the
         * list, so iterating it while deleting would skip entries. */
        while (list.count > 0u) {
            (void)nn20clock_storage_delete_alarm(fixture->storage,
                                                 list.alarms[0].id);
            if (nn20clock_storage_list_alarms(fixture->storage, &list)
                != ESP_OK) {
                break;
            }
        }
    }

    NN20ClockConfig defaults;
    if (nn20clock_storage_default_config(&defaults) == ESP_OK) {
        (void)nn20clock_storage_save_config(fixture->storage, &defaults);
    }
}

static Fixture fixture_up(void)
{
    Fixture fixture = {0};
    fixture.worker = nn20_worker_create();
    if (fixture.worker != NULL) {
        fixture.storage = nn20clock_storage_ctor_named(fixture.worker, TEST_NAMESPACE);
    }
    return fixture;
}

/* Started and wiped clean - what most cases below want. */
static Fixture fixture_up_started(void)
{
    Fixture fixture = fixture_up();
    if (fixture.storage != NULL &&
        nn20clock_storage_start(fixture.storage) == ESP_OK) {
        reset_storage(&fixture);
    }
    return fixture;
}

static void fixture_down(Fixture *fixture)
{
    nn20clock_storage_dtor(fixture->storage);
    nn20_worker_delete(fixture->worker);
}

/* --------------------------------------------------------- lifecycle -- */

/* Design 4 says the StorageWorker is passed in. A Storage without one
 * has nowhere to run, so this must fail loudly rather than quietly
 * doing its work on the caller's thread. */
TEST(ctor_requires_a_worker)
{
    CHECK(nn20clock_storage_ctor(NULL) == NULL);
}

TEST(dtor_tolerates_null)
{
    nn20clock_storage_dtor(NULL);
}

TEST(start_makes_it_ready)
{
    Fixture fixture = fixture_up();
    REQUIRE(fixture.storage != NULL);

    CHECK(!nn20clock_storage_is_ready(fixture.storage));
    CHECK_EQ(ESP_OK, nn20clock_storage_start(fixture.storage));
    CHECK(nn20clock_storage_is_ready(fixture.storage));

    /* Second start is a caller bug, not a no-op: it would re-open the
     * backing store under live readers once NVS lands. */
    CHECK_EQ(ESP_ERR_INVALID_STATE, nn20clock_storage_start(fixture.storage));

    fixture_down(&fixture);
}

/* Milestone 1 keeps the config in RAM only. Callers deciding whether a
 * setting will survive a power cycle need to be told the truth. */
TEST(is_not_persistent_yet)
{
    Fixture fixture = fixture_up();
    REQUIRE(fixture.storage != NULL);
    CHECK(!nn20clock_storage_is_persistent(fixture.storage));
    fixture_down(&fixture);
}

/* ------------------------------------------------------------ config -- */

TEST(default_config_is_in_range)
{
    NN20ClockConfig config;
    REQUIRE(nn20clock_storage_default_config(&config) == ESP_OK);

    CHECK_EQ(NN20CLOCK_STORAGE_SCHEMA_VERSION, config.schema_version);
    CHECK(config.brightness_day <= 100u);
    CHECK(config.brightness_night <= 100u);
    CHECK(config.brightness_playback_min <= 100u);
    CHECK(config.day_start_minutes < 1440u);
    CHECK(config.night_start_minutes < 1440u);
    /* The default has to actually have a night in it, or the feature
     * ships switched off for everyone who never opens the screen. */
    CHECK(config.brightness_night < config.brightness_day);
    CHECK(config.day_start_minutes != config.night_start_minutes);
    CHECK(config.volume <= 100u);
    CHECK(config.timezone[0] != '\0');
    /* On by default: a clock nobody has configured should get the right
     * time by itself. */
    CHECK(config.ntp_enabled);
    CHECK_EQ(0, config.last_known_time);   /* never synced */

    CHECK_EQ(ESP_ERR_INVALID_ARG, nn20clock_storage_default_config(NULL));
}

TEST(load_returns_the_defaults_before_anything_is_saved)
{
    Fixture fixture = fixture_up_started();
    REQUIRE(fixture.storage != NULL);

    NN20ClockConfig defaults;
    REQUIRE(nn20clock_storage_default_config(&defaults) == ESP_OK);

    NN20ClockConfig loaded;
    CHECK_EQ(ESP_OK, nn20clock_storage_load_config(fixture.storage, &loaded));
    CHECK_EQ(defaults.schema_version, loaded.schema_version);
    CHECK_EQ(defaults.brightness_day, loaded.brightness_day);
    CHECK_EQ(defaults.brightness_night, loaded.brightness_night);
    CHECK_EQ(defaults.day_start_minutes, loaded.day_start_minutes);
    CHECK_EQ(defaults.night_start_minutes, loaded.night_start_minutes);
    CHECK_EQ(defaults.volume, loaded.volume);
    CHECK_STR_EQ(defaults.timezone, loaded.timezone);

    fixture_down(&fixture);
}

TEST(save_then_load_round_trips)
{
    Fixture fixture = fixture_up_started();
    REQUIRE(fixture.storage != NULL);

    NN20ClockConfig config;
    REQUIRE(nn20clock_storage_default_config(&config) == ESP_OK);
    config.ntp_enabled = false;
    config.brightness_day = 75u;
    config.brightness_night = 25u;
    config.day_start_minutes = 6u * 60u + 30u;
    config.night_start_minutes = 23u * 60u;
    config.brightness_playback_min = 55u;
    config.volume = 90u;
    config.last_known_time = 1735689600;   /* 2025-01-01T00:00:00Z */
    snprintf(config.timezone, sizeof(config.timezone), "UTC0");

    CHECK_EQ(ESP_OK, nn20clock_storage_save_config(fixture.storage, &config));

    NN20ClockConfig loaded;
    CHECK_EQ(ESP_OK, nn20clock_storage_load_config(fixture.storage, &loaded));
    CHECK(!loaded.ntp_enabled);
    CHECK_EQ(75u, loaded.brightness_day);
    CHECK_EQ(25u, loaded.brightness_night);
    CHECK_EQ(6u * 60u + 30u, loaded.day_start_minutes);
    CHECK_EQ(23u * 60u, loaded.night_start_minutes);
    CHECK_EQ(55u, loaded.brightness_playback_min);
    CHECK_EQ(90u, loaded.volume);
    CHECK_EQ(1735689600, loaded.last_known_time);
    CHECK_STR_EQ("UTC0", loaded.timezone);

    fixture_down(&fixture);
}

/* The schema version describes the layout this build writes, so a caller
 * must not be able to talk Storage into claiming another one. */
TEST(save_forces_the_current_schema_version)
{
    Fixture fixture = fixture_up_started();
    REQUIRE(fixture.storage != NULL);

    NN20ClockConfig config;
    REQUIRE(nn20clock_storage_default_config(&config) == ESP_OK);
    config.schema_version = 999u;
    CHECK_EQ(ESP_OK, nn20clock_storage_save_config(fixture.storage, &config));

    NN20ClockConfig loaded;
    REQUIRE(nn20clock_storage_load_config(fixture.storage, &loaded) == ESP_OK);
    CHECK_EQ(NN20CLOCK_STORAGE_SCHEMA_VERSION, loaded.schema_version);

    fixture_down(&fixture);
}

TEST(save_rejects_out_of_range_values)
{
    Fixture fixture = fixture_up_started();
    REQUIRE(fixture.storage != NULL);

    NN20ClockConfig config;
    REQUIRE(nn20clock_storage_default_config(&config) == ESP_OK);
    const uint8_t original_brightness = config.brightness_day;

    config.brightness_day = 101u;
    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_storage_save_config(fixture.storage, &config));

    config.brightness_day = 50u;
    config.brightness_night = 101u;
    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_storage_save_config(fixture.storage, &config));

    config.brightness_night = 25u;
    config.brightness_playback_min = 101u;
    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_storage_save_config(fixture.storage, &config));
    config.brightness_playback_min = 0u;

    /* A time outside the day is not a time. Rejected here rather than
     * wrapped, because a stored 25:00 means something upstream computed
     * it wrong and quietly folding it hides that. */
    config.brightness_night = 25u;
    config.day_start_minutes = 1440u;
    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_storage_save_config(fixture.storage, &config));

    config.day_start_minutes = 420u;
    config.night_start_minutes = 5000u;
    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_storage_save_config(fixture.storage, &config));

    config.night_start_minutes = 1320u;
    config.volume = 200u;
    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_storage_save_config(fixture.storage, &config));

    /* A rejected save must leave the stored config alone, not half-apply
     * it. */
    NN20ClockConfig loaded;
    REQUIRE(nn20clock_storage_load_config(fixture.storage, &loaded) == ESP_OK);
    CHECK_EQ(original_brightness, loaded.brightness_day);

    fixture_down(&fixture);
}

/* An unterminated timezone would overrun the first strcpy that touched
 * it, so it is rejected at the boundary rather than trusted. */
TEST(save_rejects_an_unterminated_timezone)
{
    Fixture fixture = fixture_up_started();
    REQUIRE(fixture.storage != NULL);

    NN20ClockConfig config;
    REQUIRE(nn20clock_storage_default_config(&config) == ESP_OK);
    memset(config.timezone, 'X', sizeof(config.timezone));

    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_storage_save_config(fixture.storage, &config));

    fixture_down(&fixture);
}

TEST(config_calls_reject_null)
{
    Fixture fixture = fixture_up();
    REQUIRE(fixture.storage != NULL);

    NN20ClockConfig config;
    CHECK_EQ(ESP_ERR_INVALID_ARG, nn20clock_storage_load_config(NULL, &config));
    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_storage_load_config(fixture.storage, NULL));
    CHECK_EQ(ESP_ERR_INVALID_ARG, nn20clock_storage_save_config(NULL, &config));
    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_storage_save_config(fixture.storage, NULL));

    fixture_down(&fixture);
}

/* ------------------------------------------------------------- async -- */

typedef struct {
    int calls;
    esp_err_t status;
} DoneSink;

static void on_save_done(esp_err_t status, void *user_data)
{
    DoneSink *sink = user_data;
    sink->calls++;
    sink->status = status;
}

TEST(async_save_applies_and_calls_the_continuation)
{
    Fixture fixture = fixture_up_started();
    REQUIRE(fixture.storage != NULL);

    NN20ClockConfig config;
    REQUIRE(nn20clock_storage_default_config(&config) == ESP_OK);
    config.volume = 33u;

    DoneSink sink = {0};
    CHECK_EQ(ESP_OK, nn20clock_storage_save_config_async(
                         fixture.storage, &config, on_save_done, &sink));

    flush(fixture.worker);
    CHECK_EQ(1, sink.calls);
    CHECK_EQ(ESP_OK, sink.status);

    NN20ClockConfig loaded;
    REQUIRE(nn20clock_storage_load_config(fixture.storage, &loaded) == ESP_OK);
    CHECK_EQ(33u, loaded.volume);

    fixture_down(&fixture);
}

/* The caller's struct is copied into the request, so it may go out of
 * scope - or be reused - the instant the call returns. */
TEST(async_save_copies_the_caller_s_config)
{
    Fixture fixture = fixture_up_started();
    REQUIRE(fixture.storage != NULL);

    NN20ClockConfig config;
    REQUIRE(nn20clock_storage_default_config(&config) == ESP_OK);
    config.volume = 11u;
    CHECK_EQ(ESP_OK, nn20clock_storage_save_config_async(fixture.storage,
                                                         &config, NULL, NULL));

    /* Overwritten before the worker can possibly have run it. */
    config.volume = 99u;

    flush(fixture.worker);
    NN20ClockConfig loaded;
    REQUIRE(nn20clock_storage_load_config(fixture.storage, &loaded) == ESP_OK);
    CHECK_EQ(11u, loaded.volume);

    fixture_down(&fixture);
}

TEST(async_save_validates_before_queueing)
{
    Fixture fixture = fixture_up_started();
    REQUIRE(fixture.storage != NULL);

    NN20ClockConfig config;
    REQUIRE(nn20clock_storage_default_config(&config) == ESP_OK);
    config.volume = 101u;

    DoneSink sink = {0};
    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_storage_save_config_async(fixture.storage, &config,
                                                 on_save_done, &sink));
    flush(fixture.worker);
    /* Rejected up front, so the continuation must not fire at all - a
     * caller that only listens to the continuation would otherwise wait
     * forever. */
    CHECK_EQ(0, sink.calls);

    fixture_down(&fixture);
}

/* ------------------------------------------------------ alarms (M3) -- */

static NN20ClockAlarmConfig sample_alarm(uint8_t hour, uint8_t minute)
{
    NN20ClockAlarmConfig alarm;
    (void)nn20clock_alarm_defaults(&alarm);
    alarm.id = NN20CLOCK_ALARM_ID_NONE;
    alarm.hour = hour;
    alarm.minute = minute;
    return alarm;
}

TEST(a_fresh_storage_has_no_alarms)
{
    Fixture fixture = fixture_up_started();
    REQUIRE(fixture.storage != NULL);

    /* Static: over 5 KB, and the board's task stack is smaller. */
    static NN20ClockAlarmList list;
    CHECK_EQ(ESP_OK, nn20clock_storage_list_alarms(fixture.storage, &list));
    CHECK_EQ(0, list.count);

    fixture_down(&fixture);
}

TEST(a_saved_alarm_comes_back)
{
    Fixture fixture = fixture_up_started();
    REQUIRE(fixture.storage != NULL);

    NN20ClockAlarmConfig alarm = sample_alarm(6u, 45u);
    uint32_t id = 0;
    CHECK_EQ(ESP_OK, nn20clock_storage_save_alarm(fixture.storage, &alarm,
                                                  &id));
    CHECK(id != NN20CLOCK_ALARM_ID_NONE);

    /* Static: over 5 KB, and the board's task stack is smaller. */
    static NN20ClockAlarmList list;
    REQUIRE(nn20clock_storage_list_alarms(fixture.storage, &list) == ESP_OK);
    CHECK_EQ(1, list.count);

    size_t index = 0;
    REQUIRE(nn20clock_alarm_list_find(&list, id, &index) == ESP_OK);
    CHECK_EQ(6, list.alarms[index].hour);
    CHECK_EQ(45, list.alarms[index].minute);

    fixture_down(&fixture);
}

TEST(saving_an_existing_id_edits_rather_than_duplicates)
{
    Fixture fixture = fixture_up_started();
    REQUIRE(fixture.storage != NULL);

    NN20ClockAlarmConfig alarm = sample_alarm(6u, 45u);
    uint32_t id = 0;
    REQUIRE(nn20clock_storage_save_alarm(fixture.storage, &alarm, &id)
            == ESP_OK);

    alarm.id = id;
    alarm.hour = 8u;
    CHECK_EQ(ESP_OK, nn20clock_storage_save_alarm(fixture.storage, &alarm,
                                                  NULL));

    /* Static: over 5 KB, and the board's task stack is smaller. */
    static NN20ClockAlarmList list;
    REQUIRE(nn20clock_storage_list_alarms(fixture.storage, &list) == ESP_OK);
    CHECK_EQ(1, list.count);
    CHECK_EQ(8, list.alarms[0].hour);

    fixture_down(&fixture);
}

/* Validation happens before anything is written, so a record the
 * scheduler could not evaluate never reaches flash. */
TEST(an_invalid_alarm_is_never_stored)
{
    Fixture fixture = fixture_up_started();
    REQUIRE(fixture.storage != NULL);

    NN20ClockAlarmConfig alarm = sample_alarm(25u, 0u);   /* no such hour */
    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_storage_save_alarm(fixture.storage, &alarm, NULL));

    /* A recurrent alarm with no days would never fire. */
    alarm = sample_alarm(7u, 0u);
    alarm.weekdays_mask = 0u;
    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_storage_save_alarm(fixture.storage, &alarm, NULL));

    /* Static: over 5 KB, and the board's task stack is smaller. */
    static NN20ClockAlarmList list;
    REQUIRE(nn20clock_storage_list_alarms(fixture.storage, &list) == ESP_OK);
    CHECK_EQ(0, list.count);

    fixture_down(&fixture);
}

/* Design 10: a fired one-off is deleted once it has been dismissed. */
TEST(an_alarm_can_be_deleted)
{
    Fixture fixture = fixture_up_started();
    REQUIRE(fixture.storage != NULL);

    NN20ClockAlarmConfig alarm = sample_alarm(7u, 0u);
    uint32_t id = 0;
    REQUIRE(nn20clock_storage_save_alarm(fixture.storage, &alarm, &id)
            == ESP_OK);

    CHECK_EQ(ESP_OK, nn20clock_storage_delete_alarm(fixture.storage, id));

    /* Static: over 5 KB, and the board's task stack is smaller. */
    static NN20ClockAlarmList list;
    REQUIRE(nn20clock_storage_list_alarms(fixture.storage, &list) == ESP_OK);
    CHECK_EQ(0, list.count);

    /* Deleting it twice is not found, not a crash. */
    CHECK_EQ(ESP_ERR_NOT_FOUND,
             nn20clock_storage_delete_alarm(fixture.storage, id));

    fixture_down(&fixture);
}

TEST(the_stored_list_fills_up_and_says_so)
{
    Fixture fixture = fixture_up_started();
    REQUIRE(fixture.storage != NULL);

    for (int i = 0; i < NN20CLOCK_ALARM_MAX; i++) {
        NN20ClockAlarmConfig alarm = sample_alarm(7u, (uint8_t)i);
        REQUIRE(nn20clock_storage_save_alarm(fixture.storage, &alarm, NULL)
                == ESP_OK);
    }

    NN20ClockAlarmConfig one_too_many = sample_alarm(9u, 0u);
    CHECK_EQ(ESP_ERR_NO_MEM,
             nn20clock_storage_save_alarm(fixture.storage, &one_too_many,
                                          NULL));

    fixture_down(&fixture);
}

TEST(alarm_calls_reject_null)
{
    Fixture fixture = fixture_up();
    REQUIRE(fixture.storage != NULL);

    /* Static: over 5 KB, and the board's task stack is smaller. */
    static NN20ClockAlarmList list;
    NN20ClockAlarmConfig alarm = sample_alarm(7u, 0u);

    CHECK_EQ(ESP_ERR_INVALID_ARG, nn20clock_storage_list_alarms(NULL, &list));
    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_storage_list_alarms(fixture.storage, NULL));
    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_storage_save_alarm(NULL, &alarm, NULL));
    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_storage_save_alarm(fixture.storage, NULL, NULL));
    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_storage_delete_alarm(fixture.storage,
                                            NN20CLOCK_ALARM_ID_NONE));

    fixture_down(&fixture);
}

/* ------------------------------------------- not implemented yet ------ */

TEST(media_calls_report_not_supported)
{
    Fixture fixture = fixture_up();
    REQUIRE(fixture.storage != NULL);

    int dummy = 0;
    CHECK_EQ(ESP_ERR_NOT_SUPPORTED,
             nn20clock_storage_list_media(fixture.storage, &dummy));
    CHECK_EQ(ESP_ERR_NOT_SUPPORTED,
             nn20clock_storage_list_media_sets(fixture.storage, &dummy));

    /* Bad arguments still outrank "not implemented": a NULL here is a
     * caller bug today and will still be one at Milestone 6. */
    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_storage_list_media(fixture.storage, NULL));

    fixture_down(&fixture);
}

/* ---------------------------------------------- wifi credentials (M5) -- */

/* Credentials joined the config record at schema 2. They round-trip
 * like any other field - and, on the target, that round trip is
 * through flash. */
TEST(wifi_credentials_round_trip)
{
    Fixture fixture = fixture_up_started();
    REQUIRE(fixture.storage != NULL);

    NN20ClockConfig config;
    REQUIRE(nn20clock_storage_load_config(fixture.storage, &config) == ESP_OK);
    /* A device that has never visited the settings screen has none, and
     * falls back to the build-time values. */
    CHECK_STR_EQ("", config.wifi_ssid);

    snprintf(config.wifi_ssid, sizeof(config.wifi_ssid), "some-network");
    snprintf(config.wifi_password, sizeof(config.wifi_password), "hunter2");
    CHECK_EQ(ESP_OK, nn20clock_storage_save_config(fixture.storage, &config));

    NN20ClockConfig reloaded;
    REQUIRE(nn20clock_storage_load_config(fixture.storage, &reloaded)
            == ESP_OK);
    CHECK_STR_EQ("some-network", reloaded.wifi_ssid);
    CHECK_STR_EQ("hunter2", reloaded.wifi_password);

    fixture_down(&fixture);
}

/* An unterminated credential would overrun the first copy that touched
 * it, so it is refused at the boundary rather than trusted. */
TEST(unterminated_credentials_are_rejected)
{
    Fixture fixture = fixture_up_started();
    REQUIRE(fixture.storage != NULL);

    NN20ClockConfig config;
    REQUIRE(nn20clock_storage_load_config(fixture.storage, &config) == ESP_OK);
    memset(config.wifi_ssid, 'x', sizeof(config.wifi_ssid));
    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_storage_save_config(fixture.storage, &config));

    REQUIRE(nn20clock_storage_load_config(fixture.storage, &config) == ESP_OK);
    memset(config.wifi_password, 'x', sizeof(config.wifi_password));
    CHECK_EQ(ESP_ERR_INVALID_ARG,
             nn20clock_storage_save_config(fixture.storage, &config));

    fixture_down(&fixture);
}

TEST(the_schema_version_is_the_one_this_build_writes)
{
    NN20ClockConfig config;
    REQUIRE(nn20clock_storage_default_config(&config) == ESP_OK);
    /*
     * Bumped to 2 when credentials were added, to 3 when brightness
     * became a day/night schedule, and to 4 when the NTP switch and the
     * last-known time arrived; to 5 for the playback brightness
     * minimum, appended so that version 4 is migrated rather than
     * dropped. A stored record from any other version is discarded
     * rather than misread.
     *
     * This test exists to fail. Changing NN20ClockConfig without moving
     * the version is how a device reads yesterday's bytes as today's
     * fields - which for a struct of small integers is not a crash but
     * a clock that quietly comes up at the wrong brightness. If this
     * line is what broke, the fix is the version, not the test.
     */
    CHECK_EQ(5, NN20CLOCK_STORAGE_SCHEMA_VERSION);
    CHECK_EQ(NN20CLOCK_STORAGE_SCHEMA_VERSION, config.schema_version);
}

TEST_MAIN("nn20clock_storage")
{
    RUN(ctor_requires_a_worker);
    RUN(dtor_tolerates_null);
    RUN(start_makes_it_ready);
    RUN(is_not_persistent_yet);
    RUN(default_config_is_in_range);
    RUN(load_returns_the_defaults_before_anything_is_saved);
    RUN(save_then_load_round_trips);
    RUN(save_forces_the_current_schema_version);
    RUN(save_rejects_out_of_range_values);
    RUN(save_rejects_an_unterminated_timezone);
    RUN(config_calls_reject_null);
    RUN(async_save_applies_and_calls_the_continuation);
    RUN(async_save_copies_the_caller_s_config);
    RUN(async_save_validates_before_queueing);
    RUN(a_fresh_storage_has_no_alarms);
    RUN(a_saved_alarm_comes_back);
    RUN(saving_an_existing_id_edits_rather_than_duplicates);
    RUN(an_invalid_alarm_is_never_stored);
    RUN(an_alarm_can_be_deleted);
    RUN(the_stored_list_fills_up_and_says_so);
    RUN(alarm_calls_reject_null);
    RUN(media_calls_report_not_supported);

    RUN(wifi_credentials_round_trip);
    RUN(unterminated_credentials_are_rejected);
    RUN(the_schema_version_is_the_one_this_build_writes);
}
