/*
 * The nvm app (DS-70, DS-74, DS-75).
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The FRAM service (lib/nvm) is a library: each app reads and writes its
 * own records through it, from its own thread. This app looks after the
 * service as a whole. Woken once a major frame, it:
 *
 *   - scrubs one region: checks every slot, in FRAM and the mirror, and
 *     counts any that hold a record failing its CRC. It never writes:
 *     each record belongs to its owner (DS-74), and the owner's next write
 *     goes over the bad slot. With N regions, every record is checked
 *     every N seconds.
 *   - publishes the service's housekeeping: whether FRAM and the mirror
 *     are available, and its counts.
 *
 * Its commands, from the ground:
 *
 *   retry   try FRAM and the mirror again after a failure (DS-75).
 *   dump    send bytes of FRAM or the mirror to the ground as they are, in
 *           one D packet: 'D', the store, the address (2 bytes,
 *           little-endian), the length, then the bytes (DS-66). Read-only.
 *           tools/nvm_dump.py puts the chunks together and decodes every
 *           record by name.
 *
 * It isn't on the ground command path, so it isn't protected: if it
 * stalls, health stops it (DS-43), and the apps' own records carry on.
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/zbus/zbus.h>

#include "msg/common.h"
#include "msg/nvm.h"
#include "nvm/map.h"
#include "silversat/link.h"
#include "silversat/nvm.h"
#include "silversat/resource_map.h"

ZBUS_MSG_SUBSCRIBER_DEFINE(nvm_sub);
ZBUS_CHAN_ADD_OBS(nvm_wakeup_chan, nvm_sub, 3);
ZBUS_CHAN_ADD_OBS(nvm_cmd_chan, nvm_sub, 3);

/* A dump packet (DS-66): kind, store, address, length, then the bytes. */
#define DUMP_KIND       'D'
#define DUMP_HEADER_LEN 5
#define DUMP_MAX        240

BUILD_ASSERT(DUMP_HEADER_LEN + DUMP_MAX <= LINK_FRAME_MAX, "a dump fits one link frame");

static struct app_status status;
static struct nvm_hk hk;

/* The region the scrub checks next. */
static uint8_t next_region;

static void scrub(void)
{
#if NVM_REGION_COUNT > 0
	int bad = nvm_check(&nvm_map[next_region]);

	if (bad > 0) {
		hk.scrub_bad += (uint32_t)bad;
	}
	next_region++;
	if (next_region == NVM_REGION_COUNT) {
		next_region = 0;
		hk.scrub_passes++;
	}
#endif
}

static int dump(const struct nvm_dump *args)
{
	struct link_frame frame;
	uint8_t *out = (uint8_t *)frame.data;

	if (args->length == 0 || args->length > DUMP_MAX) {
		hk.dump_refused++;
		return -EINVAL;
	}
	if (nvm_raw_read(args->store, args->address, &out[DUMP_HEADER_LEN], args->length) != 0) {
		hk.dump_refused++; /* outside the store, or the store unavailable */
		return -EIO;
	}
	out[0] = DUMP_KIND;
	out[1] = args->store;
	sys_put_le16(args->address, &out[2]);
	out[4] = args->length;
	frame.len = DUMP_HEADER_LEN + args->length;
	if (k_msgq_put(&downlink_msgq, &frame, K_NO_WAIT) != 0) {
		hk.dump_refused++; /* downlink full: ask again */
		return -EBUSY;
	}
	hk.dumps++;
	return 0;
}

static int handle_command(const struct nvm_cmd *cmd)
{
	switch (cmd->id) {
	case NVM_CMD_RETRY:
		(void)nvm_retry(); /* the housekeeping flags show the result */
		return 0;
	case NVM_CMD_DUMP:
		return dump(&cmd->args.dump);
	default:
		return -ENOTSUP;
	}
}

static void publish_hk(void)
{
	struct nvm_stats stats = nvm_stats();

	hk.fram_available = nvm_available();
	hk.mirror_available = nvm_mirror_available();
	hk.reads = stats.reads;
	hk.writes = stats.writes;
	hk.defaults_used = stats.defaults_used;
	hk.from_mirror = stats.from_mirror;
	hk.bad_slots = stats.bad_slots;
	hk.errors = stats.errors;
	zbus_chan_pub(&nvm_hk_chan, &hk, K_NO_WAIT);
}

static void nvm_main(void *a, void *b, void *c)
{
	const struct zbus_channel *chan;
	union nvm_msg msg;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	publish_hk();
	while (zbus_sub_wait_msg(&nvm_sub, &chan, &msg, K_FOREVER) == 0) {
		if (chan == &nvm_wakeup_chan) {
			scrub();
			status.steps++;
		} else if (chan == &nvm_cmd_chan) {
			if (handle_command(&msg.cmd) == 0) {
				status.cmd_accepted++;
			} else {
				status.cmd_rejected++;
			}
		}
		publish_hk();
		zbus_chan_pub(&nvm_status_chan, &status, K_NO_WAIT);
	}
}

K_THREAD_DEFINE(nvm_tid, NVM_STACK_SIZE, nvm_main, NULL, NULL, NULL, NVM_PRIORITY, 0, 0);
