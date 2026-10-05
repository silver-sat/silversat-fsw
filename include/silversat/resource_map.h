/*
 * Resource map (DS-07).
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The one place that sets frame timing, thread priorities, stack sizes, the
 * zbus buffer pool, UART assignments, and each app's attribute row. Apps
 * take every such number from here; never hard-code one in an app.
 *
 * Adding an app: give it a priority and stack size below, and a row in
 * app_attrs[]. The pool check at the bottom tells you if the zbus pool
 * needs to grow.
 */

#ifndef SILVERSAT_RESOURCE_MAP_H_
#define SILVERSAT_RESOURCE_MAP_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/devicetree.h>
#include <zephyr/sys/util.h>

#include "msg/common.h"

/* ---- Frame timing (DS-21) ---------------------------------------------- */

/* A major frame of 1 second, divided into minor-frame slots. */
#define FRAME_MINOR_MS 100
#define FRAME_SLOTS    10

BUILD_ASSERT(FRAME_MINOR_MS * FRAME_SLOTS == 1000, "a major frame is 1 second (DS-21)");

/* The most entries one frame table may have. Sizes the enable flags. */
#define FRAME_ENTRIES_MAX 32

/* ---- Thread priorities (DS-24) ----------------------------------------- */

/*
 * Rate-monotonic: the shorter an app's period, the higher its priority. In
 * Zephyr a lower number is a higher priority. Every app thread is
 * preemptible. The bands leave gaps so a new app can be placed between them.
 *
 * An app's rate, and so its band, follows the rate its data is used at.
 * A sensor app that feeds ADCS runs at ADCS's rate, in the slot before it
 * (DS-21), and shares its band. A sensor app whose data only goes to
 * housekeeping runs at 1 Hz or slower. Event-driven apps (command ingest,
 * subsystem receive paths) are placed by the response time they need.
 *
 * Priority 0 is left to the main thread, which only starts the system.
 */
#define PRIORITY_FRAME_MANAGER 1
#define PRIORITY_10HZ          3 /* for example, ADCS and the sensors it reads */
#define PRIORITY_1HZ           5
#define PRIORITY_HOUSEKEEPING  7 /* housekeeping and telemetry output */

#define FRAME_MANAGER_PRIORITY PRIORITY_FRAME_MANAGER

/* Woken every minor frame, so a ground command waits at most one frame. */
#define COMMAND_INGEST_PRIORITY PRIORITY_10HZ

/* Woken every minor frame to move frames to and from the radio. */
#define RADIO_PRIORITY PRIORITY_10HZ

/* Woken once a major frame to send one app's housekeeping. */
#define TELEMETRY_OUTPUT_PRIORITY PRIORITY_HOUSEKEEPING

/* Woken once a major frame to check its triggers (DS-40, DS-46). */
#define MODE_MANAGER_PRIORITY PRIORITY_1HZ

/* ---- Stack sizes (bytes) ------------------------------------------------ */

#define FRAME_MANAGER_STACK_SIZE 1024
#define COMMAND_INGEST_STACK_SIZE 2048 /* BLAKE2s state, one link frame */
#define RADIO_STACK_SIZE          2048 /* one decoded and one encoded link frame */
#define TELEMETRY_OUTPUT_STACK_SIZE 1024 /* one link frame */
#define MODE_MANAGER_STACK_SIZE     1024

/* ---- UART assignments --------------------------------------------------- */

/*
 * Each subsystem's UART is a devicetree alias, bound to a real UART in each
 * board's overlay (app/boards/). On native_sim it is a pseudo-terminal that
 * a Python simulator connects to (DS-90).
 *
 *   radio-uart   native_sim: uart1 (PTY)
 *                nucleo_f446re: usart1 on PA9/PA10, 19200 baud (SilverSat 1's
 *                rate), pending the five-UART pin-mux check
 */
#define RADIO_UART_NODE DT_ALIAS(radio_uart)

/*
 * The test signal (DS-42): present at boot, it puts the spacecraft in test
 * mode. A devicetree alias, like the UARTs; a board without it never enters
 * test mode.
 *
 *   test-mode-signal   native_sim: an emulated GPIO pin (app/boards/)
 *                      nucleo_f446re: the blue user button, held during
 *                      reset, until the real signal is chosen
 */
#define TEST_SIGNAL_NODE DT_ALIAS(test_mode_signal)

/*
 * Bytes buffered between the UART interrupt and the radio app, each way.
 * At 19200 baud, 1024 bytes is about half a second: five minor frames.
 */
#define RADIO_RX_RING_SIZE 1024
#define RADIO_TX_RING_SIZE 1024

/* ---- Link frame queues (DS-11) ------------------------------------------ */

/*
 * Raw link frames are too big for zbus (every zbus buffer is sized for the
 * largest message), so they travel through these static queues instead.
 * Each entry holds one whole frame (struct link_frame, 256 bytes).
 */

/* Uplink frames waiting for command ingest, which empties it every frame. */
#define UPLINK_QUEUE_DEPTH 4

/* Downlink frames waiting for the radio: command replies, later telemetry. */
#define DOWNLINK_QUEUE_DEPTH 8

/* ---- zbus buffer pool (DS-07, DS-22) ------------------------------------ */

