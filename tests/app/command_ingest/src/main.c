/*
 * Command ingest tests (DS-50 to DS-54).
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Command ingest runs as it does in flight. The tests stand in for:
 *   - the radio app: they put frames in uplink_msgq and read replies from
 *     downlink_msgq;
 *   - the frame manager: they publish ticks to command ingest's wakeup
 *     channel;
 *   - the mode manager: they publish mode_chan;
 *   - target_app: they read its command channel and publish its status.
 * The packets are signed at build time by tools/sign_command.py.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/ztest.h>

#include "app_test.h"
#include "msg/command_ingest.h"
#include "msg/common.h"
#include "msg/target_app.h"
#include "packets.h"
#include "silversat/cmd_counter.h"
#include "silversat/link.h"
#include "silversat/resource_map.h"

#define WAIT K_SECONDS(1)

/* ---- Standing in for target_app ----------------------------------------- */

ZBUS_MSG_SUBSCRIBER_DEFINE(target_sub);
ZBUS_CHAN_ADD_OBS(target_app_cmd_chan, target_sub, 3);

static struct app_status target_status;

/*
 * Take the next command routed to target_app, if any, and count it as
 * handled, as the app would: command ingest uses the count to decide
 * whether the app is busy.
 */
static bool target_next(struct target_app_cmd *cmd)
{
	const struct zbus_channel *chan;
	union target_app_msg msg;

	if (zbus_sub_wait_msg(&target_sub, &chan, &msg, K_NO_WAIT) != 0) {
		return false;
	}
	*cmd = msg.cmd;
	target_status.cmd_accepted++;
	zassert_ok(zbus_chan_pub(&target_app_status_chan, &target_status, K_NO_WAIT));
	return true;
}

/* Handle every command waiting for target_app. */
static void target_catch_up(void)
{
	struct target_app_cmd cmd;

	while (target_next(&cmd)) {
	}
}

/* ---- Standing in for the radio, the frame manager, the mode manager ----- */

static uint32_t frame_count;

static void uplink(const char *packet)
{
	struct link_frame frame = {.len = (uint8_t)strlen(packet)};

	memcpy(frame.data, packet, frame.len);
	zassert_ok(k_msgq_put(&uplink_msgq, &frame, K_NO_WAIT));
}

/* Run command ingest for one minor frame and wait until it has finished. */
static void run_one_frame(void)
{
	struct app_status before;

	zassert_ok(zbus_chan_read(&command_ingest_status_chan, &before, K_MSEC(10)));
	zassert_ok(app_test_wake(&command_ingest_wakeup_chan, frame_count++));
	zassert_ok(app_test_wait_status(&command_ingest_status_chan, before.steps + 1, 0, WAIT,
					NULL));
}

/*
 * Counts command ingest's housekeeping publishes. A listener runs in the
 * publishing thread, so the count is current by the time a step finishes.
 */
static atomic_t hk_publishes;

static void count_hk(const struct zbus_channel *chan)
{
	ARG_UNUSED(chan);
	atomic_inc(&hk_publishes);
}

ZBUS_LISTENER_DEFINE(hk_listener, count_hk);
ZBUS_CHAN_ADD_OBS(command_ingest_hk_chan, hk_listener, 3);

static void set_mode(uint8_t mode)
{
	const struct mode_state state = {.mode = mode, .reason = MODE_REASON_GROUND_COMMAND};

	zassert_ok(zbus_chan_pub(&mode_chan, &state, K_NO_WAIT));
}

/* The next reply on the downlink, as a string; "" if there is none. */
static const char *next_reply(void)
{
	static char text[LINK_FRAME_MAX + 1];
	struct link_frame frame;

	if (k_msgq_get(&downlink_msgq, &frame, K_NO_WAIT) != 0) {
		return "";
	}
	memcpy(text, frame.data, frame.len);
	text[frame.len] = '\0';
	return text;
}

/* The reply command ingest should send, for comparison. */
static const char *expected(const char *verb, uint64_t counter, const char *result)
{
	static char text[64];

	snprintk(text, sizeof(text), "%s %08x%08x %s", verb, (uint32_t)(counter >> 32),
		 (uint32_t)counter, result);
	return text;
}

