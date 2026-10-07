/*
 * Health tests (DS-43).
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Health runs as it does in flight, against a fake watchdog that counts
 * feeds (fake_watchdog.c). The tests stand in for the frame manager: they
 * publish frame_report_chan, wake health, and receive the commands health
 * sends it, counting them handled only when a test says so. They stand in
 * for the mode manager too, receiving health's requests (request_mode).
 *
 * Stall thresholds come from app_attrs[] in the resource map. Nearly every
 * flight app health watches is protected, so the tests use watched_app, a
 * test app with its own row (src/test_app_attrs.h), for an app health
 * stops, critical_app for a critical one (DS-41), and auto_app for one health
 * restarts by itself (REENABLE_AUTO, DS-43); command ingest stands for a
 * protected app. The nvm app runs too, so health finds two app threads for
 * the stack high-water marks (DS-44).
 *
 * Built with PROTECTED_STALL (testcase.yaml), only the protected-app test
 * runs: it stops the watchdog for good, so it needs its own boot. Built
 * with AFTER_RESET, FRAM holds the checkpoint and boot log of earlier runs
 * before health's boot work, as after a reset: two short runs, one short of
 * a reset loop. Built with RESET_LOOP=n too, the boot log shows n short runs
 * in a row (DS-44), then one exactly at the limit, and only the boot and
 * reset-loop tests run.
 */

#include <zephyr/drivers/hwinfo.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/ztest.h>

#include "app_test.h"
#include "fake_watchdog.h"
#include "msg/common.h"
#include "msg/frame_manager.h"
#include "msg/health.h"
#include "msg/mode_manager.h"
#include "nvm/health.h"
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

/* ---- Standing in for the mode manager ----------------------------------- */

ZBUS_MSG_SUBSCRIBER_DEFINE(mode_manager_sub);
ZBUS_CHAN_ADD_OBS(mode_manager_cmd_chan, mode_manager_sub, 3);

static struct app_status mode_manager_status;

/* The next request health sent the mode manager, if any; handled only if handle is true. */
static bool mode_manager_next(struct mode_manager_cmd *cmd, bool handle)
{
	const struct zbus_channel *chan;
	union mode_manager_msg msg;

	if (zbus_sub_wait_msg(&mode_manager_sub, &chan, &msg, K_NO_WAIT) != 0) {
		return false;
	}
	*cmd = msg.cmd;
	if (handle) {
		mode_manager_status.cmd_accepted++;
		zassert_ok(zbus_chan_pub(&mode_manager_status_chan, &mode_manager_status,
					 K_NO_WAIT));
	}
	return true;
}

#if !defined(RESET_LOOP) && !defined(PROTECTED_STALL)
/* The next request should be for safe mode, for this reason. */
static void expect_safe_request(uint8_t reason)
{
	struct mode_manager_cmd cmd;

	zassert_true(mode_manager_next(&cmd, true), "a request for reason %u", reason);
	zassert_equal(cmd.id, MODE_MANAGER_CMD_REQUEST_MODE);
	zassert_equal(cmd.args.request_mode.mode, MODE_SAFE);
	zassert_equal(cmd.args.request_mode.reason, reason);
}
#endif

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

#define WATCHED BIT64(APP_ID_WATCHED_APP)
#define CRITICAL BIT64(APP_ID_CRITICAL_APP)
#define AUTO BIT64(APP_ID_AUTO_APP)
#define COOLDOWN CONFIG_SS_REENABLE_COOLDOWN_FRAMES
#define COMMAND_INGEST BIT64(APP_ID_COMMAND_INGEST)

/* ---- Boot (DS-25, DS-44) ------------------------------------------------- */

#if defined(AFTER_RESET)
/* The run before ended with this checkpoint: boot 41, a stall in command ingest. */
static const struct nvm_run_checkpoint earlier = {
	.boot_number = 41,
	.met_ms = 1000000,
	.uptime_ms = 5000,
	.reset_app = APP_ID_COMMAND_INGEST,
};

#define SHORT_MS 5000 /* well under CONFIG_SS_RESET_LOOP_RUN_MINUTES */
/* Exactly the limit: not short. */
#define LONG_MS ((int64_t)CONFIG_SS_RESET_LOOP_RUN_MINUTES * 60 * 1000)

