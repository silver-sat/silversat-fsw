/*
 * Attribute rows for this test's own apps, added to app_attrs[] by the
 * resource map (SS_TEST_APP_ATTRS, set in CMakeLists.txt).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Watched, not protected: health stops it at its threshold (DS-43). */
[APP_ID_WATCHED_APP] = {
	.protected = false,
	.stall_threshold = 3,
	.reenable = REENABLE_GROUND,
	.auto_retry_cap = 0,
	.critical = false,
},

/* Watched, not protected, and critical: stopping it also asks for safe mode (DS-41). */
[APP_ID_CRITICAL_APP] = {
	.protected = false,
	.stall_threshold = 3,
	.reenable = REENABLE_GROUND,
	.auto_retry_cap = 0,
	.critical = true,
},

/* Watched, restarted by health after its cooldown, at most twice (DS-43). */
[APP_ID_AUTO_APP] = {
	.protected = false,
	.stall_threshold = 3,
	.reenable = REENABLE_AUTO,
	.auto_retry_cap = 2,
	.critical = false,
},
