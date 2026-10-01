/*
 * Command counter: replay protection (DS-50, DS-53). See
 * include/silversat/cmd_counter.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "silversat/cmd_counter.h"

enum cmd_counter_result cmd_counter_check(uint64_t counter, uint64_t floor)
{
	/* Plain unsigned comparison: no wrap, no serial arithmetic, no floor + 1. */
	if (counter <= floor) {
		return CMD_COUNTER_REPLAY;
	}
	/* counter > floor here, so the subtraction cannot wrap. */
	if (counter - floor > CMD_COUNTER_MAX_JUMP_MS) {
		return CMD_COUNTER_JUMP_TOO_LARGE;
	}
	return CMD_COUNTER_OK;
}

enum cmd_counter_result cmd_counter_accept(uint8_t slot, uint64_t counter)
{
	uint64_t floor;
	enum cmd_counter_result result;

	if (floor_store_get(slot, &floor) != 0) {
		return CMD_COUNTER_BAD_SLOT;
	}
	result = cmd_counter_check(counter, floor);
	if (result != CMD_COUNTER_OK) {
		return result;
	}
	if (floor_store_set(slot, counter) != 0) {
		return CMD_COUNTER_STORE_FAILED;
	}
	return CMD_COUNTER_OK;
}
