/*
 * Radio app (DS-32, DS-33).
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The proxy for the radio board. It carries ground traffic between the
 * radio's UART and the link queues (DS-11):
 *
 *   radio --UART--> [interrupt] rx_ring --> decode --> uplink_msgq --> command ingest
 *   radio <--UART-- [interrupt] tx_ring <-- encode <-- downlink_msgq <-- command ingest
 *
 * Every frame on the UART uses the link codec (DS-65): type, seq, len,
 * payload, CRC-32C, KISS-escaped. Ground traffic uses the standard KISS
 * data frame, type 0x00, both ways (DS-66). The radio is not yet selected,
 * so no other type byte is handled yet.
 *
 * The UART interrupt does nothing but move bytes between the UART and two
 * ring buffers. Nothing is published from it. Everything else happens here,
 * in the app's thread, once per minor frame, and nothing waits: a full
 * queue drops and counts (DS-33).
 *
 * set_transmit stops all transmission to the ground (DS-46). Every packet
 * avionics sends, replies, telemetry, and later the beacon, goes out
 * through transmit() below, so this one switch silences them all. The
 * radio keeps receiving, so the ground can always turn it back on.
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/zbus/zbus.h>

#include "msg/common.h"
#include "msg/radio.h"
#include "silversat/link.h"
#include "silversat/link_codec.h"
#include "silversat/resource_map.h"

/* The standard KISS data frame: ground traffic, both ways (DS-66). */
#define RADIO_TYPE_DATA 0x00

ZBUS_MSG_SUBSCRIBER_DEFINE(radio_sub);
ZBUS_CHAN_ADD_OBS(radio_wakeup_chan, radio_sub, 3);
ZBUS_CHAN_ADD_OBS(radio_cmd_chan, radio_sub, 3);

static const struct device *const uart = DEVICE_DT_GET(RADIO_UART_NODE);

/*
 * Each ring buffer has one writer and one reader, the interrupt and this
 * app, so no lock is needed (see ring_buffer.h).
 */
RING_BUF_DECLARE(rx_ring, RADIO_RX_RING_SIZE);
RING_BUF_DECLARE(tx_ring, RADIO_TX_RING_SIZE);

/* Bytes the interrupt could not fit in rx_ring. */
static atomic_t rx_overrun;

static struct link_decoder decoder;
static struct link_seq seq;
static struct app_status status;
static struct radio_hk hk = {.transmit_enabled = true};

/* ---- The UART interrupt: bytes only ------------------------------------ */

static void uart_isr(const struct device *dev, void *user_data)
{
	ARG_UNUSED(user_data);

	if (uart_irq_update(dev) <= 0) {
		return;
	}
	if (uart_irq_rx_ready(dev)) {
		uint8_t bytes[32];
		int got = uart_fifo_read(dev, bytes, sizeof(bytes));

		if (got > 0) {
			uint32_t kept = ring_buf_put(&rx_ring, bytes, (uint32_t)got);

			if (kept < (uint32_t)got) {
				atomic_add(&rx_overrun, (atomic_val_t)(got - kept));
			}
		}
	}
	if (uart_irq_tx_ready(dev)) {
		uint8_t *bytes;
		uint32_t ready = ring_buf_get_claim(&tx_ring, &bytes, 32);

		if (ready == 0) {
			uart_irq_tx_disable(dev); /* nothing left to send */
		} else {
			int sent = uart_fifo_fill(dev, bytes, (int)ready);

			ring_buf_get_finish(&tx_ring, sent > 0 ? (uint32_t)sent : 0);
		}
	}
}

/* ---- Receive: radio to ground commands ---------------------------------- */

static void received(const struct link_packet *packet, int64_t met_ms)
{
	struct link_frame frame = {.len = packet->len};
	int lost = link_seq_received(&seq, packet->seq);

	hk.frames_received++;
	hk.last_rx_met_ms = met_ms;
	if (lost < 0) {
		hk.frames_repeated++;
		return; /* the same frame twice: pass it on once */
	}
	hk.frames_lost += (uint32_t)lost;

	if (packet->type != RADIO_TYPE_DATA) {
		hk.unknown_type++;
		return;
	}
	memcpy(frame.data, packet->payload, packet->len);
	if (k_msgq_put(&uplink_msgq, &frame, K_NO_WAIT) != 0) {
		hk.uplink_dropped++;
	}
}

