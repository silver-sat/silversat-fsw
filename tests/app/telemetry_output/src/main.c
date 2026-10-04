/*
 * Telemetry output tests (DS-10, DS-14, DS-69).
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Telemetry output runs as it does in flight. The tests stand in for the
 * frame manager, waking it with ticks, and for the radio, reading the
 * downlink queue. The encoding itself is tested against the ground decoder
 * in tests/unit/libs.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/ztest.h>

#include "app_test.h"
#include "msg/common.h"
#include "msg/frame_manager.h"
#include "msg/telemetry_output.h"
#include "silversat/link.h"
#include "silversat/resource_map.h"
#include "silversat/tlm_encode.h"

#define WAIT K_SECONDS(1)

static uint32_t frame_count;

/* Wake telemetry output once, as the frame manager would, and wait for it. */
static struct frame_tick wake(void)
{
	struct app_status before;
	struct frame_tick tick = app_test_tick(frame_count++);

	zassert_ok(zbus_chan_read(&telemetry_output_status_chan, &before, K_MSEC(10)));
	zassert_ok(zbus_chan_pub(&telemetry_output_wakeup_chan, &tick, K_NO_WAIT));
	zassert_ok(app_test_wait_status(&telemetry_output_status_chan, before.steps + 1, 0, WAIT,
					NULL));
	return tick;
}

static bool take_packet(struct link_frame *frame)
{
	return k_msgq_get(&downlink_msgq, frame, K_NO_WAIT) == 0;
}

static struct telemetry_output_hk to_hk(void)
{
	struct telemetry_output_hk hk;

	zassert_ok(zbus_chan_read(&telemetry_output_hk_chan, &hk, K_MSEC(10)));
	return hk;
}

static void drain(void *fixture)
{
	struct link_frame frame;

	ARG_UNUSED(fixture);
	while (take_packet(&frame)) {
	}
}

ZTEST_SUITE(telemetry_output, NULL, NULL, drain, NULL, NULL);

ZTEST(telemetry_output, test_one_app_per_wakeup_in_turn)
{
	struct link_frame frame;
	size_t first = 0;

	/* Two full rounds: every app in turn, then the same order again. */
	for (size_t i = 0; i < 2 * tlm_hk_app_count; i++) {
		struct frame_tick tick = wake();

		zassert_true(take_packet(&frame), "wakeup %zu sent nothing", i);
		zassert_equal(k_msgq_num_used_get(&downlink_msgq), 0, "one packet per wakeup");
		zassert_equal(frame.data[0], TLM_KIND_HK);
		zassert_equal(sys_get_le64((const uint8_t *)&frame.data[2]), (uint64_t)tick.met_ms,
			      "the packet carries the tick's MET (DS-25)");
		if (i == 0) {
			for (first = 0; tlm_hk_apps[first] != (uint8_t)frame.data[1]; first++) {
			}
		}
		zassert_equal((uint8_t)frame.data[1],
			      tlm_hk_apps[(first + i) % tlm_hk_app_count], "wakeup %zu", i);
	}
}

ZTEST(telemetry_output, test_sends_the_latest_housekeeping)
{
	const struct frame_manager_hk latest = {.frame_count = 1234567};
	struct link_frame frame;

	zassert_ok(zbus_chan_pub(&frame_manager_hk_chan, &latest, K_NO_WAIT));
	for (size_t i = 0; i < tlm_hk_app_count; i++) {
		wake();
		zassert_true(take_packet(&frame));
		if ((uint8_t)frame.data[1] == APP_ID_FRAME_MANAGER) {
			/* frame_count is the first field, right after the header. */
			zassert_equal(sys_get_le32((const uint8_t *)&frame.data[TLM_HK_HEADER_LEN]),
				      1234567);
			return;
		}
	}
	ztest_test_fail();
}

ZTEST(telemetry_output, test_counts_what_it_sends)
{
	struct telemetry_output_hk before = to_hk();

	wake();
	wake();
	zassert_equal(to_hk().sent - before.sent, 2, "it publishes its own housekeeping too");
}

ZTEST(telemetry_output, test_full_downlink_drops_and_counts)
{
	struct telemetry_output_hk before = to_hk();
	struct link_frame filler = {.len = 1, .data = "x"};

	for (int i = 0; i < DOWNLINK_QUEUE_DEPTH; i++) {
		zassert_ok(k_msgq_put(&downlink_msgq, &filler, K_NO_WAIT));
	}
	wake();
	zassert_equal(to_hk().dropped - before.dropped, 1);
	zassert_equal(to_hk().sent, before.sent);
}

ZTEST(telemetry_output, test_own_command_channel_rejects)
{
	const struct telemetry_output_cmd cmd = {.id = 1};
	struct app_status before;
	struct app_status after;

	zassert_ok(zbus_chan_read(&telemetry_output_status_chan, &before, K_MSEC(10)));
	zassert_ok(zbus_chan_pub(&telemetry_output_cmd_chan, &cmd, K_NO_WAIT));
	zassert_ok(app_test_wait_status(&telemetry_output_status_chan, 0,
					before.cmd_accepted + before.cmd_rejected + 1, WAIT,
					&after));
	zassert_equal(after.cmd_rejected - before.cmd_rejected, 1);
}