static struct command_ingest_hk ci_hk(void)
{
	struct command_ingest_hk hk;

	zassert_ok(zbus_chan_read(&command_ingest_hk_chan, &hk, K_MSEC(10)));
	return hk;
}

static void reset_floors(void)
{
	for (uint8_t slot = 0; slot < CMD_COUNTER_SLOTS; slot++) {
		zassert_ok(floor_store_set(slot, CMD_COUNTER_EPOCH_MS));
	}
}

static void drain_queues(void)
{
	struct link_frame frame;

	while (k_msgq_get(&uplink_msgq, &frame, K_NO_WAIT) == 0) {
	}
	while (k_msgq_get(&downlink_msgq, &frame, K_NO_WAIT) == 0) {
	}
}

/*
 * Each test starts with both floors at the epoch, key slot 0 active, no
 * rotation armed, empty queues, target_app caught up, and safe mode.
 */
static void reset(void *fixture)
{
	ARG_UNUSED(fixture);
	drain_queues();
	target_catch_up();
	set_mode(MODE_SAFE);
	reset_floors();

	/* An earlier test may have left slot 1 active: switch back to 0. */
	if (ci_hk().active_slot != 0) {
		uplink(PKT_ARM_0);
		uplink(PKT_ROTATE_0);
		run_one_frame();
		zassert_equal(ci_hk().active_slot, 0, "could not return to slot 0");
		reset_floors();
		drain_queues();
	}
	/* Let any arm expire. Simulated time makes this instant. */
	if (ci_hk().rotation_armed) {
		k_sleep(K_MINUTES(11));
	}
	/* Let command ingest see the mode, and expire the arm. */
	run_one_frame();
	zassert_false(ci_hk().rotation_armed);
}

ZTEST_SUITE(command_ingest, NULL, NULL, reset, NULL, NULL);

/* ---- Tests ---------------------------------------------------------------- */

ZTEST(command_ingest, test_valid_command_is_routed_and_acked)
{
	struct command_ingest_hk before = ci_hk();
	struct command_ingest_hk after;
	struct target_app_cmd cmd;

	uplink(PKT_SET_LEVEL_7);
	run_one_frame();

	zassert_str_equal(next_reply(), expected("ACK", PKT_SET_LEVEL_7_COUNTER, "ok"));
	zassert_true(target_next(&cmd), "the command reached target_app");
	zassert_equal(cmd.id, TARGET_APP_CMD_SET_LEVEL);
	zassert_equal(cmd.args.set_level.level, 7);

	after = ci_hk();
	zassert_equal(after.received - before.received, 1);
	zassert_equal(after.routed - before.routed, 1);
	zassert_equal(after.last_counter, PKT_SET_LEVEL_7_COUNTER);
	/* app_test_tick() sets MET equal to uptime. */
	zassert_true(after.last_accepted_met_ms > 0 &&
			     after.last_accepted_met_ms <= k_uptime_get(),
		     "MET of acceptance comes from the tick (DS-46)");
}

ZTEST(command_ingest, test_replay_is_refused)
{
	struct target_app_cmd cmd;

	uplink(PKT_SET_LEVEL_7);
	run_one_frame();
	zassert_str_equal(next_reply(), expected("ACK", PKT_SET_LEVEL_7_COUNTER, "ok"));
	target_catch_up();

	/* The same packet again, as an eavesdropper might send it. */
	uplink(PKT_SET_LEVEL_7);
	run_one_frame();
	zassert_str_equal(next_reply(), expected("NAK", PKT_SET_LEVEL_7_COUNTER, "replay"));
	zassert_false(target_next(&cmd), "a replay must not reach the app");
}

ZTEST(command_ingest, test_out_of_order_is_refused)
{
	/* A gap is fine; going back below the floor is not (DS-53). */
	uplink(PKT_SET_LEVEL_9);
	uplink(PKT_SET_LEVEL_8);
	run_one_frame();
	zassert_str_equal(next_reply(), expected("ACK", PKT_SET_LEVEL_9_COUNTER, "ok"));
	zassert_str_equal(next_reply(), expected("NAK", PKT_SET_LEVEL_8_COUNTER, "replay"));
}

