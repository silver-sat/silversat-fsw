/*
 * Events (DS-10): one message per thing worth telling the ground, from any
 * app, to telemetry output, which sends each as an 'E' packet.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Apps don't call event_emit() directly. Each app's events are defined
 * under events: in its YAML, and the generator writes an
 * emit_<app>_<event>() for each into the app's own header, which fills in
 * the app, the severity and the event's number.
 *
 * Events go from every app to one reader, so they wait in a static queue
 * of EVENT_QUEUE_DEPTH (the resource map), not on zbus: a burst of events
 * can then never use up the zbus pool that commands need (DS-07, DS-11).
 * Emitting never waits. When the queue is full the new event is dropped and
 * counted, so the oldest are kept: the first events of a fault cascade,
 * usually its cause, survive.
 *
 * In a development build each event is also logged on the console as it
 * is raised (CONFIG_SS_EVENT_LOG), so the bench sees it at once. The flight
 * build doesn't queue events of severity debug (CONFIG_SS_EVENT_QUEUE_DEBUG).
 */

#ifndef SILVERSAT_EVENT_H_
#define SILVERSAT_EVENT_H_

#include <stdint.h>

#include "msg/common.h"

/*
 * An event's names, for the development log only: the event's, and each
 * argument's (NULL for an argument it doesn't have), and the name of each
 * argument's value when it has one (an app or an enum value; NULL to print
 * the number).
 */
struct event_text {
	const char *name;
	const char *arg_names[2];
	const char *arg_values[2];
};

/* Queue an event for the downlink. Never waits; never call it from an interrupt. */
void event_emit(const struct event *event, const struct event_text *text);

/*
 * Take the oldest queued event, for telemetry output. Returns 0, or
 * -ENOMSG if the queue is empty.
 */
int event_take(struct event *event);

/*
 * Events queued since boot, and events dropped because the queue was full
 * (debug events left out of the flight build aren't counted as dropped).
 */
struct event_stats {
	uint32_t queued;
	uint32_t dropped;
};

struct event_stats event_stats(void);

#endif /* SILVERSAT_EVENT_H_ */
