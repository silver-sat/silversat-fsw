/*
 * Test-only apps for the frame manager tests, written the way every flight
 * app is written (DS-10, DS-14; design rationale section 9).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/zbus/zbus.h>

#include "msg/common.h"
#include "msg/counter_app.h"
#include "msg/stuck_app.h"
#include "silversat/resource_map.h"
#include "test_apps.h"

/* Test-only. A flight app takes its stack size from the resource map. */
#define TEST_APP_STACK_SIZE 1024

struct seen_ticks counter_app_seen;
struct seen_ticks stuck_app_seen;

static atomic_t stuck_app_held;

void seen_clear(struct seen_ticks *seen)
{
	seen->len = 0;
}

void stuck_app_hold(bool hold)
{
	atomic_set(&stuck_app_held, hold ? 1 : 0);
}

static void record(struct seen_ticks *seen, const struct frame_tick *tick)
{
	if (seen->len < SEEN_MAX) {
		seen->ticks[seen->len++] = *tick;
	}
}

/* ---- counter_app ---- */

ZBUS_MSG_SUBSCRIBER_DEFINE(counter_app_sub);
ZBUS_CHAN_ADD_OBS(counter_app_wakeup_chan, counter_app_sub, 3);
ZBUS_CHAN_ADD_OBS(counter_app_cmd_chan, counter_app_sub, 3);

static void counter_app_main(void *a, void *b, void *c)
{
	static struct app_status status;
	static struct counter_app_hk hk;
	const struct zbus_channel *chan;
	union counter_app_msg msg;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	while (zbus_sub_wait_msg(&counter_app_sub, &chan, &msg, K_FOREVER) == 0) {
		if (chan == &counter_app_wakeup_chan) {
			record(&counter_app_seen, &msg.tick);
			hk.ticks++;
			status.steps++;
		} else if (chan == &counter_app_cmd_chan) {
			status.cmd_rejected++; /* it has no commands */
		}
		zbus_chan_pub(&counter_app_hk_chan, &hk, K_NO_WAIT);
		zbus_chan_pub(&counter_app_status_chan, &status, K_NO_WAIT);
	}
}

K_THREAD_DEFINE(counter_app_tid, TEST_APP_STACK_SIZE, counter_app_main, NULL, NULL, NULL,
		PRIORITY_1HZ, 0, 0);

/* ---- stuck_app ---- */

ZBUS_MSG_SUBSCRIBER_DEFINE(stuck_app_sub);
ZBUS_CHAN_ADD_OBS(stuck_app_wakeup_chan, stuck_app_sub, 3);
ZBUS_CHAN_ADD_OBS(stuck_app_cmd_chan, stuck_app_sub, 3);

static void stuck_app_main(void *a, void *b, void *c)
{
	static struct app_status status;
	static struct stuck_app_hk hk;
	const struct zbus_channel *chan;
	union stuck_app_msg msg;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	while (zbus_sub_wait_msg(&stuck_app_sub, &chan, &msg, K_FOREVER) == 0) {
		if (chan == &stuck_app_wakeup_chan) {
			/* Stuck part-way through a step: no progress, no status. */
			while (atomic_get(&stuck_app_held)) {
				k_sleep(K_MSEC(10));
			}
			record(&stuck_app_seen, &msg.tick);
			hk.ticks++;
			status.steps++;
		} else if (chan == &stuck_app_cmd_chan) {
			status.cmd_rejected++; /* it has no commands */
		}
		zbus_chan_pub(&stuck_app_hk_chan, &hk, K_NO_WAIT);
		zbus_chan_pub(&stuck_app_status_chan, &status, K_NO_WAIT);
	}
}

K_THREAD_DEFINE(stuck_app_tid, TEST_APP_STACK_SIZE, stuck_app_main, NULL, NULL, NULL,
		PRIORITY_1HZ, 0, 0);
