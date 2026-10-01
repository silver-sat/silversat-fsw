/*
 * What zbus does when its message buffer pool runs out (DS-07, DS-22).
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The pool sizing in include/silversat/resource_map.h and the frame
 * manager's pending bound both rest on this behavior of Zephyr's zbus
 * (subsys/zbus/zbus.c, v4.4.0). If a Zephyr upgrade changes it, this test
 * fails, and DS-07 and DS-22 need another look.
 *
 * These channels are written by hand, not generated: this tests zbus
 * itself, so it stays independent of our message definitions.
 */

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/ztest.h>
#include <zephyr/ztest_error_hook.h>

struct msg {
	uint32_t value;
};

BUILD_ASSERT(CONFIG_ZBUS_MSG_SUBSCRIBER_NET_BUF_POOL_SIZE == 2,
	     "the steps below assume a two-buffer pool");

/* Observed by a message subscriber that never reads: a stuck app. */
ZBUS_CHAN_DEFINE(queued_chan, struct msg, NULL, NULL, ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0));
ZBUS_MSG_SUBSCRIBER_DEFINE(stuck_sub);
ZBUS_CHAN_ADD_OBS(queued_chan, stuck_sub, 3);

/* Observed by no message subscriber: a housekeeping or status channel. */
ZBUS_CHAN_DEFINE(plain_chan, struct msg, NULL, NULL, ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0));

/*
 * A second publish in flight. In flight software this is a low-priority app
 * preempted part-way through a publish by the frame manager. Here a
 * listener publishes from inside another publish, which is the same
 * situation without depending on thread timing.
 */
ZBUS_CHAN_DEFINE(outer_chan, struct msg, NULL, NULL, ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0));

static volatile bool nested_returned;

static void publish_while_outer_holds_a_buffer(const struct zbus_channel *chan)
{
	const struct msg m = {.value = 3};

	ARG_UNUSED(chan);
	(void)zbus_chan_pub(&plain_chan, &m, K_NO_WAIT);
	nested_returned = true; /* not reached: the publish above asserts */
}
ZBUS_LISTENER_DEFINE(nested_lis, publish_while_outer_holds_a_buffer);
ZBUS_CHAN_ADD_OBS(outer_chan, nested_lis, 3);

K_THREAD_STACK_DEFINE(publisher_stack, 2048);
static struct k_thread publisher;

static void publish_outer(void *a, void *b, void *c)
{
	const struct msg m = {.value = 4};

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	ztest_set_assert_valid(true);
	(void)zbus_chan_pub(&outer_chan, &m, K_NO_WAIT);
}

/*
 * One test, in order: each step depends on the pool state the step before
 * left. The last step leaves outer_chan and plain_chan locked by an aborted
 * thread, so nothing may follow it.
 */
ZTEST(zbus_pool, test_running_out_of_buffers)
{
	const struct msg m = {.value = 1};

	/*
	 * 1. A publish takes one buffer, and the stuck subscriber's copy takes
	 *    another. The publish's own buffer is freed when it returns, so one
	 *    stays in use: the queued copy.
	 */
	zassert_ok(zbus_chan_pub(&queued_chan, &m, K_NO_WAIT));

	/*
	 * 2. The next publish takes the last free buffer, so its copy cannot
	 *    be allocated. That is an error return, not a crash. (It is not a
	 *    useful queue-full signal: the pool is shared, so by now every
	 *    other app is one buffer from trouble.)
	 */
	zassert_equal(zbus_chan_pub(&queued_chan, &m, K_NO_WAIT), -ENOMEM);

	/* 3. A channel with no message subscribers still publishes: it needs
	 *    only its own buffer, which is free again.
	 */
	zassert_ok(zbus_chan_pub(&plain_chan, &m, K_NO_WAIT));

	/*
	 * 4. With one buffer queued and one held by a publish in flight, a
	 *    second publish cannot get its own buffer. zbus asserts (a flight
	 *    build, with asserts off, dereferences NULL). This is why the pool
	 *    needs one buffer per publishing thread on top of every queued copy.
	 */
	k_thread_create(&publisher, publisher_stack, K_THREAD_STACK_SIZEOF(publisher_stack),
			publish_outer, NULL, NULL, NULL, K_PRIO_PREEMPT(1), 0, K_NO_WAIT);
	zassert_ok(k_thread_join(&publisher, K_SECONDS(1)));
	zassert_false(nested_returned, "the nested publish should have asserted");
}

ZTEST_SUITE(zbus_pool, NULL, NULL, NULL, NULL, NULL);
