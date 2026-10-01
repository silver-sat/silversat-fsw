/*
 * Frame tables for the frame manager tests, in place of the flight tables
 * (CONFIG_SS_FRAME_MANAGER_FLIGHT_TABLES=n).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/sys/util.h>

#include "msg/common.h"
#include "silversat/frame_table.h"
#include "silversat/resource_map.h"
#include "test_tables.h"

static const struct frame_entry safe_entries[] = {
	[SAFE_COUNTER_SLOT2] = {.slot = 2, .app = APP_ID_COUNTER_APP,
				.wakeup_chan = &counter_app_wakeup_chan,
				.status_chan = &counter_app_status_chan},
	/* A protected app (DS-43), for the reject-disable test. */
	[SAFE_MODE_MANAGER_SLOT9] = {.slot = 9, .app = APP_ID_MODE_MANAGER,
				     .wakeup_chan = &mode_manager_wakeup_chan,
				     .status_chan = &mode_manager_status_chan},
};

static const struct frame_entry nominal_entries[] = {
	[NOMINAL_COUNTER_SLOT0] = {.slot = 0, .app = APP_ID_COUNTER_APP,
				   .wakeup_chan = &counter_app_wakeup_chan,
				   .status_chan = &counter_app_status_chan},
	[NOMINAL_STUCK_SLOT1] = {.slot = 1, .app = APP_ID_STUCK_APP,
				 .wakeup_chan = &stuck_app_wakeup_chan,
				 .status_chan = &stuck_app_status_chan},
	[NOMINAL_COUNTER_SLOT5] = {.slot = 5, .app = APP_ID_COUNTER_APP,
				   .wakeup_chan = &counter_app_wakeup_chan,
				   .status_chan = &counter_app_status_chan},
};

BUILD_ASSERT(ARRAY_SIZE(safe_entries) == SAFE_ENTRIES);
BUILD_ASSERT(ARRAY_SIZE(nominal_entries) == NOMINAL_ENTRIES);

const struct frame_table frame_tables[MODE_MAX + 1] = {
	[MODE_SAFE] = {.entries = safe_entries, .len = ARRAY_SIZE(safe_entries)},
	[MODE_NOMINAL] = {.entries = nominal_entries, .len = ARRAY_SIZE(nominal_entries)},
};
