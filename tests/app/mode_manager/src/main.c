/*
 * Mode manager tests (DS-40, DS-41, DS-42, DS-46).
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The mode manager runs as it does in flight. The tests stand in for:
 *   - the frame manager: they wake it with ticks whose MET they choose, so
 *     a week passes in one call;
 *   - command ingest: they publish ground_contact_chan, as if a command had
 *     been accepted, and send set_mode on the command channel;
 *   - the radio: they receive its command channel, and count commands
 *     handled on its status channel only when a test says so.
 */

#include <errno.h>

#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/gpio/gpio_emul.h>
#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/ztest.h>

#include "app_test.h"
#include "msg/common.h"
#include "msg/mode_manager.h"
#include "msg/radio.h"
#include "nvm/mode_manager.h"
#include "silversat/resource_map.h"

#define WAIT K_SECONDS(1)

#define HOUR_MS (60LL * 60 * 1000)
#define TIMEOUT_MS (CONFIG_SS_COMMAND_LOSS_TIMEOUT_HOURS * HOUR_MS)
#define SEPARATION_MS (CONFIG_SS_SEPARATION_DELAY_MINUTES * 60LL * 1000)

/* The test signal, an emulated pin (app.overlay). It starts absent. */
static const struct gpio_dt_spec test_signal = GPIO_DT_SPEC_GET(TEST_SIGNAL_NODE, gpios);

/* ---- Standing in for the radio ----------------------------------------- */

ZBUS_MSG_SUBSCRIBER_DEFINE(radio_sub);
ZBUS_CHAN_ADD_OBS(radio_cmd_chan, radio_sub, 3);

static struct app_status radio_status;

/*
 * The next command the mode manager sent the radio, if any. Counts it as
 * handled only if handle is true; otherwise it stays pending, as if the
 * radio had stalled.
 */
static bool radio_next(struct radio_cmd *cmd, bool handle)
{
	const struct zbus_channel *chan;
	union radio_msg msg;

	if (zbus_sub_wait_msg(&radio_sub, &chan, &msg, K_NO_WAIT) != 0) {
		return false;
	}
	*cmd = msg.cmd;
	if (handle) {
		radio_status.cmd_accepted++;
		zassert_ok(zbus_chan_pub(&radio_status_chan, &radio_status, K_NO_WAIT));
	}
	return true;
}

/* The radio catches up on everything it was sent but didn't handle. */
static void radio_catch_up(uint32_t unhandled)
{
	struct radio_cmd cmd;

	while (radio_next(&cmd, true)) {
	}
	radio_status.cmd_accepted += unhandled;
	zassert_ok(zbus_chan_pub(&radio_status_chan, &radio_status, K_NO_WAIT));
}

/* ---- Standing in for the frame manager and command ingest -------------- */

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

/* Command ingest accepted a command at this MET. */
static void contact_at(int64_t met_ms)
{
	const struct ground_contact contact = {.contacted = true, .last_accepted_met_ms = met_ms};

	zassert_ok(zbus_chan_pub(&ground_contact_chan, &contact, K_NO_WAIT));
}

/* Send one command and wait for it. Returns 1 if accepted, 0 if rejected. */
static int command(const struct mode_manager_cmd *cmd)
{
	struct app_status before;
	struct app_status after;

	zassert_ok(zbus_chan_read(&mode_manager_status_chan, &before, K_MSEC(10)));
	zassert_ok(zbus_chan_pub(&mode_manager_cmd_chan, cmd, K_NO_WAIT));
	zassert_ok(app_test_wait_status(&mode_manager_status_chan, 0,
					before.cmd_accepted + before.cmd_rejected + 1, WAIT,
					&after));
	return (int)(after.cmd_accepted - before.cmd_accepted);
}

static int set_mode(uint8_t mode)
{
	const struct mode_manager_cmd cmd = {
		.id = MODE_MANAGER_CMD_SET_MODE,
		.args.set_mode = {.mode = mode},
	};

	return command(&cmd);
}

static struct mode_state mode_now(void)
{
	struct mode_state state;

	zassert_ok(zbus_chan_read(&mode_chan, &state, K_MSEC(10)));
	return state;
}

static struct mode_manager_hk mm_hk(void)
{
	struct mode_manager_hk hk;

	zassert_ok(zbus_chan_read(&mode_manager_hk_chan, &hk, K_MSEC(10)));
	return hk;
}

