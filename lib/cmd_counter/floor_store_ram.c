/*
 * Floor store held in RAM, for builds without command ingest (DS-53).
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Command ingest keeps the floors in FRAM (apps/command_ingest/persist.c).
 * This store is for the library tests: the floors go back to the mission
 * epoch at every boot. One thread calls these, so no lock is needed.
 */

#include <errno.h>

#include "silversat/cmd_counter.h"

static uint64_t floors[CMD_COUNTER_SLOTS] = {
	CMD_COUNTER_EPOCH_MS,
	CMD_COUNTER_EPOCH_MS,
};

int floor_store_get(uint8_t slot, uint64_t *floor)
{
	if (slot >= CMD_COUNTER_SLOTS) {
		return -EINVAL;
	}
	*floor = floors[slot];
	return 0;
}

int floor_store_set(uint8_t slot, uint64_t floor)
{
	if (slot >= CMD_COUNTER_SLOTS) {
		return -EINVAL;
	}
	floors[slot] = floor;
	return 0;
}
