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
#include "fake_fram.h"
#include "fake_retained_mem.h"
#include "msg/command_ingest.h"
#include "msg/common.h"
#include "msg/target_app.h"
#include "nvm/command_ingest.h"
#include "packets.h"
#include "silversat/cmd_counter.h"
#include "silversat/cmd_route.h"
#include "silversat/event.h"
#include "silversat/link.h"
#include "silversat/nvm.h"
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
	/* FRAM and the mirror working (a test may have failed them). */
	fake_fram_fail(DEVICE_DT_GET(FRAM_NODE), false);
	fake_retained_mem_fail(DEVICE_DT_GET(NVM_MIRROR_NODE), false);
	zassert_ok(nvm_retry());
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

	/*
	 * Close every rejection limit an earlier test opened: one quiet period
	 * reports any count still owed, a second finds nothing more and closes
	 * it. Then each test sees only its own events.
	 */
	for (int i = 0; i < 2; i++) {
		k_sleep(K_SECONDS(CONFIG_SS_CMD_REJECT_EVENT_SECONDS + 1));
		run_one_frame();
	}
	app_test_drain_events();
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

/* ---- What command ingest stores (DS-53, DS-54, DS-74, DS-75) ------------ */

static struct nvm_command_state stored(void)
{
	struct nvm_command_state record;

	zassert_ok(nvm_command_state_read(&record));
	return record;
}

ZTEST(command_ingest, test_an_accepted_command_is_stored)
{
	uplink(PKT_SET_LEVEL_7);
	run_one_frame();
	zassert_str_equal(next_reply(), expected("ACK", PKT_SET_LEVEL_7_COUNTER, "ok"));
	zassert_equal(stored().floor_slot0, PKT_SET_LEVEL_7_COUNTER,
		      "stored before the ACK, so a reset can't reopen the replay window");
	zassert_true(stored().contacted);
	zassert_equal(stored().last_accepted_met_ms, ci_hk().last_accepted_met_ms);
}

ZTEST(command_ingest, test_the_floor_store_writes_through)
{
	/* Each floor is stored by the floor store itself, not later. */
	zassert_ok(floor_store_set(1, CMD_COUNTER_EPOCH_MS + 42));
	zassert_equal(stored().floor_slot1, CMD_COUNTER_EPOCH_MS + 42);
	zassert_ok(floor_store_set(0, CMD_COUNTER_EPOCH_MS + 43));
	zassert_equal(stored().floor_slot0, CMD_COUNTER_EPOCH_MS + 43);
}

ZTEST(command_ingest, test_a_rotation_is_stored)
{
	uplink(PKT_ARM_1);
	uplink(PKT_ROTATE_1);
	run_one_frame();
	zassert_equal(ci_hk().active_slot, 1);
	zassert_equal(stored().active_slot, 1, "a reset keeps the new key (DS-54)");
}

ZTEST(command_ingest, test_a_command_still_runs_if_it_cannot_be_stored)
{
	struct target_app_cmd cmd;
	uint32_t failures = ci_hk().store_failures;

	/* Neither FRAM nor the mirror works. */
	fake_fram_fail(DEVICE_DT_GET(FRAM_NODE), true);
	fake_retained_mem_fail(DEVICE_DT_GET(NVM_MIRROR_NODE), true);
	uplink(PKT_SET_LEVEL_7);
	run_one_frame();
	zassert_str_equal(next_reply(), expected("ACK", PKT_SET_LEVEL_7_COUNTER, "ok"),
			  "commanding goes on (DS-75)");
	zassert_true(target_next(&cmd));
	zassert_true(ci_hk().store_failures > failures);

	/* The floor is kept in RAM: a replay is still refused this boot. */
	uplink(PKT_SET_LEVEL_7);
	run_one_frame();
	zassert_str_equal(next_reply(), expected("NAK", PKT_SET_LEVEL_7_COUNTER, "replay"));
}

/* ---- Events (DS-10, DS-50) -------------------------------------------------- */

/* Command ingest should have raised this event. Returns it. */
static struct event expect_event(uint16_t id)
{
	struct event event;

	zassert_true(app_test_find_event(APP_ID_COMMAND_INGEST, id, &event), "event %u", id);
	return event;
}

/* Let a whole rejection period pass, and run one frame at its end. */
static void after_a_period(void)
{
	k_sleep(K_SECONDS(CONFIG_SS_CMD_REJECT_EVENT_SECONDS + 1));
	run_one_frame();
}

