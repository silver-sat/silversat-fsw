/*
 * A test-only temperature sensor, with controls. See fake_temperature.c. A
 * test adds tests/fakes to DTS_ROOT and its sources, and points the
 * temperature-sensor alias at a silversat,fake-temperature node.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SILVERSAT_TESTS_FAKE_TEMPERATURE_H_
#define SILVERSAT_TESTS_FAKE_TEMPERATURE_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>

/* What the next readings return, in millidegrees Celsius. */
void fake_temperature_set_mc(const struct device *dev, int32_t millidegrees);

/* While failing, fetching a sample returns -EIO, as a dead sensor would. */
void fake_temperature_fail(const struct device *dev, bool failing);

/* Samples fetched so far, whether or not they worked. */
uint32_t fake_temperature_fetches(const struct device *dev);

/* Working, reading 0, with its count of fetches cleared. */
void fake_temperature_reset(const struct device *dev);

#endif /* SILVERSAT_TESTS_FAKE_TEMPERATURE_H_ */
