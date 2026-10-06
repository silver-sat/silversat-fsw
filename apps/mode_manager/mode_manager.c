/*
 * Mode manager (DS-40, DS-41, DS-46).
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The only app that publishes mode_chan. The frame manager reads it to
 * choose its frame table, and command ingest to decide which commands are
 * allowed.
 *
 * Two tables say everything the mode manager does, so a student can read
 * the spacecraft's mode logic in one place:
 *
 *   transitions[]  which mode changes are allowed, and for which reasons.
 *                  Anything may enter safe mode; only the ground leaves it
 *                  (DS-41).
 *   actions[]      what to do on entering or leaving a mode, or when a
 *                  trigger fires: internal commands to other apps (DS-40).
 *
 * Boot (DS-42): the spacecraft starts in deploy mode, in which nothing that
 * transmits runs and no ground command is accepted. After the separation
 * delay it enters safe mode and waits for the ground. If the test signal is
 * present at boot, it enters test mode instead, for ground testing: no
 * delay, no antenna deployment, and ground commands accepted. Nothing
 * leads back into deploy or test, so each is used at most once per boot.
 *
 * Triggers: the mode manager checks its own once a major frame: the
 * separation delay and the command-loss timer (DS-46). Triggers that other
 * apps detect (low battery, a failed app) will arrive as requests, starting
 * with health.
 *
 * Across a reset (DS-41, DS-42, DS-74): two FRAM records, also kept in the
 * mirror (DS-75). deployment says whether the separation delay has ever
 * ended; once it has, every boot starts in safe mode instead of deploy, so
 * deploy mode is used once in the mission. mode_state holds the mode and
 * reason, written at every change, so a spacecraft that was in safe mode
 * keeps its reason (for example, command loss) through a reset. With
 * neither FRAM nor the mirror, both read as their defaults: deploy mode
 * again, which waits out the delay, late but never early.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/zbus/zbus.h>

#include "msg/common.h"
#include "msg/mode_manager.h"
#include "nvm/mode_manager.h"
#include "silversat/resource_map.h"

ZBUS_MSG_SUBSCRIBER_DEFINE(mode_manager_sub);
ZBUS_CHAN_ADD_OBS(mode_manager_wakeup_chan, mode_manager_sub, 3);
ZBUS_CHAN_ADD_OBS(mode_manager_cmd_chan, mode_manager_sub, 3);

#define COMMAND_LOSS_TIMEOUT_MS ((int64_t)CONFIG_SS_COMMAND_LOSS_TIMEOUT_HOURS * 60 * 60 * 1000)
#define SEPARATION_DELAY_MS ((int64_t)CONFIG_SS_SEPARATION_DELAY_MINUTES * 60 * 1000)

/*
 * The test signal (DS-42), from the test-mode-signal devicetree alias in the
 * resource map. A board without the alias never enters test mode: .port is
 * NULL.
 */
static const struct gpio_dt_spec test_signal = GPIO_DT_SPEC_GET_OR(TEST_SIGNAL_NODE, gpios, {0});

/* ---- Transitions (DS-40, DS-41) ----------------------------------------- */

/* For a transition any reason may cause. */
#define ANY_REASON UINT32_MAX

struct transition {
	uint8_t from; /* enum mode */
	uint8_t to;   /* enum mode */
	/* The reasons allowed to cause it: BIT(enum mode_reason) for each. */
	uint32_t reasons;
};

BUILD_ASSERT(MODE_REASON_MAX < 32, "the reasons mask has 32 bits");

/*
 * A mode change not listed here is refused. No row leads into deploy: the
 * spacecraft starts there at boot, until deployment has ended once. Only
 * the test signal, read once at boot, leads into test (DS-42).
 */
static const struct transition transitions[] = {
	/* After separation, wait out the delay, then wait for the ground (DS-42). */
	{MODE_DEPLOY, MODE_SAFE, BIT(MODE_REASON_DEPLOYMENT_COMPLETE)},
	/*
	 * On the ground, the test signal at boot skips the delay. After the
	 * first deployment, later boots start in safe mode, and the signal
	 * still selects test mode there, so a bench unit keeps it.
	 */
	{MODE_DEPLOY, MODE_TEST, BIT(MODE_REASON_TEST_SIGNAL)},
	{MODE_SAFE, MODE_TEST, BIT(MODE_REASON_TEST_SIGNAL)},
	/* Anything may put the spacecraft in safe mode (DS-41). */
	{MODE_NOMINAL, MODE_SAFE, ANY_REASON},
	{MODE_TEST, MODE_SAFE, ANY_REASON},
	/* Only the ground takes it out (DS-41). */
	{MODE_SAFE, MODE_NOMINAL, BIT(MODE_REASON_GROUND_COMMAND)},
	/* The ground may test nominal operation from test mode. */
	{MODE_TEST, MODE_NOMINAL, BIT(MODE_REASON_GROUND_COMMAND)},
};

