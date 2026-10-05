/*
 * Command delivery with a pending bound. See cmd_deliver.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>

#include "msg/common.h"
#include "silversat/cmd_deliver.h"
#include "silversat/resource_map.h"

int cmd_deliver(struct cmd_pending *pending, const struct zbus_channel *cmd_chan,
		const struct zbus_channel *status_chan, const void *cmd)
{
	struct app_status status;
	uint32_t handled;
	uint32_t progress;

	/* If the status can't be read without waiting, don't guess. */
	if (zbus_chan_read(status_chan, &status, K_NO_WAIT) != 0) {
		return -EBUSY;
	}
	handled = status.cmd_accepted + status.cmd_rejected;
	progress = handled - pending->handled_seen; /* unsigned: a wrap is harmless */
	pending->handled_seen = handled;
	pending->outstanding =
		progress >= pending->outstanding ? 0 : pending->outstanding - progress;

	if (pending->outstanding >= CMD_MAX_PENDING) {
		return -EBUSY;
	}
	if (zbus_chan_pub(cmd_chan, cmd, K_NO_WAIT) != 0) {
		return -EIO;
	}
	pending->outstanding++;
	return 0;
}
