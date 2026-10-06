/*
 * After a reset: each app picks up where it left off (DS-41, DS-42, DS-46,
 * DS-53, DS-54, DS-74, DS-75).
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Before any app's thread starts, seed() writes the records the apps would
 * have stored before a reset: a deployment that has ended, safe mode
 * entered for command loss, key slot 1 in use with raised floors, a ground
 * contact, and transmission stopped. The tests then check that each app
 * starts from them.
 *
 * Built with FRAM_FAILED (testcase.yaml), FRAM then fails before the apps
 * start, so every record must come from the mirror, the backup SRAM: what
 * the Nucleo does until it has an FRAM part.
 */

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/ztest.h>

#include "fake_fram.h"
#include "msg/command_ingest.h"
#include "msg/common.h"
#include "msg/radio.h"
#include "nvm/command_ingest.h"
#include "nvm/mode_manager.h"
#include "nvm/radio.h"
#include "silversat/cmd_counter.h"
#include "silversat/nvm.h"
#include "silversat/resource_map.h"

#define FLOOR_SLOT0 1790812800000ULL /* 2026-10-01 */
#define FLOOR_SLOT1 1790812900000ULL

static int seed(void)
{
	const struct nvm_deployment deployment = {.complete = true};
	const struct nvm_mode_state mode = {.mode = MODE_SAFE, .reason = MODE_REASON_COMMAND_LOSS};
	const struct nvm_command_state command = {
		.floor_slot0 = FLOOR_SLOT0,
		.floor_slot1 = FLOOR_SLOT1,
		.active_slot = 1,
		.contacted = true,
		.last_accepted_met_ms = 123456,
	};
	const struct nvm_radio_state radio = {.transmit_enabled = false};

	(void)nvm_deployment_write(&deployment);
	(void)nvm_mode_state_write(&mode);
	(void)nvm_command_state_write(&command);
	(void)nvm_radio_state_write(&radio);
#if defined(FRAM_FAILED)
	fake_fram_fail(DEVICE_DT_GET(FRAM_NODE), true);
	(void)nvm_retry();
#endif
	return 0;
}

/* After the FRAM service starts (priority 90), before any app's thread. */
SYS_INIT(seed, APPLICATION, 95);

static void *setup(void)
{
	k_sleep(K_MSEC(10)); /* let the apps start */
#if defined(FRAM_FAILED)
	zassert_false(nvm_available());
#else
	zassert_true(nvm_available());
#endif
	return NULL;
}

ZTEST_SUITE(after_reset, NULL, setup, NULL, NULL, NULL);

ZTEST(after_reset, test_safe_mode_keeps_its_reason)
{
	struct mode_state state;

	/* Deployment has ended, so not deploy; and safe for command loss still. */
	zassert_ok(zbus_chan_read(&mode_chan, &state, K_MSEC(10)));
	zassert_equal(state.mode, MODE_SAFE);
	zassert_equal(state.reason, MODE_REASON_COMMAND_LOSS);
}

ZTEST(after_reset, test_the_key_slot_and_floors_come_back)
{
	struct command_ingest_hk hk;
	uint64_t floor;

	zassert_ok(zbus_chan_read(&command_ingest_hk_chan, &hk, K_MSEC(10)));
	zassert_equal(hk.active_slot, 1, "the rotation survived the reset");
	zassert_ok(floor_store_get(0, &floor));
	zassert_equal(floor, FLOOR_SLOT0, "a command recorded before the reset is still a replay");
	zassert_ok(floor_store_get(1, &floor));
	zassert_equal(floor, FLOOR_SLOT1);
}

ZTEST(after_reset, test_the_command_loss_timer_keeps_running)
{
	struct ground_contact contact;

	/*
	 * The ground was heard from before the reset, and MET carries on
	 * across it (DS-25), so the timer runs on from that contact.
	 */
	zassert_ok(zbus_chan_read(&ground_contact_chan, &contact, K_MSEC(10)));
	zassert_true(contact.contacted);
	zassert_equal(contact.last_accepted_met_ms, 123456);
}

ZTEST(after_reset, test_transmission_stays_stopped)
{
	struct radio_hk hk;

	zassert_ok(zbus_chan_read(&radio_hk_chan, &hk, K_MSEC(10)));
	zassert_false(hk.transmit_enabled, "the ground stopped it; a reset doesn't start it");
}
