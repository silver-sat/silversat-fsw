/*
 * Tests that zbus priority boost is off (DS-24).
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * With CONFIG_ZBUS_PRIORITY_BOOST on (Zephyr's default), every publish or
 * notify ends by setting the caller's priority again, even when nothing
 * was boosted, and Zephyr moves a thread whose priority is set behind any
 * other ready thread of the same priority. So an app that published in the
 * middle of its work handed the CPU to an equal-priority app there and
 * then. messages/Kconfig turns it off; this test shows a notify no longer
 * yields.
 *
 * mode_chan has no observers in this build (the apps that observe it
 * aren't compiled in), so a notify queues nothing and changes nothing.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/ztest.h>

#include "msg/common.h"

BUILD_ASSERT(!IS_ENABLED(CONFIG_ZBUS_PRIORITY_BOOST),
	     "messages/Kconfig turns zbus priority boost off (DS-24)");

/*
 * Both test threads share one preemptible priority, below the ztest
 * thread, so neither runs until the test waits for them, and then they run
 * in the order they were started.
 */
#define SCHED_TEST_PRIORITY 5
#define SCHED_TEST_STACK    4096

K_THREAD_STACK_DEFINE(notifier_stack, SCHED_TEST_STACK);
K_THREAD_STACK_DEFINE(neighbour_stack, SCHED_TEST_STACK);
static struct k_thread notifier_thread;
static struct k_thread neighbour_thread;

/* What ran, in order: N before the notify, n after it, O for the other. */
static char order[4];
static int order_len;
static int notify_result;

static void note(char c)
{
	order[order_len++] = c;
}

static void notifier(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	note('N');
	notify_result = zbus_chan_notify(&mode_chan, K_NO_WAIT);
	note('n');
}

static void neighbour(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	note('O');
}

ZTEST_SUITE(zbus_sched, NULL, NULL, NULL, NULL, NULL);

ZTEST(zbus_sched, test_a_notify_does_not_yield_to_an_equal_priority_thread)
{
	order_len = 0;
	memset(order, 0, sizeof(order));
	notify_result = -1;

	k_tid_t first = k_thread_create(&notifier_thread, notifier_stack,
					K_THREAD_STACK_SIZEOF(notifier_stack), notifier, NULL,
					NULL, NULL, SCHED_TEST_PRIORITY, 0, K_FOREVER);
	k_tid_t second = k_thread_create(&neighbour_thread, neighbour_stack,
					 K_THREAD_STACK_SIZEOF(neighbour_stack), neighbour, NULL,
					 NULL, NULL, SCHED_TEST_PRIORITY, 0, K_FOREVER);

	k_thread_start(first);
	k_thread_start(second);
	zassert_ok(k_thread_join(first, K_SECONDS(1)));
	zassert_ok(k_thread_join(second, K_SECONDS(1)));

	zassert_ok(notify_result);
	/* With priority boost on, this is "NOn": the notify yielded. */
	zassert_str_equal(order, "NnO", "ran in the order %s", order);
}
