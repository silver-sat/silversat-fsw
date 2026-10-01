/*
 * Flight frame tables (DS-21, DS-23): which app the frame manager wakes in
 * which slot, one table per mode.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Both tables are empty until the first frame-driven app is written. To
 * add an app, add an entry for each slot it runs in:
 *
 *   static const struct frame_entry nominal_entries[] = {
 *           {.slot = 0, .app = APP_ID_FOO,
 *            .wakeup_chan = &foo_wakeup_chan, .status_chan = &foo_status_chan},
 *   };
 *
 * Phase the work on purpose: an app that uses another app's data goes in a
 * later slot than the app that produces it (DS-21). The safe table holds
 * only trusted apps (DS-41).
 */

#include "msg/common.h"
#include "silversat/frame_table.h"

const struct frame_table frame_tables[MODE_MAX + 1] = {
	[MODE_SAFE] = {.entries = NULL, .len = 0},
	[MODE_NOMINAL] = {.entries = NULL, .len = 0},
};
