/*
 * Radio end-to-end test image: the flight apps for ground commands, driven
 * by the Python tests in pytest/ through the radio UART.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Telemetry output sends the radio's housekeeping on the downlink only
 * once per round of apps, every few seconds. To keep the real-time tests
 * short, this also prints it once a second for them to read from the
 * console.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/zbus/zbus.h>

#include "msg/radio.h"

int main(void)
{
	struct radio_hk hk;

	/* The apps start themselves; this only tells the test it can begin. */
	printk("radio test ready\n");
	for (;;) {
		k_sleep(K_SECONDS(1));
		if (zbus_chan_read(&radio_hk_chan, &hk, K_MSEC(10)) == 0) {
			printk("radio hk: received=%u sent=%u bad_crc=%u bad_length=%u "
			       "bad_escape=%u too_long=%u lost=%u repeated=%u unknown_type=%u "
			       "uplink_dropped=%u rx_overrun=%u\n",
			       hk.frames_received, hk.frames_sent, hk.bad_crc, hk.bad_length,
			       hk.bad_escape, hk.too_long, hk.frames_lost, hk.frames_repeated,
			       hk.unknown_type, hk.uplink_dropped, hk.rx_overrun);
		}
	}
	return 0;
}
