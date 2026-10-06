/*
 * The FRAM service: records that survive a reset (DS-70, DS-71, DS-75).
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * A synchronous library, called from the owning app's thread: a call
 * returns once the record is stored, so command ingest can store a floor
 * before it sends the ACK. Apps don't call these directly; they call the
 * typed functions the generator writes into their own nvm/<owner>.h from
 * messages/nvm_map.yaml, which encode the record and pass its region.
 *
 * Each record is stored in two slots, one after the other:
 *
 *   magic (2)  version (1)  length (1)  generation (4)  payload  CRC-32C (4)
 *
 * little-endian, the CRC over everything before it. A write goes to the
 * slot not holding the newest record, with the next generation, so a write
 * torn by a power failure only ever damages the slot being written; the
 * other still holds the previous value. A read takes the valid slot with
 * the newest generation.
 *
 * Whenever a record can't be read, because FRAM is blank, the record is of
 * another version, both slots are corrupt, or FRAM has failed, the caller
 * gets the record's default, never garbage (DS-75). A device error, a
 * write that doesn't read back, or both slots of a record corrupt marks
 * FRAM degraded: the service stops using it until nvm_retry().
 *
 * The mirror (DS-75's second tier): a record marked mirror: in
 * nvm_map.yaml is also kept, in the same two-slot form, in a small
 * retained memory that survives a reset (the devicetree alias nvm-mirror:
 * the STM32's backup SRAM). Both copies of a write carry the same
 * generation, and a read takes the newest valid record from either, so a
 * mirrored record survives FRAM failing, or not being fitted at all. The
 * mirror degrades the same way FRAM does, separately.
 *
 * A ring (ring: in nvm_map.yaml, DS-71's boot log) is a different kind of
 * region: entries in the same slot form, each written once, at its number
 * modulo the ring's length, over the entry that many before it. The
 * entry's number is its generation. A ring is kept in FRAM only.
 */

#ifndef SILVERSAT_NVM_H_
#define SILVERSAT_NVM_H_

#include <stdbool.h>
#include <stdint.h>

/* The slot layout above. */
#define NVM_MAGIC        0x5353 /* "SS" */
#define NVM_HEADER_LEN   8
#define NVM_CRC_LEN      4
#define NVM_PAYLOAD_MAX  128
#define NVM_SLOT_SIZE(payload_size) (NVM_HEADER_LEN + (payload_size) + NVM_CRC_LEN)

/* One record's place in FRAM, and in the mirror. Generated; see nvm/<owner>.h. */
struct nvm_region {
	const char *name;
	uint16_t address;        /* in FRAM: slot 0; slot 1 follows it */
	uint8_t size;            /* payload bytes */
	uint8_t version;
	const uint8_t *defaults; /* the encoded default payload, size bytes */
	bool mirrored;           /* also kept in the mirror */
	uint16_t mirror_address; /* in the mirror, if mirrored */
	uint8_t ring;            /* entries in a ring, or 0 for a two-slot record */
};

/*
 * Read a record into payload (region->size bytes). Returns:
 *   0        the newest valid record, from FRAM or the mirror
 *   -ENOENT  no valid record (blank, or another version); the default
 *   -EIO     neither FRAM nor (for a mirrored record) the mirror is
 *            available; the default
 * payload always holds a record.
 */
int nvm_read(const struct nvm_region *region, uint8_t *payload);

/*
 * Store a record (region->size bytes), replacing the older slot, in FRAM
 * and, if the record is mirrored, in the mirror. Returns 0 once it is
 * stored and read back in at least one of them, or -EIO if in neither.
 */
int nvm_write(const struct nvm_region *region, const uint8_t *payload);

/*
 * Write ring entry number at its place, number % region->ring. Returns 0
 * once it is stored and read back, or -EIO if FRAM is unavailable or failed.
 */
int nvm_ring_write(const struct nvm_region *region, uint32_t number, const uint8_t *payload);

/*
 * Read the ring entry at place index (0 to region->ring - 1) into payload,
 * and its number into *number. Returns 0, -ENOENT if the place holds no
 * valid entry of this version, -EINVAL for a place outside the ring, or
 * -EIO if FRAM is unavailable or failed.
 */
int nvm_ring_read(const struct nvm_region *region, uint8_t index, uint8_t *payload,
		  uint32_t *number);

/* False if there is no FRAM, or it has failed. */
bool nvm_available(void);

/* False if there is no mirror, or it has failed. */
bool nvm_mirror_available(void);

/*
 * After a failure, try FRAM and the mirror again (DS-75: the ground can
 * command a retry). Returns 0 if FRAM is available, -EIO if not.
 */
int nvm_retry(void);

/* Counts for housekeeping. */
struct nvm_stats {
	uint32_t reads;
	uint32_t writes;
	uint32_t defaults_used; /* reads that returned a default */
	uint32_t from_mirror;   /* reads answered by the mirror, not FRAM */
	uint32_t bad_slots;     /* slots that held a record but failed their CRC */
	uint32_t errors;        /* device errors and failed read-backs */
};

struct nvm_stats nvm_stats(void);

#endif /* SILVERSAT_NVM_H_ */
