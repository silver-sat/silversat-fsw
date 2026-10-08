/*
 * The FRAM service. See silversat/nvm.h for the slot layout and the rules.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Two stores hold records in the same two-slot form:
 *
 *   FRAM     the devicetree alias "fram" (the resource map's FRAM_NODE),
 *            through Zephyr's EEPROM API. Every record.
 *   mirror   the alias "nvm-mirror" (NVM_MIRROR_NODE), through Zephyr's
 *            retained memory API: the STM32's backup SRAM. Only the records
 *            marked mirror: in nvm_map.yaml (DS-75's second tier).
 *
 * A board without an alias runs without that store: a record kept in
 * neither reads as its default.
 *
 * Rings (DS-71's boot log) are entries in the same slot form, each written
 * once at its number modulo the ring's length, in FRAM and, if the ring is
 * mirrored, in the mirror too.
 *
 * For the nvm app, nvm_check() reads every slot of a region and counts the
 * bad ones, and nvm_raw_read() reads a store's bytes as they are. Neither
 * writes.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/eeprom.h>
#include <zephyr/drivers/retained_mem.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>

#include "silversat/nvm.h"
#include "silversat/resource_map.h"

enum store {
	FRAM,
	MIRROR,
	STORES,
};

static const struct device *const devices[STORES] = {
	[FRAM] = DEVICE_DT_GET_OR_NULL(FRAM_NODE),
	[MIRROR] = DEVICE_DT_GET_OR_NULL(NVM_MIRROR_NODE),
};

/* One operation at a time (DS-70). Covers the flags and counts too. */
K_MUTEX_DEFINE(nvm_lock);

static bool available[STORES];
static struct nvm_stats stats;

/* What a slot was found to hold. */
enum slot_state {
	SLOT_EMPTY,   /* no magic: never written */
	SLOT_OTHER,   /* a record of another version */
	SLOT_BAD,     /* magic, but the CRC or length is wrong */
	SLOT_VALID,
};

/* The newest valid record in one store. */
struct found {
	int slot; /* 0 or 1; -1 if none */
	uint32_t generation;
};

/* Whether this store holds this region, and is working. */
static bool in_use(const struct nvm_region *region, enum store s)
{
	return available[s] && (s == FRAM || region->mirrored);
}

static off_t slot_address(const struct nvm_region *region, enum store s, int slot)
{
	uint16_t base = s == FRAM ? region->address : region->mirror_address;

	return base + slot * NVM_SLOT_SIZE(region->size);
}

static int store_read(enum store s, off_t offset, uint8_t *buf, size_t len)
{
	return s == FRAM ? eeprom_read(devices[s], offset, buf, len)
			 : retained_mem_read(devices[s], offset, buf, len);
}

static int store_write(enum store s, off_t offset, const uint8_t *buf, size_t len)
{
	return s == FRAM ? eeprom_write(devices[s], offset, buf, len)
			 : retained_mem_write(devices[s], offset, buf, len);
}

/* Newer by serial-number arithmetic, so the generation may wrap (DS-71). */
static bool newer(uint32_t a, uint32_t b)
{
	return (int32_t)(a - b) > 0;
}

/* The store failed: stop using it until nvm_retry(). */
static void fail(enum store s)
{
	available[s] = false;
	stats.errors++;
}

/*
 * Read and check one slot into buf (NVM_SLOT_SIZE(region->size) bytes).
 * Returns the slot's state, or -EIO if the device failed.
 */
static int read_slot(const struct nvm_region *region, enum store s, int slot, uint8_t *buf,
		     uint32_t *generation)
{
	size_t len = NVM_SLOT_SIZE(region->size);
	size_t crc_at = len - NVM_CRC_LEN;

	if (store_read(s, slot_address(region, s, slot), buf, len) != 0) {
		return -EIO;
	}
	if (sys_get_le16(&buf[0]) != NVM_MAGIC) {
		return SLOT_EMPTY;
	}
	/*
	 * A record from another version of the software may have another
	 * size, so it isn't checked further: it reads as no record, never as
	 * a fault. A record of this version must have this size.
	 */
	if (buf[2] != region->version) {
		return SLOT_OTHER;
	}
	if (buf[3] != region->size ||
	    crc32_c(0, buf, crc_at, true, true) != sys_get_le32(&buf[crc_at])) {
		stats.bad_slots++;
		return SLOT_BAD;
	}
	*generation = sys_get_le32(&buf[4]);
	return SLOT_VALID;
}

/*
 * Find a store's newest valid slot, leaving its bytes in bufs[slot].
 * Returns 0 (found->slot is -1 if neither slot is valid), -EBADMSG if both
 * slots are bad, or -EIO if the device failed.
 */