/* Boot `number`'s log entry: the run before it lasted run_ms. */
static int log_boot(uint32_t number, int64_t run_ms)
{
	const struct nvm_boot_log entry = {.previous_run_ms = run_ms};

	return nvm_boot_log_write(number, &entry);
}

static int seed(void)
{
	int rc = nvm_run_checkpoint_write(&earlier); /* run 41: short */

#if defined(RESET_LOOP)
	/* Runs 41 back to 42 - RESET_LOOP short, and the one before that long. */
	for (uint32_t boot = 41; boot > 42 - RESET_LOOP; boot--) {
		rc |= log_boot(boot, SHORT_MS);
	}
	rc |= log_boot(42 - RESET_LOOP, LONG_MS);
#else
	/*
	 * Run 40 short too. Boot 40's place holds boot 24's entry, 16 boots
	 * older, also short: it must not count, so this is two short runs.
	 */
	rc |= log_boot(41, SHORT_MS);
	rc |= log_boot(24, SHORT_MS);
#endif
	return rc;
}

/* After the FRAM service starts (90), before health's boot work (95). */
SYS_INIT(seed, APPLICATION, 91);
#define BOOT 42
#define MET_AT_BOOT 1000000
#if defined(RESET_LOOP)
/* Counted up to CONFIG_SS_RESET_LOOP_STOP_RUNS: no response needs more. */
#define SHORT_RUNS MIN(RESET_LOOP, CONFIG_SS_RESET_LOOP_STOP_RUNS)
#else
#define SHORT_RUNS 2
#endif
#else
#define BOOT 1
#define MET_AT_BOOT 0
#define SHORT_RUNS 0
#endif

/*
 * What health sent in its first major frames, recorded by setup(): a boot's
 * work happens once per run, so the tests can't each repeat it.
 */
static struct {
	uint32_t requests;     /* request_mode, each for safe mode */
	uint8_t last_reason;
	uint64_t stopped;      /* set_app_enabled false: bit n is app n */
	uint32_t action_failures;
} at_boot;

static struct nvm_run_checkpoint checkpoint_now(void)
{
	struct nvm_run_checkpoint checkpoint;

	zassert_ok(nvm_run_checkpoint_read(&checkpoint));
	return checkpoint;
}

static void *setup(void)
{
	struct mode_manager_cmd request;
	struct frame_manager_cmd cmd;

	k_sleep(K_MSEC(10)); /* let health start and set up the watchdog */

	/*
	 * Three major frames, the frame manager and the mode manager handling
	 * everything: enough for a reset loop's stops to all go, CMD_MAX_PENDING
	 * a major frame.
	 */
	for (int i = 0; i < 3; i++) {
		report(0, 0);
		while (mode_manager_next(&request, true)) {
			zassert_equal(request.id, MODE_MANAGER_CMD_REQUEST_MODE);
			zassert_equal(request.args.request_mode.mode, MODE_SAFE);
			at_boot.requests++;
			at_boot.last_reason = request.args.request_mode.reason;
		}
		while (frame_manager_next(&cmd, true)) {
			zassert_equal(cmd.id, FRAME_MANAGER_CMD_SET_APP_ENABLED);
			zassert_false(cmd.args.set_app_enabled.enabled);
			at_boot.stopped |= BIT64(cmd.args.set_app_enabled.app);
		}
	}
	at_boot.action_failures = health_hk().action_failures;
	return NULL;
}

/* Each test starts with every app on time and nothing waiting. */
static void before(void *fixture)
{
	struct frame_manager_cmd cmd;
	struct mode_manager_cmd request;

	ARG_UNUSED(fixture);
	report(0, 0);
	while (frame_manager_next(&cmd, true)) {
	}
	while (mode_manager_next(&request, true)) {
	}
}

ZTEST_SUITE(health, NULL, setup, before, NULL, NULL);

ZTEST(health, test_boot_number_and_mission_time)
{
	struct mission_time time;

	zassert_equal(health_hk().boot_number, BOOT);
	zassert_ok(zbus_chan_read(&mission_time_chan, &time, K_MSEC(10)));
	zassert_equal(time.boot_number, BOOT);
	zassert_equal(time.met_at_boot_ms, MET_AT_BOOT, "MET carries on from the checkpoint");
}

