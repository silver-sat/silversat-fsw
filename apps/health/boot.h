/*
 * Health's boot work and run checkpoint (DS-25, DS-44, DS-72). See boot.c.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SILVERSAT_HEALTH_BOOT_H_
#define SILVERSAT_HEALTH_BOOT_H_

#include <stdint.h>

/* This boot's number, and why the last run ended (hwinfo RESET_* flags). */
uint32_t boot_number(void);
uint32_t boot_reset_cause(void);

/*
 * Short runs in a row up to this boot (a reset loop, DS-44), at most
 * CONFIG_SS_RESET_LOOP_STOP_RUNS. 0 if the last run was long.
 */
uint8_t boot_short_runs(void);

/* Store where this run has got to: once a major frame. */
void boot_checkpoint(int64_t met_ms, int64_t uptime_ms);

/*
 * Note the protected app whose stall is about to reset the spacecraft, so
 * the next boot's log names it (DS-44). The checkpoint that follows in the
 * same wakeup stores it, seconds before the watchdog can fire.
 */
void boot_record_reset_app(uint8_t app);

/* Checkpoints or log entries that reached neither FRAM nor its mirror. */
uint32_t boot_store_failures(void);

#endif /* SILVERSAT_HEALTH_BOOT_H_ */