/*
 * Each test gets its own stretch of mission time, far from every other
 * test's, so a contact or a silence from one test can't reach the next.
 */
static int64_t epoch;

/*
 * What happened between boot and safe mode, recorded once by setup() and
 * checked by the test_boot_* tests. A boot happens only once per run, so
 * the tests can't each repeat it.
 */
static struct {
	struct mode_state at_boot;
	int ground_set_safe_in_deploy;
	uint8_t after_first_wakeup;
	uint8_t after_late_signal;
	uint8_t just_before_delay;
	struct mode_state at_delay;
} boot;

static void *setup(void)
{
	/* Let the mode manager's thread start. */
	k_sleep(K_MSEC(10));
	boot.at_boot = mode_now();
	boot.ground_set_safe_in_deploy = set_mode(MODE_SAFE);

	/* The first wakeup reads the test signal, absent here. */
	wake_at(1000);
	boot.after_first_wakeup = mode_now().mode;

	/* The signal appearing later must change nothing. */
	zassert_ok(gpio_emul_input_set(test_signal.port, test_signal.pin, 1));
	wake_at(2000);
	boot.after_late_signal = mode_now().mode;
	zassert_ok(gpio_emul_input_set(test_signal.port, test_signal.pin, 0));

	wake_at(SEPARATION_MS - 1);
	boot.just_before_delay = mode_now().mode;
	wake_at(SEPARATION_MS);
	boot.at_delay = mode_now();
	return NULL;
}

/*
 * Each test starts in safe mode, with the radio caught up and no contact
 * with the ground, so an earlier test's contact can't fire the timer.
 */
static void before(void *fixture)
{
	const struct ground_contact never = {0};

	ARG_UNUSED(fixture);
	epoch += 1000 * 24 * HOUR_MS;
	zassert_ok(zbus_chan_pub(&ground_contact_chan, &never, K_NO_WAIT));
	zassert_equal(set_mode(MODE_SAFE), 1);
	radio_catch_up(0);
}

ZTEST_SUITE(mode_manager, NULL, setup, before, NULL, NULL);

/* ---- Boot: deploy, then safe (DS-42) ------------------------------------- */

ZTEST(mode_manager, test_boot_starts_in_deploy)
{
	zassert_equal(boot.at_boot.mode, MODE_DEPLOY);
	zassert_equal(boot.at_boot.reason, MODE_REASON_BOOT);
	zassert_equal(boot.ground_set_safe_in_deploy, 0, "no ground command ends deploy early");
}

ZTEST(mode_manager, test_boot_test_signal_is_read_only_at_boot)
{
	zassert_equal(boot.after_first_wakeup, MODE_DEPLOY, "no signal: no test mode");
	zassert_equal(boot.after_late_signal, MODE_DEPLOY,
		      "a signal after boot, a glitch in flight, is ignored");
}

ZTEST(mode_manager, test_boot_safe_after_the_separation_delay)
{
	zassert_equal(boot.just_before_delay, MODE_DEPLOY, "a millisecond early: still deploy");
	zassert_equal(boot.at_delay.mode, MODE_SAFE);
	zassert_equal(boot.at_delay.reason, MODE_REASON_DEPLOYMENT_COMPLETE);
	zassert_equal(boot.at_delay.since_met_ms, SEPARATION_MS);
}

ZTEST(mode_manager, test_nothing_leads_back_to_deploy_or_test)
{
	uint32_t refused = mm_hk().refused;

	zassert_equal(set_mode(MODE_DEPLOY), 0);
	zassert_equal(set_mode(MODE_TEST), 0);
	zassert_equal(set_mode(MODE_NOMINAL), 1);
	zassert_equal(set_mode(MODE_DEPLOY), 0);
	zassert_equal(set_mode(MODE_TEST), 0);
	zassert_equal(mm_hk().refused - refused, 4);
}

/* ---- Ground commands and the transition table --------------------------- */

ZTEST(mode_manager, test_ground_sets_nominal_and_safe)
{
	uint32_t transitions = mm_hk().transitions;
	struct mode_state state;

	wake_at(epoch + 5000);
	zassert_equal(set_mode(MODE_NOMINAL), 1);
	state = mode_now();
	zassert_equal(state.mode, MODE_NOMINAL);
	zassert_equal(state.reason, MODE_REASON_GROUND_COMMAND);
	zassert_equal(state.since_met_ms, epoch + 5000, "timed from the latest tick (DS-25)");

	zassert_equal(set_mode(MODE_SAFE), 1);
	zassert_equal(mode_now().mode, MODE_SAFE);
	zassert_equal(mm_hk().transitions - transitions, 2);
	zassert_equal(mm_hk().mode, MODE_SAFE, "housekeeping follows at once (DS-14)");
}

