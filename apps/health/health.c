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
 *          zbus pool buffers or spread. If the app is critical (its
 *          attribute row), health also asks the mode manager for safe
 *          mode, reason app_failure (DS-41). If its re-enable policy is
 *          REENABLE_AUTO, health starts it again after
 *          CONFIG_SS_REENABLE_COOLDOWN_FRAMES, up to its auto_retry_cap
 *          times; after that only the ground restarts it (DS-43);
 *   3. stores the run checkpoint, so the next boot carries MET on
 *      (boot.c, which also does health's work at boot: the boot number,
 *      the reset cause, and the boot log, DS-25, DS-44);
 *   4. reads every app thread's stack high-water mark: the least stack
 *      left unused, and which app (DS-44). Each app's thread is named
 *      <app>_tid (app_thread_name()), so health finds it by name;
 *   5. feeds the hardware watchdog, unless a reset is coming.
 *
 * A reset loop (DS-44): at boot, boot.c counts the short runs in a row in
 * the boot log. At CONFIG_SS_RESET_LOOP_SAFE_RUNS, health asks the mode
 * manager for safe mode, reason reset_loop. After every deployment boot the
 * spacecraft is already in safe mode, so this mostly records the reason;
 * the mode manager refuses it in deploy mode, which only the separation
 * delay ends (DS-42). If the loop goes on to CONFIG_SS_RESET_LOOP_STOP_RUNS,
 * in safe mode too, health also stops every app it watches that isn't
 * protected, until the ground starts them again: only a stall's stop is
 * ever undone automatically (REENABLE_AUTO), never a reset loop's.
 *
 * Requests go to the mode manager as an internal command (request_mode,
 * DS-40); like set_app_enabled, one that can't be sent is tried again next
 * major frame.
 *
 * Health is the only feeder, so the chain is frame manager, then health,
 * then watchdog: if the frame manager stops, health isn't woken; if health
 * stops, nothing feeds the watchdog. Either way, the spacecraft resets.
 * Health runs at the lowest app priority, so an app that hogs the CPU also
 * starves health and causes a reset.
 *
 * Still to come: events for each response (DS-10).
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/zbus/zbus.h>

#include "msg/common.h"
#include "boot.h"
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

/*
 * Safe-mode requests not yet sent: BIT(enum mode_reason) for each reason,
 * so each reason is sent once however often it arises before it goes.
 */
static uint32_t safe_requests;

/* Apps still to stop for a reset loop: bit n is app n. */
static uint64_t loop_stops;

/*
 * REENABLE_AUTO (DS-43): apps waiting out their cooldown, the major frame
 * each is due to start again, and how many times each has been started.
 */
static uint64_t reenable_waiting;
static uint32_t reenable_due[APP_ID_MAX + 1];
static uint8_t auto_reenables[APP_ID_MAX + 1];

/* Major frames health has handled, for the cooldown. */
static uint32_t major_frames;

BUILD_ASSERT(MODE_REASON_MAX < 32, "safe_requests has a bit for each reason");

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

/*
 * An app health has just stopped for a stall: if its policy is
 * REENABLE_AUTO, start it again after the cooldown, until it has used its
 * auto_retry_cap restarts. Then it falls back to REENABLE_GROUND (DS-43).
 */
static void schedule_reenable(uint8_t app)
{
	if (app_attrs[app].reenable != REENABLE_AUTO) {
		return;
	}
	if (auto_reenables[app] >= app_attrs[app].auto_retry_cap) {
		hk.auto_exhausted |= BIT64(app);
		return;
	}
	reenable_waiting |= BIT64(app);
	reenable_due[app] = major_frames + CONFIG_SS_REENABLE_COOLDOWN_FRAMES;
}

/* An app reached its threshold and hasn't been stopped. */
static void respond(uint8_t app)
{
	if (app_attrs[app].protected) {
		/* Nothing is fed after this; the watchdog resets the spacecraft. */
		if (!reset_coming) {
			reset_coming = true;
			hk.reset_app = app;
			boot_record_reset_app(app); /* stored with this frame's checkpoint */
		}
		return;
	}
	/* If it can't be sent now, the next report tries again. */
	if (send_frame_manager_set_app_enabled(app, false) == 0) {
		hk.disabled_by_health |= BIT64(app);
		if (app_attrs[app].critical) {
			safe_requests |= BIT(MODE_REASON_APP_FAILURE);
		}
		schedule_reenable(app);
	} else {
		hk.action_failures++;
	}
}

/* Act on a reset loop that boot.c counted (DS-44). */
static void check_reset_loop(void)
{
	hk.short_runs = boot_short_runs();
	if (hk.short_runs >= CONFIG_SS_RESET_LOOP_SAFE_RUNS) {
		safe_requests |= BIT(MODE_REASON_RESET_LOOP);
	}
	if (hk.short_runs < CONFIG_SS_RESET_LOOP_STOP_RUNS) {
		return;
	}
	for (uint8_t app = 1; app <= APP_ID_MAX; app++) {
		/* Every app health watches and could stop for a stall. */
		if (!app_attrs[app].protected && app_attrs[app].stall_threshold > 0) {
			loop_stops |= BIT64(app);
		}
	}
}

