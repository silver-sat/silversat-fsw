/*
 * Command ingest (DS-50 to DS-54).
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The only app that accepts ground commands. The radio app puts each
 * uplink frame in uplink_msgq (DS-11). Every minor frame, the frame manager
 * wakes this app, and it takes every waiting frame through the pipeline
 * (DS-50):
 *
 *   1. shape check            cmd_auth_parse()      no reply on failure
 *   2. signature              cmd_auth_verify()     no reply on failure
 *   3. counter above floor    cmd_counter_accept()  "NAK <counter> replay|jump"
 *   4. new floor stored       (the same call)       "NAK <counter> store"
 *   5-7. decode and route     cmd_route()           "ACK <counter> <result>"
 *
 * Frames that fail stages 1 or 2 get no reply: there is nothing to tell a
 * forger, and radio noise needs no answer. Every authenticated command gets
 * exactly one reply. Once stage 4 has stored the floor, the counter is used
 * up, whatever the result of routing, so the reply comes after routing and
 * carries both (DS-50, decided 2026-10-02).
 *
 * Replies go into downlink_msgq for the radio app, as text:
 *
 *   ACK 0000018f2c4d5e6f ok
 *   ACK 0000018f2c4d5e6f bad_arg 2
 *   NAK 0000018f2c4d5e6f replay
 *
 * Key rotation (DS-54). There are two key slots, each with its own key and
 * its own counter floor. Commands are signed with the active slot's key.
 * The other slot's key is accepted for one purpose only: the two rotation
 * commands, which switch to that slot. They are handled here rather than
 * routed, because only command ingest knows which key signed a command.
 * No floor is ever reset (DS-53, decided 2026-10-02), so switching back to
 * a slot later cannot make its recorded commands valid again.
 */

#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>
#include <zephyr/zbus/zbus.h>

#include "cmd_keys.h"
#include "msg/command_ingest.h"
#include "msg/common.h"
#include "silversat/cmd_auth.h"
#include "silversat/cmd_counter.h"
#include "silversat/cmd_route.h"
#include "silversat/cmd_text.h"
#include "silversat/link.h"
#include "silversat/resource_map.h"

BUILD_ASSERT(ARRAY_SIZE(cmd_keys) == CMD_COUNTER_SLOTS, "one key per counter slot");

/* An armed rotation must be fired within this much mission time (DS-54). */
#define ROTATION_ARM_WINDOW_MS (10 * 60 * 1000)

ZBUS_MSG_SUBSCRIBER_DEFINE(command_ingest_sub);
ZBUS_CHAN_ADD_OBS(command_ingest_wakeup_chan, command_ingest_sub, 3);
ZBUS_CHAN_ADD_OBS(command_ingest_cmd_chan, command_ingest_sub, 3);

static struct app_status status;
static struct command_ingest_hk hk;

/*
 * Until mode_chan is read, assume deploy, the most restrictive: no command
 * is allowed in it (DS-42).
 */
static uint8_t mode = MODE_DEPLOY;

/*
 * The key slot in use, and any armed rotation. Held in RAM until the FRAM
 * service exists, so a reboot returns to slot 0 (DS-75) and clears any arm.
 */
static uint8_t active_slot;
static bool armed;
static uint8_t armed_slot;
static int64_t armed_at_met_ms;

/* The words for each routing result. tools/command_text.py RESULTS matches. */
static const char *const route_words[] = {
	[CMD_ROUTE_OK] = "ok",
	[CMD_ROUTE_BAD_SYNTAX] = "bad_syntax",
	[CMD_ROUTE_UNKNOWN_APP] = "unknown_app",
	[CMD_ROUTE_UNKNOWN_COMMAND] = "unknown_command",
	[CMD_ROUTE_BAD_ARG_COUNT] = "bad_arg_count",
	[CMD_ROUTE_MODE] = "mode",
	[CMD_ROUTE_BAD_ARG] = "bad_arg",
	[CMD_ROUTE_BUSY] = "busy",
	[CMD_ROUTE_PUBLISH_FAILED] = "publish_failed",
};
BUILD_ASSERT(ARRAY_SIZE(route_words) == CMD_ROUTE_PUBLISH_FAILED + 1,
	     "every routing result needs a word");