ZTEST(command_ingest, test_jump_too_large_is_refused)
{
	struct command_ingest_hk before = ci_hk();
	struct target_app_cmd cmd;

	uplink(PKT_JUMP);
	run_one_frame();
	zassert_str_equal(next_reply(), expected("NAK", PKT_JUMP_COUNTER, "jump"));
	zassert_false(target_next(&cmd));
	zassert_equal(ci_hk().rejected_jump - before.rejected_jump, 1);

	/* The floor didn't move, so a normal counter still works. */
	uplink(PKT_SET_LEVEL_7);
	run_one_frame();
	zassert_str_equal(next_reply(), expected("ACK", PKT_SET_LEVEL_7_COUNTER, "ok"));
}

ZTEST(command_ingest, test_bad_signature_gets_no_reply)
{
	struct command_ingest_hk before = ci_hk();
	struct target_app_cmd cmd;
	char tampered[] = PKT_SET_LEVEL_7;

	/* Signed with a key in neither slot. */
	uplink(PKT_FORGED);
	/* The level changed from 7 to 8 after signing. */
	tampered[strlen(tampered) - 1] = '8';
	uplink(tampered);
	run_one_frame();

	zassert_str_equal(next_reply(), "", "nothing to tell a forger");
	zassert_false(target_next(&cmd));
	zassert_equal(ci_hk().rejected_signature - before.rejected_signature, 2);

	/* The floor didn't move: the real packet still works. */
	uplink(PKT_SET_LEVEL_7);
	run_one_frame();
	zassert_str_equal(next_reply(), expected("ACK", PKT_SET_LEVEL_7_COUNTER, "ok"));
}

ZTEST(command_ingest, test_bad_shape_gets_no_reply)
{
	struct command_ingest_hk before = ci_hk();

	uplink("hello");
	uplink("");
	run_one_frame();
	zassert_str_equal(next_reply(), "");
	zassert_equal(ci_hk().rejected_shape - before.rejected_shape, 2);
}

ZTEST(command_ingest, test_route_failure_still_uses_up_the_counter)
{
	struct command_ingest_hk before = ci_hk();

	uplink(PKT_UNKNOWN_COMMAND);
	run_one_frame();
	zassert_str_equal(next_reply(),
			  expected("ACK", PKT_UNKNOWN_COMMAND_COUNTER, "unknown_command"));
	zassert_equal(ci_hk().rejected_route - before.rejected_route, 1);

	uplink(PKT_UNKNOWN_COMMAND);
	run_one_frame();
	zassert_str_equal(next_reply(), expected("NAK", PKT_UNKNOWN_COMMAND_COUNTER, "replay"));
}

ZTEST(command_ingest, test_bad_argument_is_named)
{
	char want[64];

	uplink(PKT_BAD_ARG);
	run_one_frame();
	snprintk(want, sizeof(want), "%s 0", expected("ACK", PKT_BAD_ARG_COUNTER, "bad_arg"));
	zassert_str_equal(next_reply(), want);
}

ZTEST(command_ingest, test_mode_gating)
{
	struct target_app_cmd cmd;

	/* nominal_thing is allowed only in nominal mode; we are in safe. */
	uplink(PKT_NOMINAL_THING);
	run_one_frame();
	zassert_str_equal(next_reply(), expected("ACK", PKT_NOMINAL_THING_COUNTER, "mode"));
	zassert_false(target_next(&cmd));

	/* The rejected command used up its counter; resend with a new one. */
	set_mode(MODE_NOMINAL);
	uplink(PKT_NOMINAL_THING_AGAIN);
	run_one_frame();
	zassert_str_equal(next_reply(), expected("ACK", PKT_NOMINAL_THING_AGAIN_COUNTER, "ok"));
	zassert_true(target_next(&cmd));
	zassert_equal(cmd.id, TARGET_APP_CMD_NOMINAL_THING);
}

