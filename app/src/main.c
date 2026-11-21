/*
 * SilverSat flight software -- application entry point.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Nothing is started here. By the time main() runs, everything already is:
 *
 *   - drivers, by Zephyr's device model, from devicetree;
 *   - boot services, from SYS_INIT: the FRAM service and health's boot
 *     work (the boot number, MET, the boot log);
 *   - every app, as a thread from K_THREAD_DEFINE, chosen by its Kconfig
 *     symbol in prj.conf (DS-10).
 *
 * main() logs one line and returns; the main thread then exits. The line
 * matters: the flatsat check looks for it after flashing. The console
 * heartbeat ("alive") comes from health, once the frame manager is running.
 *
 * The main thread's stack still matters, even though main() does nothing:
 * the SYS_INIT boot work runs on it, before main() is called. See
 * CONFIG_MAIN_STACK_SIZE in prj.conf.
 *
 * Logic that is worth testing does not belong here, because main() is the
 * one place twister cannot reach.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(silversat, LOG_LEVEL_INF);

int main(void)
{
	LOG_INF("SilverSat FSW starting on %s", CONFIG_BOARD_TARGET);
	return 0;
}