/*
 * Queue a reply: "<verb> <counter> <result>", plus the argument number for
 * bad_arg. The counter is written the way the ground sent it: 16 lowercase
 * hex digits.
 */
static void reply(const char *verb, uint64_t counter, const char *result, int bad_arg)
{
	struct link_frame frame = {0};
	int len;

	if (bad_arg >= 0) {
		len = snprintk(frame.data, sizeof(frame.data), "%s %08x%08x %s %d", verb,
			       (uint32_t)(counter >> 32), (uint32_t)counter, result, bad_arg);
	} else {
		len = snprintk(frame.data, sizeof(frame.data), "%s %08x%08x %s", verb,
			       (uint32_t)(counter >> 32), (uint32_t)counter, result);
	}
	frame.len = (uint8_t)len;
	if (k_msgq_put(&downlink_msgq, &frame, K_NO_WAIT) != 0) {
		hk.replies_dropped++;
	}
}

static uint8_t spare_slot(void)
{
	return active_slot == 0 ? 1 : 0;
}

/*
 * If the text is one of the rotation commands ("command_ingest
 * arm_key_rotation <slot>" or "command_ingest rotate_key <slot>"), return
 * its command id; otherwise 0. The words come from the YAML.
 */
static uint16_t rotation_command(const struct cmd_auth_packet *packet, struct cmd_text *words)
{
	if (cmd_text_split(packet->text, packet->text_len, words) != 0 ||
	    !cmd_text_word_is(words, 0, COMMAND_INGEST_APP_NAME)) {
		return 0;
	}
	if (cmd_text_word_is(words, 1, COMMAND_INGEST_CMD_ARM_KEY_ROTATION_NAME)) {
		return COMMAND_INGEST_CMD_ARM_KEY_ROTATION;
	}
	if (cmd_text_word_is(words, 1, COMMAND_INGEST_CMD_ROTATE_KEY_NAME)) {
		return COMMAND_INGEST_CMD_ROTATE_KEY;
	}
	return 0;
}

/* Forget an arm that has not been fired in time. */
static void expire_arm(int64_t met_ms)
{
	if (armed && met_ms - armed_at_met_ms > ROTATION_ARM_WINDOW_MS) {
		armed = false;
	}
	hk.rotation_armed = armed;
}

/*
 * Carry out a rotation command signed with signed_by's key. Returns the
 * result word for the reply, setting *bad_arg for "bad_arg". The slot named
 * must be the slot whose key signed the command, and that must be the
 * spare slot (DS-54).
 */
static const char *rotate(uint16_t command, const struct cmd_text *words, uint8_t signed_by,
			  int64_t met_ms, int *bad_arg)
{
	uint64_t slot;

	if (words->count != 3) {
		return "bad_arg_count";
	}
	if (cmd_text_unsigned(words, 2, CMD_COUNTER_SLOTS - 1, &slot) != 0) {
		*bad_arg = 0;
		return "bad_arg";
	}
	if (signed_by == active_slot || slot != signed_by) {
		return "wrong_key";
	}
	if (command == COMMAND_INGEST_CMD_ARM_KEY_ROTATION) {
		armed = true;
		armed_slot = (uint8_t)slot;
		armed_at_met_ms = met_ms;
		hk.rotation_armed = true;
		return "ok";
	}
	expire_arm(met_ms);
	if (!armed || armed_slot != slot) {
		return "not_armed";
	}
	active_slot = (uint8_t)slot;
	armed = false;
	hk.rotation_armed = false;
	hk.active_slot = active_slot;
	hk.rotations++;
	return "ok";
}

/*
 * Tell the mode manager the ground was heard from, for the command-loss
 * timer (DS-46). Every accepted command counts, whatever it does.
 */
static void heard_from_ground(int64_t met_ms)
{
	const struct ground_contact contact = {
		.contacted = true,
		.last_accepted_met_ms = met_ms,
	};

	zbus_chan_pub(&ground_contact_chan, &contact, K_NO_WAIT);
}

