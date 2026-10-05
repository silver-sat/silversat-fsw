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

/* One record's place in FRAM. Generated; see nvm/<owner>.h. */
struct nvm_region {
	const char *name;
	uint16_t address;        /* slot 0; slot 1 follows it */
	uint8_t size;            /* payload bytes */
	uint8_t version;
	const uint8_t *defaults; /* the encoded default payload, size bytes */
};

/*
 * Read a record into payload (region->size bytes). Returns:
 *   0        the newest valid record from FRAM
 *   -ENOENT  no valid record (FRAM blank, or another version); the default
 *   -EIO     FRAM unavailable or failed; the default
 * payload always holds a record.
 */
int nvm_read(const struct nvm_region *region, uint8_t *payload);

/*
 * Store a record (region->size bytes), replacing the older slot. Returns 0
 * once it is stored and read back, or -EIO if FRAM is unavailable or
 * failed (and is now degraded).
 */
int nvm_write(const struct nvm_region *region, const uint8_t *payload);

/* False if there is no FRAM, or it has failed. */
bool nvm_available(void);

/*
 * After a failure, try the device again (DS-75: the ground can command a
 * retry). Returns 0 if FRAM is available again, -EIO if not.
 */
int nvm_retry(void);

/* Counts for housekeeping. */
struct nvm_stats {
	uint32_t reads;
	uint32_t writes;
	uint32_t defaults_used; /* reads that returned a default */
	uint32_t bad_slots;     /* slots that held a record but failed their CRC */
	uint32_t errors;        /* device errors and failed read-backs */
};

struct nvm_stats nvm_stats(void);

#endif /* SILVERSAT_NVM_H_ */
