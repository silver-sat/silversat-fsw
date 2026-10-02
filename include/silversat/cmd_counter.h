/*
 * Command counter: replay protection, stage 3 of command ingest (DS-50,
 * DS-53).
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Every signed command carries a 64-bit counter, epoch milliseconds by
 * default. A command is accepted only if its counter is strictly greater
 * than the floor: the counter of the last command accepted under the same
 * key slot. Gaps are allowed, so the ground can always jump ahead.
 *
 * Call this only after the signature has been checked, so a forged packet
 * can never raise the floor (DS-50).
 *
 * The counter never wraps. Comparisons are plain unsigned 64-bit, and
 * nothing here computes floor + 1, which could overflow (DS-53).
 */

#ifndef SILVERSAT_CMD_COUNTER_H_
#define SILVERSAT_CMD_COUNTER_H_

#include <stdint.h>

/*
 * The mission epoch, 2026-01-01T00:00:00Z in epoch milliseconds. The floor
 * starts here, not at 0, when nothing is stored, so the first real command
 * is not rejected as too large a jump (DS-53). No floor is ever reset,
 * including by key rotation.
 */
#define CMD_COUNTER_EPOCH_MS 1767225600000ULL

/*
 * The largest jump above the floor that is accepted: about 10 years. A
 * counter sent in the wrong units (microseconds, nanoseconds) is thousands
 * of times larger and is rejected, while the ground can still jump ahead
 * after a long silence (DS-53).
 */
#define CMD_COUNTER_MAX_JUMP_MS (10ULL * 365 * 24 * 60 * 60 * 1000)

/* Key slots, each with its own floor (DS-53, DS-54). */
#define CMD_COUNTER_SLOTS 2

enum cmd_counter_result {
	/* Accepted; the floor is now the counter. */
	CMD_COUNTER_OK,
	/* Not above the floor: a replay, or a command sent out of order. */
	CMD_COUNTER_REPLAY,
	/* Too far above the floor to be a real counter. */
	CMD_COUNTER_JUMP_TOO_LARGE,
	/* The new floor could not be stored, so the command must not run. */
	CMD_COUNTER_STORE_FAILED,
	/* No such key slot. */
	CMD_COUNTER_BAD_SLOT,
};

/* The comparison alone, with no stored state. */
enum cmd_counter_result cmd_counter_check(uint64_t counter, uint64_t floor);

/*
 * Check counter against the slot's floor and, if it is accepted, store it
 * as the new floor before returning. Command ingest calls this before it
 * sends the ACK, so a reset between the two cannot reopen the replay
 * window (DS-50). The floor changes only when the result is CMD_COUNTER_OK.
 */
enum cmd_counter_result cmd_counter_accept(uint8_t slot, uint64_t counter);

/*
 * Floor storage. Until the FRAM service exists (DS-70), the floors are
 * held in RAM: they return to CMD_COUNTER_EPOCH_MS at every boot, which is
 * the degraded behavior DS-75 defines for a failed FRAM. Only command
 * ingest uses these (DS-74).
 */
int floor_store_get(uint8_t slot, uint64_t *floor);
int floor_store_set(uint8_t slot, uint64_t floor);

#endif /* SILVERSAT_CMD_COUNTER_H_ */