ZTEST(command_ingest, test_several_frames_in_one_minor_frame)
{
	uplink(PKT_SET_LEVEL_7);
	uplink(PKT_SET_LEVEL_8);
	run_one_frame();
	zassert_str_equal(next_reply(), expected("ACK", PKT_SET_LEVEL_7_COUNTER, "ok"));
	zassert_str_equal(next_reply(), expected("ACK", PKT_SET_LEVEL_8_COUNTER, "ok"));
}

ZTEST(command_ingest, test_busy_target)
{
	/*
	 * target_app stops handling commands. Once CMD_MAX_PENDING are waiting
	 * for it, the next is refused rather than queued (DS-07); the counter
	 * is used up, and the ground resends with a new one.
	 */
	struct target_app_cmd cmd;

	BUILD_ASSERT(CMD_MAX_PENDING == 2, "the packets below assume 2");
	uplink(PKT_SET_LEVEL_7);
	uplink(PKT_SET_LEVEL_8);
	uplink(PKT_SET_LEVEL_9);
	run_one_frame();
	zassert_str_equal(next_reply(), expected("ACK", PKT_SET_LEVEL_7_COUNTER, "ok"));
	zassert_str_equal(next_reply(), expected("ACK", PKT_SET_LEVEL_8_COUNTER, "ok"));
	zassert_str_equal(next_reply(), expected("ACK", PKT_SET_LEVEL_9_COUNTER, "busy"));

	/* Once the app catches up, commands go through again. */
	target_catch_up();
	uplink(PKT_SET_LEVEL_10);
	run_one_frame();
	zassert_str_equal(next_reply(), expected("ACK", PKT_SET_LEVEL_10_COUNTER, "ok"));
	zassert_true(target_next(&cmd));
	zassert_equal(cmd.args.set_level.level, 10);
}

ZTEST(command_ingest, test_full_downlink_drops_the_reply_not_the_command)
{
	struct command_ingest_hk before = ci_hk();
	struct link_frame filler = {.len = 1, .data = "x"};
	struct target_app_cmd cmd;

	for (int i = 0; i < DOWNLINK_QUEUE_DEPTH; i++) {
		zassert_ok(k_msgq_put(&downlink_msgq, &filler, K_NO_WAIT));
	}
	uplink(PKT_SET_LEVEL_7);
	run_one_frame();
	zassert_equal(ci_hk().replies_dropped - before.replies_dropped, 1);
	zassert_true(target_next(&cmd), "the command still ran");
}

ZTEST(command_ingest, test_own_command_channel_rejects)
{
	/* Command ingest has no commands yet; anything sent to it is rejected. */
	const struct command_ingest_cmd cmd = {.id = 1};
	struct app_status before;
	struct app_status after;

	zassert_ok(zbus_chan_read(&command_ingest_status_chan, &before, K_MSEC(10)));
	zassert_ok(zbus_chan_pub(&command_ingest_cmd_chan, &cmd, K_NO_WAIT));
	zassert_ok(app_test_wait_status(&command_ingest_status_chan, 0,
					before.cmd_accepted + before.cmd_rejected + 1, WAIT,
					&after));
	zassert_equal(after.cmd_rejected - before.cmd_rejected, 1);
}

/* ---- Key rotation (DS-54) -------------------------------------------------- */

static uint64_t floor_of(uint8_t slot)
{
	uint64_t floor;

	zassert_ok(floor_store_get(slot, &floor));
	return floor;
}

ZTEST(command_ingest, test_spare_key_may_only_rotate)
{
	struct command_ingest_hk before = ci_hk();
	struct target_app_cmd cmd;

	/* A real command, signed with slot 1's key while slot 0 is active. */
	uplink(PKT_OTHER_KEY);
	run_one_frame();
	zassert_str_equal(next_reply(), expected("NAK", PKT_OTHER_KEY_COUNTER, "wrong_key"));
	zassert_false(target_next(&cmd));
	zassert_equal(ci_hk().rejected_other_key - before.rejected_other_key, 1);
	zassert_equal(floor_of(1), CMD_COUNTER_EPOCH_MS, "refused before its counter was used");
}

