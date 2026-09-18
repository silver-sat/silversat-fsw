/*
 * Magnetometer driver tests (scaffold).
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ---------------------------------------------------------------------
 * THIS FILE IS A WALKING SKELETON. Its only job is to give twister
 * something real to build and run so the CI pipeline can be proven before
 * any driver exists. DELETE test_devicetree_wiring and replace it with
 * actual tests in the first magnetometer pull request.
 *
 * A tautological test that survives to the end of the project is worse
 * than no test at all: it turns a green check into a lie.
 * ---------------------------------------------------------------------
 */

#include <zephyr/ztest.h>
#include <zephyr/devicetree.h>

ZTEST_SUITE(mag, NULL, NULL, NULL, NULL, NULL);

/*
 * Checks that the emulated bus overlay is actually being applied. This is
 * a build-time assertion in test's clothing -- if boards/native_sim.overlay
 * is not picked up, DT_NODE_EXISTS is false and this fails loudly rather
 * than the test silently passing against a nonexistent device.
 */
ZTEST(mag, test_devicetree_wiring)
{
	zassert_true(DT_NODE_EXISTS(DT_NODELABEL(mag)),
		     "mag node missing -- is boards/native_sim.overlay applied?");

	zassert_true(DT_NODE_HAS_COMPAT(DT_NODELABEL(mag), silversat_mag),
		     "mag node has the wrong compatible");
}
