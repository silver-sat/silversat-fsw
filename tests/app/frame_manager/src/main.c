/*
 * Frame manager tests (DS-20 to DS-25).
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The frame manager runs as it does in flight, against the test tables in
 * src/test_tables.c and the test apps in src/test_apps.c. The tests stand
 * in for the mode manager by publishing mode_chan, and for the ground by
 * publishing commands.
 *
 * Everything runs in simulated time. The frame manager starts minor frame n
 * at exactly n * FRAME_MINOR_MS, so a test that sleeps to the middle of a
 * minor frame knows exactly which ticks have been delivered.
 */

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/ztest.h>

#include "app_test.h"
#include "msg/common.h"
#include "msg/frame_manager.h"
#include "silversat/frame_table.h"
#include "silversat/resource_map.h"
#include "test_apps.h"
#include "test_tables.h"

/* Generous: in simulated time a wait costs nothing unless it fails. */
#define WAIT K_SECONDS(1)

/* ---- Helpers ------------------------------------------------------------ */

/* Sleep to the middle of the next minor frame whose slot is slot. */
static void sleep_to_slot(uint8_t slot)
{
	int64_t now = k_uptime_get();
	int64_t count = now / FRAME_MINOR_MS + 1;

	while (count % FRAME_SLOTS != slot) {
		count++;
	}
	k_sleep(K_MSEC(count * FRAME_MINOR_MS + FRAME_MINOR_MS / 2 - now));
}

/* Stand in for the mode manager (DS-40). */
static void set_mode(uint8_t mode)
{
	const struct mode_state state = {.mode = mode, .reason = MODE_REASON_GROUND_COMMAND};

	zassert_ok(zbus_chan_pub(&mode_chan, &state, K_NO_WAIT));
}

static struct app_status fm_status(void)
{
	struct app_status status;

	zassert_ok(zbus_chan_read(&frame_manager_status_chan, &status, K_MSEC(10)));
	return status;
}

static struct frame_manager_hk fm_hk(void)
{
	struct frame_manager_hk hk;

	zassert_ok(zbus_chan_read(&frame_manager_hk_chan, &hk, K_MSEC(10)));
	return hk;
}

/*
 * Send a command and wait for the frame manager to handle it (it collects
 * commands at the start of each minor frame). Returns true if accepted.
 */
static bool send_command(const struct frame_manager_cmd *cmd)
{
	struct app_status before = fm_status();
	struct app_status after;

	zassert_ok(zbus_chan_pub(&frame_manager_cmd_chan, cmd, K_NO_WAIT));
	zassert_ok(app_test_wait_status(&frame_manager_status_chan, 0,
					before.cmd_accepted + before.cmd_rejected + 1, WAIT,
					&after));
	return after.cmd_accepted == before.cmd_accepted + 1;
}

static bool set_entry_enabled(uint8_t mode, uint8_t entry, bool enabled)
{
	const struct frame_manager_cmd cmd = {
		.id = FRAME_MANAGER_CMD_SET_ENTRY_ENABLED,
		.args.set_entry_enabled = {.mode = mode, .entry = entry, .enabled = enabled},
	};

	return send_command(&cmd);
}

/* The slots in which counter_app was woken, oldest first. */
static uint32_t counter_slots(uint8_t *slots, uint32_t max)
{
	uint32_t n = MIN(counter_app_seen.len, max);

	for (uint32_t i = 0; i < n; i++) {
		slots[i] = counter_app_seen.ticks[i].slot;
	}
	return n;
}

/*
 * Finish the current major frame, clear the test apps' records, then run n
 * whole major frames. Returns in the middle of slot 9.
 */
static void fresh_major_frames(int n)
{
	sleep_to_slot(9);
	seen_clear(&counter_app_seen);
	seen_clear(&stuck_app_seen);
	for (int i = 0; i < n; i++) {
		sleep_to_slot(9);
	}
}

/*
 * Put the frame manager in a known state: the given mode, every entry
 * enabled, stuck_app running freely. Returns in the middle of slot 9, after
 * a full major frame in that mode, with the test apps' records cleared.
 */
