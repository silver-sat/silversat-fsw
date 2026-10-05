/*
 * A test-only watchdog driver (silversat,fake-watchdog): it records what
 * health asks of it and counts feeds, and never resets. native_sim has no
 * watchdog that a test could survive.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT silversat_fake_watchdog

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/kernel.h>

#include "fake_watchdog.h"

struct fake_watchdog_data {
	struct k_spinlock lock;
	struct fake_watchdog_state state;
	bool installed;
};

static int fake_setup(const struct device *dev, uint8_t options)
{
	struct fake_watchdog_data *data = dev->data;
	k_spinlock_key_t key = k_spin_lock(&data->lock);

	data->state.options = options;
	data->state.started = data->installed;
	k_spin_unlock(&data->lock, key);
	return data->installed ? 0 : -EINVAL;
}

static int fake_disable(const struct device *dev)
{
	ARG_UNUSED(dev);
	return -EPERM; /* like the STM32 IWDG: once started, it can't be stopped */
}

static int fake_install_timeout(const struct device *dev, const struct wdt_timeout_cfg *cfg)
{
	struct fake_watchdog_data *data = dev->data;
	k_spinlock_key_t key = k_spin_lock(&data->lock);

	data->state.timeout_ms = cfg->window.max;
	data->state.flags = cfg->flags;
	data->installed = true;
	k_spin_unlock(&data->lock, key);
	return 0; /* channel 0 */
}

static int fake_feed(const struct device *dev, int channel_id)
{
	struct fake_watchdog_data *data = dev->data;
	k_spinlock_key_t key;

	if (channel_id != 0 || !data->state.started) {
		return -EINVAL;
	}
	key = k_spin_lock(&data->lock);
	data->state.feeds++;
	k_spin_unlock(&data->lock, key);
	return 0;
}

struct fake_watchdog_state fake_watchdog_state(const struct device *dev)
{
	struct fake_watchdog_data *data = dev->data;
	k_spinlock_key_t key = k_spin_lock(&data->lock);
	struct fake_watchdog_state state = data->state;

	k_spin_unlock(&data->lock, key);
	return state;
}

static DEVICE_API(wdt, fake_watchdog_api) = {
	.setup = fake_setup,
	.disable = fake_disable,
	.install_timeout = fake_install_timeout,
	.feed = fake_feed,
};

static struct fake_watchdog_data fake_watchdog_data_0;

DEVICE_DT_INST_DEFINE(0, NULL, NULL, &fake_watchdog_data_0, NULL, POST_KERNEL,
		      CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &fake_watchdog_api);
