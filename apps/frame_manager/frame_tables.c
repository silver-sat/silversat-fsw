/*
 * Flight frame tables (DS-21, DS-23): which app the frame manager wakes in
 * which slot, one table per mode.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * To add an app, add an entry for each slot it runs in, in each mode's
 * table where it should run:
 *
 *   {.slot = 0, .app = APP_ID_FOO,
 *    .wakeup_chan = &foo_wakeup_chan, .status_chan = &foo_status_chan},
 *
 * Phase the work on purpose: an app that uses another app's data goes in a
 * later slot than the app that produces it (DS-21). The safe table holds
 * only trusted apps (DS-41).
 */

#include <zephyr/sys/util.h>

#include "msg/common.h"
#include "silversat/frame_table.h"
#include "silversat/resource_map.h"

/*
 * Command ingest runs in every slot of every mode, so a ground command
 * waits at most one minor frame. It is protected (DS-43): these entries
 * can't be disabled.
 */
#define COMMAND_INGEST_IN_SLOT(n)                                                              \
	{.slot = (n), .app = APP_ID_COMMAND_INGEST,                                            \
	 .wakeup_chan = &command_ingest_wakeup_chan, .status_chan = &command_ingest_status_chan}

static const struct frame_entry safe_entries[] = {
	COMMAND_INGEST_IN_SLOT(0), COMMAND_INGEST_IN_SLOT(1), COMMAND_INGEST_IN_SLOT(2),
	COMMAND_INGEST_IN_SLOT(3), COMMAND_INGEST_IN_SLOT(4), COMMAND_INGEST_IN_SLOT(5),
	COMMAND_INGEST_IN_SLOT(6), COMMAND_INGEST_IN_SLOT(7), COMMAND_INGEST_IN_SLOT(8),
	COMMAND_INGEST_IN_SLOT(9),
};

static const struct frame_entry nominal_entries[] = {
	COMMAND_INGEST_IN_SLOT(0), COMMAND_INGEST_IN_SLOT(1), COMMAND_INGEST_IN_SLOT(2),
	COMMAND_INGEST_IN_SLOT(3), COMMAND_INGEST_IN_SLOT(4), COMMAND_INGEST_IN_SLOT(5),
	COMMAND_INGEST_IN_SLOT(6), COMMAND_INGEST_IN_SLOT(7), COMMAND_INGEST_IN_SLOT(8),
	COMMAND_INGEST_IN_SLOT(9),
};

BUILD_ASSERT(ARRAY_SIZE(safe_entries) <= FRAME_ENTRIES_MAX);
BUILD_ASSERT(ARRAY_SIZE(nominal_entries) <= FRAME_ENTRIES_MAX);
BUILD_ASSERT(FRAME_SLOTS == 10, "command ingest has one entry per slot");

const struct frame_table frame_tables[MODE_MAX + 1] = {
	[MODE_SAFE] = {.entries = safe_entries, .len = ARRAY_SIZE(safe_entries)},
	[MODE_NOMINAL] = {.entries = nominal_entries, .len = ARRAY_SIZE(nominal_entries)},
};
