/*
 * Tests for the link codec: KISS framing, CRC-32C, and sequence numbers
 * (DS-33, DS-65, DS-66).
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The vectors come from the Python twin, tools/link_codec.py, at build
 * time, so the C codec must agree byte for byte with an independent
 * implementation (DS-92).
 */

#include <errno.h>
#include <string.h>

#include <zephyr/sys/crc.h>
#include <zephyr/ztest.h>

#include "link_vectors.h"
#include "silversat/link_codec.h"

static struct link_packet packet_from(const struct packet_vector *v)
{
	struct link_packet p = {.type = v->type, .seq = v->seq, .len = v->len};

	memcpy(p.payload, v->payload, v->len);
	return p;
}

static void assert_packet_matches(const struct link_packet *got, const struct packet_vector *v)
{
	zassert_equal(got->type, v->type, "%s: type", v->name);
	zassert_equal(got->seq, v->seq, "%s: seq", v->name);
	zassert_equal(got->len, v->len, "%s: len", v->name);
	zassert_mem_equal(got->payload, v->payload, v->len, "%s: payload", v->name);
}

ZTEST(link_codec, test_crc32c_check_value)
{
	/* DS-65: the check value for ASCII "123456789". */
	zassert_equal(crc32_c(0, (const uint8_t *)"123456789", 9, true, true), 0xE3069283);
}

ZTEST(link_codec, test_encoding_matches_python)
{
	for (size_t i = 0; i < ARRAY_SIZE(packet_vectors); i++) {
		const struct packet_vector *v = &packet_vectors[i];
		struct link_packet p = packet_from(v);
		uint8_t out[LINK_ENCODED_MAX];
		int len = link_encode(&p, out, sizeof(out));

		zassert_equal(len, (int)v->encoded_len, "%s: length", v->name);
		zassert_mem_equal(out, v->encoded, v->encoded_len, "%s: bytes", v->name);
	}
}

ZTEST(link_codec, test_decoding_matches_python)
{
	for (size_t i = 0; i < ARRAY_SIZE(stream_vectors); i++) {
		const struct stream_vector *v = &stream_vectors[i];
		struct link_decoder decoder;
		struct link_packet got;
		size_t n = 0;

		link_decoder_init(&decoder);
		for (size_t b = 0; b < v->len; b++) {
			enum link_decode_result r = link_decode_byte(&decoder, v->bytes[b], &got);

			if (r == LINK_DECODE_NONE) {
				continue;
			}
			zassert_true(n < v->expect_count, "%s: more results than expected", v->name);
			zassert_equal(r, v->expect[n].result, "%s: result %zu is %d, expected %d",
				      v->name, n, r, v->expect[n].result);
			if (r == LINK_DECODE_PACKET) {
				assert_packet_matches(&got, &packet_vectors[v->expect[n].packet]);
			}
			n++;
		}
		zassert_equal(n, v->expect_count, "%s: %zu results, expected %zu", v->name, n,
			      v->expect_count);
	}
}

ZTEST(link_codec, test_encode_needs_room)
{
	const struct packet_vector *v = &packet_vectors[0];
	struct link_packet p = packet_from(v);
	uint8_t out[LINK_ENCODED_MAX];

	for (size_t size = 0; size < v->encoded_len; size++) {
		zassert_equal(link_encode(&p, out, size), -ENOSPC, "room for %zu bytes", size);
	}
	zassert_equal(link_encode(&p, out, v->encoded_len), (int)v->encoded_len);
}

ZTEST(link_codec, test_worst_case_fits)
{
	/* Every byte escaped: LINK_ENCODED_MAX must still be enough. */
	struct link_packet p = {.type = KISS_FEND, .seq = KISS_FESC, .len = LINK_PAYLOAD_MAX};
	uint8_t out[LINK_ENCODED_MAX];

	memset(p.payload, KISS_FEND, sizeof(p.payload));
	zassert_true(link_encode(&p, out, sizeof(out)) > 0);
}

ZTEST(link_codec, test_sequence_numbers)
{
	struct link_seq tx;
	struct link_seq rx;

	link_seq_init(&tx);
	zassert_equal(link_seq_next(&tx), 0);
	zassert_equal(link_seq_next(&tx), 1);
	tx.next_tx = 255;
	zassert_equal(link_seq_next(&tx), 255);
	zassert_equal(link_seq_next(&tx), 0, "wraps after 255");

	link_seq_init(&rx);
	zassert_equal(link_seq_received(&rx, 10), 0, "the first frame sets the starting point");
	zassert_equal(link_seq_received(&rx, 11), 0);
	zassert_equal(link_seq_received(&rx, 14), 2, "12 and 13 were lost");
	zassert_equal(link_seq_received(&rx, 14), -1, "a repeat");
	zassert_equal(link_seq_received(&rx, 255), 240);
	zassert_equal(link_seq_received(&rx, 0), 0, "255 then 0 loses nothing");
	zassert_equal(link_seq_received(&rx, 254), 253);
	zassert_equal(link_seq_received(&rx, 1), 2, "255 and 0 were lost across the wrap");
}

ZTEST_SUITE(link_codec, NULL, NULL, NULL, NULL, NULL);