/* ---- Actions (DS-40) ----------------------------------------------------- */

enum action_event {
	ON_ENTER,   /* entering the mode in .which */
	ON_EXIT,    /* leaving the mode in .which */
	ON_TRIGGER, /* the trigger whose enum mode_reason is in .which fired */
};

struct action {
	enum action_event event;
	uint8_t which;
	/*
	 * Sends an internal command; returns 0 or a negative errno. A
	 * trigger's actions are retried until they all succeed, so each must
	 * be safe to repeat: commands set state, they don't toggle it (DS-35).
	 */
	int (*run)(void);
};

static int stop_transmitting(void)
{
	return send_radio_set_transmit(false);
}

/*
 * A trigger's actions run whether or not the mode changes: if the
 * spacecraft is already in safe mode when the command-loss timer fires,
 * it still stops transmitting.
 */
static const struct action actions[] = {
	/*
	 * No accepted ground command for the timeout: stop all transmission.
	 * Only a ground command turns it back on (DS-46).
	 */
	{ON_TRIGGER, MODE_REASON_COMMAND_LOSS, stop_transmitting},
};

/* ---- State ---------------------------------------------------------------- */

static struct app_status status;
static struct mode_manager_hk hk;

/* The current mode. Read from mode_chan's initial value at start: deploy. */
static struct mode_state state;

/* The test signal is read once, at the first wakeup after boot. */
static bool boot_checked;

/* MET from the latest tick. Commands carry no time of their own (DS-25). */
static int64_t now_met_ms;

/*
 * The command-loss timer (DS-46). It runs only after first contact, and
 * fires once for each silence: a newer accepted command re-arms it.
 */
static bool loss_fired;
static bool loss_actions_done;
static int64_t loss_contact_met_ms; /* the last contact before the silence */

/* ---- Mode changes ---------------------------------------------------------- */

/* Run every action for one event. Returns how many failed. */
static int run_actions(enum action_event event, uint8_t which)
{
	int failed = 0;

	for (size_t i = 0; i < ARRAY_SIZE(actions); i++) {
		if (actions[i].event == event && actions[i].which == which &&
		    actions[i].run() != 0) {
			failed++;
		}
	}
	hk.action_failures += (uint32_t)failed;
	return failed;
}

static bool allowed(uint8_t from, uint8_t to, uint8_t reason)
{
	for (size_t i = 0; i < ARRAY_SIZE(transitions); i++) {
		const struct transition *t = &transitions[i];

		if (t->from == from && t->to == to && (t->reasons & BIT(reason)) != 0) {
			return true;
		}
	}
	return false;
}

/* Store the mode and reason, for the next boot. */
static void store_mode(void)
{
	const struct nvm_mode_state record = {.mode = state.mode, .reason = state.reason};

	if (nvm_mode_state_write(&record) != 0) {
		hk.store_failures++;
	}
}

/*
 * Change to mode `to` for `reason`, if the transitions table allows it.
 * Setting the current mode changes nothing and succeeds (DS-35). Returns 0,
 * or -EPERM if refused.
 */
static int change_mode(uint8_t to, uint8_t reason)
{
	if (to == state.mode) {
		return 0;
	}
	/*
	 * allowed() refuses any mode not in the table. Check the reason first:
	 * BIT() of a value of 32 or more is undefined.
	 */
	if (reason > MODE_REASON_MAX || !allowed(state.mode, to, reason)) {
		hk.refused++;
		return -EPERM;
	}
	(void)run_actions(ON_EXIT, state.mode);
	state.mode = to;
	state.reason = reason;
	state.since_met_ms = now_met_ms;
	hk.transitions++;
	zbus_chan_pub(&mode_chan, &state, K_NO_WAIT);
	store_mode();
	(void)run_actions(ON_ENTER, to);
	return 0;
}

/* ---- Triggers --------------------------------------------------------------- */

static bool test_signal_present(void)
{
	if (test_signal.port == NULL || !gpio_is_ready_dt(&test_signal) ||
	    gpio_pin_configure_dt(&test_signal, GPIO_INPUT) != 0) {
		return false; /* no signal: fly, don't test */
	}
	return gpio_pin_get_dt(&test_signal) == 1;
}

/*
 * Once, at the first wakeup: the test signal. Read only then, so a glitch
 * on the pin in flight can never put the spacecraft in test mode.
 */
static void check_boot(void)
{
	if (boot_checked) {
		return;
	}
	boot_checked = true;
	if (test_signal_present()) {
		(void)change_mode(MODE_TEST, MODE_REASON_TEST_SIGNAL);
	}
}

