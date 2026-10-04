/*
 * Tests for the generated housekeeping encoders (DS-61, DS-64).
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The vectors come from the ground decoder, tools/telemetry.py, at build
 * time: the flight encoder must produce exactly the bytes the ground
 * expects.
 */

#include <errno.h>

#include <zephyr/zbus/zbus.h>
#include <zephyr/ztest.h>

#include "msg/common.h"
#include "msg/typed_app.h"
#include "silversat/tlm_encode.h"
#include "tlm_vectors.h"

ZTEST(tlm_encode, test_encoding_matches_the_ground)
{
	for (size_t i = 0; i < ARRAY_SIZE(hk_vectors); i++) {
		const struct hk_vector *v = &hk_vectors[i];
		uint8_t out[255];
		int len;

		zassert_ok(zbus_chan_pub(&typed_app_hk_chan, v->hk, K_NO_WAIT));
		len = tlm_encode_hk(APP_ID_TYPED_APP, v->met_ms, out, sizeof(out));
		zassert_equal(len, (int)v->len, "%s: length %d", v->name, len);
		zassert_mem_equal(out, v->packet, v->len, "%s: bytes", v->name);
	}
}

ZTEST(tlm_encode, test_every_app_is_listed_once)
{
	for (size_t i = 0; i < tlm_hk_app_count; i++) {
		for (size_t j = i + 1; j < tlm_hk_app_count; j++) {
			zassert_not_equal(tlm_hk_apps[i], tlm_hk_apps[j]);
		}
	}
	zassert_equal(tlm_hk_app_count, APP_COUNT);
}

ZTEST(tlm_encode, test_every_app_encodes)
{
	uint8_t out[255];

	for (size_t i = 0; i < tlm_hk_app_count; i++) {
		int len = tlm_encode_hk(tlm_hk_apps[i], 7, out, sizeof(out));

		zassert_true(len >= TLM_HK_HEADER_LEN, "app %u: %d", tlm_hk_apps[i], len);
		zassert_equal(out[0], TLM_KIND_HK);
		zassert_equal(out[1], tlm_hk_apps[i]);
		zassert_equal(out[2], 7, "MET, least significant byte first");
	}
}

ZTEST(tlm_encode, test_unknown_app)
{
	uint8_t out[255];

	zassert_equal(tlm_encode_hk(0, 0, out, sizeof(out)), -ENOENT);
}

ZTEST(tlm_encode, test_needs_room)
{
	uint8_t out[255];
	size_t full = (size_t)tlm_encode_hk(APP_ID_TYPED_APP, 0, out, sizeof(out));

	zassert_equal(tlm_encode_hk(APP_ID_TYPED_APP, 0, out, full - 1), -ENOSPC);
	zassert_equal(tlm_encode_hk(APP_ID_TYPED_APP, 0, out, full), (int)full);
}

ZTEST_SUITE(tlm_encode, NULL, NULL, NULL, NULL, NULL);
