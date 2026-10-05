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
 * The spacecraft boots in safe mode and waits for the ground (DS-42).
 *
 * Triggers: the mode manager checks its own once a major frame. So far that
 * is the command-loss timer (DS-46). Triggers that other apps detect (low
 * battery, a failed app) will arrive as requests, starting with health.
 *
 * Until FRAM, the mode is held in RAM, so every reset returns to safe mode
 * (DS-41 asks for it to persist).
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/zbus/zbus.h>

#include "msg/common.h"
#include "msg/mode_manager.h"
#include "silversat/resource_map.h"

ZBUS_MSG_SUBSCRIBER_DEFINE(mode_manager_sub);
ZBUS_CHAN_ADD_OBS(mode_manager_wakeup_chan, mode_manager_sub, 3);
ZBUS_CHAN_ADD_OBS(mode_manager_cmd_chan, mode_manager_sub, 3);

#define COMMAND_LOSS_TIMEOUT_MS ((int64_t)CONFIG_SS_COMMAND_LOSS_TIMEOUT_HOURS * 60 * 60 * 1000)

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
 * A mode change not listed here is refused. There is no row out of safe
 * mode except by ground command: the spacecraft boots in safe mode and
 * waits for the ground (DS-42).
 */
static const struct transition transitions[] = {
	/* Anything may put the spacecraft in safe mode (DS-41). */
	{MODE_NOMINAL, MODE_SAFE, ANY_REASON},
	/* Only the ground takes it out (DS-41). */
	{MODE_SAFE, MODE_NOMINAL, BIT(MODE_REASON_GROUND_COMMAND)},
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

/* The current mode. All zeros is safe mode, entered at boot. */
static struct mode_state state;

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
	(void)run_actions(ON_ENTER, to);
	return 0;
}

/* ---- Triggers --------------------------------------------------------------- */

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

	/* Safe mode, because of the boot (DS-42). */
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
