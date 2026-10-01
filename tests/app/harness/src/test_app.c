/*
 * A test-only app, written the way every flight app is written (DS-10,
 * DS-14; design rationale section 9), so the harness is tested against the
 * real pattern.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>

#include "msg/common.h"
#include "msg/test_app.h"
#include "silversat/resource_map.h"

/* Test-only. A flight app takes its stack size from the resource map. */
#define TEST_APP_STACK_SIZE 1024

/* The app's one pending point: every message it receives arrives here. */
ZBUS_MSG_SUBSCRIBER_DEFINE(test_app_sub);
ZBUS_CHAN_ADD_OBS(test_app_wakeup_chan, test_app_sub, 3);
ZBUS_CHAN_ADD_OBS(test_app_cmd_chan, test_app_sub, 3);

static struct app_status status;
static struct test_app_hk hk;

static void test_app_step(const struct frame_tick *tick)
{
	hk.last_count = tick->count;
	hk.last_uptime_ms = tick->uptime_ms;
}

static int test_app_dispatch(const struct test_app_cmd *cmd)
{
	switch (cmd->id) {
	case TEST_APP_CMD_SET_VALUE:
		if (cmd->args.set_value.value < 0) {
			return -EINVAL;
		}
		hk.value = cmd->args.set_value.value;
		return 0;
	default:
		return -ENOTSUP;
	}
}

static void test_app_main(void *a, void *b, void *c)
{
	const struct zbus_channel *chan;
	union test_app_msg msg;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	while (zbus_sub_wait_msg(&test_app_sub, &chan, &msg, K_FOREVER) == 0) {
		if (chan == &test_app_wakeup_chan) {
			test_app_step(&msg.tick);
			status.steps++;
		} else if (chan == &test_app_cmd_chan) {
			if (test_app_dispatch(&msg.cmd) == 0) {
				status.cmd_accepted++;
			} else {
				status.cmd_rejected++;
			}
		}
		/*
		 * Housekeeping first, then status: once a test sees the status
		 * change, the housekeeping it reads is already current.
		 */
		zbus_chan_pub(&test_app_hk_chan, &hk, K_NO_WAIT);
		zbus_chan_pub(&test_app_status_chan, &status, K_NO_WAIT);
	}
}

K_THREAD_DEFINE(test_app_tid, TEST_APP_STACK_SIZE, test_app_main, NULL, NULL, NULL,
		PRIORITY_1HZ, 0, 0);