ZTEST(health, test_this_boot_is_logged)
{
	struct nvm_boot_log entry;
	uint32_t number;

	zassert_ok(nvm_boot_log_read(BOOT % 16, &entry, &number));
	zassert_equal(number, BOOT);
	zassert_equal(entry.met_at_boot_ms, MET_AT_BOOT);
	zassert_equal(entry.reset_cause, health_hk().reset_cause);
#if defined(AFTER_RESET)
	zassert_equal(entry.previous_run_ms, earlier.uptime_ms, "the last run's length");
	zassert_equal(entry.reset_app, APP_ID_COMMAND_INGEST, "the app that caused the reset");
#else
	zassert_equal(entry.previous_run_ms, 0);
	zassert_equal(entry.reset_app, 0);
#endif
}

ZTEST(health, test_short_runs_are_counted_from_the_boot_log)
{
	zassert_equal(health_hk().short_runs, SHORT_RUNS);
}

ZTEST(health, test_the_reset_cause_is_read_and_cleared)
{
	uint32_t cause;

	/* Cleared at boot, so the next boot sees only its own reset's cause. */
	zassert_ok(hwinfo_get_reset_cause(&cause));
	zassert_equal(cause, 0);
}

ZTEST(health, test_a_checkpoint_each_major_frame)
{
	struct nvm_run_checkpoint before = checkpoint_now();
	struct nvm_run_checkpoint after;

	k_sleep(K_MSEC(5));
	report(0, 0);
	after = checkpoint_now();
	zassert_equal(after.boot_number, BOOT);
	zassert_true(after.uptime_ms > before.uptime_ms);
	/*
	 * The tick's MET, as health got it. These test ticks carry MET equal
	 * to uptime; adding MET at boot is the frame manager's job.
	 */
	zassert_equal(after.met_ms, after.uptime_ms);
	zassert_equal(after.reset_app, 0);
}

#if defined(RESET_LOOP)

/* ---- A reset loop (DS-44) ----------------------------------------------- */

ZTEST(health, test_a_reset_loop_still_feeds_the_watchdog)
{
	uint32_t before_feeds = feeds();

	/* Its response must not reset the spacecraft again. */
	report(0, 0);
	report(0, 0);
	zassert_equal(feeds() - before_feeds, 2);
}

ZTEST(health, test_a_reset_loop_asks_for_safe_mode_once)
{
	zassert_equal(at_boot.requests, 1);
	zassert_equal(at_boot.last_reason, MODE_REASON_RESET_LOOP);
	zassert_equal(health_hk().mode_requests, 1);
}

#if CONFIG_SS_STACK_MARGIN_BYTES > 8192
ZTEST(health, test_a_stack_below_the_margin_is_flagged)
{
	/* This build's margin is larger than any app's stack (testcase.yaml). */
	report(0, 0);
	zassert_true(health_hk().stack_low);
}
#endif

#if RESET_LOOP >= CONFIG_SS_RESET_LOOP_STOP_RUNS
ZTEST(health, test_a_reset_loop_stop_is_never_undone_automatically)
{
	struct frame_manager_cmd cmd;

	zassert_true((at_boot.stopped & AUTO) != 0, "auto_app was stopped for the loop");
	for (int i = 0; i < COOLDOWN + 2; i++) {
		report(0, AUTO);
	}
	zassert_false(frame_manager_next(&cmd, true), "only the ground starts it again");
	zassert_equal(health_hk().auto_reenables, 0);
}

ZTEST(health, test_a_long_reset_loop_stops_every_app_that_isnt_protected)
{
	uint64_t expected = 0;

	for (uint8_t app = 1; app <= APP_ID_MAX; app++) {
		if (!app_attrs[app].protected && app_attrs[app].stall_threshold > 0) {
			expected |= BIT64(app);
		}
	}
	zassert_true((expected & WATCHED) != 0 && (expected & CRITICAL) != 0 &&
			     (expected & BIT64(APP_ID_NVM)) != 0,
		     "the test apps and nvm");
	zassert_equal(at_boot.stopped, expected, "every watched app that isn't protected");
	zassert_equal(health_hk().disabled_by_health, expected);
	zassert_equal(at_boot.requests, 1, "a stopped critical app adds no app_failure request");

	/*
	 * More stops than CMD_MAX_PENDING: the rest waited a major frame,
	 * each wait counted.
	 */
	BUILD_ASSERT(CMD_MAX_PENDING == 2, "the stops take more than one major frame");
	zassert_true(at_boot.action_failures >= 1);
}
#else
ZTEST(health, test_a_short_reset_loop_stops_nothing)
{
	zassert_equal(at_boot.stopped, 0);
	zassert_equal(health_hk().disabled_by_health, 0);
}
#endif

