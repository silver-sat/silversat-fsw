/*
 * nvm app tests (DS-70, DS-74, DS-75).
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The nvm app runs as in flight, on the fake FRAM and mirror
 * (tests/fakes/), with the flight region map (nvm/map.h). The tests stand
 * in for the frame manager (waking it), for command ingest (sending it
 * commands), and for the radio (taking its dumps off downlink_msgq).
 *
 * The tests write regions they don't own through the FRAM service, to
 * have records to scrub and dump; flight code only ever writes its own
 * (DS-74).
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/ztest.h>

#include "app_test.h"
#include "fake_fram.h"
#include "fake_retained_mem.h"
#include "msg/common.h"
#include "msg/nvm.h"
#include "nvm/map.h"
#include "silversat/event.h"
#include "silversat/link.h"
#include "silversat/nvm.h"
#include "silversat/resource_map.h"

#define WAIT K_SECONDS(1)

static const struct device *const fram = DEVICE_DT_GET(FRAM_NODE);
static const struct device *const mirror = DEVICE_DT_GET(NVM_MIRROR_NODE);

/* Wakeups and commands sent so far: the app's status counts both. */
static uint32_t steps;
static uint32_t commands;

/* Wake the app once, as the frame manager does each major frame. */
static void wake(void)
{
	zassert_ok(app_test_wake(&nvm_wakeup_chan, steps * FRAME_SLOTS));
	steps++;
	zassert_ok(app_test_wait_status(&nvm_status_chan, steps, commands, WAIT, NULL));
}

/* Send a command, as command ingest does, and return the app's status after it. */
static struct app_status send(const struct nvm_cmd *cmd)
{
	struct app_status status;

	zassert_ok(zbus_chan_pub(&nvm_cmd_chan, cmd, K_NO_WAIT));
	commands++;
	zassert_ok(app_test_wait_status(&nvm_status_chan, steps, commands, WAIT, &status));
	return status;
}

static struct app_status send_dump(uint8_t store, uint16_t address, uint8_t length)
{
	const struct nvm_cmd cmd = {
		.id = NVM_CMD_DUMP,
		.args.dump = {.store = store, .address = address, .length = length},
	};

	return send(&cmd);
}

static struct nvm_hk nvm_hk(void)
{
	struct nvm_hk hk;

	zassert_ok(zbus_chan_read(&nvm_hk_chan, &hk, K_MSEC(10)));
	return hk;
}

/* Take the next frame off the downlink, as the radio would. */
static bool next_frame(struct link_frame *frame)
{
	return k_msgq_get(&downlink_msgq, frame, K_NO_WAIT) == 0;
}

/* A payload for any region: each byte different. */
static void fill(uint8_t *payload, size_t size, uint8_t first)
{
	for (size_t i = 0; i < size; i++) {
		payload[i] = (uint8_t)(first + i);
	}
}

/* Each test starts with a blank, working FRAM and mirror, and an empty downlink. */
static void before(void *fixture)
{
	ARG_UNUSED(fixture);
	fake_fram_reset(fram);
	fake_retained_mem_reset(mirror);
	zassert_ok(nvm_retry());
	k_msgq_purge(&downlink_msgq);
}

ZTEST_SUITE(nvm_app, NULL, NULL, before, NULL, NULL);

/* ---- Housekeeping --------------------------------------------------------- */

/* A test-only mirrored record, past the end of the flight map in both stores. */
static const uint8_t test_defaults[4] = {0xd0, 0xd1, 0xd2, 0xd3};
static const struct nvm_region test_region = {
	.name = "test",
	.address = NVM_FRAM_USED + 0x100,
	.size = 4,
	.version = 1,
	.defaults = test_defaults,
	.mirrored = true,
	.mirror_address = NVM_MIRROR_USED + 0x100,
};

BUILD_ASSERT(NVM_MIRROR_USED + 0x100 + NVM_SLOT_SIZE(4) * 2 <= NVM_MIRROR_SIZE,
	     "the test record fits in the mirror");