ZTEST(mode_manager, test_setting_the_current_mode_changes_nothing)
{
	uint32_t transitions = mm_hk().transitions;
	struct mode_state state = mode_now();

	/* Sets, doesn't toggle (DS-35): accepted, and nothing happens. */
	zassert_equal(set_mode(MODE_SAFE), 1);
	zassert_equal(mm_hk().transitions, transitions);
	zassert_equal(mode_now().since_met_ms, state.since_met_ms);
}

ZTEST(mode_manager, test_unknown_mode_is_refused)
{
	uint32_t refused = mm_hk().refused;

	zassert_equal(set_mode(MODE_MAX + 1), 0);
	zassert_equal(mode_now().mode, MODE_SAFE);
	zassert_equal(mm_hk().refused - refused, 1);
}

ZTEST(mode_manager, test_unknown_command_is_rejected)
{
	const struct mode_manager_cmd cmd = {.id = 99};

	zassert_equal(command(&cmd), 0);
	zassert_equal(mode_now().mode, MODE_SAFE);
}

ZTEST(mode_manager, test_mode_is_published_every_major_frame)
{
	/*
	 * Republished each wakeup, so a publish that failed is repaired. Show
	 * it by overwriting mode_chan, as a failed publish would leave it.
	 */
	const struct mode_state wrong = {.mode = MODE_NOMINAL};

	zassert_ok(zbus_chan_pub(&mode_chan, &wrong, K_NO_WAIT));
	wake_at(epoch);
	zassert_equal(mode_now().mode, MODE_SAFE);
}

/* ---- The command-loss timer (DS-46) -------------------------------------- */

ZTEST(mode_manager, test_no_timer_before_first_contact)
{
	/*
	 * No contact yet. The MET is new, so a timer that ignored "contacted"
	 * would treat it as a fresh silence and fire.
	 */
	const struct ground_contact not_yet = {.contacted = false, .last_accepted_met_ms = epoch};
	struct radio_cmd cmd;
	uint32_t fired = mm_hk().command_loss;

	zassert_ok(zbus_chan_pub(&ground_contact_chan, &not_yet, K_NO_WAIT));
	/* A year passes without contact. */
	wake_at(epoch + 365 * 24 * HOUR_MS);
	zassert_false(radio_next(&cmd, true), "keeps transmitting so the ground can find it");
	zassert_equal(mm_hk().command_loss, fired);
}

ZTEST(mode_manager, test_command_loss_silences_and_enters_safe)
{
	struct radio_cmd cmd;
	struct mode_state state;
	uint32_t fired = mm_hk().command_loss;

	zassert_equal(set_mode(MODE_NOMINAL), 1);
	contact_at(epoch);

	wake_at(epoch + TIMEOUT_MS - 1);
	zassert_false(radio_next(&cmd, true), "a millisecond early: nothing");
	zassert_equal(mode_now().mode, MODE_NOMINAL);

	wake_at(epoch + TIMEOUT_MS);
	zassert_true(radio_next(&cmd, true));
	zassert_equal(cmd.id, RADIO_CMD_SET_TRANSMIT);
	zassert_false(cmd.args.set_transmit.enabled);
	state = mode_now();
	zassert_equal(state.mode, MODE_SAFE);
	zassert_equal(state.reason, MODE_REASON_COMMAND_LOSS);
	zassert_equal(state.since_met_ms, epoch + TIMEOUT_MS);
	zassert_equal(mm_hk().command_loss - fired, 1);
}

ZTEST(mode_manager, test_command_loss_in_safe_mode_still_silences)
{
	struct radio_cmd cmd;

	/* Already in safe mode: no transition, but the action still runs. */
	contact_at(epoch);
	wake_at(epoch + TIMEOUT_MS);
	zassert_true(radio_next(&cmd, true));
	zassert_false(cmd.args.set_transmit.enabled);
	zassert_equal(mode_now().mode, MODE_SAFE);
	zassert_not_equal(mode_now().reason, MODE_REASON_COMMAND_LOSS,
			  "the reason is why safe mode was entered, which hasn't changed");
}