ZTEST(command_ingest, test_a_rejection_is_an_event_at_once)
{
	struct event event;

	uplink("hello");
	run_one_frame();
	event = expect_event(COMMAND_INGEST_EVENT_REJECTED_SHAPE);
	zassert_equal(event.arg0, 1, "count");
	zassert_equal(event.severity, SEVERITY_INFO);
	zassert_true(event.met_ms > 0 && event.met_ms <= k_uptime_get(), "the tick's MET (DS-25)");
}

ZTEST(command_ingest, test_more_of_a_kind_are_counted_into_one_event)
{
	struct event event;

	uplink("one");
	run_one_frame();
	expect_event(COMMAND_INGEST_EVENT_REJECTED_SHAPE);

	/* Three more within the period: no event yet. */
	for (int i = 0; i < 3; i++) {
		uplink("more");
		run_one_frame();
	}
	zassert_equal(event_take(&event), -ENOMSG, "counted, not raised");

	/* The period ends: one event carries how many. */
	after_a_period();
	event = expect_event(COMMAND_INGEST_EVENT_REJECTED_SHAPE);
	zassert_equal(event.arg0, 3);

	/* A quiet period: nothing to report, and the limit closes... */
	after_a_period();
	zassert_equal(event_take(&event), -ENOMSG);

	/* ...so the next is an event at once again. */
	uplink("again");
	run_one_frame();
	event = expect_event(COMMAND_INGEST_EVENT_REJECTED_SHAPE);
	zassert_equal(event.arg0, 1);
}

ZTEST(command_ingest, test_a_flood_raises_at_most_two_events_a_period)
{
	struct event event;
	int events = 0;
	uint32_t total = 0;

	/* As much as the uplink queue holds, every minor frame of a period. */
	for (int frame = 0; frame < 40; frame++) {
		for (int i = 0; i < UPLINK_QUEUE_DEPTH; i++) {
			uplink("noise");
		}
		run_one_frame();
	}
	after_a_period();
	while (event_take(&event) == 0) {
		zassert_equal(event.id, COMMAND_INGEST_EVENT_REJECTED_SHAPE);
		events++;
		total += event.arg0;
	}
	zassert_equal(events, 2, "the first, then one carrying the rest");
	zassert_equal(total, 40 * UPLINK_QUEUE_DEPTH, "every rejection counted");
}

ZTEST(command_ingest, test_each_kind_has_its_own_limit)
{
	const uint16_t kinds[] = {COMMAND_INGEST_EVENT_REJECTED_SHAPE,
				  COMMAND_INGEST_EVENT_REJECTED_SIGNATURE,
				  COMMAND_INGEST_EVENT_REJECTED_OTHER_KEY};
	struct event event;

	/* One of each in the same minor frame: each is the first of its kind. */
	uplink("hello");
	uplink(PKT_FORGED);
	uplink(PKT_OTHER_KEY);
	run_one_frame();
	for (size_t i = 0; i < ARRAY_SIZE(kinds); i++) {
		zassert_ok(event_take(&event));
		zassert_equal(event.id, kinds[i], "event %zu", i);
		zassert_equal(event.arg0, 1);
	}
}

ZTEST(command_ingest, test_signature_and_key_rejections_are_events)
{
	struct event event;

	uplink(PKT_FORGED);
	run_one_frame();
	event = expect_event(COMMAND_INGEST_EVENT_REJECTED_SIGNATURE);
	zassert_equal(event.severity, SEVERITY_WARNING);

	uplink(PKT_OTHER_KEY);
	run_one_frame();
	event = expect_event(COMMAND_INGEST_EVENT_REJECTED_OTHER_KEY);
	zassert_equal(event.arg0, 1);
}

ZTEST(command_ingest, test_counter_rejections_are_events_with_the_slot)
{
	struct event event;

	/* The jump first, while the floor is at the epoch and it is too far above. */
	uplink(PKT_JUMP);
	run_one_frame();
	event = expect_event(COMMAND_INGEST_EVENT_REJECTED_JUMP);
	zassert_equal(event.arg0, 1);
	zassert_equal(event.arg1, 0, "signed by slot 0");

	uplink(PKT_SET_LEVEL_7);
	uplink(PKT_SET_LEVEL_7);
	run_one_frame();
	event = expect_event(COMMAND_INGEST_EVENT_REJECTED_REPLAY);
	zassert_equal(event.arg0, 1);
	zassert_equal(event.arg1, 0);
}

