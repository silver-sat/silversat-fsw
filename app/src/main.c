/*
 * SilverSat flight software -- application entry point.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Deliberately minimal. Keep this file boring: it should do nothing but
 * start the subsystems that live under drivers/ and lib/. Logic that is
 * worth testing does not belong here, because main() is the one place
 * twister cannot reach.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(silversat, LOG_LEVEL_INF);

int main(void)
{
	LOG_INF("SilverSat FSW starting on %s", CONFIG_BOARD_TARGET);

	while (1) {
		k_sleep(K_SECONDS(10));
		LOG_INF("alive");
	}

	return 0;
}