ZTEST(mode_manager, test_command_loss_fires_once_per_silence)
{
	struct radio_cmd cmd;
	uint32_t fired = mm_hk().command_loss;

	contact_at(epoch);
	wake_at(epoch + TIMEOUT_MS);
	zassert_true(radio_next(&cmd, true));

	/* The silence goes on: no more commands. */
	wake_at(epoch + 2 * TIMEOUT_MS);
	zassert_false(radio_next(&cmd, true));

	/* The ground is heard again, then goes quiet again: it fires again. */
	contact_at(epoch + 3 * TIMEOUT_MS);
	wake_at(epoch + 3 * TIMEOUT_MS + 1);
	zassert_false(radio_next(&cmd, true), "contact re-arms the timer; it doesn't fire");
	wake_at(epoch + 4 * TIMEOUT_MS);
	zassert_true(radio_next(&cmd, true));
	zassert_equal(mm_hk().command_loss - fired, 2);
}

ZTEST(mode_manager, test_ground_can_leave_safe_after_command_loss)
{
	zassert_equal(set_mode(MODE_NOMINAL), 1);
	contact_at(epoch);
	wake_at(epoch + TIMEOUT_MS);
	zassert_equal(mode_now().mode, MODE_SAFE);

	/* Command loss can't take the spacecraft out of safe mode; the ground can. */
	contact_at(epoch + TIMEOUT_MS + 1000);
	zassert_equal(set_mode(MODE_NOMINAL), 1);
	zassert_equal(mode_now().mode, MODE_NOMINAL);
}

ZTEST(mode_manager, test_busy_radio_is_retried_next_major_frame)
{
	struct radio_cmd cmd;
	uint32_t failures = mm_hk().action_failures;

	/*
	 * The radio stops handling commands. Two silences fill the mode
	 * manager's CMD_MAX_PENDING to it; the third can't be sent.
	 */
	BUILD_ASSERT(CMD_MAX_PENDING == 2, "this test fills the pending count with two silences");
	for (int i = 0; i < CMD_MAX_PENDING; i++) {
		contact_at(epoch + i * 2 * TIMEOUT_MS);
		wake_at(epoch + i * 2 * TIMEOUT_MS + TIMEOUT_MS);
		zassert_true(radio_next(&cmd, false));
	}
	contact_at(epoch + 4 * TIMEOUT_MS);
	wake_at(epoch + 5 * TIMEOUT_MS);
	zassert_false(radio_next(&cmd, false), "the radio already has too many waiting");
	zassert_equal(mm_hk().action_failures - failures, 1);

	/* Still busy a major frame later: tried and counted again. */
	wake_at(epoch + 5 * TIMEOUT_MS + 1000);
	zassert_equal(mm_hk().action_failures - failures, 2);

	/* The radio catches up; the next major frame sends it. */
	radio_catch_up(CMD_MAX_PENDING);
	wake_at(epoch + 5 * TIMEOUT_MS + 2000);
	zassert_true(radio_next(&cmd, true));
	zassert_false(cmd.args.set_transmit.enabled);

	/* Sent: no more tries. */
	wake_at(epoch + 5 * TIMEOUT_MS + 3000);
	zassert_false(radio_next(&cmd, true));
	zassert_equal(mm_hk().action_failures - failures, 2);
}

/* ---- What the mode manager stores (DS-41, DS-42, DS-74) ----------------- */

ZTEST(mode_manager, test_the_end_of_deployment_is_stored)
{
	struct nvm_deployment deployment;

	/* setup() booted through deploy into safe mode. */
	zassert_ok(nvm_deployment_read(&deployment));
	zassert_true(deployment.complete, "later boots start in safe mode, not deploy");
}

ZTEST(mode_manager, test_every_mode_change_is_stored)
{
	struct nvm_mode_state stored;

	zassert_equal(set_mode(MODE_NOMINAL), 1);
	zassert_ok(nvm_mode_state_read(&stored));
	zassert_equal(stored.mode, MODE_NOMINAL);
	zassert_equal(stored.reason, MODE_REASON_GROUND_COMMAND);

	zassert_equal(set_mode(MODE_SAFE), 1);
	zassert_ok(nvm_mode_state_read(&stored));
	zassert_equal(stored.mode, MODE_SAFE);
}
