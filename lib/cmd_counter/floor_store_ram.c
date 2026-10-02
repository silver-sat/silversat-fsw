/*
 * Floor store held in RAM, until the FRAM service exists (DS-53, DS-70).
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The floors go back to the mission epoch at every boot, so a command
 * recorded during an earlier boot could be replayed. That is the degraded
 * behavior DS-75 already accepts when FRAM has failed. Only command ingest
 * calls these, from its one thread, so no lock is needed.
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
