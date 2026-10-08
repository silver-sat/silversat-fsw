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
 *
 * In a test with events (CONFIG_SS_EVENT), it also reads the events the app
 * raised, straight from the event queue, as telemetry output would (DS-10).
 */

#ifndef SILVERSAT_TESTS_APP_TEST_H_
#define SILVERSAT_TESTS_APP_TEST_H_

#include <stdbool.h>
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

#if defined(CONFIG_SS_EVENT)
/* Throw away every queued event, so a test sees only the events it causes. */
void app_test_drain_events(void);

/*
 * Take queued events, oldest first, until one from app with this id (enum
 * <app>_event_id), and copy it into *out. Events before it are thrown
 * away. Returns false, with the queue empty, if there is none.
 */
bool app_test_find_event(uint8_t app, uint16_t id, struct event *out);
#endif

#endif /* SILVERSAT_TESTS_APP_TEST_H_ */
