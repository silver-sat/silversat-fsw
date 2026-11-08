/*
 * The temperature app on a board with no sensor (DS-30): no alias, as on the
 * Nucleo and native_sim. It keeps running, and says it has nothing to report.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/ztest.h>

#include "app_test.h"
#include "msg/common.h"
#include "msg/temperature.h"
#include "silversat/resource_map.h"

#define WAIT K_SECONDS(1)

ZTEST_SUITE(temperature_no_sensor, NULL, NULL, NULL, NULL, NULL);

ZTEST(temperature_no_sensor, test_it_runs_and_publishes_nothing_valid)
{
	struct temperature_reading reading;
	struct temperature_hk hk;
	struct app_status status;
	const struct temperature_cmd reinit = {.id = TEMPERATURE_CMD_REINIT};

	zassert_ok(app_test_wake(&temperature_wakeup_chan, 0));
	zassert_ok(app_test_wait_status(&temperature_status_chan, 1, 0, WAIT, NULL));

	zassert_ok(zbus_chan_read(&temperature_chan, &reading, K_NO_WAIT));
	zassert_false(reading.valid);
	zassert_ok(zbus_chan_read(&temperature_hk_chan, &hk, K_NO_WAIT));
	zassert_false(hk.present);
	zassert_equal(hk.read_errors, 0, "no sensor is not a failing sensor");

	/* There's nothing to reinitialize. */
	zassert_ok(zbus_chan_pub(&temperature_cmd_chan, &reinit, K_NO_WAIT));
	zassert_ok(app_test_wait_status(&temperature_status_chan, 1, 1, WAIT, &status));
	zassert_equal(status.cmd_rejected, 1);
}