static void receive(int64_t met_ms)
{
	struct link_packet packet;
	uint8_t byte;

	while (ring_buf_get(&rx_ring, &byte, 1) == 1) {
		switch (link_decode_byte(&decoder, byte, &packet)) {
		case LINK_DECODE_NONE:
			break;
		case LINK_DECODE_PACKET:
			received(&packet, met_ms);
			break;
		case LINK_DECODE_BAD_CRC:
			hk.bad_crc++;
			break;
		case LINK_DECODE_BAD_LENGTH:
			hk.bad_length++;
			break;
		case LINK_DECODE_BAD_ESCAPE:
			hk.bad_escape++;
			break;
		case LINK_DECODE_TOO_LONG:
			hk.too_long++;
			break;
		}
	}
	hk.rx_overrun = (uint32_t)atomic_get(&rx_overrun);
}

/* ---- Transmit: replies and telemetry to the radio ---------------------- */

static void transmit(void)
{
	struct link_frame frame;
	struct link_packet packet = {.type = RADIO_TYPE_DATA};
	uint8_t encoded[LINK_ENCODED_MAX];
	bool queued = false;

	/*
	 * Look before taking: a frame that doesn't fit in tx_ring yet stays in
	 * the downlink queue for the next minor frame.
	 */
	while (k_msgq_peek(&downlink_msgq, &frame) == 0) {
		int len;

		packet.len = frame.len;
		memcpy(packet.payload, frame.data, frame.len);
		packet.seq = seq.next_tx;
		len = link_encode(&packet, encoded, sizeof(encoded));
		if (len < 0 || ring_buf_space_get(&tx_ring) < (uint32_t)len) {
			break;
		}
		(void)k_msgq_get(&downlink_msgq, &frame, K_NO_WAIT);
		(void)link_seq_next(&seq);
		ring_buf_put(&tx_ring, encoded, (uint32_t)len);
		hk.frames_sent++;
		queued = true;
	}
	if (queued) {
		uart_irq_tx_enable(uart);
	}
}

/*
 * While transmission is stopped, throw away what's waiting. Holding it
 * would fill the downlink queue, and turning transmission back on would
 * then send a burst of stale packets.
 */
static void discard(void)
{
	struct link_frame frame;

	while (k_msgq_get(&downlink_msgq, &frame, K_NO_WAIT) == 0) {
		hk.frames_discarded++;
	}
}

static void step(const struct frame_tick *tick)
{
	receive(tick->met_ms);
	if (hk.transmit_enabled) {
		transmit();
	} else {
		discard();
	}
	if (tick->slot == 0) {
		zbus_chan_pub(&radio_hk_chan, &hk, K_NO_WAIT);
	}
}

/* ---- Commands ----------------------------------------------------------- */

/*
 * Until FRAM, this state is in RAM, so a reset turns transmission back on
 * (DS-41, DS-46).
 */
static void set_transmit(const struct radio_set_transmit *args)
{
	if (!args->enabled && hk.transmit_enabled) {
		/*
		 * Send what is already queued before stopping. Command ingest
		 * runs at this app's priority in the same slot, after it, so the
		 * reply to this command is already in the queue: the ground sees
		 * the ACK, then silence.
		 */
		transmit();
	}
	hk.transmit_enabled = args->enabled;
}

static int handle_command(const struct radio_cmd *cmd)
{
	switch (cmd->id) {
	case RADIO_CMD_SET_TRANSMIT:
		set_transmit(&cmd->args.set_transmit);
		return 0;
	default:
		return -ENOTSUP;
	}
}

static void radio_main(void *a, void *b, void *c)
{
	const struct zbus_channel *chan;
	union radio_msg msg;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	link_decoder_init(&decoder);
	link_seq_init(&seq);
	if (device_is_ready(uart)) {
		uart_irq_callback_user_data_set(uart, uart_isr, NULL);
		uart_irq_rx_enable(uart);
	}
	/*
	 * If the UART isn't ready, nothing arrives and nothing is sent; the
	 * steps still count, so health sees the app running, and the ground
	 * sees frames_received stay at zero.
	 */

	/* So the channel never shows the zeroed default, transmission off. */
	zbus_chan_pub(&radio_hk_chan, &hk, K_NO_WAIT);

	while (zbus_sub_wait_msg(&radio_sub, &chan, &msg, K_FOREVER) == 0) {
		if (chan == &radio_wakeup_chan) {
			step(&msg.tick);
			status.steps++;
		} else if (chan == &radio_cmd_chan) {
			if (handle_command(&msg.cmd) == 0) {
				status.cmd_accepted++;
			} else {
				status.cmd_rejected++;
			}
			/* The state changed: publish now, not at the next slot 0 (DS-14). */
			zbus_chan_pub(&radio_hk_chan, &hk, K_NO_WAIT);
		}
		zbus_chan_pub(&radio_status_chan, &status, K_NO_WAIT);
	}
}

K_THREAD_DEFINE(radio_tid, RADIO_STACK_SIZE, radio_main, NULL, NULL, NULL, RADIO_PRIORITY, 0, 0);