#elif !defined(PROTECTED_STALL)

ZTEST(health, test_no_reset_loop_no_request)
{
	/* Fewer short runs than CONFIG_SS_RESET_LOOP_SAFE_RUNS. */
	BUILD_ASSERT(SHORT_RUNS < CONFIG_SS_RESET_LOOP_SAFE_RUNS);
	zassert_equal(at_boot.requests, 0);
	zassert_equal(at_boot.stopped, 0);
}

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
	uint8_t threshold = app_attrs[APP_ID_WATCHED_APP].stall_threshold;

	zassert_true(threshold > 1, "this test needs a threshold above one");
	for (uint8_t i = 1; i < threshold; i++) {
		report(WATCHED, 0);
		zassert_false(frame_manager_next(&cmd, true), "report %u: too early", i);
	}
	report(WATCHED, 0);
	zassert_true(frame_manager_next(&cmd, true));
	zassert_equal(cmd.id, FRAME_MANAGER_CMD_SET_APP_ENABLED);
	zassert_equal(cmd.args.set_app_enabled.app, APP_ID_WATCHED_APP);
	zassert_false(cmd.args.set_app_enabled.enabled);
	zassert_equal(health_hk().stalls - stalls, 1);
	zassert_true((health_hk().disabled_by_health & WATCHED) != 0);

	/* The frame manager stopped it: health doesn't ask again. */
	report(WATCHED, WATCHED);
	report(WATCHED, WATCHED);
	zassert_false(frame_manager_next(&cmd, true));
	zassert_equal(health_hk().stalls - stalls, 1, "one stall, counted once");
}

ZTEST(health, test_stalls_must_be_in_a_row)
{
	struct frame_manager_cmd cmd;
	uint8_t threshold = app_attrs[APP_ID_WATCHED_APP].stall_threshold;

	/* Behind in every report but one: never enough in a row. */
	for (int round = 0; round < 3; round++) {
		for (uint8_t i = 1; i < threshold; i++) {
			report(WATCHED, 0);
		}
		report(0, 0);
	}
	zassert_false(frame_manager_next(&cmd, true));
}

ZTEST(health, test_a_report_is_counted_once)
{
	struct frame_manager_cmd cmd;
	const struct frame_report r = {.major_frame = major_frame, .stuck = WATCHED};
	uint8_t threshold = app_attrs[APP_ID_WATCHED_APP].stall_threshold;

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
	uint8_t threshold = app_attrs[APP_ID_WATCHED_APP].stall_threshold;

	for (uint8_t i = 0; i < threshold; i++) {
		report(WATCHED, 0);
	}
	zassert_true(frame_manager_next(&cmd, true));
	report(WATCHED, WATCHED);

	/* The ground starts it again; it is still behind at first. */
	for (uint8_t i = 1; i < threshold; i++) {
		report(WATCHED, 0);
		zassert_false(frame_manager_next(&cmd, true), "report %u: too early", i);
	}
	report(WATCHED, 0);
	zassert_true(frame_manager_next(&cmd, true), "stopped again after a whole threshold");
}