static void settle_in(uint8_t mode)
{
	stuck_app_hold(false);
	set_mode(mode);
	for (uint8_t i = 0; i < SAFE_ENTRIES; i++) {
		zassert_true(set_entry_enabled(MODE_SAFE, i, true));
	}
	for (uint8_t i = 0; i < NOMINAL_ENTRIES; i++) {
		zassert_true(set_entry_enabled(MODE_NOMINAL, i, true));
	}
	sleep_to_slot(0);
	sleep_to_slot(9);
	seen_clear(&counter_app_seen);
	seen_clear(&stuck_app_seen);
}

/* ---- Before any test: boot -------------------------------------------- */

/* What counter_app saw in the first major frame after boot. */
static struct seen_ticks boot_seen;
static struct frame_manager_hk boot_hk;

/*
 * How far each minor frame starts after count * FRAME_MINOR_MS. Zephyr
 * starts a timer one system tick late rather than early, so this is one
 * tick, and it must never change: a change is drift.
 */
static int64_t frame_offset_ms(void)
{
	return boot_seen.ticks[0].uptime_ms - (int64_t)boot_seen.ticks[0].count * FRAME_MINOR_MS;
}

static void *record_boot(void)
{
	/* No one has published mode_chan yet. */
	sleep_to_slot(9);
	boot_seen = counter_app_seen;
	boot_hk = fm_hk();
	return NULL;
}

ZTEST_SUITE(frame_manager, NULL, record_boot, NULL, NULL, NULL);

/* ---- Tests ------------------------------------------------------------- */

ZTEST(frame_manager, test_boots_into_the_deploy_table)
{
	/* mode_chan starts as deploy until the mode manager publishes (DS-42). */
	zassert_equal(boot_hk.mode, MODE_DEPLOY);
	zassert_equal(boot_seen.len, 1, "only the deploy table's slot-3 entry runs");
	zassert_equal(boot_seen.ticks[0].slot, 3);
	zassert_equal(boot_seen.ticks[0].count, 3);
	zassert_true(frame_offset_ms() >= 0 && frame_offset_ms() < FRAME_MINOR_MS / 2,
		     "frames start %lld ms late", frame_offset_ms());
}

ZTEST(frame_manager, test_slot_phasing)
{
	const uint8_t expected[] = {0, 5, 0, 5, 0, 5};
	uint8_t slots[SEEN_MAX];

	settle_in(MODE_NOMINAL);
	for (int major = 0; major < 3; major++) {
		sleep_to_slot(9);
	}

	zassert_equal(counter_slots(slots, SEEN_MAX), ARRAY_SIZE(expected));
	zassert_mem_equal(slots, expected, sizeof(expected));
	for (uint32_t i = 0; i < counter_app_seen.len; i++) {
		const struct frame_tick *t = &counter_app_seen.ticks[i];

		zassert_equal(t->slot, t->count % FRAME_SLOTS);
		zassert_equal(t->uptime_ms,
			      (int64_t)t->count * FRAME_MINOR_MS + frame_offset_ms());
		zassert_equal(t->met_ms, t->uptime_ms, "MET starts at zero until health exists");
	}
}

ZTEST(frame_manager, test_disabled_entry_is_not_delivered)
{
	uint8_t slots[SEEN_MAX];

	settle_in(MODE_NOMINAL);
	zassert_true(set_entry_enabled(MODE_NOMINAL, NOMINAL_COUNTER_SLOT5, false));
	/* Commands set state; repeating one is accepted and changes nothing (DS-35). */
	zassert_true(set_entry_enabled(MODE_NOMINAL, NOMINAL_COUNTER_SLOT5, false));

	fresh_major_frames(1);
	zassert_equal(counter_slots(slots, SEEN_MAX), 1);
	zassert_equal(slots[0], 0, "slot 5 is disabled");

	zassert_true(set_entry_enabled(MODE_NOMINAL, NOMINAL_COUNTER_SLOT5, true));
	fresh_major_frames(1);
	zassert_equal(counter_slots(slots, SEEN_MAX), 2);
	zassert_equal(slots[1], 5, "slot 5 is enabled again");
}

ZTEST(frame_manager, test_mode_change_waits_for_the_major_frame)
{
	uint8_t slots[SEEN_MAX];

	settle_in(MODE_SAFE);
	sleep_to_slot(3);
	seen_clear(&counter_app_seen);
	set_mode(MODE_NOMINAL);

	/* The rest of this major frame stays on the safe table: no slot 5. */
	sleep_to_slot(9);
	zassert_equal(counter_app_seen.len, 0);

	/* The nominal table starts with the next major frame. */
	sleep_to_slot(0);
	zassert_equal(counter_slots(slots, SEEN_MAX), 1);
	zassert_equal(slots[0], 0);
	zassert_equal(fm_hk().mode, MODE_NOMINAL);
}

