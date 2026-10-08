/*
 * Tests for the event queue (DS-10) and the generated emit functions.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * typed_app (apps/typed_app.yaml) defines three events: one with an app and
 * an enum argument, one with a plain number, and one with none.
 */

#include <errno.h>

#include <zephyr/ztest.h>

#include "msg/common.h"
#include "msg/typed_app.h"
#include "silversat/event.h"
#include "silversat/resource_map.h"

/* Each test starts with the queue empty. */
static void before(void *fixture)
{
	struct event event;

	ARG_UNUSED(fixture);
	while (event_take(&event) == 0) {
	}
}

ZTEST_SUITE(event, NULL, NULL, before, NULL, NULL);

ZTEST(event, test_emit_fills_in_the_event)
{
	struct event event;

	emit_typed_app_typed(1234, APP_ID_HEALTH, MODE_NOMINAL);
	zassert_ok(event_take(&event));
	zassert_equal(event.met_ms, 1234);
	zassert_equal(event.app, APP_ID_TYPED_APP);
	zassert_equal(event.severity, SEVERITY_WARNING);
	zassert_equal(event.id, TYPED_APP_EVENT_TYPED);
	zassert_equal(event.arg0, APP_ID_HEALTH);
	zassert_equal(event.arg1, MODE_NOMINAL);
}

ZTEST(event, test_unused_arguments_are_zero)
{
	struct event event;

	emit_typed_app_plain(-1, INT32_MIN);
	emit_typed_app_bare(0);
	zassert_ok(event_take(&event));
	zassert_equal(event.id, 513);
	zassert_equal(event.severity, SEVERITY_CRITICAL);
	zassert_equal(event.arg0, INT32_MIN);
	zassert_equal(event.arg1, 0);
	zassert_ok(event_take(&event));
	zassert_equal(event.id, 65535);
	zassert_equal(event.arg0, 0);
	zassert_equal(event.arg1, 0);
}

ZTEST(event, test_oldest_first)
{
	struct event event;

	for (int32_t i = 0; i < 3; i++) {
		emit_typed_app_plain(i, i);
	}
	for (int32_t i = 0; i < 3; i++) {
		zassert_ok(event_take(&event));
		zassert_equal(event.arg0, i);
	}
	zassert_equal(event_take(&event), -ENOMSG, "empty");
}

ZTEST(event, test_a_full_queue_keeps_the_oldest_and_counts_the_rest)
{
	struct event_stats start = event_stats();
	struct event event;

	for (int32_t i = 0; i < EVENT_QUEUE_DEPTH + 3; i++) {
		emit_typed_app_plain(i, i);
	}
	zassert_equal(event_stats().queued - start.queued, EVENT_QUEUE_DEPTH);
	zassert_equal(event_stats().dropped - start.dropped, 3);
	for (int32_t i = 0; i < EVENT_QUEUE_DEPTH; i++) {
		zassert_ok(event_take(&event));
		zassert_equal(event.arg0, i, "the first %d kept", EVENT_QUEUE_DEPTH);
	}
	zassert_equal(event_take(&event), -ENOMSG);
}
