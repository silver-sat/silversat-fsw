/*
 * Health's boot work and run checkpoint (DS-25, DS-44, DS-71, DS-72).
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * At boot, before any app's thread starts, health:
 *
 *   1. reads the run checkpoint that the last run wrote once a major frame
 *      (FRAM, and the mirror): its boot number, MET, and uptime;
 *   2. takes the next boot number, and carries MET on from the checkpoint,
 *      so MET counts the whole mission, not just this boot (DS-25);
 *   3. reads why the last run ended (Zephyr's hwinfo reset cause) and clears
 *      it for next time;
 *   4. writes this boot's entry in the boot log, a ring of the last 16
 *      boots, with the last run's length and any app that caused a health
 *      reset (DS-44);
 *   5. counts the short runs in a row up to this boot, from the boot log:
 *      runs that ended within CONFIG_SS_RESET_LOOP_RUN_MINUTES. Health's
 *      thread acts on the count (a reset loop, DS-44);
 *   6. publishes mission_time_chan, so the frame manager's first tick
 *      already carries the right MET.
 *
 * Then, once a major frame, health stores a new checkpoint. A reset loses
 * at most a major frame of MET (the time while the spacecraft is off isn't
 * counted; that needs the RTC, DS-72).
 *
 * With neither FRAM nor the mirror, every record reads as zero: each boot
 * is boot 1 at MET 0, and the boot log is lost (DS-75), so no reset loop
 * can be seen.
 */

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/drivers/hwinfo.h>
#include <zephyr/init.h>
#include <zephyr/zbus/zbus.h>

#include "boot.h"
#include "msg/common.h"
#include "nvm/health.h"

#define SHORT_RUN_MS ((int64_t)CONFIG_SS_RESET_LOOP_RUN_MINUTES * 60 * 1000)

BUILD_ASSERT(CONFIG_SS_RESET_LOOP_SAFE_RUNS <= CONFIG_SS_RESET_LOOP_STOP_RUNS,
	     "health asks for safe mode before it stops apps");

/* The checkpoint as this run last stored it. Only health uses it. */
static struct nvm_run_checkpoint checkpoint;
static uint32_t reset_cause;
static uint32_t failures;
static uint8_t short_runs;
static int64_t met_at_boot_ms;

static void store(void)
{
	if (nvm_run_checkpoint_write(&checkpoint) != 0) {
		failures++;
	}
}

/* A run that ended within CONFIG_SS_RESET_LOOP_RUN_MINUTES of its boot. */
static bool is_short(int64_t run_ms)
{
	return run_ms < SHORT_RUN_MS;
}

/*
 * Was the run before boot `number` short? Its boot log entry says. Boot 1
 * had no run before it, and a missing entry (overwritten, or lost with
 * FRAM) counts as not short: health never acts on a loop it can't see.
 */
static bool short_run_before(uint32_t number)
{
	struct nvm_boot_log entry;
	uint32_t found;

	if (number < 2 ||
	    nvm_boot_log_read(number % nvm_boot_log_region.ring, &entry, &found) != 0 ||
	    found != number) {
		return false;
	}
	return is_short(entry.previous_run_ms);
}

/*
 * Short runs in a row up to this boot, counting back through the boot log
 * from this boot's own entry, up to CONFIG_SS_RESET_LOOP_STOP_RUNS (no
 * response needs more).
 */
static uint8_t count_short_runs(uint32_t number)
{
	uint8_t runs = 0;

	while (runs < CONFIG_SS_RESET_LOOP_STOP_RUNS && short_run_before(number - runs)) {
		runs++;
	}
	return runs;
}

static int health_boot(void)
{
	struct nvm_run_checkpoint last;
	struct nvm_boot_log entry;
	uint32_t cause;

	/* All zeros if there is none: then this is boot 1, at MET 0. */
	(void)nvm_run_checkpoint_read(&last);

	if (hwinfo_get_reset_cause(&cause) == 0) {
		(void)hwinfo_clear_reset_cause();
	} else {
		cause = 0; /* the board can't say */
	}
	reset_cause = cause;

	checkpoint = (struct nvm_run_checkpoint){
		.boot_number = last.boot_number + 1,
		.met_ms = last.met_ms,
		.uptime_ms = 0,
		.reset_app = 0,
	};
	entry = (struct nvm_boot_log){
		.reset_cause = cause,
		.previous_run_ms = last.uptime_ms,
		.met_at_boot_ms = last.met_ms,
		.reset_app = last.reset_app,
	};
	if (nvm_boot_log_write(checkpoint.boot_number, &entry) != 0) {
		failures++;
	}
	/* Stored at once, so a reset before the first checkpoint still counts this boot. */
	store();
	short_runs = count_short_runs(checkpoint.boot_number);
	met_at_boot_ms = checkpoint.met_ms;

	{
		const struct mission_time time = {
			.boot_number = checkpoint.boot_number,
			.met_at_boot_ms = checkpoint.met_ms,
		};

		zbus_chan_pub(&mission_time_chan, &time, K_NO_WAIT);
	}
	return 0;
}

/* After the FRAM service starts (priority 90), before any app's thread. */
SYS_INIT(health_boot, APPLICATION, 95);

uint32_t boot_number(void)
{
	return checkpoint.boot_number;
}

uint32_t boot_reset_cause(void)
{
	return reset_cause;
}

int64_t boot_met_ms(void)
{
	return met_at_boot_ms;
}

uint8_t boot_short_runs(void)
{
	return short_runs;
}

void boot_checkpoint(int64_t met_ms, int64_t uptime_ms)
{
	checkpoint.met_ms = met_ms;
	checkpoint.uptime_ms = uptime_ms;
	store();
}

void boot_record_reset_app(uint8_t app)
{
	/* Stored by the checkpoint health writes later in the same wakeup. */
	checkpoint.reset_app = app;
}

uint32_t boot_store_failures(void)
{
	return failures;
}
