/*
 * Health tests (DS-43).
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Health runs as it does in flight, against a fake watchdog that counts
 * feeds (fake_watchdog.c). The tests stand in for the frame manager: they
 * publish frame_report_chan, wake health, and receive the commands health
 * sends it, counting them handled only when a test says so.
 *
 * The apps and their stall thresholds are the flight ones, from
 * app_attrs[] in the resource map: the radio is not protected, command
 * ingest is.
 *
 * Built with PROTECTED_STALL (testcase.yaml), only the protected-app test
 * runs: it stops the watchdog for good, so it needs its own boot.
 */

#include <zephyr/drivers/watchdog.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/ztest.h>

#include "app_test.h"
#include "fake_watchdog.h"
#include "msg/common.h"
#include "msg/frame_manager.h"
#include "msg/health.h"
#include "silversat/resource_map.h"

#define WAIT K_SECONDS(1)

static const struct device *const watchdog = DEVICE_DT_GET(WATCHDOG_NODE);

/* ---- Standing in for the frame manager ---------------------------------- */

ZBUS_MSG_SUBSCRIBER_DEFINE(frame_manager_sub);
ZBUS_CHAN_ADD_OBS(frame_manager_cmd_chan, frame_manager_sub, 3);

static struct app_status frame_manager_status;
static uint32_t major_frame;
static uint32_t frame_count;

/*
 * Publish one frame report and wake health for that major frame, as the
 * frame manager does at slot 0.
 */
static void report(uint64_t stuck, uint64_t disabled)
{
	const struct frame_report r = {
		.major_frame = major_frame++,
		.stuck = stuck,
		.disabled = disabled,
	};

	zassert_ok(zbus_chan_pub(&frame_report_chan, &r, K_NO_WAIT));
	zassert_ok(app_test_wake(&health_wakeup_chan, frame_count));
	frame_count += FRAME_SLOTS;
	zassert_ok(app_test_wait_status(&health_status_chan, major_frame, 0, WAIT, NULL));
}

/*
 * The next command health sent the frame manager, if any. Counted as
 * handled only if handle is true, as if the frame manager had stalled.
 */
static bool frame_manager_next(struct frame_manager_cmd *cmd, bool handle)
{
	const struct zbus_channel *chan;
	union frame_manager_msg msg;

	if (zbus_sub_wait_msg(&frame_manager_sub, &chan, &msg, K_NO_WAIT) != 0) {
		return false;
	}
	*cmd = msg.cmd;
	if (handle) {
		frame_manager_status.cmd_accepted++;
		zassert_ok(zbus_chan_pub(&frame_manager_status_chan, &frame_manager_status,
					 K_NO_WAIT));
	}
	return true;
}

static struct health_hk health_hk(void)
{
	struct health_hk hk;

	zassert_ok(zbus_chan_read(&health_hk_chan, &hk, K_MSEC(10)));
	return hk;
}

static uint32_t feeds(void)
{
	return fake_watchdog_state(watchdog).feeds;
}

#define RADIO BIT64(APP_ID_RADIO)
#define COMMAND_INGEST BIT64(APP_ID_COMMAND_INGEST)

static void *setup(void)
{
	k_sleep(K_MSEC(10)); /* let health start and set up the watchdog */
	return NULL;
}

/* Each test starts with every app on time and nothing waiting. */
static void before(void *fixture)
{
	struct frame_manager_cmd cmd;

	ARG_UNUSED(fixture);
	report(0, 0);
	while (frame_manager_next(&cmd, true)) {
	}
}

ZTEST_SUITE(health, NULL, setup, before, NULL, NULL);

#if !defined(PROTECTED_STALL)

/* ---- The watchdog ------------------------------------------------------- */

ZTEST(health, test_watchdog_is_set_up)
{
	struct fake_watchdog_state state = fake_watchdog_state(watchdog);

	zassert_true(state.started);
	zassert_equal(state.timeout_ms, CONFIG_SS_WATCHDOG_TIMEOUT_MS);
	zassert_equal(state.flags, WDT_FLAG_RESET_SOC, "a missed feed resets the SoC");
	zassert_true((state.options & WDT_OPT_PAUSE_HALTED_BY_DBG) != 0,
		     "paused under a debugger, on the flatsat (DS-43)");
	zassert_true(health_hk().watchdog);
}

ZTEST(health, test_fed_once_a_major_frame)
{
	uint32_t before_feeds = feeds();

	report(0, 0);
	report(0, 0);
	zassert_equal(feeds() - before_feeds, 2);
	zassert_equal(health_hk().feeds, feeds());
}

/* ---- A stalled app that isn't protected --------------------------------- */

ZTEST(health, test_stalled_app_is_stopped_at_its_threshold)
{
	struct frame_manager_cmd cmd;
	uint32_t stalls = health_hk().stalls;
	uint8_t threshold = app_attrs[APP_ID_RADIO].stall_threshold;

	zassert_true(threshold > 1, "this test needs a threshold above one");
	for (uint8_t i = 1; i < threshold; i++) {
		report(RADIO, 0);
		zassert_false(frame_manager_next(&cmd, true), "report %u: too early", i);
	}
	report(RADIO, 0);
	zassert_true(frame_manager_next(&cmd, true));
	zassert_equal(cmd.id, FRAME_MANAGER_CMD_SET_APP_ENABLED);
	zassert_equal(cmd.args.set_app_enabled.app, APP_ID_RADIO);
	zassert_false(cmd.args.set_app_enabled.enabled);
	zassert_equal(health_hk().stalls - stalls, 1);
	zassert_true((health_hk().disabled_by_health & RADIO) != 0);

	/* The frame manager stopped it: health doesn't ask again. */
	report(RADIO, RADIO);
	report(RADIO, RADIO);
	zassert_false(frame_manager_next(&cmd, true));
	zassert_equal(health_hk().stalls - stalls, 1, "one stall, counted once");
}

