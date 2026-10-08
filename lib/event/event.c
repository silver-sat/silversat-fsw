/*
 * Events (DS-10). See include/silversat/event.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>

#include "silversat/event.h"
#include "silversat/resource_map.h"

LOG_MODULE_REGISTER(event, LOG_LEVEL_INF);

K_MSGQ_DEFINE(event_msgq, sizeof(struct event), EVENT_QUEUE_DEPTH, 8);

/* Counted from any app's thread, so atomic. */
static atomic_t queued;
static atomic_t dropped;

#if defined(CONFIG_SS_EVENT_LOG)
/* A name, or "?" for a value without one. */
static const char *or_unknown(const char *name)
{
	return name != NULL ? name : "?";
}

/* " app=nvm", or " restarts=2", for one argument, or nothing. */
static void format_arg(char *out, size_t size, const char *name, const char *value_name,
		       int32_t value)
{
	if (name == NULL) {
		out[0] = '\0';
	} else if (value_name != NULL) {
		snprintk(out, size, " %s=%s", name, value_name);
	} else {
		snprintk(out, size, " %s=%d", name, (int)value);
	}
}

/* "health app_stalled (warning) app=nvm", for the bench (DS-06). */
static void log_event(const struct event *event, const struct event_text *text)
{
	char arg0[40];
	char arg1[40];

	format_arg(arg0, sizeof(arg0), text->arg_names[0], text->arg_values[0], event->arg0);
	format_arg(arg1, sizeof(arg1), text->arg_names[1], text->arg_values[1], event->arg1);
	LOG_INF("%s %s (%s)%s%s", or_unknown(app_name(event->app)), text->name,
		or_unknown(severity_name(event->severity)), arg0, arg1);
}
#endif

void event_emit(const struct event *event, const struct event_text *text)
{
#if defined(CONFIG_SS_EVENT_LOG)
	log_event(event, text);
#else
	ARG_UNUSED(text);
#endif
	/* Not in the flight build: development detail, not worth the link. */
	if (!IS_ENABLED(CONFIG_SS_EVENT_QUEUE_DEBUG) && event->severity == SEVERITY_DEBUG) {
		return;
	}
	if (k_msgq_put(&event_msgq, event, K_NO_WAIT) == 0) {
		atomic_inc(&queued);
	} else {
		atomic_inc(&dropped); /* full: keep the oldest (DS-10) */
	}
}

int event_take(struct event *event)
{
	return k_msgq_get(&event_msgq, event, K_NO_WAIT) == 0 ? 0 : -ENOMSG;
}

struct event_stats event_stats(void)
{
	return (struct event_stats){
		.queued = (uint32_t)atomic_get(&queued),
		.dropped = (uint32_t)atomic_get(&dropped),
	};
}