ZTEST(nvm_app, test_housekeeping_follows_the_service)
{
	const uint8_t value[4] = {1, 2, 3, 4};
	uint8_t payload[4];
	struct nvm_hk hk;
	struct nvm_stats stats;

	/* Make every count move: a write, a read, a default, a bad slot, an
	 * error, and a read answered by the mirror. */
	zassert_equal(nvm_read(&test_region, payload), -ENOENT, "blank: the default");
	zassert_ok(nvm_write(&test_region, value));
	zassert_ok(nvm_write(&test_region, value));
	fake_fram_memory(fram)[test_region.address + NVM_HEADER_LEN] ^= 0x01;
	zassert_ok(nvm_read(&test_region, payload), "one bad slot");
	fake_fram_fail(fram, true);
	zassert_ok(nvm_read(&test_region, payload), "FRAM failed: from the mirror");
	wake();

	hk = nvm_hk();
	stats = nvm_stats();
	zassert_true(stats.reads > 0 && stats.writes > 0 && stats.defaults_used > 0 &&
		     stats.from_mirror > 0 && stats.bad_slots > 0 && stats.errors > 0,
		     "every count is in use");
	zassert_false(hk.fram_available);
	hk = nvm_hk();
	stats = nvm_stats();
	zassert_true(hk.mirror_available);
	zassert_equal(hk.reads, stats.reads);
	zassert_equal(hk.writes, stats.writes);
	zassert_equal(hk.defaults_used, stats.defaults_used);
	zassert_equal(hk.from_mirror, stats.from_mirror);
	zassert_equal(hk.bad_slots, stats.bad_slots);
	zassert_equal(hk.errors, stats.errors);
}

ZTEST(nvm_app, test_each_wakeup_is_a_step)
{
	struct app_status status;

	wake();
	zassert_ok(app_test_wait_status(&nvm_status_chan, steps, commands, WAIT, &status));
	zassert_equal(status.steps, steps);
}

/* ---- Scrubbing ------------------------------------------------------------ */

BUILD_ASSERT(NVM_REGION_COUNT >= 2, "the scrub tests need two regions");

ZTEST(nvm_app, test_a_blank_store_scrubs_clean)
{
	struct nvm_hk start = nvm_hk();

	for (int i = 0; i < NVM_REGION_COUNT; i++) {
		wake();
	}
	zassert_equal(nvm_hk().scrub_passes, start.scrub_passes + 1, "one pass, one region a wakeup");
	zassert_equal(nvm_hk().scrub_bad, start.scrub_bad, "empty slots aren't bad");
}

ZTEST(nvm_app, test_a_scrub_pass_finds_a_corrupt_slot)
{
	const struct nvm_region *r = &nvm_map[1];
	uint8_t payload[NVM_PAYLOAD_MAX];
	struct nvm_hk start = nvm_hk();

	fill(payload, r->size, 0x10);
	zassert_ok(nvm_write(r, payload));
	fake_fram_memory(fram)[r->address + NVM_HEADER_LEN] ^= 0x01;

	for (int i = 0; i < NVM_REGION_COUNT; i++) {
		wake();
	}
	zassert_equal(nvm_hk().scrub_bad, start.scrub_bad + 1);
	zassert_equal(nvm_hk().scrub_passes, start.scrub_passes + 1);

	/* Every pass counts it again, until its owner writes over it. */
	for (int i = 0; i < NVM_REGION_COUNT; i++) {
		wake();
	}
	zassert_equal(nvm_hk().scrub_bad, start.scrub_bad + 2);
}

ZTEST(nvm_app, test_scrubbing_never_writes)
{
	const struct nvm_region *r = &nvm_map[0];
	uint8_t payload[NVM_PAYLOAD_MAX];
	static uint8_t fram_before[NVM_FRAM_SIZE];
	static uint8_t mirror_before[NVM_MIRROR_SIZE];
	uint32_t writes;

	fill(payload, r->size, 0x20);
	zassert_ok(nvm_write(r, payload));
	fake_fram_memory(fram)[r->address + NVM_HEADER_LEN] ^= 0x01;
	memcpy(fram_before, fake_fram_memory(fram), sizeof(fram_before));
	memcpy(mirror_before, fake_retained_mem_memory(mirror), sizeof(mirror_before));
	writes = nvm_stats().writes;

	for (int i = 0; i < NVM_REGION_COUNT; i++) {
		wake();
	}
	zassert_mem_equal(fake_fram_memory(fram), fram_before, sizeof(fram_before),
			  "the bad slot is left for its owner");
	zassert_mem_equal(fake_retained_mem_memory(mirror), mirror_before, sizeof(mirror_before));
	zassert_equal(nvm_stats().writes, writes);
}

