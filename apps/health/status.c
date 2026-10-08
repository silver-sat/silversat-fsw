/*
 * Health's development status line (DS-06, DS-44).
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * On the bench, with only a USB cable, the console is the one window into
 * the spacecraft: telemetry goes out on the radio UART. So in a
 * development build health logs one line every
 * CONFIG_SS_HEALTH_STATUS_SECONDS with what matters most: the boot, MET,
 * the mode and why, the least stack any app has left, stalls, short runs
 * in a row (a reset loop, DS-44), and the apps health has stopped. The
 * flight build leaves it out (CONFIG_SS_HEALTH_STATUS_LOG), and nothing
 * depends on it.
 */

#include <stdio.h>

#include "status.h"

bool status_due(uint32_t major_frame, uint32_t period)
{
	return major_frame >= 2 && (major_frame - 2) % period == 0;
}

/* A name, or "?" for a value without one. */
static const char *or_unknown(const char *name)
{
	return name != NULL ? name : "?";
}

int status_format(char *buf, size_t size, const struct health_hk *hk,
		  const struct mode_state *mode, int64_t met_ms)
{
	char stack[48];

	if (hk->stack_apps > 0) {
		snprintf(stack, sizeof(stack), "least stack %u B (%s)",
			 (unsigned int)hk->stack_min_unused, or_unknown(app_name(hk->stack_min_app)));
	} else {
		snprintf(stack, sizeof(stack), "no app stacks found");
	}
	return snprintf(buf, size,
			"boot %u, MET %lld s, mode %s (%s), %s%s, stalls %u, short runs %u, "
			"stopped 0x%llx",
			(unsigned int)hk->boot_number, (long long)(met_ms / 1000),
			or_unknown(mode_name(mode->mode)), or_unknown(mode_reason_name(mode->reason)),
			stack, hk->stack_low ? " LOW" : "", (unsigned int)hk->stalls,
			(unsigned int)hk->short_runs, (unsigned long long)hk->disabled_by_health);
}