static int newest_slot(const struct nvm_region *region, enum store s,
		       uint8_t bufs[2][NVM_SLOT_SIZE(NVM_PAYLOAD_MAX)], struct found *found)
{
	int state[2];
	uint32_t gen[2] = {0, 0};

	for (int slot = 0; slot < 2; slot++) {
		state[slot] = read_slot(region, s, slot, bufs[slot], &gen[slot]);
		if (state[slot] < 0) {
			return state[slot];
		}
	}
	found->slot = -1;
	for (int slot = 0; slot < 2; slot++) {
		if (state[slot] == SLOT_VALID &&
		    (found->slot < 0 || newer(gen[slot], found->generation))) {
			found->slot = slot;
			found->generation = gen[slot];
		}
	}
	if (found->slot < 0 && state[0] == SLOT_BAD && state[1] == SLOT_BAD) {
		return -EBADMSG;
	}
	return 0;
}

int nvm_read(const struct nvm_region *region, uint8_t *payload)
{
	uint8_t bufs[2][NVM_SLOT_SIZE(NVM_PAYLOAD_MAX)];
	bool looked = false;
	bool have = false;
	uint32_t best = 0;

	k_mutex_lock(&nvm_lock, K_FOREVER);
	stats.reads++;
	for (enum store s = FRAM; s < STORES; s++) {
		struct found found;

		if (!in_use(region, s)) {
			continue;
		}
		if (newest_slot(region, s, bufs, &found) != 0) {
			/* A device error, or both slots corrupt: the store has failed (DS-75). */
			fail(s);
			continue;
		}
		looked = true;
		/* Both copies of a write carry one generation: the newest wins. */
		if (found.slot >= 0 && (!have || newer(found.generation, best))) {
			memcpy(payload, &bufs[found.slot][NVM_HEADER_LEN], region->size);
			best = found.generation;
			have = true;
			if (s == MIRROR) {
				stats.from_mirror++;
			}
		}
	}
	if (!have) {
		memcpy(payload, region->defaults, region->size);
		stats.defaults_used++;
	}
	k_mutex_unlock(&nvm_lock);
	if (have) {
		return 0;
	}
	return looked ? -ENOENT : -EIO;
}

/* Write one store's copy over its older slot, and read it back. */
static bool write_copy(const struct nvm_region *region, enum store s, int target, uint8_t *out)
{
	uint8_t check[NVM_SLOT_SIZE(NVM_PAYLOAD_MAX)];
	size_t len = NVM_SLOT_SIZE(region->size);
	off_t at = slot_address(region, s, target);

	/* Committed when the read-back matches: the new slot's CRC is valid. */
	if (store_write(s, at, out, len) != 0 || store_read(s, at, check, len) != 0 ||
	    memcmp(out, check, len) != 0) {
		fail(s);
		return false;
	}
	return true;
}

/* Build a slot: header, payload, and the CRC over both. */
static void make_slot(const struct nvm_region *region, uint32_t generation,
		      const uint8_t *payload, uint8_t *out)
{
	size_t crc_at = NVM_SLOT_SIZE(region->size) - NVM_CRC_LEN;

	sys_put_le16(NVM_MAGIC, &out[0]);
	out[2] = region->version;
	out[3] = region->size;
	sys_put_le32(generation, &out[4]);
	memcpy(&out[NVM_HEADER_LEN], payload, region->size);
	sys_put_le32(crc32_c(0, out, crc_at, true, true), &out[crc_at]);
}

int nvm_write(const struct nvm_region *region, const uint8_t *payload)
{
	uint8_t bufs[2][NVM_SLOT_SIZE(NVM_PAYLOAD_MAX)];
	uint8_t out[NVM_SLOT_SIZE(NVM_PAYLOAD_MAX)];
	struct found found[STORES];
	bool any = false;
	uint32_t generation = 0;
	bool stored = false;

	k_mutex_lock(&nvm_lock, K_FOREVER);
	stats.writes++;

	/* The next generation after the newest record in either store. */
	for (enum store s = FRAM; s < STORES; s++) {
		found[s].slot = -1;
		if (!in_use(region, s)) {
			continue;
		}
		if (newest_slot(region, s, bufs, &found[s]) == -EIO) {
			fail(s);
			continue;
		}
		if (found[s].slot >= 0 && (!any || newer(found[s].generation, generation))) {
			generation = found[s].generation;
			any = true;
		}
	}
	generation = any ? generation + 1 : 1;
	make_slot(region, generation, payload, out);

	/* In each store, over the slot not holding its newest record. */
	for (enum store s = FRAM; s < STORES; s++) {
		if (in_use(region, s) &&
		    write_copy(region, s, found[s].slot >= 0 ? 1 - found[s].slot : 0, out)) {
			stored = true;
		}
	}
	k_mutex_unlock(&nvm_lock);
	return stored ? 0 : -EIO;
}

