/*
 * Mode manager tests: booting with the test signal (DS-42).
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Each boot path needs a fresh boot, so this is its own test app;
 * tests/app/mode_manager covers the boot without the signal. Here the test
 * signal (an emulated pin, app.overlay) is present before the mode
 * manager's first wakeup, as if the board had been jumpered on the bench.
 *
 * Built with DEPLOYED (testcase.yaml), FRAM already records a finished
 * deployment, so the boot starts in safe mode rather than deploy: a bench
 * unit after its first deployment must still reach test mode.
 */

#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/gpio/gpio_emul.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/ztest.h>

#include "app_test.h"
#include "msg/common.h"
#include "msg/mode_manager.h"
#include "nvm/mode_manager.h"
#include "silversat/resource_map.h"

#define WAIT K_SECONDS(1)
#define SEPARATION_MS (CONFIG_SS_SEPARATION_DELAY_MINUTES * 60LL * 1000)

static const struct gpio_dt_spec test_signal = GPIO_DT_SPEC_GET(TEST_SIGNAL_NODE, gpios);

static uint32_t frame_count;

/* Wake the mode manager once, as the frame manager would, at this MET. */
static void wake_at(int64_t met_ms)
{
	struct frame_tick tick = app_test_tick(frame_count++);
	struct app_status before;

	tick.met_ms = met_ms;
	zassert_ok(zbus_chan_read(&mode_manager_status_chan, &before, K_MSEC(10)));
	zassert_ok(zbus_chan_pub(&mode_manager_wakeup_chan, &tick, K_NO_WAIT));
	zassert_ok(app_test_wait_status(&mode_manager_status_chan, before.steps + 1, 0, WAIT,
					NULL));
}

/* The ground sets the mode. Returns 1 if accepted, 0 if refused. */
static int set_mode(uint8_t mode)
{
	const struct mode_manager_cmd cmd = {
		.id = MODE_MANAGER_CMD_SET_MODE,
		.args.set_mode = {.mode = mode},
	};
	struct app_status before;
	struct app_status after;

	zassert_ok(zbus_chan_read(&mode_manager_status_chan, &before, K_MSEC(10)));
	zassert_ok(zbus_chan_pub(&mode_manager_cmd_chan, &cmd, K_NO_WAIT));
	zassert_ok(app_test_wait_status(&mode_manager_status_chan, 0,
					before.cmd_accepted + before.cmd_rejected + 1, WAIT,
					&after));
	return (int)(after.cmd_accepted - before.cmd_accepted);
}

static struct mode_state mode_now(void)
{
	struct mode_state state;

	zassert_ok(zbus_chan_read(&mode_chan, &state, K_MSEC(10)));
	return state;
}

/*
 * The mode the ground leaves test mode for. Leaving happens once per boot,
 * so testcase.yaml runs this test twice: nominal, and safe.
 */
#ifndef LEAVE_TEST_FOR
#define LEAVE_TEST_FOR MODE_NOMINAL
#endif

/* The mode after the first wakeup, recorded once: a boot happens once. */
static struct mode_state after_first_wakeup;

#if defined(DEPLOYED)
/* Before the mode manager starts: deployment ended on an earlier boot. */
static int seed(void)
{
	const struct nvm_deployment done = {.complete = true};

	return nvm_deployment_write(&done);
}

SYS_INIT(seed, APPLICATION, 95);
#define BOOT_MODE MODE_SAFE
#else
#define BOOT_MODE MODE_DEPLOY
#endif

static void *setup(void)
{
	k_sleep(K_MSEC(10));
	zassert_equal(mode_now().mode, BOOT_MODE, "deploy until deployment has ended once");
	/* The emulator takes a value only on a pin set up as an input. */
	zassert_ok(gpio_pin_configure_dt(&test_signal, GPIO_INPUT));
	zassert_ok(gpio_emul_input_set(test_signal.port, test_signal.pin, 1));
	wake_at(1000);
	after_first_wakeup = mode_now();
	return NULL;
}

ZTEST_SUITE(mode_manager_test_signal, NULL, setup, NULL, NULL, NULL);

ZTEST(mode_manager_test_signal, test_signal_at_boot_enters_test)
{
	zassert_equal(after_first_wakeup.mode, MODE_TEST);
	zassert_equal(after_first_wakeup.reason, MODE_REASON_TEST_SIGNAL);
	zassert_equal(after_first_wakeup.since_met_ms, 1000);
}

ZTEST(mode_manager_test_signal, test_ground_commands_in_test_mode)
{
	/* Test mode skips the separation delay: it doesn't end test mode. */
	wake_at(2 * SEPARATION_MS);
	zassert_equal(mode_now().mode, MODE_TEST);

	/* The ground may leave for safe or nominal, and nothing leads back. */
	zassert_equal(set_mode(LEAVE_TEST_FOR), 1);
	zassert_equal(mode_now().mode, LEAVE_TEST_FOR);
	zassert_equal(mode_now().reason, MODE_REASON_GROUND_COMMAND);
	zassert_equal(set_mode(MODE_TEST), 0, "test mode is entered only at boot");
	zassert_equal(set_mode(MODE_DEPLOY), 0);
}
