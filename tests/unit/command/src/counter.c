/*
 * Tests for replay protection: the counter check and the floor (DS-50,
 * DS-53).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>

#include <zephyr/ztest.h>

#include "silversat/cmd_counter.h"

#define EPOCH    CMD_COUNTER_EPOCH_MS
#define MAX_JUMP CMD_COUNTER_MAX_JUMP_MS

/* 2026-10-01T00:00:00Z in epoch milliseconds: a typical real counter. */
#define OCT_2026 1790812800000ULL

static uint64_t floor_of(uint8_t slot)
{
	uint64_t floor;

	zassert_ok(floor_store_get(slot, &floor));
	return floor;
}

/* Each test starts with both floors at their default. */
static void reset_floors(void *fixture)
{
	ARG_UNUSED(fixture);
	for (uint8_t slot = 0; slot < CMD_COUNTER_SLOTS; slot++) {
		zassert_ok(floor_store_set(slot, EPOCH));
	}
}

ZTEST_SUITE(cmd_counter, NULL, NULL, reset_floors, NULL, NULL);

ZTEST(cmd_counter, test_above_the_floor)
{
	zassert_equal(cmd_counter_check(101, 100), CMD_COUNTER_OK);
	zassert_equal(cmd_counter_check(100, 100), CMD_COUNTER_REPLAY, "equal is a replay");
	zassert_equal(cmd_counter_check(99, 100), CMD_COUNTER_REPLAY);
	zassert_equal(cmd_counter_check(0, 100), CMD_COUNTER_REPLAY);
}

ZTEST(cmd_counter, test_jump_limit)
{
	zassert_equal(cmd_counter_check(100 + MAX_JUMP, 100), CMD_COUNTER_OK, "at the limit");
	zassert_equal(cmd_counter_check(100 + MAX_JUMP + 1, 100), CMD_COUNTER_JUMP_TOO_LARGE);

	/* A counter sent in microseconds instead of milliseconds. */
	zassert_equal(cmd_counter_check(OCT_2026 * 1000, OCT_2026 - 1),
		      CMD_COUNTER_JUMP_TOO_LARGE);
}

ZTEST(cmd_counter, test_counter_at_max)
{
	/*
	 * DS-53: the counter never wraps. With the floor within reach of the
	 * maximum, a command carrying the maximum is accepted once; after
	 * that, nothing is above the floor, including 0, which a wrap would
	 * have allowed. Recovery is a key rotation (DS-54).
	 */
	zassert_ok(floor_store_set(0, UINT64_MAX - 1));
	zassert_equal(cmd_counter_accept(0, UINT64_MAX), CMD_COUNTER_OK);
	zassert_equal(floor_of(0), UINT64_MAX);

	zassert_equal(cmd_counter_accept(0, UINT64_MAX), CMD_COUNTER_REPLAY);
	zassert_equal(cmd_counter_accept(0, 0), CMD_COUNTER_REPLAY);
	zassert_equal(cmd_counter_accept(0, OCT_2026), CMD_COUNTER_REPLAY);
	zassert_equal(floor_of(0), UINT64_MAX);

	/* The jump limit right at the top of the range. */
	zassert_equal(cmd_counter_check(UINT64_MAX, UINT64_MAX - MAX_JUMP), CMD_COUNTER_OK);
	zassert_equal(cmd_counter_check(UINT64_MAX, UINT64_MAX - MAX_JUMP - 1),
		      CMD_COUNTER_JUMP_TOO_LARGE);
}

ZTEST(cmd_counter, test_floor_starts_at_the_mission_epoch)
{
	/* The first real command after boot is accepted, not taken for a huge jump. */
	zassert_equal(floor_of(0), EPOCH);
	zassert_equal(floor_of(1), EPOCH);
	zassert_equal(cmd_counter_accept(0, OCT_2026), CMD_COUNTER_OK);
	zassert_equal(cmd_counter_accept(0, OCT_2026), CMD_COUNTER_REPLAY);
	zassert_equal(cmd_counter_check(EPOCH, EPOCH), CMD_COUNTER_REPLAY);
}

ZTEST(cmd_counter, test_years_of_silence)
{
	/* Five years with no command accepted; the ground can still get in. */
	const uint64_t five_years = 5ULL * 365 * 24 * 60 * 60 * 1000;

	zassert_equal(cmd_counter_accept(0, OCT_2026), CMD_COUNTER_OK);
	zassert_equal(cmd_counter_accept(0, OCT_2026 + five_years), CMD_COUNTER_OK);
}

ZTEST(cmd_counter, test_gaps_are_allowed)
{
	/* After a lost command, the ground just sends a larger counter (DS-53). */
	zassert_equal(cmd_counter_accept(0, OCT_2026), CMD_COUNTER_OK);
	zassert_equal(cmd_counter_accept(0, OCT_2026 + 60000), CMD_COUNTER_OK);
	zassert_equal(floor_of(0), OCT_2026 + 60000);
	zassert_equal(cmd_counter_accept(0, OCT_2026 + 1), CMD_COUNTER_REPLAY,
		      "the skipped counter is now below the floor");
}

ZTEST(cmd_counter, test_rejection_leaves_the_floor_alone)
{
	zassert_equal(cmd_counter_accept(0, OCT_2026), CMD_COUNTER_OK);
	zassert_equal(cmd_counter_accept(0, OCT_2026 - 1), CMD_COUNTER_REPLAY);
	zassert_equal(cmd_counter_accept(0, OCT_2026 + MAX_JUMP + 1), CMD_COUNTER_JUMP_TOO_LARGE);
	zassert_equal(floor_of(0), OCT_2026);
}

ZTEST(cmd_counter, test_slots_are_independent)
{
	/* Each key slot has its own floor (DS-53, DS-54). */
	zassert_equal(cmd_counter_accept(0, OCT_2026 + 1000), CMD_COUNTER_OK);
	zassert_equal(floor_of(1), EPOCH);
	zassert_equal(cmd_counter_accept(1, OCT_2026), CMD_COUNTER_OK,
		      "slot 1 accepts a counter below slot 0's floor");
}

ZTEST(cmd_counter, test_bad_slot)
{
	uint64_t floor;

	zassert_equal(cmd_counter_accept(CMD_COUNTER_SLOTS, OCT_2026), CMD_COUNTER_BAD_SLOT);
	zassert_equal(floor_store_get(CMD_COUNTER_SLOTS, &floor), -EINVAL);
	zassert_equal(floor_store_set(CMD_COUNTER_SLOTS, OCT_2026), -EINVAL);
}