int nvm_ring_write(const struct nvm_region *region, uint32_t number, const uint8_t *payload)
{
	uint8_t out[NVM_SLOT_SIZE(NVM_PAYLOAD_MAX)];
	bool stored = false;

	k_mutex_lock(&nvm_lock, K_FOREVER);
	stats.writes++;
	if (region->ring > 0) {
		make_slot(region, number, payload, out);
		/* In each store that holds the ring: FRAM, and the mirror if mirrored. */
		for (enum store s = FRAM; s < STORES; s++) {
			if (in_use(region, s) && write_copy(region, s, number % region->ring, out)) {
				stored = true;
			}
		}
	}
	k_mutex_unlock(&nvm_lock);
	return stored ? 0 : -EIO;
}

int nvm_ring_read(const struct nvm_region *region, uint8_t index, uint8_t *payload,
		  uint32_t *number)
{
	uint8_t buf[NVM_SLOT_SIZE(NVM_PAYLOAD_MAX)];
	bool looked = false;
	bool have = false;

	if (index >= region->ring) {
		return -EINVAL;
	}
	k_mutex_lock(&nvm_lock, K_FOREVER);
	stats.reads++;
	for (enum store s = FRAM; s < STORES; s++) {
		uint32_t generation;
		int state;

		if (!in_use(region, s)) {
			continue;
		}
		state = read_slot(region, s, index, buf, &generation);
		if (state < 0) {
			fail(s);
			continue;
		}
		looked = true;
		/* The newer entry at this place, if the two stores differ. */
		if (state == SLOT_VALID && (!have || newer(generation, *number))) {
			memcpy(payload, &buf[NVM_HEADER_LEN], region->size);
			*number = generation;
			have = true;
			if (s == MIRROR) {
				stats.from_mirror++;
			}
		}
	}
	k_mutex_unlock(&nvm_lock);
	if (have) {
		return 0;
	}
	/* Never written, another version, or corrupt; or no store to look in. */
	return looked ? -ENOENT : -EIO;
}

int nvm_raw_read(uint8_t store, uint16_t address, uint8_t *buf, size_t len)
{
	enum store s = store == NVM_STORE_MIRROR ? MIRROR : FRAM;
	ssize_t size;
	int rc;

	if (store > NVM_STORE_MIRROR) {
		return -EINVAL;
	}
	k_mutex_lock(&nvm_lock, K_FOREVER);
	if (!available[s]) {
		k_mutex_unlock(&nvm_lock);
		return -EIO;
	}
	size = s == FRAM ? (ssize_t)eeprom_get_size(devices[s]) : retained_mem_size(devices[s]);
	if (size < 0 || (size_t)address + len > (size_t)size) {
		k_mutex_unlock(&nvm_lock);
		return -EINVAL;
	}
	rc = store_read(s, address, buf, len);
	if (rc != 0) {
		fail(s);
		rc = -EIO;
	}
	k_mutex_unlock(&nvm_lock);
	return rc;
}

int nvm_check(const struct nvm_region *region)
{
	uint8_t buf[NVM_SLOT_SIZE(NVM_PAYLOAD_MAX)];
	int slots = region->ring > 0 ? region->ring : 2;
	int bad = 0;

	k_mutex_lock(&nvm_lock, K_FOREVER);
	for (enum store s = FRAM; s < STORES; s++) {
		if (!in_use(region, s)) {
			continue;
		}
		for (int slot = 0; slot < slots; slot++) {
			uint32_t generation;
			int state = read_slot(region, s, slot, buf, &generation);

			if (state < 0) {
				fail(s);
				k_mutex_unlock(&nvm_lock);
				return -EIO;
			}
			if (state == SLOT_BAD) {
				bad++;
			}
		}
	}
	k_mutex_unlock(&nvm_lock);
	return bad;
}

static bool is_available(enum store s)
{
	bool result;

	k_mutex_lock(&nvm_lock, K_FOREVER);
	result = available[s];
	k_mutex_unlock(&nvm_lock);
	return result;
}

bool nvm_available(void)
{
	return is_available(FRAM);
}

bool nvm_fram_fitted(void)
{
	return devices[FRAM] != NULL;
}

bool nvm_mirror_fitted(void)
{
	return devices[MIRROR] != NULL;
}

bool nvm_mirror_available(void)
{
	return is_available(MIRROR);
}

int nvm_retry(void)
{
	int rc;

	k_mutex_lock(&nvm_lock, K_FOREVER);
	for (enum store s = FRAM; s < STORES; s++) {
		available[s] = devices[s] != NULL && device_is_ready(devices[s]);
	}
	rc = available[FRAM] ? 0 : -EIO;
	k_mutex_unlock(&nvm_lock);
	return rc;
}

struct nvm_stats nvm_stats(void)
{
	struct nvm_stats result;

	k_mutex_lock(&nvm_lock, K_FOREVER);
	result = stats;
	k_mutex_unlock(&nvm_lock);
	return result;
}

/* Before any app's thread starts, so every app's first read sees FRAM. */
static int nvm_init(void)
{
	(void)nvm_retry();
	return 0;
}

SYS_INIT(nvm_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
