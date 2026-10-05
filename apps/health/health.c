/*
 * Health (DS-43).
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Woken once a major frame, in slot 0, health:
 *
 *   1. reads the frame manager's report (frame_report_chan) of which apps
 *      are a whole major frame behind;
 *   2. counts, for each app, how many reports in a row have shown it
 *      behind, and when the count reaches the app's stall_threshold
 *      (app_attrs[] in the resource map):
 *        - a protected app: stops feeding the watchdog, which resets the
 *          spacecraft (DS-43). Restarting one thread is never safe in a
 *          single address space; a reset is honest.
 *        - any other app: tells the frame manager to stop waking it
 *          (frame_manager set_app_enabled). Its stall can't then hold
 *          zbus pool buffers or spread;
 *   3. feeds the hardware watchdog, unless a reset is coming.
 *
 * Health is the only feeder, so the chain is frame manager, then health,
 * then watchdog: if the frame manager stops, health isn't woken; if health
 * stops, nothing feeds the watchdog. Either way, the spacecraft resets.
 * Health runs at the lowest app priority, so an app that hogs the CPU also
 * starves health and causes a reset.
 *
 * Still to come: events for each response (DS-10), re-enable policies, and
 * asking the mode manager for safe mode when a critical app fails (DS-41).
 */

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/zbus/zbus.h>

#include "msg/common.h"
#include "msg/health.h"
#include "silversat/resource_map.h"

ZBUS_MSG_SUBSCRIBER_DEFINE(health_sub);
ZBUS_CHAN_ADD_OBS(health_wakeup_chan, health_sub, 3);
ZBUS_CHAN_ADD_OBS(health_cmd_chan, health_sub, 3);

/* The hardware watchdog, if the board has one (the watchdog0 alias). */
static const struct device *const watchdog = DEVICE_DT_GET_OR_NULL(WATCHDOG_NODE);
static int watchdog_channel = -1;

static struct app_status status;
static struct health_hk hk;

/* Reports in a row that have shown each app a major frame behind. */
static uint8_t behind[APP_ID_MAX + 1];

/* The last report read, so a report is counted only once. */
static bool have_report;
static uint32_t last_major_frame;

/* Set when a protected app stalls: health stops feeding the watchdog. */
static bool reset_coming;

static void watchdog_start(void)
{
	const struct wdt_timeout_cfg config = {
		.window.max = CONFIG_SS_WATCHDOG_TIMEOUT_MS,
		.flags = WDT_FLAG_RESET_SOC,
	};

	if (watchdog == NULL || !device_is_ready(watchdog)) {
		return; /* no watchdog: health still watches the apps */
	}
	watchdog_channel = wdt_install_timeout(watchdog, &config);
	/* Paused while a debugger halts the board, or the flatsat resets under a student. */
	if (watchdog_channel < 0 || wdt_setup(watchdog, WDT_OPT_PAUSE_HALTED_BY_DBG) != 0) {
		watchdog_channel = -1;
		return;
	}
	hk.watchdog = true;
}

static void watchdog_feed(void)
{
	if (watchdog_channel >= 0 && !reset_coming &&
	    wdt_feed(watchdog, watchdog_channel) == 0) {
		hk.feeds++;
	}
}

/* An app reached its threshold and hasn't been stopped. */
static void respond(uint8_t app)
{
	if (app_attrs[app].protected) {
		/* Nothing is fed after this; the watchdog resets the spacecraft. */
		if (!reset_coming) {
			reset_coming = true;
			hk.reset_app = app;
		}
		return;
	}
	/* If it can't be sent now, the next report tries again. */
	if (send_frame_manager_set_app_enabled(app, false) == 0) {
		hk.disabled_by_health |= BIT64(app);
	} else {
		hk.action_failures++;
	}
}

static void check(const struct frame_report *report)
{
	for (uint8_t app = 1; app <= APP_ID_MAX; app++) {
		uint8_t threshold = app_attrs[app].stall_threshold;

		if (threshold == 0) {
			continue; /* not watched by health */
		}
		/*
		 * A stopped app counts from zero again, so if the ground starts
		 * it, it gets its whole threshold before health acts again.
		 */
		if ((report->stuck & BIT64(app)) == 0 || (report->disabled & BIT64(app)) != 0) {
			behind[app] = 0;
			continue;
		}
		if (behind[app] < UINT8_MAX) {
			behind[app]++;
		}
		if (behind[app] == threshold) {
			hk.stalls++;
		}
		if (behind[app] >= threshold) {
			respond(app);
		}
	}
}

static void step(void)
{
	struct frame_report report;

	if (zbus_chan_read(&frame_report_chan, &report, K_NO_WAIT) == 0 &&
	    (!have_report || report.major_frame != last_major_frame)) {
		have_report = true;
		last_major_frame = report.major_frame;
		check(&report);
	}
	watchdog_feed();
	zbus_chan_pub(&health_hk_chan, &hk, K_NO_WAIT);
}

static void health_main(void *a, void *b, void *c)
{
	const struct zbus_channel *chan;
	union health_msg msg;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	watchdog_start();
	zbus_chan_pub(&health_hk_chan, &hk, K_NO_WAIT);

	while (zbus_sub_wait_msg(&health_sub, &chan, &msg, K_FOREVER) == 0) {
		if (chan == &health_wakeup_chan) {
			step();
			status.steps++;
		} else if (chan == &health_cmd_chan) {
			status.cmd_rejected++; /* no commands yet */
		}
		zbus_chan_pub(&health_status_chan, &status, K_NO_WAIT);
	}
}

K_THREAD_DEFINE(health_tid, HEALTH_STACK_SIZE, health_main, NULL, NULL, NULL, HEALTH_PRIORITY, 0,
		0);
