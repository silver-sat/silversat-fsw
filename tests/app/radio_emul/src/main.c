/*
 * Radio app tests against an emulated UART (DS-33, DS-46).
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The radio app runs as it does in flight, on Zephyr's emulated UART
 * (app.overlay). The tests stand in for:
 *   - the radio board: they put encoded frames in the UART's receive side
 *     and decode what the app sends;
 *   - the frame manager: they publish ticks to the radio's wakeup channel;
 *   - command ingest: they publish commands to the radio's command channel,
 *     read the uplink queue, and fill the downlink queue.
 * Everything runs in simulated time. tests/app/radio covers the same app
 * end to end, in real time, through a pseudo-terminal.
 */

#include <string.h>

#include <zephyr/drivers/serial/uart_emul.h>
#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/ztest.h>

#include "app_test.h"
#include "msg/common.h"
#include "msg/radio.h"
#include "silversat/link.h"
#include "silversat/link_codec.h"
#include "silversat/resource_map.h"

#define WAIT K_SECONDS(1)

static const struct device *const uart = DEVICE_DT_GET(RADIO_UART_NODE);

static uint32_t frame_count;

/* ---- Standing in for the frame manager and command ingest -------------- */

/* Wake the radio for one minor frame, wait for it, then let the UART send. */
static void wake(void)
{
	struct app_status before;

	zassert_ok(zbus_chan_read(&radio_status_chan, &before, K_MSEC(10)));
	zassert_ok(app_test_wake(&radio_wakeup_chan, frame_count++));
	zassert_ok(app_test_wait_status(&radio_status_chan, before.steps + 1, 0, WAIT, NULL));
	k_sleep(K_MSEC(10));
}

/* Send the radio one command and wait until it has been handled. */
static struct app_status command(uint16_t id, bool enabled)
{
	const struct radio_cmd cmd = {
		.id = id,
		.args.set_transmit = {.enabled = enabled},
	};
	struct app_status before;
	struct app_status after;

	zassert_ok(zbus_chan_read(&radio_status_chan, &before, K_MSEC(10)));
	zassert_ok(zbus_chan_pub(&radio_cmd_chan, &cmd, K_NO_WAIT));
	zassert_ok(app_test_wait_status(&radio_status_chan, 0,
					before.cmd_accepted + before.cmd_rejected + 1, WAIT,
					&after));
	k_sleep(K_MSEC(10));
	after.cmd_accepted -= before.cmd_accepted;
	after.cmd_rejected -= before.cmd_rejected;
	return after;
}

static void set_transmit(bool enabled)
{
	zassert_equal(command(RADIO_CMD_SET_TRANSMIT, enabled).cmd_accepted, 1);
}

static void downlink(const char *text)
{
	struct link_frame frame = {.len = (uint8_t)strlen(text)};

	memcpy(frame.data, text, frame.len);
	zassert_ok(k_msgq_put(&downlink_msgq, &frame, K_NO_WAIT));
}

static struct radio_hk radio_hk(void)
{
	struct radio_hk hk;

	zassert_ok(zbus_chan_read(&radio_hk_chan, &hk, K_MSEC(10)));
	return hk;
}

/*
 * The radio's housekeeping after running to the next slot 0, where the
 * radio publishes its counters (DS-14).
 */
static struct radio_hk counters(void)
{
	do {
		wake();
	} while ((frame_count - 1) % FRAME_SLOTS != 0);
	return radio_hk();
}

/* ---- Standing in for the radio board ------------------------------------ */

/*
 * Decode everything the app has sent since the last call. Returns the
 * number of packets; the payload of the last one goes in *last.
 */
static int sent(struct link_packet *last)
{
	static struct link_decoder decoder;
	struct link_packet packet;
	uint8_t byte;
	int count = 0;

	link_decoder_init(&decoder);
	while (uart_emul_get_tx_data(uart, &byte, 1) == 1) {
		if (link_decode_byte(&decoder, byte, &packet) == LINK_DECODE_PACKET) {
			count++;
			if (last != NULL) {
				*last = packet;
			}
		}
	}
	return count;
}