ZTEST(nvm_app, test_a_scrub_with_fram_failed)
{
	struct nvm_hk start = nvm_hk();

	fake_fram_fail(fram, true);
	for (int i = 0; i < NVM_REGION_COUNT; i++) {
		wake();
	}
	zassert_false(nvm_hk().fram_available, "the scrub found it");
	zassert_equal(nvm_hk().scrub_bad, start.scrub_bad, "a dead part has no bad slots");
	zassert_equal(nvm_hk().scrub_passes, start.scrub_passes + 1, "and the scrub goes on");
}

/* ---- Dump (DS-66, DS-74) -------------------------------------------------- */

/* Check a D packet: its header, and that it holds the store's bytes. */
static void expect_dump(const struct link_frame *frame, uint8_t store, uint16_t address,
			uint8_t length)
{
	const uint8_t *data = (const uint8_t *)frame->data;
	const uint8_t *memory = store == NVM_STORE_FRAM ? fake_fram_memory(fram)
							: fake_retained_mem_memory(mirror);

	zassert_equal(frame->len, 5 + length);
	zassert_equal(data[0], 'D');
	zassert_equal(data[1], store);
	zassert_equal(sys_get_le16(&data[2]), address);
	zassert_equal(data[4], length);
	zassert_mem_equal(&data[5], &memory[address], length);
}

ZTEST(nvm_app, test_dump_fram)
{
	const struct nvm_region *r = &nvm_map[0];
	uint8_t payload[NVM_PAYLOAD_MAX];
	struct link_frame frame;
	struct app_status before_status;
	struct app_status status;
	uint32_t dumps = nvm_hk().dumps;

	fill(payload, r->size, 0x30);
	zassert_ok(nvm_write(r, payload));
	zassert_ok(app_test_wait_status(&nvm_status_chan, steps, commands, WAIT, &before_status));
	status = send_dump(NVM_STORE_FRAM, r->address, 240);
	zassert_equal(status.cmd_accepted, before_status.cmd_accepted + 1);
	zassert_true(next_frame(&frame));
	expect_dump(&frame, NVM_STORE_FRAM, r->address, 240);
	zassert_mem_equal((uint8_t *)&frame.data[5 + NVM_HEADER_LEN], payload, r->size,
			  "the record's payload, as stored");
	zassert_false(next_frame(&frame), "one packet a command");
	zassert_equal(nvm_hk().dumps, dumps + 1);
}

ZTEST(nvm_app, test_dump_the_mirror)
{
	const struct nvm_region *r = &nvm_map[0];
	uint8_t payload[NVM_PAYLOAD_MAX];
	struct link_frame frame;

	zassert_true(r->mirrored, "nvm_map.yaml mirrors the first record");
	fill(payload, r->size, 0x40);
	zassert_ok(nvm_write(r, payload));
	send_dump(NVM_STORE_MIRROR, r->mirror_address, 17);
	zassert_true(next_frame(&frame));
	expect_dump(&frame, NVM_STORE_MIRROR, r->mirror_address, 17);
}

ZTEST(nvm_app, test_dump_the_last_bytes)
{
	struct link_frame frame;

	fake_fram_memory(fram)[NVM_FRAM_SIZE - 1] = 0xa5;
	send_dump(NVM_STORE_FRAM, NVM_FRAM_SIZE - 1, 1);
	zassert_true(next_frame(&frame));
	expect_dump(&frame, NVM_STORE_FRAM, NVM_FRAM_SIZE - 1, 1);
}

ZTEST(nvm_app, test_dump_does_not_read_a_record)
{
	struct nvm_stats start = nvm_stats();
	struct link_frame frame;

	send_dump(NVM_STORE_FRAM, 0, 64);
	zassert_true(next_frame(&frame));
	zassert_equal(nvm_stats().reads, start.reads, "bytes as they are, not records");
	zassert_equal(nvm_stats().writes, start.writes);
}

