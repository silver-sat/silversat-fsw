/*
 * Tests for command routing: decoding command text and publishing it to
 * the target app (DS-50 stages 6 and 7).
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The vectors are shared with the ground's tests
 * (tools/tests/test_command_text.py), so both sides must give each one the
 * same result.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/ztest.h>

#include "msg/common.h"
#include "msg/typed_app.h"
#include "route_vectors.h"
#include "silversat/cmd_route.h"
#include "silversat/resource_map.h"

/* Stands in for typed_app, so tests can see what was published. */
ZBUS_MSG_SUBSCRIBER_DEFINE(typed_sub);
ZBUS_CHAN_ADD_OBS(typed_app_cmd_chan, typed_sub, 3);

static enum cmd_route_result route(const char *text, uint8_t mode, struct cmd_route_info *info)
{
	return cmd_route(text, strlen(text), mode, info);
}

/*
 * Report commands as handled on an app's status channel, as the app would.
 * Routing counts each app's unhandled ground commands (CMD_ROUTE_BUSY).
 */
static struct app_status typed_status;
static struct app_status frame_manager_status;

static void typed_handled(uint32_t count)
{
	typed_status.cmd_accepted += count;
	zassert_ok(zbus_chan_pub(&typed_app_status_chan, &typed_status, K_NO_WAIT));
}

/* Nothing reads the frame manager's commands here; just count them handled. */
static void frame_manager_handled(void)
{
	frame_manager_status.cmd_accepted++;
	zassert_ok(zbus_chan_pub(&frame_manager_status_chan, &frame_manager_status, K_NO_WAIT));
}

/* Take the next command typed_app was sent, or fail. */
static struct typed_app_cmd next_typed_command(void)
{
	const struct zbus_channel *chan;
	union typed_app_msg msg;

	zassert_ok(zbus_sub_wait_msg(&typed_sub, &chan, &msg, K_NO_WAIT), "nothing published");
	zassert_equal_ptr(chan, &typed_app_cmd_chan);
	typed_handled(1);
	return msg.cmd;
}

/* Handle everything waiting for typed_app. */
static void drain(void *fixture)
{
	const struct zbus_channel *chan;
	union typed_app_msg msg;
	uint32_t count = 0;

	ARG_UNUSED(fixture);
	while (zbus_sub_wait_msg(&typed_sub, &chan, &msg, K_NO_WAIT) == 0) {
		count++;
	}
	typed_handled(count);
}

ZTEST_SUITE(cmd_route, NULL, NULL, drain, drain, NULL);

ZTEST(cmd_route, test_shared_vectors)
{
	for (size_t i = 0; i < ARRAY_SIZE(route_vectors); i++) {
		const struct route_vector *v = &route_vectors[i];
		struct cmd_route_info info;
		enum cmd_route_result result = route(v->text, v->mode, &info);

		zassert_equal(result, v->result, "\"%s\": got %d, expected %d", v->text, result,
			      v->result);
		if (v->result == CMD_ROUTE_BAD_ARG) {
			zassert_equal(info.bad_arg, v->bad_arg, "\"%s\": argument %u", v->text,
				      info.bad_arg);
		}
		if (result == CMD_ROUTE_OK && info.app == APP_ID_FRAME_MANAGER) {
			frame_manager_handled();
		}
		drain(NULL);
	}
}

ZTEST(cmd_route, test_every_kind_at_its_limits)
{
	struct cmd_route_info info;
	struct typed_app_cmd cmd;

	zassert_equal(route("typed_app set_all 255 65535 4294967295 18446744073709551615 "
			    "127 32767 2147483647 9223372036854775807 true nominal",
			    MODE_SAFE, &info),
		      CMD_ROUTE_OK);
	cmd = next_typed_command();
	zassert_equal(cmd.id, TYPED_APP_CMD_SET_ALL);
	zassert_equal(cmd.args.set_all.u8, UINT8_MAX);
	zassert_equal(cmd.args.set_all.u16, UINT16_MAX);
	zassert_equal(cmd.args.set_all.u32, UINT32_MAX);
	zassert_equal(cmd.args.set_all.u64, UINT64_MAX);
	zassert_equal(cmd.args.set_all.i8, INT8_MAX);
	zassert_equal(cmd.args.set_all.i16, INT16_MAX);
	zassert_equal(cmd.args.set_all.i32, INT32_MAX);
	zassert_equal(cmd.args.set_all.i64, INT64_MAX);
	zassert_true(cmd.args.set_all.flag);
	zassert_equal(cmd.args.set_all.mode, MODE_NOMINAL);

	zassert_equal(route("typed_app set_all 0 0 0 0 -128 -32768 -2147483648 "
			    "-9223372036854775808 false safe",
			    MODE_NOMINAL, &info),
		      CMD_ROUTE_OK);
	cmd = next_typed_command();
	zassert_equal(cmd.args.set_all.u8, 0);
	zassert_equal(cmd.args.set_all.u64, 0);
	zassert_equal(cmd.args.set_all.i8, INT8_MIN);
	zassert_equal(cmd.args.set_all.i16, INT16_MIN);
	zassert_equal(cmd.args.set_all.i32, INT32_MIN);
	zassert_equal(cmd.args.set_all.i64, INT64_MIN);
	zassert_false(cmd.args.set_all.flag);
	zassert_equal(cmd.args.set_all.mode, MODE_SAFE);
}

