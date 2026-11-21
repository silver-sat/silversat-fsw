/*
 * Counting log lines in app tests. See log_capture.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include <zephyr/logging/log_backend.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/logging/log_output.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include "log_capture.h"

static const char *wanted;
static atomic_t count;

/* The line being written. A longer line is cut short; only its start is checked. */
static char line[200];
static size_t line_len;

/*
 * log_output hands over a line in pieces. Collect them, and check each
 * line when it ends. Only one thread writes log lines out at a time (the
 * log thread, or in immediate mode the caller, under the logger's lock).
 */
static int collect(uint8_t *data, size_t length, void *ctx)
{
	ARG_UNUSED(ctx);

	for (size_t i = 0; i < length; i++) {
		char c = (char)data[i];

		if (c == '\n') {
			line[line_len] = '\0';
			if (wanted != NULL && strstr(line, wanted) != NULL) {
				atomic_inc(&count);
			}
			line_len = 0;
		} else if (c != '\r' && line_len < sizeof(line) - 1) {
			line[line_len++] = c;
		}
	}
	return (int)length;
}

static uint8_t output_buf[64];
LOG_OUTPUT_DEFINE(capture_output, collect, output_buf, sizeof(output_buf));

static void capture_process(const struct log_backend *const backend, union log_msg_generic *msg)
{
	ARG_UNUSED(backend);

	/* Flags 0: no timestamp or level, so a line reads "health: alive". */
	log_output_msg_process(&capture_output, &msg->log, 0);
}

static void capture_panic(const struct log_backend *const backend)
{
	ARG_UNUSED(backend);
	log_output_flush(&capture_output);
}

static const struct log_backend_api capture_api = {
	.process = capture_process,
	.panic = capture_panic,
};

LOG_BACKEND_DEFINE(log_capture_backend, capture_api, true);

void log_capture_start(const char *text)
{
	log_flush(); /* so lines logged before now are never counted */
	wanted = text;
	atomic_set(&count, 0);
}

uint32_t log_capture_count(void)
{
	log_flush(); /* lines already logged, but not yet written out */
	return (uint32_t)atomic_get(&count);
}
