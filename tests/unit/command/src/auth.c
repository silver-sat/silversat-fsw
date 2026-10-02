/*
 * Tests for command authentication: shape check and signature (DS-50,
 * DS-52).
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The signed packets come from tools/sign_command.py at build time, so
 * every test checks the C code against the ground's signer.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/ztest.h>

#include "signed_vectors.h"
#include "silversat/cmd_auth.h"

/* A writable copy of a packet, for tampering with. */
static char buf[CMD_AUTH_PACKET_MAX + 2];

static size_t copy_packet(const char *packet)
{
	size_t len = strlen(packet);

	zassert_true(len < sizeof(buf));
	memcpy(buf, packet, len);
	return len;
}

static bool parse_and_verify(const char *packet, size_t len)
{
	struct cmd_auth_packet parsed;

	return cmd_auth_parse(packet, len, &parsed) == 0 &&
	       cmd_auth_verify(packet, len, &parsed, vector_key);
}

/* Replace one character with a different character of the same kind. */
static char other(char c)
{
	if (c >= '0' && c <= '9') {
		return c == '9' ? '0' : c + 1;
	}
	if (c >= 'a' && c <= 'f') {
		return c == 'f' ? 'a' : c + 1;
	}
	return c == '~' ? ' ' : c + 1;
}

ZTEST(cmd_auth, test_ground_signed_packets_verify)
{
	for (size_t i = 0; i < ARRAY_SIZE(signed_vectors); i++) {
		const struct signed_vector *v = &signed_vectors[i];
		size_t len = strlen(v->packet);
		struct cmd_auth_packet parsed;

		zassert_ok(cmd_auth_parse(v->packet, len, &parsed), "%s", v->name);
		zassert_equal(parsed.counter, v->counter, "%s", v->name);
		zassert_mem_equal(parsed.salt, vector_salt, CMD_AUTH_SALT_LEN, "%s", v->name);
		zassert_equal(parsed.text_len, strlen(v->text), "%s", v->name);
		zassert_mem_equal(parsed.text, v->text, parsed.text_len, "%s", v->name);
		zassert_true(cmd_auth_verify(v->packet, len, &parsed, vector_key), "%s", v->name);
	}
}

ZTEST(cmd_auth, test_counter_max_parses)
{
	for (size_t i = 0; i < ARRAY_SIZE(signed_vectors); i++) {
		if (strcmp(signed_vectors[i].name, "counter_max") == 0) {
			struct cmd_auth_packet parsed;

			zassert_ok(cmd_auth_parse(signed_vectors[i].packet,
						  strlen(signed_vectors[i].packet), &parsed));
			zassert_equal(parsed.counter, UINT64_MAX);
			return;
		}
	}
	ztest_test_fail();
}

ZTEST(cmd_auth, test_wrong_key_fails)
{
	uint8_t key[CMD_AUTH_KEY_LEN];

	memcpy(key, vector_key, sizeof(key));
	key[0] ^= 1;
	for (size_t i = 0; i < ARRAY_SIZE(signed_vectors); i++) {
		const char *packet = signed_vectors[i].packet;
		struct cmd_auth_packet parsed;

		zassert_ok(cmd_auth_parse(packet, strlen(packet), &parsed));
		zassert_false(cmd_auth_verify(packet, strlen(packet), &parsed, key));
	}
}

ZTEST(cmd_auth, test_any_changed_character_fails)
{
	/*
	 * Change each character in turn: the tag, the salt, the counter, and
	 * the text. Each change keeps the shape valid, so only the signature
	 * check can catch it.
	 */
	const char *packet = signed_vectors[1].packet;
	size_t len = strlen(packet);

	for (size_t i = 0; i < len; i++) {
		copy_packet(packet);
		buf[i] = other(buf[i]);
		zassert_false(parse_and_verify(buf, len), "change at %zu accepted", i);
	}
	copy_packet(packet);
	zassert_true(parse_and_verify(buf, len), "the unchanged packet still verifies");
}

ZTEST(cmd_auth, test_truncated_or_extended_fails)
{
	const char *packet = signed_vectors[0].packet;
	size_t len = copy_packet(packet);

	zassert_false(parse_and_verify(buf, len - 1), "truncated");
	buf[len] = 'x';
	zassert_false(parse_and_verify(buf, len + 1), "extended");
}

ZTEST(cmd_auth, test_shape_check_rejects)
{
	struct cmd_auth_packet parsed;
	const char *packet = signed_vectors[0].packet; /* "noop" */
	size_t len;

	/* No command text: the header alone. */
	len = copy_packet(packet);
	zassert_equal(cmd_auth_parse(buf, CMD_AUTH_HEADER_CHARS, &parsed), -EINVAL);

	/* Longer than a packet may be (DS-66). */
	memset(buf, 'a', sizeof(buf));
	zassert_equal(cmd_auth_parse(buf, CMD_AUTH_PACKET_MAX + 1, &parsed), -EINVAL);

	/* Uppercase hex in each field (DS-52). */
	for (size_t i = 0; i < CMD_AUTH_HEADER_CHARS; i += CMD_AUTH_SALT_CHARS) {
		len = copy_packet(packet);
		buf[i] = 'A';
		zassert_equal(cmd_auth_parse(buf, len, &parsed), -EINVAL, "uppercase at %zu", i);
	}

	/* A character that isn't hex. */
	len = copy_packet(packet);
	buf[CMD_AUTH_TAG_CHARS + CMD_AUTH_SALT_CHARS] = 'g';
	zassert_equal(cmd_auth_parse(buf, len, &parsed), -EINVAL);

	/* Text that isn't printable ASCII. */
	len = copy_packet(packet);
	buf[len - 1] = '\n';
	zassert_equal(cmd_auth_parse(buf, len, &parsed), -EINVAL, "newline");
	buf[len - 1] = 0x7f;
	zassert_equal(cmd_auth_parse(buf, len, &parsed), -EINVAL, "DEL");
	buf[len - 1] = (char)0x80;
	zassert_equal(cmd_auth_parse(buf, len, &parsed), -EINVAL, "high bit");
}

ZTEST_SUITE(cmd_auth, NULL, NULL, NULL, NULL, NULL);