ZTEST(command_ingest, test_a_routing_rejection_is_an_event_every_time)
{
	struct event event;

	/* In counter order, so neither is a replay. */
	uplink(PKT_UNKNOWN_COMMAND);
	run_one_frame();
	event = expect_event(COMMAND_INGEST_EVENT_REJECTED_ROUTE);
	zassert_equal(event.arg0, APP_ID_TARGET_APP);
	zassert_equal(event.arg1, CMD_ROUTE_UNKNOWN_COMMAND);

	/* Only the ground can cause these, so they aren't limited. */
	uplink(PKT_NOMINAL_THING);
	run_one_frame();
	event = expect_event(COMMAND_INGEST_EVENT_REJECTED_ROUTE);
	zassert_equal(event.arg1, CMD_ROUTE_MODE);
}

ZTEST(command_ingest, test_rotation_is_an_event)
{
	struct event event;

	uplink(PKT_ARM_1);
	run_one_frame();
	event = expect_event(COMMAND_INGEST_EVENT_ROTATION_ARMED);
	zassert_equal(event.arg0, 1);

	uplink(PKT_ROTATE_1);
	run_one_frame();
	event = expect_event(COMMAND_INGEST_EVENT_KEY_ROTATED);
	zassert_equal(event.arg0, 1, "slot 1 now in use");
	zassert_equal(event.severity, SEVERITY_WARNING);
}

ZTEST(command_ingest, test_a_lapsed_arm_is_an_event)
{
	struct event event;

	uplink(PKT_ARM_1);
	run_one_frame();
	app_test_drain_events();
	k_sleep(K_MINUTES(11));
	run_one_frame();
	event = expect_event(COMMAND_INGEST_EVENT_ROTATION_EXPIRED);
	zassert_equal(event.arg0, 1);
}

ZTEST(command_ingest, test_a_summary_comes_when_the_period_ends)
{
	struct event event;

	uplink("one");
	run_one_frame();
	expect_event(COMMAND_INGEST_EVENT_REJECTED_SHAPE);
	uplink("two");
	run_one_frame();

	/* Half a second before the period ends: nothing yet. */
	k_sleep(K_MSEC(CONFIG_SS_CMD_REJECT_EVENT_SECONDS * 1000 - 500));
	run_one_frame();
	zassert_equal(event_take(&event), -ENOMSG, "too early");

	/* Half a second after it: the summary. */
	k_sleep(K_SECONDS(1));
	run_one_frame();
	zassert_equal(expect_event(COMMAND_INGEST_EVENT_REJECTED_SHAPE).arg0, 1);
}

ZTEST(command_ingest, test_a_flood_that_goes_on_is_summarised_once_a_period)
{
	struct event event;

	uplink("one");
	run_one_frame();
	uplink("two");
	run_one_frame();
	after_a_period();
	app_test_drain_events(); /* the first event, and the summary of "two" */

	/* More, just after the summary: held until the next period ends. */
	uplink("three");
	run_one_frame();
	zassert_equal(event_take(&event), -ENOMSG, "a new period has started");
	after_a_period();
	zassert_equal(expect_event(COMMAND_INGEST_EVENT_REJECTED_SHAPE).arg0, 1);
}

ZTEST(command_ingest, test_a_summary_names_the_slot_of_the_last)
{
	struct event event;

	/* A replay signed by slot 0: the first, an event at once. */
	uplink(PKT_SET_LEVEL_7);
	uplink(PKT_SET_LEVEL_7);
	run_one_frame();
	zassert_equal(expect_event(COMMAND_INGEST_EVENT_REJECTED_REPLAY).arg1, 0);

	/*
	 * Then one signed by slot 1: a rotation command (the only kind slot 1's
	 * key may sign while slot 0 is active), sent twice.
	 */
	uplink(PKT_ARM_1);
	uplink(PKT_ARM_1);
	run_one_frame();
	after_a_period();
	zassert_true(app_test_find_event(APP_ID_COMMAND_INGEST,
					 COMMAND_INGEST_EVENT_REJECTED_REPLAY, &event));
	zassert_equal(event.arg0, 1);
	zassert_equal(event.arg1, 1, "the slot of the last replay");
}
