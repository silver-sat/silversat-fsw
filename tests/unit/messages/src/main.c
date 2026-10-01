/*
 * Tests for the generated message headers and channels (DS-60, DS-61, DS-68).
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The generator's rules are tested in Python (messages/tests/). These tests
 * check what only a real build can: the generated C compiles, each channel
 * carries the type its header says, and an app's receive union works with
 * a message subscriber under ASan and UBSan.
 */

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/ztest.h>

#include "msg/common.h"
#include "msg/frame_manager.h"

/* Stands in for the frame manager's one pending point (DS-10). */
ZBUS_MSG_SUBSCRIBER_DEFINE(test_sub);
ZBUS_CHAN_ADD_OBS(frame_manager_cmd_chan, test_sub, 3);

/* Every message an app receives must fit one buffer from the zbus pool. */
BUILD_ASSERT(sizeof(union frame_manager_msg) <= CONFIG_ZBUS_MSG_SUBSCRIBER_NET_BUF_STATIC_DATA_SIZE,
	     "raise CONFIG_ZBUS_MSG_SUBSCRIBER_NET_BUF_STATIC_DATA_SIZE");

/* Empty the subscriber's queue so each test starts clean. */
static void drain(void *fixture)
{
	const struct zbus_channel *chan;
	union frame_manager_msg msg;

	ARG_UNUSED(fixture);
	while (zbus_sub_wait_msg(&test_sub, &chan, &msg, K_NO_WAIT) == 0) {
	}
}

ZTEST_SUITE(messages, NULL, NULL, drain, NULL, NULL);

ZTEST(messages, test_ids_match_the_yaml)
{
	zassert_equal(APP_ID_FRAME_MANAGER, 1);
	zassert_equal(FRAME_MANAGER_CMD_SET_ENTRY_ENABLED, 1);
	zassert_equal(SEVERITY_DEBUG, 0);
	zassert_equal(SEVERITY_CRITICAL, 4);
}

ZTEST(messages, test_channels_carry_their_types)
{
	zassert_equal(zbus_chan_msg_size(&frame_manager_cmd_chan),
		      sizeof(struct frame_manager_cmd));
	zassert_equal(zbus_chan_msg_size(&frame_manager_hk_chan),
		      sizeof(struct frame_manager_hk));
	zassert_equal(zbus_chan_msg_size(&frame_manager_status_chan),
		      sizeof(struct app_status));
}

ZTEST(messages, test_commands_are_queued_not_overwritten)
{
	/*
	 * DS-11: two commands published back to back both arrive, in order.
	 * A plain subscriber would see only the second.
	 */
	const struct frame_manager_cmd first = {
		.id = FRAME_MANAGER_CMD_SET_ENTRY_ENABLED,
		.args.set_entry_enabled = {.mode = MODE_SAFE, .entry = 3, .enabled = false},
	};
	const struct frame_manager_cmd second = {
		.id = FRAME_MANAGER_CMD_SET_ENTRY_ENABLED,
		.args.set_entry_enabled = {.mode = MODE_NOMINAL, .entry = 4, .enabled = true},
	};
	const struct zbus_channel *chan;
	union frame_manager_msg msg;

	zassert_ok(zbus_chan_pub(&frame_manager_cmd_chan, &first, K_NO_WAIT));
	zassert_ok(zbus_chan_pub(&frame_manager_cmd_chan, &second, K_NO_WAIT));

	zassert_ok(zbus_sub_wait_msg(&test_sub, &chan, &msg, K_NO_WAIT));
	zassert_equal_ptr(chan, &frame_manager_cmd_chan);
	zassert_equal(msg.cmd.id, FRAME_MANAGER_CMD_SET_ENTRY_ENABLED);
	zassert_equal(msg.cmd.args.set_entry_enabled.mode, MODE_SAFE);
	zassert_equal(msg.cmd.args.set_entry_enabled.entry, 3);
	zassert_false(msg.cmd.args.set_entry_enabled.enabled);

	zassert_ok(zbus_sub_wait_msg(&test_sub, &chan, &msg, K_NO_WAIT));
	zassert_equal_ptr(chan, &frame_manager_cmd_chan);
	zassert_equal(msg.cmd.args.set_entry_enabled.mode, MODE_NOMINAL);
	zassert_equal(msg.cmd.args.set_entry_enabled.entry, 4);
	zassert_true(msg.cmd.args.set_entry_enabled.enabled);

	zassert_equal(zbus_sub_wait_msg(&test_sub, &chan, &msg, K_NO_WAIT), -ENOMSG);
}

ZTEST(messages, test_status_channel_round_trip)
{
	/*
	 * Status channels are declared in msg/common.h, so the frame manager
	 * and health can read any app's status (DS-22, DS-43).
	 */
	const struct app_status sent = {.steps = 7, .cmd_accepted = 2, .cmd_rejected = 1};
	struct app_status got;

	zassert_ok(zbus_chan_pub(&frame_manager_status_chan, &sent, K_NO_WAIT));
	zassert_ok(zbus_chan_read(&frame_manager_status_chan, &got, K_NO_WAIT));
	zassert_mem_equal(&got, &sent, sizeof(got));
}

ZTEST(messages, test_housekeeping_channel_is_last_value)
{
	/* Telemetry channels keep only the newest value (DS-11). */
	const struct frame_manager_hk older = {.frame_count = 10, .mode = MODE_SAFE};
	const struct frame_manager_hk newer = {.frame_count = 20, .mode = MODE_NOMINAL};
	struct frame_manager_hk got;

	zassert_ok(zbus_chan_pub(&frame_manager_hk_chan, &older, K_NO_WAIT));
	zassert_ok(zbus_chan_pub(&frame_manager_hk_chan, &newer, K_NO_WAIT));
	zassert_ok(zbus_chan_read(&frame_manager_hk_chan, &got, K_NO_WAIT));
	zassert_equal(got.frame_count, 20);
	zassert_equal(got.mode, MODE_NOMINAL);
}
