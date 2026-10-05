/*
 * Command delivery with a pending bound (DS-07, DS-50).
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Every command waiting in an app's message subscriber holds a buffer from
 * the zbus pool, which all channels share. So each sender keeps at most
 * CMD_MAX_PENDING commands to each app that the app hasn't handled yet,
 * and the resource map budgets the pool for that (POOL_REQUIRED).
 *
 * Two kinds of sender use this: the generated ground command routing
 * (one struct cmd_pending per app) and the generated internal command
 * senders, send_<app>_<command>() (one per sender and target app). Each
 * struct cmd_pending belongs to one thread, so no lock is needed.
 */

#ifndef SILVERSAT_CMD_DELIVER_H_
#define SILVERSAT_CMD_DELIVER_H_

#include <stdint.h>

#include <zephyr/zbus/zbus.h>

/* Commands one sender has published to one app, and what it has seen handled. */
struct cmd_pending {
	uint32_t outstanding;
	uint32_t handled_seen;
};

/*
 * Publish cmd on the target's command channel without waiting, unless the
 * sender already has CMD_MAX_PENDING commands the target hasn't handled.
 *
 * The target's status counts every command it handles, from any sender,
 * and all of that progress is credited to this sender. That can only
 * undercount what is waiting, and once the target stops handling
 * commands, the count is exact.
 *
 * Returns 0 if published; -EBUSY if the sender must wait (too many
 * pending, or the status couldn't be read without waiting); -EIO if the
 * publish failed (the channel's validator, or no pool buffer).
 */
int cmd_deliver(struct cmd_pending *pending, const struct zbus_channel *cmd_chan,
		const struct zbus_channel *status_chan, const void *cmd);

#endif /* SILVERSAT_CMD_DELIVER_H_ */