ZTEST(health, test_keeps_asking_until_the_frame_manager_acts)
{
	struct frame_manager_cmd cmd;
	uint32_t failures = health_hk().action_failures;
	uint32_t stalls = health_hk().stalls;
	uint8_t threshold = app_attrs[APP_ID_WATCHED_APP].stall_threshold;

	/*
	 * The frame manager has stalled too: it doesn't handle the command,
	 * so the radio isn't stopped. Health asks at every report, until it
	 * has CMD_MAX_PENDING waiting; after that each try fails and counts.
	 */
	for (uint8_t i = 0; i < threshold; i++) {
		report(WATCHED, 0);
	}
	for (int i = 1; i < CMD_MAX_PENDING; i++) {
		report(WATCHED, 0);
	}
	report(WATCHED, 0);
	zassert_equal(health_hk().action_failures - failures, 1);
	zassert_equal(health_hk().stalls - stalls, 1, "still one stall, however long it lasts");
	for (int i = 0; i < CMD_MAX_PENDING; i++) {
		zassert_true(frame_manager_next(&cmd, false));
	}
	zassert_false(frame_manager_next(&cmd, false));

	/* The frame manager catches up and stops it. */
	frame_manager_status.cmd_accepted += CMD_MAX_PENDING;
	zassert_ok(zbus_chan_pub(&frame_manager_status_chan, &frame_manager_status, K_NO_WAIT));
	report(WATCHED, WATCHED);
	zassert_false(frame_manager_next(&cmd, true));
}

/* ---- A critical app (DS-41) ---------------------------------------------- */

ZTEST(health, test_a_stopped_critical_app_asks_for_safe_mode)
{
	struct frame_manager_cmd cmd;
	struct mode_manager_cmd request;
	uint32_t requests = health_hk().mode_requests;
	uint8_t threshold = app_attrs[APP_ID_CRITICAL_APP].stall_threshold;

	zassert_true(app_attrs[APP_ID_CRITICAL_APP].critical);
	for (uint8_t i = 1; i < threshold; i++) {
		report(CRITICAL, 0);
	}
	zassert_false(mode_manager_next(&request, true), "not before it is stopped");
	report(CRITICAL, 0);
	zassert_true(frame_manager_next(&cmd, true));
	zassert_equal(cmd.args.set_app_enabled.app, APP_ID_CRITICAL_APP);
	expect_safe_request(MODE_REASON_APP_FAILURE);
	zassert_equal(health_hk().mode_requests - requests, 1);

	/* Stopped: nothing more. */
	report(CRITICAL, CRITICAL);
	zassert_false(mode_manager_next(&request, true));
}

ZTEST(health, test_an_app_that_isnt_critical_asks_for_nothing)
{
	struct frame_manager_cmd cmd;
	struct mode_manager_cmd request;
	uint8_t threshold = app_attrs[APP_ID_WATCHED_APP].stall_threshold;

	zassert_false(app_attrs[APP_ID_WATCHED_APP].critical);
	for (uint8_t i = 0; i < threshold; i++) {
		report(WATCHED, 0);
	}
	zassert_true(frame_manager_next(&cmd, true), "stopped");
	zassert_false(mode_manager_next(&request, true), "but no safe mode");
	report(WATCHED, WATCHED);
}

ZTEST(health, test_a_busy_mode_manager_gets_the_request_later)
{
	struct frame_manager_cmd cmd;
	struct mode_manager_cmd request;
	uint32_t failures = health_hk().action_failures;
	uint8_t threshold = app_attrs[APP_ID_CRITICAL_APP].stall_threshold;

	/* The mode manager already has CMD_MAX_PENDING requests from health waiting. */
	for (int i = 0; i < CMD_MAX_PENDING; i++) {
		for (uint8_t j = 0; j < threshold; j++) {
			report(CRITICAL, 0);
		}
		zassert_true(frame_manager_next(&cmd, true));
		report(CRITICAL, CRITICAL);
		report(0, 0); /* started again by the ground */
	}
	for (uint8_t j = 0; j < threshold; j++) {
		report(CRITICAL, 0);
	}
	zassert_true(frame_manager_next(&cmd, true), "still stopped");
	zassert_equal(health_hk().action_failures - failures, 1, "the request waits");

	/* Still busy the next major frame: tried and counted again. */
	report(CRITICAL, CRITICAL);
	zassert_equal(health_hk().action_failures - failures, 2);

	/* The mode manager catches up: the request goes at the next major frame. */
	for (int i = 0; i < CMD_MAX_PENDING; i++) {
		expect_safe_request(MODE_REASON_APP_FAILURE);
	}
	report(0, 0);
	expect_safe_request(MODE_REASON_APP_FAILURE);
	report(0, 0);
	zassert_false(mode_manager_next(&request, true), "sent once");
	zassert_equal(health_hk().action_failures - failures, 2);
}

