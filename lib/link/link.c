/*
 * Link frame queues (DS-11). See include/silversat/link.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>

#include "silversat/link.h"
#include "silversat/resource_map.h"

K_MSGQ_DEFINE(uplink_msgq, sizeof(struct link_frame), UPLINK_QUEUE_DEPTH, 1);
K_MSGQ_DEFINE(downlink_msgq, sizeof(struct link_frame), DOWNLINK_QUEUE_DEPTH, 1);
