/*
 * temperature app tests (DS-30, DS-31).
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The app runs as in flight, on a fake sensor (tests/fakes/). The tests
 * stand in for the frame manager (waking it) and for command ingest (sending
 * it commands).
 */

#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/ztest.h>

#include "app_test.h"
#include "fake_temperature.h"
#include "msg/common.h"
#include "msg/temperature.h"
#include "silversat/event.h"
#include "silversat/resource_map.h"

#define WAIT K_SECONDS(1)

/* Failed readings that use up every reinit, and mark the sensor failed. */
#define READINGS_TO_FAIL (CONFIG_SS_TEMPERATURE_FAIL_LIMIT * CONFIG_SS_TEMPERATURE_REINIT_LIMIT)

static const struct device *const sensor = DEVICE_DT_GET(TEMPERATURE_SENSOR_NODE);

/*
 * The channel the app should read in this build. The fake answers only on
 * the channel it's told to, so every test here also checks the app asks for
 * the right one.
 */
#define EXPECTED_CHANNEL                                                                           \
	(IS_ENABLED(CONFIG_SS_TEMPERATURE_CHIP) ? SENSOR_CHAN_DIE_TEMP : SENSOR_CHAN_AMBIENT_TEMP)
#define OTHER_CHANNEL                                                                              \
	(IS_ENABLED(CONFIG_SS_TEMPERATURE_CHIP) ? SENSOR_CHAN_AMBIENT_TEMP : SENSOR_CHAN_DIE_TEMP)

/* Wakeups and commands sent so far: the app's status counts both. */
static uint32_t steps;
static uint32_t commands;

/* Wake the app once, as the frame manager does each major frame. */
static void wake(void)
{
	zassert_ok(app_test_wake(&temperature_wakeup_chan, steps * FRAME_SLOTS));
	steps++;
	zassert_ok(app_test_wait_status(&temperature_status_chan, steps, commands, WAIT, NULL));
}

static void wake_times(uint32_t times)
{
	for (uint32_t i = 0; i < times; i++) {
		wake();
	}
}

/* Send a command, as command ingest does, and return the app's status after it. */
static struct app_status send(const struct temperature_cmd *cmd)
{
	struct app_status status;

	zassert_ok(zbus_chan_pub(&temperature_cmd_chan, cmd, K_NO_WAIT));
	commands++;
	zassert_ok(app_test_wait_status(&temperature_status_chan, steps, commands, WAIT, &status));
	return status;
}

static void set_enabled(bool enabled)
{
	const struct temperature_cmd cmd = {
		.id = TEMPERATURE_CMD_SET_ENABLED,
		.args.set_enabled = {.enabled = enabled},
	};

	(void)send(&cmd);
}

static struct app_status send_reinit(void)
{
	const struct temperature_cmd cmd = {.id = TEMPERATURE_CMD_REINIT};

	return send(&cmd);
}

static struct temperature_reading latest(void)
{
	struct temperature_reading reading;

	zassert_ok(zbus_chan_read(&temperature_chan, &reading, K_NO_WAIT));
	return reading;
}

static struct temperature_hk housekeeping(void)
{
	struct temperature_hk hk;

	zassert_ok(zbus_chan_read(&temperature_hk_chan, &hk, K_NO_WAIT));
	return hk;
}

/* Each test starts with a working sensor and clean fault counts. */
static void before(void *fixture)
{
	ARG_UNUSED(fixture);
	fake_temperature_reset(sensor);
	fake_temperature_answer_on(sensor, EXPECTED_CHANNEL);
	set_enabled(true);
	(void)send_reinit();
	app_test_drain_events();
}

ZTEST_SUITE(temperature, NULL, NULL, before, NULL, NULL);

ZTEST(temperature, test_a_reading_is_published_in_degrees_with_its_time)
{
	struct frame_tick tick = app_test_tick(steps * FRAME_SLOTS);

	tick.met_ms = 123456789; /* nothing a clock would read */
	fake_temperature_set_mc(sensor, 21500);
	zassert_ok(zbus_chan_pub(&temperature_wakeup_chan, &tick, K_NO_WAIT));
	steps++;
	zassert_ok(app_test_wait_status(&temperature_status_chan, steps, commands, WAIT, NULL));

	struct temperature_reading reading = latest();

	zassert_true(reading.valid);
	zassert_within(reading.temperature_c, 21.5f, 0.001f);
	zassert_equal(reading.met_ms, 123456789, "the tick's MET, not the clock's (DS-25)");
}

ZTEST(temperature, test_a_below_zero_reading)
{
	fake_temperature_set_mc(sensor, -40250);
	wake();
	zassert_within(latest().temperature_c, -40.25f, 0.001f);
	zassert_equal(housekeeping().temperature_mc, -40250);
}

ZTEST(temperature, test_the_sequence_number_counts_every_publish)
{
	wake();
	uint32_t first = latest().seq;

	fake_temperature_fail(sensor, true);
	wake(); /* a failed reading is still a publish */
	zassert_equal(latest().seq, first + 1);
}

ZTEST(temperature, test_housekeeping_says_the_sensor_is_there_and_counts_samples)
{
	uint32_t before = housekeeping().samples;

	wake_times(3);
	struct temperature_hk hk = housekeeping();

	zassert_true(hk.present);
	zassert_true(hk.enabled);
	zassert_false(hk.failed);
	zassert_equal(hk.samples, before + 3);
}