ZTEST(health, test_stalls_must_be_in_a_row)
{
	struct frame_manager_cmd cmd;
	uint8_t threshold = app_attrs[APP_ID_RADIO].stall_threshold;

	/* Behind in every report but one: never enough in a row. */
	for (int round = 0; round < 3; round++) {
		for (uint8_t i = 1; i < threshold; i++) {
			report(RADIO, 0);
		}
		report(0, 0);
	}
	zassert_false(frame_manager_next(&cmd, true));
}

ZTEST(health, test_a_report_is_counted_once)
{
	struct frame_manager_cmd cmd;
	const struct frame_report r = {.major_frame = major_frame, .stuck = RADIO};
	uint8_t threshold = app_attrs[APP_ID_RADIO].stall_threshold;

	/* The same report, read at every wakeup, counts as one. */
	zassert_ok(zbus_chan_pub(&frame_report_chan, &r, K_NO_WAIT));
	major_frame++;
	for (uint8_t i = 0; i < threshold + 2; i++) {
		zassert_ok(app_test_wake(&health_wakeup_chan, frame_count));
		frame_count += FRAME_SLOTS;
		zassert_ok(app_test_wait_status(&health_status_chan, major_frame + i, 0, WAIT,
						NULL));
	}
	major_frame += threshold + 1;
	zassert_false(frame_manager_next(&cmd, true));
}

ZTEST(health, test_started_again_by_the_ground_gets_a_fresh_count)
{
	struct frame_manager_cmd cmd;
	uint8_t threshold = app_attrs[APP_ID_RADIO].stall_threshold;

	for (uint8_t i = 0; i < threshold; i++) {
		report(RADIO, 0);
	}
	zassert_true(frame_manager_next(&cmd, true));
	report(RADIO, RADIO);

	/* The ground starts it again; it is still behind at first. */
	for (uint8_t i = 1; i < threshold; i++) {
		report(RADIO, 0);
		zassert_false(frame_manager_next(&cmd, true), "report %u: too early", i);
	}
	report(RADIO, 0);
	zassert_true(frame_manager_next(&cmd, true), "stopped again after a whole threshold");
}

ZTEST(health, test_keeps_asking_until_the_frame_manager_acts)
{
	struct frame_manager_cmd cmd;
	uint32_t failures = health_hk().action_failures;
	uint32_t stalls = health_hk().stalls;
	uint8_t threshold = app_attrs[APP_ID_RADIO].stall_threshold;

	/*
	 * The frame manager has stalled too: it doesn't handle the command,
	 * so the radio isn't stopped. Health asks at every report, until it
	 * has CMD_MAX_PENDING waiting; after that each try fails and counts.
	 */
	for (uint8_t i = 0; i < threshold; i++) {
		report(RADIO, 0);
	}
	for (int i = 1; i < CMD_MAX_PENDING; i++) {
		report(RADIO, 0);
	}
	report(RADIO, 0);
	zassert_equal(health_hk().action_failures - failures, 1);
	zassert_equal(health_hk().stalls - stalls, 1, "still one stall, however long it lasts");
	for (int i = 0; i < CMD_MAX_PENDING; i++) {
		zassert_true(frame_manager_next(&cmd, false));
	}
	zassert_false(frame_manager_next(&cmd, false));

	/* The frame manager catches up and stops it. */
	frame_manager_status.cmd_accepted += CMD_MAX_PENDING;
	zassert_ok(zbus_chan_pub(&frame_manager_status_chan, &frame_manager_status, K_NO_WAIT));
	report(RADIO, RADIO);
	zassert_false(frame_manager_next(&cmd, true));
}

ZTEST(health, test_unwatched_apps_are_ignored)
{
	struct frame_manager_cmd cmd;
	uint32_t before_feeds = feeds();

	/* The frame manager and health have no threshold (DS-43). */
	zassert_equal(app_attrs[APP_ID_FRAME_MANAGER].stall_threshold, 0);
	for (int i = 0; i < 10; i++) {
		report(BIT64(APP_ID_FRAME_MANAGER) | BIT64(APP_ID_HEALTH), 0);
	}
	zassert_false(frame_manager_next(&cmd, true));
	zassert_equal(feeds() - before_feeds, 10);
}

#else /* PROTECTED_STALL */

/* ---- A stalled protected app --------------------------------------------- */

ZTEST(health, test_protected_stall_stops_the_watchdog)
{
	struct frame_manager_cmd cmd;
	uint8_t threshold = app_attrs[APP_ID_COMMAND_INGEST].stall_threshold;
	uint32_t before_feeds;

	zassert_true(app_attrs[APP_ID_COMMAND_INGEST].protected);
	before_feeds = feeds();
	for (uint8_t i = 1; i < threshold; i++) {
		report(COMMAND_INGEST, 0);
	}
	zassert_equal(feeds() - before_feeds, threshold - 1, "fed until the threshold");

	report(COMMAND_INGEST, 0);
	before_feeds = feeds();
	report(COMMAND_INGEST, 0);
	report(0, 0);
	zassert_equal(feeds(), before_feeds, "never fed again: the watchdog resets");
	zassert_equal(health_hk().reset_app, APP_ID_COMMAND_INGEST);
	zassert_false(frame_manager_next(&cmd, true), "a protected app is never stopped");
}

#endif /* PROTECTED_STALL */
