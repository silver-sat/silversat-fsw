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
 * The radio and command ingest run in every slot of every mode, the radio
 * first: within a minor frame, the radio hands command ingest the frames it
 * received, and command ingest's replies go out in the next minor frame. A
 * ground command waits at most one minor frame. Command ingest is protected
 * (DS-43): its entries can't be disabled.
 */
#define RADIO_IN_SLOT(n)                                                                       \
	{.slot = (n), .app = APP_ID_RADIO, .wakeup_chan = &radio_wakeup_chan,                  \
	 .status_chan = &radio_status_chan}
#define COMMAND_INGEST_IN_SLOT(n)                                                              \
	{.slot = (n), .app = APP_ID_COMMAND_INGEST,                                            \
	 .wakeup_chan = &command_ingest_wakeup_chan, .status_chan = &command_ingest_status_chan}
#define LINK_IN_SLOT(n) RADIO_IN_SLOT(n), COMMAND_INGEST_IN_SLOT(n)

/*
 * Telemetry output runs once a major frame, in slot 5. Apps publish their
 * housekeeping early in the major frame (DS-14), and the radio sends the
 * packet in slot 6 (DS-21's "collected in slot 5, shipped in slot 6").
 */
#define TELEMETRY_OUTPUT_IN_SLOT(n)                                                            \
	{.slot = (n), .app = APP_ID_TELEMETRY_OUTPUT,                                          \
	 .wakeup_chan = &telemetry_output_wakeup_chan,                                         \
	 .status_chan = &telemetry_output_status_chan}

/*
 * The mode manager runs once a major frame, in the last slot, so a mode
 * change it makes takes effect at the next major frame (DS-23).
 */
#define MODE_MANAGER_IN_SLOT(n)                                                                \
	{.slot = (n), .app = APP_ID_MODE_MANAGER, .wakeup_chan = &mode_manager_wakeup_chan,    \
	 .status_chan = &mode_manager_status_chan}

/*
 * Health runs once a major frame, in slot 0, in every mode: it reads the
 * frame manager's report, published at the start of slot 0, and feeds the
 * watchdog (DS-43). Without it in a table, the watchdog would reset the
 * spacecraft.
 */
#define HEALTH_IN_SLOT(n)                                                                      \
	{.slot = (n), .app = APP_ID_HEALTH, .wakeup_chan = &health_wakeup_chan,                \
	 .status_chan = &health_status_chan}

static const struct frame_entry safe_entries[] = {
	LINK_IN_SLOT(0), LINK_IN_SLOT(1), LINK_IN_SLOT(2), LINK_IN_SLOT(3), LINK_IN_SLOT(4),
	LINK_IN_SLOT(5), LINK_IN_SLOT(6), LINK_IN_SLOT(7), LINK_IN_SLOT(8), LINK_IN_SLOT(9),
	TELEMETRY_OUTPUT_IN_SLOT(5), MODE_MANAGER_IN_SLOT(9), HEALTH_IN_SLOT(0),
};

/*
 * Deploy mode (DS-42): nothing that transmits runs, and nothing takes
 * ground commands, so neither the radio, command ingest, nor telemetry
 * output is here. The mode manager runs to end the separation delay, and
 * health to feed the watchdog.
 */
static const struct frame_entry deploy_entries[] = {
	MODE_MANAGER_IN_SLOT(9), HEALTH_IN_SLOT(0),
};

/*
 * Test mode (DS-42): everything nominal runs, for ground testing. The
 * antenna app, when it exists, stays out, so test mode never deploys it.
 */
static const struct frame_entry test_entries[] = {
	LINK_IN_SLOT(0), LINK_IN_SLOT(1), LINK_IN_SLOT(2), LINK_IN_SLOT(3), LINK_IN_SLOT(4),
	LINK_IN_SLOT(5), LINK_IN_SLOT(6), LINK_IN_SLOT(7), LINK_IN_SLOT(8), LINK_IN_SLOT(9),
	TELEMETRY_OUTPUT_IN_SLOT(5), MODE_MANAGER_IN_SLOT(9), HEALTH_IN_SLOT(0),
};

static const struct frame_entry nominal_entries[] = {
	LINK_IN_SLOT(0), LINK_IN_SLOT(1), LINK_IN_SLOT(2), LINK_IN_SLOT(3), LINK_IN_SLOT(4),
	LINK_IN_SLOT(5), LINK_IN_SLOT(6), LINK_IN_SLOT(7), LINK_IN_SLOT(8), LINK_IN_SLOT(9),
	TELEMETRY_OUTPUT_IN_SLOT(5), MODE_MANAGER_IN_SLOT(9), HEALTH_IN_SLOT(0),
};

BUILD_ASSERT(ARRAY_SIZE(safe_entries) <= FRAME_ENTRIES_MAX);
BUILD_ASSERT(ARRAY_SIZE(nominal_entries) <= FRAME_ENTRIES_MAX);
BUILD_ASSERT(ARRAY_SIZE(deploy_entries) <= FRAME_ENTRIES_MAX);
BUILD_ASSERT(ARRAY_SIZE(test_entries) <= FRAME_ENTRIES_MAX);
BUILD_ASSERT(FRAME_SLOTS == 10, "the radio and command ingest have one entry per slot");

const struct frame_table frame_tables[MODE_MAX + 1] = {
	[MODE_SAFE] = {.entries = safe_entries, .len = ARRAY_SIZE(safe_entries)},
	[MODE_NOMINAL] = {.entries = nominal_entries, .len = ARRAY_SIZE(nominal_entries)},
	[MODE_DEPLOY] = {.entries = deploy_entries, .len = ARRAY_SIZE(deploy_entries)},
	[MODE_TEST] = {.entries = test_entries, .len = ARRAY_SIZE(test_entries)},
};
