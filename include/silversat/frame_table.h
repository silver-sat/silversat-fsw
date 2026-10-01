/*
 * Frame tables: which app the frame manager wakes in which slot (DS-21,
 * DS-23).
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * There is one const table per mode, indexed by enum mode. The flight
 * tables are in apps/frame_manager/frame_tables.c. A test that needs
 * different tables turns off CONFIG_SS_FRAME_MANAGER_FLIGHT_TABLES and
 * defines frame_tables[] itself.
 *
 * Whether an entry is enabled is not stored here: tables stay in flash,
 * and the frame manager keeps the enable flags in RAM.
 */

#ifndef SILVERSAT_FRAME_TABLE_H_
#define SILVERSAT_FRAME_TABLE_H_

#include <stdint.h>

#include <zephyr/zbus/zbus.h>

#include "msg/common.h"

/* One app woken in one slot. A 10 Hz app has ten entries, one per slot. */
struct frame_entry {
	/* Minor-frame slot, 0 to FRAME_SLOTS - 1. */
	uint8_t slot;
	/* The app (enum app_id). Selects its row in app_attrs[] (DS-07). */
	uint8_t app;
	/* The app's wakeup channel: the frame manager publishes the tick here. */
	const struct zbus_channel *wakeup_chan;
	/* The app's status channel: the frame manager reads its steps (DS-22). */
	const struct zbus_channel *status_chan;
};

struct frame_table {
	const struct frame_entry *entries;
	uint8_t len;
};

/* One table per mode, indexed by enum mode. */
extern const struct frame_table frame_tables[MODE_MAX + 1];

/*
 * Check one table: no more than FRAME_ENTRIES_MAX entries, each with a slot
 * in range, a known app, and both channels. Returns 0, or -EINVAL. The
 * frame manager checks every table at startup.
 */
int frame_table_check(const struct frame_table *table);

#endif /* SILVERSAT_FRAME_TABLE_H_ */
