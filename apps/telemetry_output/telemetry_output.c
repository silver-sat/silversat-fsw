/*
 * Telemetry output (DS-10, DS-69).
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Sends housekeeping to the ground. Once a major frame, the frame manager
 * wakes this app; it takes the next app in turn, encodes that app's latest
 * housekeeping as an 'H' packet (include/silversat/tlm_encode.h), and puts
 * the packet in the downlink queue for the radio. With N apps, each app's
 * housekeeping goes down every N seconds, until the telemetry budget is
 * known (DS-73).
 *
 * Every app publishes its housekeeping at least once a major frame, so the
 * latest value is never more than a second old (DS-14). Nothing here waits:
 * a full downlink queue drops the packet and counts it.
 *
 * Events (DS-10): then it takes up to TLM_EVENTS_PER_FRAME events from the
 * event queue (silversat/event.h), oldest first, and sends each as an 'E'
 * packet, leaving TLM_DOWNLINK_RESERVE places in the downlink queue free
 * for command replies. Events wait in their own queue meanwhile, including
 * through deploy mode, when this app doesn't run.
 *
 * Still to come: the beacon (DS-69), and RTC time (until there is an RTC,
 * packets carry MET from the tick, DS-25).
 */

#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>

#include "msg/common.h"
#include "msg/telemetry_output.h"
#include "silversat/event.h"
#include "silversat/link.h"
#include "silversat/resource_map.h"
#include "silversat/tlm_encode.h"

ZBUS_MSG_SUBSCRIBER_DEFINE(telemetry_output_sub);
ZBUS_CHAN_ADD_OBS(telemetry_output_wakeup_chan, telemetry_output_sub, 3);
ZBUS_CHAN_ADD_OBS(telemetry_output_cmd_chan, telemetry_output_sub, 3);

static struct app_status status;
static struct telemetry_output_hk hk;

/* Index into tlm_hk_apps of the app whose turn is next. */
static size_t next_app;

/*
 * The packet being built. One buffer, not one on the stack per packet: a
 * link frame is 256 bytes, and k_msgq_put() copies it into the downlink
 * queue, so this app's single thread can reuse it for every packet.
 */
static struct link_frame frame;

/* Send queued events, oldest first, while the downlink has room (DS-10). */
static void send_events(void)
{
	for (int i = 0; i < TLM_EVENTS_PER_FRAME; i++) {
		struct event event;
		int len;

		if (k_msgq_num_free_get(&downlink_msgq) <= TLM_DOWNLINK_RESERVE ||
		    event_take(&event) != 0) {
			return;
		}
		len = tlm_encode_event(&event, (uint8_t *)frame.data, sizeof(frame.data));
		if (len < 0) {
			hk.encode_errors++;
			continue;
		}
		frame.len = (uint8_t)len;
		if (k_msgq_put(&downlink_msgq, &frame, K_NO_WAIT) == 0) {
			hk.events_sent++;
		} else {
			hk.events_lost++; /* the downlink filled since the check */
		}
	}
}

static void step(const struct frame_tick *tick)
{
	uint8_t app = tlm_hk_apps[next_app];
	int len;

	next_app = (next_app + 1) % tlm_hk_app_count;

	len = tlm_encode_hk(app, tick->met_ms, (uint8_t *)frame.data, sizeof(frame.data));
	if (len < 0) {
		hk.encode_errors++;
	} else {
		frame.len = (uint8_t)len;
		if (k_msgq_put(&downlink_msgq, &frame, K_NO_WAIT) == 0) {
			hk.sent++;
		} else {
			hk.dropped++;
		}
	}
	send_events();
	hk.events_dropped = event_stats().dropped;
	zbus_chan_pub(&telemetry_output_hk_chan, &hk, K_NO_WAIT);
}

static void telemetry_output_main(void *a, void *b, void *c)
{
	const struct zbus_channel *chan;
	union telemetry_output_msg msg;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	while (zbus_sub_wait_msg(&telemetry_output_sub, &chan, &msg, K_FOREVER) == 0) {
		if (chan == &telemetry_output_wakeup_chan) {
			step(&msg.tick);
			status.steps++;
		} else if (chan == &telemetry_output_cmd_chan) {
			status.cmd_rejected++; /* no commands yet */
		}
		zbus_chan_pub(&telemetry_output_status_chan, &status, K_NO_WAIT);
	}
}

K_THREAD_DEFINE(telemetry_output_tid, TELEMETRY_OUTPUT_STACK_SIZE, telemetry_output_main, NULL,
		NULL, NULL, TELEMETRY_OUTPUT_PRIORITY, 0, 0);
