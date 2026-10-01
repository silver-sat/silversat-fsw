/*
 * Entry indexes of the test frame tables (src/test_tables.c).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef FRAME_MANAGER_TEST_TABLES_H_
#define FRAME_MANAGER_TEST_TABLES_H_

/* MODE_SAFE: counter_app in slot 2; the protected mode manager in slot 9. */
enum {
	SAFE_COUNTER_SLOT2,
	SAFE_MODE_MANAGER_SLOT9,
	SAFE_ENTRIES,
};

/* MODE_NOMINAL: counter_app in slots 0 and 5; stuck_app in slot 1. */
enum {
	NOMINAL_COUNTER_SLOT0,
	NOMINAL_STUCK_SLOT1,
	NOMINAL_COUNTER_SLOT5,
	NOMINAL_ENTRIES,
};

#endif /* FRAME_MANAGER_TEST_TABLES_H_ */
