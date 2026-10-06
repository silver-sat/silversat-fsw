/*
 * Command ingest's state that survives a reset (DS-46, DS-53, DS-54, DS-74).
 * See persist.c.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SILVERSAT_COMMAND_INGEST_PERSIST_H_
#define SILVERSAT_COMMAND_INGEST_PERSIST_H_

#include <stdbool.h>
#include <stdint.h>

/* The key slot in use (DS-54), and setting it after a rotation. */
uint8_t persist_active_slot(void);
void persist_set_active_slot(uint8_t slot);

/* Whether the ground has ever been heard from, and recording that it has (DS-46). */
bool persist_contacted(void);
void persist_set_contact(int64_t met_ms);

/* Writes that reached neither FRAM nor its mirror. */
uint32_t persist_failures(void);

#endif /* SILVERSAT_COMMAND_INGEST_PERSIST_H_ */