ZTEST(temperature, test_a_failed_reading_is_not_valid_and_is_counted)
{
	uint32_t errors = housekeeping().read_errors;

	fake_temperature_fail(sensor, true);
	wake();
	zassert_false(latest().valid);
	zassert_equal(housekeeping().read_errors, errors + 1);
}

ZTEST(temperature, test_a_good_reading_after_a_failure_is_valid_again)
{
	fake_temperature_fail(sensor, true);
	wake();
	fake_temperature_fail(sensor, false);
	fake_temperature_set_mc(sensor, 19000);
	wake();
	zassert_true(latest().valid);
	zassert_within(latest().temperature_c, 19.0f, 0.001f);
}

ZTEST(temperature, test_failures_in_a_row_count_a_reinit)
{
	uint32_t reinits = housekeeping().reinits;

	fake_temperature_fail(sensor, true);
	wake_times(CONFIG_SS_TEMPERATURE_FAIL_LIMIT - 1);
	zassert_equal(housekeeping().reinits, reinits, "one failure short");
	wake();
	zassert_equal(housekeeping().reinits, reinits + 1);
	zassert_false(housekeeping().failed, "one reinit doesn't give up");
}

ZTEST(temperature, test_a_good_reading_restarts_the_count)
{
	uint32_t reinits = housekeeping().reinits;

	fake_temperature_fail(sensor, true);
	wake_times(CONFIG_SS_TEMPERATURE_FAIL_LIMIT - 1);
	fake_temperature_fail(sensor, false);
	wake();
	fake_temperature_fail(sensor, true);
	wake_times(CONFIG_SS_TEMPERATURE_FAIL_LIMIT - 1);
	zassert_equal(housekeeping().reinits, reinits);
}

ZTEST(temperature, test_a_good_reading_clears_the_reinits_too)
{
	/* A sensor that fails now and then is not one that has failed. */
	for (int i = 0; i < 3; i++) {
		fake_temperature_fail(sensor, true);
		wake_times(READINGS_TO_FAIL - 1);
		fake_temperature_fail(sensor, false);
		wake();
	}
	zassert_false(housekeeping().failed);
}

ZTEST(temperature, test_a_sensor_that_keeps_failing_is_marked_failed_with_an_event)
{
	struct event event;

	fake_temperature_fail(sensor, true);
	wake_times(READINGS_TO_FAIL - 1);
	zassert_false(housekeeping().failed, "one reading short");
	wake();
	zassert_true(housekeeping().failed);
	zassert_true(app_test_find_event(APP_ID_TEMPERATURE, TEMPERATURE_EVENT_FAILED, &event));
}

ZTEST(temperature, test_a_failed_sensor_is_not_read_again)
{
	fake_temperature_fail(sensor, true);
	wake_times(READINGS_TO_FAIL);
	uint32_t fetches = fake_temperature_fetches(sensor);

	fake_temperature_fail(sensor, false); /* even once it would work */
	wake_times(3);
	zassert_equal(fake_temperature_fetches(sensor), fetches);
	zassert_false(latest().valid);
}

ZTEST(temperature, test_reinit_restores_a_failed_sensor_with_an_event)
{
	struct event event;
	struct app_status before;

	fake_temperature_fail(sensor, true);
	wake_times(READINGS_TO_FAIL);
	fake_temperature_fail(sensor, false);
	app_test_drain_events();
	zassert_ok(zbus_chan_read(&temperature_status_chan, &before, K_NO_WAIT));

	zassert_equal(send_reinit().cmd_rejected, before.cmd_rejected);
	zassert_false(housekeeping().failed);
	zassert_true(app_test_find_event(APP_ID_TEMPERATURE, TEMPERATURE_EVENT_RESTORED, &event));
	wake();
	zassert_true(latest().valid);
}

ZTEST(temperature, test_reinit_of_a_working_sensor_raises_no_event)
{
	struct event event;

	(void)send_reinit();
	zassert_false(app_test_find_event(APP_ID_TEMPERATURE, TEMPERATURE_EVENT_RESTORED, &event));
}

ZTEST(temperature, test_stopped_means_not_read_and_not_valid)
{
	wake();
	uint32_t fetches = fake_temperature_fetches(sensor);

	set_enabled(false);
	zassert_false(housekeeping().enabled);
	wake_times(2);
	zassert_equal(fake_temperature_fetches(sensor), fetches);
	zassert_false(latest().valid);
}

ZTEST(temperature, test_set_enabled_sets_rather_than_toggles)
{
	set_enabled(false);
	set_enabled(false);
	zassert_false(housekeeping().enabled, "DS-35");
	set_enabled(true);
	set_enabled(true);
	zassert_true(housekeeping().enabled);
	wake();
	zassert_true(latest().valid);
}

ZTEST(temperature, test_a_sensor_without_the_channel_is_a_failed_reading)
{
	uint32_t errors = housekeeping().read_errors;

	fake_temperature_answer_on(sensor, OTHER_CHANNEL);
	wake();
	zassert_false(latest().valid);
	zassert_equal(housekeeping().read_errors, errors + 1);
}

ZTEST(temperature, test_an_unknown_command_is_rejected)
{
	const struct temperature_cmd cmd = {.id = 0xFF};
	struct app_status before;

	zassert_ok(zbus_chan_read(&temperature_status_chan, &before, K_NO_WAIT));
	zassert_equal(send(&cmd).cmd_rejected, before.cmd_rejected + 1);
}
