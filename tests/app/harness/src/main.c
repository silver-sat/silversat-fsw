/*
 * Smoke tests for the app test harness (tests/app/common).
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * They drive test_app (src/test_app.c) through its channels with the
 * harness helpers. Copy this layout when testing a real app.
 */

#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/ztest.h>

#include "app_test.h"
#include "msg/common.h"
#include "msg/test_app.h"
#include "silversat/resource_map.h"

/* Generous: in simulated time a wait costs nothing unless it fails. */
#define WAIT K_SECONDS(1)

static struct app_status current_status(void)
{
	struct app_status status;

	zassert_ok(zbus_chan_read(&test_app_status_chan, &status, K_MSEC(10)));
	return status;
}

static struct test_app_hk current_hk(void)
{
	struct test_app_hk hk;

	zassert_ok(zbus_chan_read(&test_app_hk_chan, &hk, K_MSEC(10)));
	return hk;
}

static void set_value(int32_t value)
{
	const struct test_app_cmd cmd = {
		.id = TEST_APP_CMD_SET_VALUE,
		.args.set_value.value = value,
	};

	zassert_ok(zbus_chan_pub(&test_app_cmd_chan, &cmd, K_NO_WAIT));
}

ZTEST(app_harness, test_tick_carries_slot_and_uptime)
{
	struct frame_tick tick = app_test_tick(FRAME_SLOTS + 3);

	zassert_equal(tick.count, FRAME_SLOTS + 3);
	zassert_equal(tick.slot, 3);
	zassert_equal(tick.uptime_ms, k_uptime_get());
}

ZTEST(app_harness, test_wakeups_step_the_app)
{
	struct app_status before = current_status();
	struct app_status after;

	/* One wakeup at a time, as the frame manager delivers them. */
	for (uint32_t count = 0; count < 5; count++) {
		zassert_ok(app_test_wake(&test_app_wakeup_chan, count));
		zassert_ok(app_test_wait_status(&test_app_status_chan, before.steps + count + 1,
						0, WAIT, NULL));
	}

	after = current_status();
	zassert_equal(after.steps, before.steps + 5);
	zassert_equal(current_hk().last_count, 4);
}

ZTEST(app_harness, test_command_accepted)
{
	struct app_status before = current_status();
	struct app_status after;
	uint32_t handled = before.cmd_accepted + before.cmd_rejected;

	set_value(42);
	zassert_ok(app_test_wait_status(&test_app_status_chan, 0, handled + 1, WAIT, &after));
	zassert_equal(after.cmd_accepted, before.cmd_accepted + 1);
	zassert_equal(after.cmd_rejected, before.cmd_rejected);
	zassert_equal(current_hk().value, 42);
}

ZTEST(app_harness, test_command_rejected)
{
	struct app_status before;
	struct app_status after;
	uint32_t handled;

	set_value(7);
	before = current_status();
	handled = before.cmd_accepted + before.cmd_rejected;
	zassert_ok(app_test_wait_status(&test_app_status_chan, 0, handled + 1, WAIT, &before));
	handled++;

	/* A bad argument and an unknown command are both counted as rejected. */
	set_value(-1);
	const struct test_app_cmd unknown = {.id = 99};

	zassert_ok(zbus_chan_pub(&test_app_cmd_chan, &unknown, K_NO_WAIT));
	zassert_ok(app_test_wait_status(&test_app_status_chan, 0, handled + 2, WAIT, &after));
	zassert_equal(after.cmd_rejected, before.cmd_rejected + 2);
	zassert_equal(after.cmd_accepted, before.cmd_accepted);
	zassert_equal(current_hk().value, 7, "a rejected command must not change state");
}

ZTEST(app_harness, test_wait_times_out)
{
	struct app_status before = current_status();
	struct app_status got;

	/* Nothing is published, so the app never reaches this step count. */
	zassert_equal(app_test_wait_status(&test_app_status_chan, before.steps + 1, 0,
					   K_MSEC(50), &got),
		      -EAGAIN);
	zassert_equal(got.steps, before.steps);
}

ZTEST(app_harness, test_simulated_time)
{
	/*
	 * A day of simulated time passes in well under a second of real time;
	 * twister's timeout would stop the test long before a real day. (ztest
	 * reports this test's duration in simulated time, so it looks long.)
	 */
	struct app_status before = current_status();

	k_sleep(K_HOURS(24));
	zassert_ok(app_test_wake(&test_app_wakeup_chan, 0));
	zassert_ok(app_test_wait_status(&test_app_status_chan, before.steps + 1, 0, WAIT, NULL));
	zassert_true(current_hk().last_uptime_ms >= 24LL * 60 * 60 * 1000);
}

ZTEST_SUITE(app_harness, NULL, NULL, NULL, NULL, NULL);