/* ---- Restarting an app by itself (REENABLE_AUTO, DS-43) ---------------- */

/* Report auto_app behind until health stops it; returns with it stopped. */
static void stall_auto_app(void)
{
	struct frame_manager_cmd cmd;
	uint8_t threshold = app_attrs[APP_ID_AUTO_APP].stall_threshold;

	for (uint8_t i = 0; i < threshold; i++) {
		report(AUTO, 0);
	}
	zassert_true(frame_manager_next(&cmd, true));
	zassert_equal(cmd.args.set_app_enabled.app, APP_ID_AUTO_APP);
	zassert_false(cmd.args.set_app_enabled.enabled);
}

/* The major frames of the cooldown, stopped; the last one starts it again. */
static void expect_restart_after_cooldown(void)
{
	struct frame_manager_cmd cmd;

	for (int i = 1; i < COOLDOWN; i++) {
		report(AUTO, AUTO);
		zassert_false(frame_manager_next(&cmd, true), "frame %d: still cooling down", i);
	}
	report(AUTO, AUTO);
	zassert_true(frame_manager_next(&cmd, true), "started again after the cooldown");
	zassert_equal(cmd.args.set_app_enabled.app, APP_ID_AUTO_APP);
	zassert_true(cmd.args.set_app_enabled.enabled);
	report(0, 0); /* the frame manager has started it, and it is on time */
}

ZTEST(health, test_an_auto_app_is_restarted_up_to_its_cap)
{
	struct frame_manager_cmd cmd;
	uint8_t cap = app_attrs[APP_ID_AUTO_APP].auto_retry_cap;
	uint32_t failures;

	zassert_equal(app_attrs[APP_ID_AUTO_APP].reenable, REENABLE_AUTO);
	zassert_equal(cap, 2, "this test restarts it twice");

	/* First stall: stopped, then started again after the cooldown. */
	stall_auto_app();
	zassert_true((health_hk().disabled_by_health & AUTO) != 0);
	expect_restart_after_cooldown();
	zassert_equal(health_hk().auto_reenables, 1);
	zassert_true((health_hk().disabled_by_health & AUTO) == 0, "health undid its stop");

	/*
	 * Second stall, with the frame manager behind: health has
	 * CMD_MAX_PENDING stops waiting there when the restart is due.
	 */
	BUILD_ASSERT(CMD_MAX_PENDING == 2, "two stops fill the frame manager's pending count");
	for (uint8_t i = 0; i < app_attrs[APP_ID_AUTO_APP].stall_threshold + 1; i++) {
		report(AUTO, 0);
	}
	zassert_true(frame_manager_next(&cmd, false));
	zassert_true(frame_manager_next(&cmd, false));
	failures = health_hk().action_failures;
	for (int i = 0; i < COOLDOWN; i++) {
		report(AUTO, AUTO);
	}
	zassert_equal(health_hk().action_failures - failures, 1, "the restart waits");
	zassert_false(frame_manager_next(&cmd, false));
	frame_manager_status.cmd_accepted += CMD_MAX_PENDING;
	zassert_ok(zbus_chan_pub(&frame_manager_status_chan, &frame_manager_status, K_NO_WAIT));
	report(AUTO, AUTO);
	zassert_true(frame_manager_next(&cmd, true), "and goes the next major frame");
	zassert_true(cmd.args.set_app_enabled.enabled);
	report(0, 0);
	zassert_equal(health_hk().auto_reenables, 2);

	/* Third stall: its restarts are used up, so only the ground starts it. */
	stall_auto_app();
	for (int i = 0; i < COOLDOWN + 2; i++) {
		report(AUTO, AUTO);
	}
	zassert_false(frame_manager_next(&cmd, true), "not started again");
	zassert_equal(health_hk().auto_reenables, 2);
	zassert_true((health_hk().auto_exhausted & AUTO) != 0);
	zassert_true((health_hk().disabled_by_health & AUTO) != 0);

	/* The ground starts it: its restarts are renewed, and it is no longer reported stopped. */
	report(0, 0);
	zassert_equal(health_hk().auto_exhausted & AUTO, 0);
	zassert_equal(health_hk().disabled_by_health & AUTO, 0);
	stall_auto_app();
	expect_restart_after_cooldown();
	zassert_equal(health_hk().auto_reenables, 3, "a renewed restart");
}

