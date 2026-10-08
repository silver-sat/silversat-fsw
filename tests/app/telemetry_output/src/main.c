/*
 * Telemetry output tests (DS-10, DS-14, DS-69).
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Telemetry output runs as it does in flight. The tests stand in for the
 * frame manager, waking it with ticks, and for the radio, reading the
 * downlink queue, and raise events as any app would (DS-10). The encoding
 * itself is tested against the ground decoder in tests/unit/libs.
 */

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/ztest.h>

#include "app_test.h"
#include "msg/common.h"
#include "msg/frame_manager.h"
#include "msg/health.h"
#include "msg/nvm.h"
#include "msg/telemetry_output.h"
#include "silversat/event.h"
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

/* Each test starts with the downlink and the event queue empty. */
static void drain(void *fixture)
{
	struct link_frame frame;

	ARG_UNUSED(fixture);
	while (take_packet(&frame)) {
	}
	app_test_drain_events();
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

/* ---- Events (DS-10) --------------------------------------------------------- */

/* The next packet should be an 'E' packet for health's app_stalled, with this app. */
static void expect_event(int32_t app)
{
	struct link_frame frame;
	const uint8_t *data = (const uint8_t *)frame.data;

	zassert_true(take_packet(&frame), "an event packet");
	zassert_equal(data[0], TLM_KIND_EVENT);
	zassert_equal(frame.len, 21);
	zassert_equal(data[9], APP_ID_HEALTH);
	zassert_equal(sys_get_le16(&data[11]), HEALTH_EVENT_APP_STALLED);
	zassert_equal((int32_t)sys_get_le32(&data[13]), app);
}

ZTEST(telemetry_output, test_events_follow_the_housekeeping_oldest_first)
{
	struct link_frame frame;
	uint32_t sent = to_hk().events_sent;

	emit_health_app_stalled(1000, 1);
	emit_health_app_stalled(1000, 2);
	wake();
	zassert_true(take_packet(&frame));
	zassert_equal(frame.data[0], TLM_KIND_HK, "housekeeping first");
	expect_event(1);
	expect_event(2);
	zassert_false(take_packet(&frame));
	zassert_equal(to_hk().events_sent - sent, 2);
}

ZTEST(telemetry_output, test_at_most_a_few_events_a_major_frame)
{
	struct link_frame frame;

	for (int32_t i = 0; i < TLM_EVENTS_PER_FRAME + 2; i++) {
		emit_health_app_stalled(1000, i);
	}
	wake();
	zassert_true(take_packet(&frame));
	for (int32_t i = 0; i < TLM_EVENTS_PER_FRAME; i++) {
		expect_event(i);
	}
	zassert_false(take_packet(&frame), "the rest wait for the next major frame");

	wake();
	zassert_true(take_packet(&frame));
	expect_event(TLM_EVENTS_PER_FRAME);
	expect_event(TLM_EVENTS_PER_FRAME + 1);
}

ZTEST(telemetry_output, test_events_leave_room_for_command_replies)
{
	struct link_frame frame = {.len = 1, .data = "A"};
	struct event event;

	/* After the housekeeping packet, only the reserved places are left. */
	for (int i = 0; i < DOWNLINK_QUEUE_DEPTH - 1 - TLM_DOWNLINK_RESERVE; i++) {
		zassert_ok(k_msgq_put(&downlink_msgq, &frame, K_NO_WAIT));
	}
	emit_health_app_stalled(1000, 9);
	wake();
	zassert_equal(k_msgq_num_free_get(&downlink_msgq), TLM_DOWNLINK_RESERVE);
	zassert_ok(event_take(&event), "the event still waits in its own queue");
	zassert_equal(event.arg0, 9);
}

ZTEST(telemetry_output, test_dropped_events_are_reported)
{
	for (int i = 0; i < EVENT_QUEUE_DEPTH + 2; i++) {
		emit_health_app_stalled(1000, i);
	}
	wake();
	zassert_equal(to_hk().events_dropped, event_stats().dropped);
	zassert_true(to_hk().events_dropped >= 2);
}

/* ---- The tests' own event helpers (app_test.h) ----------------------------- */

ZTEST(telemetry_output, test_find_event_matches_the_app_and_the_number)
{
	struct event event;

	/* Two apps' events with the same number: event numbers are per app. */
	BUILD_ASSERT((int)NVM_EVENT_FRAM_FAILED == (int)HEALTH_EVENT_APP_STALLED);
	emit_nvm_fram_failed(1000);
	emit_health_app_stalled(2000, 7);
	zassert_true(app_test_find_event(APP_ID_HEALTH, HEALTH_EVENT_APP_STALLED, &event));
	zassert_equal(event.app, APP_ID_HEALTH);
	zassert_equal(event.arg0, 7);
	zassert_false(app_test_find_event(APP_ID_HEALTH, HEALTH_EVENT_APP_STALLED, &event),
		      "and the queue is empty after it");
}

ZTEST(telemetry_output, test_drain_events_empties_the_queue)
{
	struct event event;

	emit_health_app_stalled(1000, 1);
	emit_nvm_fram_failed(1000);
	app_test_drain_events();
	zassert_equal(event_take(&event), -ENOMSG);
}

/* A debug event, from no app's YAML: the queue treats every severity alike. */
static void emit_debug(void)
{
	const struct event event = {.met_ms = 1000, .app = APP_ID_HEALTH,
				    .severity = SEVERITY_DEBUG, .id = 999};
	const struct event_text text = {.name = "debug_detail"};

	event_emit(&event, &text);
}

#if defined(CONFIG_SS_EVENT_QUEUE_DEBUG)
ZTEST(telemetry_output, test_debug_events_go_down_in_development)
{
	struct link_frame frame;

	emit_debug();
	wake();
	zassert_true(take_packet(&frame));
	zassert_true(take_packet(&frame), "the debug event");
	zassert_equal(frame.data[0], TLM_KIND_EVENT);
	zassert_equal(frame.data[10], SEVERITY_DEBUG);
}
#else
ZTEST(telemetry_output, test_the_flight_build_keeps_debug_events_off_the_link)
{
	struct link_frame frame;
	struct event_stats start = event_stats();

	emit_debug();
	wake();
	zassert_true(take_packet(&frame), "housekeeping");
	zassert_false(take_packet(&frame), "no debug event");
	zassert_equal(event_stats().queued, start.queued);
	zassert_equal(event_stats().dropped, start.dropped, "left out, not dropped");

	/* Other severities still go. */
	emit_health_app_stalled(1000, 1);
	wake();
	zassert_true(take_packet(&frame));
	expect_event(1);
}
#endif