ZTEST(command_ingest, test_rotation)
{
	struct command_ingest_hk before = ci_hk();
	struct target_app_cmd cmd;

	uplink(PKT_ARM_1);
	run_one_frame();
	zassert_str_equal(next_reply(), expected("ACK", PKT_ARM_1_COUNTER, "ok"));
	zassert_true(ci_hk().rotation_armed);
	zassert_equal(ci_hk().active_slot, 0, "arming alone changes nothing");

	uplink(PKT_ROTATE_1);
	run_one_frame();
	zassert_str_equal(next_reply(), expected("ACK", PKT_ROTATE_1_COUNTER, "ok"));
	zassert_equal(ci_hk().active_slot, 1);
	zassert_false(ci_hk().rotation_armed);
	zassert_equal(ci_hk().rotations - before.rotations, 1);

	/* Slot 1's key now commands; slot 0's is the spare. */
	uplink(PKT_LEVEL_BY_SLOT1);
	run_one_frame();
	zassert_str_equal(next_reply(), expected("ACK", PKT_LEVEL_BY_SLOT1_COUNTER, "ok"));
	zassert_true(target_next(&cmd));
	zassert_equal(cmd.args.set_level.level, 5);

	uplink(PKT_SET_LEVEL_7);
	run_one_frame();
	zassert_str_equal(next_reply(), expected("NAK", PKT_SET_LEVEL_7_COUNTER, "wrong_key"));
	zassert_false(target_next(&cmd));
}

ZTEST(command_ingest, test_rotation_must_be_signed_by_the_new_slot)
{
	/* Arming slot 1 with the active key (slot 0) is refused. */
	uplink(PKT_ARM_1_BY_SLOT0);
	/* So is slot 1's key arming a different slot. */
	uplink(PKT_ARM_0_BY_SLOT1);
	/* And the active key arming its own slot: there is nothing to switch to. */
	uplink(PKT_ARM_0);
	run_one_frame();
	zassert_str_equal(next_reply(), expected("ACK", PKT_ARM_1_BY_SLOT0_COUNTER, "wrong_key"));
	zassert_str_equal(next_reply(), expected("ACK", PKT_ARM_0_BY_SLOT1_COUNTER, "wrong_key"));
	zassert_str_equal(next_reply(), expected("ACK", PKT_ARM_0_COUNTER, "wrong_key"));
	zassert_false(ci_hk().rotation_armed);
	zassert_equal(ci_hk().active_slot, 0);
}

ZTEST(command_ingest, test_rotation_needs_an_arm)
{
	uplink(PKT_ROTATE_1);
	run_one_frame();
	zassert_str_equal(next_reply(), expected("ACK", PKT_ROTATE_1_COUNTER, "not_armed"));
	zassert_equal(ci_hk().active_slot, 0);
}

ZTEST(command_ingest, test_arm_expires)
{
	uplink(PKT_ARM_1);
	run_one_frame();
	zassert_str_equal(next_reply(), expected("ACK", PKT_ARM_1_COUNTER, "ok"));

	/* More than 10 minutes of mission time later (DS-54). */
	k_sleep(K_MINUTES(11));
	run_one_frame();
	zassert_false(ci_hk().rotation_armed, "the arm expired on its own");

	uplink(PKT_ROTATE_1_LATE);
	run_one_frame();
	zassert_str_equal(next_reply(), expected("ACK", PKT_ROTATE_1_LATE_COUNTER, "not_armed"));
	zassert_equal(ci_hk().active_slot, 0);
}

ZTEST(command_ingest, test_rotation_bad_slot)
{
	char want[64];

	uplink(PKT_ARM_2_BY_SLOT1);
	run_one_frame();
	snprintk(want, sizeof(want), "%s 0", expected("ACK", PKT_ARM_2_BY_SLOT1_COUNTER, "bad_arg"));
	zassert_str_equal(next_reply(), want);
	zassert_false(ci_hk().rotation_armed);
}