static void radio_sends(const char *text)
{
	const struct link_packet packet = {.type = 0x00, .seq = 0, .len = (uint8_t)strlen(text)};
	struct link_packet copy = packet;
	uint8_t encoded[LINK_ENCODED_MAX];
	int len;

	memcpy(copy.payload, text, copy.len);
	len = link_encode(&copy, encoded, sizeof(encoded));
	zassert_true(len > 0);
	zassert_equal(uart_emul_put_rx_data(uart, encoded, (size_t)len), (uint32_t)len);
	k_sleep(K_MSEC(10));
}

/* ---- Tests --------------------------------------------------------------- */

static void *setup(void)
{
	/* Let the radio thread start before the first test looks at it. */
	k_sleep(K_MSEC(10));
	zassert_true(radio_hk().transmit_enabled,
		     "transmission is on from boot, and the channel says so at once");
	return NULL;
}

/* Each test starts with transmission on and nothing queued either way. */
static void reset(void *fixture)
{
	struct link_frame frame;

	ARG_UNUSED(fixture);
	set_transmit(true);
	while (k_msgq_get(&downlink_msgq, &frame, K_NO_WAIT) == 0) {
	}
	while (k_msgq_get(&uplink_msgq, &frame, K_NO_WAIT) == 0) {
	}
	(void)sent(NULL);
}

ZTEST_SUITE(radio_emul, NULL, setup, reset, NULL, NULL);

ZTEST(radio_emul, test_sends_while_enabled)
{
	struct link_packet packet;

	downlink("ACK 1 ok");
	wake();
	zassert_equal(sent(&packet), 1);
	zassert_equal(packet.len, strlen("ACK 1 ok"));
	zassert_mem_equal(packet.payload, "ACK 1 ok", packet.len);
}

ZTEST(radio_emul, test_stopped_sends_nothing_and_counts)
{
	uint32_t discarded = counters().frames_discarded;

	set_transmit(false);
	zassert_false(radio_hk().transmit_enabled, "published when it changes, not at slot 0");

	downlink("H telemetry");
	downlink("ACK 2 ok");
	downlink("H more telemetry");
	wake();
	zassert_equal(sent(NULL), 0, "nothing reaches the UART");
	zassert_equal(k_msgq_num_used_get(&downlink_msgq), 0,
		      "thrown away, not held for a stale burst later");
	zassert_equal(counters().frames_discarded - discarded, 3);
}

ZTEST(radio_emul, test_reply_to_stop_still_goes_out)
{
	struct link_packet packet;

	/* Command ingest queues its reply before the radio handles the command. */
	downlink("ACK 3 ok");
	set_transmit(false);
	zassert_equal(sent(&packet), 1, "the ground sees the ACK, then silence");
	zassert_mem_equal(packet.payload, "ACK 3 ok", packet.len);

	downlink("ACK 4 ok");
	wake();
	zassert_equal(sent(NULL), 0);
}

ZTEST(radio_emul, test_receives_while_stopped)
{
	struct link_frame frame;

	set_transmit(false);
	radio_sends("ground command");
	wake();
	zassert_ok(k_msgq_get(&uplink_msgq, &frame, K_NO_WAIT),
		   "ground commands still get through, so the ground can turn it back on");
	zassert_mem_equal(frame.data, "ground command", frame.len);
}

ZTEST(radio_emul, test_start_again)
{
	uint32_t discarded;

	set_transmit(false);
	discarded = counters().frames_discarded;
	set_transmit(true);
	zassert_true(radio_hk().transmit_enabled);

	downlink("ACK 5 ok");
	wake();
	zassert_equal(sent(NULL), 1);
	zassert_equal(counters().frames_discarded, discarded);
}

ZTEST(radio_emul, test_sets_rather_than_toggles)
{
	/* The same command twice leaves the same state (DS-35). */
	set_transmit(false);
	set_transmit(false);
	zassert_false(radio_hk().transmit_enabled);
	set_transmit(true);
	set_transmit(true);
	zassert_true(radio_hk().transmit_enabled);
}

ZTEST(radio_emul, test_unknown_command_is_rejected)
{
	struct app_status result = command(99, false);

	zassert_equal(result.cmd_rejected, 1);
	zassert_equal(result.cmd_accepted, 0);
	zassert_true(radio_hk().transmit_enabled, "a rejected command changes nothing");
}
