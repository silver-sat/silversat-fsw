/*
 * A test-only FRAM (silversat,fake-fram): RAM behind Zephyr's EEPROM API,
 * with controls a test uses to fail it, tear a write as a power failure
 * would, or make it drop writes silently.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT silversat_fake_fram

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/eeprom.h>
#include <zephyr/kernel.h>

#include "fake_fram.h"

#define FAKE_FRAM_SIZE DT_INST_PROP(0, size)

struct fake_fram_data {
	uint8_t memory[FAKE_FRAM_SIZE];
	bool failing;
	bool dropping;
	bool tearing;
	size_t keep;
};

static int fake_read(const struct device *dev, off_t offset, void *buf, size_t len)
{
	struct fake_fram_data *data = dev->data;

	if (data->failing) {
		return -EIO;
	}
	if (offset < 0 || offset + len > FAKE_FRAM_SIZE) {
		return -EINVAL;
	}
	memcpy(buf, &data->memory[offset], len);
	return 0;
}

static int fake_write(const struct device *dev, off_t offset, const void *buf, size_t len)
{
	struct fake_fram_data *data = dev->data;

	if (data->failing) {
		return -EIO;
	}
	if (offset < 0 || offset + len > FAKE_FRAM_SIZE) {
		return -EINVAL;
	}
	if (data->tearing) {
		data->tearing = false;
		memcpy(&data->memory[offset], buf, MIN(len, data->keep));
		return -EIO;
	}
	if (!data->dropping) {
		memcpy(&data->memory[offset], buf, len);
	}
	return 0;
}

static size_t fake_size(const struct device *dev)
{
	ARG_UNUSED(dev);
	return FAKE_FRAM_SIZE;
}

void fake_fram_reset(const struct device *dev)
{
	struct fake_fram_data *data = dev->data;

	memset(data, 0, sizeof(*data));
}

void fake_fram_fail(const struct device *dev, bool failing)
{
	((struct fake_fram_data *)dev->data)->failing = failing;
}

void fake_fram_tear_next_write(const struct device *dev, size_t keep)
{
	struct fake_fram_data *data = dev->data;

	data->tearing = true;
	data->keep = keep;
}

void fake_fram_drop_writes(const struct device *dev, bool dropping)
{
	((struct fake_fram_data *)dev->data)->dropping = dropping;
}

uint8_t *fake_fram_memory(const struct device *dev)
{
	return ((struct fake_fram_data *)dev->data)->memory;
}

static DEVICE_API(eeprom, fake_fram_api) = {
	.read = fake_read,
	.write = fake_write,
	.size = fake_size,
};

static struct fake_fram_data fake_fram_data_0;

DEVICE_DT_INST_DEFINE(0, NULL, NULL, &fake_fram_data_0, NULL, POST_KERNEL,
		      CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &fake_fram_api);
