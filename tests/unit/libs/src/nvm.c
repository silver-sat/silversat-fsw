/*
 * Tests for the FRAM service (DS-70, DS-71, DS-75).
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The FRAM is a fake (tests/fakes/fake_fram.c) that the tests can fail,
 * tear a write on, or corrupt. Most tests use a region made here by hand;
 * flight code only ever uses the regions the generator writes from
 * messages/nvm_map.yaml, and the last test uses one of those.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <zephyr/ztest.h>

#include "fake_fram.h"
#include "nvm/command_ingest.h"
#include "silversat/nvm.h"
#include "silversat/resource_map.h"

static const struct device *const fram = DEVICE_DT_GET(FRAM_NODE);

static const uint8_t defaults[4] = {0xd0, 0xd1, 0xd2, 0xd3};

static const struct nvm_region region = {
	.name = "test",
	.address = 0x100,
	.size = 4,
	.version = 1,
	.defaults = defaults,
};

/* The same place in FRAM, as the next version of the software would see it. */
static const struct nvm_region region_v2 = {
	.name = "test",
	.address = 0x100,
	.size = 4,
	.version = 2,
	.defaults = defaults,
};

static const uint8_t a[4] = {1, 2, 3, 4};
static const uint8_t b[4] = {5, 6, 7, 8};
static const uint8_t c[4] = {9, 10, 11, 12};

#define SLOT_SIZE NVM_SLOT_SIZE(4)

static uint8_t *slot(int n)
{
	return &fake_fram_memory(fram)[region.address + n * SLOT_SIZE];
}

static uint32_t generation(int n)
{
	return sys_get_le32(&slot(n)[4]);
}

/* Rewrite a slot's generation and fix its CRC, as if it had been written so. */
static void set_generation(int n, uint32_t gen)
{
	sys_put_le32(gen, &slot(n)[4]);
	sys_put_le32(crc32_c(0, slot(n), SLOT_SIZE - NVM_CRC_LEN, true, true),
		     &slot(n)[SLOT_SIZE - NVM_CRC_LEN]);
}

/* Read the test region, check the result code, and return a pointer to it. */
static const uint8_t *read_expect(int expected_rc)
{
	static uint8_t payload[4];

	zassert_equal(nvm_read(&region, payload), expected_rc);
	return payload;
}

/* Each test starts with a blank, working FRAM. */
static void before(void *fixture)
{
	ARG_UNUSED(fixture);
	fake_fram_reset(fram);
	zassert_ok(nvm_retry());
}

ZTEST_SUITE(nvm, NULL, NULL, before, NULL, NULL);

ZTEST(nvm, test_blank_fram_reads_the_default)
{
	zassert_mem_equal(read_expect(-ENOENT), defaults, 4);
	zassert_true(nvm_available(), "blank is not a fault");
}

ZTEST(nvm, test_write_then_read)
{
	zassert_ok(nvm_write(&region, a));
	zassert_mem_equal(read_expect(0), a, 4);
}

ZTEST(nvm, test_writes_alternate_slots_and_the_newest_wins)
{
	zassert_ok(nvm_write(&region, a));
	zassert_ok(nvm_write(&region, b));
	zassert_ok(nvm_write(&region, c));
	zassert_mem_equal(read_expect(0), c, 4);
	zassert_equal(generation(0), 3, "a, then c over it");
	zassert_equal(generation(1), 2, "b, the previous value, still there");
	zassert_mem_equal(&slot(1)[NVM_HEADER_LEN], b, 4);
}

ZTEST(nvm, test_slot_layout)
{
	zassert_ok(nvm_write(&region, a));
	zassert_equal(sys_get_le16(&slot(0)[0]), NVM_MAGIC);
	zassert_equal(slot(0)[2], 1, "version");
	zassert_equal(slot(0)[3], 4, "length");
	zassert_equal(generation(0), 1);
	zassert_mem_equal(&slot(0)[NVM_HEADER_LEN], a, 4);
	zassert_equal(sys_get_le32(&slot(0)[NVM_HEADER_LEN + 4]),
		      crc32_c(0, slot(0), NVM_HEADER_LEN + 4, true, true), "CRC-32C over the rest");
}

ZTEST(nvm, test_torn_write_keeps_the_previous_value)
{
	zassert_ok(nvm_write(&region, a));
	/* Power fails part-way through writing b over the other slot. */
	fake_fram_tear_next_write(fram, NVM_HEADER_LEN + 2);
	zassert_equal(nvm_write(&region, b), -EIO);

	/* After the reset that follows: */
	zassert_ok(nvm_retry());
	zassert_mem_equal(read_expect(0), a, 4);
	zassert_true(nvm_available(), "one bad slot is a torn write, not a failed part");
}

