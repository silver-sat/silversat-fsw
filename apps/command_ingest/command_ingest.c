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
#include "silversat/link.h"
#include "silversat/resource_map.h"

/* The key slot in use. Switching slots (DS-54) arrives with key rotation. */
#define ACTIVE_SLOT 0

ZBUS_MSG_SUBSCRIBER_DEFINE(command_ingest_sub);
ZBUS_CHAN_ADD_OBS(command_ingest_wakeup_chan, command_ingest_sub, 3);
ZBUS_CHAN_ADD_OBS(command_ingest_cmd_chan, command_ingest_sub, 3);

static struct app_status status;
static struct command_ingest_hk hk;

/* Until mode_chan says otherwise, assume safe mode, the most restrictive. */
static uint8_t mode = MODE_SAFE;

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

/* Take one uplink frame through the pipeline. */
static void ingest(const struct link_frame *frame, int64_t met_ms)
{
	struct cmd_auth_packet packet;
	struct cmd_route_info info;
	enum cmd_route_result routed;

	hk.received++;

	/* Stages 1 and 2: nothing is trusted until the tag verifies. */
	if (cmd_auth_parse(frame->data, frame->len, &packet) != 0) {
		hk.rejected_shape++;
		return;
	}
	if (!cmd_auth_verify(frame->data, frame->len, &packet, cmd_keys[ACTIVE_SLOT])) {
		hk.rejected_signature++;
		return;
	}

	/* Stages 3 and 4: the floor is stored before anything is replied. */
	switch (cmd_counter_accept(ACTIVE_SLOT, packet.counter)) {
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
	bool handled = false;

	/* Mode gating uses the mode right now (DS-40, DS-50). */
	if (zbus_chan_read(&mode_chan, &state, K_NO_WAIT) == 0) {
		mode = state.mode;
	}
	while (k_msgq_get(&uplink_msgq, &frame, K_NO_WAIT) == 0) {
		ingest(&frame, tick->met_ms);
		handled = true;
	}
	if (handled) {
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
			status.cmd_rejected++; /* no commands yet; key rotation adds some */
		}
		zbus_chan_pub(&command_ingest_status_chan, &status, K_NO_WAIT);
	}
}

K_THREAD_DEFINE(command_ingest_tid, COMMAND_INGEST_STACK_SIZE, command_ingest_main, NULL, NULL,
		NULL, COMMAND_INGEST_PRIORITY, 0, 0);
