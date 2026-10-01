/*
 * Helpers for app tests. See app_test.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/sys_clock.h>
#include <zephyr/zbus/zbus.h>

#include "app_test.h"
#include "silversat/resource_map.h"

struct frame_tick app_test_tick(uint32_t count)
{
	struct frame_tick tick = {
		.count = count,
		.slot = count % FRAME_SLOTS,
		.uptime_ms = k_uptime_get(),
	};

	tick.met_ms = tick.uptime_ms;
	return tick;
}

int app_test_wake(const struct zbus_channel *wakeup_chan, uint32_t count)
{
	struct frame_tick tick = app_test_tick(count);

	return zbus_chan_pub(wakeup_chan, &tick, K_NO_WAIT);
}

int app_test_wait_status(const struct zbus_channel *status_chan, uint32_t steps,
			 uint32_t commands, k_timeout_t timeout, struct app_status *out)
{
	k_timepoint_t end = sys_timepoint_calc(timeout);
	struct app_status status = {0};
	int rc = -EAGAIN;

	for (;;) {
		(void)zbus_chan_read(status_chan, &status, K_MSEC(10));
		if (status.steps >= steps &&
		    status.cmd_accepted + status.cmd_rejected >= commands) {
			rc = 0;
			break;
		}
		if (sys_timepoint_expired(end)) {
			break;
		}
		/* Sleeping lets the app run; in simulated time it costs nothing. */
		k_sleep(K_MSEC(1));
	}
	if (out != NULL) {
		*out = status;
	}
	return rc;
}
