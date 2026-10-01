/*
 * Compiles include/silversat/resource_map.h on its own, so its build checks
 * (DS-07: frame timing, zbus pool size) run in every build, whether or not
 * any app is enabled yet.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "silversat/resource_map.h"