/* The separation delay (DS-42, DS-45), timed from MET. */
static void check_separation_delay(void)
{
	const struct nvm_deployment done = {.complete = true};

	if (state.mode != MODE_DEPLOY || now_met_ms < SEPARATION_DELAY_MS) {
		return;
	}
	/*
	 * Stored before the mode changes: if the power fails between the two,
	 * the next boot still starts in safe mode, never in deploy again.
	 */
	if (nvm_deployment_write(&done) != 0) {
		hk.store_failures++;
	}
	(void)change_mode(MODE_SAFE, MODE_REASON_DEPLOYMENT_COMPLETE);
}

/*
 * The mode to start in (DS-42): deploy, mode_chan's initial value, until
 * the separation delay has ended once; after that, safe mode. A spacecraft
 * that was in safe mode keeps its reason through the reset (DS-41).
 */
static void boot_mode(void)
{
	struct nvm_deployment deployment;
	struct nvm_mode_state stored;

	(void)zbus_chan_read(&mode_chan, &state, K_FOREVER);
	(void)nvm_deployment_read(&deployment);
	if (!deployment.complete) {
		return;
	}
	(void)nvm_mode_state_read(&stored);
	state.mode = MODE_SAFE;
	state.reason = stored.mode == MODE_SAFE && stored.reason <= MODE_REASON_MAX
			       ? stored.reason
			       : MODE_REASON_BOOT;
	state.since_met_ms = 0;
}

static void check_command_loss(void)
{
	struct ground_contact contact;

	if (zbus_chan_read(&ground_contact_chan, &contact, K_NO_WAIT) != 0) {
		return; /* busy this instant; check again next major frame */
	}
	if (!contact.contacted) {
		return; /* no first contact yet, so the timer isn't running */
	}
	if (loss_fired && contact.last_accepted_met_ms != loss_contact_met_ms) {
		loss_fired = false; /* the ground has been heard since: re-arm */
	}
	if (!loss_fired) {
		if (now_met_ms - contact.last_accepted_met_ms < COMMAND_LOSS_TIMEOUT_MS) {
			return;
		}
		loss_fired = true;
		loss_actions_done = false;
		loss_contact_met_ms = contact.last_accepted_met_ms;
		hk.command_loss++;
		(void)change_mode(MODE_SAFE, MODE_REASON_COMMAND_LOSS);
	}
	/* Until every action has been sent, try again each major frame. */
	if (!loss_actions_done) {
		loss_actions_done = run_actions(ON_TRIGGER, MODE_REASON_COMMAND_LOSS) == 0;
	}
}

/* ---- The app ---------------------------------------------------------------- */

static int handle_command(const struct mode_manager_cmd *cmd)
{
	switch (cmd->id) {
	case MODE_MANAGER_CMD_SET_MODE:
		/* Only command ingest sends this, so it is the ground's. */
		return change_mode(cmd->args.set_mode.mode, MODE_REASON_GROUND_COMMAND);
	default:
		return -ENOTSUP;
	}
}

static void publish_hk(void)
{
	hk.mode = state.mode;
	hk.reason = state.reason;
	zbus_chan_pub(&mode_manager_hk_chan, &hk, K_NO_WAIT);
}

static void step(const struct frame_tick *tick)
{
	now_met_ms = tick->met_ms;
	check_boot();
	check_separation_delay();
	check_command_loss();
	/*
	 * Once a major frame, publish the mode again even if it hasn't
	 * changed, so a publish that failed is repaired.
	 */
	zbus_chan_pub(&mode_chan, &state, K_NO_WAIT);
	publish_hk();
}

static void mode_manager_main(void *a, void *b, void *c)
{
	const struct zbus_channel *chan;
	union mode_manager_msg msg;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	boot_mode();
	zbus_chan_pub(&mode_chan, &state, K_NO_WAIT);
	publish_hk();

	while (zbus_sub_wait_msg(&mode_manager_sub, &chan, &msg, K_FOREVER) == 0) {
		if (chan == &mode_manager_wakeup_chan) {
			step(&msg.tick);
			status.steps++;
		} else if (chan == &mode_manager_cmd_chan) {
			if (handle_command(&msg.cmd) == 0) {
				status.cmd_accepted++;
			} else {
				status.cmd_rejected++;
			}
			publish_hk(); /* the state may have changed (DS-14) */
		}
		zbus_chan_pub(&mode_manager_status_chan, &status, K_NO_WAIT);
	}
}

K_THREAD_DEFINE(mode_manager_tid, MODE_MANAGER_STACK_SIZE, mode_manager_main, NULL, NULL, NULL,
		MODE_MANAGER_PRIORITY, 0, 0);