/*
 * Send what is waiting: safe-mode requests, then reset-loop stops. Anything
 * that can't be sent now (the target already has CMD_MAX_PENDING commands
 * waiting) stays, and goes next major frame.
 */
static void send_waiting(void)
{
	for (uint8_t reason = 0; reason <= MODE_REASON_MAX; reason++) {
		if ((safe_requests & BIT(reason)) == 0) {
			continue;
		}
		if (send_mode_manager_request_mode(MODE_SAFE, reason) == 0) {
			safe_requests &= ~BIT(reason);
			hk.mode_requests++;
		} else {
			hk.action_failures++;
		}
	}
	for (uint8_t app = 1; app <= APP_ID_MAX; app++) {
		if ((loop_stops & BIT64(app)) == 0) {
			continue;
		}
		if (send_frame_manager_set_app_enabled(app, false) != 0) {
			hk.action_failures++;
			return; /* the frame manager is busy; the rest go next major frame */
		}
		loop_stops &= ~BIT64(app);
		hk.disabled_by_health |= BIT64(app);
	}
}

/* Start again each AUTO app whose cooldown is over (DS-43). */
static void reenable_due_apps(void)
{
	for (uint8_t app = 1; app <= APP_ID_MAX; app++) {
		if ((reenable_waiting & BIT64(app)) == 0 || major_frames < reenable_due[app]) {
			continue;
		}
		if (send_frame_manager_set_app_enabled(app, true) != 0) {
			hk.action_failures++;
			return; /* the frame manager is busy; the rest go next major frame */
		}
		reenable_waiting &= ~BIT64(app);
		auto_reenables[app]++;
		hk.auto_reenables++;
		hk.disabled_by_health &= ~BIT64(app);
	}
}

/*
 * Each app thread's stack high-water mark (DS-44). Called for every thread;
 * only app threads count, found by the name each app gives its thread.
 */
static void scan_stack(const struct k_thread *thread, void *user_data)
{
	const char *name = k_thread_name_get((k_tid_t)thread);
	uint8_t *apps = user_data;
	size_t unused;

	if (name == NULL) {
		return;
	}
	for (uint8_t app = 1; app <= APP_ID_MAX; app++) {
		const char *app_name = app_thread_name(app);

		if (app_name == NULL || strcmp(name, app_name) != 0) {
			continue;
		}
		if (k_thread_stack_space_get(thread, &unused) != 0) {
			return;
		}
		(*apps)++;
		if (hk.stack_min_app == 0 || unused < hk.stack_min_unused) {
			hk.stack_min_app = app;
			hk.stack_min_unused = (uint32_t)unused;
		}
		return;
	}
}

static void check_stacks(void)
{
	uint8_t apps = 0;

	hk.stack_min_app = 0;
	hk.stack_min_unused = 0;
	k_thread_foreach_unlocked(scan_stack, &apps);
	hk.stack_apps = apps;
	/* Once low, it stays low: the high-water mark never goes down. */
	if (apps > 0 && hk.stack_min_unused < CONFIG_SS_STACK_MARGIN_BYTES) {
		hk.stack_low = true;
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

static void step(const struct frame_tick *tick)
{
	struct frame_report report;

	major_frames++;
	if (zbus_chan_read(&frame_report_chan, &report, K_NO_WAIT) == 0 &&
	    (!have_report || report.major_frame != last_major_frame)) {
		have_report = true;
		last_major_frame = report.major_frame;
		check(&report);
	}
	send_waiting();
	reenable_due_apps();
	check_stacks();
	boot_checkpoint(tick->met_ms, tick->uptime_ms);
	watchdog_feed();
	hk.store_failures = boot_store_failures();
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
	hk.boot_number = boot_number();
	hk.reset_cause = boot_reset_cause();
	hk.store_failures = boot_store_failures();
	check_reset_loop();
	zbus_chan_pub(&health_hk_chan, &hk, K_NO_WAIT);

	while (zbus_sub_wait_msg(&health_sub, &chan, &msg, K_FOREVER) == 0) {
		if (chan == &health_wakeup_chan) {
			step(&msg.tick);
			status.steps++;
		} else if (chan == &health_cmd_chan) {
			status.cmd_rejected++; /* no commands yet */
		}
		zbus_chan_pub(&health_status_chan, &status, K_NO_WAIT);
	}
}

K_THREAD_DEFINE(health_tid, HEALTH_STACK_SIZE, health_main, NULL, NULL, NULL, HEALTH_PRIORITY, 0,
		0);