/* A refused dump: rejected and counted. */
static void expect_rejected(uint8_t store, uint16_t address, uint8_t length)
{
	struct nvm_hk start = nvm_hk();
	struct app_status before_status;
	struct app_status status;

	zassert_ok(app_test_wait_status(&nvm_status_chan, steps, commands, WAIT, &before_status));
	status = send_dump(store, address, length);
	zassert_equal(status.cmd_rejected, before_status.cmd_rejected + 1);
	zassert_equal(nvm_hk().dump_refused, start.dump_refused + 1);
	zassert_equal(nvm_hk().dumps, start.dumps);
}

/* A full downlink: refused, and the frames already queued are kept. */
static void expect_refused_with_full_downlink(void)
{
	expect_rejected(NVM_STORE_FRAM, 0, 16);
	zassert_equal(k_msgq_num_used_get(&downlink_msgq), DOWNLINK_QUEUE_DEPTH);
}

/* Any other refused dump: rejected, counted, and nothing sent. */
static void expect_refused(uint8_t store, uint16_t address, uint8_t length)
{
	struct link_frame frame;

	expect_rejected(store, address, length);
	zassert_false(next_frame(&frame));
}

ZTEST(nvm_app, test_dump_refuses_no_bytes)
{
	expect_refused(NVM_STORE_FRAM, 0, 0);
}

ZTEST(nvm_app, test_dump_refuses_more_than_a_packet)
{
	expect_refused(NVM_STORE_FRAM, 0, 241);
}

ZTEST(nvm_app, test_dump_refuses_past_the_end)
{
	expect_refused(NVM_STORE_FRAM, NVM_FRAM_SIZE - 1, 2);
	expect_refused(NVM_STORE_MIRROR, NVM_MIRROR_SIZE - 1, 2);
	zassert_true(nvm_hk().fram_available, "a bad request is not a fault");
}

ZTEST(nvm_app, test_dump_refuses_an_unknown_store)
{
	expect_refused(2, 0, 16);
}

ZTEST(nvm_app, test_dump_refuses_a_failed_store)
{
	fake_fram_fail(fram, true);
	expect_refused(NVM_STORE_FRAM, 0, 16);
	zassert_false(nvm_hk().fram_available);

	fake_retained_mem_fail(mirror, true);
	expect_refused(NVM_STORE_MIRROR, 0, 16);
	zassert_false(nvm_hk().mirror_available);
}

ZTEST(nvm_app, test_dump_refused_when_the_downlink_is_full)
{
	struct link_frame frame = {.len = 1, .data = "A"};

	while (k_msgq_put(&downlink_msgq, &frame, K_NO_WAIT) == 0) {
	}
	expect_refused_with_full_downlink();
}

/* ---- Retry (DS-75) -------------------------------------------------------- */

ZTEST(nvm_app, test_retry_brings_fram_back)
{
	const struct nvm_cmd retry = {.id = NVM_CMD_RETRY};
	struct app_status before_status;
	struct app_status status;

	fake_fram_fail(fram, true);
	fake_retained_mem_fail(mirror, true);
	send_dump(NVM_STORE_FRAM, 0, 16);
	send_dump(NVM_STORE_MIRROR, 0, 16);
	zassert_false(nvm_hk().fram_available);
	zassert_false(nvm_hk().mirror_available);

	/*
	 * Still failed: the retry is accepted and tries FRAM again, and the
	 * next access finds it failed again.
	 */
	zassert_ok(app_test_wait_status(&nvm_status_chan, steps, commands, WAIT, &before_status));
	status = send(&retry);
	zassert_equal(status.cmd_accepted, before_status.cmd_accepted + 1);
	zassert_true(nvm_hk().fram_available, "tried again");
	send_dump(NVM_STORE_FRAM, 0, 16);
	zassert_false(nvm_hk().fram_available, "and failed again");

	fake_fram_fail(fram, false);
	fake_retained_mem_fail(mirror, false);
	send(&retry);
	zassert_true(nvm_hk().fram_available);
	zassert_true(nvm_hk().mirror_available);
}

/* ---- Anything else -------------------------------------------------------- */

ZTEST(nvm_app, test_an_unknown_command_is_rejected)
{
	const struct nvm_cmd cmd = {.id = 99};
	struct app_status before_status;
	struct app_status status;

	zassert_ok(app_test_wait_status(&nvm_status_chan, steps, commands, WAIT, &before_status));
	status = send(&cmd);
	zassert_equal(status.cmd_rejected, before_status.cmd_rejected + 1);
	zassert_equal(status.cmd_accepted, before_status.cmd_accepted);
}