ZTEST(command_ingest, test_recovery_from_a_maxed_out_floor)
{
	/*
	 * Slot 0's floor has been set to the maximum, so nothing signed with
	 * slot 0's key is ever accepted again (DS-53). Rotating to slot 1,
	 * with commands signed by slot 1, recovers the spacecraft (DS-54).
	 */
	struct target_app_cmd cmd;

	zassert_ok(floor_store_set(0, UINT64_MAX));
	uplink(PKT_SET_LEVEL_7);
	run_one_frame();
	zassert_str_equal(next_reply(), expected("NAK", PKT_SET_LEVEL_7_COUNTER, "replay"));

	uplink(PKT_ARM_1);
	uplink(PKT_ROTATE_1);
	uplink(PKT_LEVEL_BY_SLOT1);
	run_one_frame();
	zassert_str_equal(next_reply(), expected("ACK", PKT_ARM_1_COUNTER, "ok"));
	zassert_str_equal(next_reply(), expected("ACK", PKT_ROTATE_1_COUNTER, "ok"));
	zassert_str_equal(next_reply(), expected("ACK", PKT_LEVEL_BY_SLOT1_COUNTER, "ok"));
	zassert_true(target_next(&cmd));
}

ZTEST(command_ingest, test_no_floor_is_reset)
{
	/*
	 * Rotation never resets a floor (DS-53). Slot 0 accepts a command at
	 * counter ...095, then the spacecraft rotates to slot 1. Slot 0's
	 * floor must still be ...095, so a slot-0 command below it is refused
	 * as a replay. Had leaving slot 0 reset its floor, every command ever
	 * recorded under slot 0 would be accepted again.
	 */
	uplink(PKT_LEVEL_ABOVE_RETURN);
	uplink(PKT_ARM_1);
	uplink(PKT_ROTATE_1);
	run_one_frame();
	zassert_str_equal(next_reply(), expected("ACK", PKT_LEVEL_ABOVE_RETURN_COUNTER, "ok"));
	zassert_str_equal(next_reply(), expected("ACK", PKT_ARM_1_COUNTER, "ok"));
	zassert_str_equal(next_reply(), expected("ACK", PKT_ROTATE_1_COUNTER, "ok"));
	zassert_equal(ci_hk().active_slot, 1);

	/* Slot 0's key may still arm a rotation back, but ...090 is below its floor. */
	uplink(PKT_ARM_0);
	run_one_frame();
	zassert_str_equal(next_reply(), expected("NAK", PKT_ARM_0_COUNTER, "replay"));
	zassert_equal(floor_of(0), PKT_LEVEL_ABOVE_RETURN_COUNTER);
}

ZTEST(command_ingest, test_housekeeping_once_a_major_frame)
{
	/*
	 * With nothing to do, command ingest still publishes its housekeeping
	 * once a major frame, at slot 0, so the value telemetry output reads is
	 * never more than a second old (DS-14).
	 */
	atomic_set(&hk_publishes, 0);
	for (int i = 0; i < 2 * FRAME_SLOTS; i++) {
		run_one_frame();
	}
	zassert_equal(atomic_get(&hk_publishes), 2);
}

static struct ground_contact contact_now(void)
{
	struct ground_contact contact;

	zassert_ok(zbus_chan_read(&ground_contact_chan, &contact, K_MSEC(10)));
	return contact;
}

ZTEST(command_ingest, test_accepted_command_is_ground_contact)
{
	const struct ground_contact never = {0};

	/* Standing in for the boot state: the ground not yet heard from. */
	zassert_ok(zbus_chan_pub(&ground_contact_chan, &never, K_NO_WAIT));
	uplink(PKT_SET_LEVEL_7);
	run_one_frame();
	zassert_true(contact_now().contacted, "the mode manager's timer starts (DS-46)");
	zassert_equal(contact_now().last_accepted_met_ms, ci_hk().last_accepted_met_ms);
}

ZTEST(command_ingest, test_refused_command_is_not_ground_contact)
{
	const struct ground_contact never = {0};

	uplink(PKT_SET_LEVEL_7);
	run_one_frame();
	zassert_ok(zbus_chan_pub(&ground_contact_chan, &never, K_NO_WAIT));

	/* A replay and a forgery: anyone can send those, so they prove nothing. */
	uplink(PKT_SET_LEVEL_7);
	uplink(PKT_FORGED);
	run_one_frame();
	zassert_false(contact_now().contacted);
}
