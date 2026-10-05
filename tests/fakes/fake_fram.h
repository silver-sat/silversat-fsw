/*
 * A test-only FRAM: RAM behind Zephyr's EEPROM API, with fault controls.
 * See fake_fram.c. A test adds tests/fakes to DTS_ROOT and its sources,
 * and points the fram alias at a silversat,fake-fram node.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SILVERSAT_TESTS_FAKE_FRAM_H_
#define SILVERSAT_TESTS_FAKE_FRAM_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>

/* All zeros, no faults: a new part. */
void fake_fram_reset(const struct device *dev);

/* While failing, every read and write returns -EIO, as a dead part would. */
void fake_fram_fail(const struct device *dev, bool failing);

/*
 * Power fails during the next write: only its first keep bytes are stored,
 * and the write returns -EIO (the software never sees it finish).
 */
void fake_fram_tear_next_write(const struct device *dev, size_t keep);

/* While dropping, writes report success but store nothing. */
void fake_fram_drop_writes(const struct device *dev, bool dropping);

/* The memory itself, to corrupt or inspect. */
uint8_t *fake_fram_memory(const struct device *dev);

#endif /* SILVERSAT_TESTS_FAKE_FRAM_H_ */
