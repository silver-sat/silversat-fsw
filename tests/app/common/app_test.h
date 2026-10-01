/*
 * Helpers for app tests (tests/app/).
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * An app test drives one app the way the rest of the system would: it
 * publishes ticks to the app's wakeup channel (standing in for the frame
 * manager) and commands to its command channel, then waits for the app's
 * status channel to show the work was done. The waits run in native_sim's
 * simulated time, so a test can cover hours or days in a fraction of a
 * second.
 */

#ifndef SILVERSAT_TESTS_APP_TEST_H_
#define SILVERSAT_TESTS_APP_TEST_H_

#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>

#include "msg/common.h"

/*
 * The tick the frame manager would publish for minor frame count. MET is
 * set equal to uptime until there is a MET source to read.
 */
struct frame_tick app_test_tick(uint32_t count);

/* Publish app_test_tick(count) on an app's wakeup channel. */
int app_test_wake(const struct zbus_channel *wakeup_chan, uint32_t count);

/*
 * Wait until the app's status shows at least steps wakeups handled and at
 * least commands commands handled (accepted plus rejected). Copies the last
 * status read into *out, if out is not NULL. Returns 0, or -EAGAIN if the
 * timeout passes first.
 */
int app_test_wait_status(const struct zbus_channel *status_chan, uint32_t steps,
			 uint32_t commands, k_timeout_t timeout, struct app_status *out);

#endif /* SILVERSAT_TESTS_APP_TEST_H_ */