ZTEST(health, test_a_ground_restart_in_the_cooldown_cancels_healths)
{
	struct frame_manager_cmd cmd;
	uint32_t restarts = health_hk().auto_reenables;

	stall_auto_app();
	report(AUTO, AUTO);
	report(0, 0); /* the ground started it before the cooldown ended */
	for (int i = 0; i < COOLDOWN + 2; i++) {
		report(0, 0);
	}
	zassert_false(frame_manager_next(&cmd, true), "health doesn't start it again");
	zassert_equal(health_hk().auto_reenables, restarts);
	zassert_equal(health_hk().disabled_by_health & AUTO, 0);
}

ZTEST(health, test_a_ground_app_is_never_restarted_by_health)
{
	struct frame_manager_cmd cmd;
	uint8_t threshold = app_attrs[APP_ID_WATCHED_APP].stall_threshold;

	zassert_equal(app_attrs[APP_ID_WATCHED_APP].reenable, REENABLE_GROUND);
	for (uint8_t i = 0; i < threshold; i++) {
		report(WATCHED, 0);
	}
	zassert_true(frame_manager_next(&cmd, true));
	for (int i = 0; i < COOLDOWN + 2; i++) {
		report(WATCHED, WATCHED);
	}
	zassert_false(frame_manager_next(&cmd, true));
	zassert_equal(health_hk().auto_exhausted & WATCHED, 0, "it never had restarts to use up");

	/* The ground starts it: no longer reported stopped by health. */
	zassert_true((health_hk().disabled_by_health & WATCHED) != 0);
	report(0, 0);
	zassert_equal(health_hk().disabled_by_health & WATCHED, 0);
}

/* ---- Stack high-water marks (DS-44) -------------------------------------- */

ZTEST(health, test_each_app_threads_stack_is_read)
{
	struct health_hk hk;
	size_t size;

	report(0, 0);
	hk = health_hk();
	zassert_equal(hk.stack_apps, 2, "health's thread and nvm's, found by name");
	zassert_true(hk.stack_min_app == APP_ID_HEALTH || hk.stack_min_app == APP_ID_NVM,
		     "app %u", hk.stack_min_app);
	size = hk.stack_min_app == APP_ID_HEALTH ? HEALTH_STACK_SIZE : NVM_STACK_SIZE;
	zassert_true(hk.stack_min_unused > 0 && hk.stack_min_unused < size,
		     "%u of %u bytes unused", hk.stack_min_unused, (unsigned int)size);
	zassert_false(hk.stack_low, "well inside the margin");
}

/* The threads, to read their stacks directly. A test may; an app may not (DS-12). */
extern const k_tid_t health_tid;
extern const k_tid_t nvm_tid;

ZTEST(health, test_the_least_stack_left_is_reported)
{
	size_t health_unused;
	size_t nvm_unused;

	/*
	 * Health's stack goes deepest after its scan, as it publishes. A second
	 * major frame's scan has seen that, so the marks no longer move.
	 */
	report(0, 0);
	report(0, 0);
	zassert_ok(k_thread_stack_space_get(health_tid, &health_unused));
	zassert_ok(k_thread_stack_space_get(nvm_tid, &nvm_unused));
	zassert_equal(health_hk().stack_min_unused, MIN(health_unused, nvm_unused));
	zassert_equal(health_hk().stack_min_app,
		      health_unused <= nvm_unused ? APP_ID_HEALTH : APP_ID_NVM);
}

ZTEST(health, test_the_radio_is_never_stopped)
{
	struct frame_manager_cmd cmd;

	/*
	 * It carries the ground's commands in: stopped, it could never be told
	 * to start again. A radio stall resets instead (DS-43).
	 */
	zassert_true(app_attrs[APP_ID_RADIO].protected);
	zassert_true(app_attrs[APP_ID_COMMAND_INGEST].protected);
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
	zassert_equal(checkpoint_now().reset_app, APP_ID_COMMAND_INGEST,
		      "stored with that frame's checkpoint, so the next boot's log names it");
	zassert_false(frame_manager_next(&cmd, true), "a protected app is never stopped");
}

#endif /* PROTECTED_STALL */