ZTEST(frame_manager, test_safe_mode_takes_effect_at_once)
{
	uint8_t slots[SEEN_MAX];

	settle_in(MODE_NOMINAL);
	sleep_to_slot(1);
	seen_clear(&counter_app_seen);
	set_mode(MODE_SAFE);

	/* The safe table's slot 2 runs in this same major frame (DS-23). */
	sleep_to_slot(9);
	zassert_equal(counter_slots(slots, SEEN_MAX), 1);
	zassert_equal(slots[0], 2, "safe table, not the nominal slot 5");
	zassert_equal(counter_app_seen.ticks[0].count / FRAME_SLOTS,
		      (k_uptime_get() / FRAME_MINOR_MS) / FRAME_SLOTS);
}

ZTEST(frame_manager, test_overrun_bounds_pending_wakeups)
{
	struct frame_manager_hk before;
	struct frame_manager_hk after;
	uint32_t first_held;
	uint8_t slots[SEEN_MAX];

	settle_in(MODE_NOMINAL);
	before = fm_hk();
	stuck_app_hold(true);

	/*
	 * Five major frames, so five slot-1 wakeups for stuck_app. It takes
	 * the first and stalls in its step; the second waits in its queue.
	 * With FRAME_MAX_PENDING (2) outstanding, the frame manager skips the
	 * other three rather than queue copies it can't consume (DS-22).
	 */
	for (int major = 0; major < 5; major++) {
		sleep_to_slot(9);
	}
	first_held = (uint32_t)(k_uptime_get() / FRAME_MINOR_MS) - 4 * FRAME_SLOTS - 8;

	/* Other apps are unaffected. */
	zassert_equal(counter_slots(slots, SEEN_MAX), 10);

	/* The housekeeping published at the next slot 0 has the count. */
	sleep_to_slot(0);
	after = fm_hk();
	zassert_equal(after.overruns - before.overruns, 5 - FRAME_MAX_PENDING);
	zassert_equal(after.publish_errors, before.publish_errors, "the pool never ran out");

	/* Released, it finishes the tick it held and the one queued, then
	 * gets the next slot-1 wakeup as usual.
	 */
	stuck_app_hold(false);
	sleep_to_slot(2);
	zassert_equal(stuck_app_seen.len, FRAME_MAX_PENDING + 1);
	zassert_equal(stuck_app_seen.ticks[0].count, first_held);
	zassert_equal(stuck_app_seen.ticks[1].count, first_held + FRAME_SLOTS);
	zassert_equal(stuck_app_seen.ticks[2].slot, 1);
	zassert_equal(stuck_app_seen.ticks[2].count, k_uptime_get() / FRAME_MINOR_MS - 1);
}

ZTEST(frame_manager, test_rejected_commands)
{
	const struct frame_manager_cmd unknown = {.id = 99};
	struct app_status status_before;
	struct app_status status_after;
	struct frame_manager_hk hk_before;
	struct frame_manager_hk hk_after;
	uint8_t slots[SEEN_MAX];

	settle_in(MODE_NOMINAL);
	status_before = fm_status();
	hk_before = fm_hk();

	zassert_false(set_entry_enabled(MODE_MAX + 1, 0, false), "no such mode");
	zassert_false(set_entry_enabled(MODE_NOMINAL, NOMINAL_ENTRIES, false), "no such entry");
	zassert_false(set_entry_enabled(MODE_SAFE, SAFE_MODE_MANAGER_SLOT9, false),
		      "the mode manager is protected (DS-23)");
	zassert_false(send_command(&unknown), "no such command");
	/* Enabling a protected app's entry is allowed: it is already enabled. */
	zassert_true(set_entry_enabled(MODE_SAFE, SAFE_MODE_MANAGER_SLOT9, true));

	status_after = fm_status();
	zassert_equal(status_after.cmd_rejected - status_before.cmd_rejected, 4);
	zassert_equal(status_after.cmd_accepted - status_before.cmd_accepted, 1);

	sleep_to_slot(0);
	hk_after = fm_hk();
	zassert_equal(hk_after.rejected_bad_index - hk_before.rejected_bad_index, 2);
	zassert_equal(hk_after.rejected_protected - hk_before.rejected_protected, 1);
	zassert_equal(hk_after.rejected_bad_command - hk_before.rejected_bad_command, 1);

	/* Nothing changed: the nominal table still runs in full. */
	fresh_major_frames(1);
	zassert_equal(counter_slots(slots, SEEN_MAX), 2);
	zassert_equal(slots[0], 0);
	zassert_equal(slots[1], 5);
}

