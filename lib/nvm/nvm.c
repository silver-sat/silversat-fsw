/*
 * The FRAM service. See silversat/nvm.h for the slot layout and the rules.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The FRAM is the devicetree alias "fram" (the resource map's FRAM_NODE),
 * read and written through Zephyr's EEPROM API. A board without the alias
 * runs with FRAM unavailable: every read returns the record's default.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/eeprom.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>

#include "silversat/nvm.h"
#include "silversat/resource_map.h"

static const struct device *const fram = DEVICE_DT_GET_OR_NULL(FRAM_NODE);

/* One operation at a time (DS-70). Covers the flags and counts too. */
K_MUTEX_DEFINE(nvm_lock);

static bool available;
static struct nvm_stats stats;

/* What a slot was found to hold. */
enum slot_state {
	SLOT_EMPTY,   /* no magic: never written */
	SLOT_OTHER,   /* a record of another version */
	SLOT_BAD,     /* magic, but the CRC or length is wrong */
	SLOT_VALID,
};

static uint16_t slot_address(const struct nvm_region *region, int slot)
{
	return region->address + (uint16_t)(slot * NVM_SLOT_SIZE(region->size));
}

/* Newer by serial-number arithmetic, so the generation may wrap (DS-71). */
static bool newer(uint32_t a, uint32_t b)
{
	return (int32_t)(a - b) > 0;
}

/* The device failed: stop using it until nvm_retry(). */
static void fail(void)
{
	available = false;
	stats.errors++;
}

/*
 * Read and check one slot into buf (NVM_SLOT_SIZE(region->size) bytes).
 * Returns the slot's state, or -EIO if the device failed.
 */
static int read_slot(const struct nvm_region *region, int slot, uint8_t *buf,
		     uint32_t *generation)
{
	size_t len = NVM_SLOT_SIZE(region->size);
	size_t crc_at = len - NVM_CRC_LEN;

	if (eeprom_read(fram, slot_address(region, slot), buf, len) != 0) {
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
 * Find the newest valid slot. Returns its index (0 or 1), -ENOENT if
 * neither is valid, -EBADMSG if both are bad, or -EIO if the device failed.
 * The newest slot's bytes are left in bufs[index].
 */
static int newest_slot(const struct nvm_region *region,
		       uint8_t bufs[2][NVM_SLOT_SIZE(NVM_PAYLOAD_MAX)], uint32_t *generation)
{
	int state[2];
	uint32_t gen[2] = {0, 0};

	for (int slot = 0; slot < 2; slot++) {
		state[slot] = read_slot(region, slot, bufs[slot], &gen[slot]);
		if (state[slot] < 0) {
			return state[slot];
		}
	}
	if (state[0] == SLOT_VALID && state[1] == SLOT_VALID) {
		int slot = newer(gen[1], gen[0]) ? 1 : 0;

		*generation = gen[slot];
		return slot;
	}
	for (int slot = 0; slot < 2; slot++) {
		if (state[slot] == SLOT_VALID) {
			*generation = gen[slot];
			return slot;
		}
	}
	return (state[0] == SLOT_BAD && state[1] == SLOT_BAD) ? -EBADMSG : -ENOENT;
}

int nvm_read(const struct nvm_region *region, uint8_t *payload)
{
	uint8_t bufs[2][NVM_SLOT_SIZE(NVM_PAYLOAD_MAX)];
	uint32_t generation;
	int rc = -EIO;
	int slot;

	k_mutex_lock(&nvm_lock, K_FOREVER);
	stats.reads++;
	if (available) {
		slot = newest_slot(region, bufs, &generation);
		if (slot >= 0) {
			memcpy(payload, &bufs[slot][NVM_HEADER_LEN], region->size);
			k_mutex_unlock(&nvm_lock);
			return 0;
		}
		if (slot == -ENOENT) {
			rc = -ENOENT;
		} else {
			/* A device error, or both slots corrupt: FRAM has failed (DS-75). */
			fail();
		}
	}
	memcpy(payload, region->defaults, region->size);
	stats.defaults_used++;
	k_mutex_unlock(&nvm_lock);
	return rc;
}

int nvm_write(const struct nvm_region *region, const uint8_t *payload)
{
	uint8_t bufs[2][NVM_SLOT_SIZE(NVM_PAYLOAD_MAX)];
	uint8_t check[NVM_SLOT_SIZE(NVM_PAYLOAD_MAX)];
	size_t len = NVM_SLOT_SIZE(region->size);
	size_t crc_at = len - NVM_CRC_LEN;
	uint32_t generation = 0;
	uint8_t *out;
	int target;
	int newest;

	k_mutex_lock(&nvm_lock, K_FOREVER);
	stats.writes++;
	if (!available) {
		k_mutex_unlock(&nvm_lock);
		return -EIO;
	}
	newest = newest_slot(region, bufs, &generation);
	if (newest == -EIO) {
		fail();
		k_mutex_unlock(&nvm_lock);
		return -EIO;
	}
	/* Write over the slot not holding the newest record; slot 0 if neither does. */
	target = newest >= 0 ? 1 - newest : 0;
	generation = newest >= 0 ? generation + 1 : 1;

	out = bufs[target];
	sys_put_le16(NVM_MAGIC, &out[0]);
	out[2] = region->version;
	out[3] = region->size;
	sys_put_le32(generation, &out[4]);
	memcpy(&out[NVM_HEADER_LEN], payload, region->size);
	sys_put_le32(crc32_c(0, out, crc_at, true, true), &out[crc_at]);

	/* Committed when the read-back matches: the new slot's CRC is valid. */
	if (eeprom_write(fram, slot_address(region, target), out, len) != 0 ||
	    eeprom_read(fram, slot_address(region, target), check, len) != 0 ||
	    memcmp(out, check, len) != 0) {
		fail();
		k_mutex_unlock(&nvm_lock);
		return -EIO;
	}
	k_mutex_unlock(&nvm_lock);
	return 0;
}

bool nvm_available(void)
{
	bool result;

	k_mutex_lock(&nvm_lock, K_FOREVER);
	result = available;
	k_mutex_unlock(&nvm_lock);
	return result;
}

int nvm_retry(void)
{
	int rc;

	k_mutex_lock(&nvm_lock, K_FOREVER);
	available = fram != NULL && device_is_ready(fram);
	rc = available ? 0 : -EIO;
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
