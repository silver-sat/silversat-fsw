/*
 * Command ingest's state that survives a reset (DS-46, DS-53, DS-54, DS-74).
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * One FRAM record, command_state (messages/nvm_map.yaml), holds both
 * counter floors, the active key slot, and the last ground contact. It is
 * also kept in the mirror, the backup SRAM (DS-75). This file keeps a copy
 * in RAM, read from FRAM the first time it's needed, and writes the record
 * whenever a value changes.
 *
 * It is also the counter floors' store: cmd_counter_accept() calls
 * floor_store_set() to store a new floor before command ingest sends the
 * ACK (DS-50). If the write reaches neither FRAM nor the mirror, the floor
 * is still kept in RAM, so replays stay refused for the rest of this boot,
 * and the failure is counted: DS-75's degraded behavior. Refusing every
 * command instead would leave the spacecraft unable to be commanded.
 *
 * Only command ingest's thread calls these, except a test standing in for
 * the ground between frames.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include "nvm/command_ingest.h"
#include "persist.h"
#include "silversat/cmd_counter.h"

static struct nvm_command_state state;
static bool loaded;
static uint32_t failures;

static void load(void)
{
	if (loaded) {
		return;
	}
	/* Whatever the result, state holds a record: FRAM's, or the default. */
	(void)nvm_command_state_read(&state);
	if (state.active_slot >= CMD_COUNTER_SLOTS) {
		state.active_slot = 0;
	}
	loaded = true;
}

static void save(void)
{
	if (nvm_command_state_write(&state) != 0) {
		failures++;
	}
}

int floor_store_get(uint8_t slot, uint64_t *floor)
{
	if (slot >= CMD_COUNTER_SLOTS) {
		return -EINVAL;
	}
	load();
	*floor = slot == 0 ? state.floor_slot0 : state.floor_slot1;
	return 0;
}

int floor_store_set(uint8_t slot, uint64_t floor)
{
	if (slot >= CMD_COUNTER_SLOTS) {
		return -EINVAL;
	}
	load();
	if (slot == 0) {
		state.floor_slot0 = floor;
	} else {
		state.floor_slot1 = floor;
	}
	save();
	return 0; /* kept in RAM even if not stored: see the top of this file */
}

uint8_t persist_active_slot(void)
{
	load();
	return state.active_slot;
}

void persist_set_active_slot(uint8_t slot)
{
	load();
	state.active_slot = slot;
	save();
}

bool persist_contacted(void)
{
	load();
	return state.contacted;
}

void persist_set_contact(int64_t met_ms)
{
	load();
	state.contacted = true;
	state.last_accepted_met_ms = met_ms;
	save();
}

uint32_t persist_failures(void)
{
	return failures;
}