ZTEST(frame_manager, test_housekeeping_each_major_frame)
{
	struct frame_manager_hk first;
	struct frame_manager_hk second;

	settle_in(MODE_NOMINAL);
	sleep_to_slot(0);
	first = fm_hk();
	sleep_to_slot(0);
	second = fm_hk();

	zassert_equal(first.frame_count % FRAME_SLOTS, 0, "published in slot 0");
	zassert_equal(second.frame_count - first.frame_count, FRAME_SLOTS);
	zassert_equal(second.mode, MODE_NOMINAL);
}

ZTEST(frame_manager, test_no_drift_over_hours)
{
	struct frame_manager_hk before;

	settle_in(MODE_NOMINAL);
	before = fm_hk();
	k_sleep(K_HOURS(1));

	fresh_major_frames(1);
	zassert_equal(counter_app_seen.len, 2);
	for (uint32_t i = 0; i < counter_app_seen.len; i++) {
		const struct frame_tick *t = &counter_app_seen.ticks[i];
		int64_t offset = t->uptime_ms - (int64_t)t->count * FRAME_MINOR_MS;

		zassert_equal(offset, frame_offset_ms(), "drifted %lld ms over an hour",
			      offset - frame_offset_ms());
	}
	sleep_to_slot(0);
	zassert_equal(fm_hk().missed_frames, before.missed_frames);
}

ZTEST(frame_manager, test_missed_frames_are_skipped)
{
	struct frame_manager_hk before;
	uint8_t slots[SEEN_MAX];
	const struct frame_tick *late;

	settle_in(MODE_NOMINAL);
	before = fm_hk();

	/*
	 * The test thread is cooperative, so while it busy-waits the frame
	 * manager cannot run. From the middle of slot 9, 200 ms covers the
	 * starts of slot 0 and slot 1, with half a frame to spare at each
	 * end: the frame manager falls one frame behind. It skips slot 0 and
	 * handles slot 1, so the slot stays in step with time.
	 */
	k_busy_wait(200 * USEC_PER_MSEC);
	sleep_to_slot(9);

	zassert_equal(counter_slots(slots, SEEN_MAX), 1);
	zassert_equal(slots[0], 5, "slot 0 was skipped");
	zassert_equal(stuck_app_seen.len, 1);
	late = &stuck_app_seen.ticks[0];
	zassert_equal(late->slot, 1);
	zassert_equal(late->slot, late->count % FRAME_SLOTS);
	zassert_true(late->uptime_ms > (int64_t)late->count * FRAME_MINOR_MS + frame_offset_ms(),
		     "the late tick carries the time it was actually sent");

	sleep_to_slot(0);
	zassert_equal(fm_hk().missed_frames - before.missed_frames, 1);

	/* Afterwards, frames start on time again. */
	fresh_major_frames(1);
	zassert_equal(counter_app_seen.ticks[0].uptime_ms,
		      (int64_t)counter_app_seen.ticks[0].count * FRAME_MINOR_MS + frame_offset_ms());
}

ZTEST(frame_manager, test_frame_table_check)
{
	struct frame_entry entry = frame_tables[MODE_NOMINAL].entries[0];
	struct frame_table table = {.entries = &entry, .len = 1};

	for (uint8_t mode = 0; mode <= MODE_MAX; mode++) {
		zassert_ok(frame_table_check(&frame_tables[mode]));
	}
	zassert_ok(frame_table_check(&table));

	entry.slot = FRAME_SLOTS;
	zassert_equal(frame_table_check(&table), -EINVAL, "slot out of range");
	entry = frame_tables[MODE_NOMINAL].entries[0];

	entry.app = APP_ID_MAX + 1;
	zassert_equal(frame_table_check(&table), -EINVAL, "unknown app");
	entry = frame_tables[MODE_NOMINAL].entries[0];

	entry.status_chan = NULL;
	zassert_equal(frame_table_check(&table), -EINVAL, "missing channel");
	entry = frame_tables[MODE_NOMINAL].entries[0];

	table.len = FRAME_ENTRIES_MAX + 1;
	zassert_equal(frame_table_check(&table), -EINVAL, "too many entries");

	table.entries = NULL;
	table.len = 1;
	zassert_equal(frame_table_check(&table), -EINVAL, "no entries array");
}

