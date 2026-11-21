/*
 * Counting log lines in app tests (tests/app/).
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Some things an app does are only seen on the console: health's heartbeat,
 * for example. To test them, this adds a second log backend beside the
 * console, which writes each line as the console would, but without the
 * timestamp or level ("health: alive"), and counts the lines that contain
 * the text a test asks for.
 *
 * Works with deferred logging (the default) or immediate: log_capture_count
 * first waits for the log thread to write out every line already logged.
 */

#ifndef SILVERSAT_TESTS_LOG_CAPTURE_H_
#define SILVERSAT_TESTS_LOG_CAPTURE_H_

#include <stdint.h>

/*
 * Start counting, from zero, the log lines that contain text. text must
 * stay valid until the next call (a string literal is fine).
 */
void log_capture_start(const char *text);

/* How many lines containing text have been logged since log_capture_start. */
uint32_t log_capture_count(void);

#endif /* SILVERSAT_TESTS_LOG_CAPTURE_H_ */