ZTEST(nvm, test_a_corrupt_slot_falls_back_to_the_other)
{
	uint32_t bad = nvm_stats().bad_slots;

	zassert_ok(nvm_write(&region, a));
	zassert_ok(nvm_write(&region, b));
	slot(1)[NVM_HEADER_LEN] ^= 0x01;
	zassert_mem_equal(read_expect(0), a, 4);
	zassert_equal(nvm_stats().bad_slots - bad, 1);
	zassert_true(nvm_available(), "one bad slot is not a failed part");

	/* The next write replaces the corrupt slot, not the good one. */
	zassert_ok(nvm_write(&region, c));
	zassert_mem_equal(read_expect(0), c, 4);
	zassert_mem_equal(&slot(0)[NVM_HEADER_LEN], a, 4);
}

ZTEST(nvm, test_a_slot_of_the_wrong_length_is_bad)
{
	uint32_t bad = nvm_stats().bad_slots;

	/* This version, a valid CRC, but a length this record can't have. */
	zassert_ok(nvm_write(&region, a));
	slot(0)[3] = 3;
	set_generation(0, 1);
	zassert_mem_equal(read_expect(-ENOENT), defaults, 4);
	zassert_equal(nvm_stats().bad_slots - bad, 1);
}

ZTEST(nvm, test_both_slots_corrupt_is_a_failure)
{
	zassert_ok(nvm_write(&region, a));
	zassert_ok(nvm_write(&region, b));
	slot(0)[NVM_HEADER_LEN] ^= 0x01;
	slot(1)[NVM_HEADER_LEN] ^= 0x01;
	zassert_mem_equal(read_expect(-EIO), defaults, 4);
	zassert_false(nvm_available(), "FRAM degraded (DS-75)");
}

ZTEST(nvm, test_another_version_reads_the_default)
{
	zassert_ok(nvm_write(&region, a));
	zassert_equal(nvm_read(&region_v2, (uint8_t[4]){0}), -ENOENT);
	zassert_true(nvm_available(), "a new version of the software is not a fault");

	/* The new version writes its own record, and reads it back. */
	zassert_ok(nvm_write(&region_v2, b));
	{
		uint8_t payload[4];

		zassert_ok(nvm_read(&region_v2, payload));
		zassert_mem_equal(payload, b, 4);
	}
}

ZTEST(nvm, test_generation_wraps)
{
	zassert_ok(nvm_write(&region, a));
	set_generation(0, UINT32_MAX);
	zassert_ok(nvm_write(&region, b));
	zassert_equal(generation(1), 0, "the generation wrapped");
	zassert_mem_equal(read_expect(0), b, 4, "0 is newer than UINT32_MAX (DS-71)");
}

ZTEST(nvm, test_dead_part_gives_defaults_until_retried)
{
	zassert_ok(nvm_write(&region, a));
	fake_fram_fail(fram, true);
	zassert_mem_equal(read_expect(-EIO), defaults, 4);
	zassert_false(nvm_available());
	zassert_equal(nvm_write(&region, b), -EIO);

	/* Working again, but unused until the retry the ground commands. */
	fake_fram_fail(fram, false);
	zassert_mem_equal(read_expect(-EIO), defaults, 4);
	zassert_ok(nvm_retry());
	zassert_mem_equal(read_expect(0), a, 4);
}

ZTEST(nvm, test_a_write_that_does_not_stick_is_a_failure)
{
	fake_fram_drop_writes(fram, true);
	zassert_equal(nvm_write(&region, a), -EIO, "caught by the read-back");
	zassert_false(nvm_available());
}

ZTEST(nvm, test_stats)
{
	struct nvm_stats before = nvm_stats();

	(void)nvm_read(&region, (uint8_t[4]){0});
	zassert_ok(nvm_write(&region, a));
	(void)nvm_read(&region, (uint8_t[4]){0});
	fake_fram_fail(fram, true);
	(void)nvm_read(&region, (uint8_t[4]){0});

	zassert_equal(nvm_stats().reads - before.reads, 3);
	zassert_equal(nvm_stats().writes - before.writes, 1);
	zassert_equal(nvm_stats().defaults_used - before.defaults_used, 2, "blank, then failed");
	zassert_equal(nvm_stats().errors - before.errors, 1);
}

/* ---- A generated record (nvm/command_ingest.h) ------------------------- */

ZTEST(nvm, test_generated_record_round_trip)
{
	struct nvm_command_state state;
	const struct nvm_command_state written = {
		.floor_slot0 = 0x0123456789abcdefULL,
		.floor_slot1 = UINT64_MAX,
		.active_slot = 1,
		.contacted = true,
		.last_accepted_met_ms = -2,
	};

	/* Blank FRAM: the defaults from nvm_map.yaml. */
	zassert_equal(nvm_command_state_read(&state), -ENOENT);
	zassert_equal(state.floor_slot0, 1767225600000ULL, "the mission epoch");
	zassert_equal(state.floor_slot1, 1767225600000ULL);
	zassert_equal(state.active_slot, 0);
	zassert_false(state.contacted);

	zassert_ok(nvm_command_state_write(&written));
	zassert_ok(nvm_command_state_read(&state));
	zassert_equal(state.floor_slot0, written.floor_slot0);
	zassert_equal(state.floor_slot1, written.floor_slot1);
	zassert_equal(state.active_slot, 1);
	zassert_true(state.contacted);
	zassert_equal(state.last_accepted_met_ms, -2);
}
