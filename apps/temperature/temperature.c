/*
 * The temperature app (DS-30, DS-31).
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * A device app: the pattern for the others. It drives one Zephyr sensor,
 * named by the temperature-sensor devicetree alias, so the same code runs on
 * any board's sensor. Woken once a major frame, it:
 *
 *   - reads the sensor through the classic sensor API;
 *   - publishes the result on temperature_chan, in degrees Celsius, with its
 *     time, a sequence number, and whether it is valid;
 *   - publishes its housekeeping.
 *
 * Its commands, from the ground:
 *
 *   set_enabled   start or stop reading the sensor.
 *   reinit        clear the fault counts and try the sensor again.
 *
 * The fault policy (DS-31): SS_TEMPERATURE_FAIL_LIMIT failed readings in a
 * row count as one reinit, and SS_TEMPERATURE_REINIT_LIMIT reinits with no
 * good reading between them mark the sensor failed, with an event. Then it
 * isn't read until the ground sends reinit. Zephyr's sensor API has no call
 * to restart a driver, so a reinit here is a clean start to the counting; a
 * sensor with its own reset would do that as well.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>

#include "msg/common.h"
#include "msg/temperature.h"
#include "silversat/resource_map.h"

ZBUS_MSG_SUBSCRIBER_DEFINE(temperature_sub);
ZBUS_CHAN_ADD_OBS(temperature_wakeup_chan, temperature_sub, 3);
ZBUS_CHAN_ADD_OBS(temperature_cmd_chan, temperature_sub, 3);

/* NULL on a board with no temperature-sensor alias. */
static const struct device *const sensor = DEVICE_DT_GET_OR_NULL(TEMPERATURE_SENSOR_NODE);

static struct app_status status;
static struct temperature_hk hk = {.enabled = true};
static struct temperature_reading reading;

/* The fault policy's counts: failed readings, and reinits, in a row. */
static uint32_t failures_in_row;
static uint32_t reinits_in_row;

/* MET from the latest tick, for events and readings (DS-25). */
static int64_t now_met_ms;

/* A failed reading, through the fault policy. */
static void count_failure(void)
{
	hk.read_errors++;
	failures_in_row++;
	if (failures_in_row < CONFIG_SS_TEMPERATURE_FAIL_LIMIT) {
		return;
	}
	failures_in_row = 0;
	hk.reinits++;
	reinits_in_row++;
	if (reinits_in_row >= CONFIG_SS_TEMPERATURE_REINIT_LIMIT) {
		hk.failed = true;
		emit_temperature_failed(now_met_ms);
	}
}

/* Take one reading. Returns true and sets *value if it worked. */
static bool read_sensor(struct sensor_value *value)
{
	if (sensor_sample_fetch(sensor) != 0) {
		return false;
	}
	return sensor_channel_get(sensor, SENSOR_CHAN_AMBIENT_TEMP, value) == 0;
}

/* Read the sensor if there is one to read, and publish what came of it. */
static void take_reading(void)
{
	reading.valid = false;
	if (hk.present && hk.enabled && !hk.failed) {
		struct sensor_value value;

		if (read_sensor(&value)) {
			failures_in_row = 0;
			reinits_in_row = 0;
			hk.samples++;
			hk.temperature_mc = (int32_t)sensor_value_to_milli(&value);
			reading.temperature_c = sensor_value_to_float(&value);
			reading.valid = true;
		} else {
			count_failure();
		}
	}
	reading.met_ms = now_met_ms;
	reading.seq++;
	zbus_chan_pub(&temperature_chan, &reading, K_NO_WAIT);
}

static int reinit(void)
{
	if (!hk.present) {
		return -ENODEV;
	}
	failures_in_row = 0;
	reinits_in_row = 0;
	if (hk.failed) {
		hk.failed = false;
		emit_temperature_restored(now_met_ms);
	}
	return 0;
}

static int handle_command(const struct temperature_cmd *cmd)
{
	switch (cmd->id) {
	case TEMPERATURE_CMD_SET_ENABLED:
		hk.enabled = cmd->args.set_enabled.enabled;
		return 0;
	case TEMPERATURE_CMD_REINIT:
		return reinit();
	default:
		return -ENOTSUP;
	}
}

static void temperature_main(void *a, void *b, void *c)
{
	const struct zbus_channel *chan;
	union temperature_msg msg;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	hk.present = sensor != NULL && device_is_ready(sensor);
	zbus_chan_pub(&temperature_hk_chan, &hk, K_NO_WAIT);
	while (zbus_sub_wait_msg(&temperature_sub, &chan, &msg, K_FOREVER) == 0) {
		if (chan == &temperature_wakeup_chan) {
			now_met_ms = msg.tick.met_ms;
			take_reading();
			status.steps++;
		} else if (chan == &temperature_cmd_chan) {
			if (handle_command(&msg.cmd) == 0) {
				status.cmd_accepted++;
			} else {
				status.cmd_rejected++;
			}
		}
		zbus_chan_pub(&temperature_hk_chan, &hk, K_NO_WAIT);
		zbus_chan_pub(&temperature_status_chan, &status, K_NO_WAIT);
	}
}

K_THREAD_DEFINE(temperature_tid, TEMPERATURE_STACK_SIZE, temperature_main, NULL, NULL, NULL,
		TEMPERATURE_PRIORITY, 0, 0);