/* Take one uplink frame through the pipeline. */
static void ingest(const struct link_frame *frame, int64_t met_ms)
{
	struct cmd_auth_packet packet;
	struct cmd_route_info info;
	struct cmd_text words;
	enum cmd_route_result routed;
	uint16_t rotation;
	uint8_t signed_by;

	hk.received++;

	/* Stages 1 and 2: nothing is trusted until the tag verifies. */
	if (cmd_auth_parse(frame->data, frame->len, &packet) != 0) {
		hk.rejected_shape++;
		return;
	}
	if (cmd_auth_verify(frame->data, frame->len, &packet, cmd_keys[active_slot])) {
		signed_by = active_slot;
	} else if (cmd_auth_verify(frame->data, frame->len, &packet, cmd_keys[spare_slot()])) {
		signed_by = spare_slot();
	} else {
		hk.rejected_signature++;
		return;
	}

	/*
	 * The spare key may only rotate (DS-54). Anything else it signed is
	 * refused before its counter is used, so it can't move that floor.
	 * The text is authenticated, so reading it here is safe.
	 */
	rotation = rotation_command(&packet, &words);
	if (signed_by != active_slot && rotation == 0) {
		hk.rejected_other_key++;
		reply("NAK", packet.counter, "wrong_key", -1);
		return;
	}

	/* Stages 3 and 4, against the floor of the slot that signed it. */
	switch (cmd_counter_accept(signed_by, packet.counter)) {
	case CMD_COUNTER_OK:
		break;
	case CMD_COUNTER_REPLAY:
		hk.rejected_replay++;
		reply("NAK", packet.counter, "replay", -1);
		return;
	case CMD_COUNTER_JUMP_TOO_LARGE:
		hk.rejected_jump++;
		reply("NAK", packet.counter, "jump", -1);
		return;
	default:
		/* The floor wasn't stored, so the command must not run. */
		hk.rejected_store++;
		reply("NAK", packet.counter, "store", -1);
		return;
	}
	hk.last_counter = packet.counter;
	hk.last_accepted_met_ms = met_ms;
	heard_from_ground(met_ms);

	if (rotation != 0) {
		int bad_arg = -1;
		const char *result = rotate(rotation, &words, signed_by, met_ms, &bad_arg);

		reply("ACK", packet.counter, result, bad_arg);
		return;
	}

	/* Stages 5 to 7: the decoder only ever sees authenticated text. */
	routed = cmd_route(packet.text, packet.text_len, mode, &info);
	if (routed == CMD_ROUTE_OK) {
		hk.routed++;
	} else {
		hk.rejected_route++;
	}
	reply("ACK", packet.counter, route_words[routed],
	      routed == CMD_ROUTE_BAD_ARG ? info.bad_arg : -1);
}

static void step(const struct frame_tick *tick)
{
	struct mode_state state;
	struct link_frame frame;
	bool was_armed = armed;
	bool handled = false;

	/* Mode gating uses the mode right now (DS-40, DS-50). */
	if (zbus_chan_read(&mode_chan, &state, K_NO_WAIT) == 0) {
		mode = state.mode;
	}
	expire_arm(tick->met_ms);
	while (k_msgq_get(&uplink_msgq, &frame, K_NO_WAIT) == 0) {
		ingest(&frame, tick->met_ms);
		handled = true;
	}
	/* At least once a major frame (DS-14), and whenever something changed. */
	if (handled || armed != was_armed || tick->slot == 0) {
		zbus_chan_pub(&command_ingest_hk_chan, &hk, K_NO_WAIT);
	}
}

static void command_ingest_main(void *a, void *b, void *c)
{
	const struct zbus_channel *chan;
	union command_ingest_msg msg;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	while (zbus_sub_wait_msg(&command_ingest_sub, &chan, &msg, K_FOREVER) == 0) {
		if (chan == &command_ingest_wakeup_chan) {
			step(&msg.tick);
			status.steps++;
		} else if (chan == &command_ingest_cmd_chan) {
			/*
			 * Rotation commands are handled in ingest(), never routed
			 * here, and no other app may rotate keys.
			 */
			status.cmd_rejected++;
		}
		zbus_chan_pub(&command_ingest_status_chan, &status, K_NO_WAIT);
	}
}

K_THREAD_DEFINE(command_ingest_tid, COMMAND_INGEST_STACK_SIZE, command_ingest_main, NULL, NULL,
		NULL, COMMAND_INGEST_PRIORITY, 0, 0);
