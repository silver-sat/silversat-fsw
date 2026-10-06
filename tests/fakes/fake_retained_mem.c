/*
 * A test-only retained memory (silversat,fake-retained-mem): RAM behind
 * Zephyr's retained memory API, with controls to fail it. It stands in for
 * the STM32's backup SRAM, the FRAM service's mirror.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT silversat_fake_retained_mem

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/retained_mem.h>
#include <zephyr/kernel.h>

#include "fake_retained_mem.h"

#define FAKE_SIZE DT_INST_PROP(0, size)

struct fake_retained_mem_data {
	uint8_t memory[FAKE_SIZE];
	bool failing;
};

static ssize_t fake_size(const struct device *dev)
{
	ARG_UNUSED(dev);
	return FAKE_SIZE;
}

static int fake_read(const struct device *dev, off_t offset, uint8_t *buffer, size_t size)
{
	struct fake_retained_mem_data *data = dev->data;

	if (data->failing) {
		return -EIO;
	}
	if (offset < 0 || offset + size > FAKE_SIZE) {
		return -EINVAL;
	}
	memcpy(buffer, &data->memory[offset], size);
	return 0;
}

static int fake_write(const struct device *dev, off_t offset, const uint8_t *buffer,
		      size_t size)
{
	struct fake_retained_mem_data *data = dev->data;

	if (data->failing) {
		return -EIO;
	}
	if (offset < 0 || offset + size > FAKE_SIZE) {
		return -EINVAL;
	}
	memcpy(&data->memory[offset], buffer, size);
	return 0;
}

static int fake_clear(const struct device *dev)
{
	struct fake_retained_mem_data *data = dev->data;

	memset(data->memory, 0, FAKE_SIZE);
	return 0;
}

void fake_retained_mem_reset(const struct device *dev)
{
	struct fake_retained_mem_data *data = dev->data;

	memset(data, 0, sizeof(*data));
}

void fake_retained_mem_fail(const struct device *dev, bool failing)
{
	((struct fake_retained_mem_data *)dev->data)->failing = failing;
}

uint8_t *fake_retained_mem_memory(const struct device *dev)
{
	return ((struct fake_retained_mem_data *)dev->data)->memory;
}

static DEVICE_API(retained_mem, fake_retained_mem_api) = {
	.size = fake_size,
	.read = fake_read,
	.write = fake_write,
	.clear = fake_clear,
};

static struct fake_retained_mem_data fake_retained_mem_data_0;

DEVICE_DT_INST_DEFINE(0, NULL, NULL, &fake_retained_mem_data_0, NULL, POST_KERNEL,
		      CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &fake_retained_mem_api);
