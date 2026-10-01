/*
 * Test-only apps for the frame manager tests.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef FRAME_MANAGER_TEST_APPS_H_
#define FRAME_MANAGER_TEST_APPS_H_

#include <stdbool.h>
#include <stdint.h>

#include "msg/common.h"

#define SEEN_MAX 64

/*
 * Ticks each app has handled since it was last cleared, oldest first. When
 * full, later ticks are not recorded. The test thread is cooperative, so the
 * apps cannot change these while a test reads them.
 */
struct seen_ticks {
	struct frame_tick ticks[SEEN_MAX];
	uint32_t len;
};

extern struct seen_ticks counter_app_seen;
extern struct seen_ticks stuck_app_seen;

void seen_clear(struct seen_ticks *seen);

/*
 * While held, stuck_app stops inside its step: it has taken a tick but not
 * finished it, so its step counter does not advance.
 */
void stuck_app_hold(bool hold);

#endif /* FRAME_MANAGER_TEST_APPS_H_ */