/* ---- The frame report and set_app_enabled (DS-43) ----------------------- */

static struct frame_report frame_report(void)
{
	struct frame_report r;

	zassert_ok(zbus_chan_read(&frame_report_chan, &r, K_MSEC(10)));
	return r;
}

static bool set_app_enabled(uint8_t app, bool enabled)
{
	const struct frame_manager_cmd cmd = {
		.id = FRAME_MANAGER_CMD_SET_APP_ENABLED,
		.args.set_app_enabled = {.app = app, .enabled = enabled},
	};

	return send_command(&cmd);
}

#define STUCK_APP BIT64(APP_ID_STUCK_APP)
#define COUNTER_APP BIT64(APP_ID_COUNTER_APP)

/*
 * The test apps' bits only. The safe table's mode manager entry never
 * steps here (no mode manager runs in this test), so its bit is set too.
 */
#define TEST_APPS (STUCK_APP | COUNTER_APP)

ZTEST(frame_manager, test_report_shows_an_app_a_major_frame_behind)
{
	struct frame_report r;

	settle_in(MODE_NOMINAL);
	sleep_to_slot(0);
	r = frame_report();
	zassert_equal(r.stuck & TEST_APPS, 0, "both test apps are keeping up");
	zassert_equal(r.major_frame, fm_hk().frame_count / FRAME_SLOTS);

	/* stuck_app takes its slot-1 wakeup and never finishes it. */
	stuck_app_hold(true);
	sleep_to_slot(0);
	zassert_equal(frame_report().stuck & STUCK_APP, 0,
		      "it has had less than a major frame to finish");
	sleep_to_slot(0);
	r = frame_report();
	zassert_equal(r.stuck & TEST_APPS, STUCK_APP,
		      "a whole major frame behind; counter_app is not");

	/* Released, it catches up, and the next report clears it. */
	stuck_app_hold(false);
	sleep_to_slot(0);
	sleep_to_slot(0);
	zassert_equal(frame_report().stuck & TEST_APPS, 0);
}

ZTEST(frame_manager, test_set_app_enabled_stops_every_wakeup)
{
	settle_in(MODE_NOMINAL);
	zassert_true(set_app_enabled(APP_ID_COUNTER_APP, false));
	fresh_major_frames(1);
	zassert_equal(counter_app_seen.len, 0, "neither of its two nominal entries runs");
	zassert_equal(frame_report().disabled, COUNTER_APP);
	zassert_equal(fm_hk().disabled_apps, COUNTER_APP);

	/* Stopped in every mode, not just the current one. */
	settle_in(MODE_SAFE);
	fresh_major_frames(1);
	zassert_equal(counter_app_seen.len, 0);

	zassert_true(set_app_enabled(APP_ID_COUNTER_APP, true));
	fresh_major_frames(1);
	zassert_equal(counter_app_seen.len, 1, "its safe-table entry runs again");
	sleep_to_slot(0);
	zassert_equal(frame_report().disabled, 0);
}

ZTEST(frame_manager, test_set_app_enabled_refuses_a_protected_app)
{
	uint32_t rejected = fm_hk().rejected_protected;

	zassert_false(set_app_enabled(APP_ID_MODE_MANAGER, false));
	zassert_true(set_app_enabled(APP_ID_MODE_MANAGER, true), "starting it is harmless");
	sleep_to_slot(0);
	zassert_equal(fm_hk().rejected_protected - rejected, 1);
	zassert_equal(frame_report().disabled, 0);
}

ZTEST(frame_manager, test_set_app_enabled_refuses_an_app_in_no_table)
{
	uint32_t rejected = fm_hk().rejected_bad_index;

	zassert_false(set_app_enabled(APP_ID_RADIO, false), "not in the test tables");
	zassert_false(set_app_enabled(APP_ID_MAX + 1, false), "no such app");
	sleep_to_slot(0);
	zassert_equal(fm_hk().rejected_bad_index - rejected, 2);
}
