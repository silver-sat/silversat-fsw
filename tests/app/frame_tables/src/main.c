/*
 * Checks on the flight frame tables (DS-21, DS-23, DS-42).
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The tables are data, so an edit can break a safety rule without changing
 * any code. These tests read apps/frame_manager/frame_tables.c as built for
 * flight.
 */

#include <zephyr/ztest.h>

#include "msg/common.h"
#include "silversat/frame_table.h"
#include "silversat/resource_map.h"

static bool table_has(uint8_t mode, uint8_t app)
{
	const struct frame_table *table = &frame_tables[mode];

	for (uint8_t i = 0; i < table->len; i++) {
		if (table->entries[i].app == app) {
			return true;
		}
	}
	return false;
}

ZTEST(frame_tables, test_every_table_is_well_formed)
{
	for (uint8_t mode = 0; mode <= MODE_MAX; mode++) {
		zassert_ok(frame_table_check(&frame_tables[mode]), "mode %u", mode);
		zassert_true(frame_tables[mode].len > 0, "mode %u has no table", mode);
	}
}

ZTEST(frame_tables, test_deploy_has_nothing_that_transmits_or_takes_commands)
{
	/* Nothing may transmit before the separation delay is over (DS-42). */
	zassert_false(table_has(MODE_DEPLOY, APP_ID_RADIO));
	zassert_false(table_has(MODE_DEPLOY, APP_ID_TELEMETRY_OUTPUT));
	zassert_false(table_has(MODE_DEPLOY, APP_ID_COMMAND_INGEST));
	/* But something must end the delay. */
	zassert_true(table_has(MODE_DEPLOY, APP_ID_MODE_MANAGER));
}

ZTEST(frame_tables, test_the_mode_manager_and_health_run_in_every_mode)
{
	/* Without health, nothing feeds the watchdog, and it resets (DS-43). */
	for (uint8_t mode = 0; mode <= MODE_MAX; mode++) {
		zassert_true(table_has(mode, APP_ID_MODE_MANAGER), "mode %u", mode);
		zassert_true(table_has(mode, APP_ID_HEALTH), "mode %u", mode);
	}
}

ZTEST(frame_tables, test_health_runs_in_slot_0)
{
	/* The frame manager publishes its report at the start of slot 0. */
	for (uint8_t mode = 0; mode <= MODE_MAX; mode++) {
		const struct frame_table *table = &frame_tables[mode];

		for (uint8_t i = 0; i < table->len; i++) {
			if (table->entries[i].app == APP_ID_HEALTH) {
				zassert_equal(table->entries[i].slot, 0, "mode %u", mode);
			}
		}
	}
}

ZTEST(frame_tables, test_the_ground_can_command_every_other_mode)
{
	/* Safe, nominal, and test accept ground commands; so they need the link. */
	const uint8_t modes[] = {MODE_SAFE, MODE_NOMINAL, MODE_TEST};

	for (size_t i = 0; i < ARRAY_SIZE(modes); i++) {
		zassert_true(table_has(modes[i], APP_ID_RADIO), "mode %u", modes[i]);
		zassert_true(table_has(modes[i], APP_ID_COMMAND_INGEST), "mode %u", modes[i]);
	}
}

ZTEST_SUITE(frame_tables, NULL, NULL, NULL, NULL, NULL);
