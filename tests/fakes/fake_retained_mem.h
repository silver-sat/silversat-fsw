/*
 * A test-only retained memory, standing in for the backup SRAM that the
 * FRAM service mirrors records into. See fake_retained_mem.c.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SILVERSAT_TESTS_FAKE_RETAINED_MEM_H_
#define SILVERSAT_TESTS_FAKE_RETAINED_MEM_H_

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#include <zephyr/device.h>

/* All zeros, no faults. */
void fake_retained_mem_reset(const struct device *dev);

/* While failing, every read and write returns -EIO. */
void fake_retained_mem_fail(const struct device *dev, bool failing);

/* Report this size instead of the real one, as a smaller memory would; 0 undoes it. */
void fake_retained_mem_report_size(const struct device *dev, ssize_t size);

/* The memory itself, to corrupt or inspect. */
uint8_t *fake_retained_mem_memory(const struct device *dev);

#endif /* SILVERSAT_TESTS_FAKE_RETAINED_MEM_H_ */
