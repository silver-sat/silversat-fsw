/*
 * Link frame queues between the radio and the apps that use its frames
 * (DS-11, DS-32).
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * zbus carries only small messages, so a raw link frame (up to 255 bytes,
 * DS-66) goes through one of these static queues instead. They are the one
 * place apps share something other than a zbus channel; DS-11 allows it for
 * link frames only. Every put and get uses K_NO_WAIT: a full queue drops
 * the frame, and the sender counts the drop.
 */

#ifndef SILVERSAT_LINK_H_
#define SILVERSAT_LINK_H_

#include <stdint.h>

#include <zephyr/kernel.h>

#define LINK_FRAME_MAX 255

/* One frame: the bytes inside the KISS framing, after the CRC check. */
struct link_frame {
	uint8_t len;
	char data[LINK_FRAME_MAX];
};

/* From the radio app to command ingest: signed ground commands. */
extern struct k_msgq uplink_msgq;

/* From command ingest (and later telemetry output) to the radio app. */
extern struct k_msgq downlink_msgq;

#endif /* SILVERSAT_LINK_H_ */
