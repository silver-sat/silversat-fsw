/*
 * A test-only temperature sensor (silversat,fake-temperature), behind
 * Zephyr's sensor API, with controls a test uses to set the reading and to
 * make the sensor fail.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT silversat_fake_temperature

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>

#include "fake_temperature.h"

struct fake_temperature_data {
	int32_t millidegrees;
	enum sensor_channel channel;
	bool failing;
	uint32_t fetches;
};

static int fake_sample_fetch(const struct device *dev, enum sensor_channel chan)
{
	struct fake_temperature_data *data = dev->data;

	ARG_UNUSED(chan);
	data->fetches++;
	return data->failing ? -EIO : 0;
}

static int fake_channel_get(const struct device *dev, enum sensor_channel chan,
			    struct sensor_value *val)
{
	struct fake_temperature_data *data = dev->data;

	if (chan != data->channel) {
		return -ENOTSUP;
	}
	return sensor_value_from_milli(val, data->millidegrees);
}

void fake_temperature_set_mc(const struct device *dev, int32_t millidegrees)
{
	((struct fake_temperature_data *)dev->data)->millidegrees = millidegrees;
}

void fake_temperature_answer_on(const struct device *dev, enum sensor_channel chan)
{
	((struct fake_temperature_data *)dev->data)->channel = chan;
}

void fake_temperature_fail(const struct device *dev, bool failing)
{
	((struct fake_temperature_data *)dev->data)->failing = failing;
}

uint32_t fake_temperature_fetches(const struct device *dev)
{
	return ((struct fake_temperature_data *)dev->data)->fetches;
}

void fake_temperature_reset(const struct device *dev)
{
	memset(dev->data, 0, sizeof(struct fake_temperature_data));
	fake_temperature_answer_on(dev, SENSOR_CHAN_AMBIENT_TEMP);
}

static DEVICE_API(sensor, fake_temperature_api) = {
	.sample_fetch = fake_sample_fetch,
	.channel_get = fake_channel_get,
};

static struct fake_temperature_data fake_temperature_data_0;

DEVICE_DT_INST_DEFINE(0, NULL, NULL, &fake_temperature_data_0, NULL, POST_KERNEL,
		      CONFIG_SENSOR_INIT_PRIORITY, &fake_temperature_api);
