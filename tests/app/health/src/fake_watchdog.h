/*
 * A test-only watchdog: counts feeds, never resets. See fake_watchdog.c.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SILVERSAT_TESTS_FAKE_WATCHDOG_H_
#define SILVERSAT_TESTS_FAKE_WATCHDOG_H_

#include <stdint.h>

#include <zephyr/device.h>

struct fake_watchdog_state {
	uint32_t timeout_ms; /* from wdt_install_timeout() */
	uint32_t flags;      /* from wdt_install_timeout() */
	uint8_t options;     /* from wdt_setup() */
	bool started;        /* wdt_setup() was called */
	uint32_t feeds;
};

/* What the fake has been asked to do so far. */
struct fake_watchdog_state fake_watchdog_state(const struct device *dev);

#endif /* SILVERSAT_TESTS_FAKE_WATCHDOG_H_ */
