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

/* Stands in for typed_app, so tests can see what was published. */
ZBUS_MSG_SUBSCRIBER_DEFINE(typed_sub);
ZBUS_CHAN_ADD_OBS(typed_app_cmd_chan, typed_sub, 3);

static enum cmd_route_result route(const char *text, uint8_t mode, struct cmd_route_info *info)
{
	return cmd_route(text, strlen(text), mode, info);
}

/* Take the next command typed_app was sent, or fail. */
static struct typed_app_cmd next_typed_command(void)
{
	const struct zbus_channel *chan;
	union typed_app_msg msg;

	zassert_ok(zbus_sub_wait_msg(&typed_sub, &chan, &msg, K_NO_WAIT), "nothing published");
	zassert_equal_ptr(chan, &typed_app_cmd_chan);
	return msg.cmd;
}

static void drain(void *fixture)
{
	const struct zbus_channel *chan;
	union typed_app_msg msg;

	ARG_UNUSED(fixture);
	while (zbus_sub_wait_msg(&typed_sub, &chan, &msg, K_NO_WAIT) == 0) {
	}
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

ZTEST(cmd_route, test_publish_failure_is_reported)
{
	/*
	 * Nothing reads typed_sub here, so each accepted command holds a zbus
	 * buffer. Once the pool is down to the buffer each publish needs for
	 * itself, the copy for typed_sub cannot be made and the publish fails.
	 */
	struct cmd_route_info info;
	int accepted = 0;

	while (route("typed_app ping", MODE_SAFE, &info) == CMD_ROUTE_OK) {
		accepted++;
		zassert_true(accepted < CONFIG_ZBUS_MSG_SUBSCRIBER_NET_BUF_POOL_SIZE, "never failed");
	}
	zassert_equal(route("typed_app ping", MODE_SAFE, &info), CMD_ROUTE_PUBLISH_FAILED);
	zassert_equal(accepted, CONFIG_ZBUS_MSG_SUBSCRIBER_NET_BUF_POOL_SIZE - 1);
}
