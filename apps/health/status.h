/*
 * Health's development status line (DS-06, DS-44). See status.c.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SILVERSAT_HEALTH_STATUS_H_
#define SILVERSAT_HEALTH_STATUS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "msg/common.h"
#include "msg/health.h"

/*
 * Is a status line due at this major frame (1, 2, 3, ...)? The first comes
 * at major frame 2, after the mode manager has read the test signal in
 * major frame 1; then one every period major frames.
 */
bool status_due(uint32_t major_frame, uint32_t period);

/*
 * Write the status line into buf (at most size bytes, always terminated):
 *
 *   boot 3, MET 125 s, mode test (test_signal), least stack 412 B (radio),
 *   stalls 0, short runs 0, stopped 0x0
 *
 * Returns the length it would have, as snprintf does.
 */
int status_format(char *buf, size_t size, const struct health_hk *hk,
		  const struct mode_state *mode, int64_t met_ms);

#endif /* SILVERSAT_HEALTH_STATUS_H_ */
