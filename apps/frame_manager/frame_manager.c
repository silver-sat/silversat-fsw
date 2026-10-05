/*
 * Frame manager (DS-20 to DS-25).
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Drives all periodic work. A k_timer wakes this thread once per minor
 * frame (FRAME_MINOR_MS). The thread publishes a frame tick to the wakeup
 * channel of every app whose table entry is in the current slot. Nothing is
 * published from the timer's interrupt, and nothing here ever waits: every
 * publish and read uses K_NO_WAIT, so one stuck app cannot stall the frame
 * (DS-22).
 *
 * Each mode has its own frame table (DS-23). The frame manager reads the
 * mode from mode_chan every minor frame (DS-40). Entering safe mode takes
 * effect at once; any other change waits for the next major frame, so one
 * major frame never mixes two tables.
 *
 * At the start of each major frame it publishes frame_report_chan for
 * health: which apps are a whole major frame behind, and which it has been
 * told to stop (DS-43). Health decides what to do; the frame manager only
 * reports what it already counts.
 *
 * Unlike other apps, the frame manager's one pending point is its timer,
 * not zbus. It collects its commands without waiting, at the start of each
 * minor frame, so a command takes effect within one minor frame.
 */

#include <errno.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/__assert.h>
#include <zephyr/zbus/zbus.h>

#include "msg/common.h"
#include "msg/frame_manager.h"
#include "silversat/frame_table.h"
#include "silversat/resource_map.h"

ZBUS_MSG_SUBSCRIBER_DEFINE(frame_manager_sub);
ZBUS_CHAN_ADD_OBS(frame_manager_cmd_chan, frame_manager_sub, 3);

K_TIMER_DEFINE(minor_frame_timer, NULL, NULL);

/* Whether each entry of each table is delivered. Kept in RAM; the tables are const. */
static bool entry_enabled[MODE_MAX + 1][FRAME_ENTRIES_MAX];

/*
 * Wakeups delivered and skipped, per app rather than per entry: a 10 Hz app
 * has ten entries but only one step counter (DS-22).
 */
static uint32_t delivered[APP_ID_MAX + 1];
static uint32_t overruns[APP_ID_MAX + 1];

/* Whether each app's wakeups are delivered at all (set_app_enabled). */
static bool app_enabled[APP_ID_MAX + 1];

/*
 * For the frame report (DS-43): each app's status channel, found in the
 * tables at start, and the wakeups delivered to it by the start of the
 * last major frame.
 */
static const struct zbus_channel *status_chans[APP_ID_MAX + 1];
static uint32_t delivered_at_last_report[APP_ID_MAX + 1];

BUILD_ASSERT(APP_ID_MAX < 64, "the frame report has one bit per app in a uint64");

/*
 * The mode whose table is in use. Set at start from mode_chan's initial
 * value, deploy, before the mode manager has published (DS-42).
 */
static uint8_t active_mode;

static struct app_status status;
static struct frame_manager_hk hk;

/*
 * Mission elapsed time is mission time at boot plus uptime (DS-25). Until
 * health publishes mission time at boot from FRAM (DS-45, DS-72), each
 * boot's MET starts at zero.
 */
static int64_t frame_met(int64_t uptime_ms)
{
	return uptime_ms;
}

int frame_table_check(const struct frame_table *table)
{
	if (table->len > FRAME_ENTRIES_MAX || (table->len > 0 && table->entries == NULL)) {
		return -EINVAL;
	}
	for (uint8_t i = 0; i < table->len; i++) {
		const struct frame_entry *e = &table->entries[i];

		if (e->slot >= FRAME_SLOTS || e->app > APP_ID_MAX || e->wakeup_chan == NULL ||
		    e->status_chan == NULL) {
			return -EINVAL;
		}
	}
	return 0;
}

static int set_entry_enabled(const struct frame_manager_set_entry_enabled *args)
{
	const struct frame_table *table;

	if (args->mode > MODE_MAX) {
		hk.rejected_bad_index++;
		return -EINVAL;
	}
	table = &frame_tables[args->mode];
	if (args->entry >= table->len) {
		hk.rejected_bad_index++;
		return -EINVAL;
	}
	/* A protected app's wakeups are never stopped (DS-23, DS-43). */
	if (!args->enabled && app_attrs[table->entries[args->entry].app].protected) {
		hk.rejected_protected++;
		return -EPERM;
	}
	entry_enabled[args->mode][args->entry] = args->enabled;
	return 0;
}

static int set_app_enabled(const struct frame_manager_set_app_enabled *args)
{
	/* Only an app in some table has wakeups to start or stop. */
	if (args->app > APP_ID_MAX || status_chans[args->app] == NULL) {
		hk.rejected_bad_index++;
		return -EINVAL;
	}
	/* A protected app's wakeups are never stopped (DS-23, DS-43). */
	if (!args->enabled && app_attrs[args->app].protected) {
		hk.rejected_protected++;
		return -EPERM;
	}
	app_enabled[args->app] = args->enabled;
	if (args->enabled) {
		hk.disabled_apps &= ~BIT64(args->app);
	} else {
		hk.disabled_apps |= BIT64(args->app);
	}
	return 0;
}

/* Handle every command waiting for us, without waiting for more. */
static void handle_commands(void)
{
	const struct zbus_channel *chan;
	union frame_manager_msg msg;

	while (zbus_sub_wait_msg(&frame_manager_sub, &chan, &msg, K_NO_WAIT) == 0) {
		int rc;

		switch (msg.cmd.id) {
		case FRAME_MANAGER_CMD_SET_ENTRY_ENABLED:
			rc = set_entry_enabled(&msg.cmd.args.set_entry_enabled);
			break;
		case FRAME_MANAGER_CMD_SET_APP_ENABLED:
			rc = set_app_enabled(&msg.cmd.args.set_app_enabled);
			break;
		default:
			hk.rejected_bad_command++;
			rc = -ENOTSUP;
			break;
		}
		if (rc == 0) {
			status.cmd_accepted++;
		} else {
			status.cmd_rejected++;
		}
	}
}