/*
 * zbus takes one buffer from a shared pool for every publish on every
 * channel, observed or not, and holds it until the publish returns. Each
 * message subscriber that receives the message takes one more buffer (its
 * copy), held until the app reads it. If a copy cannot be allocated, the
 * publish returns -ENOMEM; if the publish's own buffer cannot be allocated,
 * zbus asserts, and a flight build (asserts off) dereferences NULL.
 *
 * So the pool must hold every copy that can be queued, plus one buffer for
 * every thread that can be part-way through a publish. Each count below is
 * per app.
 */

/* Wakeups queued for an app. The frame manager enforces it (DS-22). */
#define FRAME_MAX_PENDING 2

/*
 * Commands queued for an app from one sender. Command routing enforces it
 * for ground commands: once an app has this many it hasn't handled, the
 * next gets "busy" (DS-50). The generated internal command senders enforce
 * it for each app that sends (sends: in its YAML), returning -EBUSY.
 */
#define CMD_MAX_PENDING 2

/* Each app is one thread, in at most one publish at a time (DS-10). */
#define POOL_PUBLISHING_THREADS APP_COUNT

/*
 * There are no housekeeping requests: each app publishes its housekeeping
 * at least once a major frame, and telemetry output reads the latest
 * (DS-14).
 */
#define POOL_REQUIRED                                                                  \
	(FRAME_MAX_PENDING * APP_WAKEUP_COUNT + CMD_MAX_PENDING * APP_COUNT +              \
	 CMD_MAX_PENDING * APP_SEND_PAIR_COUNT + POOL_PUBLISHING_THREADS)

#if defined(CONFIG_ZBUS_MSG_SUBSCRIBER)
BUILD_ASSERT(CONFIG_ZBUS_MSG_SUBSCRIBER_NET_BUF_POOL_SIZE >= POOL_REQUIRED,
	     "zbus pool too small: raise CONFIG_ZBUS_MSG_SUBSCRIBER_NET_BUF_POOL_SIZE "
	     "to at least POOL_REQUIRED in include/silversat/resource_map.h");
#endif

/* ---- Per-app attributes (DS-07, DS-23, DS-43) --------------------------- */

/* What health does when a disabled app could run again (DS-43). */
enum reenable_policy {
	/* Stays disabled. */
	REENABLE_NEVER,
	/* Only a ground command re-enables it. */
	REENABLE_GROUND,
	/*
	 * Health re-enables it after a cooldown and watches it. After
	 * auto_retry_cap automatic re-enables, it falls back to
	 * REENABLE_GROUND with an event.
	 */
	REENABLE_AUTO,
};

/*
 * One row per app, read by the frame manager (protected) and health (the
 * rest). Indexed by enum app_id. An app with no row gets all zeros: not
 * protected, not watched by health, never re-enabled.
 */
struct app_attr {
	/*
	 * A protected app's frame entries cannot be disabled; a stall resets
	 * the spacecraft instead (DS-23, DS-43).
	 */
	bool protected;
	/*
	 * Major frames an app may go without stepping while it has wakeups
	 * pending before health declares it stalled. 0: health does not watch
	 * this app.
	 */
	uint8_t stall_threshold;
	enum reenable_policy reenable;
	/* For REENABLE_AUTO: automatic re-enables before falling back. */
	uint8_t auto_retry_cap;
};

static const struct app_attr app_attrs[APP_ID_MAX + 1] = {
	/*
	 * The frame manager has no wakeups, so health cannot compare its
	 * steps. If it stalls, health stops being woken and stops feeding
	 * the watchdog (DS-43).
	 */
	[APP_ID_FRAME_MANAGER] = {
		.protected = false,
		.stall_threshold = 0,
		.reenable = REENABLE_NEVER,
		.auto_retry_cap = 0,
	},
	/*
	 * Protected (DS-43): without it the ground cannot command the
	 * spacecraft. The stall threshold is set when health is written.
	 */
	[APP_ID_COMMAND_INGEST] = {
		.protected = true,
		.stall_threshold = 0,
		.reenable = REENABLE_NEVER,
		.auto_retry_cap = 0,
	},
	/*
	 * Not protected: DS-43's protected list is health, mode manager,
	 * command ingest, and telemetry output. The stall threshold is set
	 * when health is written.
	 */
	[APP_ID_RADIO] = {
		.protected = false,
		.stall_threshold = 0,
		.reenable = REENABLE_NEVER,
		.auto_retry_cap = 0,
	},
	/*
	 * Protected (DS-43): without it the ground sees nothing. The stall
	 * threshold is set when health is written.
	 */
	[APP_ID_TELEMETRY_OUTPUT] = {
		.protected = true,
		.stall_threshold = 0,
		.reenable = REENABLE_NEVER,
		.auto_retry_cap = 0,
	},
	/*
	 * Protected (DS-43): it is the only publisher of the mode, and runs
	 * the command-loss timer (DS-46). The stall threshold is set when
	 * health is written.
	 */
	[APP_ID_MODE_MANAGER] = {
		.protected = true,
		.stall_threshold = 0,
		.reenable = REENABLE_NEVER,
		.auto_retry_cap = 0,
	},
};

#endif /* SILVERSAT_RESOURCE_MAP_H_ */