ZTEST(cmd_route, test_info_names_the_target)
{
	struct cmd_route_info info;

	zassert_equal(route("typed_app nominal_only 7", MODE_NOMINAL, &info), CMD_ROUTE_OK);
	zassert_equal(info.app, APP_ID_TYPED_APP);
	zassert_equal(info.command, TYPED_APP_CMD_NOMINAL_ONLY);
	zassert_equal(next_typed_command().args.nominal_only.level, 7);

	/* Known app and command, but not allowed in safe mode. */
	zassert_equal(route("typed_app nominal_only 7", MODE_SAFE, &info), CMD_ROUTE_MODE);
	zassert_equal(info.app, APP_ID_TYPED_APP);
	zassert_equal(info.command, TYPED_APP_CMD_NOMINAL_ONLY);

	/* Known app, unknown command. */
	zassert_equal(route("typed_app pong", MODE_SAFE, &info), CMD_ROUTE_UNKNOWN_COMMAND);
	zassert_equal(info.app, APP_ID_TYPED_APP);
	zassert_equal(info.command, 0);

	zassert_equal(route("no_such_app ping", MODE_SAFE, &info), CMD_ROUTE_UNKNOWN_APP);
	zassert_equal(info.app, 0);
}

ZTEST(cmd_route, test_rejected_commands_are_not_published)
{
	const struct zbus_channel *chan;
	union typed_app_msg msg;
	struct cmd_route_info info;

	zassert_equal(route("typed_app nominal_only 7", MODE_SAFE, &info), CMD_ROUTE_MODE);
	zassert_equal(route("typed_app nominal_only 256", MODE_NOMINAL, &info), CMD_ROUTE_BAD_ARG);
	zassert_equal(route("typed_app ping extra", MODE_SAFE, &info), CMD_ROUTE_BAD_ARG_COUNT);
	zassert_equal(zbus_sub_wait_msg(&typed_sub, &chan, &msg, K_NO_WAIT), -ENOMSG);
}

ZTEST(cmd_route, test_unknown_mode)
{
	struct cmd_route_info info;

	zassert_equal(route("typed_app ping", MODE_MAX + 1, &info), CMD_ROUTE_MODE);
}

ZTEST(cmd_route, test_busy_app)
{
	/*
	 * typed_app stops handling commands. Once CMD_MAX_PENDING are waiting,
	 * the next is refused rather than queued, so the zbus pool stays within
	 * its budget (DS-07).
	 */
	struct cmd_route_info info;

	for (int i = 0; i < CMD_MAX_PENDING; i++) {
		zassert_equal(route("typed_app ping", MODE_SAFE, &info), CMD_ROUTE_OK);
	}
	zassert_equal(route("typed_app ping", MODE_SAFE, &info), CMD_ROUTE_BUSY);
	zassert_equal(info.app, APP_ID_TYPED_APP);
	zassert_equal(info.command, TYPED_APP_CMD_PING);

	/* Another app is unaffected. */
	zassert_equal(route("frame_manager set_entry_enabled safe 0 true", MODE_SAFE, &info),
		      CMD_ROUTE_OK);
	frame_manager_handled();

	/* Once typed_app catches up, it can be sent commands again. */
	drain(NULL);
	zassert_equal(route("typed_app ping", MODE_SAFE, &info), CMD_ROUTE_OK);
}

ZTEST(cmd_route, test_publish_failure_is_reported)
{
	/*
	 * Here typed_app reports each command handled without taking it from
	 * its queue, so the busy check passes but every accepted command still
	 * holds a zbus buffer. Once the pool is down to the buffer each publish
	 * needs for itself, the copy for typed_sub cannot be made and the
	 * publish fails.
	 */
	struct cmd_route_info info;
	int accepted = 0;

	while (route("typed_app ping", MODE_SAFE, &info) == CMD_ROUTE_OK) {
		typed_handled(1);
		accepted++;
		zassert_true(accepted < CONFIG_ZBUS_MSG_SUBSCRIBER_NET_BUF_POOL_SIZE, "never failed");
	}
	zassert_equal(route("typed_app ping", MODE_SAFE, &info), CMD_ROUTE_PUBLISH_FAILED);
	zassert_equal(accepted, CONFIG_ZBUS_MSG_SUBSCRIBER_NET_BUF_POOL_SIZE - 1);
}