/* Switch tables when the mode manager changes the mode (DS-23, DS-40). */
static void follow_mode(uint8_t slot)
{
	struct mode_state mode;

	if (zbus_chan_read(&mode_chan, &mode, K_NO_WAIT) != 0) {
		return; /* busy this instant; read it again next minor frame */
	}
	if (mode.mode == active_mode || mode.mode > MODE_MAX) {
		return;
	}
	if (mode.mode == MODE_SAFE || slot == 0) {
		active_mode = mode.mode;
	}
}

static void deliver(const struct frame_tick *tick)
{
	const struct frame_table *table = &frame_tables[active_mode];

	for (uint8_t i = 0; i < table->len; i++) {
		const struct frame_entry *e = &table->entries[i];
		struct app_status app;

		if (e->slot != tick->slot || !entry_enabled[active_mode][i] ||
		    !app_enabled[e->app]) {
			continue;
		}
		/*
		 * Never publish a copy the app can't consume (DS-22): every
		 * wakeup waiting for it holds a buffer from the shared zbus
		 * pool. Only the frame manager publishes wakeups, so steps can
		 * never pass delivered and the unsigned difference is the
		 * number still pending. If the status can't be read without
		 * waiting, skip rather than guess.
		 */
		if (zbus_chan_read(e->status_chan, &app, K_NO_WAIT) != 0 ||
		    delivered[e->app] - app.steps >= FRAME_MAX_PENDING) {
			overruns[e->app]++;
			hk.overruns++;
			continue;
		}
		if (zbus_chan_pub(e->wakeup_chan, tick, K_NO_WAIT) == 0) {
			delivered[e->app]++;
		} else {
			hk.publish_errors++;
		}
	}
}

/*
 * At the start of each major frame, before any wakeup: which apps are still
 * not finished with the wakeups delivered before the last report (DS-43).
 * An app working normally finishes a wakeup within the major frame it was
 * delivered in, even one delivered in the last slot. If an app's status
 * can't be read without waiting, it isn't reported this time.
 */
static void report(uint32_t major_frame)
{
	struct frame_report report = {
		.major_frame = major_frame,
		.disabled = hk.disabled_apps,
	};

	for (uint8_t app = 0; app <= APP_ID_MAX; app++) {
		struct app_status app_status;

		if (status_chans[app] == NULL) {
			continue;
		}
		/* Signed difference: steps may lag delivered, and both wrap. */
		if (zbus_chan_read(status_chans[app], &app_status, K_NO_WAIT) == 0 &&
		    (int32_t)(delivered_at_last_report[app] - app_status.steps) > 0) {
			report.stuck |= BIT64(app);
		}
		delivered_at_last_report[app] = delivered[app];
	}
	zbus_chan_pub(&frame_report_chan, &report, K_NO_WAIT);
}

static void frame_manager_main(void *a, void *b, void *c)
{
	struct frame_tick tick = {0};
	uint32_t count = 0;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	for (uint8_t mode = 0; mode <= MODE_MAX; mode++) {
		__ASSERT(frame_table_check(&frame_tables[mode]) == 0, "bad frame table for mode %u",
			 mode);
		for (uint8_t i = 0; i < FRAME_ENTRIES_MAX; i++) {
			entry_enabled[mode][i] = true;
		}
		for (uint8_t i = 0; i < frame_tables[mode].len; i++) {
			const struct frame_entry *e = &frame_tables[mode].entries[i];

			status_chans[e->app] = e->status_chan;
		}
	}
	for (uint8_t app = 0; app <= APP_ID_MAX; app++) {
		app_enabled[app] = true;
	}

	/* The first minor frame starts now; the timer keeps the rest in step. */
	{
		struct mode_state boot;

		(void)zbus_chan_read(&mode_chan, &boot, K_FOREVER);
		active_mode = boot.mode <= MODE_MAX ? boot.mode : MODE_DEPLOY;
	}
	k_timer_start(&minor_frame_timer, K_NO_WAIT, K_MSEC(FRAME_MINOR_MS));

	for (;;) {
		/*
		 * Returns how many minor frames have started since the last
		 * call. More than one means this thread fell behind: skip the
		 * frames it missed, so the slot stays in step with time.
		 */
		uint32_t started = k_timer_status_sync(&minor_frame_timer);

		if (started > 1) {
			hk.missed_frames += started - 1;
			count += started - 1;
		}
		tick.count = count;
		tick.slot = count % FRAME_SLOTS;
		tick.uptime_ms = k_uptime_get(); /* 64-bit: never wraps (DS-25) */
		tick.met_ms = frame_met(tick.uptime_ms);

		handle_commands();
		follow_mode(tick.slot);
		if (tick.slot == 0) {
			report(tick.count / FRAME_SLOTS);
		}
		deliver(&tick);

		if (tick.slot == 0) {
			hk.frame_count = tick.count;
			hk.mode = active_mode;
			zbus_chan_pub(&frame_manager_hk_chan, &hk, K_NO_WAIT);
		}
		status.steps++;
		zbus_chan_pub(&frame_manager_status_chan, &status, K_NO_WAIT);
		count++;
	}
}

K_THREAD_DEFINE(frame_manager_tid, FRAME_MANAGER_STACK_SIZE, frame_manager_main, NULL, NULL,
		NULL, FRAME_MANAGER_PRIORITY, 0, 0);