/* ---- Events (DS-10) --------------------------------------------------------- */

/*
 * Wake the app once and throw away the events that reports: the setup
 * before each test resets the stores, and the app reports any change it
 * sees at its next wakeup.
 */
static void settle(void)
{
	struct event event;

	wake();
	while (event_take(&event) == 0) {
	}
}

/* The next event should be the nvm app's event id. Returns it. */
static struct event expect_event(uint16_t id)
{
	struct event event;

	zassert_ok(event_take(&event), "event %u", id);
	zassert_equal(event.app, APP_ID_NVM);
	zassert_equal(event.id, id, "event %u, not %u", id, event.id);
	return event;
}

ZTEST(nvm_app, test_fram_failing_and_coming_back_are_events)
{
	const struct nvm_cmd retry = {.id = NVM_CMD_RETRY};
	struct event event;

	settle();
	fake_fram_fail(fram, true);
	send_dump(NVM_STORE_FRAM, 0, 16);
	event = expect_event(NVM_EVENT_FRAM_FAILED);
	zassert_equal(event.severity, SEVERITY_ERROR);
	zassert_true(event.met_ms > 0 && event.met_ms <= k_uptime_get(),
		     "the latest tick's MET (DS-25), not %lld", (long long)event.met_ms);
	wake();
	zassert_equal(event_take(&event), -ENOMSG, "reported once");

	fake_fram_fail(fram, false);
	send(&retry);
	event = expect_event(NVM_EVENT_FRAM_RESTORED);
	zassert_equal(event.severity, SEVERITY_INFO);
}

ZTEST(nvm_app, test_the_mirror_failing_and_coming_back_are_events)
{
	const struct nvm_cmd retry = {.id = NVM_CMD_RETRY};

	settle();
	fake_retained_mem_fail(mirror, true);
	send_dump(NVM_STORE_MIRROR, 0, 16);
	expect_event(NVM_EVENT_MIRROR_FAILED);
	fake_retained_mem_fail(mirror, false);
	send(&retry);
	expect_event(NVM_EVENT_MIRROR_RESTORED);
}

ZTEST(nvm_app, test_bad_slots_are_an_event_once_until_clean)
{
	const struct nvm_region *r = &nvm_map[1];
	uint8_t payload[NVM_PAYLOAD_MAX];
	struct event event;
	bool found = false;

	settle();
	fill(payload, r->size, 0x50);
	zassert_ok(nvm_write(r, payload));
	fake_fram_memory(fram)[r->address + NVM_HEADER_LEN] ^= 0x01;
	for (int i = 0; i < NVM_REGION_COUNT; i++) {
		wake();
	}
	while (event_take(&event) == 0) {
		if (event.id == NVM_EVENT_BAD_SLOTS) {
			zassert_false(found, "once a pass");
			found = true;
			zassert_equal(event.arg0, 1, "region 1");
			zassert_equal(event.arg1, 1, "one bad slot");
			zassert_equal(event.severity, SEVERITY_WARNING);
		}
	}
	zassert_true(found);

	/* Still bad next pass: no new event. */
	for (int i = 0; i < NVM_REGION_COUNT; i++) {
		wake();
	}
	zassert_equal(event_take(&event), -ENOMSG);

	/* Its owner writes it (the write goes over the bad slot): clean, then bad again. */
	zassert_ok(nvm_write(r, payload));
	for (int i = 0; i < NVM_REGION_COUNT; i++) {
		wake();
	}
	zassert_equal(event_take(&event), -ENOMSG, "clean");
	for (int slot = 0; slot < 2; slot++) {
		/* Whichever slot now holds the record; an empty one stays empty. */
		fake_fram_memory(fram)[r->address + slot * NVM_SLOT_SIZE(r->size) + NVM_HEADER_LEN] ^=
			0x01;
	}
	for (int i = 0; i < NVM_REGION_COUNT; i++) {
		wake();
	}
	event = expect_event(NVM_EVENT_BAD_SLOTS);
	zassert_equal(event.arg0, 1, "reported again, once it had been clean");
}
